/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

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

static struct console_dispatch_event dispatch_events[CONSOLE_DISPATCH_EVENTS];
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

static void console_drain_input_locked(void);

/*
 * Forward declaration.
 */
static struct console_open *console_open_state(struct file *file);
static unsigned console_file_vt(struct file *file);
static int console_open_file(struct file *file);
static int console_close_file(struct file *file);
static int console_capability_add(struct input_capability *capabilities, size_t *count, uint16_t code);
static int console_capabilities(const struct hal_cons_input_info *hal_info, struct input_capability *capabilities, size_t *count);
static int console_input_take(int consume, int wait);
static struct console_source_state * console_source_find(struct input_device *source, int create);
static uint32_t console_source_active_key(struct console_source_state *source, const struct input_report_event *item, uint32_t translated);
static void console_dispatch_enqueue(uint32_t translated, unsigned device_id, unsigned repeat);
static void console_input_subscriber(void *context, const struct input_report *report);
static void console_deliver(uint32_t translated, unsigned device_id, unsigned overflow, unsigned repeat);
static void console_dispatch_worker(void *argument);
static void console_input_worker(void *argument);
static ssize_t console_event_read(struct file *file, void *buffer, size_t size);
static ssize_t console_read(struct file *file, void *buffer, size_t size);
static ssize_t console_write(struct file *file, const void *buffer, size_t size);
static int console_write_at(uintptr_t argument);
static int console_ioctl(struct file *file, unsigned long request, uintptr_t argument);
static int console_poll(struct file *file, short events, short *revents);
static ssize_t vt_read(struct file *file, void *buffer, size_t size);
static ssize_t vt_write(struct file *file, const void *buffer, size_t size);
static int vt_poll(struct file *file, short events, short *revents);
static int vt_ioctl(struct file *file, unsigned long request, uintptr_t argument);

/* Supports the console drain input locked operation. */
static void
console_drain_input_locked(
	void)
{
	input_head = input_tail = input_used = 0;
	event_head = event_tail = event_used = 0;
	dispatch_head = dispatch_tail = dispatch_used = 0;

	/* The broker owns HAL transitions once started; never drop a break. */
	if (!input_started)
		hal_cons_drain_input();
}

#ifndef ZEDBSD_INPUT_OWNERSHIP_TEST

/* Supports the console open state operation. */
static struct console_open *
console_open_state(
	struct file *file)
{
	/* Returns the computed result. */
	return file != NULL ? file->f_data : NULL;
}

/* Supports the console file vt operation. */
static unsigned
console_file_vt(
	struct file *file)
{
	struct console_open *state = console_open_state(file);

	/* Returns the computed result. */
	return state != NULL ? state->vt : 0U;
}

/* Supports the console open file operation. */
static int
console_open_file(
	struct file *file)
{
	struct console_open *state = kern_malloc(sizeof(*state));

	/* Handles the state availability. */
	if (state == NULL)
		return ENOMEM;
	state->vt = 0;
	state->input_mode = ZEDBSD_CONSOLE_INPUT_TEXT;
	file->f_data = state;

	/* Reports successful completion. */
	return 0;
}

/* Supports the console close file operation. */
static int
console_close_file(
	struct file *file)
{
	struct console_open *state = console_open_state(file);
	unsigned long irq;

	/* Handles the state availability. */
	if (state == NULL)
		return 0;
	irq = spin_lock_irqsave(&input_lock);

	/* Handles the event owner condition. */
	if (event_owner == state) {
		event_owner = NULL;
		event_head = event_tail = event_used = 0;
		waitq_wake_all(&input_waitq);
	}

	spin_unlock_irqrestore(&input_lock, irq);

	kern_free(state);
	file->f_data = NULL;
	poll_notify();

	/* Reports successful completion. */
	return 0;
}

/* Supports the console capability add operation. */
static int
console_capability_add(
	struct input_capability *capabilities,
	size_t *count,
	uint16_t code)
{
	size_t index;

	/* Handles the code condition. */
	if (code == KEY_RESERVED)
		return 0;
	/* Process each remaining element. */
	for (index = 0; index < *count; index++) {
		/* Handles the capabilities condition. */
		if (capabilities[index].type == EV_KEY &&
		    capabilities[index].code == code) {
			/* Reports successful completion. */
			return 0;
		}
	}

	/* Checks the remaining item count. */
	if (*count == CONSOLE_KEY_CAPABILITIES)
		return ENOSPC;
	capabilities[*count].type = EV_KEY;
	capabilities[*count].code = code;
	(*count)++;

	/* Reports successful completion. */
	return 0;
}

/* Supports the console capabilities operation. */
static int
console_capabilities(
	const struct hal_cons_input_info *hal_info,
	struct input_capability *capabilities,
	size_t *count)
{
	int error_local;
	int error_local1;
	unsigned character;
	size_t index;

	*count = 1;
	capabilities[0].type = EV_SYN;
	capabilities[0].code = SYN_REPORT;

	/* Handles the hal info condition. */
	if ((hal_info->flags & HAL_CONS_INPUT_TEXT) != 0) {
		/* Process each element required by the operation. */
		for (character = 1; character < 0x80U; character++) {
			char symbol[2] = {(char)character, '\0'};

			/* Checks the operation status. */
			error_local = console_capability_add(
				capabilities, count,
				drv_input_key_from_symbol(symbol));
			if (error_local != 0)
				return error_local;
		}
	}

	/* Process each remaining element. */
	for (index = 0; index < hal_info->symbol_count; index++) {
		/* Checks the operation status. */
		error_local1 = console_capability_add(
			capabilities, count,
			drv_input_key_from_symbol(hal_info->symbols[index]));
		if (error_local1 != 0)
			return error_local1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the console input take operation. */
static int
console_input_take(
	int consume,
	int wait)
{
	uint8_t caps;
	uint8_t kana;
	struct hal_key_event event;
	struct input_keymap_state state;
	uint32_t translated;
	int available;
	uint64_t sequence;
	int error;
	unsigned long irq;
	int result;

	/* Handles the input started condition. */
	if (!input_started) {
		/* Continue until the operation reaches a terminal state. */
		for (;;) {
			state = early_keymap;

			/* Handles the available condition. */
			available = wait ? hal_cons_read_event(&event)
					 : hal_cons_poll_event(&event);
			if (!available)
				return -1;

			/* Handles the wait condition. */
			if (!wait &&
			    ((event.flags &
			      (HAL_KEY_EVENT_RESYNC | HAL_KEY_EVENT_SNAPSHOT |
			       HAL_KEY_EVENT_RESYNC_END)) != 0 ||
			     early_resyncing))
				(void)hal_cons_read_event(&event);

			/* Handles the event condition. */
			if ((event.flags & HAL_KEY_EVENT_RESYNC) != 0) {
				drv_input_keymap_init(&early_keymap);
				early_keymap.caps_lock =
					(event.flags &
					 HAL_KEY_EVENT_LOCK_CAPS) != 0;
				early_keymap.kana_lock =
					(event.flags &
					 HAL_KEY_EVENT_LOCK_KANA) != 0;
				early_resyncing = 1;
				continue;
			}

			/* Handles the event condition. */
			if (event.flags == (HAL_KEY_EVENT_PRESS |
					    HAL_KEY_EVENT_SNAPSHOT) &&
			    early_resyncing) {
				caps = early_keymap.caps_lock;
				kana = early_keymap.kana_lock;

				/* Translates the key without disturbing the lock state. */
				event.flags = HAL_KEY_EVENT_PRESS;
				(void)drv_input_keymap_translate(
					&early_keymap, &event, &translated);
				early_keymap.caps_lock = caps;
				early_keymap.kana_lock = kana;
				continue;
			}

			/* Handles the event condition. */
			if (event.flags == HAL_KEY_EVENT_RESYNC_END) {
				early_resyncing = 0;
				continue;
			}

			/* Handles the early resyncing condition. */
			if (early_resyncing)
				continue;

			/* Checks the drv input keymap translate result. */
			if (!drv_input_keymap_translate(&state, &event,
							&translated)) {
				/* Reports operation failure. */
				return -1;
			}

			/* Handles the consume condition. */
			if (consume)
				early_keymap = state;

			/* Returns the computed result. */
			return (int)translated;
		}
	}

	irq = spin_lock_irqsave(&input_lock);

	/* Continue while the operation condition remains true. */
	while (input_used == 0) {
		/* Handles the wait condition. */
		if (!wait) {
			spin_unlock_irqrestore(&input_lock, irq);

			/* Reports operation failure. */
			return -1;
		}

		sequence = waitq_sequence(&input_waitq);

		/* Checks the operation status. */
		error = waitq_sleep(&input_waitq, &input_lock, sequence, 0,
				    WAITQ_INTERRUPTIBLE);
		if (error == EINTR) {
			spin_unlock_irqrestore(&input_lock, irq);

			/* Returns the computed result. */
			return -EINTR;
		}
	}

	result = (int)input_events[input_tail];

	/* Handles the consume condition. */
	if (consume) {
		input_tail = (input_tail + 1U) % CONSOLE_INPUT_EVENTS;
		input_used--;
	}

	spin_unlock_irqrestore(&input_lock, irq);

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the drv console input poll event operation.
 */
int
drv_console_input_poll_event(
	void)
{
	int error;

	/* Obtains the console input take result. */
	error = console_input_take(0, 0);

	/* Returns the computed result. */
	return error;
}
/*
 * Implements the drv console input read event operation.
 */
int
drv_console_input_read_event(
	void)
{
	int error;

	/* Obtains the console input take result. */
	error = console_input_take(1, 1);

	/* Returns the computed result. */
	return error;
}
#endif

/* Supports the console source find operation. */
static struct console_source_state *
console_source_find(
	struct input_device *source,
	int create)
{
	unsigned index;
	struct console_source_state *empty = NULL;

	/* Process each remaining element. */
	for (index = 0; index < CONSOLE_INPUT_SOURCES; index++) {
		/* Handles the console sources condition. */
		if (console_sources[index].source == source)
			return &console_sources[index];

		/* Handles the source availability. */
		if (console_sources[index].source == NULL && empty == NULL)
			empty = &console_sources[index];
	}

	/* Handles the empty availability. */
	if (!create || empty == NULL)
		return NULL;
	empty->source = source;
	drv_input_keymap_init(&empty->keymap);

	/* Returns the computed result. */
	return empty;
}

/* Supports the console source active key operation. */
static uint32_t
console_source_active_key(
	struct console_source_state *source,
	const struct input_report_event *item,
	uint32_t translated)
{
	struct console_logical_key *empty;
	uint16_t *active = NULL;
	unsigned index;

	/* Handles the item condition. */
	if (item->event.code <= KEY_MAX && item->event.code != KEY_RESERVED) {
		active = &source->active[item->event.code];
	} else if (item->symbol[0] != '\0') {
		empty = NULL;

		/* Process each remaining element. */
		for (index = 0; index < CONSOLE_LOGICAL_KEYS; index++) {
			/* Selects the matching value. */
			if (strcmp(source->logical[index].symbol,
				   item->symbol) == 0) {
				active = &source->logical[index].key;
				break;
			}

			/* Handles the empty availability. */
			if (source->logical[index].symbol[0] == '\0' &&
			    empty == NULL)
				empty = &source->logical[index];
		}

		/* Handles the active availability. */
		if (active == NULL && item->event.value != 0 && empty != NULL) {
			memcpy(empty->symbol, item->symbol,
			       sizeof(empty->symbol));
			active = &empty->key;
		}
	}

	/* Handles the active availability. */
	if (active == NULL)
		return translated;

	/* Handles the item condition. */
	if (item->event.value != 0)
		*active = (uint16_t)(translated & INPUT_KEY_MASK);
	else if (*active != 0)
		translated = (translated & ~INPUT_KEY_MASK) | *active;

	/* Handles the item condition. */
	if (item->event.value == 0) {
		*active = 0;
		/* Handles the item condition. */
		if (item->event.code == KEY_RESERVED) {
			/* Process each remaining element. */
			for (index = 0; index < CONSOLE_LOGICAL_KEYS; index++) {
				/* Handles the source condition. */
				if (&source->logical[index].key == active) {
					memset(&source->logical[index], 0,
					       sizeof(source->logical[index]));
					break;
				}
			}
		}
	}

	/* Returns the computed result. */
	return translated;
}

/* Supports the console dispatch enqueue operation. */
static void
console_dispatch_enqueue(
	uint32_t translated,
	unsigned device_id,
	unsigned repeat)
{
	struct console_dispatch_event event;

	memset(&event, 0, sizeof(event));
	event.translated = translated;
	event.device_id = device_id;
	event.repeat = repeat;

	/* Handles the dispatch used condition. */
	if (dispatch_used == CONSOLE_DISPATCH_EVENTS) {
		event.overflow = 1;

		/* Handles the dispatch events condition. */
		if (dispatch_events[dispatch_tail].overflow != 0) {
			event.overflow =
				dispatch_events[dispatch_tail].overflow;
		}

		dispatch_tail = (dispatch_tail + 1U) % CONSOLE_DISPATCH_EVENTS;
		dispatch_used--;
	}

	dispatch_events[dispatch_head] = event;
	dispatch_head = (dispatch_head + 1U) % CONSOLE_DISPATCH_EVENTS;
	dispatch_used++;
}

/* Translation is deliberately completed in this bounded callback.  The dispatch ring may lose old output under overload, but it can never lose a modifier transition from the per-source translation state. */
static void
console_input_subscriber(
	void *context,
	const struct input_report *report)
{
	const struct input_report_event *item_local;
	struct hal_key_event key_event_local;
	uint32_t translated_local;
	const struct input_report_event *item_local1;
	struct hal_key_event key_event_local2;
	uint32_t translated_local3;
	uint8_t caps, kana;
	uint32_t modifiers;
	struct console_source_state *source;
	unsigned long irq;
	size_t index;
	int queued = 0;

	(void)context;

	/* Handles the report availability. */
	if (report == NULL)
		return;
	irq = spin_lock_irqsave(&input_lock);

	/* Handles the report condition. */
	if ((report->flags & INPUT_REPORT_RESYNC_BEGIN) != 0) {
		/* Handles the source availability. */
		source = console_source_find(report->device, 1);
		if (source != NULL) {
			memset(source, 0, sizeof(*source));
			source->source = report->device;
			drv_input_keymap_init(&source->keymap);
			source->keymap.caps_lock =
				(report->flags & INPUT_REPORT_LOCK_CAPS) != 0;
			source->keymap.kana_lock =
				(report->flags & INPUT_REPORT_LOCK_KANA) != 0;
			source->resyncing = 1;
		}

		spin_unlock_irqrestore(&input_lock, irq);

		/* Returns the computed result. */
		return;
	}

	/* Handles the report condition. */
	source = console_source_find(report->device, report->flags == 0);
	if ((report->flags & INPUT_REPORT_SNAPSHOT) != 0) {
		/* Handles the source availability. */
		if (source != NULL && source->resyncing) {
			/* Process each remaining element. */
			for (index = 0; index < report->event_count; index++) {
				/* Handles the item local condition. */
				item_local = &report->events[index];
				if (item_local->event.type != EV_KEY ||
				    item_local->event.value != 1 ||
				    item_local->symbol[0] == '\0')
					continue;
				memset(&key_event_local, 0,
				       sizeof(key_event_local));
				memcpy(key_event_local.symbol,
				       item_local->symbol,
				       sizeof(key_event_local.symbol));
				key_event_local.flags = HAL_KEY_EVENT_PRESS;
				caps = source->keymap.caps_lock;
				kana = source->keymap.kana_lock;

				/* Checks the drv input keymap translate result. */
				if (drv_input_keymap_translate(
					    &source->keymap, &key_event_local,
					    &translated_local)) {
					(void)console_source_active_key(
						source, item_local,
						translated_local);
				}

				source->keymap.caps_lock = caps;
				source->keymap.kana_lock = kana;
			}
		}

		spin_unlock_irqrestore(&input_lock, irq);

		/* Returns the computed result. */
		return;
	}

	/* Handles the report condition. */
	if ((report->flags & INPUT_REPORT_RESYNC_END) != 0) {
		/* Handles the source availability. */
		if (source != NULL)
			source->resyncing = 0;
		spin_unlock_irqrestore(&input_lock, irq);

		/* Returns the computed result. */
		return;
	}

	/* Handles the source availability. */
	if (source != NULL && !source->resyncing) {
		/* Process each remaining element. */
		for (index = 0; index < report->event_count; index++) {
			/* Handles the item local1 condition. */
			item_local1 = &report->events[index];
			if (item_local1->event.type != EV_KEY)
				continue;

			/* Handles the item local1 condition. */
			if (item_local1->symbol[0] != '\0') {
				memset(&key_event_local2, 0,
				       sizeof(key_event_local2));
				memcpy(key_event_local2.symbol,
				       item_local1->symbol,
				       sizeof(key_event_local2.symbol));
				key_event_local2.flags = item_local1->key_flags;
			} else if (!drv_input_keymap_event_from_code(
					   item_local1->event.code,
					   item_local1->event.value,
					   &key_event_local2)) {
				continue;
			}

			/* Checks the drv input keymap translate result. */
			if (!drv_input_keymap_translate(&source->keymap,
							&key_event_local2,
							&translated_local3))
				continue;
			translated_local3 = console_source_active_key(
				source, item_local1, translated_local3);
			console_dispatch_enqueue(translated_local3,
						 report->device_id,
						 item_local1->event.value == 2);
			queued = 1;
		}
	}

	/* Handles the source availability. */
	if ((report->flags & INPUT_REPORT_DETACH) != 0 && source != NULL &&
	    !source->resyncing) {
		/* Process each remaining element. */
		for (index = 0; index < CONSOLE_LOGICAL_KEYS; index++) {
			/* Handles the source condition. */
			if (source->logical[index].key == 0)
				continue;
			modifiers =
				(source->keymap.left_shift ||
						 source->keymap.right_shift
					 ? INPUT_KEY_SHIFT
					 : 0U) |
				(source->keymap.left_control ||
						 source->keymap.right_control
					 ? INPUT_KEY_CTRL
					 : 0U) |
				(source->keymap.left_graph ||
						 source->keymap.right_graph
					 ? INPUT_KEY_GRAPH
					 : 0U);
			console_dispatch_enqueue(source->logical[index].key |
							 modifiers |
							 INPUT_KEY_RELEASE,
						 report->device_id, 0);
			queued = 1;
		}
	}

	/* Handles the source availability. */
	if ((report->flags & INPUT_REPORT_DETACH) != 0 && source != NULL)
		memset(source, 0, sizeof(*source));

	/* Handles the queued condition. */
	if (queued)
		waitq_wake_all(&dispatch_waitq);

	spin_unlock_irqrestore(&input_lock, irq);
}

#ifdef ZEDBSD_INPUT_OWNERSHIP_TEST
/*
 * Implements the drv console input ownership test reset operation.
 */
void
drv_console_input_ownership_test_reset(
	void)
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

/*
 * Implements the drv console input ownership test publish operation.
 */
void
drv_console_input_ownership_test_publish(
	const struct input_report *report)
{
	console_input_subscriber(NULL, report);
}

/*
 * Implements the drv console input ownership test pop operation.
 */
int
drv_console_input_ownership_test_pop(
	uint32_t *translated,
	unsigned *device_id,
	unsigned *repeat)
{
	struct console_dispatch_event event;
	unsigned long irq = spin_lock_irqsave(&input_lock);

	/* Handles the dispatch used condition. */
	if (dispatch_used == 0) {
		spin_unlock_irqrestore(&input_lock, irq);

		/* Reports successful completion. */
		return 0;
	}

	event = dispatch_events[dispatch_tail];
	dispatch_tail = (dispatch_tail + 1U) % CONSOLE_DISPATCH_EVENTS;
	dispatch_used--;

	spin_unlock_irqrestore(&input_lock, irq);

	/* Handles the translated availability. */
	if (translated != NULL)
		*translated = event.translated;
	/* Handles the device id availability. */
	if (device_id != NULL)
		*device_id = event.device_id;
	/* Handles the repeat availability. */
	if (repeat != NULL)
		*repeat = event.repeat;
	/* Reports operation failure. */
	return 1;
}

/*
 * Implements the drv console input ownership test state operation.
 */
int
drv_console_input_ownership_test_state(
	struct input_device *device,
	unsigned code,
	unsigned *caps,
	unsigned *kana,
	unsigned *shift,
	uint16_t *active,
	int *resyncing)
{
	struct console_source_state *source;
	unsigned long irq = spin_lock_irqsave(&input_lock);

	/* Handles the source availability. */
	source = console_source_find(device, 0);
	if (source == NULL || code > KEY_MAX) {
		spin_unlock_irqrestore(&input_lock, irq);

		/* Reports successful completion. */
		return 0;
	}

	/* Handles the caps availability. */
	if (caps != NULL)
		*caps = source->keymap.caps_lock;
	/* Handles the kana availability. */
	if (kana != NULL)
		*kana = source->keymap.kana_lock;
	/* Handles the shift availability. */
	if (shift != NULL) {
		*shift =
			source->keymap.left_shift || source->keymap.right_shift;
	}

	/* Handles the active availability. */
	if (active != NULL)
		*active = source->active[code];
	/* Handles the resyncing availability. */
	if (resyncing != NULL)
		*resyncing = source->resyncing;

	spin_unlock_irqrestore(&input_lock, irq);

	/* Reports operation failure. */
	return 1;
}

/*
 * Implements the drv console input ownership test drain operation.
 */
void
drv_console_input_ownership_test_drain(
	int started)
{
	unsigned long irq = spin_lock_irqsave(&input_lock);

	input_started = started != 0;
	console_drain_input_locked();

	spin_unlock_irqrestore(&input_lock, irq);
}
#else

/* Supports the console deliver operation. */
static void
console_deliver(
	uint32_t translated,
	unsigned device_id,
	unsigned overflow,
	unsigned repeat)
{
	struct console_input_event *record;
	unsigned flags;
	unsigned long irq = spin_lock_irqsave(&input_lock);

	/* Handles the event owner availability. */
	if (event_owner != NULL) {
		/* Handles the event used condition. */
		flags = overflow != 0 ? ZEDBSD_CONSOLE_INPUT_FLAG_OVERFLOW : 0;
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
		record->modifiers =
			translated &
			(INPUT_KEY_SHIFT | INPUT_KEY_CTRL | INPUT_KEY_GRAPH);
		record->state = (translated & INPUT_KEY_RELEASE) != 0
					? ZEDBSD_CONSOLE_KEY_RELEASE
				: repeat != 0 ? ZEDBSD_CONSOLE_KEY_REPEAT
					      : ZEDBSD_CONSOLE_KEY_PRESS;
		event_head = (event_head + 1U) % CONSOLE_EVENT_RECORDS;
		event_used++;
		waitq_wake_all(&input_waitq);
		spin_unlock_irqrestore(&input_lock, irq);
	} else if ((translated & INPUT_KEY_RELEASE) == 0) {
		/* Handles the input used condition. */
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

/* Supports the console dispatch worker operation. */
static void
console_dispatch_worker(
	void *argument)
{
	uint64_t sequence;
	struct console_dispatch_event event;
	unsigned long irq;

	(void)argument;
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		irq = spin_lock_irqsave(&input_lock);
		/* Continue while the operation condition remains true. */
		while (dispatch_used == 0) {
			sequence = waitq_sequence(&dispatch_waitq);
			(void)waitq_sleep(&dispatch_waitq, &input_lock,
					  sequence, 0, 0);
		}

		event = dispatch_events[dispatch_tail];
		dispatch_tail = (dispatch_tail + 1U) % CONSOLE_DISPATCH_EVENTS;
		dispatch_used--;
		spin_unlock_irqrestore(&input_lock, irq);
		console_deliver(event.translated, event.device_id,
				event.overflow, event.repeat);
	}
}

/* Supports the console input worker operation. */
static void
console_input_worker(
	void *argument)
{
	struct hal_key_event event;

	(void)argument;
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles the hal cons read event condition. */
		if (hal_cons_read_event(&event))
			drv_input_device_emit_key_event(keyboard_input, &event);
	}
}

/* Supports the console event read operation. */
static ssize_t
console_event_read(
	struct file *file,
	void *buffer,
	size_t size)
{
	ssize_t function_result;
	uint64_t sequence;
	int error;
	size_t capacity, count = 0;
	unsigned long irq;

	/* Checks the current data size. */
	if (size < sizeof(struct console_input_event))
		return -EINVAL;
	capacity = size / sizeof(struct console_input_event);
	irq = spin_lock_irqsave(&input_lock);

	/* Continue while the operation condition remains true. */
	while (event_used == 0) {
		/* Checks the file status flags get result. */
		if ((file_status_flags_get(file) & O_NONBLOCK) != 0) {
			spin_unlock_irqrestore(&input_lock, irq);

			/* Returns the computed result. */
			return -EAGAIN;
		}

		sequence = waitq_sequence(&input_waitq);

		/* Checks the operation status. */
		error = waitq_sleep(&input_waitq, &input_lock, sequence, 0,
				    WAITQ_INTERRUPTIBLE);
		if (error == EINTR) {
			spin_unlock_irqrestore(&input_lock, irq);

			/* Returns the computed result. */
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

	/* Computes the function result. */
	function_result = (ssize_t)(count * sizeof(struct console_input_event));

	/* Returns the computed result. */
	return function_result;
}

/* Supports the console read operation. */
static ssize_t
console_read(
	struct file *file,
	void *buffer,
	size_t size)
{
	ssize_t function_result;
	struct console_open *state = console_open_state(file);

	/* Handles the state availability. */
	if (state != NULL && state->input_mode == ZEDBSD_CONSOLE_INPUT_EVENT) {
		/* Obtains the console event read result. */
		function_result = console_event_read(file, buffer, size);

		/* Returns the computed result. */
		return function_result;
	}

	/* Obtains the tty vt read result. */
	function_result =
		tty_vt_read(console_file_vt(file), file, buffer, size);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the console write operation. */
static ssize_t
console_write(
	struct file *file,
	const void *buffer,
	size_t size)
{
	ssize_t result =
		tty_vt_write(console_file_vt(file), file, buffer, size);

	/* Checks the operation result. */
	if (result < 0)
		return result;
	hal_cons_update_cursor();

	/* Returns the computed result. */
	return result;
}

/* Supports the console write at operation. */
static int
console_write_at(
	uintptr_t argument)
{
	int function_result;
	struct console_write_at request;
	char text[CONSOLE_WRITE_MAX + 1U];
	int error = copyin(argument, &request, sizeof(request));

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the request condition. */
	if (request.row >= HAL_CONS_ROWS ||
	    request.column >= HAL_CONS_COLUMNS ||
	    request.length > CONSOLE_WRITE_MAX) {
		/* Returns the computed result. */
		return EINVAL;
	}

	/* Checks the operation status. */
	error = copyin(request.address, text, request.length);
	if (error != 0)
		return error;
	text[request.length] = '\0';

	/* Computes the function result. */
	function_result = hal_cons_write_n_at(request.row, request.column, text,
					      request.length,
					      (uint8_t)request.attribute) < 0
				  ? EIO
				  : 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the console ioctl operation. */
static int
console_ioctl(
	struct file *file,
	unsigned long request,
	uintptr_t argument)
{
	const struct console_size size = {HAL_CONS_ROWS, HAL_CONS_COLUMNS};
	int function_result;
	struct console_cursor cursor_local;
	struct console_cursor cursor_local1;
	struct console_cursor cursor_local2;
	struct console_open *open_local;
	struct console_input_mode mode_local;
	struct console_open *open_local3;
	struct console_input_mode mode_local4;
	unsigned long irq_local;
	unsigned long irq_local5;
	struct console_row row;
	struct console_position position;
	struct console_event event;
	int value;
	struct console_key_state key;
	struct hal_cons_state state;
	int error;

	/* Dispatch the selected operation case. */
	switch (request) {
	case ZEDBSD_CONSOLE_GET_SIZE:

		/* Obtains the copyout result. */
		function_result = copyout(&size, argument, sizeof(size));

		/* Returns the computed result. */
		return function_result;
	case ZEDBSD_CONSOLE_CLEAR:
		hal_cons_clear();

		/* Reports successful completion. */
		return 0;
	case ZEDBSD_CONSOLE_CLEAR_ROW:

		/* Checks the operation status. */
		error = copyin(argument, &row, sizeof(row));
		if (error != 0)
			return error;

		/* Handles the row condition. */
		if (row.row >= HAL_CONS_ROWS)
			return EINVAL;
		hal_cons_clear_row(row.row);

		/* Reports successful completion. */
		return 0;
	case ZEDBSD_CONSOLE_CLEAR_TO_EOL:

		/* Checks the operation status. */
		error = copyin(argument, &position, sizeof(position));
		if (error != 0)
			return error;

		/* Computes the function result. */
		function_result =
			hal_cons_clear_to_eol_at(position.row, position.column)
				? 0
				: EINVAL;

		/* Returns the computed result. */
		return function_result;
	case ZEDBSD_CONSOLE_GET_CURSOR:

		hal_cons_save_state(&state);
		cursor_local.row = state.row;
		cursor_local.column = state.column;
		cursor_local.visible = state.cursor_visible != 0;

		/* Obtains the copyout result. */
		function_result =
			copyout(&cursor_local, argument, sizeof(cursor_local));

		/* Returns the computed result. */
		return function_result;
	case ZEDBSD_CONSOLE_SET_CURSOR:

		/* Checks the operation status. */
		error = copyin(argument, &cursor_local1, sizeof(cursor_local1));
		if (error != 0)
			return error;

		/* Computes the function result. */
		function_result = hal_cons_set_cursor(cursor_local1.row,
						      cursor_local1.column)
					  ? 0
					  : EINVAL;

		/* Returns the computed result. */
		return function_result;
	case ZEDBSD_CONSOLE_SHOW_CURSOR:

		/* Checks the operation status. */
		error = copyin(argument, &cursor_local2, sizeof(cursor_local2));
		if (error != 0)
			return error;
		hal_cons_show_cursor(cursor_local2.visible != 0);

		/* Reports successful completion. */
		return 0;
	case ZEDBSD_CONSOLE_WRITE_AT:
		/* Obtains the console write at result. */
		function_result = console_write_at(argument);

		/* Returns the computed result. */
		return function_result;
	case ZEDBSD_CONSOLE_POLL_EVENT:
	case ZEDBSD_CONSOLE_READ_EVENT:

		/* Validates the current value. */
		value = request == ZEDBSD_CONSOLE_POLL_EVENT
				? console_input_take(0, 0)
				: console_input_take(1, 1);
		if (value == -EINTR)
			return EINTR;

		/* Validates the current value. */
		if (value < 0)
			return EAGAIN;
		event.value = (uint32_t)value;

		/* Obtains the copyout result. */
		function_result = copyout(&event, argument, sizeof(event));

		/* Returns the computed result. */
		return function_result;
	case ZEDBSD_CONSOLE_GET_INPUT_MODE:

		/* Handles the open local availability. */
		open_local = console_open_state(file);
		if (open_local == NULL)
			return ENODEV;
		mode_local.mode = open_local->input_mode;
		mode_local.flags = 0;

		/* Obtains the copyout result. */
		function_result =
			copyout(&mode_local, argument, sizeof(mode_local));

		/* Returns the computed result. */
		return function_result;
	case ZEDBSD_CONSOLE_SET_INPUT_MODE:

		/* Handles the open local3 availability. */
		open_local3 = console_open_state(file);
		if (open_local3 == NULL)
			return ENODEV;

		/* Checks the operation status. */
		error = copyin(argument, &mode_local4, sizeof(mode_local4));
		if (error != 0)
			return error;

		/* Handles the mode local4 condition. */
		if ((mode_local4.mode != ZEDBSD_CONSOLE_INPUT_TEXT &&
		     mode_local4.mode != ZEDBSD_CONSOLE_INPUT_EVENT) ||
		    mode_local4.flags != 0) {
			/* Returns the computed result. */
			return EINVAL;
		}

		/* Handles the event owner availability. */
		irq_local = spin_lock_irqsave(&input_lock);
		if (mode_local4.mode == ZEDBSD_CONSOLE_INPUT_EVENT &&
		    event_owner != NULL && event_owner != open_local3) {
			spin_unlock_irqrestore(&input_lock, irq_local);

			/* Returns the computed result. */
			return EBUSY;
		}

		/* Handles the mode local4 condition. */
		if (mode_local4.mode == ZEDBSD_CONSOLE_INPUT_EVENT)
			event_owner = open_local3;
		else if (event_owner == open_local3)
			event_owner = NULL;
		open_local3->input_mode = mode_local4.mode;
		event_head = event_tail = event_used = 0;
		waitq_wake_all(&input_waitq);
		spin_unlock_irqrestore(&input_lock, irq_local);
		poll_notify();

		/* Reports successful completion. */
		return 0;
	case ZEDBSD_CONSOLE_KEY_STATE:

		/* Checks the operation status. */
		error = copyin(argument, &key, sizeof(key));
		if (error != 0)
			return error;
		key.down = hal_cons_key_state((int)key.key);

		/* Obtains the copyout result. */
		function_result = copyout(&key, argument, sizeof(key));

		/* Returns the computed result. */
		return function_result;
	case ZEDBSD_CONSOLE_DRAIN_INPUT:
		irq_local5 = spin_lock_irqsave(&input_lock);
		console_drain_input_locked();
		spin_unlock_irqrestore(&input_lock, irq_local5);
		poll_notify();

		/* Reports successful completion. */
		return 0;
	case ZEDBSD_CONSOLE_ISATTY:
		/* Reports successful completion. */
		return 0;
	default:
		/* Obtains the tty vt ioctl result. */
		function_result = tty_vt_ioctl(console_file_vt(file), file,
					       request, argument);

		/* Returns the computed result. */
		return function_result;
	}
}

/* Supports the console poll operation. */
static int
console_poll(
	struct file *file,
	short events,
	short *revents)
{
	int error;
	unsigned long irq;
	short result;
	struct console_open *state = console_open_state(file);

	/* Handles the state availability. */
	if (state != NULL && state->input_mode == ZEDBSD_CONSOLE_INPUT_EVENT) {
		result = events & (POLLOUT | POLLWRNORM);

		/* Handles the event used condition. */
		irq = spin_lock_irqsave(&input_lock);
		if (event_used != 0)
			result |= events & (POLLIN | POLLRDNORM);
		spin_unlock_irqrestore(&input_lock, irq);
		*revents = result;
		/* Reports successful completion. */
		return 0;
	}

	/* Obtains the tty vt poll result. */
	error =
		tty_vt_poll(console_file_vt(file), file, events, revents);

	/* Returns the computed result. */
	return error;
}

/* Supports the vt read operation. */
static ssize_t
vt_read(
	struct file *file,
	void *buffer,
	size_t size)
{
	ssize_t function_result;
	unsigned vt = (unsigned)((uintptr_t)file->f_data - 1U);

	/* Obtains the tty vt read result. */
	function_result = tty_vt_read(vt, file, buffer, size);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the vt write operation. */
static ssize_t
vt_write(
	struct file *file,
	const void *buffer,
	size_t size)
{
	unsigned vt = (unsigned)((uintptr_t)file->f_data - 1U);
	ssize_t result = tty_vt_write(vt, file, buffer, size);

	/* Checks the operation result. */
	if (result >= 0)
		hal_cons_update_cursor();

	/* Returns the computed result. */
	return result;
}

/* Supports the vt poll operation. */
static int
vt_poll(
	struct file *file,
	short events,
	short *revents)
{
	int error;
	unsigned vt = (unsigned)((uintptr_t)file->f_data - 1U);

	/* Obtains the tty vt poll result. */
	error = tty_vt_poll(vt, file, events, revents);

	/* Returns the computed result. */
	return error;
}

/* Supports the vt ioctl operation. */
static int
vt_ioctl(
	struct file *file,
	unsigned long request,
	uintptr_t argument)
{
	int error;
	unsigned vt = (unsigned)((uintptr_t)file->f_data - 1U);

	/* Obtains the tty vt ioctl result. */
	error = tty_vt_ioctl(vt, file, request, argument);

	/* Returns the computed result. */
	return error;
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

/*
 * Implements the drv console device register operation.
 */
int
drv_console_device_register(
	void)
{
	unsigned i_index_for;
	struct input_capability capabilities[CONSOLE_KEY_CAPABILITIES];
	struct hal_cons_input_info hal_info;
	struct input_device_info keyboard_info;
	struct thread *producer = NULL, *dispatcher = NULL;
	size_t capability_count;
	int error;

	memset(&hal_info, 0, sizeof(hal_info));
	hal_cons_get_input_info(&hal_info);

	/* Handles the symbols availability. */
	if ((hal_info.flags & ~(HAL_CONS_INPUT_TEXT | HAL_CONS_INPUT_RELEASE |
				HAL_CONS_INPUT_REPEAT)) != 0 ||
	    ((hal_info.flags & HAL_CONS_INPUT_REPEAT) != 0 &&
	     (hal_info.flags & HAL_CONS_INPUT_RELEASE) == 0) ||
	    (hal_info.symbol_count != 0 && hal_info.symbols == NULL)) {
		/* Returns the computed result. */
		return EINVAL;
	}

	/* Checks the operation status. */
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

	/* Handles the hal info condition. */
	if ((hal_info.flags & HAL_CONS_INPUT_RELEASE) == 0)
		keyboard_info.flags |= INPUT_DEVICE_KEY_MOMENTARY;

	/* Handles the hal info condition. */
	if ((hal_info.flags & HAL_CONS_INPUT_REPEAT) != 0)
		keyboard_info.flags |= INPUT_DEVICE_KEY_REPEAT;

	/* Starts every queue, lock and keymap out empty. */
	spin_init(&input_lock, LOCK_RANK_DEVICE, "console input");
	waitq_init(&input_waitq, "console input");
	waitq_init(&dispatch_waitq, "console input dispatch");
	input_head = input_tail = input_used = 0;
	event_head = event_tail = event_used = event_sequence = 0;
	dispatch_head = dispatch_tail = dispatch_used = 0;
	event_owner = NULL;
	keyboard_input = NULL;
	drv_input_keymap_init(&early_keymap);
	early_resyncing = 0;
	memset(console_sources, 0, sizeof(console_sources));
	memset(&console_subscription, 0, sizeof(console_subscription));

	/* Checks the operation status. */
	error = tty_console_init();
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = kthread_create(console_input_worker, NULL,
			       SCHED_PRIORITY_DEFAULT, &producer);
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = kthread_create(console_dispatch_worker, NULL,
			       SCHED_PRIORITY_DEFAULT, &dispatcher);
	if (error != 0)
		goto fail;

	/* Checks the operation status. */
	error = cdev_register("console", 0x00010000U, &console_ops,
			      (void *)(uintptr_t)1U);
	if (error != 0)
		goto fail;
	/* Process each remaining element. */
	for (i_index_for = 0; i_index_for < tty_vt_count(); i_index_for++) {
		char name[] = "ttyv0";
		name[4] = (char)('0' + i_index_for);

		/* Checks the operation status. */
		error = cdev_register(name, (dev_t)(0x00010010U + i_index_for),
				      &vt_ops,
				      (void *)(uintptr_t)(i_index_for + 1U));
		if (error != 0)
			goto fail;
	}

	/* Checks the operation status. */
	error = tty_pty_register();
	if (error != 0)
		goto fail;

	/* Checks the operation status. */
	error = drv_input_device_register(&keyboard_info, &keyboard_input);
	if (error != 0)
		goto fail;

	/* Checks the operation status. */
	error = drv_input_subscribe(&console_subscription,
				    console_input_subscriber, NULL);
	if (error != 0)
		goto fail;
	input_started = 1;
	thread_start(dispatcher);
	thread_start(producer);
	hal_cons_set_mode(HAL_CONS_TERMINAL);

	/* Reports successful completion. */
	return 0;

fail:
	drv_input_unsubscribe(&console_subscription);

	/* Handles the keyboard input availability. */
	if (keyboard_input != NULL) {
		drv_input_device_unregister(keyboard_input);
		keyboard_input = NULL;
	}

	/* Handles the dispatcher availability. */
	if (dispatcher != NULL)
		(void)thread_abort_new(dispatcher);

	/* Handles the producer availability. */
	if (producer != NULL)
		(void)thread_abort_new(producer);
	input_started = 0;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}
#endif
