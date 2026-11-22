/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * mikuterm's settings files, written as ssh_config is: one "Keyword value"
 * (or "Keyword=value") a line, the keyword in any case, a value in double
 * quotes when it holds spaces, '#' starting a comment line, blank lines
 * skipped, and the first line of a keyword the one that counts.
 *
 * config\mikuterm.conf is the user's: Font, FontFallback, FontSize,
 * Columns, Rows, Theme, AmbiguousWide, Term, Connect, CopyOnSelect,
 * ConnectTimeout and WslPort.  A line mikuterm cannot use is skipped, the default
 * kept, and named in the problems the first tab shows ("line N: why").
 * config\state.conf is mikuterm's own (settings-file.c): FontSize, Theme,
 * AmbiguousWide and CopyOnSelect as the View menu and the settings' tab
 * chose them; it reads nothing else.
 *
 * The settings' tab changes mikuterm.conf in place (mikuterm_conf_set):
 * the first line of a keyword takes the new value, the other lines and
 * the comments stay as the user wrote them, and a keyword not there yet
 * is added at the end.
 */

#include "mikuterm.h"

#include "userland/desktop/terminal/terminal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The longest line and the longest keyword read. */
#define CONF_LINE_MAX		1024U
#define CONF_KEYWORD_MAX	32U

/* The default grid, font size, TERM and wait for a connection (seconds). */
#define CONF_COLUMNS		90U
#define CONF_ROWS		28U
#define CONF_FONT_SIZE		16U
#define CONF_TERM		"xterm-256color"
#define CONF_CONNECT_TIMEOUT	15U

/* The port of the sshd in WSL a distribution is reached on, when ssh\config has no Host for it. */
#define CONF_WSL_PORT		2222U

/* The longest wait for a connection the settings may ask, in seconds. */
#define CONF_CONNECT_TIMEOUT_MAX	3600U

/* The keywords, each a bit of the mask of those already given. */
enum conf_keyword {
	CONF_FONT = 0,
	CONF_FALLBACK = 1,
	CONF_SIZE = 2,
	CONF_COLUMNS_KEY = 3,
	CONF_ROWS_KEY = 4,
	CONF_THEME = 5,
	CONF_AMBIGUOUS = 6,
	CONF_TERM_KEY = 7,
	CONF_CONNECT = 8,
	CONF_COPY = 9,
	CONF_TIMEOUT = 10,
	CONF_WSL = 11,
	CONF_KEYWORDS = 12
};

/*
 * One keyword of the files: its name and whether state.conf may give it
 * (the View menu's settings).
 */
struct conf_name {
	const char *name;
	int state;
};

/* The keywords' names, in the order of enum conf_keyword. */
static const struct conf_name conf_names[CONF_KEYWORDS] = {
	{ "Font", 0 },
	{ "FontFallback", 0 },
	{ "FontSize", 1 },
	{ "Columns", 0 },
	{ "Rows", 0 },
	{ "Theme", 1 },
	{ "AmbiguousWide", 1 },
	{ "Term", 0 },
	{ "Connect", 0 },
	{ "CopyOnSelect", 1 },
	{ "ConnectTimeout", 0 },
	{ "WslPort", 0 }
};

/* The themes' names, in the order of TERMINAL_THEME_*. */
static const char *const conf_themes[TERMINAL_THEMES] = {
	"dark",
	"light",
	"contrast"
};

static int conf_line(struct mikuterm_conf *conf, const char *line, size_t length, int state, unsigned *given, const char **why);
static int conf_split(const char *line, size_t length, char *keyword, size_t keyword_size, char *text, size_t text_size, const char **value, const char **why);
static int conf_keyword(const char *keyword, unsigned *found);
static int conf_value(struct mikuterm_conf *conf, unsigned keyword, const char *value, const char **why);
static int conf_number(const char *text, unsigned minimum, unsigned maximum, unsigned *number);
static int conf_yes(const char *text, int *yes);
static int conf_copy(char *to, size_t size, const char *text);
static void conf_problem(struct mikuterm_conf *conf, unsigned line, const char *why);

/* Fills the settings with mikuterm's defaults. */
void
mikuterm_conf_defaults(
	struct mikuterm_conf *conf)
{
	/* Nothing given; the fonts are the folder's own (share\fonts). */
	memset(conf, 0, sizeof(*conf));
	(void)conf_copy(conf->font, sizeof(conf->font), "keiland-mono.ttf");
	(void)conf_copy(conf->fallback, sizeof(conf->fallback), "keiland-fallback.ttf");

	/* The grid, the font's size and the look of zedBSD's terminal. */
	conf->font_size = CONF_FONT_SIZE;
	conf->columns = CONF_COLUMNS;
	conf->rows = CONF_ROWS;
	conf->theme = TERMINAL_THEME_DARK;
	conf->ambiguous_wide = 0;

	/* The connections: xterm's TERM, the list of hosts first, a selection copied, 15 seconds to connect, WSL's sshd on 2222. */
	(void)conf_copy(conf->term, sizeof(conf->term), CONF_TERM);
	conf->connect[0] = '\0';
	conf->copy_on_select = 1;
	conf->connect_timeout = CONF_CONNECT_TIMEOUT;
	conf->wsl_port = CONF_WSL_PORT;
}

/*
 * Reads a settings file's text into the settings: mikuterm.conf (state 0)
 * or state.conf (state 1, the View menu's keywords alone; anything else is
 * passed over in silence).  A line that cannot be used leaves the value as
 * it was and adds a problem.  Returns 0, or EINVAL when there was a
 * problem.
 */
int
mikuterm_conf_read(
	struct mikuterm_conf *conf,
	const char *text,
	size_t size,
	int state)
{
	const char *line;
	const char *end;
	const char *why;
	unsigned given;
	unsigned number;
	size_t length;
	int problems;
	int status;

	/* Each line in turn, numbered from 1. */
	given = 0U;
	number = 0U;
	problems = 0;
	line = text;
	while (line < text + size) {
		/* The line's end (a CR before the LF is not the line's). */
		number++;
		end = memchr(line, '\n', (size_t)(text + size - line));
		if (end == NULL)
			end = text + size;
		length = (size_t)(end - line);
		if (length != 0U && line[length - 1U] == '\r')
			length--;

		/* The line, or why it cannot be used. */
		why = NULL;
		status = conf_line(conf, line, length, state, &given, &why);
		if (status != 0) {
			problems = 1;
			if (!state)
				conf_problem(conf, number, why);
		}

		/* The next line. */
		line = end + 1;
	}

	/* Reports a file with lines that were not used. */
	if (problems)
		return EINVAL;

	/* Succeeded: every line was used. */
	return 0;
}

/*
 * Checks a value a keyword of mikuterm.conf may have, as the file's line
 * would be read.  Returns 0, or EINVAL with why set (an unknown keyword
 * among them).
 */
int
mikuterm_conf_value(
	const char *keyword,
	const char *value,
	const char **why)
{
	struct mikuterm_conf conf;
	unsigned found;
	int status;

	/* The keyword. */
	status = conf_keyword(keyword, &found);
	if (status != 0) {
		*why = "unknown keyword";
		return EINVAL;
	}

	/* The value, read into settings of their own. */
	mikuterm_conf_defaults(&conf);
	status = conf_value(&conf, found, value, why);
	if (status != 0)
		return EINVAL;

	/* Succeeded: the value may be written. */
	return 0;
}

/*
 * Writes a keyword's value in the settings as the file writes it (the
 * default's when the file does not give it).  Returns 0, EINVAL for an
 * unknown keyword, or ENAMETOOLONG.
 */
int
mikuterm_conf_text(
	const struct mikuterm_conf *conf,
	const char *keyword,
	char *value,
	size_t size)
{
	unsigned found;
	int written;
	int status;

	/* The keyword. */
	status = conf_keyword(keyword, &found);
	if (status != 0)
		return EINVAL;

	/* Its value as text. */
	written = 0;
	switch (found) {
	case CONF_FONT:
		written = snprintf(value, size, "%s", conf->font);
		break;
	case CONF_FALLBACK:
		written = snprintf(value, size, "%s", conf->fallback);
		break;
	case CONF_SIZE:
		written = snprintf(value, size, "%u", conf->font_size);
		break;
	case CONF_COLUMNS_KEY:
		written = snprintf(value, size, "%u", conf->columns);
		break;
	case CONF_ROWS_KEY:
		written = snprintf(value, size, "%u", conf->rows);
		break;
	case CONF_THEME:
		written = snprintf(value, size, "%s", mikuterm_theme_text(conf->theme));
		break;
	case CONF_AMBIGUOUS:
		written = snprintf(value, size, "%s", conf->ambiguous_wide ? "yes" : "no");
		break;
	case CONF_TERM_KEY:
		written = snprintf(value, size, "%s", conf->term);
		break;
	case CONF_CONNECT:
		written = snprintf(value, size, "%s", conf->connect);
		break;
	case CONF_COPY:
		written = snprintf(value, size, "%s", conf->copy_on_select ? "yes" : "no");
		break;
	case CONF_WSL:
		written = snprintf(value, size, "%u", conf->wsl_port);
		break;
	default:
		written = snprintf(value, size, "%u", conf->connect_timeout);
		break;
	}

	/* A value too long for the room. */
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded: the value's text. */
	return 0;
}

/*
 * Makes a settings file's text with a keyword's value changed, into memory
 * *out (*out_size bytes, a NUL after them) the caller frees: the first
 * line of the keyword becomes "Keyword value" (quoted when it holds a
 * space; its line break kept), or, without such a line, the line is added
 * at the end (after a line break when the text does not end with one).
 * An empty value removes the keyword's first line, so its default holds.
 * Every other line stays as it was.  Returns 0 or ENOMEM.
 */
int
mikuterm_conf_set(
	const char *text,
	size_t size,
	const char *keyword,
	const char *value,
	char **out,
	size_t *out_size)
{
	char copy[CONF_LINE_MAX];
	char name[CONF_KEYWORD_MAX];
	char line[CONF_LINE_MAX + CONF_KEYWORD_MAX + 4U];
	const char *found_value;
	const char *why;
	const char *start;
	const char *end;
	const char *next;
	const char *space;
	const char *tab;
	char *made;
	size_t line_length;
	size_t at;
	int differs;
	int status;
	int quoted;

	/* The new line, "Keyword value", quoted when the value holds a space or a tab. */
	space = strchr(value, ' ');
	tab = strchr(value, '\t');
	quoted = 0;
	if (space != NULL || tab != NULL)
		quoted = 1;
	if (quoted)
		(void)snprintf(line, sizeof(line), "%s \"%s\"", keyword, value);
	else
		(void)snprintf(line, sizeof(line), "%s %s", keyword, value);
	line_length = strlen(line);

	/* Room for the text, the new line and two line breaks. */
	made = malloc(size + line_length + 5U);
	if (made == NULL)
		return ENOMEM;

	/* The first line of the keyword. */
	start = text;
	end = text;
	while (start < text + size) {
		/* One line, its CR LF end set apart. */
		next = memchr(start, '\n', (size_t)(text + size - start));
		if (next == NULL)
			next = text + size;
		end = next;
		if (end > start && end[-1] == '\r')
			end--;
		status = conf_split(start, (size_t)(end - start), name, sizeof(name), copy, sizeof(copy), &found_value, &why);

		/* A line of the keyword ends the search. */
		if (status == 0 && name[0] != '\0') {
			differs = _stricmp(name, keyword);
			if (differs == 0)
				break;
		}

		/* The next line. */
		start = next;
		if (start < text + size)
			start++;
	}

	/* The text before it. */
	at = (size_t)(start - text);
	memcpy(made, text, at);

	/* The keyword's line in its place, or none for an empty value, or the line added at the end. */
	if (start < text + size) {
		if (value[0] != '\0') {
			memcpy(made + at, line, line_length);
			at += line_length;
			memcpy(made + at, end, (size_t)(text + size - end));
			at += (size_t)(text + size - end);
		} else {
			next = end;
			while (next < text + size && *next != '\n')
				next++;
			if (next < text + size)
				next++;
			memcpy(made + at, next, (size_t)(text + size - next));
			at += (size_t)(text + size - next);
		}
	} else if (value[0] != '\0') {
		if (at != 0U && made[at - 1U] != '\n') {
			made[at++] = '\r';
			made[at++] = '\n';
		}

		/* The new line, ended with CR LF. */
		memcpy(made + at, line, line_length);
		at += line_length;
		made[at++] = '\r';
		made[at++] = '\n';
	}

	/* Succeeded: the new text. */
	made[at] = '\0';
	*out = made;
	*out_size = at;
	return 0;
}

/* Reads a theme's name ("dark", "light", "contrast"); returns 0 or EINVAL. */
int
mikuterm_theme_name(
	const char *name,
	unsigned *theme)
{
	unsigned index;
	int differs;

	/* Each theme's name, in any case. */
	for (index = 0U; index < TERMINAL_THEMES; index++) {
		/* The theme named. */
		differs = _stricmp(name, conf_themes[index]);
		if (differs == 0) {
			*theme = index;
			return 0;
		}
	}

	/* No theme has the name. */
	return EINVAL;
}

/* Returns a theme's name, as the settings files write it. */
const char *
mikuterm_theme_text(
	unsigned theme)
{
	/* A theme out of the range is the default. */
	if (theme >= TERMINAL_THEMES)
		theme = TERMINAL_THEME_DARK;

	/* Reports the name. */
	return conf_themes[theme];
}

/*
 * Reads one line: nothing for a blank or a comment line, else a keyword
 * and its value.  Returns 0, or EINVAL with why set.
 */
static int
conf_line(
	struct mikuterm_conf *conf,
	const char *line,
	size_t length,
	int state,
	unsigned *given,
	const char **why)
{
	char text[CONF_LINE_MAX];
	char keyword[CONF_KEYWORD_MAX];
	const char *value;
	unsigned found;
	int status;

	/* The keyword and the value; a blank line and a comment say nothing. */
	status = conf_split(line, length, keyword, sizeof(keyword), text, sizeof(text), &value, why);
	if (status != 0)
		return status;
	if (keyword[0] == '\0')
		return 0;

	/* A keyword mikuterm does not know (state.conf's file passes over the user's keywords). */
	status = conf_keyword(keyword, &found);
	if (status != 0 ||
	    (state &&
	     !conf_names[found].state)) {
		*why = "unknown keyword";
		return EINVAL;
	}

	/* A keyword given before keeps its first value. */
	if ((*given & (1U << found)) != 0U) {
		*why = "the keyword was given before";
		return EINVAL;
	}

	/* A keyword needs a value. */
	if (value[0] == '\0') {
		*why = "the keyword has no value";
		return EINVAL;
	}

	/* The value. */
	status = conf_value(conf, found, value, why);
	if (status != 0)
		return status;

	/* Succeeded: the keyword is given. */
	*given |= 1U << found;
	return 0;
}

/*
 * Splits a line into its keyword (empty for a blank or a comment line,
 * cut to keyword_size - 1 bytes; a longer one is unknown) and its value,
 * which *value points at inside text (the line copied, its quotes or
 * trailing spaces taken off).  Returns 0, or EINVAL with why set.
 */
static int
conf_split(
	const char *line,
	size_t length,
	char *keyword,
	size_t keyword_size,
	char *text,
	size_t text_size,
	const char **value,
	const char **why)
{
	char *start;
	char *end;
	size_t index;
	size_t keyword_length;

	/* The line as a string; too long a line is refused. */
	keyword[0] = '\0';
	*value = "";
	if (length >= text_size) {
		*why = "the line is too long";
		return EINVAL;
	}

	/* A copy to cut into words. */
	memcpy(text, line, length);
	text[length] = '\0';

	/* Spaces before the keyword; a blank line and a comment say nothing. */
	index = 0U;
	while (text[index] == ' ' || text[index] == '\t')
		index++;
	if (text[index] == '\0' || text[index] == '#')
		return 0;

	/* The keyword: up to a space, a tab or '='. */
	keyword_length = 0U;
	while (text[index] != '\0' &&
	       text[index] != ' ' &&
	       text[index] != '\t' &&
	       text[index] != '=') {
		if (keyword_length + 1U >= keyword_size) {
			*why = "unknown keyword";
			return EINVAL;
		}

		/* One more of the keyword's characters. */
		keyword[keyword_length] = text[index];
		keyword_length++;
		index++;
	}

	/* The keyword ends as a C string. */
	keyword[keyword_length] = '\0';

	/* The spaces and the one '=' between the keyword and the value. */
	while (text[index] == ' ' || text[index] == '\t')
		index++;
	if (text[index] == '=')
		index++;
	while (text[index] == ' ' || text[index] == '\t')
		index++;
	start = text + index;

	/* A value in quotes ends at the closing quote; another at the line's end, without its trailing spaces. */
	if (start[0] == '"') {
		start++;
		end = strchr(start, '"');
		if (end == NULL) {
			*why = "a quote is not closed";
			return EINVAL;
		}

		/* The value ends at the closing quote. */
		*end = '\0';
	} else {
		end = start + strlen(start);
		while (end > start &&
		       (end[-1] == ' ' ||
		        end[-1] == '\t'))
			end--;
		*end = '\0';
	}

	/* Succeeded: the keyword and its value. */
	*value = start;
	return 0;
}

/* Finds a keyword of mikuterm.conf by its name, in any case; returns 0, or EINVAL for none. */
static int
conf_keyword(
	const char *keyword,
	unsigned *found)
{
	unsigned index;
	int differs;

	/* Each keyword's own entry. */
	for (index = 0U; index < CONF_KEYWORDS; index++) {
		differs = _stricmp(keyword, conf_names[index].name);
		if (differs == 0) {
			*found = index;
			return 0;
		}
	}

	/* No keyword has the name. */
	return EINVAL;
}

/* Takes a keyword's value into the settings; returns 0, or EINVAL with why set. */
static int
conf_value(
	struct mikuterm_conf *conf,
	unsigned keyword,
	const char *value,
	const char **why)
{
	unsigned number;
	int yes;
	int status;

	/* What the keyword sets. */
	status = 0;
	switch (keyword) {
	case CONF_FONT:
		status = conf_copy(conf->font, sizeof(conf->font), value);
		*why = "the path is too long";
		break;
	case CONF_FALLBACK:
		status = conf_copy(conf->fallback, sizeof(conf->fallback), value);
		*why = "the path is too long";
		break;
	case CONF_SIZE:
		status = conf_number(value, TERMINAL_PIXELS_MIN, TERMINAL_PIXELS_MAX, &number);
		*why = "FontSize is a number from 8 to 32";
		if (status == 0) {
			conf->font_size = number;
			conf->font_size_given = 1;
		}

		break;
	case CONF_COLUMNS_KEY:
		status = conf_number(value, 1U, TERMINAL_MAX_COLUMNS, &number);
		*why = "Columns is a number from 1 to 240";
		if (status == 0)
			conf->columns = number;
		break;
	case CONF_ROWS_KEY:
		status = conf_number(value, 1U, TERMINAL_MAX_ROWS, &number);
		*why = "Rows is a number from 1 to 100";
		if (status == 0)
			conf->rows = number;
		break;
	case CONF_THEME:
		status = mikuterm_theme_name(value, &number);
		*why = "Theme is dark, light or contrast";
		if (status == 0) {
			conf->theme = number;
			conf->theme_given = 1;
		}

		break;
	case CONF_AMBIGUOUS:
		status = conf_yes(value, &yes);
		*why = "AmbiguousWide is yes or no";
		if (status == 0) {
			conf->ambiguous_wide = yes;
			conf->ambiguous_given = 1;
		}

		break;
	case CONF_TERM_KEY:
		status = conf_copy(conf->term, sizeof(conf->term), value);
		*why = "the TERM name is too long";
		break;
	case CONF_CONNECT:
		status = conf_copy(conf->connect, sizeof(conf->connect), value);
		*why = "the host is too long";
		break;
	case CONF_COPY:
		status = conf_yes(value, &yes);
		*why = "CopyOnSelect is yes or no";
		if (status == 0) {
			conf->copy_on_select = yes;
			conf->copy_given = 1;
		}

		break;
	case CONF_WSL:
		status = conf_number(value, 1U, 65535U, &number);
		*why = "WslPort is a port from 1 to 65535";
		if (status == 0)
			conf->wsl_port = number;
		break;
	default:
		status = conf_number(value, 0U, CONF_CONNECT_TIMEOUT_MAX, &number);
		*why = "ConnectTimeout is a number of seconds from 0 to 3600";
		if (status == 0)
			conf->connect_timeout = number;
		break;
	}

	/* Reports a value that cannot be used. */
	if (status != 0)
		return EINVAL;

	/* Succeeded: the value is taken. */
	*why = NULL;
	return 0;
}

/* Reads a decimal number within a range; returns 0 or EINVAL. */
static int
conf_number(
	const char *text,
	unsigned minimum,
	unsigned maximum,
	unsigned *number)
{
	unsigned long value;
	size_t index;

	/* Digits alone, no larger than the maximum as they come. */
	value = 0UL;
	for (index = 0U; text[index] != '\0'; index++) {
		/* A digit, and the value so far within the range. */
		if (text[index] < '0' || text[index] > '9')
			return EINVAL;
		value = value * 10UL + (unsigned long)(text[index] - '0');
		if (value > maximum)
			return EINVAL;
	}

	/* At least one digit, and not below the minimum. */
	if (index == 0U || value < minimum)
		return EINVAL;

	/* Succeeded: the number. */
	*number = (unsigned)value;
	return 0;
}

/* Reads yes or no (in any case); returns 0 or EINVAL. */
static int
conf_yes(
	const char *text,
	int *yes)
{
	int differs;

	/* Yes. */
	differs = _stricmp(text, "yes");
	if (differs == 0) {
		*yes = 1;
		return 0;
	}

	/* No. */
	differs = _stricmp(text, "no");
	if (differs == 0) {
		*yes = 0;
		return 0;
	}

	/* Anything else. */
	return EINVAL;
}

/* Copies a string into a field; returns 0, or ENAMETOOLONG (the field then unchanged). */
static int
conf_copy(
	char *to,
	size_t size,
	const char *text)
{
	size_t length;

	/* The string and its NUL must fit. */
	length = strlen(text);
	if (length + 1U > size)
		return ENAMETOOLONG;

	/* Succeeded: the copy. */
	memcpy(to, text, length + 1U);
	return 0;
}

/* Adds a problem line ("config/mikuterm.conf line N: why", the name as mikuterm shows paths) to what the first tab shows; a full list keeps its lines. */
static void
conf_problem(
	struct mikuterm_conf *conf,
	unsigned line,
	const char *why)
{
	char text[160];
	size_t have;
	size_t length;
	int written;

	/* The line. */
	written = snprintf(text, sizeof(text), "config/mikuterm.conf line %u: %s\r\n", line, why);
	if (written < 0)
		return;
	length = strlen(text);

	/* After the problems before it, while there is room. */
	have = strlen(conf->problems);
	if (have + length + 1U > sizeof(conf->problems))
		return;
	memcpy(conf->problems + have, text, length + 1U);
}
