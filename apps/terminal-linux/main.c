/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * mikuterm (Micro Unicode Terminal) for Linux: zedBSD's terminal as a
 * program of a Wayland session whose tabs are SSH connections.
 *
 * It keeps the user's files in its folder (folder.c): it reads
 * config/mikuterm.conf, finds its fonts, and runs the terminal (run.h)
 * drawn on the processor and shown by libkeiland, each tab connecting on
 * a thread of its own (connection-ssh.c, ssh-thread.c).  New Window
 * starts the program again.  A run that fails says what failed on the
 * standard error.  Started with --x11-server, it is instead the X server
 * process of one connection's X11 forwarding (x11-child.c).
 */

#include "mikuterm.h"
#include "x11-host.h"
#include "x11-spawn.h"

#include "userland/desktop/terminal/run.h"

#include <keiland/keiland.h>

#include "mikuterm-version.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The user's settings and the View menu's, inside the folder. */
#define MAIN_CONF		"config/mikuterm.conf"

/* The fonts the program brings (share/fonts): the signs' and box drawing's, and the colour emoji's. */
#define MAIN_COMPANION		"keiland-fallback-mono.ttf"
#define MAIN_EMOJI		"keiland-emoji.ttf"

/* Mahora Bold, the companion of the fonts libkeiland opens (share/fonts). */
#define MAIN_BOLD		"keiland-bold.ttf"

/* The fonts the settings' tab draws its words with (share/fonts): the interface's and its fallback. */
#define MAIN_PAGE_FONT		"keiland.ttf"
#define MAIN_PAGE_FALLBACK	"keiland-fallback.ttf"

/* How long the program waits at its end for the connections' threads to say goodbye, in milliseconds. */
#define MAIN_FAREWELL_MS	3000U

/* The screen's scale the run is given, in 96ths (libkeiland follows the compositor's own). */
#define MAIN_SCALE		96U

/* What Help > About writes on the screen. */
#define MAIN_ABOUT		"\r\n\033[1mmikuterm\033[0m " MIKUTERM_VERSION_TEXT \
				" (Micro Unicode Terminal): zedBSD's terminal and SSH client for Linux.\r\n"

/*
 * The run's settings and paths, for the whole program: they are given to
 * the run, which keeps pointers to them.
 */
struct main_state {
	struct mikuterm_conf conf;
	struct mikuterm_connector connector;
	char font[MIKUTERM_PATH_MAX];
	char fallback[MIKUTERM_PATH_MAX];
	char companion[MIKUTERM_PATH_MAX];
	char bold[MIKUTERM_PATH_MAX];
	char emoji[MIKUTERM_PATH_MAX];
	char page_font[MIKUTERM_PATH_MAX];
	char page_fallback[MIKUTERM_PATH_MAX];
};

/* The program's state (large, and needed for the whole run). */
static struct main_state main_state;

static void main_read_conf(struct mikuterm_conf *conf);
static void main_new_window(void *data, struct kl_app *app, struct kl_window *from);
static void main_forget_passphrases(void *data);
static void main_ended(void *data, const struct terminal_run_result *result);

/*
 * Runs mikuterm.
 */
int
main(
	int argc,
	char **argv)
{
	struct mikuterm_connect_options connect;
	const struct terminal_page_program *page;
	struct terminal_run_options run;
	struct terminal_run_result result;
	int status;
	int error;

	/* A write to a connection that has gone must not end the program. */
	(void)signal(SIGPIPE, SIG_IGN);

	/*
	 * The X server process of an SSH connection (started by mikuterm
	 * itself): it shows no window of mikuterm's and reads no settings of
	 * its own.
	 */
	if (argc == 2 && strcmp(argv[1], X11_SPAWN_SWITCH) == 0) {
		status = mikuterm_x11_child();
		return status;
	}

	/* The folder everything is in. */
	error = mikuterm_folder_init();
	if (error != 0) {
		fprintf(stderr, "mikuterm: cannot find its folder or its own files (errno %d).\n", error);
		return 1;
	}

	/* The user's settings, and the View menu's kept over them. */
	main_read_conf(&main_state.conf);
	mikuterm_settings_defaults(&main_state.conf);

	/* The fonts: the user's choice or the program's own, the companion and the emoji the program's. */
	error = mikuterm_font(main_state.font, sizeof(main_state.font), main_state.conf.font);
	if (error == 0)
		error = mikuterm_font(main_state.fallback, sizeof(main_state.fallback), main_state.conf.fallback);
	if (error == 0)
		error = mikuterm_font(main_state.companion, sizeof(main_state.companion), MAIN_COMPANION);
	if (error == 0)
		error = mikuterm_font(main_state.bold, sizeof(main_state.bold), MAIN_BOLD);
	if (error == 0)
		error = mikuterm_font(main_state.emoji, sizeof(main_state.emoji), MAIN_EMOJI);
	if (error != 0) {
		fprintf(stderr, "mikuterm: the folder's path is too long for its fonts.\n");
		return 1;
	}

	/* Every font libkeiland opens takes its companions from beside the program, wherever it is installed. */
	kl_text_companions(main_state.bold, main_state.companion);

	/* The tabs' connections. */
	memset(&connect, 0, sizeof(connect));
	connect.term = main_state.conf.term;
	connect.connect = main_state.conf.connect;
	connect.connect_timeout = main_state.conf.connect_timeout;
	connect.wsl_port = main_state.conf.wsl_port;
	connect.x11_scale = main_state.conf.x11_scale;
	connect.x11_render_cpu = main_state.conf.x11_render_cpu;
	connect.x11_primary = main_state.conf.x11_primary;
	connect.sftp = main_state.conf.sftp;
	connect.notice = main_state.conf.problems;
	mikuterm_connector_init(&main_state.connector, &connect);

	/* The settings' tab: the folder's ssh/ files and mikuterm.conf (none when the folder's path is too long for them). */
	error = mikuterm_page_init(&main_state.conf, &main_state.connector, &page);
	if (error != 0)
		page = NULL;
	error = mikuterm_font(main_state.page_font, sizeof(main_state.page_font), MAIN_PAGE_FONT);
	if (error == 0)
		error = mikuterm_font(main_state.page_fallback, sizeof(main_state.page_fallback), MAIN_PAGE_FALLBACK);
	if (error != 0)
		page = NULL;

	/*
	 * The window: drawn on the processor, the grid the settings give,
	 * Session > Disable Input Method for a password the remote side asks
	 * for (the connection cannot see it asked), Alt+F11 for the ambiguous
	 * width and Alt+Enter for Fullscreen (F10 and F11 go to the remote
	 * side, for the programs that use them), and Session > Forget
	 * Remembered Passphrases.  The application's ID names the window to
	 * the session (its desktop entry's).
	 */
	memset(&run, 0, sizeof(run));
	run.application = MIKUTERM_APP_ID;
	run.display = NULL;
	run.title = "mikuterm";
	run.present = KL_PRESENT_SHM;
	run.font = main_state.font;
	run.companion = main_state.companion;
	run.fallback = main_state.fallback;
	run.emoji = main_state.emoji;
	run.pixels = main_state.conf.font_size;
	run.pixels_given = 0;
	run.scale = MAIN_SCALE;
	run.columns = main_state.conf.columns;
	run.rows = main_state.conf.rows;
	run.timeout = 0U;
	run.token = "mikuterm";
	run.about = MAIN_ABOUT;
	run.input_method_menu = 1;
	run.ambiguous_wide_key = 1;
	run.fullscreen_alt_enter = 1;
	run.forget_passphrases = main_forget_passphrases;
	run.page = page;
	run.page_font = main_state.page_font;
	run.page_fallback = main_state.page_fallback;
	run.connector = &main_state.connector.connector;
	run.new_window = main_new_window;
	run.ended = main_ended;
	run.data = &main_state;

	/* The run, until the last tab or the window closes; then the threads have a while to say goodbye. */
	status = terminal_run(&run, &result);
	(void)mikuterm_connections_wait(MAIN_FAREWELL_MS);

	/* A run that failed says what failed. */
	if (status != 0) {
		fprintf(stderr, "mikuterm stopped: %s failed (result %d, errno %d).\n", result.operation, result.result, result.error);
		return 1;
	}

	/* Succeeded: the run ended normally. */
	return 0;
}

/*
 * Finds a font named in the settings: a path from the root as it is, else
 * the user's fonts/ folder, else the program's share/fonts/.  Returns 0 or
 * ENAMETOOLONG.
 */
int
mikuterm_font(
	char *path,
	size_t size,
	const char *name)
{
	char inside[MIKUTERM_PATH_MAX];
	size_t length;
	int exists;
	int error;

	/* A path from the root. */
	length = strlen(name);
	if (name[0] == '/') {
		if (length + 1U > size)
			return ENAMETOOLONG;
		memcpy(path, name, length + 1U);
		return 0;
	}

	/* The user's fonts first. */
	(void)snprintf(inside, sizeof(inside), "fonts/%s", name);
	error = mikuterm_path(path, size, inside);
	if (error != 0)
		return error;
	exists = mikuterm_file_exists(path);
	if (exists)
		return 0;

	/* The program's own. */
	(void)snprintf(inside, sizeof(inside), "share/fonts/%s", name);
	error = mikuterm_path(path, size, inside);
	if (error != 0)
		return error;

	/* Succeeded: the path (the run says so when the font is not there). */
	return 0;
}

/* Reads config/mikuterm.conf into the settings (its defaults when it is not there). */
static void
main_read_conf(
	struct mikuterm_conf *conf)
{
	char path[MIKUTERM_PATH_MAX];
	char shown[sizeof(MAIN_CONF)];
	char *text;
	size_t size;
	int error;

	/* The defaults. */
	mikuterm_conf_defaults(conf);

	/* The file, when it is there; one that cannot be read is a problem to show. */
	error = mikuterm_path(path, sizeof(path), MAIN_CONF);
	if (error != 0)
		return;
	error = mikuterm_file_read(path, MIKUTERM_CONF_MAX, &text, &size);
	if (error == ENOENT)
		return;
	if (error != 0) {
		mikuterm_path_shown(MAIN_CONF, shown, sizeof(shown));
		(void)snprintf(conf->problems, sizeof(conf->problems), "%s could not be read (errno %d).\r\n", shown, error);
		return;
	}

	/* What it says. */
	(void)mikuterm_conf_read(conf, text, size, 0);
	free(text);
}

/* Starts another mikuterm for a new window: the same program, apart from this one. */
static void
main_new_window(
	void *data,
	struct kl_app *app,
	struct kl_window *from)
{
	struct kl_launch_options launch;
	const char *arguments[2];
	int error;

	/* The program itself. */
	(void)data;
	arguments[0] = mikuterm_program();
	arguments[1] = NULL;

	/* On its way; a start that failed leaves this window as it was. */
	memset(&launch, 0, sizeof(launch));
	launch.argv = arguments;
	launch.from = from;
	error = kl_launch(app, &launch);
	if (error != 0) {
		printf("ZTERM NEW-WINDOW failed run=mikuterm errno=%d\n", error);
		fflush(stdout);
	}
}

/*
 * Hears how the run ended, before the application closes: the
 * connections' threads post no more from now on.
 */
static void
main_ended(
	void *data,
	const struct terminal_run_result *result)
{
	/* The application is closing. */
	(void)data;
	mikuterm_connections_stop();

	/* The run's line, for whoever reads the program's output. */
	if (!result->failed) {
		printf("ZTERM DONE run=mikuterm reason=%s\n", result->reason);
	} else {
		printf("ZTERM FAILED run=mikuterm operation=%s result=%d errno=%d\n", result->operation, result->result, result->error);
	}

	/* The line is out before the program ends. */
	fflush(stdout);
}

/* Forgets every passphrase mikuterm keeps (Session > Forget Remembered Passphrases). */
static void
main_forget_passphrases(
	void *data)
{
	unsigned forgotten;

	/* Nothing of the program's state is needed. */
	(void)data;

	/* Every passphrase kept goes. */
	forgotten = mikuterm_passphrases_forget_all();
	printf("MIKUTERM PASSPHRASE forget count=%u\n", forgotten);
	fflush(stdout);
}
