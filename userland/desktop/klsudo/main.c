/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * klsudo: runs a system action as the administrator, as pkexec does: the
 * program the system's definition names for the action
 * (/etc/keiland/admin.d/ACTION), with the words given, once the person at
 * the screen has allowed it in the compositor's own dialog.
 *
 *   klsudo [-n] [-s STREAMS] ACTION [WORD ...]
 *
 *   -n          klsudo's standard input is not passed on: the program's
 *               input stays open while klsudo runs (a desktop program,
 *               whose input is its life line)
 *   -s STREAMS  the program's streams klsudo asks for and passes on to its
 *               own standard output and error: "output,error" (the
 *               default), "output", "error" or "none"; the definition must
 *               allow them
 *
 * The request needs a window that has the keyboard, so klsudo shows a small
 * one while the dialog is asked, and closes it once the program runs.  A
 * desktop program (a definition's "display wayland") gets its own
 * connection from the compositor.  klsudo's standard input goes to the
 * program's (its end closes the program's input), the program's output and
 * error to klsudo's.
 *
 * The exit status is the program's, 128 + the signal that ended it, 125
 * when its end was not told (what it changed is not known), 126 when the
 * action did not run (the reason on standard error), or 127 when klsudo
 * could not ask (a wrong use, no desktop, no system actions here).
 */

#include <keiland/keiland.h>

#include "userland/desktop/paths.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The exit statuses of klsudo itself. */
#define KLSUDO_LOST		125
#define KLSUDO_REFUSED		126
#define KLSUDO_CANNOT		127

/* The fonts. */
#define KLSUDO_FONT		KEILAND_DATADIR "/fonts/keiland.ttf"
#define KLSUDO_FALLBACK_FONT	KEILAND_DATADIR "/fonts/keiland-fallback.ttf"

/* The window's size until the compositor gives one, and its texts' sizes. */
#define KLSUDO_WIDTH		460U
#define KLSUDO_HEIGHT		120U
#define KLSUDO_TITLE_PIXELS	17U
#define KLSUDO_TEXT_PIXELS	14U

/* The most words after the action, and the bytes of the action and its words shown. */
#define KLSUDO_WORDS_MAX	8
#define KLSUDO_SHOWN_MAX	320U

/* The bytes of standard input handed over at once, and of output moved at once. */
#define KLSUDO_CHUNK		4096U

/* The code of the input thread's post: a chunk of standard input waits. */
#define KLSUDO_POST_INPUT	1U

/*
 * klsudo's run.  The fields above the lock are the main thread's; the
 * lock holds the hand-over of standard input between the input thread and
 * the main thread: the chunk read (its bytes and size, 0 for the input's
 * end) while busy, which the main thread writes from offset on and gives
 * back (busy 0, room signalled), and stopping, which keeps the input
 * thread from posting once the application closes.
 */
struct klsudo {
	/* The words asked for: the action, its words and the streams; whether standard input is passed on. */
	const char *action;
	const char *words[KLSUDO_WORDS_MAX + 1];
	unsigned streams;
	int pass_input;
	char shown[KLSUDO_SHOWN_MAX];

	/* The application, its window while the dialog is asked, its text and its frame. */
	struct kl_app *app;
	struct kl_window *window;
	struct kl_text text;
	int text_open;
	uint32_t *pixels;
	uint32_t width;
	uint32_t height;

	/* The action's process, whether it started and ended, and whether its input broke. */
	struct kl_process *process;
	int started;
	int ended;
	int input_broken;

	/* The chunk being written to the program's input, from offset on. */
	size_t offset;

	struct kl_mutex lock;
	struct kl_cond room;
	unsigned char chunk[KLSUDO_CHUNK];
	size_t chunk_size;
	int busy;
	int stopping;
};

int main(int argc, char **argv);
static int klsudo_parse(struct klsudo *run, int argc, char **argv);
static int klsudo_streams(const char *text, unsigned *streams);
static int klsudo_ask(struct klsudo *run);
static int klsudo_loop(struct klsudo *run);
static int klsudo_draw(struct klsudo *run);
static void klsudo_window_close(struct klsudo *run);
static void klsudo_process(struct klsudo *run, unsigned ready);
static void klsudo_copy(struct klsudo *run, unsigned stream, int descriptor);
static void klsudo_input(struct klsudo *run);
static void klsudo_input_done(struct klsudo *run);
static int klsudo_input_start(struct klsudo *run);
static void klsudo_input_run(void *argument);
static void klsudo_input_stop(struct klsudo *run);
static int klsudo_status(struct klsudo *run);
static const char *klsudo_reason(int reason);
static void klsudo_write_all(int descriptor, const unsigned char *bytes, size_t size);

/* klsudo's run, for the program's one run (the input thread reaches it through its argument). */
static struct klsudo klsudo_run;

/*
 * Asks for the action and runs it.
 */
int
main(
	int argc,
	char **argv)
{
	struct klsudo *run;
	int status;
	int error;

	/* The command line. */
	run = &klsudo_run;
	status = klsudo_parse(run, argc, argv);
	if (status != 0) {
		fprintf(stderr, "usage: klsudo [-n] [-s output,error|output|error|none] action [word ...]\n");
		return KLSUDO_CANNOT;
	}

	/* A write to a closed pipe does not end klsudo. */
	(void)signal(SIGPIPE, SIG_IGN);

	/* The hand-over of standard input. */
	error = kl_mutex_init(&run->lock);
	if (error == 0)
		error = kl_cond_init(&run->room);
	if (error != 0) {
		fprintf(stderr, "klsudo: %s\n", strerror(error));
		return KLSUDO_CANNOT;
	}

	/* The application, its window and the request. */
	status = klsudo_ask(run);

	/* The run until the program ended, unless the request could not be made. */
	if (status == 0)
		status = klsudo_loop(run);

	/* Everything goes: the input thread posts no more, the process closes (the program reads its input's end). */
	klsudo_input_stop(run);
	if (run->process != NULL)
		kl_process_close(run->process);
	klsudo_window_close(run);
	if (run->app != NULL)
		kl_app_close(run->app);
	if (run->text_open)
		kl_text_close(&run->text);

	/* How it ended. */
	return status;
}

/* Reads the command line; nonzero for a wrong one. */
static int
klsudo_parse(
	struct klsudo *run,
	int argc,
	char **argv)
{
	size_t used;
	size_t length;
	int index;
	int count;
	int same;
	int error;

	/* The defaults: standard input passed on, the output and the error asked for. */
	run->pass_input = 1;
	run->streams = KL_PROCESS_INPUT | KL_PROCESS_OUTPUT | KL_PROCESS_ERROR;

	/* The options, before the action. */
	for (index = 1; index < argc; index++) {
		same = strcmp(argv[index], "-n");
		if (same == 0) {
			run->pass_input = 0;
			continue;
		}

		/* The streams, in the next argument. */
		same = strcmp(argv[index], "-s");
		if (same == 0) {
			if (index + 1 >= argc)
				return -1;
			index++;
			error = klsudo_streams(argv[index], &run->streams);
			if (error != 0)
				return -1;
			continue;
		}

		/* The end of the options. */
		same = strcmp(argv[index], "--");
		if (same == 0) {
			index++;
			break;
		}

		/* Another option is wrong; anything else is the action. */
		if (argv[index][0] == '-')
			return -1;
		break;
	}

	/* The action and up to KLSUDO_WORDS_MAX words. */
	if (index >= argc)
		return -1;
	run->action = argv[index];
	index++;
	count = argc - index;
	if (count > KLSUDO_WORDS_MAX)
		return -1;
	memcpy(run->words, argv + index, (size_t)count * sizeof(run->words[0]));
	run->words[count] = NULL;

	/* Without standard input passed on, the program's input is only closed. */
	if (!run->pass_input)
		run->streams &= ~KL_PROCESS_INPUT;

	/* What the window shows: the action and its words. */
	used = (size_t)snprintf(run->shown, sizeof(run->shown), "%s", run->action);
	for (count = 0; run->words[count] != NULL && used < sizeof(run->shown); count++) {
		length = (size_t)snprintf(run->shown + used, sizeof(run->shown) - used, " %s", run->words[count]);
		used += length;
	}

	/* Succeeded: the command line is read. */
	return 0;
}

/* Reads the streams asked for (besides the input); nonzero for a wrong word. */
static int
klsudo_streams(
	const char *text,
	unsigned *streams)
{
	int same;

	/* The input is the life line, asked for unless -n. */
	*streams = KL_PROCESS_INPUT;

	/* Each of the four words. */
	same = strcmp(text, "output,error");
	if (same == 0) {
		*streams |= KL_PROCESS_OUTPUT | KL_PROCESS_ERROR;
		return 0;
	}
	same = strcmp(text, "output");
	if (same == 0) {
		*streams |= KL_PROCESS_OUTPUT;
		return 0;
	}
	same = strcmp(text, "error");
	if (same == 0) {
		*streams |= KL_PROCESS_ERROR;
		return 0;
	}
	same = strcmp(text, "none");
	if (same == 0)
		return 0;

	/* Anything else is wrong. */
	return -1;
}

/*
 * Opens the application and the window, and asks for the action.
 * Returns 0, or the exit status when it could not be asked.
 */
static int
klsudo_ask(
	struct klsudo *run)
{
	struct kl_window_options window_options;
	struct kl_admin_options admin_options;
	struct kl_app_options app_options;
	struct kl_system *system;
	int status;
	int error;

	/* The application: the connection and its globals. */
	memset(&app_options, 0, sizeof(app_options));
	app_options.application = "klsudo";
	run->app = kl_app_open(&app_options);
	if (run->app == NULL) {
		fprintf(stderr, "klsudo: no desktop to ask on (%s)\n", strerror(errno));
		return KLSUDO_CANNOT;
	}

	/* The system that runs the actions. */
	system = kl_app_system(run->app);
	if (system == NULL) {
		fprintf(stderr, "klsudo: the desktop runs no system actions here\n");
		return KLSUDO_CANNOT;
	}

	/* The words in the desktop's language, and the font; without the font the window shows no words. */
	(void)kl_tr_open("klsudo", kl_app_language(run->app));
	error = kl_text_open(&run->text, KLSUDO_FONT, KLSUDO_FALLBACK_FONT);
	if (error == 0)
		run->text_open = 1;

	/* The window the request is made from: the dialog is asked for the window that has the keyboard. */
	memset(&window_options, 0, sizeof(window_options));
	window_options.title = kl_tr("Administrator permission");
	window_options.width = KLSUDO_WIDTH;
	window_options.height = KLSUDO_HEIGHT;
	window_options.present = KL_PRESENT_SHM;
	run->window = kl_app_window_create(run->app, &window_options);
	if (run->window == NULL) {
		fprintf(stderr, "klsudo: no window (%s)\n", strerror(errno));
		return KLSUDO_CANNOT;
	}

	/* Its first frame. */
	status = klsudo_draw(run);
	if (status != 0) {
		fprintf(stderr, "klsudo: the window cannot be shown\n");
		return KLSUDO_CANNOT;
	}

	/* The request. */
	memset(&admin_options, 0, sizeof(admin_options));
	admin_options.action = run->action;
	admin_options.argv = run->words;
	admin_options.streams = run->streams;
	admin_options.window = run->window;
	error = kl_process_start_admin(run->app, system, &admin_options, &run->process);
	if (error == ENOTSUP) {
		fprintf(stderr, "klsudo: the desktop runs no system actions here\n");
		return KLSUDO_CANNOT;
	}
	if (error != 0) {
		fprintf(stderr, "klsudo: %s: cannot be asked (%s)\n", run->action, strerror(error));
		return KLSUDO_CANNOT;
	}

	/* Standard input to the program's, by a thread of its own. */
	if (run->pass_input) {
		error = klsudo_input_start(run);
		if (error != 0) {
			fprintf(stderr, "klsudo: the standard input cannot be passed on (%s)\n", strerror(error));
			return KLSUDO_CANNOT;
		}
	}

	/* Succeeded: asked. */
	return 0;
}

/* Runs until the program ended; returns the exit status. */
static int
klsudo_loop(
	struct klsudo *run)
{
	struct kl_app_event event;
	int taken;
	int status;

	/* Each round: what happened, until the end. */
	while (!run->ended) {
		status = kl_app_dispatch(run->app, -1);
		if (status != 0) {
			fprintf(stderr, "klsudo: the desktop went away\n");
			return KLSUDO_LOST;
		}

		/* Every event. */
		for (;;) {
			taken = kl_app_take(run->app, &event);
			if (!taken)
				break;

			/* The process's news. */
			if (event.kind == KL_APP_PROCESS && event.process == run->process) {
				klsudo_process(run, event.ready);
				continue;
			}

			/* A chunk of standard input. */
			if (event.kind == KL_APP_POST && event.code == KLSUDO_POST_INPUT) {
				klsudo_input(run);
				continue;
			}

			/* The appearance changed: the window again. */
			if (event.kind == KL_APP_THEME && run->window != NULL) {
				(void)klsudo_draw(run);
				continue;
			}

			/* Only the window's own inputs are left. */
			if (event.kind != KL_APP_WINDOW ||
			    run->window == NULL ||
			    event.window != run->window)
				continue;

			/* A new size: drawn again. */
			if (event.input.kind == KL_WINDOW_RESIZE) {
				(void)klsudo_draw(run);
				continue;
			}

			/* Closed before the program started: the request is taken back. */
			if (event.input.kind == KL_WINDOW_CLOSE && !run->started) {
				fprintf(stderr, "klsudo: %s: not run (the window was closed)\n", run->action);
				return KLSUDO_REFUSED;
			}
		}
	}

	/* How the program ended. */
	return klsudo_status(run);
}

/* Draws the window: what it waits for, and the action asked.  Returns 0, or -1 when it cannot be shown. */
static int
klsudo_draw(
	struct klsudo *run)
{
	const struct kl_theme *theme;
	struct kl_canvas canvas;
	struct kl_rect all;
	uint32_t *pixels;
	uint32_t width;
	uint32_t height;
	const char *title;
	int status;

	/* The size the presenter takes. */
	status = kl_window_present_resize(run->window, &width, &height);
	if (status != 0)
		return -1;

	/* A frame of that size. */
	if (run->pixels == NULL || width != run->width || height != run->height) {
		pixels = realloc(run->pixels, (size_t)width * (size_t)height * sizeof(*pixels));
		if (pixels == NULL)
			return -1;
		run->pixels = pixels;
		run->width = width;
		run->height = height;
	}
	status = kl_canvas_init(&canvas, run->pixels, (size_t)width, (int)width, (int)height);
	if (status != 0)
		return -1;

	/* The ground. */
	theme = kl_theme_default();
	all.x = 0;
	all.y = 0;
	all.width = (int)width;
	all.height = (int)height;
	kl_canvas_gradient(&canvas, &all, theme->ground_top, theme->ground_bottom);

	/* What it waits for, and the action. */
	if (run->text_open) {
		title = kl_tr("Waiting for the administrator's permission");
		(void)kl_text_draw_fit(&run->text, &canvas, 24, 44, title, KLSUDO_TITLE_PIXELS, 1, (int)width - 48, theme->text);
		(void)kl_text_draw_fit(&run->text, &canvas, 24, 78, run->shown, KLSUDO_TEXT_PIXELS, 0, (int)width - 48, theme->text_secondary);
	}

	/* Shown. */
	status = kl_window_present(run->window, run->pixels, (size_t)width);
	kl_canvas_release(&canvas);
	if (status != 0)
		return -1;

	/* Succeeded. */
	return 0;
}

/* Closes the window, once the program runs or klsudo ends. */
static void
klsudo_window_close(
	struct klsudo *run)
{
	/* The window and its frame. */
	if (run->window != NULL)
		kl_window_close(run->window);
	run->window = NULL;
	free(run->pixels);
	run->pixels = NULL;
}

/* Takes the process's news: its start, its streams' bytes, its input's room and its end. */
static void
klsudo_process(
	struct klsudo *run,
	unsigned ready)
{
	/* Started: the window is not needed any more. */
	if ((ready & KL_PROCESS_READY_START) != 0U) {
		run->started = 1;
		klsudo_window_close(run);
	}

	/* The output and the error, to klsudo's. */
	if ((ready & KL_PROCESS_READY_OUTPUT) != 0U)
		klsudo_copy(run, KL_PROCESS_OUTPUT, STDOUT_FILENO);
	if ((ready & KL_PROCESS_READY_ERROR) != 0U)
		klsudo_copy(run, KL_PROCESS_ERROR, STDERR_FILENO);

	/* Room for more input. */
	if ((ready & KL_PROCESS_READY_INPUT) != 0U)
		klsudo_input(run);

	/* The end, after what is left of the streams. */
	if ((ready & KL_PROCESS_READY_EXIT) != 0U) {
		klsudo_copy(run, KL_PROCESS_OUTPUT, STDOUT_FILENO);
		klsudo_copy(run, KL_PROCESS_ERROR, STDERR_FILENO);
		run->ended = 1;
	}
}

/* Copies what waits of a stream to a descriptor of klsudo's. */
static void
klsudo_copy(
	struct klsudo *run,
	unsigned stream,
	int descriptor)
{
	unsigned char bytes[KLSUDO_CHUNK];
	size_t got;

	/* Each part until none waits. */
	for (;;) {
		got = kl_process_read(run->process, stream, bytes, sizeof(bytes));
		if (got == 0U)
			break;
		klsudo_write_all(descriptor, bytes, got);
	}
}

/*
 * Writes the chunk of standard input that waits to the program's input, as
 * much as it takes; the chunk is given back once written (or once the
 * input broke), and its end closes the program's input.
 */
static void
klsudo_input(
	struct klsudo *run)
{
	size_t written;
	int error;

	/* Only a chunk handed over. */
	kl_mutex_lock(&run->lock);

	if (!run->busy) {
		kl_mutex_unlock(&run->lock);
		return;
	}

	kl_mutex_unlock(&run->lock);

	/* Nowhere to go: dropped. */
	if (run->input_broken || run->ended) {
		klsudo_input_done(run);
		return;
	}

	/* The input's end closes the program's input. */
	if (run->chunk_size == 0U) {
		(void)kl_process_close_input(run->process);
		klsudo_input_done(run);
		return;
	}

	/* As much as the input takes; the rest when it has room. */
	error = kl_process_write(run->process, run->chunk + run->offset, run->chunk_size - run->offset, &written);
	if (error != 0) {
		run->input_broken = 1;
		klsudo_input_done(run);
		return;
	}
	run->offset += written;
	if (run->offset < run->chunk_size)
		return;

	/* Written whole. */
	klsudo_input_done(run);
}

/* Gives the chunk back to the input thread. */
static void
klsudo_input_done(
	struct klsudo *run)
{
	/* Free again, and the thread told. */
	run->offset = 0;
	kl_mutex_lock(&run->lock);

	run->busy = 0;
	kl_cond_signal(&run->room);

	kl_mutex_unlock(&run->lock);
}

/* Starts the thread that reads standard input.  Returns 0 or an errno value. */
static int
klsudo_input_start(
	struct klsudo *run)
{
	struct kl_thread_options options;
	struct kl_thread *thread;
	int error;

	/* The thread, given back at once: it ends by itself or with the program. */
	memset(&options, 0, sizeof(options));
	options.name = "klsudo-input";
	options.run = klsudo_input_run;
	options.argument = run;
	error = kl_thread_start(&options, &thread);
	if (error != 0)
		return error;
	(void)kl_thread_detach(thread);

	/* Succeeded. */
	return 0;
}

/*
 * The thread that reads standard input: a chunk at a time, handed to the
 * main thread and waited for, until the input's end or klsudo stops.
 */
static void
klsudo_input_run(
	void *argument)
{
	struct klsudo *run;
	ssize_t got;
	size_t size;
	int stopping;

	/* klsudo's run. */
	run = argument;

	/* Each chunk, until the end. */
	for (;;) {
		/* Waits for the last chunk to be written. */
		kl_mutex_lock(&run->lock);

		while (run->busy && !run->stopping)
			kl_cond_wait(&run->room, &run->lock);
		stopping = run->stopping;

		kl_mutex_unlock(&run->lock);
		if (stopping)
			return;

		/* The next bytes (none at the end, or when it cannot be read). */
		do {
			got = read(STDIN_FILENO, run->chunk, sizeof(run->chunk));
		} while (got < 0 && errno == EINTR);
		size = 0;
		if (got > 0)
			size = (size_t)got;

		/* Handed over, unless klsudo stopped meanwhile. */
		kl_mutex_lock(&run->lock);

		stopping = run->stopping;
		if (!stopping) {
			run->chunk_size = size;
			run->busy = 1;
			(void)kl_app_post(run->app, KLSUDO_POST_INPUT, NULL);
		}

		kl_mutex_unlock(&run->lock);

		/* The input's end, or klsudo stopped. */
		if (stopping || size == 0U)
			return;
	}
}

/* Keeps the input thread from posting any more (before the application closes). */
static void
klsudo_input_stop(
	struct klsudo *run)
{
	/* Stopping, and the thread told if it waits. */
	kl_mutex_lock(&run->lock);

	run->stopping = 1;
	kl_cond_broadcast(&run->room);

	kl_mutex_unlock(&run->lock);
}

/* Tells how the program ended as klsudo's exit status, and why when it did not run. */
static int
klsudo_status(
	struct klsudo *run)
{
	unsigned how;
	int code;

	/* How it ended. */
	how = kl_process_exit(run->process, &code);

	/* Each way. */
	switch (how) {
	case KL_PROCESS_EXITED:
		return code;
	case KL_PROCESS_SIGNALED:
		return 128 + code;
	case KL_PROCESS_REFUSED:
		fprintf(stderr, "klsudo: %s: not run: %s\n", run->action, klsudo_reason(code));
		return KLSUDO_REFUSED;
	default:
		break;
	}

	/* Its end was not told. */
	fprintf(stderr, "klsudo: %s: how it ended is not known\n", run->action);
	return KLSUDO_LOST;
}

/* Tells why an action did not run, in words. */
static const char *
klsudo_reason(
	int reason)
{
	/* Each reason. */
	switch (reason) {
	case KL_ADMIN_NO:
		return "the permission was not given";
	case KL_ADMIN_TIMEOUT:
		return "nobody answered in time";
	case KL_ADMIN_UNKNOWN:
		return "no such action, or words or streams it does not list";
	case KL_ADMIN_NOT_ALLOWED:
		return "this user may not run it";
	case KL_ADMIN_BUSY:
		return "another request is under way";
	case KL_ADMIN_NOT_FRONT:
		return "the window did not get the keyboard";
	case KL_ADMIN_LOCKED:
		return "the screen locked";
	case KL_ADMIN_FAILED:
		return "the program could not be started";
	case KL_ADMIN_UNSUPPORTED:
		return "the dialog cannot be shown now";
	case KL_ADMIN_METHOD:
		return "the action asks for a way of confirming not made yet";
	default:
		break;
	}

	/* A reason not known here. */
	return "refused";
}

/* Writes all the bytes to a descriptor (what cannot be written is dropped). */
static void
klsudo_write_all(
	int descriptor,
	const unsigned char *bytes,
	size_t size)
{
	ssize_t written;

	/* Each part until all are written or the descriptor fails. */
	while (size != 0U) {
		written = write(descriptor, bytes, size);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return;
		bytes += written;
		size -= (size_t)written;
	}
}
