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
 * the files are written here with POSIX calls (main_page_*).
 *
 * Every outcome is one line on standard output: ZTERM DONE on a normal end
 * (with the reason), ZTERM FAILED naming what failed otherwise.
 */

#include "run.h"
#include "connection.h"
#include "settings-page.h"

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

/* The grid the window opens with. */
#define MAIN_COLUMNS		90U
#define MAIN_ROWS		28U

/* The program started for a new window when the terminal was not run by a path. */
#define MAIN_PROGRAM		KEILAND_BINDIR "/terminal"

/* What Help > About Terminal writes on the screen. */
#define MAIN_ABOUT		"\r\n\033[1mTerminal\033[0m: a VT100 terminal for " ZEDBSD_OS_NAME ", drawn with Vulkan.\r\n" \
				"Its menus are drawn by the System Menu (xdg_toplevel_menu_v1).\r\n"

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
	unsigned columns;
	unsigned rows;
	unsigned timeout;
};

/*
 * What the settings' tab is given: the user's SSH files' paths, the
 * comment of a new key (user@host) and the functions that read and write
 * them.  Set once before the run, for its whole life.
 */
struct main_page {
	char ssh[MAIN_PATH_MAX];
	char config[MAIN_PATH_MAX];
	char keys[MAIN_PATH_MAX];
	char known_hosts[MAIN_PATH_MAX];
	char comment[MAIN_COMMENT_MAX];
	struct terminal_page_program program;
};

/* The settings' tab's files (the main thread's). */
static struct main_page main_page;

static int main_parse(int argc, char **argv, struct main_options *options);
static int main_page_init(const struct main_options *options);
static int main_page_read(void *data, const char *path, size_t limit, char **text, size_t *size);
static int main_page_replace(void *data, const char *path, const void *bytes, size_t size);
static int main_page_create(void *data, const char *path, const void *bytes, size_t size, int secret);
static int main_page_exists(void *data, const char *path);
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

	/* The terminal's window, fonts and grid, drawn with its own Vulkan. */
	memset(&run, 0, sizeof(run));
	run.application = "terminal";
	run.display = options.display;
	run.title = "Terminal";
	run.present = KL_PRESENT_NONE;
	run.font = options.font;
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

	/* The settings' tab, with the user's SSH files (none without a home directory). */
	status = main_page_init(&options);
	if (status == 0) {
		run.page = &main_page.program;
		run.page_font = MAIN_PAGE_FONT;
		run.page_fallback = MAIN_PAGE_FALLBACK;
	}
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
			continue;
		}

		/* The fallback font file. */
		value = main_value(argv[index], "--fallback-font=");
		if (value != NULL) {
			options->fallback = value;
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
			continue;
		}

		/* The rows the window opens with. */
		value = main_value(argv[index], "--rows=");
		if (value != NULL) {
			status = main_number(value, TERMINAL_MAX_ROWS, &options->rows);
			if (status != 0)
				return -1;
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
 * ~/.ssh/ and ~/.ssh/known_hosts, the comment user@host of a new key and
 * the font the terminal draws with.  Returns 0, or ENOENT without a home
 * directory (the terminal then has no settings' tab).
 */
static int
main_page_init(
	const struct main_options *options)
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
	main_page.program.check = NULL;
	main_page.program.items = NULL;
	main_page.program.item_count = 0U;
	main_page.program.comment = main_page.comment;
	main_page.program.font = options->font;
	main_page.program.data = &main_page;

	/* Succeeded: the tab has the user's files. */
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
	got = 0U;
	while (got < (size_t)status.st_size) {
		count = read(descriptor, bytes + got, (size_t)status.st_size - got);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			break;
		got += (size_t)count;
	}
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
		at += count;
		size -= (size_t)count;
	}

	/* On the disk before it is used. */
	if (error == 0) {
		error = fsync(descriptor);
		if (error != 0)
			error = errno;
	}
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
