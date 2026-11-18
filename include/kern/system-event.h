/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The system's events (ws132-p002): what drivers post, and what each open
 * of /dev/system that subscribed reads (system-event.c).
 */

#ifndef KERN_SYSTEM_EVENT_H
#define KERN_SYSTEM_EVENT_H

#include <uapi/system.h>

#include <stddef.h>
#include <stdint.h>
#include <uapi/types.h>

struct kern_system_subscriber;

void kern_system_event_post(uint32_t class_bit, uint32_t action, int32_t value, const char *subject, const char *detail);
int kern_system_event_open(struct kern_system_subscriber **result);
void kern_system_event_close(struct kern_system_subscriber *subscriber);
int kern_system_event_subscribe(struct kern_system_subscriber *subscriber, uint32_t classes);
ssize_t kern_system_event_read(struct kern_system_subscriber *subscriber, struct system_event *events, size_t capacity, int nonblock);
int kern_system_event_readable(struct kern_system_subscriber *subscriber);

#endif
