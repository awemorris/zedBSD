/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * mikuterm's body on Android: what the Windows program's WinMain does, on the
 * program's native thread.  It unpacks the program's read-only data, takes the
 * settings (their defaults for now), finds the fonts in that data and runs the
 * terminal (run.h) on libkeiland's Android layer until the last tab or the
 * window closes.
 *
 * A tab's connection is the echo connection (echo-connection.h) while the
 * program has no other, so that the terminal's screen, keys and touch can be
 * tried without a network.
 */

#include "apps/libkeiland/android/keiland-android.h"
#include "apps/terminal-android/jni/mikuterm-android.h"

#include "mikuterm.h"

#include "userland/desktop/terminal/run.h"

#if MIKUTERM_ANDROID_ECHO
#include "apps/terminal-android/jni/echo-connection.h"
#endif

#include <android/log.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The fonts the program's data brings (share/fonts): the signs' and box drawing's. */
#define MAIN_COMPANION		"keiland-fallback-mono.ttf"

/*
 * The run's settings and paths, for the whole program: they are given to the
 * run, which keeps pointers to them.
 */
struct main_state {
	struct mikuterm_conf conf;
	struct terminal_connector connector;
	char font[MIKUTERM_PATH_MAX];
	char fallback[MIKUTERM_PATH_MAX];
	char companion[MIKUTERM_PATH_MAX];
};

/* The program's state (large, and needed for the whole run). */
static struct main_state main_state;

static int main_font(char *path, size_t size, const char *name);

/*
 * Runs mikuterm until its last tab or its window closes.
 */
void
mikuterm_android_main(void)
{
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

	/* The settings: their defaults, and the View menu's kept over them. */
	mikuterm_conf_defaults(&main_state.conf);
	mikuterm_settings_defaults(&main_state.conf);

	/* The fonts: the settings' choice and the companion, in the program's data. */
	error = main_font(main_state.font, sizeof(main_state.font), main_state.conf.font);
	if (error == 0)
		error = main_font(main_state.fallback, sizeof(main_state.fallback), main_state.conf.fallback);
	if (error == 0)
		error = main_font(main_state.companion, sizeof(main_state.companion), MAIN_COMPANION);
	if (error != 0) {
		__android_log_print(ANDROID_LOG_ERROR, "mikuterm", "the fonts' paths are too long (%d)", error);
		return;
	}

	/* The tabs' connection. */
#if MIKUTERM_ANDROID_ECHO
	main_state.connector.open = echo_connector_open;
	main_state.connector.operation = "echo_connector_open";
	main_state.connector.data = NULL;
#else
	__android_log_print(ANDROID_LOG_ERROR, "mikuterm", "no connection to open");
	return;
#endif

	/*
	 * The window: drawn on the processor at the screen's scale, the grid the
	 * settings give (the window is the screen's, so the grid follows its size),
	 * with no settings' tab, no new window and no emoji font yet.
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
	run.scale = 0U;
	run.columns = main_state.conf.columns;
	run.rows = main_state.conf.rows;
	run.timeout = 0U;
	run.token = "mikuterm";
	run.about = NULL;
	run.input_method_menu = 0;
	run.ambiguous_wide_key = 0;
	run.fullscreen_alt_enter = 0;
	run.forget_passphrases = NULL;
	run.page = NULL;
	run.connector = &main_state.connector;
	run.new_window = NULL;
	run.ended = NULL;
	run.data = &main_state;

	/* The run, until the last tab or the window closes. */
	status = terminal_run(&run, &result);
	if (status != 0)
		__android_log_print(ANDROID_LOG_ERROR, "mikuterm", "the run failed: %s (result %d, errno %d)", result.operation, result.result, result.error);
}

/* Writes the path of a font file of the program's data (share/fonts/<name>); a name that is a path stays as it is. */
static int
main_font(
	char *path,
	size_t size,
	const char *name)
{
	char directory[MIKUTERM_PATH_MAX];
	int written;
	int error;

	/* A path is the user's own. */
	if (name[0] == '/') {
		written = snprintf(path, size, "%s", name);
		if (written < 0 || (size_t)written >= size)
			return ENAMETOOLONG;
		return 0;
	}

	/* The program's data. */
	error = kl_data_directory(directory, sizeof(directory));
	if (error != 0)
		return error;
	written = snprintf(path, size, "%s/fonts/%s", directory, name);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded. */
	return 0;
}
