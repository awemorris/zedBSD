/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * mikuterm's body on Android: what the Windows program's WinMain does, on the
 * program's native thread.  It unpacks the program's read-only data, reads the
 * settings, finds the fonts, and runs the terminal (run.h) on libkeiland's
 * Android layer until the last tab or the window closes.  Each tab connects
 * over SSH on a thread of its own (connection-ssh.c, ssh-thread.c).
 *
 * A build made with the echo connection (echo-connection.h) opens tabs with it
 * instead, so that the terminal's screen, keys and touch can be tried without
 * a network.
 */

#include "apps/libkeiland/android/bridge-jni.h"
#include "apps/libkeiland/android/keiland-android.h"
#include "apps/terminal-android/jni/mikuterm-android.h"

#include "mikuterm.h"

#include "userland/desktop/terminal/run.h"

#if MIKUTERM_ANDROID_ECHO
#include "apps/terminal-android/jni/echo-connection.h"
#endif

#include "mikuterm-version.h"

#include <android/log.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The user's settings and the View menu's, inside the folder. */
#define MAIN_CONF		"config/mikuterm.conf"

/* The fonts the program's data brings (share/fonts): the signs' and box drawing's. */
#define MAIN_COMPANION		"keiland-fallback-mono.ttf"

/* The fonts the settings' tab draws its words with (share/fonts): the interface's and its fallback. */
#define MAIN_PAGE_FONT		"keiland.ttf"
#define MAIN_PAGE_FALLBACK	"keiland-fallback.ttf"

/* How long the program waits at its end for the connections' threads to say goodbye, in milliseconds. */
#define MAIN_FAREWELL_MS	3000U

/* What Help > About writes on the screen. */
#define MAIN_ABOUT		"\r\n\033[1mmikuterm\033[0m " MIKUTERM_VERSION_TEXT \
				" (Micro Unicode Terminal): zedBSD's terminal and SSH client for Android.\r\n"

/*
 * The run's settings and paths, for the whole program: they are given to the
 * run, which keeps pointers to them.
 */
struct main_state {
	struct mikuterm_conf conf;
	struct mikuterm_connector connector;
	char font[MIKUTERM_PATH_MAX];
	char fallback[MIKUTERM_PATH_MAX];
	char companion[MIKUTERM_PATH_MAX];
	char page_font[MIKUTERM_PATH_MAX];
	char page_fallback[MIKUTERM_PATH_MAX];
};

/* The program's state (large, and needed for the whole run). */
static struct main_state main_state;

static void main_read_conf(struct mikuterm_conf *conf);
#if !MIKUTERM_ANDROID_ECHO
static void main_connector_init(struct mikuterm_connector *connector, const struct mikuterm_conf *conf);
#endif
static void main_forget_passphrases(void *data);
static void main_ended(void *data, const struct terminal_run_result *result);

/*
 * Runs mikuterm until its last tab or its window closes.
 */
void
mikuterm_android_main(void)
{
	const struct terminal_page_program *page;
	struct terminal_run_options run;
	struct terminal_run_result result;
	int status;
	int error;

	/* The read-only data in place (the fonts); a package that holds none leaves the fonts missing. */
	error = android_places_unpack(keiland_android_activity());
	if (error != 0)
		__android_log_print(ANDROID_LOG_WARN, "mikuterm", "the program's data was not unpacked (%d)", error);

	/* The folder everything of the user's is in. */
	error = mikuterm_folder_init();
	if (error != 0) {
		__android_log_print(ANDROID_LOG_ERROR, "mikuterm", "the folder cannot be found (%d)", error);
		return;
	}

	/* The passphrases kept on the disk are asked of Java, and the answers of Java and of the connections' threads come to the hook. */
	keiland_android_set_post_hook(mikuterm_android_post);
	mikuterm_credential_start();

	/* The user's settings, and the View menu's kept over them. */
	main_read_conf(&main_state.conf);
	mikuterm_settings_defaults(&main_state.conf);

	/* The fonts: the user's choice or the program's own, and the companion the program's. */
	error = mikuterm_font(main_state.font, sizeof(main_state.font), main_state.conf.font);
	if (error == 0)
		error = mikuterm_font(main_state.fallback, sizeof(main_state.fallback), main_state.conf.fallback);
	if (error == 0)
		error = mikuterm_font(main_state.companion, sizeof(main_state.companion), MAIN_COMPANION);
	if (error != 0) {
		__android_log_print(ANDROID_LOG_ERROR, "mikuterm", "the fonts' paths are too long (%d)", error);
		return;
	}

	/* The tabs' connections. */
#if MIKUTERM_ANDROID_ECHO
	memset(&main_state.connector, 0, sizeof(main_state.connector));
	main_state.connector.connector.open = echo_connector_open;
	main_state.connector.connector.operation = "echo_connector_open";
	main_state.connector.connector.data = NULL;
#else
	main_connector_init(&main_state.connector, &main_state.conf);
#endif

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
	 * The window: drawn on the processor at the screen's scale, the grid the
	 * settings give (the window is the screen's, so the grid follows its size),
	 * with no new window and no emoji font yet.
	 */
	memset(&run, 0, sizeof(run));
	run.application = "mikuterm";
	run.display = NULL;
	run.title = "mikuterm";
	run.present = KL_PRESENT_SHM;
	run.font = main_state.font;
	run.companion = main_state.companion;
	run.fallback = main_state.fallback;
	run.emoji = NULL;
	run.pixels = main_state.conf.font_size;
	run.pixels_given = 0;
	run.scale = keiland_android_scale();
	run.columns = main_state.conf.columns;
	run.rows = main_state.conf.rows;
	run.timeout = 0U;
	run.token = "mikuterm";
	run.about = MAIN_ABOUT;
	run.input_method_menu = 0;
	run.ambiguous_wide_key = 0;
	run.fullscreen_alt_enter = 0;
	run.forget_passphrases = main_forget_passphrases;
	run.page = page;
	run.page_font = main_state.page_font;
	run.page_fallback = main_state.page_fallback;
	run.connector = &main_state.connector.connector;
	run.new_window = NULL;
	run.ended = main_ended;
	run.data = &main_state;

	/* The run, until the last tab or the window closes; then the threads have a while to say goodbye. */
	status = terminal_run(&run, &result);
	(void)mikuterm_connections_wait(MAIN_FAREWELL_MS);
	if (status != 0)
		__android_log_print(ANDROID_LOG_ERROR, "mikuterm", "the run failed: %s (result %d, errno %d)", result.operation, result.result, result.error);

	/* The Activity goes with the run. */
	(void)keiland_java_quit();
}

/*
 * M's hook for the posts of Java and of the connections' threads: a secret's
 * answer, a passphrase to keep or forget on the disk, the network and the ask
 * to close every connection.  Returns 1 when the post was one of these.
 */
int
mikuterm_android_post(
	uint32_t code,
	void *data)
{
	/* By code. */
	switch (code) {
	case MIKUTERM_POST_SECRET:
		mikuterm_credential_answer(data);
		return 1;
	case MIKUTERM_POST_KEEP_SECRET:
		mikuterm_credential_keep(data);
		return 1;
	case MIKUTERM_POST_FORGET_SECRET:
		mikuterm_credential_forget(data);
		return 1;
	case MIKUTERM_POST_NETWORK:
		/* Nothing acts on the network's changes yet. */
		free(data);
		return 1;
	case MIKUTERM_POST_STOP_ALL:
		mikuterm_connections_stop();
		return 1;
	default:
		break;
	}

	/* Not one of these. */
	return 0;
}

/*
 * Finds a font named in the settings: a path from the root as it is, else the
 * user's fonts/ folder, else the program's own (share/fonts of the program's
 * data).  Returns 0 or ENAMETOOLONG.
 */
int
mikuterm_font(
	char *path,
	size_t size,
	const char *name)
{
	char inside[MIKUTERM_PATH_MAX];
	char directory[MIKUTERM_PATH_MAX];
	size_t length;
	int written;
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
	written = snprintf(inside, sizeof(inside), "fonts/%s", name);
	if (written < 0 || (size_t)written >= sizeof(inside))
		return ENAMETOOLONG;
	error = mikuterm_path(path, size, inside);
	if (error != 0)
		return error;
	if (mikuterm_file_exists(path))
		return 0;

	/* The program's own. */
	error = kl_data_directory(directory, sizeof(directory));
	if (error != 0)
		return error;
	written = snprintf(path, size, "%s/fonts/%s", directory, name);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded: the path (the run says so when the font is not there). */
	return 0;
}

#if !MIKUTERM_ANDROID_ECHO
/* Makes the tabs' connector from the settings: SSH connections, the first to the Connect host when the settings name one. */
static void
main_connector_init(
	struct mikuterm_connector *connector,
	const struct mikuterm_conf *conf)
{
	struct mikuterm_connect_options connect;

	/* What the settings say of the connections. */
	memset(&connect, 0, sizeof(connect));
	connect.term = conf->term;
	connect.connect = conf->connect;
	connect.connect_timeout = conf->connect_timeout;
	connect.wsl_port = conf->wsl_port;
	connect.x11_scale = conf->x11_scale;
	connect.x11_render_cpu = conf->x11_render_cpu;
	connect.x11_primary = conf->x11_primary;
	connect.notice = conf->problems;
	mikuterm_connector_init(connector, &connect);
}
#endif

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

/*
 * Hears how the run ended, before the application closes: the connections'
 * threads post no more from now on.
 */
static void
main_ended(
	void *data,
	const struct terminal_run_result *result)
{
	/* The application is closing. */
	(void)data;
	(void)result;
	mikuterm_connections_stop();
}

/* Forgets every passphrase mikuterm keeps (Session > Forget Remembered Passphrases). */
static void
main_forget_passphrases(
	void *data)
{
	/* Nothing of the program's state is needed. */
	(void)data;

	/* Every passphrase kept goes. */
	(void)mikuterm_passphrases_forget_all();
}
