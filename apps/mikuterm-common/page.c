/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What mikuterm gives the terminal's settings' tab (settings-page.h): the
 * folder's ssh\config, ssh\ for the keys (a private key made so that only
 * the user may read it) and ssh\known_hosts, the check of an ssh\config
 * with the SSH library's own reader, the system's text editor the
 * ssh\config opens in (the file's own "edit" verb, else "open", else
 * Notepad; on Linux, xdg-open), a host's X11 lines in its form (mikuterm forwards X11), and
 * mikuterm's own settings of config\mikuterm.conf: the fonts and the grid
 * (from the next start), the TERM, the wait for a connection, the port of
 * WSL's sshd and the X11 windows' scale, drawing and selection (from the
 * next connection: the connector takes them at once) and the host of the
 * first tab (from the next start).  A setting is written in place of its
 * line in mikuterm.conf, every other line kept.
 */

#include "mikuterm.h"

#include "userland/desktop/terminal/settings-page.h"
#include "userland/base/ssh/ssh-config.h"

/* Whether a file can be opened in another program (an editor, the desktop's opener): each build says so. */
#ifndef MIKUTERM_HAVE_OPENER
#error "MIKUTERM_HAVE_OPENER must be defined (1 or 0) by the build"
#endif

/* Whether the Keys pane offers to paste a private key (a system with no file to bring one by): each build says so. */
#ifndef MIKUTERM_HAVE_KEY_PASTE
#error "MIKUTERM_HAVE_KEY_PASTE must be defined (1 or 0) by the build"
#endif

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <pwd.h>
#include <unistd.h>
#if MIKUTERM_HAVE_OPENER
#include <fcntl.h>
#include <sys/wait.h>
#endif
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The files of the settings' tab, inside the folder. */
#define PAGE_CONF		"config/mikuterm.conf"
#define PAGE_SSH_CONFIG		"ssh/config"
#define PAGE_SSH		"ssh"
#define PAGE_KNOWN_HOSTS	"ssh/known_hosts"

/* The most host names an ssh\config is checked for, besides a name no Host matches. */
#define PAGE_CHECK_HOSTS	32U

/* The longest name of a host checked, with its NUL. */
#define PAGE_HOST_MAX		128U

#ifdef _WIN32
/* The editor of a file no verb opens: Notepad, which every Windows has. */
#define PAGE_NOTEPAD		L"notepad.exe"
#elif MIKUTERM_HAVE_OPENER
/* The program that opens a file in the desktop's own application for it (the user's text editor). */
#define PAGE_OPENER		"xdg-open"
#endif

/*
 * The names of an ssh\config's Host lines, which it is checked for: so
 * many of them (the first PAGE_CHECK_HOSTS kept).
 */
struct page_hosts {
	char names[PAGE_CHECK_HOSTS][PAGE_HOST_MAX];
	unsigned count;
};

/*
 * mikuterm's part of the settings' tab: the files' paths, a new key's
 * comment, the program given to the tab, the settings mikuterm runs with
 * (the TERM a new connection takes from them) and the connector (its wait
 * for a connection).  Set once before the run; the main thread's.
 */
struct page_state {
	char config[MIKUTERM_PATH_MAX];
	char keys[MIKUTERM_PATH_MAX];
	char known_hosts[MIKUTERM_PATH_MAX];
	char comment[MIKUTERM_HOST_MAX];
	struct terminal_page_program program;
	struct mikuterm_conf *conf;
	struct mikuterm_connector *connector;
};

/* The settings' tab's part (one for the run). */
static struct page_state page_state;

/* mikuterm's own settings in the tab: the look and the grid, then the connections and their X11. */
static const struct terminal_page_item page_items[] = {
	{ "Font", "Font", TERMINAL_PANE_APPEARANCE, TERMINAL_ITEM_TEXT, TERMINAL_EFFECT_START, NULL },
	{ "FontFallback", "Fallback font", TERMINAL_PANE_APPEARANCE, TERMINAL_ITEM_TEXT, TERMINAL_EFFECT_START, NULL },
	{ "Columns", "Columns", TERMINAL_PANE_APPEARANCE, TERMINAL_ITEM_NUMBER, TERMINAL_EFFECT_START, NULL },
	{ "Rows", "Rows", TERMINAL_PANE_APPEARANCE, TERMINAL_ITEM_NUMBER, TERMINAL_EFFECT_START, NULL },
	{ "Term", "TERM", TERMINAL_PANE_BEHAVIOR, TERMINAL_ITEM_TEXT, TERMINAL_EFFECT_CONNECTION, NULL },
	{ "ConnectTimeout", "Connect timeout (s)", TERMINAL_PANE_BEHAVIOR, TERMINAL_ITEM_NUMBER, TERMINAL_EFFECT_CONNECTION, NULL },
	{ "WslPort", "WSL sshd port", TERMINAL_PANE_BEHAVIOR, TERMINAL_ITEM_NUMBER, TERMINAL_EFFECT_CONNECTION, NULL },
	{ "X11Scale", "X11 scale (%)", TERMINAL_PANE_BEHAVIOR, TERMINAL_ITEM_TEXT, TERMINAL_EFFECT_CONNECTION, "auto, or 100 to 400 by 25 (X11Scale)" },
	{ "X11Render", "X11 drawing", TERMINAL_PANE_BEHAVIOR, TERMINAL_ITEM_TEXT, TERMINAL_EFFECT_CONNECTION, "auto or cpu (X11Render)" },
	{ "X11PrimaryToClipboard", "X11 selection to clipboard", TERMINAL_PANE_BEHAVIOR, TERMINAL_ITEM_TEXT, TERMINAL_EFFECT_CONNECTION, "yes or no (X11PrimaryToClipboard)" },
	{ "Sftp", "SFTP client", TERMINAL_PANE_BEHAVIOR, TERMINAL_ITEM_TEXT, TERMINAL_EFFECT_CONNECTION, "yes or no (Sftp)" },
	{ "SftpPreserveTimes", "SFTP preserve times", TERMINAL_PANE_BEHAVIOR, TERMINAL_ITEM_TEXT, TERMINAL_EFFECT_CONNECTION, "yes or no (SftpPreserveTimes)" },
	{ "Connect", "Host at start", TERMINAL_PANE_BEHAVIOR, TERMINAL_ITEM_TEXT, TERMINAL_EFFECT_START, NULL }
};

static int page_read(void *data, const char *path, size_t limit, char **text, size_t *size);
static int page_replace(void *data, const char *path, const void *bytes, size_t size);
static int page_create(void *data, const char *path, const void *bytes, size_t size, int secret);
static int page_exists(void *data, const char *path);
static int page_check(void *data, const char *text, size_t size, char *report, size_t capacity);
static int page_host_named(void *data, const char *name, size_t size);
#if MIKUTERM_HAVE_OPENER
static int page_editor(void *data, struct terminal_window *window, const char *path);
#endif
#ifdef _WIN32
static int page_shell(const wchar_t *verb, const wchar_t *file, const wchar_t *parameters);
#endif
static int page_item_get(void *data, const char *keyword, char *value, size_t size);
static int page_item_set(void *data, const char *keyword, const char *value, char *why, size_t size);
static void page_conf_now(struct mikuterm_conf *conf);
static void page_comment(char *comment, size_t size);
#ifdef _WIN32
static void page_utf8(const wchar_t *wide, char *text, size_t size);
#endif

/*
 * Makes what the settings' tab is given, for the settings mikuterm runs
 * with and its connector.  Returns 0, or ENAMETOOLONG when the folder's
 * path leaves no room for the files' (the tab is then not offered).
 */
int
mikuterm_page_init(
	struct mikuterm_conf *conf,
	struct mikuterm_connector *connector,
	const struct terminal_page_program **program)
{
	size_t length;
	int error;

	/* The files' paths: ssh/config, ssh/ with its separator, ssh/known_hosts (the system's separator). */
	*program = NULL;
	error = mikuterm_path(page_state.config, sizeof(page_state.config), PAGE_SSH_CONFIG);
	if (error == 0)
		error = mikuterm_path(page_state.keys, sizeof(page_state.keys) - 1U, PAGE_SSH);
	if (error == 0)
		error = mikuterm_path(page_state.known_hosts, sizeof(page_state.known_hosts), PAGE_KNOWN_HOSTS);
	if (error != 0)
		return error;
	length = strlen(page_state.keys);
#ifdef _WIN32
	page_state.keys[length] = '\\';
#else
	page_state.keys[length] = '/';
#endif
	page_state.keys[length + 1U] = '\0';

	/* A new key's comment, user@computer. */
	page_comment(page_state.comment, sizeof(page_state.comment));
	page_state.conf = conf;
	page_state.connector = connector;

	/* The tab's program. */
	memset(&page_state.program, 0, sizeof(page_state.program));
	page_state.program.config_path = page_state.config;
	page_state.program.config_shown = "ssh/config";
	page_state.program.key_directory = page_state.keys;
	page_state.program.key_directory_shown = "ssh/";
	page_state.program.known_hosts_path = page_state.known_hosts;
	page_state.program.known_hosts_shown = "ssh/known_hosts";
	page_state.program.read = page_read;
	page_state.program.replace = page_replace;
	page_state.program.create = page_create;
	page_state.program.exists = page_exists;
	page_state.program.check = page_check;

	/* The editor of the hosts file: none where no other program can be started (the tab then says so). */
#if MIKUTERM_HAVE_OPENER
	page_state.program.open_editor = page_editor;
#endif
	page_state.program.items = page_items;
	page_state.program.item_count = sizeof(page_items) / sizeof(page_items[0]);
	page_state.program.item_get = page_item_get;
	page_state.program.item_set = page_item_set;
	page_state.program.comment = page_state.comment;
	page_state.program.x11 = 1;
	page_state.program.key_paste = MIKUTERM_HAVE_KEY_PASTE;
	page_state.program.font = NULL;
	page_state.program.data = &page_state;

	/* Succeeded: the tab has mikuterm's files. */
	*program = &page_state.program;
	return 0;
}

/* Reads a file of the folder whole (a NUL after it); 0, ENOENT, EFBIG or another errno value. */
static int
page_read(
	void *data,
	const char *path,
	size_t limit,
	char **text,
	size_t *size)
{
	int error;

	(void)data;

	/* The folder's reader. */
	error = mikuterm_file_read(path, limit, text, size);
	if (error != 0)
		return error;

	/* Succeeded: the file's bytes. */
	return 0;
}

/* Writes a file of the folder whole, in place of the old one. */
static int
page_replace(
	void *data,
	const char *path,
	const void *bytes,
	size_t size)
{
	int error;

	(void)data;

	/* The folder's writer. */
	error = mikuterm_file_replace(path, bytes, size);
	if (error != 0)
		return error;

	/* Succeeded: the file holds the bytes. */
	return 0;
}

/* Makes a new file in the folder, a secret one readable by the user alone. */
static int
page_create(
	void *data,
	const char *path,
	const void *bytes,
	size_t size,
	int secret)
{
	int error;

	(void)data;

	/* The folder's maker of new files. */
	error = mikuterm_file_create(path, bytes, size, secret);
	if (error != 0)
		return error;

	/* Succeeded: the file is made. */
	return 0;
}

/* Tells whether anything is at a path. */
static int
page_exists(
	void *data,
	const char *path)
{
	int exists;

	(void)data;

	/* The folder's answer. */
	exists = mikuterm_file_exists(path);
	return exists;
}

/*
 * Checks an ssh\config as a connection reads it: for a name no Host line
 * matches and for each Host's name, the first problem found ("line N:
 * what") is the report (empty when there is none).
 */
static int
page_check(
	void *data,
	const char *text,
	size_t size,
	char *report,
	size_t capacity)
{
	struct ssh_config config;
	struct page_hosts *hosts;
	char error[SSH_CONFIG_ERROR_MAX];
	unsigned index;
	int status;

	(void)data;

	/* No problem yet. */
	report[0] = '\0';

	/* The names to check it for. */
	hosts = calloc(1U, sizeof(*hosts));
	if (hosts == NULL)
		return ENOMEM;
	error[0] = '\0';
	status = ssh_config_hosts(text, size, page_host_named, hosts, error);
	if (status != 0) {
		(void)snprintf(report, capacity, "%s", error);
		free(hosts);
		return 0;
	}

	/* A name no Host matches, then each Host's. */
	for (index = 0U; index <= hosts->count; index++) {
		ssh_config_init(&config);
		error[0] = '\0';
		if (index == 0U)
			status = ssh_config_parse(&config, text, size, "mikuterm.invalid", error);
		else
			status = ssh_config_parse(&config, text, size, hosts->names[index - 1U], error);
		if (status != 0) {
			(void)snprintf(report, capacity, "%s", error);
			break;
		}
	}

	/* Succeeded: the report. */
	free(hosts);
	return 0;
}

/* Keeps one Host name of the ssh\config, while there is room; answers 0 to go on. */
static int
page_host_named(
	void *data,
	const char *name,
	size_t size)
{
	struct page_hosts *hosts;

	/* A full list, or a name too long, is passed over. */
	hosts = data;
	if (hosts->count == PAGE_CHECK_HOSTS || size >= PAGE_HOST_MAX)
		return 0;

	/* The name. */
	memcpy(hosts->names[hosts->count], name, size);
	hosts->names[hosts->count][size] = '\0';
	hosts->count++;
	return 0;
}

#ifdef _WIN32
/*
 * Opens a file of the folder in the system's text editor: the editor the
 * file's type names for "edit", else the program that opens it, else
 * Notepad with the file.  No dialog asks the user to choose a program.
 * Returns 0, ENAMETOOLONG or EINVAL for a path that cannot be a wide one,
 * or ENOENT when none could start.
 */
static int
page_editor(
	void *data,
	struct terminal_window *window,
	const char *path)
{
	wchar_t wide[MIKUTERM_PATH_MAX];
	wchar_t quoted[MIKUTERM_PATH_MAX + 2U];
	int units;
	int started;

	(void)data;
	(void)window;

	/* The path as Windows takes it. */
	units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, (int)(sizeof(wide) / sizeof(wide[0])));
	if (units <= 0)
		return EINVAL;

	/* The file's own editor. */
	started = page_shell(L"edit", wide, NULL);
	if (started) {
		printf("MIKUTERM SETTINGS editor edit\n");
		fflush(stdout);
		return 0;
	}

	/* The program that opens it. */
	started = page_shell(L"open", wide, NULL);
	if (started) {
		printf("MIKUTERM SETTINGS editor open\n");
		fflush(stdout);
		return 0;
	}

	/* Notepad with the file, its path in quotes. */
	if ((size_t)units + 2U > sizeof(quoted) / sizeof(quoted[0]))
		return ENAMETOOLONG;
	quoted[0] = L'"';
	memcpy(quoted + 1, wide, ((size_t)units - 1U) * sizeof(wchar_t));
	quoted[units] = L'"';
	quoted[units + 1] = L'\0';
	started = page_shell(L"open", PAGE_NOTEPAD, quoted);
	if (!started)
		return ENOENT;

	/* Succeeded: Notepad is starting. */
	printf("MIKUTERM SETTINGS editor notepad\n");
	fflush(stdout);
	return 0;
}

/* Starts a verb of the shell on a file without asking the user anything; reports 1 when it started. */
static int
page_shell(
	const wchar_t *verb,
	const wchar_t *file,
	const wchar_t *parameters)
{
	SHELLEXECUTEINFOW shell;
	BOOL done;

	/* The verb on the file, no dialog when nothing takes it. */
	memset(&shell, 0, sizeof(shell));
	shell.cbSize = sizeof(shell);
	shell.fMask = SEE_MASK_FLAG_NO_UI;
	shell.lpVerb = verb;
	shell.lpFile = file;
	shell.lpParameters = parameters;
	shell.nShow = SW_SHOWNORMAL;
	done = ShellExecuteExW(&shell);
	if (!done)
		return 0;

	/* Succeeded: it started. */
	return 1;
}

#elif MIKUTERM_HAVE_OPENER
/*
 * Opens a file of the folder in the desktop's application for it
 * (xdg-open), started apart from mikuterm so that nothing waits for it.
 * Returns 0, or ENOENT when xdg-open could not start.
 */
static int
page_editor(
	void *data,
	struct terminal_window *window,
	const char *path)
{
	char *arguments[3];
	int pipe_ends[2];
	int status;
	int failure;
	ssize_t got;
	pid_t child;
	pid_t opener;

	(void)data;
	(void)window;

	/* The program and the file. */
	arguments[0] = (char *)PAGE_OPENER;
	arguments[1] = (char *)path;
	arguments[2] = NULL;

	/* A pipe the opener's start writes its failure on (closed by a start that succeeded). */
	status = pipe2(pipe_ends, O_CLOEXEC);
	if (status != 0)
		return ENOENT;

	/* A child that starts the opener and ends at once, so that the opener is no child of mikuterm's. */
	child = fork();
	if (child < 0) {
		(void)close(pipe_ends[0]);
		(void)close(pipe_ends[1]);
		return ENOENT;
	}
	if (child == 0) {
		/* The opener, in a process of its own; a start that failed says why on the pipe. */
		(void)close(pipe_ends[0]);
		opener = fork();
		if (opener == 0) {
			(void)execvp(PAGE_OPENER, arguments);
			failure = errno;
			got = write(pipe_ends[1], &failure, sizeof(failure));
			if (got < 0)
				_exit(126);
			_exit(127);
		}
		_exit(0);
	}

	/* The child ends at once; the pipe says whether the opener started. */
	(void)close(pipe_ends[1]);
	(void)waitpid(child, &status, 0);
	got = read(pipe_ends[0], &failure, sizeof(failure));
	(void)close(pipe_ends[0]);
	if (got != 0)
		return ENOENT;

	/* Succeeded: the opener is starting. */
	printf("MIKUTERM SETTINGS editor xdg-open\n");
	fflush(stdout);
	return 0;
}
#endif

/* Gives a setting's value as config\mikuterm.conf gives it now (its default when the file does not). */
static int
page_item_get(
	void *data,
	const char *keyword,
	char *value,
	size_t size)
{
	struct mikuterm_conf conf;
	int error;

	(void)data;

	/* The file as it is now. */
	page_conf_now(&conf);

	/* The value's text. */
	error = mikuterm_conf_text(&conf, keyword, value, size);
	if (error != 0)
		return error;

	/* Succeeded: the value. */
	return 0;
}

/*
 * Keeps a setting in config\mikuterm.conf, in place of its line (an empty
 * value removes the line: its default holds), after checking the value
 * (a font must be in fonts\ or share\fonts\, or a path to one); the TERM,
 * the wait for a connection, the WSL port and the X11 settings take effect
 * for the next connection at once.  Returns 0, or EINVAL with why set, or the error of the file.
 */
static int
page_item_set(
	void *data,
	const char *keyword,
	const char *value,
	char *why,
	size_t size)
{
	struct page_state *state;
	struct mikuterm_conf conf;
	char path[MIKUTERM_PATH_MAX];
	const char *reason;
	char *text;
	char *made;
	size_t text_size;
	size_t made_size;
	int font;
	int differs;
	int exists;
	int error;

	/* A value the file may hold. */
	state = data;
	if (value[0] != '\0') {
		error = mikuterm_conf_value(keyword, value, &reason);
		if (error != 0) {
			(void)snprintf(why, size, "%s", reason);
			return EINVAL;
		}
	}

	/* A font that is there. */
	font = 0;
	differs = _stricmp(keyword, "Font");
	if (differs == 0)
		font = 1;
	differs = _stricmp(keyword, "FontFallback");
	if (differs == 0)
		font = 1;
	if (font && value[0] != '\0') {
		error = mikuterm_font(path, sizeof(path), value);
		exists = 0;
		if (error == 0)
			exists = mikuterm_file_exists(path);
		if (!exists) {
			(void)snprintf(why, size, "there is no such font in fonts/ or share/fonts/");
			return EINVAL;
		}
	}

	/* The file as it is (none yet is empty). */
	error = mikuterm_path(path, sizeof(path), PAGE_CONF);
	if (error != 0)
		return error;
	text = NULL;
	text_size = 0U;
	error = mikuterm_file_read(path, MIKUTERM_CONF_MAX, &text, &text_size);
	if (error != 0 && error != ENOENT) {
		(void)snprintf(why, size, "config/mikuterm.conf could not be read (errno %d)", error);
		return error;
	}

	/* The text with the line changed, written whole. */
	if (text == NULL)
		error = mikuterm_conf_set("", 0U, keyword, value, &made, &made_size);
	else
		error = mikuterm_conf_set(text, text_size, keyword, value, &made, &made_size);
	free(text);
	if (error != 0)
		return error;
	error = mikuterm_file_replace(path, made, made_size);
	free(made);
	if (error != 0) {
		(void)snprintf(why, size, "config/mikuterm.conf could not be written (errno %d)", error);
		return error;
	}

	/* The next connection's TERM, wait, WSL port and X11 settings, from the file as it is now. */
	page_conf_now(&conf);
	(void)snprintf(state->conf->term, sizeof(state->conf->term), "%s", conf.term);
	state->conf->connect_timeout = conf.connect_timeout;
	state->connector->options.connect_timeout = conf.connect_timeout;
	state->conf->wsl_port = conf.wsl_port;
	state->connector->options.wsl_port = conf.wsl_port;
	state->conf->x11_scale = conf.x11_scale;
	state->connector->options.x11_scale = conf.x11_scale;
	state->conf->x11_render_cpu = conf.x11_render_cpu;
	state->connector->options.x11_render_cpu = conf.x11_render_cpu;
	state->conf->x11_primary = conf.x11_primary;
	state->connector->options.x11_primary = conf.x11_primary;
	state->conf->sftp = conf.sftp;
	state->connector->options.sftp = conf.sftp;
	state->conf->sftp_preserve_times = conf.sftp_preserve_times;
	state->conf->sftp_show_hidden = conf.sftp_show_hidden;
	printf("MIKUTERM SETTINGS saved %s\n", keyword);
	fflush(stdout);

	/* Succeeded: the setting is kept. */
	return 0;
}

/* Reads config\mikuterm.conf as it is now into settings (its defaults where it says nothing). */
static void
page_conf_now(
	struct mikuterm_conf *conf)
{
	char path[MIKUTERM_PATH_MAX];
	char *text;
	size_t size;
	int error;

	/* The defaults. */
	mikuterm_conf_defaults(conf);

	/* What the file says. */
	error = mikuterm_path(path, sizeof(path), PAGE_CONF);
	if (error != 0)
		return;
	error = mikuterm_file_read(path, MIKUTERM_CONF_MAX, &text, &size);
	if (error != 0)
		return;
	(void)mikuterm_conf_read(conf, text, size, 0);
	free(text);
}

#ifdef _WIN32
/* Writes a new key's comment: the Windows user's name and the computer's, as user@computer. */
static void
page_comment(
	char *comment,
	size_t size)
{
	wchar_t user_wide[257];
	wchar_t computer_wide[MAX_COMPUTERNAME_LENGTH + 1];
	char user[MIKUTERM_HOST_MAX];
	char computer[MIKUTERM_HOST_MAX];
	DWORD length;
	BOOL done;

	/* The user's name. */
	length = sizeof(user_wide) / sizeof(user_wide[0]);
	done = GetUserNameW(user_wide, &length);
	if (!done)
		user_wide[0] = L'\0';
	page_utf8(user_wide, user, sizeof(user));

	/* The computer's. */
	length = sizeof(computer_wide) / sizeof(computer_wide[0]);
	done = GetComputerNameW(computer_wide, &length);
	if (!done)
		computer_wide[0] = L'\0';
	page_utf8(computer_wide, computer, sizeof(computer));

	/* Together; without either, mikuterm's name. */
	if (user[0] == '\0' || computer[0] == '\0') {
		(void)snprintf(comment, size, "mikuterm");
		return;
	}

	/* The user at the computer, as ssh-keygen's comment. */
	(void)snprintf(comment, size, "%s@%s", user, computer);
}

/* Converts a UTF-16 string to UTF-8 (empty when it does not fit or cannot be). */
static void
page_utf8(
	const wchar_t *wide,
	char *text,
	size_t size)
{
	int written;

	/* The conversion, its NUL included. */
	written = WideCharToMultiByte(CP_UTF8, 0, wide, -1, text, (int)size, NULL, NULL);
	if (written <= 0)
		text[0] = '\0';
}
#else
/* Writes a new key's comment: the user's name and the computer's, as user@computer. */
static void
page_comment(
	char *comment,
	size_t size)
{
	const struct passwd *entry;
	char computer[MIKUTERM_HOST_MAX];
	int status;

	/* The user's name. */
	entry = getpwuid(getuid());

	/* The computer's. */
	status = gethostname(computer, sizeof(computer));
	if (status != 0)
		computer[0] = '\0';
	computer[sizeof(computer) - 1U] = '\0';

	/* Together; without either, mikuterm's name. */
	if (entry == NULL ||
	    entry->pw_name == NULL ||
	    entry->pw_name[0] == '\0' ||
	    computer[0] == '\0') {
		(void)snprintf(comment, size, "mikuterm");
		return;
	}

	/* The user at the computer, as ssh-keygen's comment. */
	(void)snprintf(comment, size, "%s@%s", entry->pw_name, computer);
}
#endif
