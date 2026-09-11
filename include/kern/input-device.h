/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_INPUT_DEVICE_H
#define ZEDBSD_KERN_INPUT_DEVICE_H

#include "kern/input-capability.h"

#include <zedbsd/input.h>
#include <hal/hal.h>
#include <stddef.h>
#include <stdint.h>

struct input_device;

#define INPUT_DEVICE_KEY_MOMENTARY 0x00000001U
#define INPUT_DEVICE_KEY_REPEAT    0x00000002U
#define INPUT_REPORT_EVENT_MAX     8U

#define INPUT_REPORT_DETACH        0x00000001U
#define INPUT_REPORT_RESYNC_BEGIN  0x00000002U
#define INPUT_REPORT_SNAPSHOT      0x00000004U
#define INPUT_REPORT_RESYNC_END    0x00000008U
#define INPUT_REPORT_LOCK_CAPS     0x00000010U
#define INPUT_REPORT_LOCK_KANA     0x00000020U

/*
 * Keyboard event encoding owned by the kernel input layer. The HAL
 * console passes a keysymbol and these flags as separate parameters;
 * src/hal/cons-keys.h carries the matching values for that side until
 * console input moves out of the HAL.
 */
#define KERN_KEY_SYMBOL_SIZE	16U
#define KERN_KEY_EVENT_PRESS	0x00000001U
#define KERN_KEY_EVENT_RELEASE	0x00000002U
#define KERN_KEY_EVENT_REPEAT	0x00000004U
#define KERN_KEY_EVENT_RESYNC	0x00000008U
#define KERN_KEY_EVENT_SNAPSHOT	0x00000010U
#define KERN_KEY_EVENT_RESYNC_END	0x00000020U
#define KERN_KEY_EVENT_LOCK_CAPS	0x00000040U
#define KERN_KEY_EVENT_LOCK_KANA	0x00000080U

struct input_report_event {
	struct input_event event;
	char symbol[KERN_KEY_SYMBOL_SIZE];
	uint32_t key_flags;
};

struct input_report {
	struct input_device *device;
	unsigned device_id;
	unsigned flags;
	size_t event_count;
	struct input_report_event events[INPUT_REPORT_EVENT_MAX];
};

typedef void (*input_subscriber_callback_t)(void *,
	const struct input_report *);

struct input_subscription {
	input_subscriber_callback_t callback;
	void *context;
	unsigned registered;
};

struct input_device_info {
	const char *name;
	const char *physical_path;
	const char *unique_id;
	struct input_id id;
	const struct input_capability *capabilities;
	size_t capability_count;
	const struct input_abs_axis *absolute_axes;
	size_t absolute_axis_count;
	unsigned flags;
	int (*open)(void *);
	void (*close)(void *);
	void *context;
};

/*
 * One keyboard event as the generic input layer carries it. The HAL
 * passes the keysymbol and the flags as separate parameters, so this
 * record belongs to the kernel side of the boundary.
 */
struct kern_key_event {
	char symbol[KERN_KEY_SYMBOL_SIZE];
	uint32_t flags;
};

void drv_input_core_init(void);
int drv_input_device_register(const struct input_device_info *,
			  struct input_device **);
void drv_input_device_unregister(struct input_device *);
void drv_input_device_emit(struct input_device *, uint16_t, uint16_t, int32_t);
void drv_input_device_emit_key_event(struct input_device *,
	const struct kern_key_event *);
int drv_input_subscribe(struct input_subscription *, input_subscriber_callback_t,
	void *);
void drv_input_unsubscribe(struct input_subscription *);
void drv_input_subscriber_init(void);
void drv_input_subscriber_publish(const struct input_report *);

#endif
