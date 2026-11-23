/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The programs an application starts, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_PROCESS_H
#define KEILAND_PROCESS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * KL_VERSION 98: a program the application starts and talks to (a
 * process).  The application writes its standard input and reads its
 * standard output and standard error, or talks to it through a terminal of
 * its own (a shell in a terminal), and hears when it ended and how.
 *
 * What a process has to tell arrives as the application's KL_APP_PROCESS
 * events (event.process, and in event.ready what it is): output waiting on
 * a stream, room again for input, its end.  The output is told for as long
 * as some of it waits: the program reads as much as it likes at each event
 * and hears again at the next dispatch about what it left.  One event may
 * tell several things; the output it tells comes before the end it tells.
 * The library keeps up to KL_PROCESS_BUFFER bytes of each stream, and stops
 * reading a stream that is full until the program reads.
 *
 * Every call is made from the application's thread.  The library waits for
 * its processes and reaps them itself: a program that starts processes does
 * not wait for "any child" of its own.
 */
struct kl_app;
struct kl_process;
struct kl_window;

/* The streams the program uses (kl_process_options' streams); the others are inherited, but the standard input, which is empty. */
#define KL_PROCESS_INPUT	0x1U
#define KL_PROCESS_OUTPUT	0x2U
#define KL_PROCESS_ERROR	0x4U

/*
 * A terminal of the process's own in place of the three: what the
 * program writes is typed into it, and what it shows is read as the
 * process's KL_PROCESS_OUTPUT.
 */
#define KL_PROCESS_TERMINAL	0x8U

/* What a KL_APP_PROCESS event tells (event.ready). */
#define KL_PROCESS_READY_OUTPUT	0x1U
#define KL_PROCESS_READY_ERROR		0x2U
#define KL_PROCESS_READY_INPUT		0x4U
#define KL_PROCESS_READY_EXIT		0x8U

/* How a process ended (kl_process_exit): with an exit code, or by a signal (where the system has them). */
#define KL_PROCESS_RUNNING	0U
#define KL_PROCESS_EXITED	1U
#define KL_PROCESS_SIGNALED	2U

/*
 * How a process is asked to end (kl_process_kill): its terminal hangs up
 * (one without a terminal is asked as with TERMINATE), it is asked to end,
 * or it is ended.
 */
#define KL_PROCESS_HANGUP	1U
#define KL_PROCESS_TERMINATE	2U
#define KL_PROCESS_KILL	3U

/* A terminal's modes (kl_process_terminal_mode): what is typed is echoed, and the input is edited and taken by the line. */
#define KL_PROCESS_TERMINAL_ECHO	0x1U
#define KL_PROCESS_TERMINAL_LINES	0x2U

/*
 * What kl_process_read_line found: a whole line, or the start of a longer
 * one (or of a full stream without a line's end), whose rest comes next.
 */
#define KL_PROCESS_LINE	1
#define KL_PROCESS_LINE_CUT	2

/* The bytes kept of each stream, and of the input waiting to be written. */
#define KL_PROCESS_BUFFER	65536U

/*
 * What a process is started with: its arguments (argv[0] is the program,
 * a path or a name looked for in PATH; NULL ends them), the changes to the
 * application's environment (NULL for none: "NAME=value" sets a variable,
 * "NAME" removes it; NULL ends them), the directory it starts in (NULL: the
 * application's), the streams the program uses (KL_PROCESS_*; 0 for none),
 * a terminal's size in characters, and the program's own pointer
 * (kl_process_data).
 */
struct kl_process_options {
	const char *const *argv;
	const char *const *environment;
	const char *directory;
	unsigned streams;
	unsigned columns;
	unsigned rows;
	void *data;
};

/*
 * Starts a process.  It runs when this returns.  Returns 0 with *process
 * set, or EINVAL, ENOENT or EACCES (the program cannot be found or run),
 * ENOMEM or EAGAIN.  A program the system can run only to fail in it ends
 * with the code 127.
 */
int kl_process_start(struct kl_app *app, const struct kl_process_options *options, struct kl_process **process);

/*
 * Writes bytes to the process's input (or types them into its terminal):
 * they are copied, and *written tells how many were taken -- fewer when
 * the input waiting is full, and a KL_PROCESS_READY_INPUT comes when there
 * is room again.  Returns 0, EPIPE when the input is closed or the process
 * ended, or EINVAL.
 */
int kl_process_write(struct kl_process *process, const void *bytes, size_t size, size_t *written);

/*
 * Closes the process's input once what waits is written: it reads the
 * end.  Returns 0, or EINVAL (no input, or a terminal's).
 */
int kl_process_close_input(struct kl_process *process);

/*
 * Reads up to size bytes of a stream (KL_PROCESS_OUTPUT or
 * KL_PROCESS_ERROR).  Returns how many were read, 0 when none waits.
 */
size_t kl_process_read(struct kl_process *process, unsigned stream, void *buffer, size_t size);

/*
 * Reads a line of a stream into line (size bytes with its NUL; its end,
 * "\n" or "\r\n", is not kept).  Returns KL_PROCESS_LINE, KL_PROCESS_LINE_CUT
 * for the first size - 1 bytes of a longer line (its rest is read next),
 * or 0 when no whole line waits.  The last line of a stream that ended is
 * whole without an end.
 */
int kl_process_read_line(struct kl_process *process, unsigned stream, char *line, size_t size);

/*
 * Tells whether a process ended and how: KL_PROCESS_RUNNING, or
 * KL_PROCESS_EXITED with its exit code in *code, or KL_PROCESS_SIGNALED with
 * the signal's number.  code may be NULL.
 */
unsigned kl_process_exit(const struct kl_process *process, int *code);

/*
 * Asks a process to end (KL_PROCESS_HANGUP, KL_PROCESS_TERMINATE,
 * KL_PROCESS_KILL).  Returns 0 (also when it ended already), EPERM when it
 * may not be asked (a program that runs as another user), or EINVAL.
 */
int kl_process_kill(struct kl_process *process, unsigned how);

/*
 * Tells the process's terminal its new size in characters.  Returns 0,
 * ENOTSUP without a terminal, or EINVAL.
 */
int kl_process_resize(struct kl_process *process, unsigned columns, unsigned rows);

/*
 * Reports the modes the process set on its terminal (KL_PROCESS_TERMINAL_*):
 * a password asked for is typed without an echo but by the line.  Returns
 * 0, ENOTSUP without a terminal or where the system does not tell, or EIO
 * when the terminal hung up.
 */
int kl_process_terminal_mode(const struct kl_process *process, unsigned *mode);

/* The program's pointer the process was started with. */
void *kl_process_data(const struct kl_process *process);

/*
 * The program is done with a process: its streams close (a terminal hangs
 * up), its events still queued go, and none comes any more.  The process is
 * not ended by it; the library reaps it when it ends.
 */
void kl_process_close(struct kl_process *process);

/*
 * KL_VERSION 100: a program started apart from the application (a
 * launch), such as another program's window opened on a file or a new
 * window of the same program.  It runs on when the application ends; its
 * standard input is empty, it writes where the application does, and
 * nothing of it comes back: a program that wants to hear of its end starts
 * it as a process.
 *
 * What a launch is started with: its arguments (argv[0] is the program, a
 * path or a name looked for in PATH; NULL ends them) or a command line the
 * system's command interpreter runs (exactly one of the two), the changes
 * to the application's environment (as a process's), the directory it
 * starts in (NULL: the application's), and the window the user works in
 * (NULL for none): the compositor is asked for leave for the program's
 * window to come to the front from it.
 */
struct kl_launch_options {
	const char *const *argv;
	const char *command;
	const char *const *environment;
	const char *directory;
	struct kl_window *from;
};

/*
 * Starts a program apart from the application.  It is on its way when this
 * returns.  Returns 0, or EINVAL, ENOENT or EACCES (the program cannot be
 * found or run), ENOMEM, or another error of the system's start.
 */
int kl_launch(struct kl_app *app, const struct kl_launch_options *options);

/*
 * KL_VERSION 111: a system action, a program the system's definition names
 * (/etc/keiland/admin.d on zedBSD), run as the system's administrator once
 * the person at the screen has allowed it in the compositor's own dialog.
 * It is a process as kl_process_start's are, but for these: its standard
 * input is always a pipe (when it is not asked for, the application can
 * only close it), the streams not asked for are the null device, it has no
 * terminal, and it may not be signalled (kl_process_kill: EPERM).  The
 * program stops when it reads its input's end: when the process is closed,
 * its input closed, or the application ends.
 */
struct kl_system;

/*
 * How a process ended (kl_process_exit), besides the above: the action did
 * not run (*code is a KL_ADMIN_* reason); or it ran, but how it ended was
 * not told (*code is 0): what it changed is not known.
 */
#define KL_PROCESS_REFUSED	3U
#define KL_PROCESS_LOST	4U

/*
 * What a KL_APP_PROCESS event tells besides (event.ready): a system
 * action's dialog was answered and its program started; its streams'
 * news follows.
 */
#define KL_PROCESS_READY_START		0x10U

/* Why an action did not run. */
#define KL_ADMIN_NO		1	/* the person said no, or closed the dialog */
#define KL_ADMIN_TIMEOUT	2	/* nobody answered in time */
#define KL_ADMIN_UNKNOWN	3	/* no such action, or words or streams it does not list */
#define KL_ADMIN_NOT_ALLOWED	4	/* this user may not run it */
#define KL_ADMIN_BUSY		5	/* another request or dialog is under way */
#define KL_ADMIN_NOT_FRONT	6	/* the window is not the one the person works in */
#define KL_ADMIN_LOCKED		7	/* the screen locked */
#define KL_ADMIN_FAILED		8	/* the program could not be started */
#define KL_ADMIN_UNSUPPORTED	9	/* no session manager runs actions here, or the dialog cannot show now */
#define KL_ADMIN_METHOD		10	/* the action asks for a way of confirming not made yet */

/*
 * What an action is asked with: its name, the words after its program
 * (NULL ends them; NULL for none), the streams the program uses
 * (KL_PROCESS_INPUT, _OUTPUT, _ERROR; the definition must allow them), the
 * window the person works in (it must have the keyboard, or get it within
 * a few seconds), and the program's own pointer (kl_process_data).
 */
struct kl_admin_options {
	const char *action;
	const char *const *argv;
	unsigned streams;
	struct kl_window *window;
	void *data;
};

/*
 * Asks for an action through a system of the application's connection
 * (kl_app_system).  Returns 0 with *process set at once: the dialog may
 * still be showing.  Its answer comes as the process's events: a refusal as
 * KL_PROCESS_READY_EXIT with kl_process_exit giving KL_PROCESS_REFUSED; a
 * start as KL_PROCESS_READY_START, then the streams' events, and the end
 * as KL_PROCESS_EXITED or KL_PROCESS_SIGNALED (or KL_PROCESS_LOST).  Input
 * written before the start is kept (up to KL_PROCESS_BUFFER) and written
 * once it starts; after a refusal a write is EPIPE.  While an answer is
 * awaited the application's dispatch reads the system's events itself.
 * Returns EINVAL, ENOMEM, or ENOTSUP where the system runs no actions.
 */
int kl_process_start_admin(struct kl_app *app, struct kl_system *system, const struct kl_admin_options *options, struct kl_process **process);

#ifdef __cplusplus
}
#endif

#endif
