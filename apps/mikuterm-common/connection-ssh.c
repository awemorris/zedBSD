/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The tabs' SSH connections on the application's thread (connection.h):
 * each tab's connection is a session (session.h) whose thread
 * (ssh-thread.c) connects and runs the SSH engine.  The application's
 * thread never waits for the network: it reads what the thread put in
 * the session's output ring, hands the keys to the thread, and closing a
 * tab only tells the thread, which ends the connection and lets the
 * session go on its own.
 *
 * While the thread asks a question, the keys typed are the answer's line,
 * edited here: Backspace, Ctrl+U, Enter to answer, Ctrl+C or Ctrl+D to
 * give up; a secret is not shown, and the input method is off for it
 * (the connection's mode is then a line without echo).
 *
 * The threads post only while the application is open: the run tells
 * mikuterm_connections_stop before it closes the application, and the
 * program waits a while for the threads to end (mikuterm_connections_wait).
 *
 * A tab whose user chose the login shell (MIKUTERM_HAVE_LOGIN_SHELL)
 * becomes the shell's terminal: its thread ends, the tab starts the shell
 * on a terminal of its own (login-shell.h), reads what is left in the
 * ring and then what the shell writes, and hands its keys, size and end
 * to the shell.
 */

#include "session.h"
#include "sftp-link.h"
#include "login-shell.h"
#include "x11-tab.h"

#include <compat/crypto/crypto.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The first title of a tab, before the remote side sets its own: the
 * session's number (the order the tabs were opened in, from 1) and the
 * program's name, the number first so that a tab cut to a narrow width
 * still shows it, and tabs still on the launch screen differ in the tab
 * strip and in its list of the tabs out of sight.
 */
#define CONNECTION_TITLE	"mikuterm"

/*
 * The threads of all the sessions: whether they may post (the application
 * is open), how many still run, and the number the next session gets.
 * The registry's lock keeps them; ended is signalled when a thread ends.
 * They live as long as the program.
 */
static struct kl_mutex connection_registry = KL_MUTEX_INIT;
static struct kl_cond connection_threads_ended;
static int connection_cond_made;
static int connection_posting;
static unsigned connection_threads;
static uint32_t connection_next = 1U;

static int connection_open(void *data, struct kl_app *app, uint32_t id, unsigned columns, unsigned rows, unsigned width, unsigned height, char *title, size_t size, struct terminal_connection **connection);
static size_t connection_read(struct terminal_connection *connection, unsigned char *buffer, size_t size);
static int connection_write(struct terminal_connection *connection, const unsigned char *bytes, size_t size, size_t *written);
static int connection_resize(struct terminal_connection *connection, unsigned columns, unsigned rows, unsigned width, unsigned height);
static int connection_mode(const struct terminal_connection *connection, unsigned *mode);
static unsigned connection_event(struct terminal_connection *connection, const struct kl_app_event *event);
static unsigned connection_pending(const struct terminal_connection *connection);
static void connection_close(struct terminal_connection *connection);
static struct terminal_connection *connection_take(struct terminal_connection *connection);
static void connection_edit(struct mikuterm_session *session, const unsigned char *bytes, size_t size);
static void connection_erase(struct mikuterm_session *session);
static void connection_free(struct mikuterm_session *session);
static void connection_copy(char *to, size_t size, const char *text);
#if MIKUTERM_HAVE_LOGIN_SHELL
static unsigned connection_shell(struct mikuterm_session *session, const char *path);
#endif

/* The functions of a tab's SSH connection. */
static const struct terminal_connection_ops connection_ops = {
	connection_read,
	connection_write,
	connection_resize,
	connection_mode,
	connection_event,
	connection_pending,
	connection_close,
	connection_take,
	NULL
};

/*
 * Makes the tabs' connector: each tab connects on a thread of its own,
 * the first to options->connect when it names a host.  The options last
 * as long as the run.
 */
void
mikuterm_connector_init(
	struct mikuterm_connector *connector,
	const struct mikuterm_connect_options *options)
{
	int error;

	/* The connector. */
	memset(connector, 0, sizeof(*connector));
	connector->connector.open = connection_open;
	connector->connector.operation = "mikuterm_session_start";
	connector->connector.data = connector;
	connector->options = *options;
	connector->first_opened = 0;

	/* The threads may post from now on; the signal of their ends is made once. */
	kl_mutex_lock(&connection_registry);

	connection_posting = 1;
	if (!connection_cond_made) {
		error = kl_cond_init(&connection_threads_ended);
		if (error == 0)
			connection_cond_made = 1;
	}

	kl_mutex_unlock(&connection_registry);
}

/*
 * Stops the threads' posts, before the application closes: a post after
 * it would go to an application that is gone.
 */
void
mikuterm_connections_stop(void)
{
	/* No thread posts from now on. */
	kl_mutex_lock(&connection_registry);

	connection_posting = 0;

	kl_mutex_unlock(&connection_registry);
}

/*
 * Waits for the threads to end, at most so many milliseconds in all.
 * Returns 0 when none runs, or ETIMEDOUT.
 */
int
mikuterm_connections_wait(
	unsigned milliseconds)
{
	uint64_t deadline;
	uint64_t now;
	unsigned running;

	/* The time the wait gives up. */
	deadline = kl_clock_us() / 1000U + milliseconds;

	/* Until no thread runs or the time is up. */
	kl_mutex_lock(&connection_registry);

	for (;;) {
		/* No thread runs, or the time is up. */
		running = connection_threads;
		now = kl_clock_us() / 1000U;
		if (running == 0U ||
		    now >= deadline ||
		    !connection_cond_made)
			break;
		(void)kl_cond_wait_for(&connection_threads_ended, &connection_registry, (uint32_t)(deadline - now));
	}

	kl_mutex_unlock(&connection_registry);

	/* Reports threads still running. */
	if (running != 0U)
		return ETIMEDOUT;

	/* Succeeded: every thread ended. */
	return 0;
}

/*
 * Lets go of a session: the tab's or the thread's reference.  The last
 * frees it; a thread that lets go also says it ended.
 */
void
session_release(
	struct mikuterm_session *session)
{
	unsigned remaining;

	/* One reference fewer. */
	kl_mutex_lock(&session->lock);

	session->references--;
	remaining = session->references;

	kl_mutex_unlock(&session->lock);

	/* The last one frees the session. */
	if (remaining == 0U)
		connection_free(session);
}

/*
 * Tells the tab that its session has something for it, unless a post is
 * already on its way or the application is closing.
 */
void
session_post(
	struct mikuterm_session *session)
{
	int error;

	/* One post at a time: the tab takes everything there is when it takes one. */
	kl_mutex_lock(&session->lock);

	if (session->posted) {
		kl_mutex_unlock(&session->lock);
		return;
	}

	/* The post is this call's; the tab clears the mark when it takes it. */
	session->posted = 1;

	kl_mutex_unlock(&session->lock);

	/* The post; a post that failed lets the next one try. */
	error = session_app_post(session->app, session->number);

	/* A failed post is not on its way. */
	if (error != 0) {
		kl_mutex_lock(&session->lock);

		session->posted = 0;

		kl_mutex_unlock(&session->lock);
	}
}

/*
 * Adds bytes to what the tab reads, as many as the ring has room for;
 * the caller holds the session's lock.  Returns how many were added.
 */
size_t
session_put_locked(
	struct mikuterm_session *session,
	const void *bytes,
	size_t size)
{
	const unsigned char *from;
	size_t room;
	size_t end;
	size_t first;

	/* As many as fit. */
	room = session->output_capacity - session->output_length;
	if (size > room)
		size = room;
	if (size == 0U)
		return 0U;

	/* After the bytes there, wrapping round the ring's end. */
	from = bytes;
	end = (session->output_start + session->output_length) % session->output_capacity;
	first = session->output_capacity - end;
	if (first > size)
		first = size;
	memcpy(session->output + end, from, first);
	memcpy(session->output, from + first, size - first);
	session->output_length += size;

	/* Whether the tab's cursor is now at the start of a line. */
	session->line_started = 0;
	if (from[size - 1U] == '\n')
		session->line_started = 1;

	/* Succeeded: the bytes are the tab's to read. */
	return size;
}

/* Shows a text in the tab (the thread's own lines: questions, why a connection ended). */
void
session_say(
	struct mikuterm_session *session,
	const char *text)
{
	/* The text in the ring. */
	kl_mutex_lock(&session->lock);

	(void)session_put_locked(session, text, strlen(text));

	kl_mutex_unlock(&session->lock);

	/* The tab reads it. */
	session_post(session);
}

/* Shows a line of the thread's own in the tab, on a line of its own (after what the remote side left unended). */
void
session_say_line(
	struct mikuterm_session *session,
	const char *text)
{
	/* The end of a line left open, then the text. */
	kl_mutex_lock(&session->lock);

	if (!session->line_started)
		(void)session_put_locked(session, "\r\n", 2U);
	(void)session_put_locked(session, text, strlen(text));

	kl_mutex_unlock(&session->lock);

	/* The tab reads it. */
	session_post(session);
}

/*
 * Shows whether the passphrase asked for is kept for the user's session
 * (the caller holds the lock): one line with a box, checked when it is,
 * and the key that switches it.
 */
void
session_keep_line_locked(
	struct mikuterm_session *session)
{
	const char *keep_text;

	/* The box as the choice stands. */
	if (session->keep)
		(void)session_put_locked(session, "[x] ", 4U);
	else
		(void)session_put_locked(session, "[ ] ", 4U);

	/* What it means. */
	keep_text = mikuterm_passphrase_keep_text();
	(void)session_put_locked(session, keep_text, strlen(keep_text));
}

/* Says that the connection ended as the tab ends: the tab closes once it read everything. */
void
session_ended(
	struct mikuterm_session *session)
{
	/* The end. */
	kl_mutex_lock(&session->lock);

	session->ended = 1;

	kl_mutex_unlock(&session->lock);

	/* The tab learns it. */
	session_post(session);
}

/*
 * Says that the user asked the tab for the settings: the run opens them
 * in a tab of their own (or chooses the one open), and this tab goes on.
 */
void
session_settings(
	struct mikuterm_session *session)
{
	/* The request, for the tab's next post. */
	kl_mutex_lock(&session->lock);

	session->settings = 1;

	kl_mutex_unlock(&session->lock);

	/* The tab learns it. */
	session_post(session);
}

#if MIKUTERM_HAVE_LOGIN_SHELL
/*
 * Asks the tab for the login shell of a path in place of the connection,
 * the thread's last word: the tab starts it at its next post.
 */
void
session_shell(
	struct mikuterm_session *session,
	const char *path)
{
	/* The shell, for the tab's next post. */
	kl_mutex_lock(&session->lock);

	connection_copy(session->shell, sizeof(session->shell), path);

	kl_mutex_unlock(&session->lock);

	/* The tab learns it. */
	session_post(session);
}
#endif

/*
 * Posts to the application that the session or the X window's tab of a
 * number has something for it, while the application is open.  Returns
 * 0, or an errno value (ECANCELED once it is closing).
 */
int
session_app_post(
	struct kl_app *app,
	uint32_t number)
{
	int error;

	/* The post, only while the application is open. */
	kl_mutex_lock(&connection_registry);

	error = ECANCELED;
	if (connection_posting)
		error = kl_app_post(app, SESSION_POST, (void *)(uintptr_t)number);

	kl_mutex_unlock(&connection_registry);

	/* Reports whether the post is on its way. */
	if (error != 0)
		return error;

	/* Succeeded: posted. */
	return 0;
}

/*
 * Gives the session's tab an X window's tab the thread opened, with the
 * run's hold on it: the tab gets a number for its posts and the session,
 * and waits for the run to take it (the session's next post says it is
 * there).  A session that already has as many waiting refuses it.
 */
void
session_tab(
	struct mikuterm_session *session,
	struct x11_tab *tab)
{
	uint32_t number;
	int closing;
	int queued;

	/* A number of its own, among the sessions'. */
	kl_mutex_lock(&connection_registry);

	number = connection_next;
	connection_next++;

	kl_mutex_unlock(&connection_registry);

	/* The session and the number, before the tab is anywhere the application's thread looks. */
	x11_tab_attach(tab, session, number);

	/* The tab waits for the run, when the session's tab is still open and there is room. */
	kl_mutex_lock(&session->lock);

	closing = session->closing;
	queued = 0;
	if (!closing && session->tab_count < SESSION_TABS) {
		session->tabs[session->tab_count] = tab;
		session->tab_count++;
		queued = 1;
	}

	kl_mutex_unlock(&session->lock);

	/* A tab with no room is refused (the X window becomes a window of its own); a connection ending closes it. */
	if (!queued) {
		if (!closing)
			tab->connection.ops->picture->refuse(&tab->connection);
		tab->connection.ops->close(&tab->connection);
		return;
	}

	/* The run learns of it. */
	session_post(session);
}

/* Says that a session's thread ended (its very last step), for the program's wait. */
void
session_thread_ended(void)
{
	/* One thread fewer; the wait learns it. */
	kl_mutex_lock(&connection_registry);

	connection_threads--;
	if (connection_cond_made)
		kl_cond_broadcast(&connection_threads_ended);

	kl_mutex_unlock(&connection_registry);
}

/*
 * Makes a tab's session and starts its thread.  The first tab connects to
 * the Connect host when there is one; the others ask.  Returns 0, ENOMEM,
 * or the error of making the waker or the thread.
 */
static int
connection_open(
	void *data,
	struct kl_app *app,
	uint32_t id,
	unsigned columns,
	unsigned rows,
	unsigned width,
	unsigned height,
	char *title,
	size_t size,
	struct terminal_connection **connection)
{
	struct mikuterm_connector *connector;
	struct mikuterm_session *session;
	struct kl_thread_options thread_options;
	struct kl_thread *thread;
	uint32_t number;
	size_t length;
	int error;

	/* The tab's ID is the core's; the session has its own number. */
	(void)id;
	connector = data;

	/* The session's memory, and its ring. */
	session = calloc(1U, sizeof(*session));
	if (session == NULL)
		return ENOMEM;
	session->output_capacity = SESSION_OUTPUT_MAX;
	session->output = malloc(session->output_capacity);
	if (session->output == NULL) {
		free(session);
		return ENOMEM;
	}

	/* The waker the thread waits on. */
	error = kl_socket_waker_open(&session->waker);
	if (error != 0) {
		free(session->output);
		free(session);
		return error;
	}

	/* Its lock. */
	error = kl_mutex_init(&session->lock);
	if (error != 0) {
		kl_socket_waker_close(session->waker);
		free(session->output);
		free(session);
		return error;
	}

	/* What it connects with: the first tab the Connect host and the settings' problems, the others neither. */
	session->connection.ops = &connection_ops;
	session->app = app;
	length = strlen(connector->options.term);
	if (length >= sizeof(session->term))
		length = sizeof(session->term) - 1U;
	memcpy(session->term, connector->options.term, length);
	session->connect_timeout = connector->options.connect_timeout;
	session->wsl_port = connector->options.wsl_port;
	session->x11_scale = connector->options.x11_scale;
	session->x11_render_cpu = connector->options.x11_render_cpu;
	session->x11_primary = connector->options.x11_primary;
	session->sftp = connector->options.sftp;
	if (!connector->first_opened) {
		connection_copy(session->host, sizeof(session->host), connector->options.connect);
		connection_copy(session->notice, sizeof(session->notice), connector->options.notice);
	}

	/* Its grid; the tab and the thread hold it. */
	session->columns = columns;
	session->rows = rows;
	session->width = width;
	session->height = height;
	session->references = 2U;
	session->line_started = 1;
	session->keep = 1;

	/* Its number, and one more thread running. */
	kl_mutex_lock(&connection_registry);

	session->number = connection_next;
	number = session->number;
	connection_next++;
	connection_threads++;

	kl_mutex_unlock(&connection_registry);

	/* The thread, which lets go of the session itself when it ends. */
	memset(&thread_options, 0, sizeof(thread_options));
	thread_options.name = "ssh";
	thread_options.run = session_run;
	thread_options.argument = session;
	thread_options.priority = KL_THREAD_PRIORITY_NORMAL;
	error = kl_thread_start(&thread_options, &thread);
	if (error != 0) {
		kl_mutex_lock(&connection_registry);

		connection_threads--;

		kl_mutex_unlock(&connection_registry);


		/* The session goes with the thread that did not start. */
		connection_free(session);
		return error;
	}

	/* The thread runs on its own. */
	(void)kl_thread_detach(thread);

	/* The tab's first title, until the remote side sets one (the session may be gone already: its number was kept). */
	if (size != 0U)
		(void)snprintf(title, size, "%u %s", (unsigned)number, CONNECTION_TITLE);

	/* Succeeded: the tab has its connection. */
	connector->first_opened = 1;
	*connection = &session->connection;
	return 0;
}

/* Reads what the thread put in the ring for the tab; returns how many bytes (0 when none waits). */
static size_t
connection_read(
	struct terminal_connection *connection,
	unsigned char *buffer,
	size_t size)
{
	struct mikuterm_session *session;
	size_t count;
	size_t first;
	int tell;

	/* The session the connection is. */
	session = (struct mikuterm_session *)connection;

	/* As much as there is and fits, from the ring's start, wrapping round its end. */
	kl_mutex_lock(&session->lock);

	count = size;
	if (count > session->output_length)
		count = session->output_length;
	first = session->output_capacity - session->output_start;
	if (first > count)
		first = count;
	memcpy(buffer, session->output + session->output_start, first);
	memcpy(buffer + first, session->output, count - first);
	session->output_start = (session->output_start + count) % session->output_capacity;
	session->output_length -= count;

	/*
	 * taken is what the thread gives back to the server's window: it is
	 * told after a step's worth, or when the tab has read everything.
	 */
	session->taken += count;
	tell = 0;
	if (session->taken >= SESSION_TAKEN_STEP)
		tell = 1;
	if (session->taken != 0U && session->output_length == 0U)
		tell = 1;

	kl_mutex_unlock(&session->lock);

	/* The thread learns what was read. */
	if (tell)
		kl_socket_waker_ring(session->waker);

#if MIKUTERM_HAVE_LOGIN_SHELL
	/* After the ring (the launch screen's last words), what the login shell wrote. */
	if (session->process != NULL && count < size)
		count += kl_process_read(session->process, KL_PROCESS_OUTPUT, buffer + count, size - count);

#endif
	/* Succeeded: the bytes read. */
	return count;
}

/*
 * Takes the keys typed: the answer's line while a question is asked, the
 * remote side's input otherwise (as many as the input holds; the rest
 * waits in the tab).  A connection that ended takes them and drops them.
 * Returns 0.
 */
static int
connection_write(
	struct terminal_connection *connection,
	const unsigned char *bytes,
	size_t size,
	size_t *written)
{
	struct mikuterm_session *session;
	size_t room;
	int wake;
#if MIKUTERM_HAVE_LOGIN_SHELL
	int error;
#endif

	/* The session the connection is. */
	session = (struct mikuterm_session *)connection;
	wake = 0;

#if MIKUTERM_HAVE_LOGIN_SHELL
	/* The keys of the login shell's terminal (as many as it takes now). */
	if (session->process != NULL) {
		error = kl_process_write(session->process, bytes, size, written);
		if (error != 0)
			return error;
		return 0;
	}

#endif
	/* Where the keys go. */
	kl_mutex_lock(&session->lock);

	if (session->question != SESSION_ASK_NONE && !session->answered) {
		/* The answer's line; the keys after an answer are dropped. */
		connection_edit(session, bytes, size);
		*written = size;
		if (session->answered)
			wake = 1;
	} else if (session->ended || session->closing) {
		/* Nobody reads them any more. */
		*written = size;
	} else {
		/* The input, as much as it has room for. */
		room = SESSION_INPUT_MAX - session->input_length;
		if (size > room) {
			size = room;
			session->input_full = 1;
		}

		/* The bytes that fit. */
		memcpy(session->input + session->input_length, bytes, size);
		session->input_length += size;
		*written = size;
		if (size != 0U)
			wake = 1;
	}

	kl_mutex_unlock(&session->lock);

	/* The thread takes them. */
	if (wake)
		kl_socket_waker_ring(session->waker);

	/* Succeeded. */
	return 0;
}

/* Gives the thread the grid's new size, which it sends once the session runs. */
static int
connection_resize(
	struct terminal_connection *connection,
	unsigned columns,
	unsigned rows,
	unsigned width,
	unsigned height)
{
	struct mikuterm_session *session;

	/* The session the connection is. */
	session = (struct mikuterm_session *)connection;

	/* The size, for the thread. */
	kl_mutex_lock(&session->lock);

	session->columns = columns;
	session->rows = rows;
	session->width = width;
	session->height = height;
	session->resized = 1;

	kl_mutex_unlock(&session->lock);

	/* The thread takes it. */
	kl_socket_waker_ring(session->waker);

#if MIKUTERM_HAVE_LOGIN_SHELL
	/* The login shell's terminal takes it (the shell gets SIGWINCH). */
	if (session->process != NULL)
		return kl_process_resize(session->process, columns, rows);

#endif
	/* Succeeded. */
	return 0;
}

/*
 * Reports the input's modes: a line without echo while a secret is asked
 * (the input method is then off), a line with echo otherwise (the remote
 * side's own modes are not known here).
 */
static int
connection_mode(
	const struct terminal_connection *connection,
	unsigned *mode)
{
	struct mikuterm_session *session;
	unsigned question;
#if MIKUTERM_HAVE_LOGIN_SHELL
	unsigned terminal;
	int error;
#endif

	/* The session the connection is. */
	session = (struct mikuterm_session *)connection;

#if MIKUTERM_HAVE_LOGIN_SHELL
	/* The modes the login shell's program set on its terminal (a password is read without echo). */
	if (session->process != NULL) {
		error = kl_process_terminal_mode(session->process, &terminal);
		if (error != 0)
			return error;
		*mode = 0U;
		if ((terminal & KL_PROCESS_TERMINAL_ECHO) != 0U)
			*mode |= TERMINAL_MODE_ECHO;
		if ((terminal & KL_PROCESS_TERMINAL_LINES) != 0U)
			*mode |= TERMINAL_MODE_LINES;
		return 0;
	}

#endif
	/* The question asked now. */
	kl_mutex_lock(&session->lock);

	question = session->question;
	if (session->answered)
		question = SESSION_ASK_NONE;

	kl_mutex_unlock(&session->lock);

	/* A secret is read on a line without echo. */
	*mode = TERMINAL_MODE_ECHO | TERMINAL_MODE_LINES;
	if (question == SESSION_ASK_SECRET)
		*mode = TERMINAL_MODE_LINES;

	/* Succeeded. */
	return 0;
}

/*
 * Tells whether an application's event is this session's post: its
 * TERMINAL_READY_* bits (output to read, the end, room for the keys), 0
 * when the event is not its.
 */
static unsigned
connection_event(
	struct terminal_connection *connection,
	const struct kl_app_event *event)
{
	struct mikuterm_session *session;
	unsigned ready;
#if MIKUTERM_HAVE_LOGIN_SHELL
	char shell[MIKUTERM_PATH_MAX];
#endif

	/* The session the connection is. */
	session = (struct mikuterm_session *)connection;

#if MIKUTERM_HAVE_LOGIN_SHELL
	/* The login shell's news: output, room for the keys, its end (the same bits). */
	if (session->process != NULL &&
	    event->kind == KL_APP_PROCESS &&
	    event->process == session->process)
		return event->ready;

#endif
	/* Only this session's post. */
	if (event->kind != KL_APP_POST || event->code != SESSION_POST)
		return 0U;
	if ((uintptr_t)event->data != (uintptr_t)session->number)
		return 0U;

	/* The post is taken: the thread may post again, everything there is now is read, and a request for the settings is made once. */
	kl_mutex_lock(&session->lock);

	session->posted = 0;
	ready = TERMINAL_READY_INPUT;
	if (session->output_length != 0U)
		ready |= TERMINAL_READY_OUTPUT;
	if (session->ended)
		ready |= TERMINAL_READY_EXIT;
	if (session->settings)
		ready |= TERMINAL_READY_SETTINGS;
	session->settings = 0;
	if (session->tab_count != 0U)
		ready |= TERMINAL_READY_TAB;
#if MIKUTERM_HAVE_LOGIN_SHELL
	memcpy(shell, session->shell, sizeof(shell));
	session->shell[0] = '\0';
#endif

	kl_mutex_unlock(&session->lock);

#if MIKUTERM_HAVE_LOGIN_SHELL
	/* The login shell the thread asked for, started once. */
	if (shell[0] != '\0' && session->process == NULL)
		ready |= connection_shell(session, shell);

#endif
	/* Reports what the session has. */
	return ready;
}

/* Tells what the session has now, whatever events came: output to read, the end. */
static unsigned
connection_pending(
	const struct terminal_connection *connection)
{
	struct mikuterm_session *session;
	unsigned ready;

	/* The session the connection is. */
	session = (struct mikuterm_session *)connection;

	/* Output, the end and X windows' tabs waiting (a request for the settings comes with the post alone). */
	kl_mutex_lock(&session->lock);

	ready = 0U;
	if (session->output_length != 0U)
		ready |= TERMINAL_READY_OUTPUT;
	if (session->ended)
		ready |= TERMINAL_READY_EXIT;
	if (session->tab_count != 0U)
		ready |= TERMINAL_READY_TAB;

	kl_mutex_unlock(&session->lock);

	/* Reports what is there. */
	return ready;
}

/*
 * Closes the tab's side: the thread is told to end the connection (a
 * question asked is given up), the X windows' tabs the run did not take
 * are closed, and the tab lets go.  It returns at once; the thread lets
 * go when it ends.
 */
static void
connection_close(
	struct terminal_connection *connection)
{
	struct mikuterm_session *session;
	struct x11_tab *tabs[SESSION_TABS];
	unsigned count;
	unsigned index;

	/* The session the connection is. */
	session = (struct mikuterm_session *)connection;

#if MIKUTERM_HAVE_LOGIN_SHELL
	/* The login shell's terminal hangs up, and the shell is let go (the library reaps it). */
	if (session->process != NULL) {
		(void)kl_process_kill(session->process, KL_PROCESS_HANGUP);
		kl_process_close(session->process);
		session->process = NULL;
	}

#endif
	/* The thread ends the connection; a question waits no more; the tabs waiting are taken out. */
	kl_mutex_lock(&session->lock);

	session->closing = 1;
	if (session->question != SESSION_ASK_NONE && !session->answered) {
		session->cancelled = 1;
		session->answered = 1;
	}
	count = session->tab_count;
	memcpy(tabs, session->tabs, count * sizeof(tabs[0]));
	session->tab_count = 0U;

	kl_mutex_unlock(&session->lock);

	/* The X windows' tabs no run will show close, as the user's closing would. */
	for (index = 0U; index < count; index++)
		tabs[index]->connection.ops->close(&tabs[index]->connection);

	/* The thread wakes to it, then the tab lets go. */
	kl_socket_waker_ring(session->waker);
	session_release(session);
}

/*
 * Gives the run the oldest X window's tab the thread opened (NULL when
 * none waits); from now on the run holds it, and the tab's own posts
 * tell what it has.
 */
static struct terminal_connection *
connection_take(
	struct terminal_connection *connection)
{
	struct mikuterm_session *session;
	struct x11_tab *tab;

	/* The session the connection is. */
	session = (struct mikuterm_session *)connection;

	/* The oldest tab waiting, out of the session. */
	kl_mutex_lock(&session->lock);

	tab = NULL;
	if (session->tab_count != 0U) {
		tab = session->tabs[0];
		session->tab_count--;
		memmove(&session->tabs[0], &session->tabs[1], session->tab_count * sizeof(session->tabs[0]));
	}

	kl_mutex_unlock(&session->lock);

	/* None waits. */
	if (tab == NULL)
		return NULL;

	/*
	 * A post the tab made before the run had it went to no one: the
	 * next may go, and the run looks at what the tab has meanwhile.
	 */
	kl_mutex_lock(&tab->lock);

	tab->posted = 0;

	kl_mutex_unlock(&tab->lock);

	/* Succeeded: the run's tab. */
	return &tab->connection;
}

/*
 * Edits the answer's line with the keys typed (the caller holds the
 * lock): text is added and shown (not a secret's), Backspace takes the
 * last character back, Ctrl+U the whole line, Enter answers, Ctrl+C or
 * Ctrl+D give up; the escape sequences of other keys are passed over.
 */
static void
connection_edit(
	struct mikuterm_session *session,
	const unsigned char *bytes,
	size_t size)
{
	size_t index;
	unsigned char byte;
	int shown;

	/* A secret is not shown. */
	shown = 0;
	if (session->question == SESSION_ASK_LINE)
		shown = 1;

	/* Each byte until the line is answered. */
	for (index = 0U; index < size && !session->answered; index++) {
		byte = bytes[index];

		/* An escape sequence's bytes (an arrow, a function key) are passed over to its end. */
		if (session->escape == SESSION_ESCAPE_START) {
			session->escape = SESSION_ESCAPE_NONE;
			if (byte == '[' || byte == 'O')
				session->escape = SESSION_ESCAPE_INSIDE;
			continue;
		}

		/* The rest of an escape sequence ends at its final byte. */
		if (session->escape == SESSION_ESCAPE_INSIDE) {
			if (byte >= 0x40U && byte <= 0x7eU)
				session->escape = SESSION_ESCAPE_NONE;
			continue;
		}

		/* What the key does. */
		switch (byte) {
		case '\r':
		case '\n':
			/* Enter: the line is the answer. */
			session->answered = 1;
			(void)session_put_locked(session, "\r\n", 2U);
			break;
		case 0x03U:
		case 0x04U:
			/* Ctrl+C, Ctrl+D: the question is given up. */
			session->answered = 1;
			session->cancelled = 1;
			(void)session_put_locked(session, "^C\r\n", 4U);
			break;
		case 0x08U:
		case 0x7fU:
			/* Backspace: the last character goes. */
			connection_erase(session);
			break;
		case 0x15U:
			/* Ctrl+U: the whole line goes. */
			while (session->line_length != 0U)
				connection_erase(session);
			break;
		case 0x1bU:
			/* The start of another key's sequence. */
			session->escape = SESSION_ESCAPE_START;
			break;
		case '\t':
			/* Tab switches whether a passphrase is kept, shown under the prompt again (the typed secret stays). */
			if (session->keep_offered) {
				if (session->keep)
					session->keep = 0;
				else
					session->keep = 1;
				(void)session_put_locked(session, "\r\n", 2U);
				session_keep_line_locked(session);
				(void)session_put_locked(session, session->prompt, strlen(session->prompt));
			}

			break;
		default:
			/* Other control characters are not text. */
			if (byte < 0x20U)
				break;

			/* Text, while the line has room; shown unless a secret. */
			if (session->line_length + 1U >= sizeof(session->line))
				break;
			session->line[session->line_length] = (char)byte;
			session->line_length++;
			if (shown)
				(void)session_put_locked(session, &byte, 1U);
			break;
		}
	}
}

/* Takes the last character (all its UTF-8 bytes) off the answer's line, and off the screen unless a secret. */
static void
connection_erase(
	struct mikuterm_session *session)
{
	/* Nothing to take. */
	if (session->line_length == 0U)
		return;

	/* The bytes that continue a character, then its first. */
	while (session->line_length > 1U && ((unsigned char)session->line[session->line_length - 1U] & 0xc0U) == 0x80U)
		session->line_length--;
	session->line_length--;

	/* Off the screen: back, a space over it, back. */
	if (session->question == SESSION_ASK_LINE)
		(void)session_put_locked(session, "\b \b", 3U);
}

/* Frees a session nobody holds: its waker, its lock, its ring, and the answer's line wiped. */
static void
connection_free(
	struct mikuterm_session *session)
{
	/* What it owns. */
	sftp_link_release(session->sftp_link);
	session->sftp_link = NULL;
	kl_socket_waker_close(session->waker);
	kl_mutex_destroy(&session->lock);
	free(session->output);

	/* A password may have been typed into it. */
	OPENSSL_cleanse(session->line, sizeof(session->line));
	free(session);
}

/* Copies a text given to the connector into a session's field; a NULL or too long text leaves it empty. */
static void
connection_copy(
	char *to,
	size_t size,
	const char *text)
{
	size_t length;

	/* Nothing given. */
	to[0] = '\0';
	if (text == NULL)
		return;

	/* Only a text that fits whole. */
	length = strlen(text);
	if (length >= size)
		return;
	memcpy(to, text, length + 1U);
}

#if MIKUTERM_HAVE_LOGIN_SHELL
/*
 * Starts the login shell of a path as the tab's connection, at the grid's
 * size and with the session's TERM.  Returns the TERMINAL_READY_* bits it
 * has at once: none when it runs (its own events tell), or the output
 * when it could not start (the tab says why and stays, for the user to
 * read and close).
 */
static unsigned
connection_shell(
	struct mikuterm_session *session,
	const char *path)
{
	char line[MIKUTERM_PATH_MAX + 64U];
	unsigned columns;
	unsigned rows;
	int error;

	/* The grid's size now. */
	kl_mutex_lock(&session->lock);

	columns = session->columns;
	rows = session->rows;

	kl_mutex_unlock(&session->lock);

	/* The shell on a terminal of its own. */
	error = login_shell_start(session->app, path, session->term, columns, rows, &session->process);
	if (error == 0)
		return 0U;

	/* Why it did not start, in the tab. */
	session->process = NULL;
	(void)snprintf(line, sizeof(line), "mikuterm: %s could not be started: %s\r\n", path, strerror(error));
	kl_mutex_lock(&session->lock);

	(void)session_put_locked(session, line, strlen(line));

	kl_mutex_unlock(&session->lock);

	/* Reports the line to read. */
	return TERMINAL_READY_OUTPUT;
}
#endif
