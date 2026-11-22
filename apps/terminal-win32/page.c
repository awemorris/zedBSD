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
 * with the SSH library's own reader, and mikuterm's own settings of
 * config\mikuterm.conf: the fonts and the grid (from the next start), the
 * TERM and the wait for a connection (from the next connection: the
 * connector takes them at once) and the host of the first tab (from the
 * next start).  A setting is written in place of its line in
 * mikuterm.conf, every other line kept.
 */

#include "mikuterm.h"

#include "userland/desktop/terminal/settings-page.h"
#include "userland/base/ssh/ssh-config.h"

#include <windows.h>

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

/* mikuterm's own settings in the tab: the look and the grid, then the connections. */
static const struct terminal_page_item page_items[] = {
	{ "Font", "Font", TERMINAL_PANE_APPEARANCE, TERMINAL_ITEM_TEXT, TERMINAL_EFFECT_START },
	{ "FontFallback", "Fallback font", TERMINAL_PANE_APPEARANCE, TERMINAL_ITEM_TEXT, TERMINAL_EFFECT_START },
	{ "Columns", "Columns", TERMINAL_PANE_APPEARANCE, TERMINAL_ITEM_NUMBER, TERMINAL_EFFECT_START },
	{ "Rows", "Rows", TERMINAL_PANE_APPEARANCE, TERMINAL_ITEM_NUMBER, TERMINAL_EFFECT_START },
	{ "Term", "TERM", TERMINAL_PANE_BEHAVIOR, TERMINAL_ITEM_TEXT, TERMINAL_EFFECT_CONNECTION },
	{ "ConnectTimeout", "Connect timeout (s)", TERMINAL_PANE_BEHAVIOR, TERMINAL_ITEM_NUMBER, TERMINAL_EFFECT_CONNECTION },
	{ "Connect", "Host at start", TERMINAL_PANE_BEHAVIOR, TERMINAL_ITEM_TEXT, TERMINAL_EFFECT_START }
};

static int page_read(void *data, const char *path, size_t limit, char **text, size_t *size);
static int page_replace(void *data, const char *path, const void *bytes, size_t size);
static int page_create(void *data, const char *path, const void *bytes, size_t size, int secret);
static int page_exists(void *data, const char *path);
static int page_check(void *data, const char *text, size_t size, char *report, size_t capacity);
static int page_host_named(void *data, const char *name, size_t size);
static int page_item_get(void *data, const char *keyword, char *value, size_t size);
static int page_item_set(void *data, const char *keyword, const char *value, char *why, size_t size);
static void page_conf_now(struct mikuterm_conf *conf);
static void page_comment(char *comment, size_t size);
static void page_utf8(const wchar_t *wide, char *text, size_t size);

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

	/* The files' paths: ssh\config, ssh\ with its separator, ssh\known_hosts. */
	*program = NULL;
	error = mikuterm_path(page_state.config, sizeof(page_state.config), PAGE_SSH_CONFIG);
	if (error == 0)
		error = mikuterm_path(page_state.keys, sizeof(page_state.keys) - 1U, PAGE_SSH);
	if (error == 0)
		error = mikuterm_path(page_state.known_hosts, sizeof(page_state.known_hosts), PAGE_KNOWN_HOSTS);
	if (error != 0)
		return error;
	length = strlen(page_state.keys);
	page_state.keys[length] = '\\';
	page_state.keys[length + 1U] = '\0';

	/* A new key's comment, user@computer. */
	page_comment(page_state.comment, sizeof(page_state.comment));
	page_state.conf = conf;
	page_state.connector = connector;

	/* The tab's program. */
	memset(&page_state.program, 0, sizeof(page_state.program));
	page_state.program.config_path = page_state.config;
	page_state.program.config_shown = "ssh\\config";
	page_state.program.key_directory = page_state.keys;
	page_state.program.key_directory_shown = "ssh\\";
	page_state.program.known_hosts_path = page_state.known_hosts;
	page_state.program.known_hosts_shown = "ssh\\known_hosts";
	page_state.program.read = page_read;
	page_state.program.replace = page_replace;
	page_state.program.create = page_create;
	page_state.program.exists = page_exists;
	page_state.program.check = page_check;
	page_state.program.items = page_items;
	page_state.program.item_count = sizeof(page_items) / sizeof(page_items[0]);
	page_state.program.item_get = page_item_get;
	page_state.program.item_set = page_item_set;
	page_state.program.comment = page_state.comment;
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
 * (a font must be in fonts\ or share\fonts\, or a path to one); the TERM
 * and the wait for a connection take effect for the next connection at
 * once.  Returns 0, or EINVAL with why set, or the error of the file.
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
			(void)snprintf(why, size, "there is no such font in fonts\\ or share\\fonts\\");
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
		(void)snprintf(why, size, "config\\mikuterm.conf could not be read (errno %d)", error);
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
		(void)snprintf(why, size, "config\\mikuterm.conf could not be written (errno %d)", error);
		return error;
	}

	/* The next connection's TERM and wait, from the file as it is now. */
	page_conf_now(&conf);
	(void)snprintf(state->conf->term, sizeof(state->conf->term), "%s", conf.term);
	state->conf->connect_timeout = conf.connect_timeout;
	state->connector->options.connect_timeout = conf.connect_timeout;
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
