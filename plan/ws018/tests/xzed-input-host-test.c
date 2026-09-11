/* WS018 KA-T060: Xzed evdev consumer host fixture.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/X11/xzed/input.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zedbsd/input.h>

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define BITS_PER_WORD (sizeof(unsigned long) * 8U)
#define BIT_WORDS(maximum)                                                     \
	(((maximum) + 1U + BITS_PER_WORD - 1U) / BITS_PER_WORD)
#define MAX_FAKE_NODES 24U
#define MAX_READ_STEPS 32U
#define MAX_STEP_EVENTS 32U
#define MAX_KEY_OBSERVATIONS 256U
#define MAX_POINTER_OBSERVATIONS 64U

#define CHECK(condition) do {                                                  \
	if (!(condition)) {                                                      \
		fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,          \
		    __LINE__, #condition);                                         \
		exit(1);                                                           \
	}                                                                        \
} while (0)

struct fake_capabilities {
	unsigned long event[BIT_WORDS(EV_MAX)];
	unsigned long key[BIT_WORDS(KEY_MAX)];
	unsigned long relative[BIT_WORDS(REL_MAX)];
	unsigned long absolute[BIT_WORDS(ABS_MAX)];
};

enum read_step_kind {
	READ_STEP_DATA,
	READ_STEP_ERROR,
	READ_STEP_EOF,
};

struct fake_read_step {
	enum read_step_kind kind;
	int error;
	size_t size;
	unsigned char data[MAX_STEP_EVENTS * sizeof(struct input_event)];
};

struct fake_node {
	char path[PATH_MAX];
	int fd;
	struct fake_capabilities capabilities;
	unsigned long key_state[BIT_WORDS(KEY_MAX)];
	struct input_absinfo abs_x;
	struct input_absinfo abs_y;
	unsigned open_failures;
	unsigned open_calls;
	unsigned close_calls;
	unsigned key_state_queries;
	unsigned abs_queries;
	struct fake_read_step steps[MAX_READ_STEPS];
	size_t step_count;
	size_t next_step;
};

struct fake_io {
	struct fake_node nodes[MAX_FAKE_NODES];
	size_t node_count;
	int next_fd;
	unsigned pause_calls;
	unsigned unexpected_opens;
};

struct key_observation {
	uint8_t keycode;
	int value;
	uint32_t time;
	uint16_t modifiers;
};

struct observations {
	struct key_observation keys[MAX_KEY_OBSERVATIONS];
	size_t key_count;
	size_t modifier_notification_count;
	uint16_t last_modifier_notification;
	struct xzed_input_pointer_frame pointers[MAX_POINTER_OBSERVATIONS];
	size_t pointer_count;
};

struct fixture {
	struct fake_io fake;
	struct observations observations;
};

static void
set_bit(unsigned long *bits, unsigned code, int set)
{
	unsigned long mask = 1UL << (code % BITS_PER_WORD);
	if (set)
		bits[code / BITS_PER_WORD] |= mask;
	else
		bits[code / BITS_PER_WORD] &= ~mask;
}

static int
join_path(char *destination, size_t size, const char *left, const char *right)
{
	int length = snprintf(destination, size, "%s/%s", left, right);
	return length >= 0 && (size_t)length < size;
}

static void
make_case_directory(const char *base, const char *name, char *path,
	size_t size)
{
	CHECK(join_path(path, size, base, name));
	CHECK(mkdir(path, 0700) == 0);
}

static void
make_directory_entry(const char *directory, const char *name)
{
	char path[PATH_MAX];
	int fd;
	CHECK(join_path(path, sizeof(path), directory, name));
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	CHECK(fd >= 0);
	CHECK(close(fd) == 0);
}

static struct fixture *
fixture_create(void)
{
	struct fixture *fixture = calloc(1, sizeof(*fixture));
	CHECK(fixture != NULL);
	fixture->fake.next_fd = 100;
	return fixture;
}

static struct fake_node *
add_node(struct fixture *fixture, const char *directory, const char *name)
{
	struct fake_node *node;
	CHECK(fixture->fake.node_count < MAX_FAKE_NODES);
	node = &fixture->fake.nodes[fixture->fake.node_count++];
	memset(node, 0, sizeof(*node));
	CHECK(join_path(node->path, sizeof(node->path), directory, name));
	node->fd = fixture->fake.next_fd++;
	set_bit(node->capabilities.event, EV_SYN, 1);
	make_directory_entry(directory, name);
	return node;
}

static void
add_keyboard_capabilities(struct fake_node *node)
{
	static const unsigned codes[] = {
	    KEY_ESC, KEY_1, KEY_0, KEY_MINUS, KEY_EQUAL, KEY_BACKSPACE,
	    KEY_TAB, KEY_LEFTBRACE, KEY_RIGHTBRACE, KEY_ENTER, KEY_LEFTCTRL,
	    KEY_A, KEY_Z, KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE,
	    KEY_LEFTSHIFT, KEY_BACKSLASH, KEY_COMMA, KEY_DOT, KEY_SLASH,
	    KEY_RIGHTSHIFT, KEY_LEFTALT, KEY_SPACE, KEY_CAPSLOCK,
	    KEY_RIGHTCTRL, KEY_RIGHTALT, KEY_HOME, KEY_UP, KEY_PAGEUP,
	    KEY_LEFT, KEY_RIGHT, KEY_END, KEY_DOWN, KEY_PAGEDOWN, KEY_INSERT,
	    KEY_DELETE,
	};
	unsigned code;
	set_bit(node->capabilities.event, EV_KEY, 1);
	for (code = KEY_A; code <= KEY_Z; code++)
		set_bit(node->capabilities.key, code, 1);
	for (size_t index = 0; index < ARRAY_COUNT(codes); index++)
		set_bit(node->capabilities.key, codes[index], 1);
}

static void
add_relative_capabilities(struct fake_node *node)
{
	set_bit(node->capabilities.event, EV_KEY, 1);
	set_bit(node->capabilities.event, EV_REL, 1);
	set_bit(node->capabilities.key, BTN_LEFT, 1);
	set_bit(node->capabilities.key, BTN_RIGHT, 1);
	set_bit(node->capabilities.key, BTN_MIDDLE, 1);
	set_bit(node->capabilities.relative, REL_X, 1);
	set_bit(node->capabilities.relative, REL_Y, 1);
}

static void
add_absolute_capabilities(struct fake_node *node, int32_t minimum,
	int32_t maximum)
{
	set_bit(node->capabilities.event, EV_KEY, 1);
	set_bit(node->capabilities.event, EV_ABS, 1);
	set_bit(node->capabilities.key, BTN_LEFT, 1);
	set_bit(node->capabilities.key, BTN_RIGHT, 1);
	set_bit(node->capabilities.key, BTN_MIDDLE, 1);
	set_bit(node->capabilities.absolute, ABS_X, 1);
	set_bit(node->capabilities.absolute, ABS_Y, 1);
	node->abs_x.minimum = minimum;
	node->abs_x.maximum = maximum;
	node->abs_y.minimum = minimum;
	node->abs_y.maximum = maximum;
}

static struct fake_node *
node_by_fd(struct fake_io *fake, int fd)
{
	for (size_t index = 0; index < fake->node_count; index++)
		if (fake->nodes[index].fd == fd)
			return &fake->nodes[index];
	return NULL;
}

static int
fake_open(void *context, const char *path)
{
	struct fake_io *fake = context;
	for (size_t index = 0; index < fake->node_count; index++) {
		struct fake_node *node = &fake->nodes[index];
		if (strcmp(node->path, path) != 0)
			continue;
		node->open_calls++;
		if (node->open_failures != 0) {
			node->open_failures--;
			errno = ENODEV;
			return -1;
		}
		return node->fd;
	}
	fake->unexpected_opens++;
	errno = ENOENT;
	return -1;
}

static int
copy_bits(void *destination, size_t destination_size, const void *source,
	size_t source_size)
{
	size_t copied = destination_size < source_size ? destination_size :
	    source_size;
	memset(destination, 0, destination_size);
	memcpy(destination, source, copied);
	return 0;
}

static int
fake_get_bits(void *context, int fd, unsigned type, void *bits, size_t size)
{
	struct fake_node *node = node_by_fd(context, fd);
	CHECK(node != NULL);
	switch (type) {
	case 0:
		return copy_bits(bits, size, node->capabilities.event,
		    sizeof(node->capabilities.event));
	case EV_KEY:
		return copy_bits(bits, size, node->capabilities.key,
		    sizeof(node->capabilities.key));
	case EV_REL:
		return copy_bits(bits, size, node->capabilities.relative,
		    sizeof(node->capabilities.relative));
	case EV_ABS:
		return copy_bits(bits, size, node->capabilities.absolute,
		    sizeof(node->capabilities.absolute));
	default:
		errno = ENOTTY;
		return -1;
	}
}

static int
fake_get_key_state(void *context, int fd, void *bits, size_t size)
{
	struct fake_node *node = node_by_fd(context, fd);
	CHECK(node != NULL);
	node->key_state_queries++;
	return copy_bits(bits, size, node->key_state, sizeof(node->key_state));
}

static int
fake_get_abs(void *context, int fd, unsigned axis,
	struct input_absinfo *information)
{
	struct fake_node *node = node_by_fd(context, fd);
	CHECK(node != NULL);
	node->abs_queries++;
	if (axis == ABS_X) {
		*information = node->abs_x;
		return 0;
	}
	if (axis == ABS_Y) {
		*information = node->abs_y;
		return 0;
	}
	errno = ENOTTY;
	return -1;
}

static ssize_t
fake_read(void *context, int fd, void *buffer, size_t size)
{
	struct fake_node *node = node_by_fd(context, fd);
	struct fake_read_step *step;
	CHECK(node != NULL);
	if (node->next_step == node->step_count) {
		errno = EAGAIN;
		return -1;
	}
	step = &node->steps[node->next_step++];
	if (step->kind == READ_STEP_ERROR) {
		errno = step->error;
		return -1;
	}
	if (step->kind == READ_STEP_EOF)
		return 0;
	CHECK(step->size <= size);
	memcpy(buffer, step->data, step->size);
	return (ssize_t)step->size;
}

static int
fake_close(void *context, int fd)
{
	struct fake_node *node = node_by_fd(context, fd);
	CHECK(node != NULL);
	node->close_calls++;
	return 0;
}

static unsigned
fake_pause(void *context, unsigned seconds)
{
	struct fake_io *fake = context;
	CHECK(seconds == 1U);
	fake->pause_calls++;
	return 0;
}

static const struct xzed_input_io fake_operations = {
	fake_open,
	fake_get_bits,
	fake_get_key_state,
	fake_get_abs,
	fake_read,
	fake_close,
	fake_pause,
};

static void
observe_key(void *context, uint8_t keycode, int value, uint32_t time,
	uint16_t modifiers)
{
	struct observations *observations = context;
	struct key_observation *observation;
	if (keycode == 0) {
		observations->modifier_notification_count++;
		observations->last_modifier_notification = modifiers;
		return;
	}
	CHECK(observations->key_count < MAX_KEY_OBSERVATIONS);
	observation = &observations->keys[observations->key_count++];
	*observation = (struct key_observation){keycode, value, time, modifiers};
}

static void
observe_pointer(void *context, const struct xzed_input_pointer_frame *frame)
{
	struct observations *observations = context;
	CHECK(observations->pointer_count < MAX_POINTER_OBSERVATIONS);
	observations->pointers[observations->pointer_count++] = *frame;
}

static const struct xzed_input_handlers test_handlers = {
	observe_key,
	observe_pointer,
};

static void
reset_observations(struct observations *observations)
{
	memset(observations, 0, sizeof(*observations));
}

static struct input_event
input_event(uint16_t type, uint16_t code, int32_t value, uint32_t milliseconds)
{
	struct input_event event;
	memset(&event, 0, sizeof(event));
	event.time.tv_sec = (time_t)(milliseconds / 1000U);
	event.time.tv_usec = (suseconds_t)((milliseconds % 1000U) * 1000U);
	event.type = type;
	event.code = code;
	event.value = value;
	return event;
}

static struct input_event
sync_event(uint32_t milliseconds)
{
	return input_event(EV_SYN, SYN_REPORT, 0, milliseconds);
}

static void
queue_events(struct fake_node *node, const struct input_event *events,
	size_t count)
{
	struct fake_read_step *step;
	CHECK(count <= MAX_STEP_EVENTS);
	CHECK(node->step_count < MAX_READ_STEPS);
	step = &node->steps[node->step_count++];
	step->kind = READ_STEP_DATA;
	step->size = count * sizeof(events[0]);
	memcpy(step->data, events, step->size);
}

static void
queue_bytes(struct fake_node *node, const void *bytes, size_t size)
{
	struct fake_read_step *step;
	CHECK(size <= sizeof(step->data));
	CHECK(node->step_count < MAX_READ_STEPS);
	step = &node->steps[node->step_count++];
	step->kind = READ_STEP_DATA;
	step->size = size;
	memcpy(step->data, bytes, size);
}

static void
queue_error(struct fake_node *node, int error)
{
	struct fake_read_step *step;
	CHECK(node->step_count < MAX_READ_STEPS);
	step = &node->steps[node->step_count++];
	step->kind = READ_STEP_ERROR;
	step->error = error;
}

static void
queue_eof(struct fake_node *node)
{
	struct fake_read_step *step;
	CHECK(node->step_count < MAX_READ_STEPS);
	step = &node->steps[node->step_count++];
	step->kind = READ_STEP_EOF;
}

static int
dispatch_one(struct xzed_input *input, int fd, short events)
{
	struct pollfd descriptors[XZED_INPUT_MAX_DEVICES];
	size_t count = xzed_input_pollfds(input, descriptors,
	    ARRAY_COUNT(descriptors));
	int found = 0;
	CHECK(count == xzed_input_device_count(input));
	for (size_t index = 0; index < count; index++) {
		descriptors[index].revents = 0;
		if (descriptors[index].fd == fd) {
			descriptors[index].revents = events;
			found = 1;
		}
	}
	CHECK(found);
	return xzed_input_dispatch(input, descriptors, count);
}

static struct xzed_input *
open_input(struct fixture *fixture, const char *directory, unsigned width,
	unsigned height)
{
	struct xzed_input *input = NULL;
	CHECK(xzed_input_open_with_io(&input, directory, width, height,
	    &test_handlers, &fixture->observations, &fake_operations,
	    &fixture->fake) == 0);
	CHECK(input != NULL);
	return input;
}

static void
expect_key(const struct observations *observations, size_t index,
	uint8_t keycode, int value, uint32_t time, uint16_t modifiers)
{
	const struct key_observation *actual;
	CHECK(index < observations->key_count);
	actual = &observations->keys[index];
	CHECK(actual->keycode == keycode);
	CHECK(actual->value == value);
	CHECK(actual->time == time);
	CHECK(actual->modifiers == modifiers);
}

static void
test_discovery(const char *base)
{
	char directory[PATH_MAX];
	struct fixture *fixture = fixture_create();
	struct fake_node *keyboard, *relative, *absolute, *unclassified;
	struct fake_node *malformed, *vanished;
	struct xzed_input *input;

	make_case_directory(base, "discovery-a", directory, sizeof(directory));
	keyboard = add_node(fixture, directory, "event0");
	add_keyboard_capabilities(keyboard);
	keyboard->open_failures = 2;
	relative = add_node(fixture, directory, "event17");
	add_relative_capabilities(relative);
	absolute = add_node(fixture, directory, "event203");
	add_absolute_capabilities(absolute, -100, 900);
	unclassified = add_node(fixture, directory, "event999");
	malformed = add_node(fixture, directory, "event61");
	add_absolute_capabilities(malformed, 7, 7);
	vanished = add_node(fixture, directory, "event88");
	vanished->open_failures = 10;
	make_directory_entry(directory, "event");
	make_directory_entry(directory, "event1x");
	make_directory_entry(directory, "xevent1");
	make_directory_entry(directory, "event-1");

	input = open_input(fixture, directory, 800, 600);
	CHECK(xzed_input_device_count(input) == 3U);
	CHECK(keyboard->open_calls == 3U);
	CHECK(vanished->open_calls == 5U);
	CHECK(fixture->fake.pause_calls == 6U);
	CHECK(fixture->fake.unexpected_opens == 0U);
	CHECK(unclassified->close_calls == 1U);
	CHECK(malformed->close_calls == 1U);
	xzed_input_close(input);
	CHECK(keyboard->close_calls == 1U);
	CHECK(relative->close_calls == 1U);
	CHECK(absolute->close_calls == 1U);
	CHECK(vanished->close_calls == 0U);
	free(fixture);

	fixture = fixture_create();
	make_case_directory(base, "discovery-b", directory, sizeof(directory));
	absolute = add_node(fixture, directory, "event0");
	add_absolute_capabilities(absolute, 0, 1000);
	keyboard = add_node(fixture, directory, "event203");
	add_keyboard_capabilities(keyboard);
	relative = add_node(fixture, directory, "event17");
	add_relative_capabilities(relative);
	input = open_input(fixture, directory, 640, 480);
	CHECK(xzed_input_device_count(input) == 3U);
	xzed_input_close(input);
	CHECK(keyboard->close_calls == 1U && relative->close_calls == 1U &&
	    absolute->close_calls == 1U);
	free(fixture);

	fixture = fixture_create();
	make_case_directory(base, "discovery-missing", directory,
	    sizeof(directory));
	keyboard = add_node(fixture, directory, "event4");
	add_keyboard_capabilities(keyboard);
	input = (struct xzed_input *)(uintptr_t)1U;
	errno = 0;
	CHECK(xzed_input_open_with_io(&input, directory, 800, 600,
	    &test_handlers, &fixture->observations, &fake_operations,
	    &fixture->fake) == -1);
	CHECK(errno == ENODEV);
	CHECK(input == NULL);
	CHECK(keyboard->close_calls == 1U);
	free(fixture);
}

static void
test_keyboard(const char *base)
{
	static const uint16_t navigation_codes[] = {
	    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END,
	    KEY_PAGEUP, KEY_PAGEDOWN, KEY_INSERT, KEY_DELETE,
	};
	char directory[PATH_MAX];
	struct fixture *fixture = fixture_create();
	struct fake_node *keyboard1, *keyboard2, *pointer;
	struct xzed_input *input;
	struct input_event events[24];
	size_t count;
	const uint16_t all_modifiers = XZED_INPUT_SHIFT_MASK |
	    XZED_INPUT_CONTROL_MASK | XZED_INPUT_ALT_MASK;

	make_case_directory(base, "keyboard", directory, sizeof(directory));
	keyboard1 = add_node(fixture, directory, "event2");
	add_keyboard_capabilities(keyboard1);
	keyboard2 = add_node(fixture, directory, "event71");
	add_keyboard_capabilities(keyboard2);
	pointer = add_node(fixture, directory, "event9");
	add_relative_capabilities(pointer);
	input = open_input(fixture, directory, 800, 600);
	reset_observations(&fixture->observations);

	events[0] = input_event(EV_KEY, KEY_LEFTSHIFT, 1, 1000);
	events[1] = input_event(EV_KEY, KEY_A, 1, 1234);
	queue_events(keyboard1, events, 2);
	CHECK(dispatch_one(input, keyboard1->fd, POLLIN) == 0);
	CHECK(fixture->observations.key_count == 0U);
	CHECK(xzed_input_modifiers(input) == 0U);
	events[0] = sync_event(1250);
	queue_error(keyboard1, EINTR);
	queue_events(keyboard1, events, 1);
	CHECK(dispatch_one(input, keyboard1->fd, POLLIN) == 0);
	CHECK(fixture->observations.key_count == 1U);
	CHECK(fixture->observations.modifier_notification_count == 1U);
	CHECK(fixture->observations.last_modifier_notification ==
	    XZED_INPUT_SHIFT_MASK);
	expect_key(&fixture->observations, 0, (uint8_t)('A' + 8), 1, 1234,
	    XZED_INPUT_SHIFT_MASK);

	count = 0;
	events[count++] = input_event(EV_KEY, KEY_A, 2, 1300);
	events[count++] = input_event(EV_KEY, KEY_A, 0, 1301);
	events[count++] = input_event(EV_KEY, KEY_LEFTSHIFT, 0, 1302);
	events[count++] = input_event(EV_KEY, KEY_CAPSLOCK, 1, 1303);
	events[count++] = input_event(EV_KEY, KEY_CAPSLOCK, 0, 1304);
	events[count++] = input_event(EV_KEY, KEY_A, 1, 1305);
	events[count++] = sync_event(1310);
	queue_events(keyboard1, events, count);
	CHECK(dispatch_one(input, keyboard1->fd, POLLIN) == 0);
	CHECK(fixture->observations.key_count == 4U);
	expect_key(&fixture->observations, 1, (uint8_t)('A' + 8), 2, 1300,
	    XZED_INPUT_SHIFT_MASK);
	expect_key(&fixture->observations, 2, (uint8_t)('A' + 8), 0, 1301,
	    XZED_INPUT_SHIFT_MASK);
	expect_key(&fixture->observations, 3, (uint8_t)('A' + 8), 1, 1305, 0);

	count = 0;
	events[count++] = input_event(EV_KEY, KEY_A, 0, 1400);
	events[count++] = input_event(EV_KEY, KEY_RIGHTSHIFT, 1, 1401);
	events[count++] = input_event(EV_KEY, KEY_RIGHTCTRL, 1, 1402);
	events[count++] = input_event(EV_KEY, KEY_RIGHTALT, 1, 1403);
	events[count++] = input_event(EV_KEY, KEY_1, 1, 1404);
	events[count++] = input_event(EV_KEY, KEY_MINUS, 1, 1405);
	events[count++] = sync_event(1410);
	queue_events(keyboard1, events, count);
	CHECK(dispatch_one(input, keyboard1->fd, POLLIN) == 0);
	CHECK(fixture->observations.key_count == 7U);
	expect_key(&fixture->observations, 4, (uint8_t)('A' + 8), 0, 1400, 0);
	expect_key(&fixture->observations, 5, (uint8_t)('!' + 8), 1, 1404,
	    all_modifiers);
	expect_key(&fixture->observations, 6, (uint8_t)('_' + 8), 1, 1405,
	    all_modifiers);
	CHECK(xzed_input_modifiers(input) == all_modifiers);

	count = 0;
	events[count++] = input_event(EV_KEY, KEY_RIGHTSHIFT, 0, 1500);
	events[count++] = input_event(EV_KEY, KEY_RIGHTCTRL, 0, 1501);
	events[count++] = input_event(EV_KEY, KEY_RIGHTALT, 0, 1502);
	for (size_t index = 0; index < ARRAY_COUNT(navigation_codes); index++)
		events[count++] = input_event(EV_KEY, navigation_codes[index], 1,
		    1510U + (uint32_t)index);
	events[count++] = input_event(EV_KEY, KEY_F1, 1, 1530);
	events[count++] = input_event(EV_KEY, KEY_B, 99, 1531);
	events[count++] = sync_event(1540);
	queue_events(keyboard1, events, count);
	CHECK(dispatch_one(input, keyboard1->fd, POLLIN) == 0);
	CHECK(fixture->observations.key_count == 17U);
	for (size_t index = 0; index < ARRAY_COUNT(navigation_codes); index++)
		expect_key(&fixture->observations, 7U + index,
		    (uint8_t)(0xe0U + index), 1, 1510U + (uint32_t)index, 0);
	CHECK(xzed_input_modifiers(input) == 0U);

	count = 0;
	events[count++] = input_event(EV_KEY, KEY_CAPSLOCK, 2, 1600);
	events[count++] = input_event(EV_KEY, KEY_A, 1, 1601);
	events[count++] = input_event(EV_KEY, KEY_A, 0, 1602);
	events[count++] = input_event(EV_KEY, KEY_CAPSLOCK, 1, 1603);
	events[count++] = input_event(EV_KEY, KEY_CAPSLOCK, 0, 1604);
	events[count++] = input_event(EV_KEY, KEY_A, 1, 1605);
	events[count++] = sync_event(1610);
	queue_events(keyboard1, events, count);
	CHECK(dispatch_one(input, keyboard1->fd, POLLIN) == 0);
	CHECK(fixture->observations.key_count == 20U);
	expect_key(&fixture->observations, 17, (uint8_t)('A' + 8), 1, 1601, 0);
	expect_key(&fixture->observations, 18, (uint8_t)('A' + 8), 0, 1602, 0);
	expect_key(&fixture->observations, 19, (uint8_t)('a' + 8), 1, 1605, 0);

	events[0] = input_event(EV_KEY, KEY_LEFTSHIFT, 1, 1700);
	events[1] = sync_event(1701);
	queue_events(keyboard1, events, 2);
	CHECK(dispatch_one(input, keyboard1->fd, POLLIN) == 0);
	events[0] = input_event(EV_KEY, KEY_RIGHTSHIFT, 1, 1710);
	events[1] = sync_event(1711);
	queue_events(keyboard2, events, 2);
	CHECK(dispatch_one(input, keyboard2->fd, POLLIN) == 0);
	events[0] = input_event(EV_KEY, KEY_LEFTSHIFT, 0, 1720);
	events[1] = sync_event(1721);
	queue_events(keyboard1, events, 2);
	CHECK(dispatch_one(input, keyboard1->fd, POLLIN) == 0);
	CHECK(xzed_input_modifiers(input) == XZED_INPUT_SHIFT_MASK);
	events[0] = input_event(EV_KEY, KEY_RIGHTSHIFT, 0, 1730);
	events[1] = sync_event(1731);
	queue_events(keyboard2, events, 2);
	CHECK(dispatch_one(input, keyboard2->fd, POLLIN) == 0);
	CHECK(xzed_input_modifiers(input) == 0U);

	/* EAGAIN may divide one input_event between two drains.  Preserve the
	 * byte carry and publish nothing until the record and frame are complete. */
	reset_observations(&fixture->observations);
	events[0] = input_event(EV_KEY, KEY_C, 1, 1800);
	events[1] = sync_event(1801);
	queue_bytes(keyboard1, &events[0], 7U);
	CHECK(dispatch_one(input, keyboard1->fd, POLLIN) == 0);
	CHECK(fixture->observations.key_count == 0U);
	queue_bytes(keyboard1, (const unsigned char *)&events[0] + 7U,
	    sizeof(events[0]) - 7U);
	queue_events(keyboard1, &events[1], 1);
	CHECK(dispatch_one(input, keyboard1->fd, POLLIN) == 0);
	CHECK(fixture->observations.key_count == 1U);
	expect_key(&fixture->observations, 0, (uint8_t)('c' + 8), 1, 1800, 0);

	xzed_input_close(input);
	free(fixture);
}

static void
test_relative_pointer(const char *base)
{
	char directory[PATH_MAX];
	struct fixture *fixture = fixture_create();
	struct fake_node *keyboard, *pointer1, *pointer2;
	struct xzed_input *input;
	struct input_event events[8];
	struct xzed_input_pointer_frame *frame;

	make_case_directory(base, "relative", directory, sizeof(directory));
	keyboard = add_node(fixture, directory, "event1");
	add_keyboard_capabilities(keyboard);
	pointer1 = add_node(fixture, directory, "event8");
	add_relative_capabilities(pointer1);
	pointer2 = add_node(fixture, directory, "event44");
	add_relative_capabilities(pointer2);
	input = open_input(fixture, directory, 800, 600);
	reset_observations(&fixture->observations);

	events[0] = input_event(EV_REL, REL_X, 10, 2000);
	events[1] = input_event(EV_REL, REL_Y, -5, 2001);
	events[2] = input_event(EV_KEY, BTN_RIGHT, 1, 2002);
	events[3] = input_event(EV_KEY, BTN_LEFT, 1, 2003);
	events[4] = sync_event(2004);
	queue_events(pointer1, events, 5);
	CHECK(dispatch_one(input, pointer1->fd, POLLIN) == 0);
	CHECK(fixture->observations.pointer_count == 1U);
	frame = &fixture->observations.pointers[0];
	CHECK(frame->relative_x == 10 && frame->relative_y == -5);
	CHECK(frame->buttons_before == 0U && frame->buttons_after == 5U);
	CHECK(frame->edge_count == 2U);
	CHECK(frame->edges[0].button == 3U && frame->edges[0].pressed == 1U &&
	    frame->edges[0].buttons == 4U);
	CHECK(frame->edges[1].button == 1U && frame->edges[1].pressed == 1U &&
	    frame->edges[1].buttons == 5U);

	events[0] = input_event(EV_KEY, BTN_LEFT, 1, 2100);
	events[1] = sync_event(2101);
	queue_events(pointer2, events, 2);
	CHECK(dispatch_one(input, pointer2->fd, POLLIN) == 0);
	CHECK(fixture->observations.pointer_count == 1U);
	events[0] = input_event(EV_KEY, BTN_LEFT, 0, 2110);
	events[1] = sync_event(2111);
	queue_events(pointer1, events, 2);
	CHECK(dispatch_one(input, pointer1->fd, POLLIN) == 0);
	CHECK(fixture->observations.pointer_count == 1U);
	CHECK(xzed_input_buttons(input) == 5U);
	events[0] = input_event(EV_KEY, BTN_LEFT, 0, 2120);
	events[1] = sync_event(2121);
	queue_events(pointer2, events, 2);
	CHECK(dispatch_one(input, pointer2->fd, POLLIN) == 0);
	CHECK(fixture->observations.pointer_count == 2U);
	frame = &fixture->observations.pointers[1];
	CHECK(frame->edge_count == 1U && frame->edges[0].button == 1U &&
	    frame->edges[0].pressed == 0U && frame->edges[0].buttons == 4U);
	events[0] = input_event(EV_KEY, BTN_RIGHT, 0, 2130);
	events[1] = sync_event(2131);
	queue_events(pointer1, events, 2);
	CHECK(dispatch_one(input, pointer1->fd, POLLIN) == 0);
	CHECK(xzed_input_buttons(input) == 0U);

	events[0] = input_event(EV_REL, REL_X, INT32_MAX, 2200);
	events[1] = input_event(EV_REL, REL_X, INT32_MAX, 2201);
	events[2] = input_event(EV_REL, REL_X, INT32_MIN, 2202);
	events[3] = input_event(EV_REL, REL_Y, INT32_MIN, 2203);
	events[4] = sync_event(2204);
	queue_events(pointer1, events, 5);
	CHECK(dispatch_one(input, pointer1->fd, POLLIN) == 0);
	frame = &fixture->observations
	    .pointers[fixture->observations.pointer_count - 1U];
	CHECK(frame->relative_x == INT32_MAX - 1);
	CHECK(frame->relative_y == INT32_MIN);

	xzed_input_close(input);
	free(fixture);
}

static void
test_absolute_pointer(const char *base)
{
	char directory[PATH_MAX];
	struct fixture *fixture = fixture_create();
	struct fake_node *keyboard, *pointer;
	struct xzed_input *input;
	struct input_event events[4];
	struct xzed_input_pointer_frame *frame;

	make_case_directory(base, "absolute", directory, sizeof(directory));
	keyboard = add_node(fixture, directory, "event5");
	add_keyboard_capabilities(keyboard);
	pointer = add_node(fixture, directory, "event101");
	add_absolute_capabilities(pointer, INT32_MIN, INT32_MAX);
	input = open_input(fixture, directory, 800, 600);
	reset_observations(&fixture->observations);

	events[0] = input_event(EV_ABS, ABS_X, INT32_MIN, 3000);
	events[1] = input_event(EV_ABS, ABS_Y, INT32_MAX, 3001);
	events[2] = sync_event(3002);
	queue_events(pointer, events, 3);
	CHECK(dispatch_one(input, pointer->fd, POLLIN) == 0);
	frame = &fixture->observations.pointers[0];
	CHECK(frame->absolute && frame->absolute_x == 0 &&
	    frame->absolute_y == 599);

	events[0] = input_event(EV_ABS, ABS_X, 0, 3010);
	events[1] = sync_event(3011);
	queue_events(pointer, events, 2);
	CHECK(dispatch_one(input, pointer->fd, POLLIN) == 0);
	frame = &fixture->observations.pointers[1];
	CHECK(frame->absolute_x == 399 && frame->absolute_y == 599);

	events[0] = input_event(EV_ABS, ABS_X, INT32_MAX, 3020);
	events[1] = input_event(EV_ABS, ABS_Y, INT32_MIN, 3021);
	events[2] = sync_event(3022);
	queue_events(pointer, events, 3);
	CHECK(dispatch_one(input, pointer->fd, POLLIN) == 0);
	frame = &fixture->observations.pointers[2];
	CHECK(frame->absolute_x == 799 && frame->absolute_y == 0);

	xzed_input_close(input);
	free(fixture);

	fixture = fixture_create();
	make_case_directory(base, "absolute-extreme-size", directory,
	    sizeof(directory));
	keyboard = add_node(fixture, directory, "event500");
	add_keyboard_capabilities(keyboard);
	pointer = add_node(fixture, directory, "event3");
	add_absolute_capabilities(pointer, INT32_MIN, INT32_MAX);
	input = open_input(fixture, directory, INT_MAX, INT_MAX);
	reset_observations(&fixture->observations);
	events[0] = input_event(EV_ABS, ABS_X, 0, 3030);
	events[1] = input_event(EV_ABS, ABS_Y, 0, 3031);
	events[2] = sync_event(3032);
	queue_events(pointer, events, 3);
	CHECK(dispatch_one(input, pointer->fd, POLLIN) == 0);
	frame = &fixture->observations.pointers[0];
	CHECK(frame->absolute_x == 1073741823 &&
	    frame->absolute_y == 1073741823);
	xzed_input_close(input);
	free(fixture);

	fixture = fixture_create();
	make_case_directory(base, "absolute-invalid-size", directory,
	    sizeof(directory));
	input = (struct xzed_input *)(uintptr_t)1U;
	errno = 0;
	CHECK(xzed_input_open_with_io(&input, directory,
	    (unsigned)INT_MAX + 1U, 600, &test_handlers,
	    &fixture->observations, &fake_operations, &fixture->fake) == -1);
	CHECK(errno == EINVAL && input == NULL);
	input = (struct xzed_input *)(uintptr_t)1U;
	errno = 0;
	CHECK(xzed_input_open_with_io(&input, directory, 800,
	    (unsigned)INT_MAX + 1U, &test_handlers, &fixture->observations,
	    &fake_operations, &fixture->fake) == -1);
	CHECK(errno == EINVAL && input == NULL);
	free(fixture);
}

static void
test_dropped_resynchronization(const char *base)
{
	char directory[PATH_MAX];
	struct fixture *fixture = fixture_create();
	struct fake_node *keyboard, *pointer;
	struct xzed_input *input;
	struct input_event events[8];
	struct xzed_input_pointer_frame *frame;
	unsigned key_queries, abs_queries;

	make_case_directory(base, "dropped", directory, sizeof(directory));
	keyboard = add_node(fixture, directory, "event3");
	add_keyboard_capabilities(keyboard);
	pointer = add_node(fixture, directory, "event12");
	add_absolute_capabilities(pointer, -100, 900);
	input = open_input(fixture, directory, 800, 600);
	reset_observations(&fixture->observations);

	events[0] = input_event(EV_KEY, KEY_CAPSLOCK, 1, 4000);
	events[1] = input_event(EV_KEY, KEY_CAPSLOCK, 0, 4001);
	events[2] = input_event(EV_KEY, KEY_A, 1, 4002);
	events[3] = sync_event(4003);
	queue_events(keyboard, events, 4);
	CHECK(dispatch_one(input, keyboard->fd, POLLIN) == 0);
	expect_key(&fixture->observations, 0, (uint8_t)('A' + 8), 1, 4002, 0);

	events[0] = input_event(EV_KEY, BTN_LEFT, 1, 4010);
	events[1] = sync_event(4011);
	queue_events(pointer, events, 2);
	CHECK(dispatch_one(input, pointer->fd, POLLIN) == 0);
	CHECK(xzed_input_buttons(input) == 1U);
	reset_observations(&fixture->observations);

	memset(keyboard->key_state, 0, sizeof(keyboard->key_state));
	set_bit(keyboard->key_state, KEY_B, 1);
	set_bit(keyboard->key_state, KEY_RIGHTCTRL, 1);
	key_queries = keyboard->key_state_queries;
	events[0] = input_event(EV_KEY, KEY_C, 1, 4100);
	events[1] = input_event(EV_SYN, SYN_DROPPED, 0, 4101);
	events[2] = input_event(EV_KEY, KEY_D, 1, 4102);
	queue_events(keyboard, events, 3);
	CHECK(dispatch_one(input, keyboard->fd, POLLIN) == 0);
	CHECK(keyboard->key_state_queries == key_queries);
	CHECK(fixture->observations.key_count == 0U);
	events[0] = sync_event(4110);
	queue_events(keyboard, events, 1);
	CHECK(dispatch_one(input, keyboard->fd, POLLIN) == 0);
	CHECK(keyboard->key_state_queries == key_queries + 1U);
	CHECK(fixture->observations.key_count == 2U);
	expect_key(&fixture->observations, 0, (uint8_t)('A' + 8), 0, 4110,
	    XZED_INPUT_CONTROL_MASK);
	expect_key(&fixture->observations, 1, (uint8_t)('B' + 8), 1, 4110,
	    XZED_INPUT_CONTROL_MASK);

	memset(pointer->key_state, 0, sizeof(pointer->key_state));
	set_bit(pointer->key_state, BTN_RIGHT, 1);
	pointer->abs_x.value = 900;
	pointer->abs_y.value = -100;
	abs_queries = pointer->abs_queries;
	events[0] = input_event(EV_ABS, ABS_X, 123, 4200);
	events[1] = input_event(EV_SYN, SYN_DROPPED, 0, 4201);
	events[2] = input_event(EV_KEY, BTN_MIDDLE, 1, 4202);
	queue_events(pointer, events, 3);
	CHECK(dispatch_one(input, pointer->fd, POLLIN) == 0);
	CHECK(pointer->abs_queries == abs_queries);
	CHECK(fixture->observations.pointer_count == 0U);
	events[0] = sync_event(4210);
	queue_events(pointer, events, 1);
	CHECK(dispatch_one(input, pointer->fd, POLLIN) == 0);
	CHECK(pointer->abs_queries == abs_queries + 2U);
	CHECK(fixture->observations.pointer_count == 1U);
	frame = &fixture->observations.pointers[0];
	CHECK(frame->absolute && frame->absolute_x == 799 &&
	    frame->absolute_y == 0);
	CHECK(frame->buttons_before == 1U && frame->buttons_after == 4U);
	CHECK(frame->edge_count == 2U);
	CHECK(frame->edges[0].button == 1U && !frame->edges[0].pressed &&
	    frame->edges[0].buttons == 0U);
	CHECK(frame->edges[1].button == 3U && frame->edges[1].pressed &&
	    frame->edges[1].buttons == 4U);

	reset_observations(&fixture->observations);
	events[0] = input_event(EV_KEY, KEY_A, 1, 4300);
	events[1] = sync_event(4301);
	queue_events(keyboard, events, 2);
	CHECK(dispatch_one(input, keyboard->fd, POLLIN) == 0);
	CHECK(fixture->observations.key_count == 1U);
	expect_key(&fixture->observations, 0, (uint8_t)('A' + 8), 1, 4300,
	    XZED_INPUT_CONTROL_MASK);

	xzed_input_close(input);
	free(fixture);
}

static void
test_hup_drain(const char *base)
{
	char directory[PATH_MAX];
	struct fixture *fixture = fixture_create();
	struct fake_node *keyboard1, *keyboard2, *keyboard3, *pointer1, *pointer2;
	struct xzed_input *input;
	struct input_event events[3];

	make_case_directory(base, "hup", directory, sizeof(directory));
	keyboard1 = add_node(fixture, directory, "event6");
	add_keyboard_capabilities(keyboard1);
	keyboard2 = add_node(fixture, directory, "event7");
	add_keyboard_capabilities(keyboard2);
	keyboard3 = add_node(fixture, directory, "event10");
	add_keyboard_capabilities(keyboard3);
	pointer1 = add_node(fixture, directory, "event8");
	add_relative_capabilities(pointer1);
	pointer2 = add_node(fixture, directory, "event9");
	add_relative_capabilities(pointer2);
	input = open_input(fixture, directory, 800, 600);
	reset_observations(&fixture->observations);

	events[0] = input_event(EV_KEY, KEY_A, 1, 5000);
	events[1] = sync_event(5001);
	queue_events(keyboard1, events, 2);
	queue_eof(keyboard1);
	CHECK(dispatch_one(input, keyboard1->fd, POLLIN | POLLHUP) == 0);
	CHECK(fixture->observations.key_count == 2U);
	expect_key(&fixture->observations, 0, (uint8_t)('a' + 8), 1, 5000, 0);
	expect_key(&fixture->observations, 1, (uint8_t)('a' + 8), 0, 0, 0);
	CHECK(keyboard1->close_calls == 1U);

	reset_observations(&fixture->observations);
	events[0] = input_event(EV_KEY, BTN_LEFT, 1, 5010);
	events[1] = sync_event(5011);
	queue_events(pointer1, events, 2);
	queue_eof(pointer1);
	CHECK(dispatch_one(input, pointer1->fd, POLLIN | POLLHUP) == 0);
	CHECK(fixture->observations.pointer_count == 2U);
	CHECK(fixture->observations.pointers[0].edge_count == 1U &&
	    fixture->observations.pointers[0].edges[0].pressed);
	CHECK(fixture->observations.pointers[1].edge_count == 1U &&
	    !fixture->observations.pointers[1].edges[0].pressed);
	CHECK(pointer1->close_calls == 1U);
	CHECK(xzed_input_device_count(input) == 3U);

	/* EOF with an incomplete record removes only the broken descriptor. */
	reset_observations(&fixture->observations);
	events[0] = input_event(EV_KEY, KEY_B, 1, 5015);
	queue_bytes(keyboard3, &events[0], sizeof(events[0]) - 1U);
	queue_eof(keyboard3);
	CHECK(dispatch_one(input, keyboard3->fd, POLLIN | POLLHUP) == 0);
	CHECK(fixture->observations.key_count == 0U);
	CHECK(keyboard3->close_calls == 1U);
	CHECK(xzed_input_device_count(input) == 2U);

	reset_observations(&fixture->observations);
	events[0] = input_event(EV_REL, REL_X, 9, 5020);
	events[1] = sync_event(5021);
	queue_events(pointer2, events, 2);
	CHECK(dispatch_one(input, pointer2->fd, POLLIN) == 0);
	CHECK(fixture->observations.pointer_count == 1U);
	CHECK(fixture->observations.pointers[0].relative_x == 9);

	xzed_input_close(input);
	CHECK(keyboard2->close_calls == 1U && pointer2->close_calls == 1U);
	free(fixture);
}

int
main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s TEMPORARY-DIRECTORY\n", argv[0]);
		return 2;
	}
	CHECK(mkdir(argv[1], 0700) == 0);
	test_discovery(argv[1]);
	test_keyboard(argv[1]);
	test_relative_pointer(argv[1]);
	test_absolute_pointer(argv[1]);
	test_dropped_resynchronization(argv[1]);
	test_hup_drain(argv[1]);
	puts("KA-T060 Xzed evdev consumer host fixture: PASS");
	return 0;
}
