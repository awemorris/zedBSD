/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Owns the bounded bus connections of the Linux backend: the system bus of
 * the seat and the power (libkeiland-backend, ws131-p006), the session
 * bus of the notifications (ws156-p006), which also answers method calls,
 * and the system bus of Bluetooth (ws143-p007), whose calls are not waited
 * for and whose agent answers BlueZ later.
 */
#ifndef KL_BACKEND_DBUS_LINUX_H
#define KL_BACKEND_DBUS_LINUX_H
#include <stddef.h>
#include <stdint.h>

#define LINUX_DBUS_MAX 65536U
#define LINUX_DBUS_FDS 16U
#define LINUX_DBUS_SIGNALS 32U

/* The kinds of message (the header's second byte). */
#define LINUX_DBUS_METHOD_CALL 1U
#define LINUX_DBUS_METHOD_RETURN 2U
#define LINUX_DBUS_ERROR 3U
#define LINUX_DBUS_SIGNAL 4U

/* A method call whose caller wants no answer (the header's flags). */
#define LINUX_DBUS_NO_REPLY_EXPECTED 0x1U

/* A method call that must not start the service it names when no one owns that name (the header's flags). */
#define LINUX_DBUS_NO_AUTO_START 0x2U

/* How deep a value's containers may nest when one is read or stepped over (the specification allows 64 in all). */
#define LINUX_DBUS_DEPTH 32U

/* One received frame owns its ancillary files until a consumer takes them. */
struct dbus_reply {
	unsigned char bytes[LINUX_DBUS_MAX];
	size_t size;
	size_t body;
	size_t cursor;
	unsigned type;
	unsigned flags;
	uint32_t serial;
	uint32_t reply_serial;
	const char *path;
	const char *interface;
	const char *member;
	const char *signature;
	const char *sender;
	const char *error;
	int fds[LINUX_DBUS_FDS];
	size_t fd_count;
};

/* A connection retains a partial frame and signals interleaved with replies. */
struct linux_dbus {
	int fd;
	uint32_t serial;
	struct dbus_reply *receiving;
	size_t received;
	size_t wanted;
	struct dbus_reply *signals[LINUX_DBUS_SIGNALS];
	size_t signal_count;
};

/*
 * One simple argument, marshaled according to the supplied signature: a
 * string, object path or signature (string), an unsigned or Boolean
 * (number), an array of strings, "as" in the signature (strings, of
 * string_count), or a variant, "v" in the signature, holding one value of
 * the simple type named by variant ("s", "o", "g", "u" or "b"), taken from
 * string or number as above.
 */
struct dbus_arg {
	const char *string;
	uint32_t number;
	const char *const *strings;
	size_t string_count;
	const char *variant;
};

typedef int (*dbus_signal_fn)(struct dbus_reply *signal, void *data);
typedef int (*dbus_message_fn)(struct dbus_reply *message, void *data);

int dbus_open_system(struct linux_dbus *bus);
int dbus_open_session(struct linux_dbus *bus);
int dbus_open_address(struct linux_dbus *bus, const char *path);
void dbus_close(struct linux_dbus *bus);
int dbus_call(struct linux_dbus *bus, const char *destination, const char *path, const char *interface, const char *member, const char *signature, const struct dbus_arg *args, size_t count, struct dbus_reply **reply);
int dbus_send_call(struct linux_dbus *bus, unsigned flags, const char *destination, const char *path, const char *interface, const char *member, const char *signature, const struct dbus_arg *args, size_t count, uint32_t *serial);
int dbus_add_match(struct linux_dbus *bus, const char *rule);
int dbus_fd(const struct linux_dbus *bus);
int dbus_dispatch(struct linux_dbus *bus, dbus_signal_fn callback, void *data);
int dbus_dispatch_messages(struct linux_dbus *bus, dbus_message_fn callback, void *data);
int dbus_send_return(struct linux_dbus *bus, const struct dbus_reply *call, const char *signature, const struct dbus_arg *args, size_t count);
int dbus_send_error(struct linux_dbus *bus, const struct dbus_reply *call, const char *name, const char *text);
int dbus_send_return_to(struct linux_dbus *bus, const char *destination, uint32_t serial, const char *signature, const struct dbus_arg *args, size_t count);
int dbus_send_error_to(struct linux_dbus *bus, const char *destination, uint32_t serial, const char *name, const char *text);
int dbus_send_signal(struct linux_dbus *bus, const char *path, const char *interface, const char *member, const char *signature, const struct dbus_arg *args, size_t count);
void dbus_reply_free(struct dbus_reply *reply);
int dbus_read_number(struct dbus_reply *reply, uint32_t *number);
int dbus_read_string(struct dbus_reply *reply, const char **string);
int dbus_read_byte(struct dbus_reply *reply, uint8_t *byte);
int dbus_read_signature(struct dbus_reply *reply, const char **signature);
int dbus_read_variant(struct dbus_reply *reply, const char **signature);
int dbus_read_int16(struct dbus_reply *reply, uint16_t *value);
int dbus_read_array(struct dbus_reply *reply, size_t alignment, size_t *end);
int dbus_align(struct dbus_reply *reply, size_t alignment);
int dbus_skip_value(struct dbus_reply *reply, const char **signature);
const char *dbus_type_end(const char *signature, unsigned depth);
int dbus_take_fd(struct dbus_reply *reply, uint32_t index);
#endif
