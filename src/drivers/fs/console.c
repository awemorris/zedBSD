/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/console-device.h"
#include "kern/cdev.h"
#include "kern/clock.h"
#include "kern/file.h"
#include "kern/input-device.h"
#include "kern/input-keymap.h"
#include "kern/kmem.h"
#include "kern/lock.h"
#include "kern/poll.h"
#ifndef ZEDBSD_INPUT_OWNERSHIP_TEST
#include "kern/sched.h"
#include "kern/thread.h"
#else
#define SCHED_PRIORITY_DEFAULT 8
int kthread_create(void (*)(void *), void *, int, struct thread **);
int thread_abort_new(struct thread *);
void thread_start(struct thread *);
#endif
#include "kern/tty.h"
#include "kern/uaccess.h"
#include "kern/waitq.h"

#include <zedbsd/console.h>
#include <errno.h>
#include <fcntl.h>
#include <hal/hal.h>
#include <string.h>

#define CONSOLE_WRITE_MAX 512U
#define CONSOLE_INPUT_EVENTS 64U
#define CONSOLE_DISPATCH_EVENTS 64U
#define CONSOLE_INPUT_SOURCES 8U
#define CONSOLE_KEY_CAPABILITIES 128U
#define CONSOLE_LOGICAL_KEYS 128U

#ifndef ZEDBSD_INPUT_OWNERSHIP_TEST
static uint32_t input_events[CONSOLE_INPUT_EVENTS];
#endif
static unsigned input_head, input_tail, input_used;
static unsigned input_started;
static struct spinlock input_lock;
static struct wait_queue input_waitq;
static struct wait_queue dispatch_waitq;
#ifndef ZEDBSD_INPUT_OWNERSHIP_TEST
static struct input_device *keyboard_input;
static struct input_keymap_state early_keymap;
static int early_resyncing;
static struct input_subscription console_subscription;
#endif

struct console_dispatch_event {
	uint32_t translated;
	unsigned device_id;
	unsigned overflow;
	unsigned repeat;
};

struct console_logical_key {
	char symbol[HAL_KEY_SYMBOL_SIZE];
	uint16_t key;
};

struct console_source_state {
	struct input_device *source;
	struct input_keymap_state keymap;
	int resyncing;
	uint16_t active[KEY_MAX + 1U];
	struct console_logical_key logical[CONSOLE_LOGICAL_KEYS];
};

static struct console_dispatch_event
    dispatch_events[CONSOLE_DISPATCH_EVENTS];
static unsigned dispatch_head, dispatch_tail, dispatch_used;
static struct console_source_state console_sources[CONSOLE_INPUT_SOURCES];

#define CONSOLE_EVENT_RECORDS 64U

#ifndef ZEDBSD_INPUT_OWNERSHIP_TEST
struct console_open {
	unsigned vt;
	unsigned input_mode;
};

static struct console_open *event_owner;
static struct console_input_event event_records[CONSOLE_EVENT_RECORDS];
static unsigned event_sequence;
#endif
static unsigned event_head, event_tail, event_used;

static void
console_drain_input_locked(void)
{
	input_head = input_tail = input_used = 0;
	event_head = event_tail = event_used = 0;
	dispatch_head = dispatch_tail = dispatch_used = 0;
	/* The broker owns HAL transitions once started; never drop a break. */
	if (!input_started)
		hal_cons_drain_input();
}

#ifndef ZEDBSD_INPUT_OWNERSHIP_TEST
static struct console_open *
console_open_state(struct file *file)
{
	return file != NULL ? file->f_data : NULL;
}

static unsigned
console_file_vt(struct file *file)
{
	struct console_open *state = console_open_state(file);
	return state != NULL ? state->vt : 0U;
}

static int
console_open_file(struct file *file)
{
	struct console_open *state = kern_malloc(sizeof(*state));
	if (state == NULL)
		return ENOMEM;
	state->vt = 0;
	state->input_mode = ZEDBSD_CONSOLE_INPUT_TEXT;
	file->f_data = state;
	return 0;
}

static int
console_close_file(struct file *file)
{
	struct console_open *state = console_open_state(file);
	unsigned long irq;
	if (state == NULL)
		return 0;
	irq = spin_lock_irqsave(&input_lock);
	if (event_owner == state) {
		event_owner = NULL;
		event_head = event_tail = event_used = 0;
		waitq_wake_all(&input_waitq);
	}
	spin_unlock_irqrestore(&input_lock, irq);
	kern_free(state);
	file->f_data = NULL;
	poll_notify();
	return 0;
}

static int
console_capability_add(struct input_capability *capabilities, size_t *count,
	uint16_t code)
{
	size_t index;

	if (code == KEY_RESERVED)
		return 0;
	for (index = 0; index < *count; index++)
		if (capabilities[index].type == EV_KEY &&
		    capabilities[index].code == code)
			return 0;
	if (*count == CONSOLE_KEY_CAPABILITIES)
		return ENOSPC;
	capabilities[*count].type = EV_KEY;
	capabilities[*count].code = code;
	(*count)++;
	return 0;
}

static int
console_capabilities(const struct hal_cons_input_info *hal_info,
	struct input_capability *capabilities, size_t *count)
{
	size_t index;

	*count = 1;
	capabilities[0].type = EV_SYN;
	capabilities[0].code = SYN_REPORT;
	if ((hal_info->flags & HAL_CONS_INPUT_TEXT) != 0) {
		unsigned character;
		for (character = 1; character < 0x80U; character++) {
			char symbol[2] = {(char)character, '\0'};
			int error = console_capability_add(capabilities, count,
			    input_key_from_symbol(symbol));
			if (error != 0)
				return error;
		}
	}
	for (index = 0; index < hal_info->symbol_count; index++) {
		int error = console_capability_add(capabilities, count,
		    input_key_from_symbol(hal_info->symbols[index]));
		if (error != 0)
			return error;
	}
	return 0;
}

static int
console_input_take(int consume, int wait)
{
	unsigned long irq;
	int result;

	if (!input_started) {
		for (;;) {
			struct hal_key_event event;
			struct input_keymap_state state = early_keymap;
			uint32_t translated;
			int available = wait ? hal_cons_read_event(&event) :
			    hal_cons_poll_event(&event);

			if (!available)
				return -1;
			if (!wait && ((event.flags & (HAL_KEY_EVENT_RESYNC |
			    HAL_KEY_EVENT_SNAPSHOT | HAL_KEY_EVENT_RESYNC_END)) != 0 ||
			    early_resyncing))
				(void)hal_cons_read_event(&event);
			if ((event.flags & HAL_KEY_EVENT_RESYNC) != 0) {
				input_keymap_init(&early_keymap);
				early_keymap.caps_lock =
				    (event.flags & HAL_KEY_EVENT_LOCK_CAPS) != 0;
				early_keymap.kana_lock =
				    (event.flags & HAL_KEY_EVENT_LOCK_KANA) != 0;
				early_resyncing = 1;
				continue;
			}
			if (event.flags == (HAL_KEY_EVENT_PRESS |
			    HAL_KEY_EVENT_SNAPSHOT) && early_resyncing) {
				uint8_t caps = early_keymap.caps_lock;
				uint8_t kana = early_keymap.kana_lock;

				event.flags = HAL_KEY_EVENT_PRESS;
				(void)input_keymap_translate(&early_keymap, &event,
				    &translated);
				early_keymap.caps_lock = caps;
				early_keymap.kana_lock = kana;
				continue;
			}
			if (event.flags == HAL_KEY_EVENT_RESYNC_END) {
				early_resyncing = 0;
				continue;
			}
			if (early_resyncing)
				continue;
			if (!input_keymap_translate(&state, &event, &translated))
				return -1;
			if (consume)
				early_keymap = state;
			return (int)translated;
		}
	}
	irq = spin_lock_irqsave(&input_lock);
	while (input_used == 0) {
		uint64_t sequence;
		int error;
		if (!wait) {
			spin_unlock_irqrestore(&input_lock, irq);
			return -1;
		}
		sequence = waitq_sequence(&input_waitq);
		error = waitq_sleep(&input_waitq, &input_lock, sequence, 0,
				    WAITQ_INTERRUPTIBLE);
		if (error == EINTR) {
			spin_unlock_irqrestore(&input_lock, irq);
			return -EINTR;
		}
	}
	result = (int)input_events[input_tail];
	if (consume) {
		input_tail = (input_tail + 1U) % CONSOLE_INPUT_EVENTS;
		input_used--;
	}
	spin_unlock_irqrestore(&input_lock, irq);
	return result;
}

int
console_input_poll_event(void)
{
	return console_input_take(0, 0);
}
int
console_input_read_event(void)
{
	return console_input_take(1, 1);
}
#endif

static struct console_source_state *
console_source_find(struct input_device *source, int create)
{
	unsigned index;
	struct console_source_state *empty = NULL;

	for (index = 0; index < CONSOLE_INPUT_SOURCES; index++) {
		if (console_sources[index].source == source)
			return &console_sources[index];
		if (console_sources[index].source == NULL && empty == NULL)
			empty = &console_sources[index];
	}
	if (!create || empty == NULL)
		return NULL;
	empty->source = source;
	input_keymap_init(&empty->keymap);
	return empty;
}

static uint32_t
console_source_active_key(struct console_source_state *source,
	const struct input_report_event *item, uint32_t translated)
{
	uint16_t *active = NULL;
	unsigned index;

	if (item->event.code <= KEY_MAX && item->event.code != KEY_RESERVED) {
		active = &source->active[item->event.code];
	} else if (item->symbol[0] != '\0') {
		struct console_logical_key *empty = NULL;

		for (index = 0; index < CONSOLE_LOGICAL_KEYS; index++) {
			if (strcmp(source->logical[index].symbol,
			    item->symbol) == 0) {
				active = &source->logical[index].key;
				break;
			}
			if (source->logical[index].symbol[0] == '\0' &&
			    empty == NULL)
				empty = &source->logical[index];
		}
		if (active == NULL && item->event.value != 0 && empty != NULL) {
			memcpy(empty->symbol, item->symbol, sizeof(empty->symbol));
			active = &empty->key;
		}
	}
	if (active == NULL)
		return translated;
	if (item->event.value != 0)
		*active = (uint16_t)(translated & INPUT_KEY_MASK);
	else if (*active != 0)
		translated = (translated & ~INPUT_KEY_MASK) | *active;
	if (item->event.value == 0) {
		*active = 0;
		if (item->event.code == KEY_RESERVED)
			for (index = 0; index < CONSOLE_LOGICAL_KEYS; index++)
				if (&source->logical[index].key == active) {
					memset(&source->logical[index], 0,
					    sizeof(source->logical[index]));
					break;
				}
	}
	return translated;
}

static void
console_dispatch_enqueue(uint32_t translated, unsigned device_id,
	unsigned repeat)
{
	struct console_dispatch_event event;

	memset(&event, 0, sizeof(event));
	event.translated = translated;
	event.device_id = device_id;
	event.repeat = repeat;
	if (dispatch_used == CONSOLE_DISPATCH_EVENTS) {
		event.overflow = 1;
		if (dispatch_events[dispatch_tail].overflow != 0)
			event.overflow = dispatch_events[dispatch_tail].overflow;
		dispatch_tail = (dispatch_tail + 1U) % CONSOLE_DISPATCH_EVENTS;
		dispatch_used--;
	}
	dispatch_events[dispatch_head] = event;
	dispatch_head = (dispatch_head + 1U) % CONSOLE_DISPATCH_EVENTS;
	dispatch_used++;
}

/*
 * Translation is deliberately completed in this bounded callback.  The
 * dispatch ring may lose old output under overload, but it can never lose a
 * modifier transition from the per-source translation state.
 */
static void
console_input_subscriber(void *context, const struct input_report *report)
{
	struct console_source_state *source;
	unsigned long irq;
	size_t index;
	int queued = 0;

	(void)context;
	if (report == NULL)
		return;
	irq = spin_lock_irqsave(&input_lock);
	if ((report->flags & INPUT_REPORT_RESYNC_BEGIN) != 0) {
		source = console_source_find(report->device, 1);
		if (source != NULL) {
			memset(source, 0, sizeof(*source));
			source->source = report->device;
			input_keymap_init(&source->keymap);
			source->keymap.caps_lock =
			    (report->flags & INPUT_REPORT_LOCK_CAPS) != 0;
			source->keymap.kana_lock =
			    (report->flags & INPUT_REPORT_LOCK_KANA) != 0;
			source->resyncing = 1;
		}
		spin_unlock_irqrestore(&input_lock, irq);
		return;
	}
	source = console_source_find(report->device,
	    report->flags == 0);
	if ((report->flags & INPUT_REPORT_SNAPSHOT) != 0) {
		if (source != NULL && source->resyncing)
			for (index = 0; index < report->event_count; index++) {
				const struct input_report_event *item =
				    &report->events[index];
				struct hal_key_event key_event;
				uint32_t translated;
				uint8_t caps, kana;

				if (item->event.type != EV_KEY ||
				    item->event.value != 1 || item->symbol[0] == '\0')
					continue;
				memset(&key_event, 0, sizeof(key_event));
				memcpy(key_event.symbol, item->symbol,
				    sizeof(key_event.symbol));
				key_event.flags = HAL_KEY_EVENT_PRESS;
				caps = source->keymap.caps_lock;
				kana = source->keymap.kana_lock;
				if (input_keymap_translate(&source->keymap, &key_event,
				    &translated))
					(void)console_source_active_key(source, item,
					    translated);
				source->keymap.caps_lock = caps;
				source->keymap.kana_lock = kana;
			}
		spin_unlock_irqrestore(&input_lock, irq);
		return;
	}
	if ((report->flags & INPUT_REPORT_RESYNC_END) != 0) {
		if (source != NULL)
			source->resyncing = 0;
		spin_unlock_irqrestore(&input_lock, irq);
		return;
	}
	if (source != NULL && !source->resyncing)
		for (index = 0; index < report->event_count; index++) {
			const struct input_report_event *item =
			    &report->events[index];
			struct hal_key_event key_event;
			uint32_t translated;

			if (item->event.type != EV_KEY)
				continue;
			if (item->symbol[0] != '\0') {
				memset(&key_event, 0, sizeof(key_event));
				memcpy(key_event.symbol, item->symbol,
				    sizeof(key_event.symbol));
				key_event.flags = item->key_flags;
			} else if (!input_keymap_event_from_code(item->event.code,
			    item->event.value, &key_event)) {
				continue;
			}
			if (!input_keymap_translate(&source->keymap, &key_event,
			    &translated))
				continue;
			translated = console_source_active_key(source, item,
			    translated);
			console_dispatch_enqueue(translated, report->device_id,
			    item->event.value == 2);
			queued = 1;
		}
	if ((report->flags & INPUT_REPORT_DETACH) != 0 && source != NULL &&
	    !source->resyncing)
		for (index = 0; index < CONSOLE_LOGICAL_KEYS; index++) {
			uint32_t modifiers;

			if (source->logical[index].key == 0)
				continue;
			modifiers =
			    (source->keymap.left_shift ||
			    source->keymap.right_shift ? INPUT_KEY_SHIFT : 0U) |
			    (source->keymap.left_control ||
			    source->keymap.right_control ? INPUT_KEY_CTRL : 0U) |
			    (source->keymap.left_graph ||
			    source->keymap.right_graph ? INPUT_KEY_GRAPH : 0U);
			console_dispatch_enqueue(source->logical[index].key |
			    modifiers | INPUT_KEY_RELEASE, report->device_id, 0);
			queued = 1;
		}
	if ((report->flags & INPUT_REPORT_DETACH) != 0 && source != NULL)
		memset(source, 0, sizeof(*source));
	if (queued)
		waitq_wake_all(&dispatch_waitq);
	spin_unlock_irqrestore(&input_lock, irq);
}

#ifdef ZEDBSD_INPUT_OWNERSHIP_TEST
void
console_input_ownership_test_reset(void)
{
	spin_init(&input_lock, LOCK_RANK_DEVICE, "console input test");
	waitq_init(&input_waitq, "console input test");
	waitq_init(&dispatch_waitq, "console dispatch test");
	input_head = input_tail = input_used = 0;
	event_head = event_tail = event_used = 0;
	dispatch_head = dispatch_tail = dispatch_used = 0;
	input_started = 1;
	memset(console_sources, 0, sizeof(console_sources));
}

void
console_input_ownership_test_publish(const struct input_report *report)
{
	console_input_subscriber(NULL, report);
}

int
console_input_ownership_test_pop(uint32_t *translated, unsigned *device_id,
	unsigned *repeat)
{
	struct console_dispatch_event event;
	unsigned long irq = spin_lock_irqsave(&input_lock);

	if (dispatch_used == 0) {
		spin_unlock_irqrestore(&input_lock, irq);
		return 0;
	}
	event = dispatch_events[dispatch_tail];
	dispatch_tail = (dispatch_tail + 1U) % CONSOLE_DISPATCH_EVENTS;
	dispatch_used--;
	spin_unlock_irqrestore(&input_lock, irq);
	if (translated != NULL)
		*translated = event.translated;
	if (device_id != NULL)
		*device_id = event.device_id;
	if (repeat != NULL)
		*repeat = event.repeat;
	return 1;
}

int
console_input_ownership_test_state(struct input_device *device,
	unsigned code, unsigned *caps, unsigned *kana, unsigned *shift,
	uint16_t *active, int *resyncing)
{
	struct console_source_state *source;
	unsigned long irq = spin_lock_irqsave(&input_lock);

	source = console_source_find(device, 0);
	if (source == NULL || code > KEY_MAX) {
		spin_unlock_irqrestore(&input_lock, irq);
		return 0;
	}
	if (caps != NULL)
		*caps = source->keymap.caps_lock;
	if (kana != NULL)
		*kana = source->keymap.kana_lock;
	if (shift != NULL)
		*shift = source->keymap.left_shift ||
		    source->keymap.right_shift;
	if (active != NULL)
		*active = source->active[code];
	if (resyncing != NULL)
		*resyncing = source->resyncing;
	spin_unlock_irqrestore(&input_lock, irq);
	return 1;
}

void
console_input_ownership_test_drain(int started)
{
	unsigned long irq = spin_lock_irqsave(&input_lock);

	input_started = started != 0;
	console_drain_input_locked();
	spin_unlock_irqrestore(&input_lock, irq);
}
#else

static void
console_deliver(uint32_t translated, unsigned device_id, unsigned overflow,
	unsigned repeat)
{
	unsigned long irq = spin_lock_irqsave(&input_lock);

	if (event_owner != NULL) {
		struct console_input_event *record;
		unsigned flags = overflow != 0 ?
		    ZEDBSD_CONSOLE_INPUT_FLAG_OVERFLOW : 0;
		if (event_used == CONSOLE_EVENT_RECORDS) {
			event_tail = (event_tail + 1U) % CONSOLE_EVENT_RECORDS;
			event_used--;
			flags |= ZEDBSD_CONSOLE_INPUT_FLAG_OVERFLOW;
		}
		record = &event_records[event_head];
		memset(record, 0, sizeof(*record));
		record->timestamp_ns = clock_milliseconds(NULL) * 1000000ULL;
		record->sequence = ++event_sequence;
		record->type = ZEDBSD_CONSOLE_INPUT_EVENT_KEY;
		record->flags = (uint16_t)flags;
		record->device_id = device_id;
		record->key = translated & INPUT_KEY_MASK;
		record->modifiers = translated &
		    (INPUT_KEY_SHIFT | INPUT_KEY_CTRL | INPUT_KEY_GRAPH);
		record->state = (translated & INPUT_KEY_RELEASE) != 0 ?
		    ZEDBSD_CONSOLE_KEY_RELEASE :
		    repeat != 0 ?
		    ZEDBSD_CONSOLE_KEY_REPEAT : ZEDBSD_CONSOLE_KEY_PRESS;
		event_head = (event_head + 1U) % CONSOLE_EVENT_RECORDS;
		event_used++;
		waitq_wake_all(&input_waitq);
		spin_unlock_irqrestore(&input_lock, irq);
	} else if ((translated & INPUT_KEY_RELEASE) == 0) {
		if (input_used == CONSOLE_INPUT_EVENTS) {
			input_tail = (input_tail + 1U) % CONSOLE_INPUT_EVENTS;
			input_used--;
		}
		input_events[input_head] = translated;
		input_head = (input_head + 1U) % CONSOLE_INPUT_EVENTS;
		input_used++;
		waitq_wake_all(&input_waitq);
		spin_unlock_irqrestore(&input_lock, irq);
		tty_console_input_event(translated);
	} else {
		spin_unlock_irqrestore(&input_lock, irq);
	}
	poll_notify();
}

static void
console_dispatch_worker(void *argument)
{
	(void)argument;
	for (;;) {
		struct console_dispatch_event event;
		unsigned long irq = spin_lock_irqsave(&input_lock);
		while (dispatch_used == 0) {
			uint64_t sequence = waitq_sequence(&dispatch_waitq);
			(void)waitq_sleep(&dispatch_waitq, &input_lock, sequence,
			    0, 0);
		}
		event = dispatch_events[dispatch_tail];
		dispatch_tail =
		    (dispatch_tail + 1U) % CONSOLE_DISPATCH_EVENTS;
		dispatch_used--;
		spin_unlock_irqrestore(&input_lock, irq);
		console_deliver(event.translated, event.device_id,
		    event.overflow, event.repeat);
	}
}

static void
console_input_worker(void *argument)
{
	(void)argument;
	for (;;) {
		struct hal_key_event event;

		if (hal_cons_read_event(&event))
			input_device_emit_key_event(keyboard_input, &event);
	}
}

static ssize_t
console_event_read(struct file *file, void *buffer, size_t size)
{
	size_t capacity, count = 0;
	unsigned long irq;
	if (size < sizeof(struct console_input_event))
		return -EINVAL;
	capacity = size / sizeof(struct console_input_event);
	irq = spin_lock_irqsave(&input_lock);
	while (event_used == 0) {
		uint64_t sequence;
		int error;
		if ((file_status_flags_get(file) & O_NONBLOCK) != 0) {
			spin_unlock_irqrestore(&input_lock, irq);
			return -EAGAIN;
		}
		sequence = waitq_sequence(&input_waitq);
		error = waitq_sleep(&input_waitq, &input_lock, sequence, 0,
				    WAITQ_INTERRUPTIBLE);
		if (error == EINTR) {
			spin_unlock_irqrestore(&input_lock, irq);
			return -EINTR;
		}
	}
	while (count < capacity && event_used != 0) {
		((struct console_input_event *)buffer)[count++] =
		    event_records[event_tail];
		event_tail = (event_tail + 1U) % CONSOLE_EVENT_RECORDS;
		event_used--;
	}
	spin_unlock_irqrestore(&input_lock, irq);
	return (ssize_t)(count * sizeof(struct console_input_event));
}

static ssize_t
console_read(struct file *file, void *buffer, size_t size)
{
	struct console_open *state = console_open_state(file);
	if (state != NULL && state->input_mode == ZEDBSD_CONSOLE_INPUT_EVENT)
		return console_event_read(file, buffer, size);
	return tty_vt_read(console_file_vt(file), file, buffer, size);
}

static ssize_t
console_write(struct file *file, const void *buffer, size_t size)
{
	ssize_t result =
	    tty_vt_write(console_file_vt(file), file, buffer, size);
	if (result < 0)
		return result;
	hal_cons_update_cursor();
	return result;
}

static int
console_write_at(uintptr_t argument)
{
	struct console_write_at request;
	char text[CONSOLE_WRITE_MAX + 1U];
	int error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;
	if (request.row >= HAL_CONS_ROWS ||
	    request.column >= HAL_CONS_COLUMNS ||
	    request.length > CONSOLE_WRITE_MAX)
		return EINVAL;
	error = copyin(request.address, text, request.length);
	if (error != 0)
		return error;
	text[request.length] = '\0';
	return hal_cons_write_n_at(request.row, request.column, text,
				   request.length,
				   (uint8_t)request.attribute) < 0
		   ? EIO
		   : 0;
}

static int
console_ioctl(struct file *file, unsigned long request, uintptr_t argument)
{
	struct hal_cons_state state;
	int error;
	switch (request) {
	case ZEDBSD_CONSOLE_GET_SIZE: {
		const struct console_size size = {HAL_CONS_ROWS,
						  HAL_CONS_COLUMNS};
		return copyout(&size, argument, sizeof(size));
	}
	case ZEDBSD_CONSOLE_CLEAR:
		hal_cons_clear();
		return 0;
	case ZEDBSD_CONSOLE_CLEAR_ROW: {
		struct console_row row;
		error = copyin(argument, &row, sizeof(row));
		if (error != 0)
			return error;
		if (row.row >= HAL_CONS_ROWS)
			return EINVAL;
		hal_cons_clear_row(row.row);
		return 0;
	}
	case ZEDBSD_CONSOLE_CLEAR_TO_EOL: {
		struct console_position position;
		error = copyin(argument, &position, sizeof(position));
		if (error != 0)
			return error;
		return hal_cons_clear_to_eol_at(position.row, position.column)
			   ? 0
			   : EINVAL;
	}
	case ZEDBSD_CONSOLE_GET_CURSOR: {
		struct console_cursor cursor;
		hal_cons_save_state(&state);
		cursor.row = state.row;
		cursor.column = state.column;
		cursor.visible = state.cursor_visible != 0;
		return copyout(&cursor, argument, sizeof(cursor));
	}
	case ZEDBSD_CONSOLE_SET_CURSOR: {
		struct console_cursor cursor;
		error = copyin(argument, &cursor, sizeof(cursor));
		if (error != 0)
			return error;
		return hal_cons_set_cursor(cursor.row, cursor.column) ? 0
								      : EINVAL;
	}
	case ZEDBSD_CONSOLE_SHOW_CURSOR: {
		struct console_cursor cursor;
		error = copyin(argument, &cursor, sizeof(cursor));
		if (error != 0)
			return error;
		hal_cons_show_cursor(cursor.visible != 0);
		return 0;
	}
	case ZEDBSD_CONSOLE_WRITE_AT:
		return console_write_at(argument);
	case ZEDBSD_CONSOLE_POLL_EVENT:
	case ZEDBSD_CONSOLE_READ_EVENT: {
		struct console_event event;
		int value = request == ZEDBSD_CONSOLE_POLL_EVENT
				? console_input_take(0, 0)
				: console_input_take(1, 1);
		if (value == -EINTR)
			return EINTR;
		if (value < 0)
			return EAGAIN;
		event.value = (uint32_t)value;
		return copyout(&event, argument, sizeof(event));
	}
	case ZEDBSD_CONSOLE_GET_INPUT_MODE: {
		struct console_open *open = console_open_state(file);
		struct console_input_mode mode;
		if (open == NULL)
			return ENODEV;
		mode.mode = open->input_mode;
		mode.flags = 0;
		return copyout(&mode, argument, sizeof(mode));
	}
	case ZEDBSD_CONSOLE_SET_INPUT_MODE: {
		struct console_open *open = console_open_state(file);
		struct console_input_mode mode;
		unsigned long irq;
		if (open == NULL)
			return ENODEV;
		error = copyin(argument, &mode, sizeof(mode));
		if (error != 0)
			return error;
		if ((mode.mode != ZEDBSD_CONSOLE_INPUT_TEXT &&
		     mode.mode != ZEDBSD_CONSOLE_INPUT_EVENT) ||
		    mode.flags != 0)
			return EINVAL;
		irq = spin_lock_irqsave(&input_lock);
		if (mode.mode == ZEDBSD_CONSOLE_INPUT_EVENT &&
		    event_owner != NULL && event_owner != open) {
			spin_unlock_irqrestore(&input_lock, irq);
			return EBUSY;
		}
		if (mode.mode == ZEDBSD_CONSOLE_INPUT_EVENT)
			event_owner = open;
		else if (event_owner == open)
			event_owner = NULL;
		open->input_mode = mode.mode;
		event_head = event_tail = event_used = 0;
		waitq_wake_all(&input_waitq);
		spin_unlock_irqrestore(&input_lock, irq);
		poll_notify();
		return 0;
	}
	case ZEDBSD_CONSOLE_KEY_STATE: {
		struct console_key_state key;
		error = copyin(argument, &key, sizeof(key));
		if (error != 0)
			return error;
		key.down = hal_cons_key_state((int)key.key);
		return copyout(&key, argument, sizeof(key));
	}
	case ZEDBSD_CONSOLE_DRAIN_INPUT: {
		unsigned long irq = spin_lock_irqsave(&input_lock);
		console_drain_input_locked();
		spin_unlock_irqrestore(&input_lock, irq);
		poll_notify();
	}
		return 0;
	case ZEDBSD_CONSOLE_ISATTY:
		return 0;
	default:
		return tty_vt_ioctl(console_file_vt(file), file, request,
				    argument);
	}
}

static int
console_poll(struct file *file, short events, short *revents)
{
	struct console_open *state = console_open_state(file);
	if (state != NULL && state->input_mode == ZEDBSD_CONSOLE_INPUT_EVENT) {
		unsigned long irq;
		short result = events & (POLLOUT | POLLWRNORM);
		irq = spin_lock_irqsave(&input_lock);
		if (event_used != 0)
			result |= events & (POLLIN | POLLRDNORM);
		spin_unlock_irqrestore(&input_lock, irq);
		*revents = result;
		return 0;
	}
	return tty_vt_poll(console_file_vt(file), file, events, revents);
}

static ssize_t
vt_read(struct file *file, void *buffer, size_t size)
{
	unsigned vt = (unsigned)((uintptr_t)file->f_data - 1U);
	return tty_vt_read(vt, file, buffer, size);
}

static ssize_t
vt_write(struct file *file, const void *buffer, size_t size)
{
	unsigned vt = (unsigned)((uintptr_t)file->f_data - 1U);
	ssize_t result = tty_vt_write(vt, file, buffer, size);
	if (result >= 0)
		hal_cons_update_cursor();
	return result;
}

static int
vt_poll(struct file *file, short events, short *revents)
{
	unsigned vt = (unsigned)((uintptr_t)file->f_data - 1U);
	return tty_vt_poll(vt, file, events, revents);
}

static int
vt_ioctl(struct file *file, unsigned long request, uintptr_t argument)
{
	unsigned vt = (unsigned)((uintptr_t)file->f_data - 1U);
	return tty_vt_ioctl(vt, file, request, argument);
}

static const struct cdev_ops vt_ops = {
    .read = vt_read,
    .write = vt_write,
    .ioctl = vt_ioctl,
    .poll = vt_poll,
};

static const struct cdev_ops console_ops = {
    .open = console_open_file,
    .close = console_close_file,
    .read = console_read,
    .write = console_write,
    .ioctl = console_ioctl,
    .poll = console_poll,
};

int
console_device_register(void)
{
	struct input_capability capabilities[CONSOLE_KEY_CAPABILITIES];
	struct hal_cons_input_info hal_info;
	struct input_device_info keyboard_info;
	struct thread *producer = NULL, *dispatcher = NULL;
	size_t capability_count;
	int error;

	memset(&hal_info, 0, sizeof(hal_info));
	hal_cons_get_input_info(&hal_info);
	if ((hal_info.flags & ~(HAL_CONS_INPUT_TEXT | HAL_CONS_INPUT_RELEASE |
	    HAL_CONS_INPUT_REPEAT)) != 0 ||
	    ((hal_info.flags & HAL_CONS_INPUT_REPEAT) != 0 &&
	    (hal_info.flags & HAL_CONS_INPUT_RELEASE) == 0) ||
	    (hal_info.symbol_count != 0 && hal_info.symbols == NULL))
		return EINVAL;
	error = console_capabilities(&hal_info, capabilities,
	    &capability_count);
	if (error != 0)
		return error;
	memset(&keyboard_info, 0, sizeof(keyboard_info));
	keyboard_info.name = "zedBSD console keyboard";
	keyboard_info.physical_path = "console/input0";
	keyboard_info.id = (struct input_id){
	    .bustype = BUS_HOST, .product = 1, .version = 1};
	keyboard_info.capabilities = capabilities;
	keyboard_info.capability_count = capability_count;
	if ((hal_info.flags & HAL_CONS_INPUT_RELEASE) == 0)
		keyboard_info.flags |= INPUT_DEVICE_KEY_MOMENTARY;
	if ((hal_info.flags & HAL_CONS_INPUT_REPEAT) != 0)
		keyboard_info.flags |= INPUT_DEVICE_KEY_REPEAT;

	spin_init(&input_lock, LOCK_RANK_DEVICE, "console input");
	waitq_init(&input_waitq, "console input");
	waitq_init(&dispatch_waitq, "console input dispatch");
	input_head = input_tail = input_used = 0;
	event_head = event_tail = event_used = event_sequence = 0;
	dispatch_head = dispatch_tail = dispatch_used = 0;
	event_owner = NULL;
	keyboard_input = NULL;
	input_keymap_init(&early_keymap);
	early_resyncing = 0;
	memset(console_sources, 0, sizeof(console_sources));
	memset(&console_subscription, 0, sizeof(console_subscription));
	error = tty_console_init();
	if (error != 0)
		return error;
	error = kthread_create(console_input_worker, NULL,
			       SCHED_PRIORITY_DEFAULT, &producer);
	if (error != 0)
		return error;
	error = kthread_create(console_dispatch_worker, NULL,
				       SCHED_PRIORITY_DEFAULT, &dispatcher);
	if (error != 0)
		goto fail;
	error = cdev_register("console", 0x00010000U, &console_ops,
				      (void *)(uintptr_t)1U);
	if (error != 0)
		goto fail;
	for (unsigned i = 0; i < tty_vt_count(); i++) {
		char name[] = "ttyv0";
		name[4] = (char)('0' + i);
		error = cdev_register(name, (dev_t)(0x00010010U + i), &vt_ops,
				      (void *)(uintptr_t)(i + 1U));
		if (error != 0)
			goto fail;
	}
	error = tty_pty_register();
	if (error != 0)
		goto fail;
	error = input_device_register(&keyboard_info, &keyboard_input);
	if (error != 0)
		goto fail;
	error = input_subscribe(&console_subscription,
	    console_input_subscriber, NULL);
	if (error != 0)
		goto fail;
	input_started = 1;
	thread_start(dispatcher);
	thread_start(producer);
	hal_cons_set_mode(HAL_CONS_TERMINAL);
	return 0;

fail:
	input_unsubscribe(&console_subscription);
	if (keyboard_input != NULL) {
		input_device_unregister(keyboard_input);
		keyboard_input = NULL;
	}
	if (dispatcher != NULL)
		(void)thread_abort_new(dispatcher);
	if (producer != NULL)
		(void)thread_abort_new(producer);
	input_started = 0;
	return error;
}
#endif
