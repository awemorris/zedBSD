/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * terminal: a VT100 terminal in a Wayland window, drawn with Vulkan.
 *
 * It runs a shell (or a command) on a pseudo-terminal for each tab
 * (connection-process.c), shows what it writes and sends it the keys
 * typed (run.c).  The window follows the size the compositor gives it, and
 * the shell is told the grid's new size.  The terminal ends when the last
 * shell exits or the window is closed.  New Window starts another
 * terminal, the way this one was run.
 *
 * Its settings' tab (settings-page.c) edits the user's ~/.ssh/config,
 * makes keys in ~/.ssh (the private key 0600) and lists ~/.ssh/known_hosts;
 * the files are written here with POSIX calls (main_page_*).  A change of
 * ~/.ssh/config is checked with the SSH library's reader before it is
 * written, and the file opens in the text editor (textedit).  The tab also
 * keeps the terminal's own settings of the next start (the fonts, the
 * grid) and of the next tab (the shells' TERM) in the desktop's settings
 * (terminal.*, main_item_*), which a command line's option wins over.
 *
 * Every outcome is one line on standard output: ZTERM DONE on a normal end
 * (with the reason), ZTERM FAILED naming what failed otherwise.
 */

#include "run.h"
#include "connection.h"
#include "settings-page.h"

#include "userland/base/ssh/ssh-config.h"

#include "userland/desktop/paths.h"

#include "include/kern/os-name.h"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The font the terminal uses unless told otherwise, and its size in pixels. */
#define MAIN_FONT		KEILAND_DATADIR "/fonts/keiland-mono.ttf"
#define MAIN_FONT_PIXELS	16U

/* The font that draws what the monospaced font has no glyph for (CJK), unless told otherwise. */
#define MAIN_FALLBACK_FONT	KEILAND_DATADIR "/fonts/keiland-fallback.ttf"

/* The colour emoji font, which draws in colour what neither of them has (the desktop's text draws from it too). */
#define MAIN_EMOJI_FONT		KEILAND_DATADIR "/fonts/keiland-emoji.ttf"

/* The fonts the settings' tab draws its words with: the desktop's interface font and its fallback. */
#define MAIN_PAGE_FONT		KEILAND_DATADIR "/fonts/keiland.ttf"
#define MAIN_PAGE_FALLBACK	KEILAND_DATADIR "/fonts/keiland-fallback.ttf"

/* The user's SSH files under the home directory, and the directory's mode when the settings make it. */
#define MAIN_SSH_DIRECTORY	"/.ssh"
#define MAIN_SSH_MODE		0700

/* A file's modes: a secret one (a private key, the ssh_config made new), a public one. */
#define MAIN_SECRET_MODE	0600
#define MAIN_PUBLIC_MODE	0644

/* The longest path of the user's SSH files, and the longest comment of a new key. */
#define MAIN_PATH_MAX		1024U
#define MAIN_COMMENT_MAX	320U

/* The grid the window opens with (the desktop's settings' defaults of terminal.columns and terminal.rows). */
#define MAIN_COLUMNS		90U
#define MAIN_ROWS		28U

/* The shells' terminal type unless the user chose another (the process connector's). */
#define MAIN_TERM		"xterm"

/* The application whose settings the terminal's are, and its keys of the next start and of the next tab. */
#define MAIN_SETTINGS_APP	"terminal"
#define MAIN_KEY_FONT		"terminal.font"
#define MAIN_KEY_FALLBACK	"terminal.fallback-font"
#define MAIN_KEY_COLUMNS	"terminal.columns"
#define MAIN_KEY_ROWS		"terminal.rows"
#define MAIN_KEY_TERM		"terminal.term"

/* Where a font named without a directory is looked for. */
#define MAIN_FONT_DIRECTORY	KEILAND_DATADIR "/fonts/"

/* The program started for a new window when the terminal was not run by a path. */
#define MAIN_PROGRAM		KEILAND_BINDIR "/terminal"

/* The text editor the settings' tab opens ~/.ssh/config in. */
#define MAIN_EDITOR		KEILAND_BINDIR "/textedit"

/* The most Host names ~/.ssh/config is checked for, besides a name no Host matches, and the longest one with its NUL. */
#define MAIN_CHECK_HOSTS	32U
#define MAIN_CHECK_NAME		128U

/* What Help > About Terminal writes on the screen. */
#define MAIN_ABOUT		"\r\n\033[1mTerminal\033[0m: a VT100 terminal for " ZEDBSD_OS_NAME ", drawn with Vulkan.\r\n" \
				"Its menus are drawn by the System Menu (xdg_toplevel_menu_v1).\r\n"

/* The terminal's own settings in the settings' tab. */
enum main_item {
	MAIN_ITEM_NONE,
	MAIN_ITEM_FONT,
	MAIN_ITEM_FALLBACK,
	MAIN_ITEM_COLUMNS,
	MAIN_ITEM_ROWS,
	MAIN_ITEM_TERM
};

/*
 * What the command line asked for.
 */
struct main_options {
	const char *program;
	const char *display;
	const char *font;
	const char *fallback;
	const char *command;
	const char *token;
	unsigned pixels;
	int pixels_given;
	int font_given;
	int font_kept;
	int fallback_given;
	unsigned columns;
	unsigned rows;
	int columns_given;
	int rows_given;
	unsigned timeout;
};

/*
 * The settings kept for the next start that the command line did not
 * override: the font files' paths (empty: the terminal's own).  Read once
 * before the run; the options point into it for the run's life.
 */
struct main_kept {
	char font[KL_SETTINGS_VALUE_MAX];
	char fallback[KL_SETTINGS_VALUE_MAX];
};

/*
 * What the settings' tab is given: the user's SSH files' paths, the
 * comment of a new key (user@host), the functions that read and write
 * them, and the connector whose shells' TERM the tab changes.  Set once
 * before the run, for its whole life.
 */
struct main_page {
	char ssh[MAIN_PATH_MAX];
	char config[MAIN_PATH_MAX];
	char keys[MAIN_PATH_MAX];
	char known_hosts[MAIN_PATH_MAX];
	char comment[MAIN_COMMENT_MAX];
	struct terminal_page_program program;
	struct terminal_process_connector *connector;
};

/*
 * The Host names of an ssh_config a check reads it for: so many of them
 * (the first MAIN_CHECK_HOSTS kept).  One for each check, on the heap for
 * that check alone (it is too large for a small stack).
 */
struct main_check_names {
	char names[MAIN_CHECK_HOSTS][MAIN_CHECK_NAME];
	unsigned count;
};

/* The settings' tab's files (the main thread's). */
static struct main_page main_page;

/* The fonts kept for the next start, which the options point into (the main thread's, set before the run). */
static struct main_kept main_kept;

/*
 * The terminal's own settings in the tab, by their keys in the desktop's
 * settings: the fonts and the grid (the next start), then the shells'
 * TERM (the next tab).
 */
static const struct terminal_page_item main_items[] = {
	{ MAIN_KEY_FONT, "Font", TERMINAL_PANE_APPEARANCE, TERMINAL_ITEM_TEXT, TERMINAL_EFFECT_START, NULL },
	{ MAIN_KEY_FALLBACK, "Fallback font", TERMINAL_PANE_APPEARANCE, TERMINAL_ITEM_TEXT, TERMINAL_EFFECT_START, NULL },
	{ MAIN_KEY_COLUMNS, "Columns", TERMINAL_PANE_APPEARANCE, TERMINAL_ITEM_NUMBER, TERMINAL_EFFECT_START, NULL },
	{ MAIN_KEY_ROWS, "Rows", TERMINAL_PANE_APPEARANCE, TERMINAL_ITEM_NUMBER, TERMINAL_EFFECT_START, NULL },
	{ MAIN_KEY_TERM, "TERM", TERMINAL_PANE_BEHAVIOR, TERMINAL_ITEM_TEXT, TERMINAL_EFFECT_CONNECTION, NULL }
};

static int main_parse(int argc, char **argv, struct main_options *options);
static void main_kept_read(struct main_options *options, struct terminal_process_connector *connector);
static int main_page_init(struct terminal_process_connector *connector);
static int main_item_get(void *data, const char *keyword, char *value, size_t size);
static int main_item_set(void *data, const char *keyword, const char *value, char *why, size_t size);
static int main_item_font(const char *value, char *path, size_t size, char *why, size_t why_size);
static unsigned main_item_kind(const char *keyword);
static int main_item_number(const char *value, unsigned maximum, unsigned *number, char *text, size_t size, char *why, size_t why_size);
static int main_item_term(const char *value, char *why, size_t why_size);
static int main_item_keep(const char *keyword, const char *value, const char *fallback, char *why, size_t why_size);
static int main_page_read(void *data, const char *path, size_t limit, char **text, size_t *size);
static int main_page_replace(void *data, const char *path, const void *bytes, size_t size);
static int main_page_create(void *data, const char *path, const void *bytes, size_t size, int secret);
static int main_page_exists(void *data, const char *path);
static int main_page_check(void *data, const char *text, size_t size, char *report, size_t capacity);
static int main_page_named(void *data, const char *name, size_t size);
static int main_page_editor(void *data, struct terminal_window *window, const char *path);
static int main_page_write(const char *path, int flags, mode_t mode, const void *bytes, size_t size);
static void main_page_directory(void);
static const char *main_value(const char *argument, const char *name);
static int main_number(const char *text, unsigned maximum, unsigned *value);
static void main_new_window(void *data, struct kl_app *app, struct kl_window *from);
static void main_ended(void *data, const struct terminal_run_result *result);

/*
 * Runs the terminal.
 */
int
main(
	int argc,
	char **argv)
{
	struct main_options options;
	struct terminal_process_connector connector;
	struct terminal_run_options run;
	struct terminal_run_result result;
	int status;

	/* The command line. */
	status = main_parse(argc, argv, &options);
	if (status != 0) {
		fprintf(stderr, "usage: terminal [--display=NAME] [--font=PATH] [--fallback-font=PATH] [--font-size=PIXELS] [--columns=N] [--rows=N] [--command=COMMAND] [--token=NAME] [--timeout-s=N]\n");
		return 2;
	}

	/* A write to a shell that has gone must not kill the terminal. */
	(void)signal(SIGPIPE, SIG_IGN);

	/* Each tab a shell on a pseudo-terminal, or the shell running the command. */
	terminal_process_connector_init(&connector, options.command);

	/* The settings kept for the next start and the next tab, where the command line chose nothing. */
	main_kept_read(&options, &connector);

	/* The terminal's window, fonts and grid, drawn with its own Vulkan. */
	memset(&run, 0, sizeof(run));
	run.application = "terminal";
	run.display = options.display;
	run.title = "Terminal";
	run.present = KL_PRESENT_NONE;
	run.font = options.font;
	run.font_default = NULL;
	run.companion = KEILAND_FONT_FALLBACK_MONO;
	run.fallback = options.fallback;
	run.emoji = MAIN_EMOJI_FONT;
	run.pixels = options.pixels;
	run.pixels_given = options.pixels_given;
	run.scale = 96U;
	run.columns = options.columns;
	run.rows = options.rows;
	run.timeout = options.timeout;
	run.token = options.token;
	run.about = MAIN_ABOUT;
	run.connector = &connector.connector;

	/* Alt+F11 switches the width of the ambiguous characters, as mikuterm's does. */
	run.ambiguous_wide_key = 1;

	/* A kept font that cannot be opened gives way to the terminal's own (one given on the command line does not). */
	if (options.font_kept)
		run.font_default = MAIN_FONT;

	/* The settings' tab, with the user's SSH files (none without a home directory). */
	status = main_page_init(&connector);
	if (status == 0) {
		run.page = &main_page.program;
		run.page_font = MAIN_PAGE_FONT;
		run.page_fallback = MAIN_PAGE_FALLBACK;
	}

	/* The program's own answers to the run. */
	run.new_window = main_new_window;
	run.ended = main_ended;
	run.data = &options;

	/* The terminal runs until it ends; main_ended says how. */
	status = terminal_run(&run, &result);
	if (status != 0)
		return 1;

	/* Succeeded: the terminal ended normally. */
	return 0;
}

/* Reads the command line into the options; returns nonzero for a malformed one. */
static int
main_parse(
	int argc,
	char **argv,
	struct main_options *options)
{
	const char *value;
	const char *slash;
	int index;
	int status;

	/* The defaults. */
	memset(options, 0, sizeof(*options));
	options->program = MAIN_PROGRAM;
	options->display = NULL;
	options->font = MAIN_FONT;
	options->fallback = MAIN_FALLBACK_FONT;
	options->command = NULL;
	options->token = "term";
	options->pixels = MAIN_FONT_PIXELS;
	options->columns = MAIN_COLUMNS;
	options->rows = MAIN_ROWS;
	options->timeout = 0U;

	/* A program run by a path is run by the same path for a new window (a bare name is looked up as MAIN_PROGRAM). */
	slash = NULL;
	if (argc > 0 && argv[0] != NULL)
		slash = strchr(argv[0], '/');

	/* The path it was run by. */
	if (slash != NULL)
		options->program = argv[0];

	/* Each option in turn; the first name that matches takes it. */
	for (index = 1; index < argc; index++) {
		/* The compositor's socket. */
		value = main_value(argv[index], "--display=");
		if (value != NULL) {
			options->display = value;
			continue;
		}

		/* The font file. */
		value = main_value(argv[index], "--font=");
		if (value != NULL) {
			options->font = value;
			options->font_given = 1;
			continue;
		}

		/* The fallback font file. */
		value = main_value(argv[index], "--fallback-font=");
		if (value != NULL) {
			options->fallback = value;
			options->fallback_given = 1;
			continue;
		}

		/* The font's size in pixels. */
		value = main_value(argv[index], "--font-size=");
		if (value != NULL) {
			status = main_number(value, 96U, &options->pixels);
			if (status != 0)
				return -1;
			options->pixels_given = 1;
			continue;
		}

		/* The grid the window opens with. */
		value = main_value(argv[index], "--columns=");
		if (value != NULL) {
			status = main_number(value, TERMINAL_MAX_COLUMNS, &options->columns);
			if (status != 0)
				return -1;
			options->columns_given = 1;
			continue;
		}

		/* The rows the window opens with. */
		value = main_value(argv[index], "--rows=");
		if (value != NULL) {
			status = main_number(value, TERMINAL_MAX_ROWS, &options->rows);
			if (status != 0)
				return -1;
			options->rows_given = 1;
			continue;
		}

		/* A command run by the shell in place of an interactive shell. */
		value = main_value(argv[index], "--command=");
		if (value != NULL) {
			options->command = value;
			continue;
		}

		/* The name of the run in the log lines. */
		value = main_value(argv[index], "--token=");
		if (value != NULL) {
			options->token = value;
			continue;
		}

		/* A deadline in seconds, 0 for none. */
		value = main_value(argv[index], "--timeout-s=");
		if (value != NULL) {
			status = main_number(value, 86400U, &options->timeout);
			if (status != 0)
				return -1;
			continue;
		}

		/* An unknown option refuses the command line. */
		return -1;
	}

	/* A font too small, or an empty grid, is refused. */
	if (options->pixels < 6U || options->columns == 0U || options->rows == 0U)
		return -1;

	/* Succeeded: the options. */
	return 0;
}

/* Returns what follows an option's name in an argument, or NULL when the argument is another option. */
static const char *
main_value(
	const char *argument,
	const char *name)
{
	size_t length;
	int differs;

	/* The argument must start with the name, equals sign included. */
	length = strlen(name);
	differs = strncmp(argument, name, length);
	if (differs != 0)
		return NULL;

	/* Reports the value after the name. */
	return argument + length;
}

/* Reads a decimal number no larger than a maximum; returns nonzero when it is not one. */
static int
main_number(
	const char *text,
	unsigned maximum,
	unsigned *value)
{
	unsigned long parsed;
	char *end;

	/* A number with nothing after it. */
	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0')
		return -1;

	/* Too large a value is refused. */
	if (parsed > maximum)
		return -1;

	/* Succeeded: the value. */
	*value = (unsigned)parsed;
	return 0;
}

/*
 * Starts another terminal for a new window, the way this one was run,
 * apart from this one (it keeps nothing of this terminal's).
 */
static void
main_new_window(
	void *data,
	struct kl_app *app,
	struct kl_window *from)
{
	const struct main_options *options;
	struct kl_launch_options launch;
	const char *arguments[4];
	char token[64];
	char display[128];
	int error;

	/* The same program, its log lines named after this run's. */
	options = data;
	(void)snprintf(token, sizeof(token), "--token=%s-new", options->token);
	arguments[0] = options->program;
	arguments[1] = token;
	arguments[2] = NULL;

	/* The same display, when this one was given one. */
	if (options->display != NULL) {
		(void)snprintf(display, sizeof(display), "--display=%s", options->display);
		arguments[2] = display;
		arguments[3] = NULL;
	}

	/* The terminal, on its way; a start that failed leaves this window as it was. */
	memset(&launch, 0, sizeof(launch));
	launch.argv = arguments;
	launch.from = from;
	error = kl_launch(app, &launch);
	if (error != 0) {
		printf("ZTERM NEW-WINDOW failed run=%s errno=%d\n", options->token, error);
		fflush(stdout);
	}
}

/*
 * Says how the run ended, in one line, before its tabs and window go:
 * ZTERM DONE with the reason, or ZTERM FAILED with what failed.
 */
static void
main_ended(
	void *data,
	const struct terminal_run_result *result)
{
	const struct main_options *options;

	/* The run's line. */
	options = data;
	if (!result->failed) {
		printf("ZTERM DONE run=%s reason=%s\n", options->token, result->reason);
	} else {
		printf("ZTERM FAILED run=%s operation=%s result=%d errno=%d\n", options->token, result->operation, result->result, result->error);
	}

	/* The line reaches whoever reads the log before the terminal goes. */
	fflush(stdout);
}

/*
 * Fills what the settings' tab is given: the paths of ~/.ssh/config,
 * ~/.ssh/ and ~/.ssh/known_hosts, the comment user@host of a new key, the
 * terminal's own settings and the connector whose TERM they change.
 * Returns 0, or ENOENT without a home directory (the terminal then has no
 * settings' tab).
 */
static int
main_page_init(
	struct terminal_process_connector *connector)
{
	const struct passwd *user;
	const char *home;
	const char *name;
	char host[256];
	int written;
	int error;

	/* The home directory: the environment's, else the user's entry. */
	home = getenv("HOME");
	user = getpwuid(getuid());
	if ((home == NULL ||
	     home[0] == '\0') &&
	    user != NULL)
		home = user->pw_dir;
	if (home == NULL || home[0] == '\0')
		return ENOENT;

	/* The SSH files' paths. */
	written = snprintf(main_page.ssh, sizeof(main_page.ssh), "%s%s", home, MAIN_SSH_DIRECTORY);
	if (written < 0 || (size_t)written + 16U >= sizeof(main_page.ssh))
		return ENOENT;
	(void)snprintf(main_page.config, sizeof(main_page.config), "%s/config", main_page.ssh);
	(void)snprintf(main_page.keys, sizeof(main_page.keys), "%s/", main_page.ssh);
	(void)snprintf(main_page.known_hosts, sizeof(main_page.known_hosts), "%s/known_hosts", main_page.ssh);

	/* A new key's comment, user@host as ssh-keygen writes it. */
	name = "user";
	if (user != NULL)
		name = user->pw_name;
	error = gethostname(host, sizeof(host));
	if (error != 0)
		(void)snprintf(host, sizeof(host), "%s", "localhost");
	host[sizeof(host) - 1U] = '\0';
	(void)snprintf(main_page.comment, sizeof(main_page.comment), "%s@%s", name, host);

	/* The tab's program. */
	memset(&main_page.program, 0, sizeof(main_page.program));
	main_page.program.config_path = main_page.config;
	main_page.program.config_shown = "~/.ssh/config";
	main_page.program.key_directory = main_page.keys;
	main_page.program.key_directory_shown = "~/.ssh/";
	main_page.program.known_hosts_path = main_page.known_hosts;
	main_page.program.known_hosts_shown = "~/.ssh/known_hosts";
	main_page.program.read = main_page_read;
	main_page.program.replace = main_page_replace;
	main_page.program.create = main_page_create;
	main_page.program.exists = main_page_exists;
	main_page.program.check = main_page_check;
	main_page.program.open_editor = main_page_editor;
	main_page.program.items = main_items;
	main_page.program.item_count = sizeof(main_items) / sizeof(main_items[0]);
	main_page.program.item_get = main_item_get;
	main_page.program.item_set = main_item_set;
	main_page.program.comment = main_page.comment;
	main_page.program.x11 = 0;
	main_page.program.font = NULL;
	main_page.program.connection = "tab";
	main_page.program.data = &main_page;
	main_page.connector = connector;

	/* Succeeded: the tab has the user's files. */
	return 0;
}

/*
 * Takes the settings kept for the next start (the fonts, the grid) where
 * the command line chose nothing, and the shells' TERM kept for the
 * tabs; without the desktop's settings the defaults stay.
 */
static void
main_kept_read(
	struct main_options *options,
	struct terminal_process_connector *connector)
{
	struct kl_settings *settings;
	const char *fallback_shown;
	char term[TERMINAL_TERM_MAX];
	int number;
	int error;

	/* The terminal's settings; its own keys need no display. */
	memset(&main_kept, 0, sizeof(main_kept));
	settings = kl_settings_open(NULL, MAIN_SETTINGS_APP);
	if (settings == NULL)
		return;

	/* The font file, unless --font chose one. */
	error = kl_settings_get(settings, MAIN_KEY_FONT, main_kept.font, sizeof(main_kept.font), NULL);
	if (error == 0 &&
	    main_kept.font[0] != '\0' &&
	    !options->font_given) {
		options->font = main_kept.font;
		options->font_kept = 1;
	}

	/* The fallback font file, unless --fallback-font chose one. */
	error = kl_settings_get(settings, MAIN_KEY_FALLBACK, main_kept.fallback, sizeof(main_kept.fallback), NULL);
	if (error == 0 &&
	    main_kept.fallback[0] != '\0' &&
	    !options->fallback_given)
		options->fallback = main_kept.fallback;

	/* The columns, unless --columns chose them (the table's default, 90, when none are kept). */
	number = kl_settings_get_int(settings, MAIN_KEY_COLUMNS, (int)MAIN_COLUMNS);
	if (number >= 1 &&
	    number <= (int)TERMINAL_MAX_COLUMNS &&
	    !options->columns_given)
		options->columns = (unsigned)number;

	/* The rows, unless --rows chose them. */
	number = kl_settings_get_int(settings, MAIN_KEY_ROWS, (int)MAIN_ROWS);
	if (number >= 1 &&
	    number <= (int)TERMINAL_MAX_ROWS &&
	    !options->rows_given)
		options->rows = (unsigned)number;

	/* The shells' TERM, when one well formed is kept. */
	error = kl_settings_get(settings, MAIN_KEY_TERM, term, sizeof(term), NULL);
	if (error == 0 && term[0] != '\0') {
		error = main_item_term(term, NULL, 0U);
		if (error == 0)
			(void)snprintf(connector->term, sizeof(connector->term), "%s", term);
	}

	/* Closes the settings, which are not needed any more. */
	kl_settings_close(settings);

	/* The fallback font as the log names it ("none" when there is none). */
	fallback_shown = "none";
	if (options->fallback != NULL)
		fallback_shown = options->fallback;

	/* What the run takes, in the log. */
	printf("ZTERM SETTINGS run=%s font=%s fallback=%s columns=%u rows=%u term=%s\n",
	       options->token,
	       options->font,
	       fallback_shown,
	       options->columns,
	       options->rows,
	       connector->term);
	fflush(stdout);
}

/*
 * Gives a setting of the tab's own as it is kept now (the terminal's
 * default when none is: its fonts' paths, the grid's 90 and 28, xterm).
 * Returns 0, or an errno value of the desktop's settings.
 */
static int
main_item_get(
	void *data,
	const char *keyword,
	char *value,
	size_t size)
{
	struct kl_settings *settings;
	const char *fallback;
	unsigned kind;
	int error;

	(void)data;

	/* The default shown for a text setting kept empty: the terminal's own fonts and TERM. */
	kind = main_item_kind(keyword);
	switch (kind) {
	case MAIN_ITEM_FONT:
		fallback = MAIN_FONT;
		break;
	case MAIN_ITEM_FALLBACK:
		fallback = MAIN_FALLBACK_FONT;
		break;
	case MAIN_ITEM_TERM:
		fallback = MAIN_TERM;
		break;
	default:
		fallback = "";
		break;
	}

	/* The terminal's settings. */
	settings = kl_settings_open(NULL, MAIN_SETTINGS_APP);
	if (settings == NULL)
		return errno;

	/* The value kept (a number's default is the table's). */
	error = kl_settings_get(settings, keyword, value, size, NULL);
	if (error != 0) {
		kl_settings_close(settings);
		return error;
	}

	/* Closes the settings, which are not needed any more. */
	kl_settings_close(settings);

	/* An empty text is the terminal's own. */
	if (value[0] == '\0')
		(void)snprintf(value, size, "%s", fallback);

	/* Succeeded: the value. */
	return 0;
}

/*
 * Keeps a setting of the tab's own after checking it: a font that the
 * terminal can open (a name alone is a file of the desktop's fonts), a
 * grid within the terminal's, a TERM of a terminal type's characters
 * (which the next tab's shell takes at once).  An empty value, or the
 * default, takes the kept value away.  Returns 0, or EINVAL with why set,
 * or an errno value of the desktop's settings with why set.
 */
static int
main_item_set(
	void *data,
	const char *keyword,
	const char *value,
	char *why,
	size_t size)
{
	struct main_page *page;
	char kept[KL_SETTINGS_VALUE_MAX];
	const char *fallback;
	unsigned number;
	unsigned kind;
	int error;

	/* Which setting it is; a keyword not the terminal's is refused. */
	page = data;
	kind = main_item_kind(keyword);
	if (kind == MAIN_ITEM_NONE) {
		(void)snprintf(why, size, "not a setting of the terminal");
		return EINVAL;
	}

	/*
	 * The value as it is kept, checked, and the default that takes no
	 * line: a font's full path, the grid's number, the TERM as it is.
	 */
	error = 0;
	kept[0] = '\0';
	fallback = NULL;
	switch (kind) {
	case MAIN_ITEM_FONT:
		fallback = MAIN_FONT;
		if (value[0] != '\0')
			error = main_item_font(value, kept, sizeof(kept), why, size);
		break;
	case MAIN_ITEM_FALLBACK:
		fallback = MAIN_FALLBACK_FONT;
		if (value[0] != '\0')
			error = main_item_font(value, kept, sizeof(kept), why, size);
		break;
	case MAIN_ITEM_COLUMNS:
		fallback = "90";
		if (value[0] != '\0')
			error = main_item_number(value, TERMINAL_MAX_COLUMNS, &number, kept, sizeof(kept), why, size);
		break;
	case MAIN_ITEM_ROWS:
		fallback = "28";
		if (value[0] != '\0')
			error = main_item_number(value, TERMINAL_MAX_ROWS, &number, kept, sizeof(kept), why, size);
		break;
	case MAIN_ITEM_TERM:
	default:
		fallback = MAIN_TERM;
		if (value[0] != '\0')
			error = main_item_term(value, why, size);
		if (value[0] != '\0' && error == 0)
			(void)snprintf(kept, sizeof(kept), "%s", value);
		break;
	}

	/* A value refused is not kept. */
	if (error != 0)
		return error;

	/* Kept, or taken away (empty, or the default). */
	error = main_item_keep(keyword, kept, fallback, why, size);
	if (error != 0)
		return error;

	/* The next tab's shell takes the TERM at once (xterm when it was taken away). */
	if (kind == MAIN_ITEM_TERM && kept[0] == '\0') {
		(void)snprintf(page->connector->term, sizeof(page->connector->term), "%s", MAIN_TERM);
	} else if (kind == MAIN_ITEM_TERM) {
		(void)snprintf(page->connector->term, sizeof(page->connector->term), "%s", kept);
	}

	/* Succeeded: the next start, or the next tab, takes it. */
	return 0;
}

/* Tells which of the terminal's settings a keyword is (MAIN_ITEM_NONE for another). */
static unsigned
main_item_kind(
	const char *keyword)
{
	int differs;

	/* The font. */
	differs = strcmp(keyword, MAIN_KEY_FONT);
	if (differs == 0)
		return MAIN_ITEM_FONT;

	/* The fallback font. */
	differs = strcmp(keyword, MAIN_KEY_FALLBACK);
	if (differs == 0)
		return MAIN_ITEM_FALLBACK;

	/* The columns. */
	differs = strcmp(keyword, MAIN_KEY_COLUMNS);
	if (differs == 0)
		return MAIN_ITEM_COLUMNS;

	/* The rows. */
	differs = strcmp(keyword, MAIN_KEY_ROWS);
	if (differs == 0)
		return MAIN_ITEM_ROWS;

	/* The shells' TERM. */
	differs = strcmp(keyword, MAIN_KEY_TERM);
	if (differs == 0)
		return MAIN_ITEM_TERM;

	/* None of the terminal's. */
	return MAIN_ITEM_NONE;
}

/*
 * Writes a font's full path into path (a name alone is a file of the
 * desktop's fonts) after opening it as the terminal would.  Returns 0, or
 * EINVAL with why set.
 */
static int
main_item_font(
	const char *value,
	char *path,
	size_t size,
	char *why,
	size_t why_size)
{
	struct terminal_font *font;
	const char *slash;
	int written;
	int error;

	/* A full path as it is, a name in the desktop's fonts, anything else refused. */
	slash = strchr(value, '/');
	if (value[0] == '/') {
		written = snprintf(path, size, "%s", value);
	} else if (slash == NULL) {
		written = snprintf(path, size, "%s%s", MAIN_FONT_DIRECTORY, value);
	} else {
		(void)snprintf(why, why_size, "a font is a full path or a file name in %s", MAIN_FONT_DIRECTORY);
		return EINVAL;
	}

	/* A path too long to keep is refused. */
	if (written < 0 || (size_t)written >= size) {
		(void)snprintf(why, why_size, "the path is too long");
		return EINVAL;
	}

	/* The font, opened at the default size as the next start would. */
	font = malloc(sizeof(*font));
	if (font == NULL) {
		(void)snprintf(why, why_size, "no memory to read the font");
		return EINVAL;
	}

	/* A file the terminal cannot draw with is refused. */
	error = terminal_font_open(font, path, NULL, MAIN_FONT_PIXELS);
	if (error != 0) {
		free(font);
		(void)snprintf(why, why_size, "%s is not a font the terminal can open (%s)", path, strerror(error));
		return EINVAL;
	}

	/* It opened; it is not needed any more. */
	terminal_font_close(font);
	free(font);

	/* Succeeded: the font's full path. */
	return 0;
}

/* Reads a grid's size, 1 to maximum, and writes it as kept into text; returns 0, or EINVAL with why set. */
static int
main_item_number(
	const char *value,
	unsigned maximum,
	unsigned *number,
	char *text,
	size_t size,
	char *why,
	size_t why_size)
{
	int status;

	/* A whole number within the grid's limit, not 0. */
	status = main_number(value, maximum, number);
	if (status != 0 || *number == 0U) {
		(void)snprintf(why, why_size, "a number from 1 to %u", maximum);
		return EINVAL;
	}

	/* The number's text as it is kept. */
	(void)snprintf(text, size, "%u", *number);

	/* Succeeded: the number, and its text. */
	return 0;
}

/*
 * Checks a terminal type's name: letters, digits, '.', '_', '+' and '-',
 * not empty and short enough for the environment's TERM.  Returns 0, or
 * EINVAL with why set (when why is given).
 */
static int
main_item_term(
	const char *value,
	char *why,
	size_t why_size)
{
	size_t length;
	size_t index;
	char character;

	/* A name of a sensible length. */
	length = strlen(value);
	if (length == 0U || length >= TERMINAL_TERM_MAX) {
		if (why != NULL)
			(void)snprintf(why, why_size, "a terminal type of 1 to %u characters", TERMINAL_TERM_MAX - 1U);
		return EINVAL;
	}

	/* Each character one a terminal type's name has. */
	for (index = 0U; index < length; index++) {
		/* A letter or a digit. */
		character = value[index];
		if ((character >= 'a' && character <= 'z') ||
		    (character >= 'A' && character <= 'Z') ||
		    (character >= '0' && character <= '9'))
			continue;

		/* Or one of the four signs. */
		if (character == '.' ||
		    character == '_' ||
		    character == '+' ||
		    character == '-')
			continue;

		/* Any other character refuses the name. */
		if (why != NULL)
			(void)snprintf(why, why_size, "a terminal type has letters, digits, '.', '_', '+' and '-'");
		return EINVAL;
	}

	/* Succeeded: a terminal type's name. */
	return 0;
}

/*
 * Keeps a setting in the desktop's settings: value, or the default (no
 * line) when value is empty or the same as fallback.  Returns 0, or an
 * errno value with why set.
 */
static int
main_item_keep(
	const char *keyword,
	const char *value,
	const char *fallback,
	char *why,
	size_t why_size)
{
	struct kl_settings *settings;
	int differs;
	int error;

	/* The terminal's settings. */
	settings = kl_settings_open(NULL, MAIN_SETTINGS_APP);
	if (settings == NULL) {
		error = errno;
		(void)snprintf(why, why_size, "the settings could not be opened (%s)", strerror(error));
		return error;
	}

	/* Whether the value is the default: none given, or the same as it. */
	differs = 0;
	if (value[0] != '\0')
		differs = strcmp(value, fallback);

	/* The default takes the line away; another value is written. */
	if (differs == 0) {
		error = kl_settings_reset(settings, keyword, NULL);
	} else {
		error = kl_settings_set(settings, keyword, value, NULL);
	}

	/* A failure leaves the old setting. */
	if (error != 0) {
		kl_settings_close(settings);
		(void)snprintf(why, why_size, "it could not be kept (%s)", strerror(error));
		return error;
	}

	/* Closes the settings, which are not needed any more. */
	kl_settings_close(settings);

	/* Succeeded: the setting is kept. */
	return 0;
}

/* Reads a file whole into a buffer the caller frees (with a NUL after it); 0, ENOENT, EFBIG or another errno value. */
static int
main_page_read(
	void *data,
	const char *path,
	size_t limit,
	char **text,
	size_t *size)
{
	struct stat status;
	char *bytes;
	size_t got;
	ssize_t count;
	int descriptor;
	int error;

	(void)data;

	/* The file. */
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return errno;

	/* Its size, within the limit. */
	error = fstat(descriptor, &status);
	if (error != 0) {
		error = errno;
		close(descriptor);
		return error;
	}

	/* A file larger than the caller takes is refused. */
	if (status.st_size < 0 || (unsigned long long)status.st_size > (unsigned long long)limit) {
		close(descriptor);
		return EFBIG;
	}

	/* Its bytes, and a NUL after them. */
	bytes = malloc((size_t)status.st_size + 1U);
	if (bytes == NULL) {
		close(descriptor);
		return ENOMEM;
	}

	/* Its bytes, until the end or the size. */
	got = 0U;
	while (got < (size_t)status.st_size) {
		count = read(descriptor, bytes + got, (size_t)status.st_size - got);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			break;
		got += (size_t)count;
	}

	/* The file closes once it is read, and the text ends as a C string. */
	close(descriptor);
	bytes[got] = '\0';

	/* Succeeded: what the file holds. */
	*text = bytes;
	*size = got;
	return 0;
}

/*
 * Writes a file whole in place of the old one: the bytes go to path.new
 * (the old file's mode, or a secret file's for a new one), which is then
 * renamed over it, so a failure leaves the old file as it was.
 */
static int
main_page_replace(
	void *data,
	const char *path,
	const void *bytes,
	size_t size)
{
	struct stat status;
	char fresh[MAIN_PATH_MAX + 8U];
	mode_t mode;
	int written;
	int error;

	(void)data;

	/* The new file's name and mode. */
	written = snprintf(fresh, sizeof(fresh), "%s.new", path);
	if (written < 0 || (size_t)written >= sizeof(fresh))
		return ENAMETOOLONG;
	mode = MAIN_SECRET_MODE;
	error = stat(path, &status);
	if (error == 0)
		mode = status.st_mode & 07777U;

	/* The bytes in it, in ~/.ssh made when it is not there. */
	main_page_directory();
	error = main_page_write(fresh, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode, bytes, size);
	if (error != 0)
		return error;

	/* It takes the old file's name. */
	error = rename(fresh, path);
	if (error != 0) {
		error = errno;
		(void)unlink(fresh);
		return error;
	}

	/* Succeeded: the file has the bytes. */
	return 0;
}

/* Makes a new file (never over one there: EEXIST), 0600 for a secret and 0644 otherwise whatever the umask is. */
static int
main_page_create(
	void *data,
	const char *path,
	const void *bytes,
	size_t size,
	int secret)
{
	mode_t mode;
	int error;

	(void)data;

	/* The mode. */
	mode = MAIN_PUBLIC_MODE;
	if (secret)
		mode = MAIN_SECRET_MODE;

	/* The file, new, in ~/.ssh made when it is not there. */
	main_page_directory();
	error = main_page_write(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode, bytes, size);
	if (error != 0)
		return error;

	/* Succeeded: the file is made. */
	return 0;
}

/* Tells whether anything is at a path (a link that leads nowhere counts). */
static int
main_page_exists(
	void *data,
	const char *path)
{
	struct stat status;
	int error;

	(void)data;

	/* The path itself. */
	error = lstat(path, &status);
	if (error == 0)
		return 1;

	/* Nothing there. */
	return 0;
}

/*
 * Checks an ssh_config as ssh reads it: for a name no Host line matches
 * and for each Host's name, the first problem found ("line N: what") is
 * the report (empty when there is none).
 */
static int
main_page_check(
	void *data,
	const char *text,
	size_t size,
	char *report,
	size_t capacity)
{
	struct ssh_config config;
	struct main_check_names *names;
	char error[SSH_CONFIG_ERROR_MAX];
	unsigned index;
	int status;

	(void)data;

	/* No problem yet. */
	report[0] = '\0';

	/* The names to check it for. */
	names = calloc(1U, sizeof(*names));
	if (names == NULL)
		return ENOMEM;
	error[0] = '\0';
	status = ssh_config_hosts(text, size, main_page_named, names, error);
	if (status != 0) {
		(void)snprintf(report, capacity, "%s", error);
		free(names);
		return 0;
	}

	/* A name no Host matches, then each Host's. */
	for (index = 0U; index <= names->count; index++) {
		ssh_config_init(&config);
		error[0] = '\0';
		if (index == 0U)
			status = ssh_config_parse(&config, text, size, "terminal.invalid", error);
		else
			status = ssh_config_parse(&config, text, size, names->names[index - 1U], error);
		if (status != 0) {
			(void)snprintf(report, capacity, "%s", error);
			break;
		}
	}

	/* Succeeded: the report. */
	free(names);
	return 0;
}

/* Keeps one Host name of an ssh_config, while there is room; answers 0 to go on. */
static int
main_page_named(
	void *data,
	const char *name,
	size_t size)
{
	struct main_check_names *names;

	/* A full list, or a name too long, is passed over. */
	names = data;
	if (names->count == MAIN_CHECK_HOSTS || size >= MAIN_CHECK_NAME)
		return 0;

	/* The name. */
	memcpy(names->names[names->count], name, size);
	names->names[names->count][size] = '\0';
	names->count++;
	return 0;
}

/* Opens a file in the text editor, a program apart from the terminal; 0 or the error of its start. */
static int
main_page_editor(
	void *data,
	struct terminal_window *window,
	const char *path)
{
	struct kl_launch_options launch;
	const char *arguments[3];
	int error;

	(void)data;

	/* textedit with the file. */
	arguments[0] = MAIN_EDITOR;
	arguments[1] = path;
	arguments[2] = NULL;

	/* On its way, beside this window. */
	memset(&launch, 0, sizeof(launch));
	launch.argv = arguments;
	launch.from = window->kui;
	error = kl_launch(window->app, &launch);
	if (error != 0)
		return error;

	/* Succeeded: the editor is starting. */
	printf("ZTERM SETTINGS editor-started %s\n", MAIN_EDITOR);
	fflush(stdout);
	return 0;
}

/*
 * Opens a file with the flags, sets its mode exactly, writes the bytes and
 * syncs them; a file this call made and could not fill is removed.
 */
static int
main_page_write(
	const char *path,
	int flags,
	mode_t mode,
	const void *bytes,
	size_t size)
{
	const unsigned char *at;
	ssize_t count;
	int descriptor;
	int error;

	/* The file. */
	descriptor = open(path, flags, mode);
	if (descriptor < 0)
		return errno;

	/* Its mode, whatever the umask took away. */
	error = fchmod(descriptor, mode);
	if (error != 0)
		error = errno;

	/* The bytes, all of them. */
	at = bytes;
	while (error == 0 && size > 0U) {
		count = write(descriptor, at, size);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			error = EIO;
			if (count < 0)
				error = errno;
			break;
		}

		/* Moves past what was written. */
		at += count;
		size -= (size_t)count;
	}

	/* On the disk before it is used. */
	if (error == 0) {
		error = fsync(descriptor);
		if (error != 0)
			error = errno;
	}

	/* The file closes either way. */
	close(descriptor);

	/* A file left half written goes. */
	if (error != 0) {
		(void)unlink(path);
		return error;
	}

	/* Succeeded: the file holds the bytes. */
	return 0;
}

/* Makes ~/.ssh (0700) when it is not there, as ssh does; one that cannot be made shows in the write that follows. */
static void
main_page_directory(void)
{
	int error;

	/* The directory, one level. */
	error = mkdir(main_page.ssh, MAIN_SSH_MODE);
	(void)error;
}
