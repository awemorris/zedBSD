/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements expanding a pattern into the names that exist.
 *
 * The work is a walk, not a match: each part of the pattern that contains
 * no magic is appended without reading anything, and only a part that does
 * causes its directory to be read and each name tested.  A pattern of
 * several parts therefore costs one directory read per matching directory,
 * not one per name in the tree.
 *
 * Written from what the interface is defined to do, not from another
 * system's source.
 */

#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <glob.h>
#include <limits.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* What a run without limits may still not exceed. */
#define GLOB_PATH_LIMIT    65536U
#define GLOB_READDIR_LIMIT 262144U
#define GLOB_BRACE_LIMIT   4096U

/* The state one call carries through the walk. */
struct glob_state {
	glob_t *result;
	int flags;
	int (*report)(const char *, int);
	size_t produced;
	size_t read_count;
	int error;
};

static int glob_walk(struct glob_state *state, char *path, size_t length,
	const char *pattern);

/*
 * Supports the has magic operation.
 *
 * Reports whether a pattern holds anything that has to be matched, which
 * decides whether a directory must be read at all.
 */
static int
has_magic(
	const char *pattern,
	int flags)
{
	/* Process each element required by the operation. */
	for (; *pattern != '\0'; pattern++) {
		/* A quoted character stands for itself. */
		if (*pattern == '\\' && (flags & GLOB_NOESCAPE) == 0) {
			if (pattern[1] != '\0')
				pattern++;
			continue;
		}
		if (*pattern == '*' || *pattern == '?' || *pattern == '[')
			return 1;
	}

	/* Returns the computed result. */
	return 0;
}

/*
 * Supports the unquote operation.
 *
 * Copies a pattern part with its quoting removed, for the case where it
 * holds no magic and so names a file directly.
 */
static void
unquote(
	char *out,
	const char *in,
	size_t length,
	int flags)
{
	size_t index;
	size_t used;

	used = 0;

	/* Process each remaining element. */
	for (index = 0; index < length; index++) {
		if (in[index] == '\\' && (flags & GLOB_NOESCAPE) == 0 &&
		    index + 1U < length)
			index++;
		out[used++] = in[index];
	}
	out[used] = '\0';
}

/*
 * Supports the add path operation.
 *
 * Records one name, growing the vector as it goes.  The vector always ends
 * in a null, so a caller may walk it without consulting gl_pathc.
 */
static int
add_path(
	struct glob_state *state,
	const char *path,
	int is_directory)
{
	glob_t *result;
	char **grown;
	char *copy;
	size_t total;
	size_t length;

	result = state->result;

	/* Refuses a result that has grown past what was asked for. */
	if ((state->flags & GLOB_LIMIT) != 0 &&
	    state->produced >= GLOB_PATH_LIMIT) {
		state->error = GLOB_NOSPACE;
		return -1;
	}
	length = strlen(path);
	copy = malloc(length + 2U);

	/* Handles a failed malloc operation. */
	if (copy == NULL) {
		state->error = GLOB_NOSPACE;
		return -1;
	}
	memcpy(copy, path, length + 1U);

	/* A directory is marked as one when the caller asked to see it. */
	if (is_directory && (state->flags & GLOB_MARK) != 0 &&
	    (length == 0U || copy[length - 1U] != '/')) {
		copy[length] = '/';
		copy[length + 1U] = '\0';
	}

	/* Grows the vector by one, keeping room for the closing null. */
	total = result->gl_offs + result->gl_pathc + 2U;
	grown = realloc(result->gl_pathv, total * sizeof(*grown));

	/* Handles a failed realloc operation. */
	if (grown == NULL) {
		free(copy);
		state->error = GLOB_NOSPACE;
		return -1;
	}
	result->gl_pathv = grown;
	grown[result->gl_offs + result->gl_pathc] = copy;
	grown[result->gl_offs + result->gl_pathc + 1U] = NULL;
	result->gl_pathc++;
	result->gl_matchc++;
	state->produced++;

	/* Reports successful completion. */
	return 0;
}

/*
 * Supports the glob stat operation.
 *
 * Reports whether a path exists, and whether it is a directory, through
 * whichever pair of functions the caller chose.
 */
static int
glob_stat(
	struct glob_state *state,
	const char *path,
	int *is_directory)
{
	struct stat status;
	int error;

	/* An empty path is the current directory. */
	if (path[0] == '\0')
		path = ".";
	if ((state->flags & GLOB_ALTDIRFUNC) != 0 &&
	    state->result->gl_stat != NULL)
		error = state->result->gl_stat(path, &status);
	else
		error = stat(path, &status);

	/* Handles a path that is not there. */
	if (error != 0)
		return -1;

	/* Handles the is directory availability. */
	if (is_directory != NULL)
		*is_directory = S_ISDIR(status.st_mode);

	/* Reports successful completion. */
	return 0;
}

/* Supports the name compare operation. */
static int
name_compare(
	const void *first,
	const void *second)
{
	/* Returns the computed result. */
	return strcmp(*(char *const *)first, *(char *const *)second);
}

/*
 * Supports the glob directory operation.
 *
 * Reads one directory and follows every name the pattern part selects.
 * Names are taken in sorted order so that the whole result is ordered by
 * the names at each level, which is not the same as sorting the finished
 * paths: a slash sorts after a dot, so "a/b" would otherwise follow "a.c/b".
 */
static int
glob_directory(
	struct glob_state *state,
	char *path,
	size_t length,
	const char *component,
	size_t component_length,
	const char *rest)
{
	struct dirent *entry;
	char pattern[PATH_MAX];
	char **names;
	size_t capacity;
	size_t count;
	size_t index;
	void *directory;
	char **grown;
	char *copy;
	int match_flags;
	int outcome;

	/* Handles a pattern part too long to hold. */
	if (component_length >= sizeof(pattern)) {
		state->error = GLOB_NOSPACE;
		return -1;
	}
	memcpy(pattern, component, component_length);
	pattern[component_length] = '\0';

	/* Opens the directory through whichever reader the caller chose. */
	if ((state->flags & GLOB_ALTDIRFUNC) != 0 &&
	    state->result->gl_opendir != NULL)
		directory = state->result->gl_opendir(length == 0U ? "." : path);
	else
		directory = opendir(length == 0U ? "." : path);

	/* A directory that cannot be read is reported, or quietly skipped. */
	if (directory == NULL) {
		if (state->report != NULL &&
		    state->report(length == 0U ? "." : path, errno) != 0) {
			state->error = GLOB_ABORTED;
			return -1;
		}
		if ((state->flags & GLOB_ERR) != 0) {
			state->error = GLOB_ABORTED;
			return -1;
		}

		/* Reports successful completion. */
		return 0;
	}

	/* Collects the matching names before following any of them. */
	names = NULL;
	capacity = 0;
	count = 0;
	outcome = 0;
	match_flags = FNM_PERIOD;
	if ((state->flags & GLOB_NOESCAPE) != 0)
		match_flags |= FNM_NOESCAPE;
	for (;;) {
		if ((state->flags & GLOB_ALTDIRFUNC) != 0 &&
		    state->result->gl_readdir != NULL)
			entry = state->result->gl_readdir(directory);
		else
			entry = readdir(directory);
		if (entry == NULL)
			break;

		/* Refuses a directory that grows without bound. */
		state->read_count++;
		if ((state->flags & GLOB_LIMIT) != 0 &&
		    state->read_count > GLOB_READDIR_LIMIT) {
			state->error = GLOB_NOSPACE;
			outcome = -1;
			break;
		}

		/* Skips a name the pattern part does not select. */
		if (fnmatch(pattern, entry->d_name, match_flags) != 0)
			continue;

		/* Keeps the name, growing the array in steps. */
		if (count == capacity) {
			capacity = capacity == 0U ? 16U : capacity * 2U;
			grown = realloc(names, capacity * sizeof(*names));
			if (grown == NULL) {
				state->error = GLOB_NOSPACE;
				outcome = -1;
				break;
			}
			names = grown;
		}
		copy = strdup(entry->d_name);
		if (copy == NULL) {
			state->error = GLOB_NOSPACE;
			outcome = -1;
			break;
		}
		names[count++] = copy;
	}
	if ((state->flags & GLOB_ALTDIRFUNC) != 0 &&
	    state->result->gl_closedir != NULL)
		state->result->gl_closedir(directory);
	else
		(void)closedir(directory);

	/* Orders the names unless the caller asked for them as found. */
	if (outcome == 0 && (state->flags & GLOB_NOSORT) == 0 && count > 1U)
		qsort(names, count, sizeof(*names), name_compare);

	/* Process each remaining element. */
	for (index = 0; outcome == 0 && index < count; index++) {
		size_t name_length = strlen(names[index]);

		/* Skips a name that would not fit the path being built. */
		if (length + name_length + 2U >= PATH_MAX)
			continue;
		memcpy(path + length, names[index], name_length + 1U);
		outcome = glob_walk(state, path, length + name_length, rest);
	}

	/* Releases the names whether or not the walk finished. */
	for (index = 0; index < count; index++)
		free(names[index]);
	free(names);
	path[length] = '\0';

	/* Returns the computed result. */
	return outcome;
}

/*
 * Supports the glob walk operation.
 *
 * Takes the next part of the pattern and either appends it and carries on,
 * when it holds no magic, or reads the directory it names.
 */
static int
glob_walk(
	struct glob_state *state,
	char *path,
	size_t length,
	const char *pattern)
{
	const char *slash;
	size_t component_length;
	size_t added;
	int is_directory;
	int outcome;

	/* The end of the pattern: what has been built is a result. */
	if (*pattern == '\0') {
		is_directory = 0;

		/* A name is reported only because it is there. */
		if (glob_stat(state, path, &is_directory) != 0)
			return 0;

		/* Returns the computed result. */
		return add_path(state, path, is_directory);
	}

	/* A run of slashes belongs to the path, not to a name. */
	if (*pattern == '/') {
		while (*pattern == '/') {
			if (length + 2U >= PATH_MAX)
				return 0;
			path[length++] = '/';
			pattern++;
		}
		path[length] = '\0';

		/* Returns the computed result. */
		return glob_walk(state, path, length, pattern);
	}
	slash = strchr(pattern, '/');
	component_length = slash != NULL ?
			   (size_t)(slash - pattern) : strlen(pattern);

	/* A part with nothing to match names a file directly. */
	if (!has_magic(pattern, state->flags)) {
		if (length + component_length + 2U >= PATH_MAX)
			return 0;
		unquote(path + length, pattern, component_length, state->flags);
		added = strlen(path + length);
		outcome = glob_walk(state, path, length + added,
				    slash != NULL ? slash : pattern +
				    component_length);
		path[length] = '\0';

		/* Returns the computed result. */
		return outcome;
	}

	/* Returns the computed result. */
	return glob_directory(state, path, length, pattern, component_length,
			      slash != NULL ? slash :
			      pattern + component_length);
}

/*
 * Supports the expand tilde operation.
 *
 * Replaces a leading ~ or ~user with the home directory it names.  A name
 * that has no such user is left as it was written, because it may still be
 * the name of a file.
 */
static int
expand_tilde(
	const char *pattern,
	char *out,
	size_t size)
{
	struct passwd *entry;
	const char *home;
	const char *rest;
	char user[64];
	size_t length;

	/* Handles a pattern that names no home directory. */
	if (pattern[0] != '~') {
		if (strlen(pattern) >= size)
			return -1;
		(void)strcpy(out, pattern);
		return 0;
	}
	rest = strchr(pattern + 1, '/');
	length = rest != NULL ? (size_t)(rest - pattern - 1) :
		 strlen(pattern + 1);

	/* A bare ~ is the caller's own home directory. */
	if (length == 0U) {
		home = getenv("HOME");
		if (home == NULL) {
			entry = getpwuid(getuid());
			home = entry != NULL ? entry->pw_dir : NULL;
		}
	} else if (length < sizeof(user)) {
		memcpy(user, pattern + 1, length);
		user[length] = '\0';
		entry = getpwnam(user);
		home = entry != NULL ? entry->pw_dir : NULL;
	} else {
		home = NULL;
	}

	/* Leaves a name with no home directory exactly as written. */
	if (home == NULL) {
		if (strlen(pattern) >= size)
			return -1;
		(void)strcpy(out, pattern);
		return 0;
	}
	if (strlen(home) + (rest != NULL ? strlen(rest) : 0U) >= size)
		return -1;
	(void)strcpy(out, home);
	if (rest != NULL)
		(void)strcat(out, rest);

	/* Reports successful completion. */
	return 0;
}

/*
 * Supports the glob one operation.
 *
 * Expands one pattern, after any braces have already been taken apart.
 */
static int
glob_one(
	struct glob_state *state,
	const char *pattern)
{
	char expanded[PATH_MAX];
	char path[PATH_MAX];

	/* A leading ~ names a home directory when the caller asked. */
	if ((state->flags & GLOB_TILDE) != 0) {
		if (expand_tilde(pattern, expanded, sizeof(expanded)) != 0) {
			state->error = GLOB_NOSPACE;
			return -1;
		}
	} else {
		if (strlen(pattern) >= sizeof(expanded)) {
			state->error = GLOB_NOSPACE;
			return -1;
		}
		(void)strcpy(expanded, pattern);
	}
	path[0] = '\0';

	/* Returns the computed result. */
	return glob_walk(state, path, 0, expanded);
}

/*
 * Supports the glob brace operation.
 *
 * Takes one {a,b} apart and expands each alternative in turn, innermost
 * first, so that nested braces come out as every combination.
 */
static int
glob_brace(
	struct glob_state *state,
	const char *pattern,
	unsigned depth)
{
	char rebuilt[PATH_MAX];
	const char *open;
	const char *close;
	const char *comma;
	const char *walk;
	size_t prefix;
	size_t piece;
	unsigned nesting;
	int outcome;

	/* Refuses an expansion that nests without end. */
	if (depth > 32U) {
		state->error = GLOB_NOSPACE;
		return -1;
	}

	/* Finds the first brace that is not quoted. */
	open = NULL;
	for (walk = pattern; *walk != '\0'; walk++) {
		if (*walk == '\\' && (state->flags & GLOB_NOESCAPE) == 0) {
			if (walk[1] != '\0')
				walk++;
			continue;
		}
		if (*walk == '{') {
			open = walk;
			break;
		}
	}

	/* A pattern with no braces is expanded as it stands. */
	if (open == NULL)
		return glob_one(state, pattern);

	/* Finds the brace that closes it, counting the ones between. */
	nesting = 0;
	close = NULL;
	for (walk = open + 1; *walk != '\0'; walk++) {
		if (*walk == '\\' && (state->flags & GLOB_NOESCAPE) == 0) {
			if (walk[1] != '\0')
				walk++;
			continue;
		}
		if (*walk == '{') {
			nesting++;
		} else if (*walk == '}') {
			if (nesting == 0U) {
				close = walk;
				break;
			}
			nesting--;
		}
	}

	/* An opening brace with no closing one is an ordinary character. */
	if (close == NULL)
		return glob_one(state, pattern);
	prefix = (size_t)(open - pattern);

	/* Process each element required by the operation. */
	comma = open + 1;
	nesting = 0;
	outcome = 0;
	for (walk = open + 1; walk <= close && outcome == 0; walk++) {
		/* Skips what a quote or a nested brace covers. */
		if (walk < close && *walk == '\\' &&
		    (state->flags & GLOB_NOESCAPE) == 0) {
			if (walk[1] != '\0')
				walk++;
			continue;
		}
		if (walk < close && *walk == '{') {
			nesting++;
			continue;
		}
		if (walk < close && *walk == '}') {
			nesting--;
			continue;
		}
		if (walk != close && (*walk != ',' || nesting != 0U))
			continue;

		/* Builds the pattern this alternative stands for. */
		piece = (size_t)(walk - comma);
		if (prefix + piece + strlen(close + 1) + 1U >
		    sizeof(rebuilt)) {
			state->error = GLOB_NOSPACE;
			return -1;
		}
		memcpy(rebuilt, pattern, prefix);
		memcpy(rebuilt + prefix, comma, piece);
		(void)strcpy(rebuilt + prefix + piece, close + 1);
		outcome = glob_brace(state, rebuilt, depth + 1U);
		comma = walk + 1;
	}

	/* Returns the computed result. */
	return outcome;
}

/*
 * Implements the glob operation.
 */
int
glob(
	const char *pattern,
	int flags,
	int (*report)(const char *, int),
	glob_t *result)
{
	struct glob_state state;
	size_t started;
	char **grown;
	size_t total;

	/* Handles the arguments availability. */
	if (pattern == NULL || result == NULL)
		return GLOB_NOSYS;

	/* A fresh call starts from nothing; an appending one carries on. */
	if ((flags & GLOB_APPEND) == 0) {
		result->gl_pathc = 0;
		result->gl_matchc = 0;
		result->gl_pathv = NULL;
		if ((flags & GLOB_DOOFFS) == 0)
			result->gl_offs = 0;
	}
	result->gl_matchc = 0;
	result->gl_flags = flags;
	if (has_magic(pattern, flags))
		result->gl_flags |= GLOB_MAGCHAR;
	started = result->gl_pathc;

	/* Reserves the slots a caller asked to keep at the front. */
	if ((flags & GLOB_APPEND) == 0 && result->gl_offs != 0U) {
		total = result->gl_offs + 1U;
		result->gl_pathv = calloc(total, sizeof(*result->gl_pathv));
		if (result->gl_pathv == NULL)
			return GLOB_NOSPACE;
	}
	memset(&state, 0, sizeof(state));
	state.result = result;
	state.flags = flags;
	state.report = report;

	/* Expands the pattern, taking braces apart first if asked. */
	if ((flags & GLOB_BRACE) != 0)
		(void)glob_brace(&state, pattern, 0);
	else
		(void)glob_one(&state, pattern);

	/* Reports a failure the walk stopped for. */
	if (state.error != 0)
		return state.error;

	/* Handles a pattern that selected nothing. */
	if (result->gl_pathc == started) {
		/*
		 * The pattern itself stands in for the names it did not
		 * find, which is what a shell passes on unchanged.
		 */
		if ((flags & GLOB_NOCHECK) != 0 ||
		    ((flags & GLOB_NOMAGIC) != 0 &&
		     (result->gl_flags & GLOB_MAGCHAR) == 0)) {
			if (add_path(&state, pattern, 0) != 0)
				return GLOB_NOSPACE;

			/* Reports successful completion. */
			return 0;
		}

		/* An empty result still ends in a null for the caller. */
		if (result->gl_pathv == NULL) {
			grown = calloc(result->gl_offs + 1U, sizeof(*grown));
			if (grown == NULL)
				return GLOB_NOSPACE;
			result->gl_pathv = grown;
		}

		/* Reports operation failure. */
		return GLOB_NOMATCH;
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the globfree operation.
 */
void
globfree(
	glob_t *result)
{
	size_t index;

	/* Handles the result availability. */
	if (result == NULL || result->gl_pathv == NULL) {
		if (result != NULL) {
			result->gl_pathc = 0;
			result->gl_matchc = 0;
		}
		return;
	}

	/* Process each remaining element. */
	for (index = 0; index < result->gl_pathc; index++)
		free(result->gl_pathv[result->gl_offs + index]);
	free(result->gl_pathv);
	result->gl_pathv = NULL;
	result->gl_pathc = 0;
	result->gl_matchc = 0;
}
