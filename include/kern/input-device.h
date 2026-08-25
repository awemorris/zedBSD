/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_INPUT_DEVICE_H
#define ZEDBSD_KERN_INPUT_DEVICE_H

#include <zedbsd/input.h>
#include <stdint.h>

struct input_device;

struct input_device_info {
	const char *name;
	const char *physical_path;
	const char *unique_id;
	struct input_id id;
	int (*open)(void *);
	void (*close)(void *);
	void *context;
};

void input_core_init(void);
int input_device_register(const struct input_device_info *,
			  struct input_device **);
void input_device_unregister(struct input_device *);
void input_device_emit(struct input_device *, uint16_t, uint16_t, int32_t);

#endif
