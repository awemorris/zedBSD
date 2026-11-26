/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * One tab's SSH connection, shared by the application's thread
 * (connection-ssh.c: the tab reads what came, writes the keys typed, gives
 * the grid's size and closes it) and the connection's own thread
 * (ssh-thread.c: it chooses the host, connects, runs the SSH engine and
 * asks the user what the connection needs).
 *
 * What both use is under the session's lock.  The thread hands the tab
 * what to show through the output ring and wakes the application's
 * thread with a post; the tab hands the thread the keys, the size, how
 * much it has read and its end, and rings the waker the thread waits on.
 * While the thread asks a question (a host, a user, a password, a yes or
 * no), the keys typed edit the answer's line here instead of going to the
 * remote side.
 */

#ifndef MIKUTERM_SESSION_H
#define MIKUTERM_SESSION_H

#include "mikuterm.h"
#include "x11-link.h"

#include <keiland/keiland.h>

#include "userland/base/ssh/ssh-channel.h"

/* The code of the posts that say a session has something for its tab. */
#define SESSION_POST		0x6d696b75U

/*
 * The output ring's room: the most the SSH engine lets the tab hold
 * unread (one and a half windows) and room for the questions' text.
 */
#define SESSION_OUTPUT_MAX	(SSH_CHANNEL_WINDOW + SSH_CHANNEL_WINDOW / 2U + 65536U)

/* The keys typed that wait for the thread, at most. */
#define SESSION_INPUT_MAX	65536U

/* The longest answer to a question, in bytes. */
#define SESSION_LINE_MAX	512U

/* The longest prompt of a question kept to be shown again (a key file's path and the words round it). */
#define SESSION_PROMPT_MAX	(MIKUTERM_PATH_MAX + 64U)

/* The tabs of X windows that wait for the run to take them, at most (as many as an X server has). */
#define SESSION_TABS		X11_LINK_TABS

/* An X window's tab (x11-tab.h). */
struct x11_tab;

/* An SFTP channel relay (sftp-link.h). */
struct sftp_link;

/* How much the tab reads before it tells the thread, so the window is given back on time. */
#define SESSION_TAKEN_STEP	65536U

/* The questions: none, a line shown as it is typed, a secret not shown. */
#define SESSION_ASK_NONE	0U
#define SESSION_ASK_LINE	1U
#define SESSION_ASK_SECRET	2U

/* Where the answer's editor is in an escape sequence the keys sent: outside, after ESC, inside one. */
#define SESSION_ESCAPE_NONE	0U
#define SESSION_ESCAPE_START	1U
#define SESSION_ESCAPE_INSIDE	2U

/*
 * One tab's connection.  The tab and the thread each hold a reference;
 * the last to let go frees it.  The fields up to the lock are set when it
 * is made and only read afterwards; the rest are under the lock.
 */
struct mikuterm_session {
	/* The core's handle: the tab's connection (first, so the core's pointer is the session's). */
	struct terminal_connection connection;

	/* The application posts go to, and the number they carry (the session's, not a pointer). */
	struct kl_app *app;
	uint32_t number;

	/* What the thread waits on besides its socket: rung when the tab has something for it. */
	struct kl_socket_waker *waker;

	/*
	 * The host to connect to without asking (empty: ask), the TERM, the
	 * wait for a connection and WSL's sshd port, and the X server's scale,
	 * drawing and PRIMARY (the root window and the tabs are the Host's,
	 * ssh\config).
	 */
	char host[MIKUTERM_HOST_MAX];
	char term[MIKUTERM_TERM_MAX];
	unsigned connect_timeout;
	unsigned wsl_port;
	unsigned x11_scale;
	int x11_render_cpu;
	int x11_primary;
	int sftp;

	/* Problems of the settings to show before anything else (empty: none). */
	char notice[MIKUTERM_PROBLEMS_MAX];

	struct kl_mutex lock;

	/* Who still holds the session: the tab and the thread; 0 frees it. */
	unsigned references;

	/* What the tab has not read yet: a ring of output_capacity bytes. */
	unsigned char *output;
	size_t output_capacity;
	size_t output_start;
	size_t output_length;

	/* The last byte put in the ring ended a line (or none was put yet): the thread's own lines start there. */
	int line_started;

	/* How much the tab read since the thread last took the count (the engine gives the window back from it). */
	size_t taken;

	/* A post is on its way and not taken yet: no other is made until the tab takes it. */
	int posted;

	/* The connection ended as the tab ends: the tab closes once it read everything. */
	int ended;

	/*
	 * The user asked for the settings: the tab's next post asks the run
	 * for the settings' tab (opened beside this one, which goes on), and
	 * the request is then taken.
	 */
	int settings;

	/*
	 * The login shell the user chose on the launch screen, by its path
	 * (empty: none, or taken): the tab's next post starts it on a
	 * terminal of its own, and the thread, which asked for it, has ended.
	 */
	char shell[MIKUTERM_PATH_MAX];

	/*
	 * The login shell running as the tab's connection (NULL: none): the
	 * application's thread's alone, not under the lock.  The tab's keys,
	 * size and end go to it, and it is read after the ring.
	 */
	struct kl_process *process;

	/* The keys typed, for the thread to send; input_full says the tab was refused some. */
	unsigned char input[SESSION_INPUT_MAX];
	size_t input_length;
	int input_full;

	/* The grid's size, and whether it changed since the thread last took it. */
	int resized;
	unsigned columns;
	unsigned rows;
	unsigned width;
	unsigned height;

	/* The tab closed: the thread ends the connection and lets go. */
	int closing;

	/* The SFTP channel relay shared with the SFTP worker and tab (NULL until opened). */
	struct sftp_link *sftp_link;

	/* Whether the SSH session is authenticated and its main channel is open, and the active/pending SFTP/RDP view tabs. */
	int sftp_ready;
	struct terminal_connection *sftp_view;
	struct terminal_connection *sftp_pending_tab;
	struct terminal_connection *rdp_pending_tab;

	/*
	 * The X windows' tabs the thread opened that the run has not taken
	 * yet (the tab's next post says they are there), oldest first; each
	 * holds the run's hold on the X window's tab.
	 */
	struct x11_tab *tabs[SESSION_TABS];
	unsigned tab_count;

	/*
	 * Whether the question offers to keep its answer for the Windows
	 * session (a passphrase), and the user's choice (on by default, Tab
	 * switches it; it stays for the tab's later questions), and the
	 * question's prompt, shown again under the choice when it switches.
	 */
	int keep_offered;
	int keep;
	char prompt[SESSION_PROMPT_MAX];

	/* The question asked (SESSION_ASK_*), its answer being typed, and how it ended. */
	unsigned question;
	char line[SESSION_LINE_MAX];
	size_t line_length;
	unsigned escape;
	int answered;
	int cancelled;

	/* Whether DisplaySink Target Mode is currently active on this session. */
	int target_mode;
};

/* Shared by the two sides (connection-ssh.c). */
void session_release(struct mikuterm_session *session);
void session_post(struct mikuterm_session *session);
size_t session_put_locked(struct mikuterm_session *session, const void *bytes, size_t size);
void session_say(struct mikuterm_session *session, const char *text);
void session_say_line(struct mikuterm_session *session, const char *text);
void session_keep_line_locked(struct mikuterm_session *session);
void session_ended(struct mikuterm_session *session);
void session_settings(struct mikuterm_session *session);
void session_shell(struct mikuterm_session *session, const char *path);
void session_thread_ended(void);
int session_app_post(struct kl_app *app, uint32_t number);
void session_tab(struct mikuterm_session *session, struct x11_tab *tab);
void session_rdp_tab(struct mikuterm_session *session, struct terminal_connection *tab);

/* The thread (ssh-thread.c). */
void session_run(void *argument);

#endif
