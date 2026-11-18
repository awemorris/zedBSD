/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Provides the selected Wayland public utility and protocol-description ABI.
 */

#ifndef KERN_WAYLAND_UTIL_H
#define KERN_WAYLAND_UTIL_H

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#define WL_EXPORT __attribute__((visibility("default")))

#ifdef __cplusplus
extern "C" {
#endif

struct wl_object;
struct wl_interface;

/* Represents a signed 24.8 protocol coordinate. */
typedef int32_t wl_fixed_t;

/*
 * Receives one message the library logs, as a printf format and its
 * arguments; the client sets it with wl_log_set_handler_client().
 */
typedef void (*wl_log_func_t)(const char *format, va_list arguments)
	__attribute__((__format__(__printf__, 1, 0)));

/* Holds a caller-owned variable-length protocol byte array. */
struct wl_array {
	size_t size;
	size_t alloc;
	void *data;
};

/* Links caller-owned objects without allocating additional storage. */
struct wl_list {
	struct wl_list *prev;
	struct wl_list *next;
};

/* Describes one immutable request or event signature. */
struct wl_message {
	const char *name;
	const char *signature;
	const struct wl_interface **types;
};

/* Describes one immutable protocol class and its supported version. */
struct wl_interface {
	const char *name;
	int version;
	int method_count;
	const struct wl_message *methods;
	int event_count;
	const struct wl_message *events;
};

/* Carries one decoded or about-to-be-marshalled protocol argument. */
union wl_argument {
	int32_t i;
	uint32_t u;
	wl_fixed_t f;
	const char *s;
	struct wl_object *o;
	uint32_t n;
	struct wl_array *a;
	int32_t h;
};

/* Dispatches a decoded event for a language binding or custom protocol. */
typedef int (*wl_dispatcher_func_t)(const void *, void *, uint32_t, const struct wl_message *, union wl_argument *);

void wl_array_init(struct wl_array *array);
void wl_array_release(struct wl_array *array);
void *wl_array_add(struct wl_array *array, size_t size);
int wl_array_copy(struct wl_array *array, struct wl_array *source);
void wl_list_init(struct wl_list *list);
void wl_list_insert(struct wl_list *list, struct wl_list *element);
void wl_list_remove(struct wl_list *element);
int wl_list_length(const struct wl_list *list);
int wl_list_empty(const struct wl_list *list);
void wl_list_insert_list(struct wl_list *list, struct wl_list *other);
int wl_fixed_to_int(wl_fixed_t fixed);
wl_fixed_t wl_fixed_from_int(int integer);
double wl_fixed_to_double(wl_fixed_t fixed);
wl_fixed_t wl_fixed_from_double(double real);

/*
 * The iteration helpers of the standard header.  Upstream programs use them,
 * and its header includes <math.h> and <inttypes.h> for them as well.
 *
 * wl_container_of gives the structure an embedded member belongs to; sample
 * is any pointer of the structure's type, used only for that type.
 */
#define wl_container_of(ptr, sample, member) \
	((__typeof__(sample))((char *)(ptr) - offsetof(__typeof__(*(sample)), member)))

/* Visits every element of a list, from the first. */
#define wl_list_for_each(pos, head, member) \
	for ((pos) = wl_container_of((head)->next, pos, member); \
	     &(pos)->member != (head); \
	     (pos) = wl_container_of((pos)->member.next, pos, member))

/* Visits every element of a list, from the first, allowing pos to be removed. */
#define wl_list_for_each_safe(pos, tmp, head, member) \
	for ((pos) = wl_container_of((head)->next, pos, member), \
	     (tmp) = wl_container_of((pos)->member.next, tmp, member); \
	     &(pos)->member != (head); \
	     (pos) = (tmp), \
	     (tmp) = wl_container_of((pos)->member.next, tmp, member))

/* Visits every element of a list, from the last. */
#define wl_list_for_each_reverse(pos, head, member) \
	for ((pos) = wl_container_of((head)->prev, pos, member); \
	     &(pos)->member != (head); \
	     (pos) = wl_container_of((pos)->member.prev, pos, member))

/* Visits every element of a list, from the last, allowing pos to be removed. */
#define wl_list_for_each_reverse_safe(pos, tmp, head, member) \
	for ((pos) = wl_container_of((head)->prev, pos, member), \
	     (tmp) = wl_container_of((pos)->member.prev, tmp, member); \
	     &(pos)->member != (head); \
	     (pos) = (tmp), \
	     (tmp) = wl_container_of((pos)->member.prev, tmp, member))

/* Visits every element of an array of pos's type. */
#define wl_array_for_each(pos, array) \
	for ((pos) = (array)->data; \
	     (array)->size != 0 && \
	     (const char *)(pos) < (const char *)(array)->data + (array)->size; \
	     (pos)++)

#ifdef __cplusplus
}
#endif

#endif
