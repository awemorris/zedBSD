/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The notifications of the Linux desktop's programs (ws156-p006,
 * plan/ws156/phase001/phase.md section 11): the backend serves
 * org.freedesktop.Notifications (the Desktop Notifications Specification
 * 1.2) on the user's session bus and hands what GTK, Qt and libnotify
 * programs post to the compositor's own notifications.
 *
 *   GetCapabilities() -> as            body, actions, persistence
 *   Notify(s app, u replaces, s icon, s summary, s body, as actions,
 *          a{sv} hints, i timeout) -> u id
 *   CloseNotification(u id)            the sender's own; NotificationClosed(id, 3)
 *   GetServerInformation() -> ssss     Keiland, zedBSD, its version, 1.2
 *   NotificationClosed(u id, u reason) and ActionInvoked(u id, s key) signals
 *
 * The body is plain text (body-markup is not offered): simple tags a
 * program sends anyway are taken out.  Only the "default" action is kept
 * (a click of the body); the icon, the images and the timeout are not used,
 * since the compositor's popup has its own course.  Text longer than the
 * compositor keeps is cut at a character's boundary rather than refused,
 * as a program on the bus does not expect a refusal.  Every peer is a
 * program of the same user, but its frames are read with every bound
 * checked.
 */

#include "userland/desktop/libkeiland-backend/backend-private.h"
#include "dbus-linux.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The name, the object and the interface the specification fixes. */
#define NOTIFY_NAME		"org.freedesktop.Notifications"
#define NOTIFY_PATH		"/org/freedesktop/Notifications"
#define NOTIFY_INTERFACE	"org.freedesktop.Notifications"

/* The standard interfaces every object answers. */
#define NOTIFY_INTROSPECTABLE	"org.freedesktop.DBus.Introspectable"
#define NOTIFY_PEER		"org.freedesktop.DBus.Peer"

/* What GetServerInformation says: the server, its vendor, its version, and the specification's. */
#define NOTIFY_SERVER_NAME	"Keiland"
#define NOTIFY_SERVER_VENDOR	"zedBSD"
#define NOTIFY_SERVER_VERSION	"1.0"
#define NOTIFY_SPEC_VERSION	"1.2"

/* RequestName's flag that refuses the queue, and its answers of owning the name. */
#define NOTIFY_DO_NOT_QUEUE	4U
#define NOTIFY_PRIMARY_OWNER	1U
#define NOTIFY_ALREADY_OWNER	4U

/* A hint's urgency that is critical. */
#define NOTIFY_URGENCY_CRITICAL	2U

/* The longest markup tag taken out of a body, in bytes (a link's tag carries its address). */
#define NOTIFY_TAG_MAX		256U

/* How many senders are remembered, and the longest unique name (the specification's 255). */
#define NOTIFY_SENDERS		64U
#define NOTIFY_SENDER_NAME	256U

/*
 * The introspection of the notifications' object: what gdbus and other
 * tools read before they call a method.
 */
#define NOTIFY_INTROSPECTION \
	"<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n" \
	" \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n" \
	"<node>\n" \
	" <interface name=\"org.freedesktop.Notifications\">\n" \
	"  <method name=\"GetCapabilities\">\n" \
	"   <arg direction=\"out\" name=\"capabilities\" type=\"as\"/>\n" \
	"  </method>\n" \
	"  <method name=\"Notify\">\n" \
	"   <arg direction=\"in\" name=\"app_name\" type=\"s\"/>\n" \
	"   <arg direction=\"in\" name=\"replaces_id\" type=\"u\"/>\n" \
	"   <arg direction=\"in\" name=\"app_icon\" type=\"s\"/>\n" \
	"   <arg direction=\"in\" name=\"summary\" type=\"s\"/>\n" \
	"   <arg direction=\"in\" name=\"body\" type=\"s\"/>\n" \
	"   <arg direction=\"in\" name=\"actions\" type=\"as\"/>\n" \
	"   <arg direction=\"in\" name=\"hints\" type=\"a{sv}\"/>\n" \
	"   <arg direction=\"in\" name=\"expire_timeout\" type=\"i\"/>\n" \
	"   <arg direction=\"out\" name=\"id\" type=\"u\"/>\n" \
	"  </method>\n" \
	"  <method name=\"CloseNotification\">\n" \
	"   <arg direction=\"in\" name=\"id\" type=\"u\"/>\n" \
	"  </method>\n" \
	"  <method name=\"GetServerInformation\">\n" \
	"   <arg direction=\"out\" name=\"name\" type=\"s\"/>\n" \
	"   <arg direction=\"out\" name=\"vendor\" type=\"s\"/>\n" \
	"   <arg direction=\"out\" name=\"version\" type=\"s\"/>\n" \
	"   <arg direction=\"out\" name=\"spec_version\" type=\"s\"/>\n" \
	"  </method>\n" \
	"  <signal name=\"NotificationClosed\">\n" \
	"   <arg name=\"id\" type=\"u\"/>\n" \
	"   <arg name=\"reason\" type=\"u\"/>\n" \
	"  </signal>\n" \
	"  <signal name=\"ActionInvoked\">\n" \
	"   <arg name=\"id\" type=\"u\"/>\n" \
	"   <arg name=\"action_key\" type=\"s\"/>\n" \
	"  </signal>\n" \
	" </interface>\n" \
	" <interface name=\"org.freedesktop.DBus.Introspectable\">\n" \
	"  <method name=\"Introspect\">\n" \
	"   <arg direction=\"out\" name=\"xml_data\" type=\"s\"/>\n" \
	"  </method>\n" \
	" </interface>\n" \
	" <interface name=\"org.freedesktop.DBus.Peer\">\n" \
	"  <method name=\"Ping\"/>\n" \
	" </interface>\n" \
	"</node>\n"

/*
 * One sender the backend has numbered: its unique bus name and its number
 * (0: a free entry), and when it last posted (to replace the least recent
 * when all are taken).  A unique name is never given again by the bus, so a
 * sender forgotten and seen again only starts a new number.
 */
struct notify_sender {
	char name[NOTIFY_SENDER_NAME];
	uint32_t number;
	uint64_t used;
};

/*
 * Serving the bus's notifications: the session bus's connection, the
 * compositor's callbacks and their data, and the senders numbered.  It
 * lives from kl_backend_notify_open to kl_backend_notify_close, as the
 * backend's notify; a bus that broke stays closed (no descriptor polled)
 * until then.
 */
struct kl_backend_notify {
	struct linux_dbus bus;
	struct kl_backend_notify_callbacks callbacks;
	void *data;
	struct notify_sender senders[NOTIFY_SENDERS];
	uint32_t next_sender;
	uint64_t clock;
};

/* What carries out one method call and answers it: 0, or the error of answering. */
typedef int (*notify_handler_fn)(struct kl_backend_notify *notify, struct dbus_reply *message);

/*
 * One method this end answers: its member and interface, whether it is
 * answered only on the notifications' object (Ping and Introspect are
 * answered on any path), and its handler.
 */
struct notify_method_entry {
	const char *member;
	const char *interface;
	int own_object;
	notify_handler_fn handler;
};

static int notify_ping(struct kl_backend_notify *notify, struct dbus_reply *message);
static int notify_post(struct kl_backend_notify *notify, struct dbus_reply *message);
static int notify_close_one(struct kl_backend_notify *notify, struct dbus_reply *message);
static int notify_capabilities(struct kl_backend_notify *notify, struct dbus_reply *message);
static int notify_information(struct kl_backend_notify *notify, struct dbus_reply *message);
static int notify_introspect(struct kl_backend_notify *notify, struct dbus_reply *message);

/*
 * The methods answered: the notifications' own on their object, and the
 * standard Ping and Introspect.  A constant table, fixed at compile time.
 */
static const struct notify_method_entry notify_methods[] = {
	{ "Notify", NOTIFY_INTERFACE, 1, notify_post },
	{ "CloseNotification", NOTIFY_INTERFACE, 1, notify_close_one },
	{ "GetCapabilities", NOTIFY_INTERFACE, 1, notify_capabilities },
	{ "GetServerInformation", NOTIFY_INTERFACE, 1, notify_information },
	{ "Ping", NOTIFY_PEER, 0, notify_ping },
	{ "Introspect", NOTIFY_INTROSPECTABLE, 0, notify_introspect },
};

static int notify_request_name(struct kl_backend_notify *notify);
static int notify_message(struct dbus_reply *message, void *data);
static int notify_method(struct kl_backend_notify *notify, struct dbus_reply *message);
static int notify_refuse(struct kl_backend_notify *notify, struct dbus_reply *message, const char *name, const char *text);
static int notify_read_actions(struct dbus_reply *message, unsigned *flags);
static int notify_read_hints(struct dbus_reply *message, unsigned *flags);
static int notify_is(const char *text, const char *expected);
static uint32_t notify_sender_number(struct kl_backend_notify *notify, const char *name);
static void notify_copy(char *out, size_t capacity, const char *text, int strip_tags);
static size_t notify_tag_length(const unsigned char *text);
static size_t notify_character_length(const unsigned char *bytes);
static void notify_lost(struct kl_backend_notify *notify, int error);

/*
 * Serves the bus's notifications on the user's session bus.
 */
int
kl_backend_notify_open(
	struct kl_backend *backend,
	const struct kl_backend_notify_callbacks *callbacks,
	void *data)
{
	struct kl_backend_notify *notify;
	int error;

	/* Served already. */
	if (backend->notify != NULL)
		return 0;

	/* The state of serving them. */
	notify = calloc(1, sizeof(*notify));
	if (notify == NULL)
		return ENOMEM;
	notify->callbacks = *callbacks;
	notify->data = data;
	notify->next_sender = 1U;

	/* The session bus, authenticated as this user. */
	error = dbus_open_session(&notify->bus);
	if (error != 0) {
		dbus_close(&notify->bus);
		free(notify);
		return error;
	}

	/* The well-known name, unless another program has it. */
	error = notify_request_name(notify);
	if (error != 0) {
		dbus_close(&notify->bus);
		free(notify);
		return error;
	}

	/* Succeeded: the bus is part of the backend's poll from now on. */
	backend->notify = notify;
	fprintf(stderr, "wayland: notify: serving %s on the session bus\n", NOTIFY_NAME);
	return 0;
}

/*
 * Tells the notification's sender that it went, and why.
 */
void
kl_backend_notify_closed(
	struct kl_backend *backend,
	uint32_t id,
	unsigned reason)
{
	struct kl_backend_notify *notify;
	struct dbus_arg args[2];
	int error;

	/* Nothing is told while the notifications are not served. */
	notify = backend->notify;
	if (notify == NULL || notify->bus.fd < 0)
		return;

	/* NotificationClosed(id, reason) to every listener. */
	memset(args, 0, sizeof(args));
	args[0].number = id;
	args[1].number = reason;
	error = dbus_send_signal(&notify->bus, NOTIFY_PATH, NOTIFY_INTERFACE, "NotificationClosed", "uu", args, 2);
	if (error != 0)
		notify_lost(notify, error);
}

/*
 * Tells the notification's sender that its body was clicked.
 */
void
kl_backend_notify_activated(
	struct kl_backend *backend,
	uint32_t id)
{
	struct kl_backend_notify *notify;
	struct dbus_arg args[2];
	int error;

	/* Nothing is told while the notifications are not served. */
	notify = backend->notify;
	if (notify == NULL || notify->bus.fd < 0)
		return;

	/* ActionInvoked(id, "default"): the only action kept. */
	memset(args, 0, sizeof(args));
	args[0].number = id;
	args[1].string = "default";
	error = dbus_send_signal(&notify->bus, NOTIFY_PATH, NOTIFY_INTERFACE, "ActionInvoked", "us", args, 2);
	if (error != 0)
		notify_lost(notify, error);
}

/*
 * Stops serving the bus's notifications.
 */
void
kl_backend_notify_close(
	struct kl_backend *backend)
{
	struct kl_backend_notify *notify;

	/* Nothing is served. */
	notify = backend->notify;
	if (notify == NULL)
		return;

	/* Closing the connection gives the name up. */
	dbus_close(&notify->bus);
	free(notify);
	backend->notify = NULL;
}

/*
 * Counts the bus's descriptor in the poll: one while the bus is open.
 */
size_t
kl_backend_notify_poll_count(
	const struct kl_backend *backend)
{
	/* Not served, or the bus broke. */
	if (backend->notify == NULL || backend->notify->bus.fd < 0)
		return 0;

	/* Succeeded: the bus's socket. */
	return 1;
}

/*
 * Fills the bus's descriptor, waiting for its frames.
 */
void
kl_backend_notify_poll_fill(
	struct kl_backend *backend,
	struct pollfd *descriptors)
{
	/* Not served, or the bus broke. */
	if (backend->notify == NULL || backend->notify->bus.fd < 0)
		return;

	/* The bus's socket, read when a frame comes. */
	descriptors[0].fd = backend->notify->bus.fd;
	descriptors[0].events = POLLIN;
	descriptors[0].revents = 0;
}

/*
 * Handles the bus's frames: the calls answered, the notifications passed to
 * the compositor.
 */
void
kl_backend_notify_poll_done(
	struct kl_backend *backend,
	const struct pollfd *descriptors)
{
	struct kl_backend_notify *notify;
	int error;

	/* Not served, or the bus broke. */
	notify = backend->notify;
	if (notify == NULL || notify->bus.fd < 0)
		return;

	/* Nothing came, and nothing waits from the open's calls. */
	if (descriptors[0].fd != notify->bus.fd)
		return;
	if (descriptors[0].revents == 0 && notify->bus.signal_count == 0)
		return;

	/* Each message that came, within the pass's bound. */
	error = dbus_dispatch_messages(&notify->bus, notify_message, notify);
	if (error != 0)
		notify_lost(notify, error);
}

/* Asks the bus for the notifications' name, without queueing behind another owner. */
static int
notify_request_name(
	struct kl_backend_notify *notify)
{
	struct dbus_arg args[2];
	struct dbus_reply *reply;
	uint32_t answer;
	int error;

	/* RequestName(name, DO_NOT_QUEUE). */
	memset(args, 0, sizeof(args));
	args[0].string = NOTIFY_NAME;
	args[1].number = NOTIFY_DO_NOT_QUEUE;
	error = dbus_call(&notify->bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName", "su", args, 2, &reply);
	if (error != 0)
		return error;

	/* Its answer: this connection owns the name, or another program does. */
	error = dbus_read_number(reply, &answer);
	dbus_reply_free(reply);
	if (error != 0)
		return error;
	if (answer != NOTIFY_PRIMARY_OWNER && answer != NOTIFY_ALREADY_OWNER) {
		fprintf(stderr, "wayland: notify: %s is served by another program (answer %u)\n", NOTIFY_NAME, answer);
		return EEXIST;
	}

	/* Succeeded: the name is this connection's. */
	return 0;
}

/* Takes one message from the bus: a method call is answered, a signal (the bus's own) is not looked at. */
static int
notify_message(
	struct dbus_reply *message,
	void *data)
{
	struct kl_backend_notify *notify;
	int error;

	/* The bus's signals (NameAcquired and the like) need nothing. */
	notify = data;
	if (message->type != LINUX_DBUS_METHOD_CALL)
		return 0;

	/* A call, answered. */
	error = notify_method(notify, message);
	if (error != 0)
		return error;

	/* Succeeded: answered. */
	return 0;
}

/*
 * Answers one method call by its interface and member.  Returns 0, or the
 * error of answering (the bus is closed then).
 */
static int
notify_method(
	struct kl_backend_notify *notify,
	struct dbus_reply *message)
{
	notify_handler_fn handler;
	const char *interface;
	size_t index;
	int on_object;
	int on_interface;
	int same;
	int error;

	/* The method asked for, by its member, its interface (which a call may leave out) and its object. */
	handler = NULL;
	for (index = 0; index < sizeof(notify_methods) / sizeof(notify_methods[0]); index++) {
		/* The member's name. */
		same = notify_is(message->member, notify_methods[index].member);
		if (!same)
			continue;

		/* The interface, when the call names one. */
		interface = message->interface;
		on_interface = 1;
		if (interface != NULL)
			on_interface = notify_is(interface, notify_methods[index].interface);

		/* The object: the notifications' own, unless the method answers on any. */
		on_object = 1;
		if (notify_methods[index].own_object)
			on_object = notify_is(message->path, NOTIFY_PATH);

		/* The method this is. */
		if (on_interface && on_object) {
			handler = notify_methods[index].handler;
			break;
		}
	}

	/* Anything else is not a method this end has. */
	if (handler == NULL) {
		error = notify_refuse(notify, message, "org.freedesktop.DBus.Error.UnknownMethod", "No such method");
		return error;
	}

	/* The method, carried out and answered. */
	error = handler(notify, message);
	if (error != 0)
		return error;

	/* Succeeded: answered. */
	return 0;
}

/* Carries out Ping: the empty answer. */
static int
notify_ping(
	struct kl_backend_notify *notify,
	struct dbus_reply *message)
{
	int error;

	/* The answer itself is the reply. */
	error = dbus_send_return(&notify->bus, message, "", NULL, 0);
	if (error != 0)
		return error;

	/* Succeeded: answered. */
	return 0;
}

/*
 * Answers a call with an error of a name and a text: the call is refused,
 * the bus stays.  Returns 0, or the error of sending (the bus is closed
 * then).
 */
static int
notify_refuse(
	struct kl_backend_notify *notify,
	struct dbus_reply *message,
	const char *name,
	const char *text)
{
	int error;

	/* The error, as the call's answer. */
	error = dbus_send_error(&notify->bus, message, name, text);
	if (error != 0)
		return error;

	/* Succeeded: refused. */
	return 0;
}

/*
 * Carries out Notify: reads its arguments, hands the notification to the
 * compositor and answers with its number (or an error the compositor's
 * refusal names).
 */
static int
notify_post(
	struct kl_backend_notify *notify,
	struct dbus_reply *message)
{
	struct dbus_arg argument;
	char app[KL_BACKEND_NOTIFY_APP_MAX + 1U];
	char title[KL_BACKEND_NOTIFY_TITLE_MAX + 1U];
	char body[KL_BACKEND_NOTIFY_BODY_MAX + 1U];
	const char *app_name;
	const char *icon;
	const char *summary;
	const char *text;
	const char *refusal;
	uint32_t replaces;
	uint32_t timeout;
	uint32_t sender;
	uint32_t id;
	unsigned flags;
	int refused;
	int error;
	int same;

	/* The arguments must be the specification's. */
	same = notify_is(message->signature, "susssasa{sv}i");
	if (!same) {
		error = notify_refuse(notify, message, "org.freedesktop.DBus.Error.InvalidArgs", "Notify takes susssasa{sv}i");
		return error;
	}

	/* The application, the notification it replaces, the icon (not used), the summary and the body. */
	error = dbus_read_string(message, &app_name);
	if (error == 0)
		error = dbus_read_number(message, &replaces);
	if (error == 0)
		error = dbus_read_string(message, &icon);
	if (error == 0)
		error = dbus_read_string(message, &summary);
	if (error == 0)
		error = dbus_read_string(message, &text);

	/* The actions (only "default" is kept) and the hints (only the urgency). */
	flags = 0;
	if (error == 0)
		error = notify_read_actions(message, &flags);
	if (error == 0)
		error = notify_read_hints(message, &flags);

	/* The timeout, which the popup's own course replaces. */
	if (error == 0)
		error = dbus_read_number(message, &timeout);

	/* A body that does not hold its signature's values is refused. */
	if (error != 0) {
		error = notify_refuse(notify, message, "org.freedesktop.DBus.Error.InvalidArgs", "Malformed Notify arguments");
		return error;
	}

	/* The words, cut to what the compositor keeps; the body as plain text. */
	notify_copy(app, sizeof(app), app_name, 0);
	notify_copy(title, sizeof(title), summary, 0);
	notify_copy(body, sizeof(body), text, 1);

	/* The sender's own number, by its unique name. */
	sender = notify_sender_number(notify, message->sender);

	/* Handed to the compositor. */
	refused = 0;
	id = notify->callbacks.posted(notify->data, sender, app, title, body, replaces, flags, &refused);
	if (id == 0U) {
		/* The refusal's name, by the compositor's reason. */
		if (refused == EBUSY) {
			refusal = "org.freedesktop.DBus.Error.LimitsExceeded";
		} else if (refused == EACCES) {
			refusal = "org.freedesktop.DBus.Error.AccessDenied";
		} else {
			refusal = "org.freedesktop.DBus.Error.Failed";
		}

		/* The refusal, logged and answered. */
		fprintf(stderr, "wayland: notify: refused sender=%u error=%d\n", sender, refused);
		error = notify_refuse(notify, message, refusal, "The notification was not taken");
		return error;
	}

	/* Its number, the answer. */
	memset(&argument, 0, sizeof(argument));
	argument.number = id;
	error = dbus_send_return(&notify->bus, message, "u", &argument, 1);
	if (error != 0)
		return error;

	/* Succeeded: posted and answered. */
	return 0;
}

/* Reads the actions (as, key and label in turn): a "default" key gives the notification its click. */
static int
notify_read_actions(
	struct dbus_reply *message,
	unsigned *flags)
{
	const char *entry;
	size_t end;
	size_t index;
	int error;
	int is_default;

	/* The array of strings. */
	error = dbus_read_array(message, 4, &end);
	if (error != 0)
		return error;

	/* Each entry, the even ones being keys. */
	index = 0;
	while (message->cursor < end) {
		error = dbus_read_string(message, &entry);
		if (error != 0)
			return error;

		/* The key that stands for a click of the body. */
		is_default = notify_is(entry, "default");
		if (index % 2U == 0U && is_default)
			*flags |= KL_BACKEND_NOTIFY_ACTION;
		index++;
	}

	/* The elements must end where the array said. */
	if (message->cursor != end)
		return EPROTO;

	/* Succeeded: past the actions. */
	return 0;
}

/* Reads the hints (a{sv}): an urgency byte of 2 makes the notification urgent; the rest are stepped over. */
static int
notify_read_hints(
	struct dbus_reply *message,
	unsigned *flags)
{
	const char *key;
	const char *signature;
	const char *type_end;
	size_t end;
	uint8_t urgency;
	int error;
	int is_urgency;
	int is_byte;

	/* The array of dictionary entries. */
	error = dbus_read_array(message, 8, &end);
	if (error != 0)
		return error;

	/* Each entry: its key, then its variant's type and value. */
	while (message->cursor < end) {
		error = dbus_align(message, 8);
		if (error != 0)
			return error;
		error = dbus_read_string(message, &key);
		if (error != 0)
			return error;
		error = dbus_read_signature(message, &signature);
		if (error != 0)
			return error;

		/* A variant holds exactly one complete type. */
		type_end = dbus_type_end(signature, 1);
		if (signature[0] == '\0' || type_end == NULL || *type_end != '\0')
			return EPROTO;

		/* The urgency, as a byte; any other hint is stepped over. */
		is_urgency = notify_is(key, "urgency");
		is_byte = notify_is(signature, "y");
		if (is_urgency && is_byte) {
			error = dbus_read_byte(message, &urgency);
			if (error != 0)
				return error;
			if (urgency == NOTIFY_URGENCY_CRITICAL)
				*flags |= KL_BACKEND_NOTIFY_URGENT;
		} else {
			error = dbus_skip_value(message, &signature);
			if (error != 0)
				return error;
		}
	}

	/* The entries must end where the array said. */
	if (message->cursor != end)
		return EPROTO;

	/* Succeeded: past the hints. */
	return 0;
}

/* Carries out CloseNotification: the sender's own notification is taken back. */
static int
notify_close_one(
	struct kl_backend_notify *notify,
	struct dbus_reply *message)
{
	uint32_t sender;
	uint32_t id;
	int error;
	int same;

	/* One unsigned, the notification's number. */
	same = notify_is(message->signature, "u");
	if (!same) {
		error = notify_refuse(notify, message, "org.freedesktop.DBus.Error.InvalidArgs", "CloseNotification takes u");
		return error;
	}

	/* The number itself. */
	error = dbus_read_number(message, &id);
	if (error != 0) {
		error = notify_refuse(notify, message, "org.freedesktop.DBus.Error.InvalidArgs", "Malformed CloseNotification argument");
		return error;
	}

	/*
	 * Taken back when it is the sender's; the compositor then tells
	 * NotificationClosed(id, 3).  One that is gone already, or another's,
	 * is answered the same, as the specification has it.
	 */
	sender = notify_sender_number(notify, message->sender);
	(void)notify->callbacks.withdrawn(notify->data, sender, id);

	/* The empty answer. */
	error = dbus_send_return(&notify->bus, message, "", NULL, 0);
	if (error != 0)
		return error;

	/* Succeeded: answered. */
	return 0;
}

/* Carries out GetCapabilities: a body, actions, and the persistence of the log. */
static int
notify_capabilities(
	struct kl_backend_notify *notify,
	struct dbus_reply *message)
{
	static const char *const capabilities[] = {
		"body",
		"actions",
		"persistence",
	};
	struct dbus_arg argument;
	int error;

	/* The list. */
	memset(&argument, 0, sizeof(argument));
	argument.strings = capabilities;
	argument.string_count = sizeof(capabilities) / sizeof(capabilities[0]);
	error = dbus_send_return(&notify->bus, message, "as", &argument, 1);
	if (error != 0)
		return error;

	/* Succeeded: answered. */
	return 0;
}

/* Carries out GetServerInformation. */
static int
notify_information(
	struct kl_backend_notify *notify,
	struct dbus_reply *message)
{
	struct dbus_arg args[4];
	int error;

	/* The server, its vendor, its version and the specification's. */
	memset(args, 0, sizeof(args));
	args[0].string = NOTIFY_SERVER_NAME;
	args[1].string = NOTIFY_SERVER_VENDOR;
	args[2].string = NOTIFY_SERVER_VERSION;
	args[3].string = NOTIFY_SPEC_VERSION;
	error = dbus_send_return(&notify->bus, message, "ssss", args, 4);
	if (error != 0)
		return error;

	/* Succeeded: answered. */
	return 0;
}

/*
 * Carries out Introspect: the notifications' object describes itself, and
 * each path above it names the one child that leads there.
 */
static int
notify_introspect(
	struct kl_backend_notify *notify,
	struct dbus_reply *message)
{
	struct dbus_arg argument;
	const char *child;
	char xml[256];
	int is_root;
	int is_org;
	int is_freedesktop;
	int is_object;
	int status;
	int error;

	/* Which path is asked about. */
	is_root = notify_is(message->path, "/");
	is_org = notify_is(message->path, "/org");
	is_freedesktop = notify_is(message->path, "/org/freedesktop");
	is_object = notify_is(message->path, NOTIFY_PATH);

	/* The path's child on the way to the object, none for the object itself. */
	child = NULL;
	if (is_root) {
		child = "org";
	} else if (is_org) {
		child = "freedesktop";
	} else if (is_freedesktop) {
		child = "Notifications";
	}

	/* The description: the object's own, or a node naming its child. */
	memset(&argument, 0, sizeof(argument));
	if (is_object) {
		argument.string = NOTIFY_INTROSPECTION;
	} else if (child != NULL) {
		status = snprintf(xml, sizeof(xml), "<node>\n <node name=\"%s\"/>\n</node>\n", child);
		if (status < 0 || (size_t)status >= sizeof(xml))
			return EOVERFLOW;
		argument.string = xml;
	}

	/* Any other path has no object. */
	if (argument.string == NULL) {
		error = notify_refuse(notify, message, "org.freedesktop.DBus.Error.UnknownObject", "No such object");
		return error;
	}

	/* The description, as the answer. */
	error = dbus_send_return(&notify->bus, message, "s", &argument, 1);
	if (error != 0)
		return error;

	/* Succeeded: answered. */
	return 0;
}

/* Tells whether a text (NULL being none) is the expected one. */
static int
notify_is(
	const char *text,
	const char *expected)
{
	int order;

	/* No text is none of them. */
	if (text == NULL)
		return 0;

	/* The same characters. */
	order = strcmp(text, expected);
	if (order != 0)
		return 0;

	/* Succeeded: the same. */
	return 1;
}

/*
 * Gives a sender's number by its unique bus name (NULL or empty for a
 * connection without one): the one given before, or a new one.  When every
 * entry is taken, the least recent sender is forgotten.
 */
static uint32_t
notify_sender_number(
	struct kl_backend_notify *notify,
	const char *name)
{
	struct notify_sender *entry;
	struct notify_sender *oldest;
	size_t index;
	size_t length;
	int order;

	/* A connection without a name stands as one sender of its own. */
	if (name == NULL)
		name = "";
	notify->clock++;

	/* The sender's entry, else the free or least recent one. */
	oldest = &notify->senders[0];
	for (index = 0; index < NOTIFY_SENDERS; index++) {
		entry = &notify->senders[index];

		/* Seen before: its number. */
		if (entry->number != 0U) {
			order = strcmp(entry->name, name);
			if (order == 0) {
				entry->used = notify->clock;
				return entry->number;
			}
		}

		/* A free entry is taken first, else the one used least recently. */
		if (entry->number == 0U) {
			if (oldest->number != 0U)
				oldest = entry;
		} else if (oldest->number != 0U && entry->used < oldest->used) {
			oldest = entry;
		}
	}

	/* A new sender: the next number (never 0) in the entry taken. */
	length = strlen(name);
	if (length >= sizeof(oldest->name))
		length = sizeof(oldest->name) - 1U;
	memcpy(oldest->name, name, length);
	oldest->name[length] = '\0';
	oldest->number = notify->next_sender;
	oldest->used = notify->clock;
	notify->next_sender++;
	if (notify->next_sender == 0U)
		notify->next_sender = 1U;

	/* Succeeded: its number. */
	return oldest->number;
}

/*
 * Copies text into out (capacity bytes with the NUL), cut before a
 * character that does not fit.  With strip_tags, a markup tag (<b>, </i>,
 * <a href="...">, see notify_tag_length) is left out, as the body is shown
 * as plain text.
 */
static void
notify_copy(
	char *out,
	size_t capacity,
	const char *text,
	int strip_tags)
{
	const unsigned char *bytes;
	size_t used;
	size_t at;
	size_t step;
	size_t tag;

	/* Each character of the text. */
	bytes = (const unsigned char *)text;
	used = 0;
	at = 0;
	while (bytes[at] != '\0') {
		/* A tag is left out whole; any other '<' stays. */
		if (strip_tags && bytes[at] == '<') {
			tag = notify_tag_length(bytes + at);
			if (tag != 0) {
				at += tag;
				continue;
			}
		}

		/* The character's bytes, which must all fit with the NUL. */
		step = notify_character_length(bytes + at);
		if (used + step + 1U > capacity)
			break;
		memcpy(out + used, bytes + at, step);
		used += step;
		at += step;
	}

	/* Ended. */
	out[used] = '\0';
}

/*
 * Gives the length of the markup tag at text ('<', an optional '/', a
 * letter, then anything but '<' or a line's end up to '>', at most
 * NOTIFY_TAG_MAX bytes), or 0 when text does not start one: a '<' in plain
 * words ("a < b") is kept.
 */
static size_t
notify_tag_length(
	const unsigned char *text)
{
	size_t at;
	int letter;

	/* The opening, and the slash of a closing tag. */
	at = 1;
	if (text[at] == '/')
		at++;

	/* A tag's name starts with a letter. */
	letter = 0;
	if (text[at] >= 'a' && text[at] <= 'z')
		letter = 1;
	if (text[at] >= 'A' && text[at] <= 'Z')
		letter = 1;
	if (!letter)
		return 0;

	/* Up to its '>', on one line and not too long. */
	while (at < NOTIFY_TAG_MAX) {
		if (text[at] == '\0' || text[at] == '<' || text[at] == '\n')
			return 0;
		if (text[at] == '>')
			return at + 1U;
		at++;
	}

	/* Too long to be a tag. */
	return 0;
}

/*
 * Gives how many bytes the character at bytes takes: its UTF-8 sequence,
 * or 1 for a byte that does not start a whole one (the compositor shows
 * such a byte as a replacement character).
 */
static size_t
notify_character_length(
	const unsigned char *bytes)
{
	size_t length;
	size_t index;

	/* The length the first byte announces. */
	if (bytes[0] < 0x80U) {
		length = 1;
	} else if (bytes[0] >= 0xc2U && bytes[0] <= 0xdfU) {
		length = 2;
	} else if (bytes[0] >= 0xe0U && bytes[0] <= 0xefU) {
		length = 3;
	} else if (bytes[0] >= 0xf0U && bytes[0] <= 0xf4U) {
		length = 4;
	} else {
		return 1;
	}

	/* Each byte after the first must continue it (a NUL ends the text first). */
	for (index = 1; index < length; index++) {
		if ((bytes[index] & 0xc0U) != 0x80U)
			return 1;
	}

	/* Succeeded: the whole sequence. */
	return length;
}

/* Notes that the bus broke: its descriptor leaves the poll, and the notifications are no longer served. */
static void
notify_lost(
	struct kl_backend_notify *notify,
	int error)
{
	/* The connection, closed when it is not already. */
	fprintf(stderr, "wayland: notify: the session bus failed (error %d); notifications from it stop\n", error);
	dbus_close(&notify->bus);
}
