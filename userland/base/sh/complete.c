/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Tab completion for an interactive shell.
 *
 * The line editor finds the word before the cursor and asks
 * complete_attempt() for what may take its place.  A word where a command
 * name stands is completed from the reserved words, the builtins, the
 * aliases, the functions, the executable files on PATH and the directories
 * here; any other word, and a command name with a slash, is completed as a
 * path.  Names are put in the line quoted with backslashes, and a list
 * shows them as they are named, a directory with a slash after it.
 */

#include "userland/base/sh/shell.h"
#include "userland/base/sh/alias.h"
#include "userland/base/sh/vars.h"

#include <dirent.h>
#include <pwd.h>
#include <readline/readline.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The characters a name put into the line has quoted with a backslash. */
#define COMPLETE_SPECIAL " \t\n\\'\"`$&|;<>()*?[]!#{}"

/* The operators that end a command, after which a command name stands again. */
#define COMPLETE_SEPARATORS ";&|()"

/* How many candidates a list first has room for. */
#define COMPLETE_INITIAL 32U

/*
 * One name a Tab found.
 *
 * text is what goes into the line (unquoted; a path keeps the directory
 * part as it was typed), shown is what a list of matches shows.  Both
 * belong to the candidate.
 */
struct complete_candidate {
	char *text;
	char *shown;
	int directory;
};

/*
 * The names one Tab found, in the order they were found.  One instance
 * lives for one call of complete_attempt().
 */
struct complete_list {
	struct complete_candidate *items;
	size_t count;
	size_t capacity;
};

/*
 * What ends the word to complete, going back from the cursor: blanks,
 * quotes and the characters of operators.  The line editor reads it through
 * rl_completer_word_break_characters for as long as the shell runs.
 */
static char complete_breaks[] = " \t\n\"'`<>=;|&()";

/*
 * The reserved words after which a command name stands again.  The table
 * is constant.
 */
static const char *const complete_command_words[] = {
	"!", "{", "do", "elif", "else", "if", "then", "time", "until", "while", NULL
};

static char **complete_attempt(const char *text, int start, int end);
static int complete_is_quoted(char *line, int index);
static int complete_in_quote(const char *line, int end);
static int complete_command_position(const char *line, int end);
static void complete_word_ended(const char *line, int start, int end, int *command, int *redirect);
static int complete_keeps_command(const char *word);
static char *complete_unquote(const char *text);
static void complete_commands(struct complete_list *list, const char *prefix);
static void complete_add_name(struct complete_list *list, const char *prefix, const char *name);
static void complete_path_commands(struct complete_list *list, const char *prefix);
static void complete_paths(struct complete_list *list, const char *word, int commands_only);
static void complete_directory(struct complete_list *list, const char *typed, const char *directory, const char *prefix, int commands_only);
static char *complete_expand_tilde(const char *text);
static int complete_hidden(const char *name, const char *prefix);
static void complete_add(struct complete_list *list, char *text, char *shown, int directory);
static char **complete_result(struct complete_list *list);
static int complete_compare(const void *left, const void *right);
static char *complete_quote(const char *text, size_t length);
static void complete_list_free(struct complete_list *list);

/*
 * Lets the line editor complete words with Tab.
 */
void
sh_complete_init(
	void)
{
	/* The editor calls the shell for the matches, and asks it which characters are quoted. */
	rl_attempted_completion_function = complete_attempt;
	rl_completer_word_break_characters = complete_breaks;
	rl_char_is_quoted_p = complete_is_quoted;
}

/*
 * Finds what may complete the word from start to end of rl_line_buffer,
 * whose text is given: returns the array readline.h describes, or NULL.
 */
static char **
complete_attempt(
	const char *text,
	int start,
	int end)
{
	struct complete_list list;
	char **matches;
	char *word;
	const char *found;
	int quoted;
	int command;
	int slash;

	/* The word ends at the cursor, where text already ends. */
	(void)end;

	/* A word inside an open quote is not completed. */
	quoted = complete_in_quote(rl_line_buffer, start);
	if (quoted)
		return NULL;

	/* The word as the shell will read it, and whether a command name stands there. */
	word = complete_unquote(text);
	command = complete_command_position(rl_line_buffer, start);
	found = strchr(word, '/');
	slash = 0;
	if (found != NULL)
		slash = 1;

	/* A command name without a slash is looked for as a command; anything else as a path. */
	memset(&list, 0, sizeof(list));
	if (command && !slash) {
		complete_commands(&list, word);
	} else {
		complete_paths(&list, word, command);
	}

	/* The candidates become the editor's array. */
	matches = complete_result(&list);
	complete_list_free(&list);
	free(word);

	/* Succeeded: the matches, or NULL for none. */
	return matches;
}

/* Tells the editor whether the character at an index is quoted by backslashes before it. */
static int
complete_is_quoted(
	char *line,
	int index)
{
	int count;

	/* Counts the backslashes just before the character. */
	count = 0;
	while (index - count > 0 && line[index - count - 1] == '\\')
		count++;

	/* An odd number quotes it. */
	if (count % 2 == 1)
		return 1;

	/* An even number quote each other. */
	return 0;
}

/* Reports whether a quote opened before end is still open there. */
static int
complete_in_quote(
	const char *line,
	int end)
{
	int index;
	int single;
	int double_quote;

	/* Follows the quotes from the start of the line. */
	single = 0;
	double_quote = 0;
	for (index = 0; index < end; index++) {
		/* Inside single quotes only a single quote means anything. */
		if (single) {
			if (line[index] == '\'')
				single = 0;
			continue;
		}

		/* A backslash quotes the next character, outside single quotes. */
		if (line[index] == '\\') {
			index++;
			continue;
		}

		/* A quote opens or closes. */
		if (line[index] == '"') {
			double_quote = !double_quote;
		} else if (line[index] == '\'' && !double_quote) {
			single = 1;
		}
	}

	/* An open quote of either kind. */
	if (single)
		return 1;
	if (double_quote)
		return 1;

	/* Every quote was closed. */
	return 0;
}

/*
 * Reports whether a command name stands at end: at the start of the line,
 * after an operator that ends a command, after a reserved word that starts
 * one, or after assignments.  A word that goes on before end (after an =)
 * and a redirection's file are not command names.
 */
static int
complete_command_position(
	const char *line,
	int end)
{
	int index;
	int word_start;
	int command;
	int redirect;
	int single;
	int double_quote;
	const char *separator;
	char character;

	/* The line starts where a command name stands, with no word open. */
	command = 1;
	redirect = 0;
	word_start = -1;
	single = 0;
	double_quote = 0;

	/* Reads the words and operators before end. */
	for (index = 0; index < end; index++) {
		character = line[index];

		/* Quoted text is part of the word it is in: single quotes end only at a single quote. */
		if (single) {
			if (character == '\'')
				single = 0;
			continue;
		}

		/* In double quotes a backslash still quotes the next character. */
		if (double_quote) {
			if (character == '\\')
				index++;
			else if (character == '"')
				double_quote = 0;
			continue;
		}

		/* Blanks end a word. */
		if (character == ' ' || character == '\t' || character == '\n') {
			complete_word_ended(line, word_start, index, &command, &redirect);
			word_start = -1;
			continue;
		}

		/* An operator that ends a command puts a command name next. */
		separator = strchr(COMPLETE_SEPARATORS, character);
		if (separator != NULL) {
			complete_word_ended(line, word_start, index, &command, &redirect);
			word_start = -1;
			command = 1;
			redirect = 0;
			continue;
		}

		/* A redirection's operator (<, >, >>, >&, <&) puts its file next. */
		if (character == '<' || character == '>') {
			complete_word_ended(line, word_start, index, &command, &redirect);
			word_start = -1;
			redirect = 1;
			if (line[index + 1] == '&' || line[index + 1] == character)
				index++;
			continue;
		}

		/* Anything else starts or continues a word. */
		if (word_start < 0)
			word_start = index;

		/* A quote or a backslash quotes what follows within the word. */
		if (character == '\\') {
			index++;
		} else if (character == '\'') {
			single = 1;
		} else if (character == '"') {
			double_quote = 1;
		}
	}

	/* A word that runs on to end (after an = or a quote) is no command name. */
	if (word_start >= 0)
		return 0;

	/* A redirection's file is not one either. */
	if (redirect)
		return 0;

	/* Succeeded: whether a command name stands there. */
	return command;
}

/*
 * Takes note of a word that ended: a redirection's file, an assignment or
 * a reserved word leave a command name to come, any other word is the
 * command name and what follows are its arguments.
 */
static void
complete_word_ended(
	const char *line,
	int start,
	int end,
	int *command,
	int *redirect)
{
	char *word;
	size_t length;
	int keeps;

	/* No word was open. */
	if (start < 0)
		return;

	/* A redirection takes the word as its file. */
	if (*redirect) {
		*redirect = 0;
		return;
	}

	/* Words after the command name change nothing. */
	if (!*command)
		return;

	/* An assignment keeps a command name to come. */
	word = sh_strndup(line + start, (size_t)(end - start));
	length = sh_var_name_length(word);
	if (length > 0 && word[length] == '=') {
		free(word);
		return;
	}

	/* So does a reserved word that starts a command; any other word is the command name. */
	keeps = complete_keeps_command(word);
	free(word);
	if (!keeps)
		*command = 0;
}

/* Reports whether a word is a reserved word after which a command name stands. */
static int
complete_keeps_command(
	const char *word)
{
	int index;
	int compare;

	/* Each word of the table. */
	for (index = 0; complete_command_words[index] != NULL; index++) {
		compare = strcmp(word, complete_command_words[index]);
		if (compare == 0)
			return 1;
	}

	/* Not one of them. */
	return 0;
}

/* Returns the word with its backslashes taken out, in allocated memory. */
static char *
complete_unquote(
	const char *text)
{
	char *word;
	size_t from;
	size_t to;

	/* Each character, the one after a backslash as it is. */
	word = sh_malloc(strlen(text) + 1U);
	to = 0;
	for (from = 0; text[from] != '\0'; from++) {
		if (text[from] == '\\' && text[from + 1] != '\0')
			from++;
		word[to++] = text[from];
	}

	/* Succeeded: the word, which the caller frees. */
	word[to] = '\0';
	return word;
}

/*
 * Finds the command names that start with a prefix: reserved words,
 * builtins, aliases, functions, executable files on PATH, and the
 * directories here (with a slash, to go on into).
 */
static void
complete_commands(
	struct complete_list *list,
	const char *prefix)
{
	const char *name;
	int index;

	/* The reserved words. */
	for (index = 0;; index++) {
		name = sh_reserved_word(index);
		if (name == NULL)
			break;
		complete_add_name(list, prefix, name);
	}

	/* The builtins. */
	for (index = 0;; index++) {
		name = sh_builtin_name(index);
		if (name == NULL)
			break;
		complete_add_name(list, prefix, name);
	}

	/* The aliases. */
	for (index = 0;; index++) {
		name = sh_alias_name(index);
		if (name == NULL)
			break;
		complete_add_name(list, prefix, name);
	}

	/* The functions. */
	for (index = 0;; index++) {
		name = sh_function_name(index);
		if (name == NULL)
			break;
		complete_add_name(list, prefix, name);
	}

	/* The executable files in the directories of PATH. */
	complete_path_commands(list, prefix);

	/* The directories here, which a command name may go on into. */
	complete_directory(list, "", ".", prefix, 2);
}

/* Adds a name that starts with a prefix, as it is. */
static void
complete_add_name(
	struct complete_list *list,
	const char *prefix,
	const char *name)
{
	int compare;

	/* Only a name that starts with the prefix. */
	compare = strncmp(name, prefix, strlen(prefix));
	if (compare != 0)
		return;

	/* The name goes into the line and into a list the same. */
	complete_add(list, sh_strdup(name), sh_strdup(name), 0);
}

/* Adds the executable files in the directories of PATH that start with a prefix. */
static void
complete_path_commands(
	struct complete_list *list,
	const char *prefix)
{
	const char *path;
	const char *colon;
	char *directory;
	size_t length;

	/* Without PATH no file is found. */
	path = sh_var_get("PATH");
	if (path == NULL)
		return;

	/* Each directory, an empty one meaning here. */
	for (;;) {
		colon = strchr(path, ':');
		if (colon == NULL)
			length = strlen(path);
		else
			length = (size_t)(colon - path);
		if (length == 0)
			directory = sh_strdup(".");
		else
			directory = sh_strndup(path, length);

		/* Its executable files, by name alone. */
		complete_directory(list, NULL, directory, prefix, 1);
		free(directory);

		/* The last directory ends the walk. */
		if (colon == NULL)
			break;
		path = colon + 1;
	}
}

/*
 * Finds the paths that complete a word: the entries of the directory the
 * word names that start with its last part.  commands_only keeps only the
 * directories and the executable files.
 */
static void
complete_paths(
	struct complete_list *list,
	const char *word,
	int commands_only)
{
	const char *slash;
	const char *prefix;
	char *typed;
	char *directory;

	/* The directory as it was typed (up to the last slash), and the part after it. */
	slash = strrchr(word, '/');
	if (slash == NULL) {
		typed = sh_strdup("");
		prefix = word;
	} else {
		typed = sh_strndup(word, (size_t)(slash - word + 1));
		prefix = slash + 1;
	}

	/* The directory to read: here, or the typed one with its tilde expanded. */
	if (typed[0] == '\0')
		directory = sh_strdup(".");
	else
		directory = complete_expand_tilde(typed);

	/* Its entries that start with the prefix. */
	complete_directory(list, typed, directory, prefix, commands_only);
	free(directory);
	free(typed);
}

/*
 * Adds the entries of a directory that start with a prefix.  typed goes
 * before each name in the line (NULL for a command found on PATH, which is
 * named alone).  commands_only is 0 for every entry, 1 for directories and
 * executable files (only executable files when typed is NULL), and 2 for
 * directories alone.
 */
static void
complete_directory(
	struct complete_list *list,
	const char *typed,
	const char *directory,
	const char *prefix,
	int commands_only)
{
	struct dirent *entry;
	struct stat status;
	DIR *stream;
	char *path;
	char *text;
	char *shown;
	size_t length;
	size_t prefix_length;
	int compare;
	int hidden;
	int found;
	int is_directory;
	int executable;
	int runnable;
	mode_t kind;

	/* A directory that cannot be read has no entries to offer. */
	stream = opendir(directory);
	if (stream == NULL)
		return;

	/* Each entry of it. */
	prefix_length = strlen(prefix);
	for (;;) {
		entry = readdir(stream);
		if (entry == NULL)
			break;

		/* Only a name that starts with the prefix, and a dot file only when asked for. */
		compare = strncmp(entry->d_name, prefix, prefix_length);
		if (compare != 0)
			continue;
		hidden = complete_hidden(entry->d_name, prefix);
		if (hidden)
			continue;

		/* What the entry is, following a symbolic link. */
		length = strlen(directory) + strlen(entry->d_name) + 2U;
		path = sh_malloc(length);
		snprintf(path, length, "%s/%s", directory, entry->d_name);
		found = stat(path, &status);
		kind = 0;
		if (found == 0)
			kind = status.st_mode & S_IFMT;
		is_directory = 0;
		if (kind == S_IFDIR)
			is_directory = 1;

		/* A regular file the shell may run is executable. */
		executable = 0;
		if (kind == S_IFREG) {
			runnable = access(path, X_OK);
			if (runnable == 0)
				executable = 1;
		}

		/* The path was needed only to look at the entry. */
		free(path);

		/* A command on PATH is an executable file. */
		if (typed == NULL) {
			if (!executable)
				continue;
			complete_add(list, sh_strdup(entry->d_name), sh_strdup(entry->d_name), 0);
			continue;
		}

		/* A command name with a slash is a directory or an executable file. */
		if (commands_only == 1 && !is_directory && !executable)
			continue;

		/* The directories here, for a command name without a slash. */
		if (commands_only == 2 && !is_directory)
			continue;

		/* The typed directory and the name, and a slash after a directory. */
		length = strlen(typed) + strlen(entry->d_name) + 2U;
		text = sh_malloc(length);
		shown = sh_malloc(length);
		if (is_directory) {
			snprintf(text, length, "%s%s/", typed, entry->d_name);
			snprintf(shown, length, "%s/", entry->d_name);
		} else {
			snprintf(text, length, "%s%s", typed, entry->d_name);
			snprintf(shown, length, "%s", entry->d_name);
		}

		/* The candidate takes both strings. */
		complete_add(list, text, shown, is_directory);
	}

	/* The directory is read. */
	closedir(stream);
}

/*
 * Returns a typed directory with a leading ~ or ~user expanded, in
 * allocated memory; a user that is not known leaves it as it is.
 */
static char *
complete_expand_tilde(
	const char *text)
{
	const struct passwd *account;
	const char *slash;
	const char *home;
	char *user;
	char *expanded;
	size_t length;

	/* Only a leading tilde is expanded. */
	if (text[0] != '~')
		return sh_strdup(text);

	/* The user runs to the first slash; none is the shell's HOME. */
	slash = strchr(text, '/');
	if (slash == NULL)
		slash = text + strlen(text);
	if (slash == text + 1) {
		home = sh_var_get("HOME");
	} else {
		user = sh_strndup(text + 1, (size_t)(slash - text - 1));
		account = getpwnam(user);
		free(user);
		home = NULL;
		if (account != NULL)
			home = account->pw_dir;
	}

	/* Without a home the text stays as it is. */
	if (home == NULL)
		return sh_strdup(text);

	/* The home directory, then the rest of the text. */
	length = strlen(home) + strlen(slash) + 1U;
	expanded = sh_malloc(length);
	snprintf(expanded, length, "%s%s", home, slash);

	/* Succeeded: the expanded directory, which the caller frees. */
	return expanded;
}

/* Reports whether an entry is left out: . and .., and dot files unless the prefix starts with a dot. */
static int
complete_hidden(
	const char *name,
	const char *prefix)
{
	int compare;

	/* The directory itself and its parent are never offered. */
	compare = strcmp(name, ".");
	if (compare == 0)
		return 1;
	compare = strcmp(name, "..");
	if (compare == 0)
		return 1;

	/* A dot file only when the prefix asks for one. */
	if (name[0] == '.' && prefix[0] != '.')
		return 1;

	/* Offered. */
	return 0;
}

/* Adds a candidate, which takes the two strings. */
static void
complete_add(
	struct complete_list *list,
	char *text,
	char *shown,
	int directory)
{
	size_t capacity;

	/* Grows the array when it is full. */
	if (list->count == list->capacity) {
		capacity = COMPLETE_INITIAL;
		if (list->capacity > 0)
			capacity = list->capacity * 2U;
		list->items = sh_realloc(list->items, capacity * sizeof(*list->items));
		list->capacity = capacity;
	}

	/* The candidate is the newest. */
	list->items[list->count].text = text;
	list->items[list->count].shown = shown;
	list->items[list->count].directory = directory;
	list->count++;
}

/*
 * Makes the editor's array of the candidates: sorted, each name once, [0]
 * the one match or what all have in common (quoted), then what a list
 * shows.  Returns NULL when there is no candidate.
 */
static char **
complete_result(
	struct complete_list *list)
{
	char **matches;
	size_t unique;
	size_t index;
	size_t common;
	size_t position;
	int compare;

	/* Nothing found. */
	if (list->count == 0)
		return NULL;

	/* Sorted by the text, and a name found twice (a builtin and a file) kept once. */
	qsort(list->items, list->count, sizeof(*list->items), complete_compare);
	unique = 1;
	for (index = 1; index < list->count; index++) {
		compare = strcmp(list->items[index].text, list->items[unique - 1U].text);
		if (compare == 0) {
			free(list->items[index].text);
			free(list->items[index].shown);
			continue;
		}

		/* A new name moves down next to the last kept one. */
		list->items[unique++] = list->items[index];
	}

	/* Only the kept names count. */
	list->count = unique;

	/* One match goes in whole, followed by a blank unless it is a directory. */
	if (list->count == 1) {
		matches = sh_malloc(2U * sizeof(*matches));
		matches[0] = complete_quote(list->items[0].text, strlen(list->items[0].text));
		matches[1] = NULL;
		rl_completion_append_character = ' ';
		if (list->items[0].directory)
			rl_completion_append_character = '\0';
		return matches;
	}

	/* What every match starts with: the first one, cut where another differs. */
	common = strlen(list->items[0].text);
	for (index = 1; index < list->count; index++) {
		for (position = 0; position < common; position++) {
			if (list->items[index].text[position] != list->items[0].text[position])
				break;
		}

		/* The common part ends where this match differs. */
		common = position;
	}

	/* The common part, then what a list shows of each match. */
	matches = sh_malloc((list->count + 2U) * sizeof(*matches));
	matches[0] = complete_quote(list->items[0].text, common);
	for (index = 0; index < list->count; index++)
		matches[index + 1U] = sh_strdup(list->items[index].shown);
	matches[list->count + 1U] = NULL;

	/* Succeeded: the array, which the editor frees. */
	return matches;
}

/* Orders two candidates by their text. */
static int
complete_compare(
	const void *left,
	const void *right)
{
	const struct complete_candidate *first;
	const struct complete_candidate *second;
	int compare;

	/* The texts decide. */
	first = left;
	second = right;
	compare = strcmp(first->text, second->text);

	/* Succeeded: the order of the texts. */
	return compare;
}

/*
 * Returns the first length characters of a text with a backslash before
 * each character the shell would otherwise read specially, in allocated
 * memory.
 */
static char *
complete_quote(
	const char *text,
	size_t length)
{
	const char *special;
	char *quoted;
	size_t from;
	size_t to;

	/* Room for a backslash before every character, at worst. */
	quoted = sh_malloc(length * 2U + 1U);
	to = 0;
	for (from = 0; from < length; from++) {
		special = strchr(COMPLETE_SPECIAL, text[from]);
		if (special != NULL)
			quoted[to++] = '\\';
		quoted[to++] = text[from];
	}

	/* Succeeded: the quoted text, which the editor frees. */
	quoted[to] = '\0';
	return quoted;
}

/* Frees the candidates and their strings. */
static void
complete_list_free(
	struct complete_list *list)
{
	size_t index;

	/* Each candidate's strings, then the array. */
	for (index = 0; index < list->count; index++) {
		free(list->items[index].text);
		free(list->items[index].shown);
	}

	/* The list is empty again. */
	free(list->items);
	list->items = NULL;
	list->count = 0;
	list->capacity = 0;
}
