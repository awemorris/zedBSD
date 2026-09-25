/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The termcap interface and tparm(), over the curses library's terminfo.
 *
 * A termcap code is looked up in the table below and answered from the
 * terminfo description setupterm() loaded, so a program written for termcap
 * sees the same terminal as one written for terminfo.  A code the table does
 * not know is reported absent.  Padding is not needed on the terminals the
 * system drives, so tputs() leaves out the $<...> delays.
 */

#include <curses.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <term.h>
#include <terminfo.h>

/* One termcap code and the terminfo capability it names. */
struct termcap_name {
	char code[3];
	const char *name;
};

static const char *terminfo_name(const char *code);
static char *expand(const char *text, const long parameters[9]);

char PC;
char *UP;
char *BC;
short ospeed;

/*
 * The codes of the capabilities programs ask for, from the termcap names
 * the terminfo(5) tables give for each.
 */
static const struct termcap_name names[] = {
	/* Booleans. */
	{ "am", "am" }, { "bw", "bw" }, { "xn", "xenl" }, { "km", "km" },
	{ "mi", "mir" }, { "ms", "msgr" }, { "ut", "bce" }, { "hs", "hs" },
	{ "ND", "ndscr" }, { "xo", "xon" }, { "5i", "mc5i" }, { "cc", "ccc" },
	/* Numbers. */
	{ "co", "cols" }, { "li", "lines" }, { "Co", "colors" },
	{ "pa", "pairs" }, { "it", "it" }, { "NC", "ncv" }, { "ws", "wsl" },
	/* Cursor motion and scrolling. */
	{ "cm", "cup" }, { "ho", "home" }, { "up", "cuu1" }, { "do", "cud1" },
	{ "le", "cub1" }, { "nd", "cuf1" }, { "UP", "cuu" }, { "DO", "cud" },
	{ "LE", "cub" }, { "RI", "cuf" }, { "ch", "hpa" }, { "cv", "vpa" },
	{ "cs", "csr" }, { "sf", "ind" }, { "sr", "ri" }, { "SF", "indn" },
	{ "SR", "rin" }, { "cr", "cr" }, { "nw", "nel" }, { "ta", "ht" },
	{ "bt", "cbt" }, { "sc", "sc" }, { "rc", "rc" }, { "ll", "ll" },
	/* Clearing, inserting and deleting. */
	{ "cl", "clear" }, { "cd", "ed" }, { "ce", "el" }, { "cb", "el1" },
	{ "al", "il1" }, { "dl", "dl1" }, { "AL", "il" }, { "DL", "dl" },
	{ "dc", "dch1" }, { "DC", "dch" }, { "ic", "ich1" }, { "IC", "ich" },
	{ "im", "smir" }, { "ei", "rmir" }, { "ec", "ech" },
	/* Modes. */
	{ "so", "smso" }, { "se", "rmso" }, { "us", "smul" }, { "ue", "rmul" },
	{ "md", "bold" }, { "mb", "blink" }, { "mh", "dim" }, { "mr", "rev" },
	{ "me", "sgr0" }, { "ZH", "sitm" }, { "ZR", "ritm" }, { "mk", "invis" },
	{ "as", "smacs" }, { "ae", "rmacs" }, { "eA", "enacs" },
	{ "ti", "smcup" }, { "te", "rmcup" }, { "ks", "smkx" }, { "ke", "rmkx" },
	{ "vi", "civis" }, { "ve", "cnorm" }, { "vs", "cvvis" },
	{ "bl", "bel" }, { "vb", "flash" }, { "is", "is2" }, { "rs", "rs2" },
	{ "ts", "tsl" }, { "fs", "fsl" }, { "ds", "dsl" },
	/* Colour. */
	{ "AF", "setaf" }, { "AB", "setab" }, { "Sf", "setf" }, { "Sb", "setb" },
	{ "op", "op" }, { "oc", "oc" },
	/* Keys. */
	{ "ku", "kcuu1" }, { "kd", "kcud1" }, { "kl", "kcub1" }, { "kr", "kcuf1" },
	{ "kh", "khome" }, { "@7", "kend" }, { "kI", "kich1" }, { "kD", "kdch1" },
	{ "kP", "kpp" }, { "kN", "knp" }, { "kb", "kbs" }, { "kB", "kcbt" },
	{ "k1", "kf1" }, { "k2", "kf2" }, { "k3", "kf3" }, { "k4", "kf4" },
	{ "k5", "kf5" }, { "k6", "kf6" }, { "k7", "kf7" }, { "k8", "kf8" },
	{ "k9", "kf9" }, { "k;", "kf10" }, { "F1", "kf11" }, { "F2", "kf12" },
	{ "Km", "kmous" }, { "%i", "kRIT" }, { "#4", "kLFT" },
	{ "pc", "pad" }, { "bc", "kbs" },
};

/*
 * Finds the terminfo name of a termcap code, or NULL.
 */
static const char *
terminfo_name(
	const char *code)
{
	size_t i;

	/* A code is two characters; anything else is not one. */
	if (code == NULL || code[0] == '\0' || code[1] == '\0')
		return NULL;
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (names[i].code[0] == code[0] && names[i].code[1] == code[1])
			return names[i].name;
	}
	return NULL;
}

/*
 * Expands a parameterised string into a buffer that lasts until the next
 * call, as tparm() and tgoto() are specified to return.
 */
static char *
expand(
	const char *text,
	const long parameters[9])
{
	static char buffer[1024];

	/* Refuses what it cannot expand rather than returning half of it. */
	if (text == NULL ||
	    terminfo_expand(text, parameters, buffer, sizeof(buffer)) < 0)
		return NULL;
	return buffer;
}

/*
 * Loads the description of a terminal type.
 *
 * Returns 1 when it is found, 0 when it is not.  The buffer termcap wanted
 * is not used; the description stays in the library.
 */
int
tgetent(
	char *buffer,
	const char *name)
{
	int result;
	char *string;

	(void)buffer;

	/* Loads through terminfo. */
	if (setupterm(name, 1, &result) != OK)
		return result < 0 ? -1 : 0;

	/* Fills in the globals termcap programs read. */
	string = tigetstr("pad");
	PC = string != NULL && string != (char *)-1 ? string[0] : '\0';
	string = tigetstr("cuu1");
	UP = string != NULL && string != (char *)-1 ? string : NULL;
	string = tigetstr("cub1");
	BC = string != NULL && string != (char *)-1 ? string : NULL;
	return 1;
}

/*
 * Reports a boolean capability: 1 when the terminal has it, else 0.
 */
int
tgetflag(
	const char *code)
{
	const char *name = terminfo_name(code);

	/* An unknown code is a capability the terminal lacks. */
	if (name == NULL)
		return 0;
	return tigetflag(name) == 1;
}

/*
 * Reports a numeric capability, or -1 when the terminal has none.
 */
int
tgetnum(
	const char *code)
{
	const char *name = terminfo_name(code);
	int value;

	/* An unknown code is a capability the terminal lacks. */
	if (name == NULL)
		return -1;
	value = tigetnum(name);
	return value < 0 ? -1 : value;
}

/*
 * Reports a string capability, or NULL when the terminal has none.
 *
 * With an area, the string is copied there and *area is moved past it, as
 * termcap did; without one, the library's own copy is returned.
 */
char *
tgetstr(
	const char *code,
	char **area)
{
	const char *name = terminfo_name(code);
	char *string;
	size_t length;

	/* An unknown code is a capability the terminal lacks. */
	if (name == NULL)
		return NULL;
	string = tigetstr(name);
	if (string == NULL || string == (char *)-1)
		return NULL;

	/* Copies into the caller's area when there is one. */
	if (area == NULL || *area == NULL)
		return string;
	length = strlen(string) + 1U;
	memcpy(*area, string, length);
	string = *area;
	*area += length;
	return string;
}

/*
 * Expands a cursor motion to a column and a row.
 *
 * termcap passes the column first; a terminfo cup string takes the row
 * first, so the two are swapped on the way in.
 */
char *
tgoto(
	const char *text,
	int column,
	int row)
{
	long parameters[9] = { row, column };
	char *result;

	/* termcap reports a string it cannot use as "OOPS". */
	result = expand(text, parameters);
	return result != NULL ? result : (char *)"OOPS";
}

/*
 * Expands a parameterised capability with up to nine arguments.
 */
char *
tparm(
	const char *text,
	...)
{
	long parameters[9];
	va_list arguments;
	size_t i;

	/* Takes nine arguments whether or not the string uses them all. */
	va_start(arguments, text);
	for (i = 0; i < 9; i++)
		parameters[i] = va_arg(arguments, long);
	va_end(arguments);
	return expand(text, parameters);
}

/*
 * Expands a parameterised capability whose arguments are ints.
 *
 * The string names how many it uses by the highest %pN, so no more than
 * that are read from the argument list.
 */
char *
tiparm(
	const char *text,
	...)
{
	long parameters[9] = { 0 };
	va_list arguments;
	const char *p;
	int count = 0;
	int i;

	/* Counts the arguments the string refers to. */
	if (text == NULL)
		return NULL;
	for (p = text; (p = strstr(p, "%p")) != NULL; p += 2) {
		if (p[2] >= '1' && p[2] <= '9' && p[2] - '0' > count)
			count = p[2] - '0';
	}

	/* Takes exactly those. */
	va_start(arguments, text);
	for (i = 0; i < count; i++)
		parameters[i] = va_arg(arguments, int);
	va_end(arguments);
	return expand(text, parameters);
}

/*
 * Writes a capability string through a character function.
 *
 * The count of lines affected only matters to padding, which is left out:
 * a $<...> delay is skipped rather than written.
 */
int
tputs(
	const char *text,
	int lines,
	int (*output)(int))
{
	const char *p;
	const char *end;

	(void)lines;

	/* Writes each character, stepping over delays. */
	if (text == NULL || output == NULL)
		return ERR;
	for (p = text; *p != '\0'; p++) {
		if (p[0] == '$' && p[1] == '<') {
			end = strchr(p, '>');
			if (end != NULL) {
				p = end;
				continue;
			}
		}
		if (output((unsigned char)*p) == EOF)
			return ERR;
	}
	return OK;
}
