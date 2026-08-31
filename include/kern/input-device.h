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

struct input_report_event {
	struct input_event event;
	char symbol[HAL_KEY_SYMBOL_SIZE];
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

void input_core_init(void);
int input_device_register(const struct input_device_info *,
			  struct input_device **);
void input_device_unregister(struct input_device *);
void input_device_emit(struct input_device *, uint16_t, uint16_t, int32_t);
void input_device_emit_key_event(struct input_device *,
	const struct hal_key_event *);
int input_subscribe(struct input_subscription *, input_subscriber_callback_t,
	void *);
void input_unsubscribe(struct input_subscription *);
void input_subscriber_init(void);
void input_subscriber_publish(const struct input_report *);

#endif
