/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * mikuterm (Micro Unicode Terminal): zedBSD's terminal as a portable
 * Windows program whose tabs are SSH connections.
 *
 * It runs from the folder its zip unpacks to and keeps everything there
 * (mikuterm.h): it reads config\mikuterm.conf, finds its fonts, and runs
 * the terminal (run.h) drawn on the processor and shown by libkeiland,
 * each tab connecting on a thread of its own (connection-ssh.c,
 * ssh-thread.c).  New Window starts the program again.  A run that fails
 * says what failed in a message box, since a Windows program of the
 * desktop has no console.
 */

#include "mikuterm.h"
#include "session.h"
#include "sftp-view.h"
#include "vi-connection-win32.h"
#include "x11-host.h"

#include "userland/desktop/terminal/run.h"

#include "mikuterm-version.h"

#include <windows.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The user's settings and the View menu's, inside the folder. */
#define MAIN_CONF		"config/mikuterm.conf"

/* The fonts the program's folder brings (share\fonts): the signs' and box drawing's, and the colour emoji's. */
#define MAIN_COMPANION		"keiland-fallback-mono.ttf"
#define MAIN_EMOJI		"keiland-emoji.ttf"

/* The fonts the settings' tab draws its words with (share\fonts): the interface's and its fallback. */
#define MAIN_PAGE_FONT		"keiland.ttf"
#define MAIN_PAGE_FALLBACK	"keiland-fallback.ttf"

/* How long the program waits at its end for the connections' threads to say goodbye, in milliseconds. */
#define MAIN_FAREWELL_MS	3000U

/* The command line of the X server process of an SSH connection (x11-child.c). */
#define MAIN_X11_SWITCH		L"--x11-server"

/* What Help > About writes on the screen. */
#define MAIN_ABOUT		"\r\n\033[1mmikuterm\033[0m " MIKUTERM_VERSION_TEXT \
				" (Micro Unicode Terminal): zedBSD's terminal and SSH client for Windows.\r\n"

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
	char emoji[MIKUTERM_PATH_MAX];
	char page_font[MIKUTERM_PATH_MAX];
	char page_fallback[MIKUTERM_PATH_MAX];
};

/* The program's state (large, and needed for the whole run). */
static struct main_state main_state;

static void main_read_conf(struct mikuterm_conf *conf);
static void main_new_window(void *data, struct kl_app *app, struct kl_window *from);
static void main_forget_passphrases(void *data);
static int main_can_open_sftp(void *data, const struct terminal_connection *connection);
static int main_open_sftp(void *data, struct terminal_connection *connection);
static int main_open_vi(void *data, struct terminal_window *window, unsigned columns, unsigned rows, const char *path, char *title, size_t size, struct terminal_connection **connection);
static int main_confirm_close(void *data, struct terminal_window *window, struct terminal_connection *connection);
static void main_ended(void *data, const struct terminal_run_result *result);
static void main_say(const char *text);

/*
 * Runs mikuterm.
 */
int WINAPI
wWinMain(
	HINSTANCE instance,
	HINSTANCE previous,
	LPWSTR command_line,
	int show)
{
	struct mikuterm_connect_options connect;
	const struct terminal_page_program *page;
	struct terminal_run_options run;
	struct terminal_run_result result;
	char text[256];
	int status;
	int error;

	(void)instance;
	(void)previous;
	(void)show;

	/*
	 * The X server process of an SSH connection (started by mikuterm
	 * itself): it shows no message and reads no settings of its own.
	 */
	if (command_line != NULL && wcscmp(command_line, MAIN_X11_SWITCH) == 0) {
		status = mikuterm_x11_child();
		return status;
	}

	/* The folder everything is in. */
	error = mikuterm_folder_init();
	if (error != 0) {
		main_say("mikuterm cannot find the folder it is in.");
		return 1;
	}

	/* The user's settings, and the View menu's kept over them. */
	main_read_conf(&main_state.conf);
	mikuterm_settings_defaults(&main_state.conf);

	/* The fonts: the user's choice or the folder's own, the companion and the emoji the folder's. */
	error = mikuterm_font(main_state.font, sizeof(main_state.font), main_state.conf.font);
	if (error == 0)
		error = mikuterm_font(main_state.fallback, sizeof(main_state.fallback), main_state.conf.fallback);
	if (error == 0)
		error = mikuterm_font(main_state.companion, sizeof(main_state.companion), MAIN_COMPANION);
	if (error == 0)
		error = mikuterm_font(main_state.emoji, sizeof(main_state.emoji), MAIN_EMOJI);
	if (error != 0) {
		main_say("mikuterm's folder path is too long for its fonts.");
		return 1;
	}

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

	/* The settings' tab: the folder's ssh\ files and mikuterm.conf (none when the folder's path is too long for them). */
	error = mikuterm_page_init(&main_state.conf, &main_state.connector, &page);
	if (error != 0)
		page = NULL;
	error = mikuterm_font(main_state.page_font, sizeof(main_state.page_font), MAIN_PAGE_FONT);
	if (error == 0)
		error = mikuterm_font(main_state.page_fallback, sizeof(main_state.page_fallback), MAIN_PAGE_FALLBACK);
	if (error != 0)
		page = NULL;

	/*
	 * The window: drawn on the processor, at the screen's scale, the grid
	 * the settings give, Session > Disable Input Method for a password the
	 * remote side asks for (the connection cannot see it asked), Alt+F11
	 * for the ambiguous width and Alt+Enter for Fullscreen (F10 and F11 go
	 * to the remote side, for the programs that use them), and Session >
	 * Forget Remembered Passphrases.
	 */
	memset(&run, 0, sizeof(run));
	run.application = "mikuterm";
	run.display = NULL;
	run.title = "mikuterm";
	run.present = KL_PRESENT_SHM;
	run.font = main_state.font;
	run.companion = main_state.companion;
	run.fallback = main_state.fallback;
	run.emoji = main_state.emoji;
	run.pixels = main_state.conf.font_size;
	run.pixels_given = 0;
	run.scale = GetDpiForSystem();
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
	run.refuse_terminal_drop = 1;
	run.open_sftp_menu = 1;
	run.can_open_sftp = main_can_open_sftp;
	run.open_sftp = main_open_sftp;
	run.open_vi_menu = 1;
	run.open_vi = main_open_vi;
	run.confirm_close = main_confirm_close;
	run.data = &main_state;

	/* The run, until the last tab or the window closes; then the threads have a while to say goodbye. */
	status = terminal_run(&run, &result);
	(void)mikuterm_connections_wait(MAIN_FAREWELL_MS);

	/* A run that failed says what failed. */
	if (status != 0) {
		(void)snprintf(text, sizeof(text), "mikuterm stopped: %s failed (result %d, errno %d).", result.operation, result.result, result.error);
		main_say(text);
		return 1;
	}

	/* Succeeded: the run ended normally. */
	return 0;
}

/*
 * Finds a font named in the settings: a Windows path from a drive or a
 * server (mikuterm_path_absolute: C:/..., //server/share/..., or with
 * backslashes) as it is, else the user's fonts\ folder, else the
 * program's share\fonts\.  Returns 0 or ENAMETOOLONG.
 */
int
mikuterm_font(
	char *path,
	size_t size,
	const char *name)
{
	char inside[MIKUTERM_PATH_MAX];
	size_t length;
	int absolute;
	int exists;
	int error;

	/* A path from a drive or a server. */
	length = strlen(name);
	absolute = mikuterm_path_absolute(name);
	if (absolute) {
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

/* Reads config\mikuterm.conf into the settings (its defaults when it is not there). */
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

/* Shows a message in a box, the one way a program without a console tells the user. */
static void
main_say(
	const char *text)
{
	/* The box, until the user closes it. */
	(void)MessageBoxA(NULL, text, "mikuterm", MB_OK | MB_ICONERROR);
}

/*
 * Forgets every passphrase mikuterm keeps for the Windows session
 * (Session > Forget Remembered Passphrases).
 */
static void
main_forget_passphrases(
	void *data)
{
	unsigned forgotten;

	/* Nothing of the program's state is needed. */
	(void)data;

	/* Every credential of mikuterm's goes. */
	forgotten = mikuterm_passphrases_forget_all();
	printf("MIKUTERM PASSPHRASE forget count=%u\n", forgotten);
	fflush(stdout);
}

/* Reports whether Shell > Open SFTP is available for connection. */
static int
main_can_open_sftp(
	void *data,
	const struct terminal_connection *connection)
{
	int allowed;

	/* Forwards the query to the SSH session. */
	(void)data;
	allowed = mikuterm_session_can_open_sftp(connection);
	return allowed;
}

/* Opens an SFTP browser tab on the active SSH connection. */
static int
main_open_sftp(
	void *data,
	struct terminal_connection *connection)
{
	struct main_state *st;
	struct mikuterm_session *session;
	struct sftp_link *link;
	struct terminal_connection *view_conn;
	char host[MIKUTERM_HOST_MAX];
	int allowed;
	int error;

	/* Verifies that the connection is an eligible SSH session. */
	st = data;
	allowed = mikuterm_session_can_open_sftp(connection);
	if (!allowed)
		return 0;
	session = (struct mikuterm_session *)connection;

	/* Creates the channel relay between the network and SFTP worker threads. */
	error = sftp_link_create(session->waker, &link);
	if (error != 0)
		return 0;

	/* Creates the SFTP browser view tab for the target host. */
	mikuterm_session_host(connection, host, sizeof(host));
	view_conn = NULL;
	error = sftp_view_create(
		session->app,
		session,
		link,
		host,
		st->conf.sftp_preserve_times,
		st->conf.sftp_show_hidden,
		st->page_font,
		st->page_fallback,
		&view_conn);
	if (error != 0) {
		sftp_link_release(link);
		return 0;
	}

	/* Attaches the SFTP tab and requests the subsystem channel. */
	error = mikuterm_session_open_sftp(connection, view_conn, link);
	sftp_link_release(link);
	if (error != 0) {
		view_conn->ops->close(view_conn);
		return 0;
	}

	/* Succeeded: the SFTP tab is opening. */
	return 0;
}

/* Opens a file in an embedded vi.exe child-process tab. */
static int
main_open_vi(
	void *data,
	struct terminal_window *window,
	unsigned columns,
	unsigned rows,
	const char *path,
	char *title,
	size_t size,
	struct terminal_connection **connection)
{
	struct main_state *st;
	int ambiguous_wide;

	st = data;
	ambiguous_wide = (window != NULL) ? window->menu_state.ambiguous_wide : st->conf.ambiguous_wide;
	return mikuterm_vi_open(
		(window != NULL) ? window->app : NULL,
		window,
		columns,
		rows,
		ambiguous_wide,
		path,
		title,
		size,
		connection);
}

/* Confirms closing a terminal tab if its attached SFTP tab has active transfers. */
static int
main_confirm_close(
	void *data,
	struct terminal_window *window,
	struct terminal_connection *connection)
{
	struct terminal_connection *view_conn;
	int confirmed;

	/* Checks whether an SFTP tab is attached to the closing terminal. */
	(void)data;
	if (connection == NULL)
		return 1;
	view_conn = mikuterm_session_sftp_view(connection);
	if (view_conn == NULL)
		return 1;

	/* Prompts the user if the attached SFTP tab has active transfers. */
	confirmed = sftp_view_confirm_close(view_conn, window);
	return confirmed;
}
