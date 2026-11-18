/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The applications that open files: which ones fit a file, and starting
 * one on it (spec §14).
 *
 * The ways to open a file come, the first being the default, from the way
 * the user chose for the file's type (Always Open With, ws093-p003), then
 * from three lists, the first match of the earliest list first: the user's
 * own $XDG_CONFIG_HOME/keiland/open-with, the system's
 * /etc/keiland/open-with, and the built-in table below.  A list's line is
 *
 *	PATTERNS<TAB>NAME<TAB>COMMAND
 *
 * where PATTERNS are MIME-type globs separated by commas (a star stands for
 * any characters, as in the shell), NAME is what the window shows, and
 * COMMAND is run by the shell with %f standing for the file's path, quoted
 * for the shell (the path is added at the end when there is no %f).  A command that starts
 * with "@terminal " runs the rest in a new terminal window; "@quicklook"
 * shows the file in Quick Look.  Lines starting with '#' are comments.
 *
 * The lists are read each time a file is opened, so an edit takes effect
 * at once, and nothing of them is kept between openings.  Files only reads
 * them: the lists are the user's and the system's to write.
 *
 * The way chosen for a type is the desktop's setting
 * files.open-with.<type> (libkeiland's kl_settings_*, WS135: "<name><TAB>
 * <command>", kept by libkeiland in ~/.config/keiland/files.conf); Files
 * opens no settings file itself.  The lines an earlier Files wrote into the
 * user's list after a "# set by Files" comment are not read any more.
 */

#include "files.h"

#include <keiland/keiland.h>

#include "userland/desktop/paths.h"

#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The system's list, and the user's under the configuration folder. */
#define APPS_SYSTEM_LIST	KEILAND_SYSCONFDIR "/keiland/open-with"
#define APPS_USER_LIST		"keiland/open-with"

/* The terminal a "@terminal" command runs in, and the words that mark the two special commands. */
#define APPS_TERMINAL		KEILAND_BINDIR "/terminal"
#define APPS_TERMINAL_WORD	"@terminal "
#define APPS_QUICKLOOK_WORD	"@quicklook"

/* The longest line of a list, and the longest command once the path is put in. */
#define APPS_LINE		1024
#define APPS_EXPANDED		(FM_OPENER_COMMAND + 4 * FM_PATH_MAX)

/* The comment an earlier Files put before a line it wrote into the user's list (such lines are passed over). */
#define APPS_MARK		"# set by Files"

/* The application's name in the desktop's settings, and the prefix of the key of a type's chosen way. */
#define APPS_SETTINGS_APP	"files"
#define APPS_SETTINGS_PREFIX	"files.open-with."

/* The picture types Image Viewer reads (it tells them by their first bytes, and reads no others). */
#define APPS_IMAGE_TYPES	"image/png,image/jpeg,image/gif"

/* The types that read as text for the viewers, beyond text/ itself. */
#define APPS_TEXT_TYPES		"text/" "*,application/json,application/xml,application/x-shellscript,application/javascript"

/*
 * The types that are programs: only a file of these runs (BUG-233: every
 * file of a FAT stick has its x bits, a video too, and ran in a terminal).
 */
#define APPS_PROGRAM_TYPE	"application/x-executable"
#define APPS_SCRIPT_TYPE	"application/x-shellscript"

/*
 * What Run in Terminal runs: the program, then a line saying it ended,
 * kept on the screen until Return (BUG-234: a command that ends at once
 * took its window with it, and looked as if nothing had happened).
 */
#define APPS_RUN_IN_TERMINAL	"@terminal %f; status=$?; echo; echo \"[The program ended with status $status. Press Return to close this window.]\"; read reply"

/*
 * The libraries a program that opens windows of its own links (a Wayland
 * client, or an X one), which Files starts without a terminal (BUG-234).
 */
#define APPS_WAYLAND_LIBRARY	"libwayland-client.so"
#define APPS_X_LIBRARY		"libX11.so"

/* The most of a program's headers, of its dynamic section and of its string table read to tell what it links. */
#define APPS_ELF_HEADERS	64U
#define APPS_ELF_DYNAMIC	512U
#define APPS_ELF_STRINGS	65536U

/*
 * One built-in way to open files: the types it fits, its name, its command,
 * and the program it needs (NULL when it needs none), without which it is
 * not offered.
 */
struct apps_builtin {
	const char *patterns;
	const char *name;
	const char *command;
	const char *needs;
};

/*
 * The built-in ways, after the lists of the user and the system: the
 * system's defaults.  Each application opens the kinds it reads when it is
 * installed (ws093-p002): a PDF in PDF Viewer (ws079-p006), a PNG, JPEG or
 * GIF picture in Image Viewer (which reads no other kind; the others stay
 * with Quick Look), a video in Video Player (ws122-p002), an m4a song in
 * Music (ws120-p009), an HTML page in
 * the Browser, and text in Text Editor.
 * Anything the others do not fit is shown by less in a terminal.
 */
static const struct apps_builtin apps_builtins[] = {
	{ "application/pdf", "PDF Viewer", KEILAND_BINDIR "/pdfviewer %f", "pdfviewer" },
	{ APPS_IMAGE_TYPES, "Image Viewer", KEILAND_BINDIR "/imageview %f", "imageview" },
	{ "image/" "*", "Quick Look", "@quicklook", NULL },
	{ "video/" "*", "Video Player", KEILAND_BINDIR "/videoplayer %f", "videoplayer" },
	{ "audio/mp4", "Music", KEILAND_BINDIR "/music %f", "music" },
	{ "text/html", "Browser", KEILAND_BINDIR "/browser %f", "browser" },
	{ APPS_TEXT_TYPES, "Text Editor", KEILAND_BINDIR "/textedit %f", "textedit" },
	{ APPS_TEXT_TYPES, "Terminal (less)", "@terminal less %f", NULL },
	{ APPS_TEXT_TYPES, "Emacs", "@terminal emacs %f", "emacs" },
	{ APPS_TEXT_TYPES, "Terminal (ed)", "@terminal ed %f", "ed" },
	{ "*", "Terminal (less)", "@terminal less %f", NULL }
};

/* The folders a needed program is looked for in. */
static const char *const apps_program_folders[] = {KEILAND_BINDIR, "/bin", "/usr/bin", "/usr/local/bin"};

/*
 * The application programs are started from, and its window they are
 * started from (fm_apps_attach); NULL before it is attached.  Only the
 * application's thread uses them.
 */
static struct kl_app *apps_app;
static struct kl_window *apps_window;

static int apps_user_list(char *list, size_t size);
static int apps_choice_key(const char *type, char *key, size_t size);
static int apps_choice(const char *type, struct fm_opener *opener);
static int apps_choice_set(const char *type, const struct fm_opener *opener);
static int apps_choice_clear(const char *type);
static int apps_choice_write(const char *key, const char *value);
static int apps_is_mark(const char *line);
static void apps_read_list(const char *path, const char *type, struct fm_opener *openers, int capacity, int *count);
static int apps_parse_line(char *line, char **patterns, char **name, char **command);
static int apps_matches(const char *patterns, const char *type);
static void apps_add(struct fm_opener *openers, int capacity, int *count, const char *name, const char *command);
static int apps_program_exists(const char *program);
static int apps_is_program(const struct fm_mime *mime, mode_t mode);
static int apps_opens_windows(const char *path);
static int apps_elf_needs_window(int descriptor);
static int apps_elf_file_offset(const unsigned char *headers, unsigned count, uint64_t address, uint64_t *offset);
static int apps_read_at(int descriptor, uint64_t offset, void *buffer, size_t length);
static uint64_t apps_le(const unsigned char *bytes, unsigned width);
static int apps_expand(const char *command, const char *path, char *expanded, size_t size);
static int apps_quote(const char *path, char *quoted, size_t size);

/*
 * Fills the ways a file can be opened, the default first, and reports how
 * many there are (at least one: the built-in viewer fits anything).
 *
 * A file whose mode lets it run is first offered to run in a terminal.
 */
int
fm_apps_for(
	const char *path,
	const struct fm_mime *mime,
	mode_t mode,
	struct fm_opener *openers,
	int capacity)
{
	struct fm_opener chosen;
	char list[FM_PATH_MAX];
	size_t index;
	int program;
	int windows;
	int matched;
	int count;
	int found;
	int error;

	/*
	 * A program runs first: one that opens windows of its own by itself
	 * (BUG-234), any other in a terminal that stays when it ends.  Only a
	 * program's type runs (BUG-233).
	 */
	count = 0;
	program = apps_is_program(mime, mode);
	if (program != 0) {
		windows = apps_opens_windows(path);
		if (windows != 0)
			apps_add(openers, capacity, &count, "Open", "%f");
		apps_add(openers, capacity, &count, "Run in Terminal", APPS_RUN_IN_TERMINAL);
	}

	/* Offers the way the user chose for the type first, when there is one. */
	error = apps_choice(mime->type, &chosen);
	if (error == 0)
		apps_add(openers, capacity, &count, chosen.name, chosen.command);

	/* The user's list, when there is a place for it. */
	error = apps_user_list(list, sizeof(list));
	if (error == 0)
		apps_read_list(list, mime->type, openers, capacity, &count);

	/* The system's list. */
	apps_read_list(APPS_SYSTEM_LIST, mime->type, openers, capacity, &count);

	/* The built-in ways that fit, and whose program is there. */
	for (index = 0; index < sizeof(apps_builtins) / sizeof(apps_builtins[0]); index++) {
		matched = apps_matches(apps_builtins[index].patterns, mime->type);
		if (matched == 0)
			continue;

		/* A way that needs a program the system does not have is left out. */
		if (apps_builtins[index].needs != NULL) {
			found = apps_program_exists(apps_builtins[index].needs);
			if (found == 0)
				continue;
		}

		/* It is offered. */
		apps_add(openers, capacity, &count, apps_builtins[index].name, apps_builtins[index].command);
	}

	/* Reports how many ways there are. */
	return count;
}

/*
 * Tells whether an opener is Quick Look, which the window shows itself
 * rather than starting a program.
 */
int
fm_apps_is_quicklook(
	const struct fm_opener *opener)
{
	int match;

	/* The command is the Quick Look word alone. */
	match = strcmp(opener->command, APPS_QUICKLOOK_WORD);
	if (match != 0)
		return 0;

	/* It is Quick Look. */
	return 1;
}

/*
 * Attaches the application (and its window, NULL for none) the programs
 * are started from: a program started brings its window to the front from
 * that window.
 */
void
fm_apps_attach(
	struct kl_app *app,
	struct kl_window *window)
{
	/* The application and the window, for the starts to come. */
	apps_app = app;
	apps_window = window;
}

/*
 * Starts an opener's command on a file, apart from the file manager (it
 * outlives the window): a "@terminal" command in a new terminal window,
 * any other by the system's command interpreter.
 *
 * Returns 0, ENAMETOOLONG when the command with the path is too long, or
 * the error of a start that failed.
 */
int
fm_apps_launch(
	const struct fm_opener *opener,
	const char *path)
{
	char terminal_command[APPS_EXPANDED + 16];
	char expanded[APPS_EXPANDED];
	struct kl_launch_options options;
	const char *arguments[3];
	int is_terminal;
	int error;

	/* The command with the path put in. */
	error = apps_expand(opener->command, path, expanded, sizeof(expanded));
	if (error != 0)
		return error;

	/* A terminal command: the rest runs in a new terminal window; anything else runs by the command interpreter. */
	memset(&options, 0, sizeof(options));
	options.from = apps_window;
	is_terminal = strncmp(expanded, APPS_TERMINAL_WORD, strlen(APPS_TERMINAL_WORD));
	if (is_terminal == 0) {
		snprintf(terminal_command, sizeof(terminal_command), "--command=%s", expanded + strlen(APPS_TERMINAL_WORD));
		arguments[0] = APPS_TERMINAL;
		arguments[1] = terminal_command;
		arguments[2] = NULL;
		options.argv = arguments;
	} else {
		options.command = expanded;
	}

	/* The program, on its way apart from the file manager. */
	error = kl_launch(apps_app, &options);
	if (error != 0)
		return error;

	/* The log line the tests wait for. */
	fm_log("LAUNCH name=%s command=%s", opener->name, expanded);

	/* Succeeded: the command is on its way. */
	return 0;
}

/*
 * Starts a program with its arguments apart from the file manager, as a
 * launch does (a new window of this program writes its log where this one
 * does).
 *
 * Returns 0, or the error of a start that failed.
 */
int
fm_apps_spawn(
	char *const arguments[])
{
	struct kl_launch_options options;
	int error;

	/* The program, on its way apart from the file manager. */
	memset(&options, 0, sizeof(options));
	options.argv = (const char *const *)arguments;
	options.from = apps_window;
	error = kl_launch(apps_app, &options);
	if (error != 0)
		return error;
	fm_log("SPAWN program=%s", arguments[0]);

	/* Succeeded: the program is on its way. */
	return 0;
}

/*
 * Makes a way the default for one type: the desktop's setting
 * files.open-with.<type> takes the way's name and command.
 *
 * Returns 0, or an errno value (EINVAL for a type or a way the setting
 * cannot hold, ENOENT without a home) with the default as it was.
 */
int
fm_apps_set_default(
	const char *type,
	const struct fm_opener *opener)
{
	int error;

	/* Sets the type's setting to the way. */
	error = apps_choice_set(type, opener);

	/* The log line the tests read. */
	fm_log("DEFAULT set type=%s app=%s error=%d", type, opener->name, error);

	/* Reports why the default could not be changed. */
	if (error != 0)
		return error;

	/* Succeeded: the way opens the type from now on. */
	return 0;
}

/*
 * Gives one type back to the system's default: the setting of the type
 * goes back to having no way (the user's own list stays as it is).
 *
 * Returns 0, or an errno value with the default as it was.
 */
int
fm_apps_clear_default(
	const char *type)
{
	int error;

	/* Puts the type's setting back at its default. */
	error = apps_choice_clear(type);

	/* The log line the tests read. */
	fm_log("DEFAULT clear type=%s error=%d", type, error);

	/* Reports why the default could not be given back. */
	if (error != 0)
		return error;

	/* Succeeded: the type opens with the system's default again. */
	return 0;
}

/*
 * Tells whether the user chose a way for a type: 1 when the desktop's
 * setting of the type holds one, 0 when not.
 */
int
fm_apps_has_default(
	const char *type)
{
	struct fm_opener chosen;
	int error;

	/* Reads the type's chosen way; without one the user chose none. */
	error = apps_choice(type, &chosen);
	if (error != 0)
		return 0;

	/* The user chose one. */
	return 1;
}

/* Makes a type's key in the desktop's settings (files.open-with.<type>); returns 0 or EINVAL for a type it cannot hold. */
static int
apps_choice_key(
	const char *type,
	char *key,
	size_t size)
{
	int written;

	/* Joins the prefix and the type; a key that does not fit is refused. */
	written = snprintf(key, size, "%s%s", APPS_SETTINGS_PREFIX, type);
	if (written < 0 || (size_t)written >= size)
		return EINVAL;

	/* Succeeded: the key (libkeiland checks the type's characters). */
	return 0;
}

/* Reads the way chosen for a type from the desktop's settings; returns 0, or an errno value when there is none. */
static int
apps_choice(
	const char *type,
	struct fm_opener *opener)
{
	struct kl_settings *settings;
	char key[KL_SETTINGS_KEY_MAX];
	char value[KL_SETTINGS_VALUE_MAX];
	size_t name_length;
	size_t command_length;
	char *tab;
	int error;

	/* Makes the type's key. */
	error = apps_choice_key(type, key, sizeof(key));
	if (error != 0)
		return error;

	/* Opens the settings now (another window may have chosen meanwhile). */
	settings = kl_settings_open(NULL, APPS_SETTINGS_APP);
	if (settings == NULL)
		return ENOMEM;

	/* Reads the setting; without a way chosen there is none. */
	error = kl_settings_get(settings, key, value, sizeof(value), NULL);
	if (error != 0) {
		kl_settings_close(settings);
		return error;
	}

	/* Closes the settings, which are not needed any more. */
	kl_settings_close(settings);

	/* Finds the tab between the name and the command. */
	tab = strchr(value, '\t');
	if (tab == NULL)
		return EINVAL;

	/* Ends the name at the tab; a name too long for the window is refused. */
	*tab = '\0';
	name_length = strlen(value);
	if (name_length >= sizeof(opener->name))
		return EINVAL;

	/* Gives the caller the name and the command after the tab. */
	memcpy(opener->name, value, name_length + 1U);
	command_length = strlen(tab + 1);
	memcpy(opener->command, tab + 1, command_length + 1U);

	/* Succeeded: the chosen way. */
	return 0;
}

/* Sets the desktop's setting of a type to a way ("<name><TAB><command>"); returns 0 or an errno value. */
static int
apps_choice_set(
	const char *type,
	const struct fm_opener *opener)
{
	char key[KL_SETTINGS_KEY_MAX];
	char value[KL_SETTINGS_VALUE_MAX];
	int written;
	int error;

	/* Makes the type's key. */
	error = apps_choice_key(type, key, sizeof(key));
	if (error != 0)
		return error;

	/* Writes the way as "<name><TAB><command>"; one the setting cannot hold is refused. */
	written = snprintf(value, sizeof(value), "%s\t%s", opener->name, opener->command);
	if (written < 0 || (size_t)written >= sizeof(value))
		return EINVAL;

	/* Sets the key to the way. */
	error = apps_choice_write(key, value);
	if (error != 0)
		return error;

	/* Succeeded: the way is the type's default. */
	return 0;
}

/* Puts the desktop's setting of a type back at its default (no way chosen); returns 0 or an errno value. */
static int
apps_choice_clear(
	const char *type)
{
	char key[KL_SETTINGS_KEY_MAX];
	int error;

	/* Makes the type's key. */
	error = apps_choice_key(type, key, sizeof(key));
	if (error != 0)
		return error;

	/* Puts the key back at its default. */
	error = apps_choice_write(key, NULL);
	if (error != 0)
		return error;

	/* Succeeded: no way is chosen for the type. */
	return 0;
}

/* Sets a key of Files' own settings (value NULL: back at its default), which libkeiland keeps; returns 0 or an errno value. */
static int
apps_choice_write(
	const char *key,
	const char *value)
{
	struct kl_settings *settings;
	int error;

	/* Opens Files' own settings. */
	settings = kl_settings_open(NULL, APPS_SETTINGS_APP);
	if (settings == NULL)
		return ENOMEM;

	/* Sets the key, or puts it back at its default. */
	if (value != NULL) {
		error = kl_settings_set(settings, key, value, NULL);
	} else {
		error = kl_settings_reset(settings, key, NULL);
	}

	/* A refusal leaves the setting as it was. */
	if (error != 0) {
		kl_settings_close(settings);
		return error;
	}

	/* Closes the settings, which are not needed any more. */
	kl_settings_close(settings);

	/* Succeeded: libkeiland keeps the key. */
	return 0;
}

/* Finds the user's list: under $XDG_CONFIG_HOME, or ~/.config; ENOENT when neither is set. */
static int
apps_user_list(
	char *list,
	size_t size)
{
	const char *config;
	const char *home;
	int written;

	/* $XDG_CONFIG_HOME, when it is set. */
	config = getenv("XDG_CONFIG_HOME");
	if (config != NULL && config[0] != '\0') {
		written = snprintf(list, size, "%s/%s", config, APPS_USER_LIST);
		if (written < 0 || (size_t)written >= size)
			return ENAMETOOLONG;

		/* Succeeded: the list under the configuration folder. */
		return 0;
	}

	/* Otherwise ~/.config; without a home there is no user's list. */
	home = getenv("HOME");
	if (home == NULL || home[0] == '\0')
		return ENOENT;

	/* The list under the home's configuration folder. */
	written = snprintf(list, size, "%s/.config/%s", home, APPS_USER_LIST);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded: the list under ~/.config. */
	return 0;
}

/* Tells whether a line of a list (with its end) is the comment an earlier Files put before its own lines. */
static int
apps_is_mark(
	const char *line)
{
	size_t length;
	int differs;

	/* The comment, followed by the line's end or nothing. */
	length = strlen(APPS_MARK);
	differs = strncmp(line, APPS_MARK, length);
	if (differs != 0)
		return 0;

	/* Nothing else may follow on the line. */
	if (line[length] == '\0')
		return 1;
	if (line[length] == '\n')
		return 1;
	if (line[length] == '\r')
		return 1;

	/* A longer comment is the user's. */
	return 0;
}

/* Adds the openers of a list that fit a type; a list that cannot be read adds none. */
static void
apps_read_list(
	const char *path,
	const char *type,
	struct fm_opener *openers,
	int capacity,
	int *count)
{
	char line[APPS_LINE];
	char *patterns;
	char *name;
	char *command;
	char *read;
	FILE *file;
	int after_mark;
	int is_mark;
	int parsed;
	int matched;

	/* The list. */
	file = fopen(path, "r");
	if (file == NULL)
		return;

	/* Each line that names a way to open the type. */
	after_mark = 0;
	for (;;) {
		read = fgets(line, sizeof(line), file);
		if (read == NULL)
			break;

		/* A line an earlier Files wrote (after its comment) is passed over: the choice is a setting now. */
		is_mark = apps_is_mark(line);
		if (after_mark) {
			/* after_mark tells the next line whether this one is Files' comment. */
			after_mark = is_mark;
			continue;
		}

		/* Remembers for the next line whether this one is Files' comment. */
		after_mark = is_mark;

		/* A comment, a blank or a malformed line says nothing. */
		parsed = apps_parse_line(line, &patterns, &name, &command);
		if (parsed == 0)
			continue;

		/* A line for other types says nothing either. */
		matched = apps_matches(patterns, type);
		if (matched == 0)
			continue;

		/* The way is offered. */
		apps_add(openers, capacity, count, name, command);
	}

	/* The list is not needed any more. */
	fclose(file);
}

/* Splits a list's line at its tabs into its patterns, name and command; zero for a line that is not one. */
static int
apps_parse_line(
	char *line,
	char **patterns,
	char **name,
	char **command)
{
	char *tab;
	size_t length;

	/* The line without its end. */
	length = strlen(line);
	while (length > 0U && (line[length - 1U] == '\n' || line[length - 1U] == '\r')) {
		line[length - 1U] = '\0';
		length--;
	}

	/* A blank line and a comment are not ways. */
	if (line[0] == '\0' || line[0] == '#')
		return 0;

	/* The patterns end at the first tab. */
	*patterns = line;
	tab = strchr(line, '\t');
	if (tab == NULL)
		return 0;
	*tab = '\0';

	/* The name ends at the second. */
	*name = tab + 1;
	tab = strchr(*name, '\t');
	if (tab == NULL)
		return 0;
	*tab = '\0';

	/* The command is the rest, and none of the three may be empty. */
	*command = tab + 1;
	if ((*patterns)[0] == '\0' || (*name)[0] == '\0' || (*command)[0] == '\0')
		return 0;

	/* Succeeded: the line is a way to open files. */
	return 1;
}

/* Tells whether a type fits one of the comma-separated globs of a list. */
static int
apps_matches(
	const char *patterns,
	const char *type)
{
	char pattern[128];
	const char *start;
	const char *end;
	size_t length;
	int miss;

	/* Each glob between the commas, spaces around it left out. */
	start = patterns;
	while (*start != '\0') {
		while (*start == ' ' || *start == ',')
			start++;
		if (*start == '\0')
			break;
		end = strchr(start, ',');
		if (end == NULL)
			end = start + strlen(start);

		/* The glob alone, without the spaces after it. */
		length = (size_t)(end - start);
		while (length > 0U && start[length - 1U] == ' ')
			length--;
		if (length >= sizeof(pattern))
			length = sizeof(pattern) - 1U;
		memcpy(pattern, start, length);
		pattern[length] = '\0';

		/* The type fits this glob. */
		miss = fnmatch(pattern, type, 0);
		if (miss == 0)
			return 1;

		/* The next glob. */
		start = end;
	}

	/* No glob fits. */
	return 0;
}

/* Adds a way to open a file unless one of the same name is there already or the list is full. */
static void
apps_add(
	struct fm_opener *openers,
	int capacity,
	int *count,
	const char *name,
	const char *command)
{
	int index;
	int match;

	/* No room. */
	if (*count >= capacity)
		return;

	/* A name offered already (by an earlier list) is not offered twice. */
	for (index = 0; index < *count; index++) {
		match = strcmp(openers[index].name, name);
		if (match == 0)
			return;
	}

	/* The way, at the end. */
	snprintf(openers[*count].name, sizeof(openers[*count].name), "%s", name);
	snprintf(openers[*count].command, sizeof(openers[*count].command), "%s", command);
	(*count)++;
}

/* Tells whether a program of a name is in one of the folders programs are looked for in. */
static int
apps_program_exists(
	const char *program)
{
	char path[FM_PATH_MAX];
	size_t index;
	int error;

	/* Each folder, for a file that may run. */
	for (index = 0; index < sizeof(apps_program_folders) / sizeof(apps_program_folders[0]); index++) {
		snprintf(path, sizeof(path), "%s/%s", apps_program_folders[index], program);
		error = access(path, X_OK);
		if (error == 0)
			return 1;
	}

	/* No folder has it. */
	return 0;
}

/* Tells whether a file is a program to run: it may run, and its type is a program's or a script's (BUG-233). */
static int
apps_is_program(
	const struct fm_mime *mime,
	mode_t mode)
{
	int regular;
	int differs;

	/* A file whose mode does not let it run is no program. */
	regular = S_ISREG(mode);
	if (regular == 0)
		return 0;
	if ((mode & 0111) == 0)
		return 0;

	/* A program's type. */
	differs = strcmp(mime->type, APPS_PROGRAM_TYPE);
	if (differs == 0)
		return 1;

	/* A script's type. */
	differs = strcmp(mime->type, APPS_SCRIPT_TYPE);
	if (differs == 0)
		return 1;

	/* Any other type -- a video, a picture -- opens as what it is, whatever its x bits. */
	return 0;
}

/*
 * Tells whether a program opens windows of its own: an ELF program that
 * links the Wayland or the X client library (BUG-234).  A file that cannot
 * be read, or that is no ELF program of 64 bits, is taken for one that
 * does not, and runs in a terminal as before.
 */
static int
apps_opens_windows(
	const char *path)
{
	int descriptor;
	int windows;

	/* The file. */
	descriptor = open(path, O_RDONLY);
	if (descriptor < 0)
		return 0;

	/* What it links. */
	windows = apps_elf_needs_window(descriptor);
	(void)close(descriptor);

	/* Reports what the program links. */
	return windows;
}

/*
 * Reads an ELF program's libraries (its DT_NEEDED names) and tells whether
 * one of them is a window system's client library.  Only a 64-bit
 * little-endian ELF file is read; each table is read within a bound.
 */
static int
apps_elf_needs_window(
	int descriptor)
{
	unsigned char header[64];
	unsigned char headers[APPS_ELF_HEADERS * 56U];
	unsigned char dynamic[APPS_ELF_DYNAMIC * 16U];
	unsigned char *strings;
	uint64_t needed[APPS_ELF_DYNAMIC];
	uint64_t header_offset;
	uint64_t dynamic_offset;
	uint64_t dynamic_size;
	uint64_t strings_address;
	uint64_t strings_offset;
	uint64_t strings_size;
	uint64_t tag;
	uint64_t type;
	unsigned header_count;
	unsigned header_size;
	unsigned needed_count;
	unsigned entries;
	unsigned index;
	int windows;
	int differs;
	int error;

	/* The ELF header: the magic, 64 bits (class 2), little-endian (data 1). */
	error = apps_read_at(descriptor, 0U, header, sizeof(header));
	if (error != 0)
		return 0;
	differs = memcmp(header, "\177ELF", 4U);
	if (differs != 0)
		return 0;
	if (header[4] != 2U || header[5] != 1U)
		return 0;

	/* The program headers, each of the 56 bytes of ELF64. */
	header_offset = apps_le(header + 0x20U, 8U);
	header_size = (unsigned)apps_le(header + 0x36U, 2U);
	header_count = (unsigned)apps_le(header + 0x38U, 2U);
	if (header_size != 56U || header_count == 0U || header_count > APPS_ELF_HEADERS)
		return 0;
	error = apps_read_at(descriptor, header_offset, headers, (size_t)header_count * 56U);
	if (error != 0)
		return 0;

	/* The dynamic segment (PT_DYNAMIC, 2); a program without one links nothing. */
	dynamic_offset = 0U;
	dynamic_size = 0U;
	for (index = 0; index < header_count; index++) {
		type = apps_le(headers + index * 56U, 4U);
		if (type != 2U)
			continue;
		dynamic_offset = apps_le(headers + index * 56U + 8U, 8U);
		dynamic_size = apps_le(headers + index * 56U + 32U, 8U);
		break;
	}
	if (dynamic_size == 0U)
		return 0;

	/* The dynamic entries, 16 bytes each, as many as are read. */
	entries = (unsigned)(dynamic_size / 16U);
	if (entries > APPS_ELF_DYNAMIC)
		entries = APPS_ELF_DYNAMIC;
	error = apps_read_at(descriptor, dynamic_offset, dynamic, (size_t)entries * 16U);
	if (error != 0)
		return 0;

	/* The libraries' names (DT_NEEDED, 1) and the string table (DT_STRTAB, 5; DT_STRSZ, 10), until DT_NULL. */
	needed_count = 0;
	strings_address = 0U;
	strings_size = 0U;
	for (index = 0; index < entries; index++) {
		tag = apps_le(dynamic + index * 16U, 8U);
		if (tag == 0U)
			break;
		if (tag == 1U) {
			needed[needed_count] = apps_le(dynamic + index * 16U + 8U, 8U);
			needed_count++;
		} else if (tag == 5U) {
			strings_address = apps_le(dynamic + index * 16U + 8U, 8U);
		} else if (tag == 10U) {
			strings_size = apps_le(dynamic + index * 16U + 8U, 8U);
		}
	}
	if (needed_count == 0U || strings_size == 0U)
		return 0;
	if (strings_size > APPS_ELF_STRINGS)
		strings_size = APPS_ELF_STRINGS;

	/* The string table, found in the file through the loaded segment that holds its address. */
	error = apps_elf_file_offset(headers, header_count, strings_address, &strings_offset);
	if (error != 0)
		return 0;
	strings = malloc((size_t)strings_size + 1U);
	if (strings == NULL)
		return 0;
	error = apps_read_at(descriptor, strings_offset, strings, (size_t)strings_size);
	if (error != 0) {
		free(strings);
		return 0;
	}
	strings[strings_size] = '\0';

	/* A library of a window system among the names, with any version after it ("libwayland-client.so.0" on Linux). */
	windows = 0;
	for (index = 0; index < needed_count && windows == 0; index++) {
		if (needed[index] >= strings_size)
			continue;
		differs = strncmp((const char *)strings + needed[index], APPS_WAYLAND_LIBRARY, strlen(APPS_WAYLAND_LIBRARY));
		if (differs == 0)
			windows = 1;
		differs = strncmp((const char *)strings + needed[index], APPS_X_LIBRARY, strlen(APPS_X_LIBRARY));
		if (differs == 0)
			windows = 1;
	}
	free(strings);

	/* Reports whether the program links a window system's library. */
	return windows;
}

/* Finds where an address of a program lies in its file, through the loaded segment (PT_LOAD, 1) that holds it; returns 0 or -1. */
static int
apps_elf_file_offset(
	const unsigned char *headers,
	unsigned count,
	uint64_t address,
	uint64_t *offset)
{
	const unsigned char *entry;
	uint64_t type;
	uint64_t segment_offset;
	uint64_t segment_address;
	uint64_t segment_size;
	unsigned index;

	/* Each loaded segment, for the one that holds the address in its file bytes. */
	for (index = 0; index < count; index++) {
		entry = headers + index * 56U;
		type = apps_le(entry, 4U);
		if (type != 1U)
			continue;
		segment_offset = apps_le(entry + 8U, 8U);
		segment_address = apps_le(entry + 16U, 8U);
		segment_size = apps_le(entry + 32U, 8U);
		if (address < segment_address)
			continue;
		if (address - segment_address >= segment_size)
			continue;
		*offset = segment_offset + (address - segment_address);
		return 0;
	}

	/* No loaded segment holds it. */
	return -1;
}

/* Reads bytes at an offset of a file, all of them; returns 0, or -1 for a short or failed read. */
static int
apps_read_at(
	int descriptor,
	uint64_t offset,
	void *buffer,
	size_t length)
{
	ssize_t got;

	/* The offset must be one the file can be read at. */
	if (offset > (uint64_t)0x7fffffffffffffffULL)
		return -1;

	/* The bytes, in one read. */
	got = pread(descriptor, buffer, length, (off_t)offset);
	if (got < 0)
		return -1;
	if ((size_t)got != length)
		return -1;

	/* Succeeded: the bytes are read. */
	return 0;
}

/* Reads a little-endian number of a width of bytes (2, 4 or 8). */
static uint64_t
apps_le(
	const unsigned char *bytes,
	unsigned width)
{
	uint64_t value;
	unsigned index;

	/* The bytes, the last the most significant. */
	value = 0U;
	for (index = width; index > 0U; index--)
		value = (value << 8) | bytes[index - 1U];

	/* Reports the number. */
	return value;
}

/*
 * Writes a command with each %f replaced by the path quoted for the shell
 * (the path added at the end when there is none); returns 0 or
 * ENAMETOOLONG.
 */
static int
apps_expand(
	const char *command,
	const char *path,
	char *expanded,
	size_t size)
{
	char quoted[4 * FM_PATH_MAX];
	size_t length;
	size_t done;
	int placed;
	int error;

	/* The path, quoted once for all the places it goes. */
	error = apps_quote(path, quoted, sizeof(quoted));
	if (error != 0)
		return error;
	length = strlen(quoted);

	/* The command, character by character, the path where %f is. */
	done = 0;
	placed = 0;
	while (*command != '\0') {
		if (command[0] == '%' && command[1] == 'f') {
			if (done + length >= size)
				return ENAMETOOLONG;
			memcpy(expanded + done, quoted, length);
			done += length;
			command += 2;
			placed = 1;
			continue;
		}

		/* Any other character is kept. */
		if (done + 1U >= size)
			return ENAMETOOLONG;
		expanded[done] = *command;
		done++;
		command++;
	}

	/* A command without %f takes the path at its end. */
	if (placed == 0) {
		if (done + 1U + length >= size)
			return ENAMETOOLONG;
		expanded[done] = ' ';
		memcpy(expanded + done + 1U, quoted, length);
		done += 1U + length;
	}

	/* Succeeded: the command ends there. */
	expanded[done] = '\0';
	return 0;
}

/* Quotes a path for the shell: in single quotes, each single quote written as '\''; returns 0 or ENAMETOOLONG. */
static int
apps_quote(
	const char *path,
	char *quoted,
	size_t size)
{
	size_t done;

	/* The opening quote. */
	if (size < 3U)
		return ENAMETOOLONG;
	quoted[0] = '\'';
	done = 1;

	/* Each character; a single quote closes the quote, is escaped, and opens it again. */
	while (*path != '\0') {
		if (done + 5U >= size)
			return ENAMETOOLONG;
		if (*path == '\'') {
			memcpy(quoted + done, "'\\''", 4);
			done += 4;
		} else {
			quoted[done] = *path;
			done++;
		}

		/* The next character. */
		path++;
	}

	/* The closing quote. */
	quoted[done] = '\'';
	quoted[done + 1U] = '\0';

	/* Succeeded: the path is quoted. */
	return 0;
}
