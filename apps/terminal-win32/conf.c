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
 * Columns, Rows, Theme, AmbiguousWide, Term, Connect, CopyOnSelect and
 * ConnectTimeout.  A line mikuterm cannot use is skipped, the default
 * kept, and named in the problems the first tab shows ("line N: why").
 * config\state.conf is mikuterm's own (settings-file.c): FontSize, Theme
 * and AmbiguousWide as the View menu chose them; it reads nothing else.
 */

#include "mikuterm.h"

#include "userland/desktop/terminal/terminal.h"

#include <errno.h>
#include <stdio.h>
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
	CONF_KEYWORDS = 11
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
	{ "CopyOnSelect", 0 },
	{ "ConnectTimeout", 0 }
};

/* The themes' names, in the order of TERMINAL_THEME_*. */
static const char *const conf_themes[TERMINAL_THEMES] = {
	"dark",
	"light",
	"contrast"
};

static int conf_line(struct mikuterm_conf *conf, const char *line, size_t length, int state, unsigned *given, const char **why);
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

	/* The connections: xterm's TERM, the list of hosts first, a selection copied, 15 seconds to connect. */
	(void)conf_copy(conf->term, sizeof(conf->term), CONF_TERM);
	conf->connect[0] = '\0';
	conf->copy_on_select = 1;
	conf->connect_timeout = CONF_CONNECT_TIMEOUT;
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
	char *value;
	char *end;
	size_t index;
	size_t keyword_length;
	unsigned found;
	int differs;
	int status;

	/* The line as a string; too long a line is refused. */
	if (length >= sizeof(text)) {
		*why = "the line is too long";
		return EINVAL;
	}
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
	while (text[index] != '\0' && text[index] != ' ' && text[index] != '\t' && text[index] != '=') {
		if (keyword_length + 1U >= sizeof(keyword)) {
			*why = "unknown keyword";
			return EINVAL;
		}
		keyword[keyword_length] = text[index];
		keyword_length++;
		index++;
	}
	keyword[keyword_length] = '\0';

	/* The spaces and the one '=' between the keyword and the value. */
	while (text[index] == ' ' || text[index] == '\t')
		index++;
	if (text[index] == '=')
		index++;
	while (text[index] == ' ' || text[index] == '\t')
		index++;
	value = text + index;

	/* A value in quotes ends at the closing quote; another at the line's end, without its trailing spaces. */
	if (value[0] == '"') {
		value++;
		end = strchr(value, '"');
		if (end == NULL) {
			*why = "a quote is not closed";
			return EINVAL;
		}
		*end = '\0';
	} else {
		end = value + strlen(value);
		while (end > value && (end[-1] == ' ' || end[-1] == '\t'))
			end--;
		*end = '\0';
	}

	/* The keyword, in any case. */
	found = CONF_KEYWORDS;
	for (index = 0U; index < CONF_KEYWORDS; index++) {
		/* The keyword's own entry. */
		differs = _stricmp(keyword, conf_names[index].name);
		if (differs == 0) {
			found = (unsigned)index;
			break;
		}
	}

	/* A keyword mikuterm does not know (state.conf's file passes over the user's keywords). */
	if (found == CONF_KEYWORDS || (state && !conf_names[found].state)) {
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
		if (status == 0)
			conf->copy_on_select = yes;
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

/* Adds a problem line ("mikuterm.conf line N: why") to what the first tab shows; a full list keeps its lines. */
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
	written = snprintf(text, sizeof(text), "config\\mikuterm.conf line %u: %s\r\n", line, why);
	if (written < 0)
		return;
	length = strlen(text);

	/* After the problems before it, while there is room. */
	have = strlen(conf->problems);
	if (have + length + 1U > sizeof(conf->problems))
		return;
	memcpy(conf->problems + have, text, length + 1U);
}
