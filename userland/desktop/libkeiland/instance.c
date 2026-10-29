/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * One copy of a program, and the activation.
 *
 * The copy that runs listens on $XDG_RUNTIME_DIR/keiland-NAME.instance.  A
 * later start connects, writes its request and its activation token, a
 * line each, and closes; the copy that runs takes them in its loop
 * (kl_instance_take).  The runtime directory must be the user's own and
 * closed to everybody else, so only the user's programs reach the socket;
 * without such a directory every start runs on its own.
 *
 * A socket left by a copy that ended without closing it refuses the
 * connection; it is removed (when it is the user's socket) and the new
 * start becomes the one copy.
 *
 * The activation is the compositor's xdg_activation_v1: a token asked for
 * on a queue of the library's own (so no event of the program's runs in
 * the wait), and an activation with one.
 */

#include <keiland/keiland.h>

#include "ui/internal.h"
#include "ui/window.h"

#include <wayland-client.h>
#include "userland/desktop/libwayland/xdg-activation-v1-client-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* The longest name of a program's copy. */
#define INSTANCE_NAME_MAX	32U

/* How long the copy that runs waits for a connected start's lines, in milliseconds. */
#define INSTANCE_READ_MS	500

/* How many roundtrips the wait for a token's text may take. */
#define ACTIVATION_ROUNDTRIPS	4

/* The most requests a bound copy keeps until the program takes them (more are refused). */
#define INSTANCE_PENDING	8U

/* The room the two lines of a handed-over request take: the request, the token, and their line ends. */
#define INSTANCE_MESSAGE_MAX	(KL_INSTANCE_REQUEST_MAX + KL_ACTIVATION_TOKEN_MAX + 2U)

/*
 * The one copy of a program: its listening socket, and the socket file's
 * path and identity (only the file this copy made is removed when it
 * stops).  Bound to an application (kl_instance_bind), a thread of its own
 * (kl-instance) takes the later starts' requests: the application, its
 * list through app_next and whether an event of it is queued (the
 * application's thread's), the thread and the pipe that stops it, and,
 * under the lock, the requests waiting (a ring of INSTANCE_PENDING).
 */
struct kl_instance {
	int listener;
	char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	dev_t device;
	ino_t inode;

	struct kl_app *app;
	struct kl_instance *app_next;
	int queued;
	keiui_thread_id thread;
	int control[2];

	struct keiui_lock lock;
	char requests[INSTANCE_PENDING][KL_INSTANCE_REQUEST_MAX];
	char tokens[INSTANCE_PENDING][KL_ACTIVATION_TOKEN_MAX];
	unsigned first;
	unsigned count;
};

/*
 * A token being asked for: its text once done came, and whether it came.
 */
struct activation_wait {
	char token[KL_ACTIVATION_TOKEN_MAX];
	int done;
};

static int instance_path(const char *name, char *path, size_t size);
static socklen_t instance_address(const char *path, struct sockaddr_un *address);
static int instance_connect(const char *path, int *connection);
static int instance_listen(struct kl_instance *instance);
static int instance_hand_over(int connection, const char *name, const char *request);
static int instance_write(int connection, const char *bytes, size_t length);
static int instance_read(int connection, char *message, size_t size);
static int instance_token_valid(const char *token);
static int instance_receive(struct kl_instance *instance, char *request, size_t request_size, char *token, size_t token_size);
static void *instance_run(void *argument);
static void instance_unbind(struct kl_instance *instance);
static struct xdg_activation_v1 *activation_bind(struct wl_display *display);
static void activation_done(void *data, struct xdg_activation_token_v1 *object, const char *token);

/* The token's listener, for the one event it has. */
static const struct xdg_activation_token_v1_listener activation_listener = {
	activation_done
};

/*
 * Makes this program the one copy of a name, or hands a request to the
 * copy that runs.
 */
int
kl_instance_open(
	const char *name,
	const char *request,
	struct kl_instance **instance)
{
	struct kl_instance *created;
	const char *line_end;
	size_t length;
	int connection;
	int attempt;
	int error;

	/* Nothing made yet. */
	*instance = NULL;

	/* A request of one line that fits. */
	if (request == NULL)
		return EINVAL;
	length = strlen(request);
	line_end = strchr(request, '\n');
	if (length >= KL_INSTANCE_REQUEST_MAX || line_end != NULL)
		return EINVAL;

	/* The record of the copy this may become. */
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	created->listener = -1;
	created->control[0] = -1;
	created->control[1] = -1;

	/* The socket's path, in a runtime directory that is the user's alone. */
	error = instance_path(name, created->path, sizeof(created->path));
	if (error != 0) {
		free(created);
		return error;
	}

	/* A copy that runs takes the request; otherwise this one listens (twice, when another start was quicker). */
	for (attempt = 0; attempt < 2; attempt++) {
		error = instance_connect(created->path, &connection);
		if (error != 0)
			break;

		/* A copy runs: the request goes to it, and this start ends. */
		if (connection >= 0) {
			error = instance_hand_over(connection, name, request);
			(void)close(connection);
			free(created);
			(void)unsetenv("XDG_ACTIVATION_TOKEN");
			if (error != 0)
				return error;
			return 0;
		}

		/* None runs: this start becomes the one copy, unless another start bound the path first. */
		error = instance_listen(created);
		if (error != EADDRINUSE)
			break;
	}

	/* No socket: the program runs on its own. */
	if (error != 0) {
		if (created->listener >= 0)
			(void)close(created->listener);
		free(created);
		return error;
	}

	/* The token this start may have had is not handed to the programs it starts. */
	(void)unsetenv("XDG_ACTIVATION_TOKEN");

	/* Succeeded: this is the one copy, and later starts reach it. */
	*instance = created;
	return 0;
}

/*
 * Has the requests of later starts arrive as the application's
 * KL_APP_INSTANCE events: a thread of the copy's own takes them.
 */
int
kl_instance_bind(
	struct kl_instance *instance,
	struct kl_app *app)
{
	int status;
	int error;

	/* A copy not bound yet. */
	if (instance == NULL || app == NULL || instance->app != NULL)
		return EINVAL;

	/* The lock and the pipe that stops the thread. */
	error = keiui_lock_open(&instance->lock);
	if (error != 0)
		return error;
	status = pipe2(instance->control, O_CLOEXEC | O_NONBLOCK);
	if (status != 0) {
		error = errno;
		keiui_lock_close(&instance->lock);
		return error;
	}

	/* The application's, then the thread. */
	instance->app = app;
	instance->first = 0;
	instance->count = 0;
	error = keiui_thread_start_joined(instance_run, instance, &instance->thread);
	if (error != 0) {
		instance->app = NULL;
		(void)close(instance->control[0]);
		(void)close(instance->control[1]);
		instance->control[0] = -1;
		instance->control[1] = -1;
		keiui_lock_close(&instance->lock);
		return error;
	}

	/* One of the application's copies. */
	instance->app_next = app->instances;
	app->instances = instance;

	/* Succeeded: later starts arrive as events. */
	return 0;
}

/*
 * Takes one request a later start handed over: one the thread kept when
 * the copy is bound, else the next start waiting.
 */
int
kl_instance_take(
	struct kl_instance *instance,
	char *request,
	size_t request_size,
	char *token,
	size_t token_size)
{
	size_t request_length;
	size_t token_length;
	int taken;

	/* No copy, nothing handed over. */
	if (instance == NULL || instance->listener < 0)
		return 0;

	/* Not bound: the next start waiting, read now. */
	if (instance->app == NULL) {
		taken = instance_receive(instance, request, request_size, token, token_size);
		return taken;
	}

	/* Bound: the oldest request kept, when it fits where it goes. */
	keiui_lock_take(&instance->lock);

	taken = 0;
	while (instance->count != 0U && !taken) {
		request_length = strlen(instance->requests[instance->first]);
		token_length = strlen(instance->tokens[instance->first]);
		if (request_length < request_size && token_length < token_size) {
			snprintf(request, request_size, "%s", instance->requests[instance->first]);
			snprintf(token, token_size, "%s", instance->tokens[instance->first]);
			taken = 1;
		}

		/* Drops the request from the queue, taken or too long for where it goes. */
		instance->first = (instance->first + 1U) % INSTANCE_PENDING;
		instance->count--;
	}

	keiui_lock_give(&instance->lock);

	/* One taken, or none waits. */
	return taken;
}

/*
 * Stops being the one copy.
 */
void
kl_instance_close(
	struct kl_instance *instance)
{
	struct stat status;
	int error;

	/* No copy, nothing to close. */
	if (instance == NULL)
		return;

	/* A bound copy's thread stops first. */
	if (instance->app != NULL)
		instance_unbind(instance);

	/* The socket file goes when it is still the one this copy made. */
	error = lstat(instance->path, &status);
	if (error == 0 &&
	    status.st_dev == instance->device &&
	    status.st_ino == instance->inode)
		(void)unlink(instance->path);

	/* The listener and the record. */
	if (instance->listener >= 0)
		(void)close(instance->listener);
	free(instance);
}

/*
 * Asks the compositor for an activation token.
 */
int
kl_activation_token(
	struct wl_display *display,
	struct wl_surface *surface,
	const char *app_id,
	char *token,
	size_t size)
{
	struct xdg_activation_token_v1 *object;
	struct xdg_activation_v1 *activation;
	struct activation_wait wait;
	struct wl_event_queue *queue;
	size_t length;
	int round;
	int status;

	/* Nothing yet. */
	if (size == 0U)
		return EINVAL;
	token[0] = '\0';

	/* The compositor's activation. */
	activation = activation_bind(display);
	if (activation == NULL)
		return errno;

	/* A queue of the library's own, which the token's event comes on. */
	queue = wl_display_create_queue(display);
	if (queue == NULL) {
		xdg_activation_v1_destroy(activation);
		return ENOMEM;
	}

	/* The token made from the activation comes on that queue too. */
	wl_proxy_set_queue((struct wl_proxy *)activation, queue);

	/* The token object, described and committed. */
	memset(&wait, 0, sizeof(wait));
	object = xdg_activation_v1_get_activation_token(activation);
	if (object == NULL) {
		xdg_activation_v1_destroy(activation);
		wl_event_queue_destroy(queue);
		return ENOMEM;
	}

	/* Its listener, the application it is for, the surface it comes from, and the commit. */
	(void)xdg_activation_token_v1_add_listener(object, &activation_listener, &wait);
	if (app_id != NULL)
		xdg_activation_token_v1_set_app_id(object, app_id);
	if (surface != NULL)
		xdg_activation_token_v1_set_surface(object, surface);
	xdg_activation_token_v1_commit(object);

	/* Its text, which the compositor sends at once. */
	for (round = 0; round < ACTIVATION_ROUNDTRIPS && !wait.done; round++) {
		status = wl_display_roundtrip_queue(display, queue);
		if (status < 0)
			break;
	}

	/* The objects and the queue go (a token given stays good). */
	xdg_activation_token_v1_destroy(object);
	xdg_activation_v1_destroy(activation);
	wl_event_queue_destroy(queue);

	/* No text came, or it does not fit. */
	if (!wait.done)
		return EIO;
	length = strlen(wait.token);
	if (length >= size)
		return ENOSPC;

	/* Succeeded: the caller holds the token. */
	snprintf(token, size, "%s", wait.token);
	return 0;
}

/*
 * Brings a surface's window to the front with an activation token.
 */
int
kl_activate(
	struct wl_display *display,
	struct wl_surface *surface,
	const char *token)
{
	struct xdg_activation_v1 *activation;
	int status;

	/* A token there is. */
	if (token == NULL || token[0] == '\0')
		return EINVAL;

	/* The compositor's activation. */
	activation = activation_bind(display);
	if (activation == NULL)
		return errno;

	/* The activation, sent now; the binding goes with it. */
	xdg_activation_v1_activate(activation, token, surface);
	xdg_activation_v1_destroy(activation);
	status = wl_display_flush(display);
	if (status < 0 && errno != EAGAIN)
		return errno;

	/* Succeeded: the compositor was asked. */
	return 0;
}

/* Builds the socket's path for a name, in a runtime directory that is the user's alone. */
static int
instance_path(
	const char *name,
	char *path,
	size_t size)
{
	struct stat status;
	const char *directory;
	size_t length;
	size_t index;
	uid_t user;
	int written;
	int error;

	/* A name of lowercase letters, digits and '-'. */
	length = strlen(name);
	if (length == 0U || length > INSTANCE_NAME_MAX)
		return EINVAL;
	for (index = 0; index < length; index++) {
		if ((name[index] >= 'a' && name[index] <= 'z') ||
		    (name[index] >= '0' && name[index] <= '9') ||
		    name[index] == '-')
			continue;
		return EINVAL;
	}

	/* The runtime directory, an absolute path. */
	directory = getenv("XDG_RUNTIME_DIR");
	if (directory == NULL || directory[0] != '/')
		return ENOTSUP;

	/* A directory of the user's that nobody else may enter or write. */
	error = stat(directory, &status);
	if (error != 0)
		return ENOTSUP;
	user = getuid();
	if ((status.st_mode & S_IFMT) != S_IFDIR ||
	    status.st_uid != user ||
	    (status.st_mode & 077) != 0)
		return ENOTSUP;

	/* The socket's path, which must fit a socket address. */
	written = snprintf(path, size, "%s/keiland-%s.instance", directory, name);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded: the path is built. */
	return 0;
}

/* Fills a socket address with a path (instance_path made it fit) and reports its exact length, the NUL counted. */
static socklen_t
instance_address(
	const char *path,
	struct sockaddr_un *address)
{
	size_t length;

	/* The family and the path. */
	memset(address, 0, sizeof(*address));
	address->sun_family = AF_UNIX;
	snprintf(address->sun_path, sizeof(address->sun_path), "%s", path);

	/* The length up to the path's NUL. */
	length = offsetof(struct sockaddr_un, sun_path) + strlen(address->sun_path) + 1U;
	return (socklen_t)length;
}

/*
 * Connects to the copy that runs: *connection is the connection, or -1
 * when none runs (no socket, or one left by a copy that ended, which is
 * removed).
 */
static int
instance_connect(
	const char *path,
	int *connection)
{
	struct sockaddr_un address;
	struct stat status;
	socklen_t length;
	uid_t user;
	int descriptor;
	int error;
	int saved;

	/* None yet. */
	*connection = -1;

	/* The socket and the address. */
	descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (descriptor < 0)
		return errno;
	length = instance_address(path, &address);

	/* A copy that listens takes the connection. */
	error = connect(descriptor, (const struct sockaddr *)&address, length);
	if (error == 0) {
		*connection = descriptor;
		return 0;
	}

	/* No socket: none runs. */
	saved = errno;
	(void)close(descriptor);
	if (saved == ENOENT)
		return 0;

	/* Any other refusal than one of a socket nobody listens on. */
	if (saved != ECONNREFUSED)
		return saved;

	/* A socket left by a copy that ended: removed when it is the user's socket. */
	error = lstat(path, &status);
	if (error != 0)
		return 0;
	user = getuid();
	if ((status.st_mode & S_IFMT) != S_IFSOCK || status.st_uid != user)
		return EEXIST;
	error = unlink(path);
	if (error != 0 && errno != ENOENT)
		return errno;

	/* Succeeded: none runs. */
	return 0;
}

/* Makes the listening socket of the one copy, recording the file it made. */
static int
instance_listen(
	struct kl_instance *instance)
{
	struct sockaddr_un address;
	struct stat status;
	socklen_t length;
	mode_t mask;
	int error;
	int saved;

	/* A socket that never blocks the program's loop. */
	instance->listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (instance->listener < 0)
		return errno;
	length = instance_address(instance->path, &address);

	/* The file, for the user alone (an existing one is another start's: EADDRINUSE). */
	mask = umask(077);
	error = bind(instance->listener, (const struct sockaddr *)&address, length);
	saved = errno;
	(void)umask(mask);
	if (error != 0) {
		(void)close(instance->listener);
		instance->listener = -1;
		return saved;
	}

	/* The identity of the file made, so only it is removed later. */
	error = lstat(instance->path, &status);
	if (error != 0) {
		saved = errno;
		(void)close(instance->listener);
		instance->listener = -1;
		return saved;
	}

	/* The file's identity, compared when the copy stops. */
	instance->device = status.st_dev;
	instance->inode = status.st_ino;

	/* Later starts may connect. */
	error = listen(instance->listener, 4);
	if (error != 0) {
		saved = errno;
		(void)close(instance->listener);
		instance->listener = -1;
		(void)unlink(instance->path);
		return saved;
	}

	/* Succeeded: this is the one copy. */
	return 0;
}

/* Writes the request and the activation token to the copy that runs, a line each. */
static int
instance_hand_over(
	int connection,
	const char *name,
	const char *request)
{
	char message[INSTANCE_MESSAGE_MAX + 1U];
	char token[KL_ACTIVATION_TOKEN_MAX];
	struct wl_display *display;
	const char *given;
	size_t length;
	int written;
	int valid;
	int error;

	/*
	 * A token asked of the compositor, which grants it to a program that
	 * has just started.  It is preferred to XDG_ACTIVATION_TOKEN, which a
	 * program may have inherited from a parent that never used its own
	 * (a long-running Files that starts Settings): such a token has run
	 * out by now.
	 */
	token[0] = '\0';
	display = wl_display_connect(NULL);
	if (display != NULL) {
		error = kl_activation_token(display, NULL, name, token, sizeof(token));
		if (error != 0)
			token[0] = '\0';
		wl_display_disconnect(display);
	}

	/* Without a compositor's token, the one this start was given, when it is a token that fits. */
	given = getenv("XDG_ACTIVATION_TOKEN");
	if (token[0] == '\0' && given != NULL) {
		length = strlen(given);
		valid = instance_token_valid(given);
		if (valid && length < sizeof(token))
			snprintf(token, sizeof(token), "%s", given);
	}

	/* The two lines. */
	written = snprintf(message, sizeof(message), "%s\n%s\n", request, token);
	if (written < 0 || (size_t)written >= sizeof(message))
		return EINVAL;

	/* Written whole. */
	error = instance_write(connection, message, (size_t)written);
	if (error != 0)
		return error;

	/* Succeeded: the copy that runs has the request. */
	return 0;
}

/* Writes all of a few bytes to a connection. */
static int
instance_write(
	int connection,
	const char *bytes,
	size_t length)
{
	ssize_t written;
	size_t done;

	/* Until every byte went. */
	done = 0;
	while (done < length) {
		written = write(connection, bytes + done, length - done);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return EIO;
		done += (size_t)written;
	}

	/* Succeeded: all of it went. */
	return 0;
}

/* Reads a start's lines until it closes, or the short wait ends; the bytes end with a NUL. */
static int
instance_read(
	int connection,
	char *message,
	size_t size)
{
	struct pollfd descriptor;
	ssize_t got;
	size_t length;
	int ready;

	/* Until the start closes (its lines are short) or the room is full. */
	length = 0;
	for (;;) {
		descriptor.fd = connection;
		descriptor.events = POLLIN;
		descriptor.revents = 0;
		ready = poll(&descriptor, 1, INSTANCE_READ_MS);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready <= 0)
			return ETIMEDOUT;

		/* The bytes that came. */
		got = read(connection, message + length, size - 1U - length);
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0)
			return EIO;
		if (got == 0)
			break;
		length += (size_t)got;
		if (length == size - 1U)
			return EMSGSIZE;
	}

	/* Succeeded: the lines, as one string. */
	message[length] = '\0';
	return 0;
}

/* Tells whether a token has only a token's characters (visible ASCII without spaces); the empty token is one. */
static int
instance_token_valid(
	const char *token)
{
	size_t index;

	/* Each character. */
	for (index = 0; token[index] != '\0'; index++) {
		if (token[index] <= ' ' || token[index] > '~')
			return 0;
	}

	/* Succeeded: it is a token, or none. */
	return 1;
}

/* Binds the compositor's xdg_activation_v1 on the display's default queue; NULL with errno set (ENOTSUP when there is none). */
static struct xdg_activation_v1 *
activation_bind(
	struct wl_display *display)
{
	struct xdg_activation_v1 *activation;
	struct keiui_global_search search;
	int error;

	/* The global. */
	error = keiui_global_find(&search, display, "xdg_activation_v1");
	if (error != 0) {
		keiui_global_end(&search);
		errno = error;
		return NULL;
	}

	/* Bound at version 1, when announced; the search ends. */
	activation = keiui_global_bind(&search, &xdg_activation_v1_interface, 1U);
	keiui_global_end(&search);

	/* A compositor without activation. */
	if (activation == NULL) {
		errno = ENOTSUP;
		return NULL;
	}

	/* Succeeded: the activation is bound. */
	return activation;
}

/* Keeps the text of the token asked for (done). */
static void
activation_done(
	void *data,
	struct xdg_activation_token_v1 *object,
	const char *token)
{
	struct activation_wait *wait;
	size_t length;

	/* The text, when it fits; the wait ends either way. */
	(void)object;
	wait = data;
	length = strlen(token);
	if (length < sizeof(wait->token))
		snprintf(wait->token, sizeof(wait->token), "%s", token);
	wait->done = 1;
}

/*
 * Takes the next start waiting on the listening socket: accepts it, reads
 * its two lines within a short wait and checks them.  Returns 1 with the
 * request and the token copied, 0 when none waits or what came is not a
 * request.
 */
static int
instance_receive(
	struct kl_instance *instance,
	char *request,
	size_t request_size,
	char *token,
	size_t token_size)
{
	char message[INSTANCE_MESSAGE_MAX + 1U];
	char *line_end;
	char *token_text;
	char *token_end;
	size_t request_length;
	size_t token_length;
	int connection;
	int valid;
	int error;

	/* The next start waiting, if any. */
	connection = accept(instance->listener, NULL, NULL);
	if (connection < 0)
		return 0;

	/* Its two lines, read within a short wait, then the connection goes. */
	error = instance_read(connection, message, sizeof(message));
	(void)close(connection);
	if (error != 0)
		return 0;

	/* The request's line. */
	line_end = strchr(message, '\n');
	if (line_end == NULL)
		return 0;
	*line_end = '\0';

	/* The token's line (empty when the start had none). */
	token_text = line_end + 1;
	token_end = strchr(token_text, '\n');
	if (token_end == NULL)
		return 0;
	*token_end = '\0';

	/* Both fit where they go, and the token has a token's characters. */
	request_length = strlen(message);
	token_length = strlen(token_text);
	if (request_length >= request_size || token_length >= token_size)
		return 0;
	valid = instance_token_valid(token_text);
	if (!valid)
		return 0;

	/* Succeeded: the request and its token are the caller's. */
	snprintf(request, request_size, "%s", message);
	snprintf(token, token_size, "%s", token_text);
	return 1;
}

/*
 * The thread of a bound copy: waits for a later start or its stop, keeps
 * each request (up to INSTANCE_PENDING), and rings the application's wake
 * when the first waits.
 */
static void *
instance_run(
	void *argument)
{
	char request[KL_INSTANCE_REQUEST_MAX];
	char token[KL_ACTIVATION_TOKEN_MAX];
	struct kl_instance *instance;
	struct pollfd descriptors[2];
	unsigned slot;
	int received;
	int status;

	/* The copy. */
	keiui_thread_name("kl-instance");
	instance = argument;

	/* Each wait: a start, or the stop. */
	for (;;) {
		descriptors[0].fd = instance->listener;
		descriptors[0].events = POLLIN;
		descriptors[0].revents = 0;
		descriptors[1].fd = instance->control[0];
		descriptors[1].events = POLLIN;
		descriptors[1].revents = 0;
		status = poll(descriptors, 2, -1);
		if (status < 0 && errno == EINTR)
			continue;
		if (status < 0 || descriptors[1].revents != 0)
			break;

		/* Each start waiting, kept while there is room. */
		for (;;) {
			received = instance_receive(instance, request, sizeof(request), token, sizeof(token));
			if (!received)
				break;

			/* Keeps the request, when the queue has room. */
			keiui_lock_take(&instance->lock);

			if (instance->count < INSTANCE_PENDING) {
				slot = (instance->first + instance->count) % INSTANCE_PENDING;
				snprintf(instance->requests[slot], sizeof(instance->requests[slot]), "%s", request);
				snprintf(instance->tokens[slot], sizeof(instance->tokens[slot]), "%s", token);
				instance->count++;
				if (instance->count == 1U)
					keiui_wake_ring(&instance->app->post_wake);
			}

			keiui_lock_give(&instance->lock);
		}
	}

	/* Stopped. */
	return NULL;
}

/*
 * Stops a bound copy's thread and takes the copy out of its application:
 * out of the list and its events out of the queue.
 */
static void
instance_unbind(
	struct kl_instance *instance)
{
	struct kl_instance **link;
	struct kl_app *app;
	const char byte = 1;
	ssize_t written;

	/* The thread told to stop, and waited for. */
	do {
		written = write(instance->control[1], &byte, 1);
	} while (written < 0 && errno == EINTR);
	keiui_thread_join(instance->thread);

	/* Out of the application's list and queue. */
	app = instance->app;
	for (link = &app->instances; *link != NULL; link = &(*link)->app_next) {
		if (*link == instance) {
			*link = instance->app_next;
			break;
		}
	}

	/* Takes the copy's events out of the application's queue. */
	keiui_app_forget_instance(app, instance);

	/* The pipe and the lock; the copy is no application's any more. */
	(void)close(instance->control[0]);
	(void)close(instance->control[1]);
	instance->control[0] = -1;
	instance->control[1] = -1;
	keiui_lock_close(&instance->lock);
	instance->app = NULL;
	instance->app_next = NULL;
	instance->queued = 0;
}

/*
 * Queues an event for each bound copy with requests waiting and none
 * queued (the application's dispatch).
 */
void
keiui_instances_queue(
	struct kl_app *app)
{
	struct kl_instance *instance;
	struct kl_app_event *event;
	unsigned count;

	/* Each copy bound to the application. */
	for (instance = app->instances; instance != NULL; instance = instance->app_next) {
		if (instance->queued)
			continue;

		/* Whether requests wait. */
		keiui_lock_take(&instance->lock);

		count = instance->count;

		keiui_lock_give(&instance->lock);
		if (count == 0U)
			continue;

		/* Its event, while the queue has room. */
		event = keiui_app_slot(app);
		if (event == NULL)
			return;
		event->kind = KL_APP_INSTANCE;
		event->instance = instance;
		instance->queued = 1;
	}
}

/*
 * Notes that a copy's event was taken, so that the next one may be queued.
 */
void
keiui_instance_taken(
	struct kl_app_event *event)
{
	/* A copy's event. */
	if (event->kind != KL_APP_INSTANCE || event->instance == NULL)
		return;

	/* None of it queued now. */
	event->instance->queued = 0;
}

/*
 * Unbinds every copy bound to an application that closes.
 */
void
keiui_instances_unbind(
	struct kl_app *app)
{
	/* Each copy (its unbinding takes it out of the list). */
	while (app->instances != NULL)
		instance_unbind(app->instances);
}
