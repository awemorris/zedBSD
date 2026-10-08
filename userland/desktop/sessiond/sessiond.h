/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The graphical login's session manager (plan/ws035/login-manager-design.md).
 */

#ifndef SESSIOND_H
#define SESSIOND_H

#include <pwd.h>
#include <signal.h>
#include <sys/types.h>
#include <time.h>

/* The greeter program and the session script sessiond starts by default. */
#define SESSIOND_GREETER	"/bin/wayland"
#define SESSIOND_SESSION	"/etc/keiland/session"

/* The unprivileged account the greeter runs as. */
#define SESSIOND_GREETER_USER	"_greeter"

/* The wallpaper the greeter shows when the image has one. */
#define SESSIOND_WALLPAPER	"/usr/share/keiland/wallpaper.png"

/* The descriptor the greeter talks to sessiond on, and the one the session does (ws035-p101). */
#define SESSIOND_AUTH_FD	3
#define SESSIOND_CONTROL_FD	3

/* The longest line of the greeter's protocol, and the longest user name. */
#define SESSIOND_LINE_MAX	512U
#define SESSIOND_NAME_MAX	64U

/* The size of the buffer an account's strings are kept in. */
#define SESSIOND_ACCOUNT_BUFFER	2048U

/*
 * How a greeter ended: a user logged in, it failed, it ended by itself, or
 * sessiond is being stopped.
 */
enum sessiond_greeter_end {
	SESSIOND_GREETER_LOGIN,
	SESSIOND_GREETER_FAILED,
	SESSIOND_GREETER_ENDED,
	SESSIOND_GREETER_STOP
};

/*
 * One seat's user: who owns the display and the input devices, and the
 * buffer passwd's strings for the account live in.
 */
struct sessiond_account {
	struct passwd passwd;
	char buffer[SESSIOND_ACCOUNT_BUFFER];
};

/*
 * What sessiond was started with: the greeter program and the session
 * script.  A greeter that outlives the step that started it (ws035-p101:
 * after a login it stays on the screen until the session is ready for the
 * display; at a Log Out it starts while the session still shows): its
 * process (0 for none), sessiond's end of its socket, and when it started.
 */
struct sessiond {
	const char *greeter;
	const char *session;
	pid_t greeter_pid;
	int greeter_socket;
	time_t greeter_started;
};

/* Set by SIGTERM and SIGINT: sessiond ends its greeter or session and stops. */
extern volatile sig_atomic_t sessiond_stopping;

enum sessiond_greeter_end sessiond_greeter_run(struct sessiond *daemon, struct sessiond_account *account);
void sessiond_greeter_finish(struct sessiond *daemon);
void sessiond_greeter_release(struct sessiond *daemon);
int sessiond_greeter_prepare(struct sessiond *daemon);
void sessiond_greeter_go(struct sessiond *daemon);
int sessiond_read_line(int descriptor, char *line, size_t size, int timeout_ms);
long long sessiond_milliseconds(void);
int sessiond_session_run(struct sessiond *daemon, struct sessiond_account *account);
void sessiond_seat_give(uid_t uid, gid_t gid, int keys);
void sessiond_seat_restore(void);

/* The system's events of the seat's devices: a device that comes is given at once (seat.c, BUG-264). */
void sessiond_seat_events_open(void);
int sessiond_seat_events_fd(void);
int sessiond_seat_events_collect(void);

void sessiond_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* Ending the machine: the login screen's and a session's POWER (power.c, ws131-p027); nonzero once it is ending. */
extern int sessiond_power_started;
int sessiond_power_run(const char *program, const char *what, const char *from);
void sessiond_power_session(const struct sessiond_account *account, int control, const char *what);

/* The machine's sleep: POWER suspend and POWER cancel (sleep.c, ws052-p011). */
void sessiond_sleep_request(int control, const char *from);
void sessiond_sleep_cancel(void);
int sessiond_sleep_fd(void);
void sessiond_sleep_collect(void);
void sessiond_sleep_forget(int control);

/* A session's SERVICE request (service.c, ws089-p025). */
void sessiond_service(const struct sessiond_account *account, int control, const char *arguments);

#endif
