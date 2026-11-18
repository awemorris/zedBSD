/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the small D-Bus client the Linux backend needs: the
 * authenticated, fd-carrying calls of the seat and the power on the system
 * bus, and on the session bus the notification server's side (ws156-p006):
 * method calls received and answered, signals sent.
 */
#include "dbus-linux.h"
#include <errno.h>
#include <stddef.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define DBUS_TIMEOUT_MS 5000U

/* The header fields' codes (the specification's HEADER_FIELD_*). */
#define DBUS_FIELD_PATH 1U
#define DBUS_FIELD_INTERFACE 2U
#define DBUS_FIELD_MEMBER 3U
#define DBUS_FIELD_ERROR_NAME 4U
#define DBUS_FIELD_REPLY_SERIAL 5U
#define DBUS_FIELD_DESTINATION 6U
#define DBUS_FIELD_SIGNATURE 8U

/* The longest array the specification allows, in bytes. */
#define DBUS_ARRAY_MAX 67108864U

/* One bounded encoder refuses growth before writing any bytes. */
struct dbus_writer {
	unsigned char *bytes;
	size_t used;
};

/*
 * The header of one message this end sends: its kind and flags, its serial
 * and the serial it answers (0 for none), and its fields, NULL when a field
 * is absent (an empty signature is absent too).
 */
struct dbus_header {
	unsigned type;
	unsigned flags;
	uint32_t serial;
	uint32_t reply_serial;
	const char *destination;
	const char *path;
	const char *interface;
	const char *member;
	const char *error_name;
	const char *signature;
};

static int bus_connect(struct linux_dbus *bus, const struct sockaddr_un *address, socklen_t length, uint64_t deadline);
static int bus_authenticate(struct linux_dbus *bus, int negotiate_fds, uint64_t deadline);
static int bus_hello(struct linux_dbus *bus);
static int bus_session_address(struct sockaddr_un *address, socklen_t *length);
static int bus_address_value(const char *value, size_t length, char *out, size_t capacity, size_t *used);
static int bus_hex_digit(char digit);
static int bus_now(uint64_t *now);
static int bus_wait(int fd, short events, uint64_t deadline);
static int bus_send(struct linux_dbus *bus, const void *bytes, size_t size, uint64_t deadline);
static int bus_auth_line(struct linux_dbus *bus, char *line, size_t capacity, uint64_t deadline);
static uint32_t bus_get32(const unsigned char *bytes);
static void bus_put32(unsigned char *bytes, uint32_t value);
static int writer_align(struct dbus_writer *writer, size_t alignment);
static int writer_string(struct dbus_writer *writer, char type, const char *string);
static int writer_number(struct dbus_writer *writer, uint32_t number);
static int writer_field(struct dbus_writer *writer, unsigned code, char type, const char *string);
static int writer_field_number(struct dbus_writer *writer, unsigned code, uint32_t number);
static int writer_strings(struct dbus_writer *writer, const char *const *strings, size_t count);
static int writer_variant(struct dbus_writer *writer, const struct dbus_arg *argument);
static int bus_build(struct dbus_reply *message, const struct dbus_header *header, const struct dbus_arg *args, size_t count);
static int bus_emit(struct linux_dbus *bus, struct dbus_header *header, const struct dbus_arg *args, size_t count);
static uint32_t bus_next_serial(struct linux_dbus *bus);
static size_t bus_alignment(char type);
static int bus_skip(struct dbus_reply *message, const char **signature, unsigned depth);
static int reader_string(struct dbus_reply *message, size_t *cursor, size_t end, char type, const char **string);
static int bus_parse(struct dbus_reply *message);
static int bus_receive(struct linux_dbus *bus, struct dbus_reply **message);
static int bus_queue(struct linux_dbus *bus, struct dbus_reply *message);

/*
 * Authenticates this UID and negotiates ancillary file passing on the system bus.
 */
int
dbus_open_system(
	struct linux_dbus *bus)
{
	struct sockaddr_un address;
	uint64_t deadline = 0;
	int error;

	/* A failed open can always be unwound by the same close path. */
	memset(bus, 0, sizeof(*bus));
	bus->fd = -1;
	error = bus_now(&deadline);
	if (error != 0)
		return error;
	deadline += DBUS_TIMEOUT_MS;

	/* The system endpoint is fixed; callers cannot redirect device authority. */
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	memcpy(address.sun_path, "/run/dbus/system_bus_socket", sizeof("/run/dbus/system_bus_socket"));
	error = bus_connect(bus, &address, sizeof(address), deadline);
	if (error != 0)
		return error;

	/* The seat's devices arrive as descriptors, so their passing is required. */
	error = bus_authenticate(bus, 1, deadline);
	if (error != 0)
		return error;

	/* The bus names this connection before any other message. */
	error = bus_hello(bus);
	if (error != 0)
		return error;

	/* Succeeded: this bus owns an authenticated unique connection name. */
	return 0;
}

/*
 * Connects to the user's session bus and authenticates this UID.
 *
 * The bus is the first unix address of DBUS_SESSION_BUS_ADDRESS (a path or
 * an abstract name), or $XDG_RUNTIME_DIR/bus without it.  Returns 0, ENOENT
 * when neither names a bus, or the error of connecting.
 */
int
dbus_open_session(
	struct linux_dbus *bus)
{
	struct sockaddr_un address;
	socklen_t length;
	uint64_t deadline = 0;
	int error;

	/* A failed open can always be unwound by the same close path. */
	memset(bus, 0, sizeof(*bus));
	bus->fd = -1;
	error = bus_now(&deadline);
	if (error != 0)
		return error;
	deadline += DBUS_TIMEOUT_MS;

	/* Where the user's session bus listens. */
	error = bus_session_address(&address, &length);
	if (error != 0)
		return error;

	/* The connection, inside the authentication deadline. */
	error = bus_connect(bus, &address, length, deadline);
	if (error != 0)
		return error;

	/* No descriptor travels on the session bus's notifications. */
	error = bus_authenticate(bus, 0, deadline);
	if (error != 0)
		return error;

	/* The bus names this connection before any other message. */
	error = bus_hello(bus);
	if (error != 0)
		return error;

	/* Succeeded: this bus owns an authenticated unique connection name. */
	return 0;
}

/*
 * Connects to the bus listening on a Unix socket path and authenticates
 * this UID, without file passing.
 *
 * Bluetooth's connection uses it with the system bus's path, or with a
 * private bus's path in a host test.  Returns 0, EINVAL for a path that
 * does not fit, or the error of connecting.
 */
int
dbus_open_address(
	struct linux_dbus *bus,
	const char *path)
{
	struct sockaddr_un address;
	uint64_t deadline;
	size_t length;
	int error;

	/* A failed open can always be unwound by the same close path. */
	memset(bus, 0, sizeof(*bus));
	bus->fd = -1;
	deadline = 0;
	error = bus_now(&deadline);
	if (error != 0)
		return error;
	deadline += DBUS_TIMEOUT_MS;

	/* The socket's path, with its NUL, must fit the address. */
	length = strlen(path);
	if (length == 0 || length >= sizeof(address.sun_path))
		return EINVAL;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	memcpy(address.sun_path, path, length + 1);

	/* The connection, inside the authentication deadline. */
	error = bus_connect(bus, &address, sizeof(address), deadline);
	if (error != 0)
		return error;

	/* No descriptor travels on this connection. */
	error = bus_authenticate(bus, 0, deadline);
	if (error != 0)
		return error;

	/* The bus names this connection before any other message. */
	error = bus_hello(bus);
	if (error != 0)
		return error;

	/* Succeeded: this bus owns an authenticated unique connection name. */
	return 0;
}

/*
 * Closes the bus and every file owned by partial or queued frames.
 */
void
dbus_close(
	struct linux_dbus *bus)
{
	size_t index;

	/* No queued signal may retain a descriptor after seat cleanup. */
	for (index = 0; index < bus->signal_count; index++)
		dbus_reply_free(bus->signals[index]);

	/* Partial receives own rights even before the frame can be decoded. */
	dbus_reply_free(bus->receiving);
	if (bus->fd >= 0)
		(void)close(bus->fd);

	/* Succeeded: repeated cleanup sees an empty, disconnected bus. */
	memset(bus, 0, sizeof(*bus));
	bus->fd = -1;
	return;
}

/*
 * Sends one method and retains intervening signals while awaiting its serial.
 */
int
dbus_call(
	struct linux_dbus *bus,
	const char *destination,
	const char *path,
	const char *interface,
	const char *member,
	const char *signature,
	const struct dbus_arg *args,
	size_t count,
	struct dbus_reply **reply)
{
	struct dbus_header header;
	struct dbus_reply *message;
	uint64_t deadline = 0;
	uint64_t now = 0;
	uint32_t serial;
	int error;

	/* Each call owns a unique nonzero serial and one finite deadline. */
	*reply = NULL;
	serial = bus_next_serial(bus);
	error = bus_now(&deadline);
	if (error != 0)
		return error;
	deadline += DBUS_TIMEOUT_MS;

	/* The call's header: where it goes and what it asks. */
	memset(&header, 0, sizeof(header));
	header.type = LINUX_DBUS_METHOD_CALL;
	header.serial = serial;
	header.destination = destination;
	header.path = path;
	header.interface = interface;
	header.member = member;
	header.signature = signature;

	/* An encoder failure cannot leak a partially transmitted request. */
	message = calloc(1, sizeof(*message));
	if (message == NULL)
		return ENOMEM;
	error = bus_build(message, &header, args, count);
	if (error != 0) {
		dbus_reply_free(message);
		return error;
	}

	/* A partial send failure terminates the framing connection. */
	error = bus_send(bus, message->bytes, message->size, deadline);
	dbus_reply_free(message);
	if (error != 0) {
		dbus_close(bus);
		return error;
	}

	/* Signals keep their original arrival order until the main loop dispatches them. */
	for (;;) {
		/* Even a continuously busy signal stream cannot extend this method deadline. */
		error = bus_now(&now);
		if (error != 0 || now >= deadline) {
			dbus_close(bus);
			if (error != 0)
				return error;
			return ETIMEDOUT;
		}

		/* Reads the next bounded frame, retaining any partial suffix. */
		error = bus_receive(bus, &message);
		if (error == EAGAIN) {
			error = bus_wait(bus->fd, POLLIN, deadline);
			if (error == 0)
				continue;
		}

		/* A timeout or broken frame cannot leave rights behind on the bus. */
		if (error != 0) {
			dbus_close(bus);
			return error;
		}

		/* Only the matching return may complete this synchronous call. */
		if ((message->type == 2 ||
		     message->type == 3) &&
		    message->reply_serial == serial) {
			if (message->type == 3) {
				fprintf(stderr, "wayland: dbus %s: %s\n", member, message->error);
				dbus_reply_free(message);
				return EACCES;
			}

			/* Succeeded: the caller now owns the response and its ancillary files. */
			*reply = message;
			return 0;
		}

		/* Unrelated replies are discarded; signals are bounded rather than silently lost. */
		if (message->type == 4) {
			error = bus_queue(bus, message);
			if (error != 0) {
				dbus_reply_free(message);
				dbus_close(bus);
				return error;
			}
		} else {
			dbus_reply_free(message);
		}
	}
}

/*
 * Sends one method call without waiting for its answer.
 *
 * The answer (a return or an error naming *serial) comes later through
 * dbus_dispatch_messages().  flags are the header's flags, for example
 * LINUX_DBUS_NO_AUTO_START.  Returns 0 with the call's serial, or the
 * error of building or sending (a sending error closes the bus).
 */
int
dbus_send_call(
	struct linux_dbus *bus,
	unsigned flags,
	const char *destination,
	const char *path,
	const char *interface,
	const char *member,
	const char *signature,
	const struct dbus_arg *args,
	size_t count,
	uint32_t *serial)
{
	struct dbus_header header;
	int error;

	/* The call's header: where it goes and what it asks. */
	memset(&header, 0, sizeof(header));
	header.type = LINUX_DBUS_METHOD_CALL;
	header.flags = flags;
	header.destination = destination;
	header.path = path;
	header.interface = interface;
	header.member = member;
	header.signature = signature;

	/* The call goes out with the next serial. */
	error = bus_emit(bus, &header, args, count);
	if (error != 0)
		return error;

	/* Succeeded: the answer will name this serial. */
	*serial = header.serial;
	return 0;
}

/*
 * Installs one signal subscription through the authenticated bus daemon.
 */
int
dbus_add_match(
	struct linux_dbus *bus,
	const char *rule)
{
	struct dbus_arg argument;
	struct dbus_reply *reply;
	int error;

	/* The daemon owns the subscription for this connection's lifetime. */
	memset(&argument, 0, sizeof(argument));
	argument.string = rule;
	error = dbus_call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "AddMatch", "s", &argument, 1, &reply);
	if (error != 0)
		return error;
	dbus_reply_free(reply);

	/* Succeeded: subsequent matching signals can join the bounded queue. */
	return 0;
}

/*
 * Supplies the socket used by the compositor's OS poll range.
 */
int
dbus_fd(
	const struct linux_dbus *bus)
{
	/* Succeeded: a negative fd explicitly denotes a closed connection. */
	return bus->fd;
}

/*
 * Dispatches queued and currently readable signals without blocking the main loop.
 */
int
dbus_dispatch(
	struct linux_dbus *bus,
	dbus_signal_fn callback,
	void *data)
{
	struct dbus_reply *message;
	size_t index;
	unsigned count;
	int error;

	/* A per-pass bound preserves client service under a busy signal stream. */
	for (count = 0; count < LINUX_DBUS_SIGNALS; count++) {
		/* Queued signals must run even when the socket's current revents is zero. */
		if (bus->signal_count != 0) {
			message = bus->signals[0];
			bus->signal_count--;

			/* Retains arrival order while removing this owned message. */
			for (index = 0; index < bus->signal_count; index++)
				bus->signals[index] = bus->signals[index + 1];
		} else {
			/* Receives at most the remainder of one frame, with no blocking read. */
			error = bus_receive(bus, &message);
			if (error == EAGAIN)
				return 0;
			if (error != 0) {
				dbus_close(bus);
				return error;
			}
		}

		/* The callback may take a resume fd; untaken rights close with the frame. */
		error = 0;
		if (message->type == 4)
			error = callback(message, data);
		dbus_reply_free(message);
		if (error != 0)
			return error;
	}

	/* Succeeded: remaining work stays owned for the next finite poll pass. */
	return 0;
}

/*
 * Returns every untaken ancillary descriptor and the response storage.
 */
void
dbus_reply_free(
	struct dbus_reply *reply)
{
	size_t index;

	/* Callers may unwind before a response has been allocated. */
	if (reply == NULL)
		return;

	/* Transferred rights are marked negative and never closed a second time. */
	for (index = 0; index < reply->fd_count; index++) {
		if (reply->fds[index] >= 0)
			(void)close(reply->fds[index]);
	}

	/* Succeeded: no descriptor lifetime depends on this frame any more. */
	free(reply);
	return;
}

/*
 * Reads one aligned unsigned, Boolean or file index from a reply body.
 */
int
dbus_read_number(
	struct dbus_reply *reply,
	uint32_t *number)
{
	size_t cursor;

	/* Padding and a complete scalar must fit before any body byte is read. */
	cursor = (reply->cursor + 3U) & ~(size_t)3U;
	if (cursor > reply->size || reply->size - cursor < 4)
		return EPROTO;
	*number = bus_get32(reply->bytes + cursor);
	reply->cursor = cursor + 4;

	/* Succeeded: the next body argument follows this scalar. */
	return 0;
}

/*
 * Reads a string or object path without copying outside the owned frame.
 */
int
dbus_read_string(
	struct dbus_reply *reply,
	const char **string)
{
	int error;

	/* Body readers share the same bounded representation as header strings. */
	error = reader_string(reply, &reply->cursor, reply->size, 's', string);
	if (error != 0)
		return error;

	/* Succeeded: the string remains valid until the caller frees this reply. */
	return 0;
}

/*
 * Transfers exactly one ancillary descriptor into seat ownership.
 */
int
dbus_take_fd(
	struct dbus_reply *reply,
	uint32_t index)
{
	int fd;

	/* An absent or already transferred index is a malformed handle. */
	if (index >= reply->fd_count)
		return -1;
	fd = reply->fds[index];
	if (fd < 0)
		return -1;
	reply->fds[index] = -1;

	/* Succeeded: only the seat now closes this file. */
	return fd;
}

/*
 * Dispatches the method calls and signals that have arrived, without
 * blocking the main loop.
 *
 * The callback is given each message in arrival order: each call (the
 * bus's own and other peers'), each signal, and each return and error,
 * which answer the calls of dbus_send_call() (a callback that sent none
 * looks only at the calls and the signals).  Returns 0, or the error that
 * closed the bus.
 */
int
dbus_dispatch_messages(
	struct linux_dbus *bus,
	dbus_message_fn callback,
	void *data)
{
	struct dbus_reply *message;
	size_t index;
	unsigned count;
	int error;

	/* A per-pass bound preserves the main loop's other work under a busy bus. */
	for (count = 0; count < LINUX_DBUS_SIGNALS; count++) {
		/* A closed bus has nothing more to give. */
		if (bus->fd < 0)
			return 0;

		/* Messages queued during a call come first, in their order. */
		if (bus->signal_count != 0) {
			message = bus->signals[0];
			bus->signal_count--;

			/* Retains arrival order while removing this owned message. */
			for (index = 0; index < bus->signal_count; index++)
				bus->signals[index] = bus->signals[index + 1];
		} else {
			/* Receives at most the remainder of one frame, with no blocking read. */
			error = bus_receive(bus, &message);
			if (error == EAGAIN)
				return 0;
			if (error != 0) {
				dbus_close(bus);
				return error;
			}
		}

		/* Every message goes to the callback; the frame is freed after it. */
		error = callback(message, data);
		dbus_reply_free(message);
		if (error != 0)
			return error;
	}

	/* Succeeded: remaining work stays owned for the next finite poll pass. */
	return 0;
}

/*
 * Answers a method call with a return carrying the arguments of signature.
 *
 * A call whose caller expects no answer is answered with nothing.  Returns
 * 0, or the error of building or sending (which closes the bus).
 */
int
dbus_send_return(
	struct linux_dbus *bus,
	const struct dbus_reply *call,
	const char *signature,
	const struct dbus_arg *args,
	size_t count)
{
	int error;

	/* A caller that wants no answer gets none. */
	if ((call->flags & LINUX_DBUS_NO_REPLY_EXPECTED) != 0)
		return 0;

	/* The return goes back to the caller, naming the call it answers. */
	error = dbus_send_return_to(bus, call->sender, call->serial, signature, args, count);
	if (error != 0)
		return error;

	/* Succeeded: the caller has its answer. */
	return 0;
}

/*
 * Answers a method call with an error of a name and a text.
 *
 * A call whose caller expects no answer is answered with nothing.  Returns
 * 0, or the error of building or sending (which closes the bus).
 */
int
dbus_send_error(
	struct linux_dbus *bus,
	const struct dbus_reply *call,
	const char *name,
	const char *text)
{
	int error;

	/* A caller that wants no answer gets none. */
	if ((call->flags & LINUX_DBUS_NO_REPLY_EXPECTED) != 0)
		return 0;

	/* The error goes back to the caller with its text. */
	error = dbus_send_error_to(bus, call->sender, call->serial, name, text);
	if (error != 0)
		return error;

	/* Succeeded: the caller has its answer. */
	return 0;
}

/*
 * Answers a call later, by its caller's name and serial, with a return
 * carrying the arguments of signature.
 *
 * The caller kept both from the call (the call's frame is freed when its
 * dispatch ends), and checked that it wants an answer.  Returns 0, or the
 * error of building or sending (which closes the bus).
 */
int
dbus_send_return_to(
	struct linux_dbus *bus,
	const char *destination,
	uint32_t serial,
	const char *signature,
	const struct dbus_arg *args,
	size_t count)
{
	struct dbus_header header;
	int error;

	/* The return names the caller and the call it answers. */
	memset(&header, 0, sizeof(header));
	header.type = LINUX_DBUS_METHOD_RETURN;
	header.flags = LINUX_DBUS_NO_REPLY_EXPECTED;
	header.reply_serial = serial;
	header.destination = destination;
	header.signature = signature;
	error = bus_emit(bus, &header, args, count);
	if (error != 0)
		return error;

	/* Succeeded: the caller has its answer. */
	return 0;
}

/*
 * Answers a call later, by its caller's name and serial, with an error of
 * a name and a text.
 *
 * Returns 0, or the error of building or sending (which closes the bus).
 */
int
dbus_send_error_to(
	struct linux_dbus *bus,
	const char *destination,
	uint32_t serial,
	const char *name,
	const char *text)
{
	struct dbus_header header;
	struct dbus_arg argument;
	int error;

	/* The error names the caller and the call it answers, with its text. */
	memset(&header, 0, sizeof(header));
	header.type = LINUX_DBUS_ERROR;
	header.flags = LINUX_DBUS_NO_REPLY_EXPECTED;
	header.reply_serial = serial;
	header.destination = destination;
	header.error_name = name;
	header.signature = "s";
	memset(&argument, 0, sizeof(argument));
	argument.string = text;
	error = bus_emit(bus, &header, &argument, 1);
	if (error != 0)
		return error;

	/* Succeeded: the caller has its answer. */
	return 0;
}

/*
 * Sends a signal of an object's interface to every peer that listens for it.
 *
 * Returns 0, or the error of building or sending (which closes the bus).
 */
int
dbus_send_signal(
	struct linux_dbus *bus,
	const char *path,
	const char *interface,
	const char *member,
	const char *signature,
	const struct dbus_arg *args,
	size_t count)
{
	struct dbus_header header;
	int error;

	/* A signal is sent to no one in particular. */
	memset(&header, 0, sizeof(header));
	header.type = LINUX_DBUS_SIGNAL;
	header.flags = LINUX_DBUS_NO_REPLY_EXPECTED;
	header.path = path;
	header.interface = interface;
	header.member = member;
	header.signature = signature;
	error = bus_emit(bus, &header, args, count);
	if (error != 0)
		return error;

	/* Succeeded: the signal is on the bus. */
	return 0;
}

/*
 * Reads one byte from a message body.
 */
int
dbus_read_byte(
	struct dbus_reply *reply,
	uint8_t *byte)
{
	/* The byte must be in the message. */
	if (reply->cursor >= reply->size)
		return EPROTO;
	*byte = reply->bytes[reply->cursor];
	reply->cursor++;

	/* Succeeded: the next body argument follows this byte. */
	return 0;
}

/*
 * Reads a signature (a variant's type) from a message body.
 */
int
dbus_read_signature(
	struct dbus_reply *reply,
	const char **signature)
{
	int error;

	/* A signature has a one-byte length and no alignment. */
	error = reader_string(reply, &reply->cursor, reply->size, 'g', signature);
	if (error != 0)
		return error;

	/* Succeeded: the signature remains valid until the caller frees this reply. */
	return 0;
}

/*
 * Reads a variant's signature from a message body and checks that it is
 * one complete type, so that the value that follows can be read or stepped
 * over by it.
 */
int
dbus_read_variant(
	struct dbus_reply *reply,
	const char **signature)
{
	const char *type_end;
	size_t length;
	int error;

	/* The variant's own signature: one byte of length and the text. */
	error = reader_string(reply, &reply->cursor, reply->size, 'g', signature);
	if (error != 0)
		return error;

	/* Exactly one complete type, no more and no less. */
	length = strlen(*signature);
	type_end = dbus_type_end(*signature, 1);
	if (length == 0)
		return EPROTO;
	if (type_end != *signature + length)
		return EPROTO;

	/* Succeeded: the value of that type follows. */
	return 0;
}

/*
 * Reads one aligned sixteen-bit integer (a signed "n" or an unsigned "q")
 * from a message body; a signed value is the same bits.
 */
int
dbus_read_int16(
	struct dbus_reply *reply,
	uint16_t *value)
{
	size_t cursor;

	/* Padding and both bytes must fit before either is read. */
	cursor = (reply->cursor + 1U) & ~(size_t)1U;
	if (cursor > reply->size)
		return EPROTO;
	if (reply->size - cursor < 2)
		return EPROTO;

	/* The little-endian value. */
	*value = (uint16_t)(reply->bytes[cursor] | (reply->bytes[cursor + 1] << 8));
	reply->cursor = cursor + 2;

	/* Succeeded: the next body argument follows this value. */
	return 0;
}

/*
 * Starts reading an array whose elements have an alignment: the cursor is
 * left at its first element and *end is where its elements end.
 */
int
dbus_read_array(
	struct dbus_reply *reply,
	size_t alignment,
	size_t *end)
{
	uint32_t length;
	int error;

	/* The elements' byte length. */
	error = dbus_read_number(reply, &length);
	if (error != 0)
		return error;
	if (length > DBUS_ARRAY_MAX)
		return EPROTO;

	/* The padding before the first element is not counted in the length. */
	error = dbus_align(reply, alignment);
	if (error != 0)
		return error;
	if (reply->size - reply->cursor < length)
		return EPROTO;

	/* Succeeded: the elements lie between the cursor and *end. */
	*end = reply->cursor + length;
	return 0;
}

/*
 * Moves the cursor of a message body to an alignment, inside the message.
 */
int
dbus_align(
	struct dbus_reply *reply,
	size_t alignment)
{
	size_t cursor;

	/* The padding must be in the message. */
	cursor = (reply->cursor + alignment - 1U) & ~(alignment - 1U);
	if (cursor > reply->size)
		return EPROTO;
	reply->cursor = cursor;

	/* Succeeded: the next value starts here. */
	return 0;
}

/*
 * Steps over one value of the complete type at *signature, which moves past
 * that type (for the arguments and the variants a reader does not use).
 */
int
dbus_skip_value(
	struct dbus_reply *reply,
	const char **signature)
{
	int error;

	/* The value and every container in it, bounded in depth. */
	error = bus_skip(reply, signature, 0);
	if (error != 0)
		return error;

	/* Succeeded: past the value. */
	return 0;
}

/*
 * Finds where one complete type that starts at signature ends.  Returns the
 * character after it, or NULL when the signature does not start with a
 * valid type or nests deeper than LINUX_DBUS_DEPTH.
 */
const char *
dbus_type_end(
	const char *signature,
	unsigned depth)
{
	const char *inner;
	const char *basic;
	char type;

	/* Containers nest only so deep. */
	if (depth > LINUX_DBUS_DEPTH)
		return NULL;
	type = signature[0];

	/* A basic type (a dictionary entry's key must be one) or a variant: one character. */
	basic = strchr("ybnqiuxtdsogh", type);
	if (type != '\0' && basic != NULL)
		return signature + 1;
	if (type == 'v')
		return signature + 1;

	/* An array: one element type after it. */
	if (type == 'a') {
		inner = dbus_type_end(signature + 1, depth + 1);
		return inner;
	}

	/* A structure: one or more member types, then its close. */
	if (type == '(') {
		inner = signature + 1;
		if (*inner == ')')
			return NULL;
		while (*inner != ')') {
			inner = dbus_type_end(inner, depth + 1);
			if (inner == NULL)
				return NULL;
		}

		/* Past its close. */
		return inner + 1;
	}

	/* A dictionary entry: a basic key, one value type, then its close. */
	if (type == '{') {
		basic = strchr("ybnqiuxtdsogh", signature[1]);
		if (signature[1] == '\0' || basic == NULL)
			return NULL;
		inner = dbus_type_end(signature + 2, depth + 1);
		if (inner == NULL || *inner != '}')
			return NULL;
		return inner + 1;
	}

	/* Not a type. */
	return NULL;
}

/* Connects the bus's socket to a listener, inside the caller's deadline. */
static int
bus_connect(
	struct linux_dbus *bus,
	const struct sockaddr_un *address,
	socklen_t length,
	uint64_t deadline)
{
	int error;
	int status;
	socklen_t status_size;

	/* Never wait indefinitely for an overloaded local socket listener. */
	bus->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (bus->fd < 0)
		return errno;

	/* A connection that is not made at once is waited for. */
	error = connect(bus->fd, (const struct sockaddr *)address, length);
	if (error == 0)
		return 0;
	if (errno != EINPROGRESS)
		return errno;

	/* An asynchronous connect must finish inside the authentication deadline. */
	error = bus_wait(bus->fd, POLLOUT, deadline);
	if (error != 0)
		return error;
	status_size = sizeof(status);
	error = getsockopt(bus->fd, SOL_SOCKET, SO_ERROR, &status, &status_size);
	if (error != 0)
		return errno;
	if (status != 0)
		return status;

	/* Succeeded: the socket is connected. */
	return 0;
}

/*
 * Authenticates this UID with EXTERNAL, then asks for ancillary file
 * passing when negotiate_fds is set, and starts the binary frames.
 */
static int
bus_authenticate(
	struct linux_dbus *bus,
	int negotiate_fds,
	uint64_t deadline)
{
	char uid[32];
	char request[160];
	char line[256];
	size_t length;
	size_t index;
	int error;
	int status;

	/* EXTERNAL carries the decimal UID encoded as hexadecimal ASCII bytes. */
	status = snprintf(uid, sizeof(uid), "%lu", (unsigned long)getuid());
	if (status < 0 || (size_t)status >= sizeof(uid))
		return EOVERFLOW;
	length = (size_t)status;
	request[0] = '\0';
	memcpy(request + 1, "AUTH EXTERNAL ", 14);

	/* Every decimal digit contributes exactly two hexadecimal digits. */
	for (index = 0; index < length; index++) {
		status = snprintf(request + 15 + index * 2, 3, "%02x", (unsigned char)uid[index]);
		if (status != 2)
			return EOVERFLOW;
	}

	/* The leading NUL proves Unix peer credentials before the authentication command. */
	memcpy(request + 15 + length * 2, "\r\n", 2);
	error = bus_send(bus, request, 17 + length * 2, deadline);
	if (error != 0)
		return error;
	error = bus_auth_line(bus, line, sizeof(line), deadline);
	if (error != 0)
		return error;
	status = strncmp(line, "OK ", 3);
	if (status != 0)
		return EACCES;

	/* Refuses a connection that cannot transfer the descriptors the caller needs. */
	if (negotiate_fds) {
		error = bus_send(bus, "NEGOTIATE_UNIX_FD\r\n", 19, deadline);
		if (error != 0)
			return error;
		error = bus_auth_line(bus, line, sizeof(line), deadline);
		if (error != 0)
			return error;
		status = strcmp(line, "AGREE_UNIX_FD\r\n");
		if (status != 0)
			return ENOTSUP;
	}

	/* Binary frames begin only after the authentication exchange is complete. */
	error = bus_send(bus, "BEGIN\r\n", 7, deadline);
	if (error != 0)
		return error;

	/* Succeeded: the connection speaks binary frames now. */
	return 0;
}

/* Says Hello, which gives the connection its unique name. */
static int
bus_hello(
	struct linux_dbus *bus)
{
	struct dbus_reply *reply;
	int error;

	/* The daemon answers with the name; this end does not need it. */
	error = dbus_call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "Hello", "", NULL, 0, &reply);
	if (error != 0)
		return error;
	dbus_reply_free(reply);

	/* Succeeded: the connection has its name. */
	return 0;
}

/*
 * Finds the session bus's socket: the first unix address of
 * DBUS_SESSION_BUS_ADDRESS with a path or an abstract name, else
 * $XDG_RUNTIME_DIR/bus.  Returns 0 with the address and its length, ENOENT
 * when there is none, or EINVAL for one that does not fit.
 */
static int
bus_session_address(
	struct sockaddr_un *address,
	socklen_t *length)
{
	const char *variable;
	const char *entry;
	const char *entry_end;
	const char *key;
	const char *key_end;
	const char *value;
	size_t key_length;
	size_t value_length;
	size_t used;
	size_t capacity;
	char *name;
	int abstract;
	int is_path;
	int is_abstract;
	int status;
	int error;

	/* Nothing found yet. */
	memset(address, 0, sizeof(*address));
	address->sun_family = AF_UNIX;

	/* Each address of the variable, separated by semicolons. */
	variable = getenv("DBUS_SESSION_BUS_ADDRESS");
	entry = variable;
	while (entry != NULL && *entry != '\0') {
		/* This address ends at the next semicolon or the end. */
		entry_end = strchr(entry, ';');
		if (entry_end == NULL)
			entry_end = entry + strlen(entry);

		/* Only a unix transport is spoken here. */
		status = strncmp(entry, "unix:", 5);
		if (status != 0 || entry_end - entry < 5) {
			entry = entry_end;
			if (*entry == ';')
				entry++;
			continue;
		}

		/* Its keys, separated by commas: path= or abstract= is the socket. */
		key = entry + 5;
		while (key < entry_end) {
			key_end = memchr(key, ',', (size_t)(entry_end - key));
			if (key_end == NULL)
				key_end = entry_end;
			key_length = (size_t)(key_end - key);

			/* Which key this is: a path, an abstract name, or another. */
			is_path = 0;
			is_abstract = 0;
			if (key_length > 5) {
				status = memcmp(key, "path=", 5);
				if (status == 0)
					is_path = 1;
			}

			/* An abstract name's key. */
			if (key_length > 9) {
				status = memcmp(key, "abstract=", 9);
				if (status == 0)
					is_abstract = 1;
			}

			/* The kind of name this key gives, if it gives one. */
			abstract = -1;
			value = NULL;
			if (is_path) {
				abstract = 0;
				value = key + 5;
			} else if (is_abstract) {
				abstract = 1;
				value = key + 9;
			}

			/* A path is copied with its NUL; an abstract name after a NUL of its own. */
			if (value != NULL) {
				value_length = (size_t)(key_end - value);
				name = address->sun_path + abstract;
				capacity = sizeof(address->sun_path) - (size_t)abstract;
				error = bus_address_value(value, value_length, name, capacity, &used);
				if (error != 0)
					return error;

				/* An abstract name's length ends the address; a path's NUL does. */
				*length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + (size_t)abstract + used);
				if (!abstract)
					*length = (socklen_t)sizeof(*address);
				return 0;
			}

			/* The next key. */
			key = key_end;
			if (key < entry_end)
				key++;
		}

		/* The next address. */
		entry = entry_end;
		if (*entry == ';')
			entry++;
	}

	/* Without the variable, the bus of the user's runtime directory. */
	variable = getenv("XDG_RUNTIME_DIR");
	if (variable == NULL || variable[0] != '/')
		return ENOENT;
	status = snprintf(address->sun_path, sizeof(address->sun_path), "%s/bus", variable);
	if (status < 0 || (size_t)status >= sizeof(address->sun_path))
		return EINVAL;

	/* Succeeded: the runtime directory's bus. */
	*length = (socklen_t)sizeof(*address);
	return 0;
}

/*
 * Copies an address's value into out, undoing its %XX escapes, without its
 * NUL past capacity - 1 bytes.  Returns 0 with the bytes copied (not
 * counting the NUL), or EINVAL for a bad escape or a value that does not
 * fit.
 */
static int
bus_address_value(
	const char *value,
	size_t length,
	char *out,
	size_t capacity,
	size_t *used)
{
	size_t index;
	size_t count;
	int high;
	int low;

	/* Each byte of the value, an escape being three. */
	count = 0;
	index = 0;
	while (index < length) {
		/* Room for it and the NUL. */
		if (count + 1 >= capacity)
			return EINVAL;

		/* A byte as it is. */
		if (value[index] != '%') {
			out[count] = value[index];
			count++;
			index++;
			continue;
		}

		/* An escape: two hexadecimal digits. */
		if (length - index < 3)
			return EINVAL;
		high = bus_hex_digit(value[index + 1]);
		low = bus_hex_digit(value[index + 2]);
		if (high < 0 || low < 0)
			return EINVAL;
		out[count] = (char)(high * 16 + low);
		count++;
		index += 3;
	}

	/* An empty name names nothing. */
	if (count == 0)
		return EINVAL;

	/* Succeeded: the value, ended. */
	out[count] = '\0';
	*used = count;
	return 0;
}

/* Gives the value of a hexadecimal digit, or -1 for another character. */
static int
bus_hex_digit(
	char digit)
{
	/* A decimal digit. */
	if (digit >= '0' && digit <= '9')
		return digit - '0';

	/* A lower-case letter. */
	if (digit >= 'a' && digit <= 'f')
		return digit - 'a' + 10;

	/* An upper-case letter. */
	if (digit >= 'A' && digit <= 'F')
		return digit - 'A' + 10;

	/* Not a digit. */
	return -1;
}

/* Measures deadlines using a clock unaffected by wall-clock corrections. */
static int
bus_now(
	uint64_t *now)
{
	struct timespec time;
	int error;

	/* Clock failure cannot make a bounded call wait forever. */
	error = clock_gettime(CLOCK_MONOTONIC, &time);
	if (error != 0)
		return errno;
	*now = (uint64_t)time.tv_sec * 1000U + (uint64_t)time.tv_nsec / 1000000U;

	/* Succeeded: the caller can compare this monotonic millisecond stamp. */
	return 0;
}

/* Waits for one socket operation inside the caller's original deadline. */
static int
bus_wait(
	int fd,
	short events,
	uint64_t deadline)
{
	struct pollfd descriptor;
	uint64_t now = 0;
	int error;
	int ready;

	/* Signal interruptions retain the same absolute timeout. */
	for (;;) {
		error = bus_now(&now);
		if (error != 0)
			return error;
		if (now >= deadline)
			return ETIMEDOUT;
		descriptor.fd = fd;
		descriptor.events = events;
		descriptor.revents = 0;
		ready = poll(&descriptor, 1, (int)(deadline - now));
		if (ready < 0) {
			if (errno == EINTR)
				continue;
			return errno;
		}

		/* EOF and invalid descriptors cannot satisfy a transport operation. */
		if (ready == 0)
			return ETIMEDOUT;
		if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
			return EPIPE;
		if ((descriptor.revents & events) != 0)
			return 0;
	}
}

/* Sends every request byte without extending the method's deadline. */
static int
bus_send(
	struct linux_dbus *bus,
	const void *bytes,
	size_t size,
	uint64_t deadline)
{
	const unsigned char *source;
	ssize_t sent;
	int error;

	/* Partial writes advance only over bytes the socket accepted. */
	source = bytes;
	while (size != 0) {
		sent = send(bus->fd, source, size, MSG_DONTWAIT | MSG_NOSIGNAL);
		if (sent < 0) {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				return errno;
			error = bus_wait(bus->fd, POLLOUT, deadline);
			if (error != 0)
				return error;
			continue;
		}

		/* A zero-sized successful send cannot advance framing. */
		if (sent == 0)
			return EPIPE;
		source += sent;
		size -= (size_t)sent;
	}

	/* Succeeded: the complete request is on the authenticated connection. */
	return 0;
}

/* Receives one bounded authentication line without consuming binary-frame bytes. */
static int
bus_auth_line(
	struct linux_dbus *bus,
	char *line,
	size_t capacity,
	uint64_t deadline)
{
	struct iovec vector;
	struct msghdr header;
	ssize_t received;
	size_t used;
	int error;

	/* Ancillary transfer is negotiated only after this authentication exchange. */
	used = 0;
	while (used + 1 < capacity) {
		memset(&header, 0, sizeof(header));
		vector.iov_base = line + used;
		vector.iov_len = 1;
		header.msg_iov = &vector;
		header.msg_iovlen = 1;
		received = recvmsg(bus->fd, &header, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
		if (received < 0) {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				return errno;
			error = bus_wait(bus->fd, POLLIN, deadline);
			if (error != 0)
				return error;
			continue;
		}

		/* Truncated ancillary input is invalid even during authentication. */
		if (received == 0)
			return EPIPE;
		if ((header.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) != 0)
			return EPROTO;
		used++;
		if (used >= 2 &&
		    line[used - 2] == '\r' &&
		    line[used - 1] == '\n') {
			line[used] = '\0';
			return 0;
		}
	}

	/* An overlong line is refused without interpreting its truncated prefix. */
	return EOVERFLOW;
}

/* Decodes one little-endian wire scalar independently of host alignment. */
static uint32_t
bus_get32(
	const unsigned char *bytes)
{
	/* Succeeded: each byte contributes its specified wire significance. */
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

/* Writes a scalar in the connection's explicit little-endian representation. */
static void
bus_put32(
	unsigned char *bytes,
	uint32_t value)
{
	/* Each independent byte avoids host alignment and aliasing assumptions. */
	bytes[0] = (unsigned char)value;
	bytes[1] = (unsigned char)(value >> 8);
	bytes[2] = (unsigned char)(value >> 16);
	bytes[3] = (unsigned char)(value >> 24);

	/* Succeeded: all four wire bytes have been stored. */
	return;
}

/* Pads an encoder to a protocol boundary without exceeding its frame. */
static int
writer_align(
	struct dbus_writer *writer,
	size_t alignment)
{
	size_t next;

	/* The current frame is already bounded, so alignment cannot wrap. */
	next = (writer->used + alignment - 1) & ~(alignment - 1);
	if (next > LINUX_DBUS_MAX)
		return EOVERFLOW;
	memset(writer->bytes + writer->used, 0, next - writer->used);
	writer->used = next;

	/* Succeeded: subsequent values start at their required alignment. */
	return 0;
}

/* Encodes a terminated string, path or signature with its length prefix. */
static int
writer_string(
	struct dbus_writer *writer,
	char type,
	const char *string)
{
	size_t length;
	size_t prefix;
	int error;

	/* NULL is not a valid protocol string even for an empty signature. */
	if (string == NULL)
		return EINVAL;
	length = strlen(string);
	prefix = 1;
	if (type != 'g') {
		error = writer_align(writer, 4);
		if (error != 0)
			return error;
		prefix = 4;
	} else {
		if (length > 255)
			return EOVERFLOW;
	}

	/* Refuses lengths that cannot leave room for the prefix and terminator. */
	if (length >= LINUX_DBUS_MAX || prefix + length + 1 > LINUX_DBUS_MAX - writer->used)
		return EOVERFLOW;
	if (type == 'g') {
		writer->bytes[writer->used] = (unsigned char)length;
	} else {
		bus_put32(writer->bytes + writer->used, (uint32_t)length);
	}

	/* Copies the terminator as part of the bounded representation. */
	writer->used += prefix;
	memcpy(writer->bytes + writer->used, string, length + 1);
	writer->used += length + 1;

	/* Succeeded: the next encoded value follows this complete string. */
	return 0;
}

/* Encodes one aligned numeric argument without unaligned stores. */
static int
writer_number(
	struct dbus_writer *writer,
	uint32_t number)
{
	int error;

	/* A numeric value reserves four bytes after its padding. */
	error = writer_align(writer, 4);
	if (error != 0)
		return error;
	if (LINUX_DBUS_MAX - writer->used < 4)
		return EOVERFLOW;
	bus_put32(writer->bytes + writer->used, number);
	writer->used += 4;

	/* Succeeded: the scalar is fully encoded. */
	return 0;
}

/* Appends one a(yv) header entry and its single-value variant. */
static int
writer_field(
	struct dbus_writer *writer,
	unsigned code,
	char type,
	const char *string)
{
	char signature[2];
	int error;

	/* Each field structure starts on an eight-byte boundary. */
	error = writer_align(writer, 8);
	if (error != 0)
		return error;
	if (writer->used >= LINUX_DBUS_MAX)
		return EOVERFLOW;
	writer->bytes[writer->used++] = (unsigned char)code;
	signature[0] = type;
	signature[1] = '\0';
	error = writer_string(writer, 'g', signature);
	if (error != 0)
		return error;
	error = writer_string(writer, type, string);
	if (error != 0)
		return error;

	/* Succeeded: this header field is complete. */
	return 0;
}

/* Appends one a(yv) header entry whose variant is an unsigned (the serial a reply answers). */
static int
writer_field_number(
	struct dbus_writer *writer,
	unsigned code,
	uint32_t number)
{
	int error;

	/* Each field structure starts on an eight-byte boundary. */
	error = writer_align(writer, 8);
	if (error != 0)
		return error;
	if (writer->used >= LINUX_DBUS_MAX)
		return EOVERFLOW;
	writer->bytes[writer->used++] = (unsigned char)code;
	error = writer_string(writer, 'g', "u");
	if (error != 0)
		return error;
	error = writer_number(writer, number);
	if (error != 0)
		return error;

	/* Succeeded: this header field is complete. */
	return 0;
}

/* Encodes an array of strings ("as"): its byte length, then each string. */
static int
writer_strings(
	struct dbus_writer *writer,
	const char *const *strings,
	size_t count)
{
	size_t length_at;
	size_t start;
	size_t index;
	int error;

	/* The length's place; a string's alignment needs no padding after it. */
	error = writer_align(writer, 4);
	if (error != 0)
		return error;
	if (LINUX_DBUS_MAX - writer->used < 4)
		return EOVERFLOW;
	length_at = writer->used;
	writer->used += 4;
	start = writer->used;

	/* Each element. */
	for (index = 0; index < count; index++) {
		error = writer_string(writer, 's', strings[index]);
		if (error != 0)
			return error;
	}

	/* The elements' byte length, in its place. */
	bus_put32(writer->bytes + length_at, (uint32_t)(writer->used - start));

	/* Succeeded: the array is complete. */
	return 0;
}

/*
 * Encodes a variant of one simple value: its signature, then the value of
 * argument->variant's type ("s", "o" or "g" from string, "u" or "b" from
 * number).  Returns 0, EINVAL for another type or a Boolean that is not 0
 * or 1, or EOVERFLOW.
 */
static int
writer_variant(
	struct dbus_writer *writer,
	const struct dbus_arg *argument)
{
	const char *type;
	int error;

	/* Only one simple type, named by one character. */
	type = argument->variant;
	if (type == NULL)
		return EINVAL;
	if (type[0] == '\0' || type[1] != '\0')
		return EINVAL;

	/* The variant's signature. */
	error = writer_string(writer, 'g', type);
	if (error != 0)
		return error;

	/* A string, an object path or a signature. */
	if (type[0] == 's' ||
	    type[0] == 'o' ||
	    type[0] == 'g') {
		error = writer_string(writer, type[0], argument->string);
		if (error != 0)
			return error;

		/* Succeeded: the variant is complete. */
		return 0;
	}

	/* A Boolean is 0 or 1. */
	if (type[0] == 'b') {
		if (argument->number > 1)
			return EINVAL;
	} else if (type[0] != 'u') {
		/* No other type is spoken here. */
		return EINVAL;
	}

	/* An unsigned or a Boolean: four aligned bytes. */
	error = writer_number(writer, argument->number);
	if (error != 0)
		return error;

	/* Succeeded: the variant is complete. */
	return 0;
}

/*
 * Builds one message from a header and the small argument vocabulary: a
 * string, an object path, a signature, an unsigned, a Boolean, an array of
 * strings, and a variant of one of the simple ones.
 */
static int
bus_build(
	struct dbus_reply *message,
	const struct dbus_header *header,
	const struct dbus_arg *args,
	size_t count)
{
	struct dbus_writer writer;
	const char *signature;
	size_t fields;
	size_t body;
	size_t index;
	char type;
	int error;

	/* Fixed header bytes precede the header-field array. */
	writer.bytes = message->bytes;
	writer.used = 16;
	signature = header->signature;
	if (signature == NULL)
		signature = "";

	/* The fields the header names, each only when present. */
	if (header->path != NULL) {
		error = writer_field(&writer, DBUS_FIELD_PATH, 'o', header->path);
		if (error != 0)
			return error;
	}

	/* The explicit destination rather than implicit peer routing. */
	if (header->destination != NULL) {
		error = writer_field(&writer, DBUS_FIELD_DESTINATION, 's', header->destination);
		if (error != 0)
			return error;
	}

	/* The interface and the member of a call or a signal. */
	if (header->interface != NULL) {
		error = writer_field(&writer, DBUS_FIELD_INTERFACE, 's', header->interface);
		if (error != 0)
			return error;
	}

	/* The member. */
	if (header->member != NULL) {
		error = writer_field(&writer, DBUS_FIELD_MEMBER, 's', header->member);
		if (error != 0)
			return error;
	}

	/* The name of an error. */
	if (header->error_name != NULL) {
		error = writer_field(&writer, DBUS_FIELD_ERROR_NAME, 's', header->error_name);
		if (error != 0)
			return error;
	}

	/* The call a return or an error answers. */
	if (header->reply_serial != 0) {
		error = writer_field_number(&writer, DBUS_FIELD_REPLY_SERIAL, header->reply_serial);
		if (error != 0)
			return error;
	}

	/* The body's signature, absent with an empty body. */
	if (signature[0] != '\0') {
		error = writer_field(&writer, DBUS_FIELD_SIGNATURE, 'g', signature);
		if (error != 0)
			return error;
	}

	/* The fields' length, which the fixed header declares. */
	fields = writer.used - 16;

	/* Body alignment is outside the header array's declared length. */
	error = writer_align(&writer, 8);
	if (error != 0)
		return error;
	body = writer.used;

	/* Each argument by its type in the signature; only the agreed simple types can be transmitted. */
	index = 0;
	while (*signature != '\0') {
		/* An argument for every complete type of the signature. */
		if (index >= count)
			return EINVAL;
		type = *signature;
		if (type == 's' ||
		    type == 'o' ||
		    type == 'g') {
			error = writer_string(&writer, type, args[index].string);
			signature++;
		} else if (type == 'u' || type == 'b') {
			if (type == 'b' && args[index].number > 1)
				return EINVAL;
			error = writer_number(&writer, args[index].number);
			signature++;
		} else if (type == 'a' && signature[1] == 's') {
			error = writer_strings(&writer, args[index].strings, args[index].string_count);
			signature += 2;
		} else if (type == 'v') {
			error = writer_variant(&writer, &args[index]);
			signature++;
		} else {
			return ENOTSUP;
		}

		/* A refused argument never publishes a truncated message. */
		if (error != 0)
			return error;
		index++;
	}

	/* Every argument given has its type. */
	if (index != count)
		return EINVAL;

	/* The completed frame declares its kind, flags, exact lengths and nonzero serial. */
	message->bytes[0] = 'l';
	message->bytes[1] = (unsigned char)header->type;
	message->bytes[2] = (unsigned char)header->flags;
	message->bytes[3] = 1;
	bus_put32(message->bytes + 4, (uint32_t)(writer.used - body));
	bus_put32(message->bytes + 8, header->serial);
	bus_put32(message->bytes + 12, (uint32_t)fields);
	message->size = writer.used;

	/* Succeeded: this exact frame can be sent without ancillary files. */
	return 0;
}

/* Builds and sends one message that awaits no answer (a return, an error or a signal). */
static int
bus_emit(
	struct linux_dbus *bus,
	struct dbus_header *header,
	const struct dbus_arg *args,
	size_t count)
{
	struct dbus_reply *message;
	uint64_t deadline = 0;
	int error;

	/* A closed bus sends nothing. */
	if (bus->fd < 0)
		return EPIPE;

	/* The message's serial and the deadline of its sending. */
	header->serial = bus_next_serial(bus);
	error = bus_now(&deadline);
	if (error != 0)
		return error;
	deadline += DBUS_TIMEOUT_MS;

	/* An encoder failure sends nothing. */
	message = calloc(1, sizeof(*message));
	if (message == NULL)
		return ENOMEM;
	error = bus_build(message, header, args, count);
	if (error != 0) {
		dbus_reply_free(message);
		return error;
	}

	/* A partial send failure terminates the framing connection. */
	error = bus_send(bus, message->bytes, message->size, deadline);
	dbus_reply_free(message);
	if (error != 0) {
		dbus_close(bus);
		return error;
	}

	/* Succeeded: the message is on the connection. */
	return 0;
}

/* Takes the next serial of the connection's messages; 0 is never one. */
static uint32_t
bus_next_serial(
	struct linux_dbus *bus)
{
	/* The serial grows and skips 0, which the protocol forbids. */
	bus->serial++;
	if (bus->serial == 0)
		bus->serial++;

	/* Succeeded: the serial. */
	return bus->serial;
}

/* Gives the alignment of a value of one type (the type's first character). */
static size_t
bus_alignment(
	char type)
{
	/* Bytes, signatures and variants: none. */
	if (type == 'y' ||
	    type == 'g' ||
	    type == 'v')
		return 1;

	/* Sixteen-bit integers. */
	if (type == 'n' || type == 'q')
		return 2;

	/* Sixty-four-bit values, structures and dictionary entries. */
	if (type == 'x' ||
	    type == 't' ||
	    type == 'd' ||
	    type == '(' ||
	    type == '{')
		return 8;

	/* Thirty-two-bit values, strings, object paths and arrays. */
	return 4;
}

/*
 * Steps over one value of the complete type at *signature, which moves past
 * that type; the containers in it nest at most LINUX_DBUS_DEPTH deep.
 * Returns 0, or EPROTO for a value or a type that is not valid.
 */
static int
bus_skip(
	struct dbus_reply *message,
	const char **signature,
	unsigned depth)
{
	const char *type;
	const char *type_end;
	const char *inner;
	const char *inner_end;
	const char *text;
	size_t inner_length;
	size_t alignment;
	size_t end;
	size_t width;
	int error;

	/* The type, and where it ends in the signature. */
	type = *signature;
	if (depth > LINUX_DBUS_DEPTH)
		return EPROTO;
	type_end = dbus_type_end(type, depth);
	if (type_end == NULL)
		return EPROTO;

	/* Strings, object paths and signatures: validated as they are stepped over. */
	if (*type == 's' || *type == 'o' || *type == 'g') {
		error = reader_string(message, &message->cursor, message->size, *type, &text);
		if (error != 0)
			return error;
		*signature = type_end;
		return 0;
	}

	/* A variant: its own signature, one complete type, then that value. */
	if (*type == 'v') {
		error = reader_string(message, &message->cursor, message->size, 'g', &inner);
		if (error != 0)
			return error;
		inner_end = dbus_type_end(inner, depth + 1);
		inner_length = strlen(inner);
		if (inner_length == 0 || inner_end != inner + inner_length)
			return EPROTO;
		error = bus_skip(message, &inner, depth + 1);
		if (error != 0)
			return error;
		*signature = type_end;
		return 0;
	}

	/* An array: its length says where it ends, its elements are not looked into. */
	if (*type == 'a') {
		alignment = bus_alignment(type[1]);
		error = dbus_read_array(message, alignment, &end);
		if (error != 0)
			return error;
		message->cursor = end;
		*signature = type_end;
		return 0;
	}

	/* A structure or a dictionary entry: each member in turn. */
	if (*type == '(' || *type == '{') {
		error = dbus_align(message, 8);
		if (error != 0)
			return error;
		inner = type + 1;
		while (inner < type_end - 1) {
			error = bus_skip(message, &inner, depth + 1);
			if (error != 0)
				return error;
		}

		/* Past the whole container. */
		*signature = type_end;
		return 0;
	}

	/* A fixed-size value: aligned to its width, which must be in the message. */
	width = bus_alignment(*type);
	if (*type == 'y')
		width = 1;
	error = dbus_align(message, width);
	if (error != 0)
		return error;
	if (message->size - message->cursor < width)
		return EPROTO;
	message->cursor += width;

	/* Succeeded: past the value. */
	*signature = type_end;
	return 0;
}

/* Validates a string against its enclosing header or body boundary. */
static int
reader_string(
	struct dbus_reply *message,
	size_t *cursor,
	size_t end,
	char type,
	const char **string)
{
	size_t offset;
	size_t length;
	size_t index;

	/* Only signature strings have a one-byte length and no four-byte alignment. */
	offset = *cursor;
	if (type == 'g') {
		if (offset >= end)
			return EPROTO;
		length = message->bytes[offset++];
	} else {
		offset = (offset + 3U) & ~(size_t)3U;
		if (offset > end || end - offset < 4)
			return EPROTO;
		length = bus_get32(message->bytes + offset);
		offset += 4;
	}

	/* The terminator must be in this value's declared enclosing range. */
	if (offset >= end || length >= end - offset)
		return EPROTO;
	if (message->bytes[offset + length] != 0)
		return EPROTO;

	/* Embedded NUL bytes cannot truncate a path, member or authority name. */
	for (index = 0; index < length; index++) {
		if (message->bytes[offset + index] == 0)
			return EPROTO;
	}

	/* Succeeded: the returned pointer remains inside the owned message. */
	*string = (const char *)message->bytes + offset;
	*cursor = offset + length + 1;
	return 0;
}

/* Decodes only known header variants and validates received file counts. */
static int
bus_parse(
	struct dbus_reply *message)
{
	const char *variant;
	const char *value;
	size_t cursor;
	size_t end;
	uint32_t number;
	uint32_t seen;
	uint32_t fd_count;
	unsigned code;
	char expected;
	int error;

	/* Framing already established the bounded header-array extent. */
	end = 16U + bus_get32(message->bytes + 12);
	message->body = (end + 7U) & ~(size_t)7U;
	message->cursor = message->body;
	message->type = message->bytes[1];
	message->flags = message->bytes[2];
	message->serial = bus_get32(message->bytes + 8);
	message->signature = "";
	message->error = "unknown";
	seen = 0;
	fd_count = 0;

	/* Header fields are unique structures whose variants must match their codes. */
	cursor = 16;
	while (cursor < end) {
		cursor = (cursor + 7U) & ~(size_t)7U;
		if (cursor >= end)
			return EPROTO;
		code = message->bytes[cursor++];
		if (code == 0 || code > 9)
			return EPROTO;
		if ((seen & (1U << code)) != 0)
			return EPROTO;
		seen |= 1U << code;
		error = reader_string(message, &cursor, end, 'g', &variant);
		if (error != 0)
			return error;

		/* Each known header variant has exactly one simple value. */
		expected = 's';
		if (code == 1)
			expected = 'o';
		if (code == 8)
			expected = 'g';
		if (code == 5 || code == 9)
			expected = 'u';
		if (variant[0] != expected || variant[1] != '\0')
			return EPROTO;

		/* Numeric header fields cannot run into body alignment padding. */
		if (expected == 'u') {
			cursor = (cursor + 3U) & ~(size_t)3U;
			if (cursor > end || end - cursor < 4)
				return EPROTO;
			number = bus_get32(message->bytes + cursor);
			cursor += 4;
			if (code == 5)
				message->reply_serial = number;
			if (code == 9)
				fd_count = number;
		} else {
			/* Text header fields borrow storage from the complete owned frame. */
			error = reader_string(message, &cursor, end, expected, &value);
			if (error != 0)
				return error;
			if (code == 1)
				message->path = value;
			if (code == 2)
				message->interface = value;
			if (code == 3)
				message->member = value;
			if (code == 4)
				message->error = value;
			if (code == 7)
				message->sender = value;
			if (code == 8)
				message->signature = value;
		}
	}

	/* All received rights must have been declared by this exact message. */
	if (fd_count != message->fd_count)
		return EPROTO;
	if (message->type == 2 || message->type == 3) {
		if (message->reply_serial == 0)
			return EPROTO;
	}

	/* A method call names the object and the method it asks for. */
	if (message->type == LINUX_DBUS_METHOD_CALL) {
		if (message->path == NULL || message->member == NULL)
			return EPROTO;
	}

	/* Signals need their complete routing identity before reaching a callback. */
	if (message->type == 4) {
		if (message->path == NULL ||
		    message->interface == NULL ||
		    message->member == NULL)
			return EPROTO;
	}

	/* Succeeded: header pointers and ancillary indices are now safe to consume. */
	return 0;
}

/* Receives only the current frame's remaining bytes and owns all its rights. */
static int
bus_receive(
	struct linux_dbus *bus,
	struct dbus_reply **message)
{
	struct msghdr header;
	struct iovec vector;
	struct cmsghdr *control;
	union {
		struct cmsghdr alignment;
		unsigned char bytes[CMSG_SPACE(sizeof(int) * LINUX_DBUS_FDS)];
	} ancillary;
	struct dbus_reply *frame;
	const int *rights;
	ssize_t received;
	size_t index;
	size_t count;
	size_t fields;
	size_t body;
	size_t control_minimum;
	uint32_t serial;
	int error;
	int malformed;

	/* The first sixteen bytes establish an exact bounded receive target. */
	*message = NULL;
	if (bus->receiving == NULL) {
		bus->receiving = calloc(1, sizeof(*bus->receiving));
		if (bus->receiving == NULL)
			return ENOMEM;
		bus->received = 0;
		bus->wanted = 16;
	}

	/* Partial reads may stop at any byte without losing ancillary ownership. */
	frame = bus->receiving;
	control_minimum = CMSG_LEN(0);
	for (;;) {
		memset(&header, 0, sizeof(header));
		vector.iov_base = frame->bytes + bus->received;
		vector.iov_len = bus->wanted - bus->received;
		header.msg_iov = &vector;
		header.msg_iovlen = 1;
		header.msg_control = ancillary.bytes;
		header.msg_controllen = sizeof(ancillary.bytes);
		received = recvmsg(bus->fd, &header, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
		if (received < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return EAGAIN;
			return errno;
		}

		/* Every delivered right is either retained or immediately closed on overflow. */
		malformed = 0;
		control = CMSG_FIRSTHDR(&header);
		while (control != NULL) {
			if (control->cmsg_level != SOL_SOCKET ||
			    control->cmsg_type != SCM_RIGHTS ||
			    control->cmsg_len < control_minimum) {
				malformed = 1;
			} else {
				/* Kernel-delivered descriptors are CLOEXEC before any callback can spawn. */
				count = (control->cmsg_len - CMSG_LEN(0)) / sizeof(int);
				rights = (const int *)CMSG_DATA(control);

				/* Overflow never leaves an untracked descriptor in the process. */
				for (index = 0; index < count; index++) {
					if (frame->fd_count >= LINUX_DBUS_FDS) {
						(void)close(rights[index]);
						malformed = 1;
					} else {
						frame->fds[frame->fd_count++] = rights[index];
					}
				}
			}

			/* Advances only through ancillary records validated by the kernel. */
			control = CMSG_NXTHDR(&header, control);
		}

		/* Truncation or unknown control data invalidates the complete connection. */
		if ((header.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) != 0 || malformed)
			return EPROTO;
		if (received == 0)
			return EPIPE;
		bus->received += (size_t)received;
		if (bus->received != bus->wanted)
			continue;

		/* Little-endian version-one messages are the agreed bounded vocabulary. */
		if (bus->wanted == 16) {
			if (frame->bytes[0] != 'l' || frame->bytes[3] != 1)
				return EPROTO;
			if (frame->bytes[1] < 1 || frame->bytes[1] > 4)
				return EPROTO;
			if ((frame->bytes[2] & ~7U) != 0)
				return EPROTO;
			serial = bus_get32(frame->bytes + 8);
			if (serial == 0)
				return EPROTO;
			fields = bus_get32(frame->bytes + 12);
			body = bus_get32(frame->bytes + 4);
			if (fields > LINUX_DBUS_MAX - 16)
				return EOVERFLOW;
			bus->wanted = (16 + fields + 7U) & ~(size_t)7U;
			if (bus->wanted > LINUX_DBUS_MAX || body > LINUX_DBUS_MAX - bus->wanted)
				return EOVERFLOW;
			bus->wanted += body;
			if (bus->wanted != bus->received)
				continue;
		}

		/* Only a complete frame exposes routing pointers or transfers its rights. */
		frame->size = bus->received;
		error = bus_parse(frame);
		if (error != 0)
			return error;
		bus->receiving = NULL;
		bus->received = 0;
		bus->wanted = 0;
		*message = frame;

		/* Succeeded: the caller owns this one frame, never bytes from its successor. */
		return 0;
	}
}

/* Retains one signal without replacing or dropping earlier device transitions. */
static int
bus_queue(
	struct linux_dbus *bus,
	struct dbus_reply *message)
{
	/* Queue overflow is a fatal protocol condition rather than a lost pause. */
	if (bus->signal_count >= LINUX_DBUS_SIGNALS)
		return EOVERFLOW;
	bus->signals[bus->signal_count++] = message;

	/* Succeeded: the main loop now owns this signal in arrival order. */
	return 0;
}
