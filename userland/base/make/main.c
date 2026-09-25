/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Maintains files from the rules of makefiles (POSIX XCU make, with the
 * GNU make features that automake's output and common packages use).
 *
 *	make [-eiknqrRsStw] [-C dir] [-f makefile]... [-I dir] [-j [jobs]] [name=value]... [target]...
 *
 * The options also come from MAKEFLAGS, which a recursive make inherits
 * with the command line's variables.  The makefiles are makefile, then
 * Makefile, unless -f names others; GNUmakefile is not read (it is GNU
 * make's own).  -j runs that many recipes at once (any number without a
 * count), sharing them with recursive makes through a jobserver as GNU
 * make does.  The status is 0, 1 when -q finds a target out of date, and
 * 2 on an error.
 */

#include "make.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The options of this run. */
struct make_options make_options;

/* The level of this make among recursive ones, from MAKELEVEL. */
int make_level;

/*
 * The jobserver MAKEFLAGS named (--jobserver-auth=R,W), whether -j was
 * given on this make's own command line, and whether MAKEFLAGS is being
 * read now.
 */
static char *jobserver_auth;
static int jobs_on_command_line;
static int reading_makeflags;

/*
 * The built-in variables and rules, read before the makefiles unless -r
 * (the rules) or -R (the variables): POSIX's suffix rules in GNU make's
 * form, so that the commands they echo are the same.
 */
static const char builtin_variables[] =
	"AR = ar\n"
	"ARFLAGS = rv\n"
	"AS = as\n"
	"CC = cc\n"
	"CXX = c++\n"
	"CPP = $(CC) -E\n"
	"LEX = lex\n"
	"YACC = yacc\n"
	"RM = rm -f\n"
	"OUTPUT_OPTION = -o $@\n"
	"COMPILE.c = $(CC) $(CFLAGS) $(CPPFLAGS) $(TARGET_ARCH) -c\n"
	"LINK.c = $(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $(TARGET_ARCH)\n"
	"COMPILE.cc = $(CXX) $(CXXFLAGS) $(CPPFLAGS) $(TARGET_ARCH) -c\n"
	"LINK.cc = $(CXX) $(CXXFLAGS) $(CPPFLAGS) $(LDFLAGS) $(TARGET_ARCH)\n"
	"COMPILE.s = $(AS) $(ASFLAGS) $(TARGET_MACH)\n"
	"COMPILE.S = $(CC) $(ASFLAGS) $(CPPFLAGS) $(TARGET_MACH) -c\n"
	"YACC.y = $(YACC) $(YFLAGS)\n"
	"LEX.l = $(LEX) $(LFLAGS) -t\n";
static const char builtin_rules[] =
	".SUFFIXES: .out .a .o .c .cc .C .cpp .cxx .s .S .sh .y .l .h\n"
	".c.o:\n\t$(COMPILE.c) $(OUTPUT_OPTION) $<\n"
	".c:\n\t$(LINK.c) $^ $(LOADLIBES) $(LDLIBS) -o $@\n"
	".cc.o:\n\t$(COMPILE.cc) $(OUTPUT_OPTION) $<\n"
	".cc:\n\t$(LINK.cc) $^ $(LOADLIBES) $(LDLIBS) -o $@\n"
	".cpp.o:\n\t$(COMPILE.cc) $(OUTPUT_OPTION) $<\n"
	".cpp:\n\t$(LINK.cc) $^ $(LOADLIBES) $(LDLIBS) -o $@\n"
	".C.o:\n\t$(COMPILE.cc) $(OUTPUT_OPTION) $<\n"
	".cxx.o:\n\t$(COMPILE.cc) $(OUTPUT_OPTION) $<\n"
	".s.o:\n\t$(COMPILE.s) -o $@ $<\n"
	".S.o:\n\t$(COMPILE.S) -o $@ $<\n"
	".y.c:\n\t$(YACC.y) $<\n\tmv -f y.tab.c $@\n"
	".l.c:\n\t@$(RM) $@\n\t$(LEX.l) $< > $@\n"
	".sh:\n\tcat $< >$@\n\tchmod a+x $@\n";

/*
 * What the command line (and MAKEFLAGS) asked for: the makefiles, the
 * directories to change to, the variables and the goals, in order.  It
 * lives for the whole run.
 */
struct request {
	char **makefiles;
	size_t makefile_count;
	char **directories;
	size_t directory_count;
	char **variables;
	size_t variable_count;
	char **goals;
	size_t goal_count;
};

static void add_string(char ***list, size_t *count, const char *text);
static void read_makeflags(struct request *request);
static char **split_makeflags(const char *text, size_t *count);
static void parse_arguments(int argc, char **argv, struct request *request, int from_makeflags);
static int long_option(const char *argument, const char *next, struct request *request);
static int short_options(const char *argument, const char *next, struct request *request);
static void set_jobs(const char *count);
static void usage(void);
static char *program_path(const char *argv0);
static void define_command_line(const struct request *request);
static void define_defaults(const char *make_path, const struct request *request);
static void define_makeflags(const struct request *request);
static void read_makefiles(const struct request *request);
static void directory_message(const char *verb);
static int make_goals(const struct request *request);
static void remake_makefiles(char **argv, const char *make_path, const char *start_directory);
static int remake_one(const char *name, int must_exist, int missing);
static void restart(char **argv, const char *make_path, const char *start_directory);

/*
 * Runs make.
 */
int
main(
	int argc,
	char **argv)
{
	extern char **environ;
	struct request request;
	const char *level;
	char *make_path;
	char start_directory[4096];
	char *found;
	size_t index;
	int error;
	int failed;

	/* The level from a make above this one. */
	memset(&request, 0, sizeof(request));
	make_options.print_directory = -1;
	make_options.jobs = 1;
	level = getenv("MAKELEVEL");
	if (level != NULL)
		make_level = atoi(level);

	/* The options: MAKEFLAGS first, then the command line, which wins. */
	read_makeflags(&request);
	parse_arguments(argc - 1, argv + 1, &request, 0);

	/* $(MAKE) is the program as run, made absolute when it is a relative path, before -C moves. */
	make_path = program_path(argv[0]);
	found = getcwd(start_directory, sizeof(start_directory));
	if (found == NULL)
		start_directory[0] = '\0';

	/* -C: each directory in turn. */
	for (index = 0; index < request.directory_count; index++) {
		error = chdir(request.directories[index]);
		if (error != 0)
			make_fatal("%s: %s", request.directories[index], strerror(errno));
	}

	/* The jobs, and the jobserver a recursive make shares. */
	job_server_start(jobserver_auth, jobs_on_command_line);

	/* The variables: the environment, the defaults, then the command line. */
	variable_import_environment(environ);
	define_defaults(make_path, &request);
	define_command_line(&request);
	define_makeflags(&request);

	/* The built-in rules, then the makefiles. */
	if (!make_options.no_builtin_variables)
		read_builtin(builtin_variables);
	if (!make_options.no_builtin_rules)
		read_builtin(builtin_rules);
	read_makefiles(&request);
	rule_finish_reading();

	/* The makefiles themselves, and the missing includes, may have rules that remake them. */
	remake_makefiles(argv, make_path, start_directory);

	/* A recursive make, or -C, says where it works. */
	if (make_options.print_directory < 0) {
		make_options.print_directory = 0;
		if (make_level > 0 || request.directory_count > 0)
			make_options.print_directory = 1;
	}

	/* The message, when it applies. */
	if (make_options.print_directory)
		directory_message("Entering");

	/* The goals. */
	job_install_signals();
	failed = make_goals(&request);
	job_server_finish();
	if (make_options.print_directory)
		directory_message("Leaving");
	fflush(stdout);

	/* An error is status 2; -q with a target out of date is 1. */
	if (failed)
		return 2;
	if (make_question_failed)
		return 1;

	/* Succeeded. */
	free(make_path);
	return 0;
}

/* Appends a copy of a string to a list. */
static void
add_string(
	char ***list,
	size_t *count,
	const char *text)
{
	/* One more entry. */
	*list = make_realloc(*list, (*count + 1U) * sizeof(**list));
	(*list)[*count] = make_strdup(text);
	(*count)++;
}

/*
 * Takes the options and variables of MAKEFLAGS from the environment: a
 * first word of letters without a dash (GNU make's form), words that are
 * options, and after -- (or anywhere) name=value words.
 */
static void
read_makeflags(
	struct request *request)
{
	const char *flags;
	char **words;
	char *letters;
	const char *equals;
	size_t count;
	size_t index;
	int after_dashes;
	int compare;

	/* Nothing to read. */
	flags = getenv("MAKEFLAGS");
	if (flags == NULL || flags[0] == '\0')
		return;
	words = split_makeflags(flags, &count);
	reading_makeflags = 1;

	/* Each word. */
	after_dashes = 0;
	for (index = 0; index < count; index++) {
		compare = strcmp(words[index], "--");
		if (compare == 0) {
			after_dashes = 1;
			continue;
		}

		/* A variable, after -- or with an = (an option such as --jobserver-auth=R,W starts with a dash). */
		equals = strchr(words[index], '=');
		if (after_dashes || (equals != NULL && words[index][0] != '-')) {
			add_string(&request->variables, &request->variable_count, words[index]);
			continue;
		}

		/* The first word may be letters without their dash. */
		if (index == 0 && words[index][0] != '-') {
			letters = make_malloc(strlen(words[index]) + 2U);
			letters[0] = '-';
			strcpy(letters + 1, words[index]);
			short_options(letters, NULL, request);
			free(letters);
			continue;
		}

		/* Any other word is an option. */
		parse_arguments(1, &words[index], request, 1);
	}

	/* The words are done with. */
	reading_makeflags = 0;
	for (index = 0; index < count; index++)
		free(words[index]);
	free(words);
}

/* Splits MAKEFLAGS at blanks, a backslash keeping the character after it in the word. */
static char **
split_makeflags(
	const char *text,
	size_t *count)
{
	struct buffer word;
	const char *cursor;
	char **words;
	int in_word;

	/* Each character. */
	words = NULL;
	*count = 0;
	memset(&word, 0, sizeof(word));
	in_word = 0;
	for (cursor = text; ; cursor++) {
		if (*cursor == '\0' || *cursor == ' ' || *cursor == '\t') {
			if (in_word) {
				words = make_realloc(words, (*count + 1U) * sizeof(*words));
				words[*count] = buffer_finish(&word);
				(*count)++;
				in_word = 0;
			}

			/* The end of the text ends the last word. */
			if (*cursor == '\0')
				break;
			continue;
		}

		/* A backslash keeps the next character, blanks included. */
		if (*cursor == '\\' && cursor[1] != '\0')
			cursor++;
		buffer_add_char(&word, *cursor);
		in_word = 1;
	}

	/* Succeeded. */
	return words;
}

/*
 * Reads command-line arguments: options, name=value, and goals (not from
 * MAKEFLAGS).
 */
static void
parse_arguments(
	int argc,
	char **argv,
	struct request *request,
	int from_makeflags)
{
	const char *next;
	const char *equals;
	int index;
	int used;
	int options_ended;
	int compare;

	/* Each argument; -- ends the options. */
	options_ended = 0;
	for (index = 0; index < argc; index++) {
		next = NULL;
		if (index + 1 < argc)
			next = argv[index + 1];
		compare = strcmp(argv[index], "--");
		if (compare == 0 && !options_ended) {
			options_ended = 1;
			continue;
		}

		/* An option, which may take the next argument. */
		if (!options_ended && argv[index][0] == '-' && argv[index][1] != '\0') {
			if (argv[index][1] == '-') {
				used = long_option(argv[index], next, request);
			} else {
				used = short_options(argv[index], next, request);
			}

			/* Past the argument the option took. */
			index += used;
			continue;
		}

		/* A variable, or a goal. */
		equals = strchr(argv[index], '=');
		if (equals != NULL) {
			add_string(&request->variables, &request->variable_count, argv[index]);
		} else if (!from_makeflags) {
			add_string(&request->goals, &request->goal_count, argv[index]);
		}
	}
}

/*
 * Reads a long option (--name or --name=value); returns 1 when it took
 * the next argument as its value.
 */
static int
long_option(
	const char *argument,
	const char *next,
	struct request *request)
{
	static const char *const flags[] = {
		"--silent", "--quiet", "--keep-going", "--ignore-errors", "--just-print",
		"--dry-run", "--recon", "--question", "--touch", "--environment-overrides",
		"--no-builtin-rules", "--no-builtin-variables", "--print-directory",
		"--no-print-directory", "--no-keep-going", "--stop"
	};
	static const char letters[] = "sskinnnqterRwxSS";
	const char *equals;
	const char *value;
	char option[3];
	size_t name_length;
	size_t index;
	int compare;
	int took_next;
	int is_file;

	/* A flag spelled out is its letter ('x' stands for --no-print-directory). */
	for (index = 0; index < sizeof(flags) / sizeof(flags[0]); index++) {
		compare = strcmp(argument, flags[index]);
		if (compare != 0)
			continue;
		if (letters[index] == 'x') {
			make_options.print_directory = 0;
			return 0;
		}

		/* Any other flag is its letter. */
		option[0] = '-';
		option[1] = letters[index];
		option[2] = '\0';
		short_options(option, NULL, request);
		return 0;
	}

	/* An option with a value, after = or as the next argument. */
	equals = strchr(argument, '=');
	took_next = 0;
	value = NULL;
	name_length = strlen(argument);
	if (equals != NULL) {
		value = equals + 1;
		name_length = (size_t)(equals - argument);
	}

	/* --jobs, with its count only after =; alone, any number of jobs. */
	compare = strncmp(argument, "--jobs", name_length);
	if (compare == 0 && name_length == 6) {
		set_jobs(value);
		return 0;
	}

	/* The jobserver of the make above (--jobserver-fds is its older name). */
	compare = strncmp(argument, "--jobserver-auth", name_length);
	if (compare == 0 && name_length == 16 && value != NULL) {
		free(jobserver_auth);
		jobserver_auth = make_strdup(value);
		return 0;
	}
	compare = strncmp(argument, "--jobserver-fds", name_length);
	if (compare == 0 && name_length == 15 && value != NULL) {
		free(jobserver_auth);
		jobserver_auth = make_strdup(value);
		return 0;
	}

	/* --file, --makefile, --directory, --include-dir. */
	if (value == NULL) {
		value = next;
		took_next = 1;
	}

	/* An option that needs a value and has none. */
	if (value == NULL)
		usage();
	is_file = 0;
	compare = strncmp(argument, "--file", name_length);
	if (compare == 0 && name_length == 6)
		is_file = 1;
	compare = strncmp(argument, "--makefile", name_length);
	if (compare == 0 && name_length == 10)
		is_file = 1;
	if (is_file) {
		add_string(&request->makefiles, &request->makefile_count, value);
		return took_next;
	}

	/* --directory. */
	compare = strncmp(argument, "--directory", name_length);
	if (compare == 0 && name_length == 11) {
		add_string(&request->directories, &request->directory_count, value);
		return took_next;
	}

	/* --include-dir. */
	compare = strncmp(argument, "--include-dir", name_length);
	if (compare == 0 && name_length == 13) {
		read_add_include_directory(value);
		return took_next;
	}

	/* Anything else. */
	usage();
	return 0;
}

/*
 * Reads a word of one-letter options; returns 1 when the last of them
 * took the next argument as its value (-f file, -C dir, -I dir, -j n).
 */
static int
short_options(
	const char *argument,
	const char *next,
	struct request *request)
{
	const char *letter;
	const char *value;

	/* Each letter. */
	for (letter = argument + 1; *letter != '\0'; letter++) {
		/* Chooses the option by its letter. */
		switch (*letter) {
		case 'e':
			make_options.environment_overrides = 1;
			break;
		case 'i':
			make_options.ignore_errors = 1;
			break;
		case 'k':
			make_options.keep_going = 1;
			break;
		case 'S':
			make_options.keep_going = 0;
			break;
		case 'n':
			make_options.dry_run = 1;
			break;
		case 'q':
			make_options.question = 1;
			break;
		case 'r':
			make_options.no_builtin_rules = 1;
			break;
		case 'R':
			make_options.no_builtin_variables = 1;
			break;
		case 's':
			make_options.silent = 1;
			break;
		case 't':
			make_options.touch = 1;
			break;
		case 'w':
			make_options.print_directory = 1;
			break;
		case 'j':
			/* -j with its count in the word, as the next argument, or none. */
			if (letter[1] >= '0' && letter[1] <= '9') {
				set_jobs(letter + 1);
				return 0;
			}
			if (letter[1] == '\0' && next != NULL && next[0] >= '0' && next[0] <= '9') {
				set_jobs(next);
				return 1;
			}
			set_jobs(NULL);
			if (letter[1] != '\0')
				usage();
			return 0;
		case 'f':
		case 'C':
		case 'I':
			/* The value is the rest of the word, or the next argument. */
			value = letter + 1;
			if (*value == '\0')
				value = next;
			if (value == NULL)
				usage();
			if (*letter == 'f')
				add_string(&request->makefiles, &request->makefile_count, value);
			if (*letter == 'C')
				add_string(&request->directories, &request->directory_count, value);
			if (*letter == 'I')
				read_add_include_directory(value);
			if (letter[1] == '\0')
				return 1;
			return 0;
		default:
			usage();
			break;
		}
	}

	/* Succeeded: no argument was taken. */
	return 0;
}

/*
 * Takes the count of -j: a positive number, or NULL for any number of
 * jobs.  A count on this make's own command line (not from MAKEFLAGS)
 * gives it a jobserver of its own.
 */
static void
set_jobs(
	const char *count)
{
	char *end;
	long value;

	/* Where the option came from. */
	if (!reading_makeflags)
		jobs_on_command_line = 1;

	/* No count: no limit. */
	if (count == NULL) {
		make_options.jobs = 0;
		return;
	}

	/* A count, which must be a positive number. */
	value = strtol(count, &end, 10);
	if (end == count || *end != '\0' || value < 1 || value > 4096)
		usage();

	/* Succeeded. */
	make_options.jobs = (int)value;
}

/* Reports the usage and ends make. */
static void
usage(
	void)
{
	/* The form. */
	fprintf(stderr, "usage: make [-eiknqrRsStw] [-C dir] [-f makefile]... [-I dir] [-j [jobs]] [name=value]... [target]...\n");
	exit(2);
}

/* Returns $(MAKE): argv[0], made absolute when it is a relative path. */
static char *
program_path(
	const char *argv0)
{
	struct buffer path;
	char directory[4096];
	const char *slash;
	char *found;

	/* A name without a slash is found on PATH by whoever runs it; an absolute one is ready. */
	slash = strchr(argv0, '/');
	if (slash == NULL || argv0[0] == '/')
		return make_strdup(argv0);

	/* A relative path, after the current directory. */
	found = getcwd(directory, sizeof(directory));
	if (found == NULL)
		return make_strdup(argv0);

	/* Succeeded. */
	memset(&path, 0, sizeof(path));
	buffer_add_string(&path, directory);
	buffer_add_char(&path, '/');
	buffer_add_string(&path, argv0);
	return buffer_finish(&path);
}

/* Defines the variables of the command line (and MAKEFLAGS), which override the makefiles. */
static void
define_command_line(
	const struct request *request)
{
	struct expansion context;
	struct variable *variable;
	enum assign_kind kind;
	const char *equals;
	char *name;
	size_t index;
	size_t length;

	/* Each name=value (name:=value too), later ones winning. */
	context.scope = &make_global_scope;
	context.automatic = NULL;
	context.file = NULL;
	context.line = 0;
	for (index = 0; index < request->variable_count; index++) {
		equals = strchr(request->variables[index], '=');
		length = (size_t)(equals - request->variables[index]);
		kind = ASSIGN_RECURSIVE;
		if (length > 0 && request->variables[index][length - 1U] == ':') {
			kind = ASSIGN_SIMPLE;
			length--;
		}

		/* The name before the operator. */
		name = make_strndup(request->variables[index], length);
		variable_assign(&make_global_variables, name, equals + 1, kind, ORIGIN_COMMAND_LINE, &context);
		variable = variable_lookup(&make_global_scope, name, length);
		if (variable != NULL)
			variable->export_state = 0;
		free(name);
	}
}

/*
 * Defines the variables make gives every makefile: MAKE, SHELL,
 * MAKELEVEL, CURDIR and MAKECMDGOALS.  MAKE_VERSION is not defined: this
 * make does not claim to be GNU make.
 */
static void
define_defaults(
	const char *make_path,
	const struct request *request)
{
	struct buffer goals;
	struct variable *variable;
	char directory[4096];
	char level[32];
	char *found;
	size_t index;

	/* The program, the shell and the level. */
	variable_set_value(&make_global_variables, "MAKE", make_path, FLAVOR_RECURSIVE, ORIGIN_DEFAULT);
	variable_set_value(&make_global_variables, "SHELL", "/bin/sh", FLAVOR_RECURSIVE, ORIGIN_DEFAULT);
	snprintf(level, sizeof(level), "%d", make_level);
	variable = variable_set_value(&make_global_variables, "MAKELEVEL", level, FLAVOR_SIMPLE, ORIGIN_ENVIRONMENT);
	variable->export_state = 1;

	/* The directory make works in, after -C. */
	found = getcwd(directory, sizeof(directory));
	if (found != NULL)
		variable_set_value(&make_global_variables, "CURDIR", directory, FLAVOR_SIMPLE, ORIGIN_FILE);

	/* The goals of the command line. */
	memset(&goals, 0, sizeof(goals));
	for (index = 0; index < request->goal_count; index++) {
		if (index > 0)
			buffer_add_char(&goals, ' ');
		buffer_add_string(&goals, request->goals[index]);
	}

	/* $(MAKECMDGOALS). */
	variable_set_value(&make_global_variables, "MAKECMDGOALS", buffer_text(&goals), FLAVOR_SIMPLE, ORIGIN_DEFAULT);
	free(goals.text);
}

/*
 * Defines MAKEFLAGS (and MFLAGS) for a recursive make: the letters of the
 * options without a dash, then " -- " and the command line's variables,
 * their blanks and backslashes escaped.
 */
static void
define_makeflags(
	const struct request *request)
{
	struct buffer flags;
	struct buffer letters;
	struct variable *variable;
	const char *cursor;
	const char *jobs;
	size_t index;

	/* The letters of the options that a recursive make should share. */
	memset(&letters, 0, sizeof(letters));
	if (make_options.environment_overrides)
		buffer_add_char(&letters, 'e');
	if (make_options.ignore_errors)
		buffer_add_char(&letters, 'i');
	if (make_options.keep_going)
		buffer_add_char(&letters, 'k');
	if (make_options.dry_run)
		buffer_add_char(&letters, 'n');
	if (make_options.question)
		buffer_add_char(&letters, 'q');
	if (make_options.no_builtin_rules)
		buffer_add_char(&letters, 'r');
	if (make_options.no_builtin_variables)
		buffer_add_char(&letters, 'R');
	if (make_options.silent)
		buffer_add_char(&letters, 's');
	if (make_options.touch)
		buffer_add_char(&letters, 't');

	/* MAKEFLAGS: the letters, the jobs, then the variables after --. */
	memset(&flags, 0, sizeof(flags));
	buffer_add_string(&flags, buffer_text(&letters));
	jobs = job_server_flags();
	if (jobs[0] != '\0') {
		buffer_add_char(&flags, ' ');
		buffer_add_string(&flags, jobs);
	}
	if (request->variable_count > 0)
		buffer_add_string(&flags, " --");
	for (index = 0; index < request->variable_count; index++) {
		buffer_add_char(&flags, ' ');
		for (cursor = request->variables[index]; *cursor != '\0'; cursor++) {
			if (*cursor == ' ' || *cursor == '\t' || *cursor == '\\')
				buffer_add_char(&flags, '\\');
			buffer_add_char(&flags, *cursor);
		}
	}

	/* MAKEFLAGS goes to every recipe's environment. */
	variable = variable_set_value(&make_global_variables, "MAKEFLAGS", buffer_text(&flags), FLAVOR_SIMPLE, ORIGIN_FILE);
	variable->export_state = 1;
	free(flags.text);

	/* MFLAGS: the letters with a dash, for old makefiles. */
	memset(&flags, 0, sizeof(flags));
	if (letters.length > 0) {
		buffer_add_char(&flags, '-');
		buffer_add_string(&flags, letters.text);
	}

	/* MFLAGS stays in make. */
	variable_set_value(&make_global_variables, "MFLAGS", buffer_text(&flags), FLAVOR_SIMPLE, ORIGIN_FILE);
	free(flags.text);
	free(letters.text);
}

/* Reads the makefiles -f named, or makefile, or else Makefile. */
static void
read_makefiles(
	const struct request *request)
{
	size_t index;
	int found;

	/* The ones -f named, each of which must exist. */
	if (request->makefile_count > 0) {
		for (index = 0; index < request->makefile_count; index++)
			read_makefile(request->makefiles[index], 1);
		return;
	}

	/* Otherwise the first of the usual names that is there. */
	found = read_makefile("makefile", 0);
	if (found)
		return;

	/* Makefile, when makefile is not there; neither being there is not yet an error. */
	read_makefile("Makefile", 0);
}

/*
 * Brings the makefiles that were read, and the include files that were
 * missing, up to date before the goals (-n, -q and -t do not stop this,
 * as in GNU make).  When any of them changed, make starts again so that
 * it reads them as they are now.
 */
static void
remake_makefiles(
	char **argv,
	const char *make_path,
	const char *start_directory)
{
	struct make_options saved;
	struct variable *list;
	const char *cursor;
	const char *word;
	char **missing;
	char *name;
	int *must_exist;
	size_t missing_count;
	size_t length;
	size_t index;
	int changed;
	int any_changed;

	/* The updates run whatever -n, -q and -t say. */
	saved = make_options;
	make_options.dry_run = 0;
	make_options.question = 0;
	make_options.touch = 0;
	any_changed = 0;

	/* Each makefile that was read. */
	list = variable_lookup(&make_global_scope, "MAKEFILE_LIST", 13);
	cursor = "";
	if (list != NULL)
		cursor = list->value;
	for (;;) {
		word = make_next_word(&cursor, &length);
		if (word == NULL)
			break;
		name = make_strndup(word, length);
		changed = remake_one(name, 1, 0);
		if (changed)
			any_changed = 1;
		free(name);
	}

	/* Each include file that was not there. */
	read_remake_list(&missing, &must_exist, &missing_count);
	for (index = 0; index < missing_count; index++) {
		changed = remake_one(missing[index], must_exist[index], 1);
		if (changed)
			any_changed = 1;
	}

	/* The options are as the command line gave them again. */
	make_options = saved;

	/* A makefile that changed is read again, by a new start. */
	if (any_changed)
		restart(argv, make_path, start_directory);
}

/*
 * Brings one makefile up to date; returns 1 when its file changed.  A
 * missing include with no rule to make it ends make when it must exist.
 */
static int
remake_one(
	const char *name,
	int must_exist,
	int missing)
{
	struct target *target;
	long long seconds;
	long nanoseconds;
	int existed;
	int found;
	int present;
	int failed;
	int compare;

	/* Standard input is not a file to remake. */
	compare = strcmp(name, "-");
	if (compare == 0)
		return 0;

	/* A makefile no rule names is as it is; a missing one may still have an implicit rule. */
	target = target_enter(name);
	present = target_has_recipe(target);
	if (!target->is_target && !present) {
		found = 0;
		if (missing)
			found = rule_find_implicit(target);
		if (!found && missing && must_exist)
			make_fatal("No rule to make target '%s'", name);
		if (!found)
			return 0;
	}

	/* A double-colon makefile rule without prerequisites would run every time: GNU make skips it. */
	if (target->is_double_colon && target->double_colon != NULL && target->double_colon->dependencies == NULL)
		return 0;

	/* Its time before, the update, and its time after. */
	update_time_of(target);
	existed = target->exists;
	seconds = target->seconds;
	nanoseconds = target->nanoseconds;
	failed = update_goal(target, 0);
	if (failed)
		exit(2);
	update_time_of(target);

	/* Succeeded: whether the file changed. */
	if (target->exists != existed || target->seconds != seconds || target->nanoseconds != nanoseconds)
		return 1;
	return 0;
}

/*
 * Starts make again with the same arguments, from the directory it was
 * started in, counting the restarts in MAKE_RESTARTS so that a makefile
 * that always changes does not loop.
 */
static void
restart(
	char **argv,
	const char *make_path,
	const char *start_directory)
{
	const char *restarts;
	const char *slash;
	char number[32];
	int count;

	/* A bounded number of restarts. */
	restarts = getenv("MAKE_RESTARTS");
	count = 0;
	if (restarts != NULL)
		count = atoi(restarts);
	if (count >= 10) {
		make_message("warning: makefiles keep changing; not restarting again");
		return;
	}

	/* The count goes to the next start. */
	snprintf(number, sizeof(number), "%d", count + 1);
	setenv("MAKE_RESTARTS", number, 1);

	/* The same program with the same arguments, where it started. */
	fflush(stdout);
	if (start_directory[0] != '\0')
		(void)chdir(start_directory);
	slash = strchr(make_path, '/');
	if (slash != NULL) {
		execv(make_path, argv);
	} else {
		execvp(make_path, argv);
	}

	/* exec returned: the program could not be started. */
	make_fatal("%s: %s", make_path, strerror(errno));
}

/* Says which directory make works in: "make[1]: Entering directory '...'". */
static void
directory_message(
	const char *verb)
{
	char directory[4096];
	char *found;

	/* The directory now. */
	found = getcwd(directory, sizeof(directory));
	if (found == NULL)
		return;

	/* Succeeded: on standard output, as GNU make writes it. */
	printf("%s: %s directory '%s'\n", make_program(), verb, directory);
	fflush(stdout);
}

/*
 * Makes the goals: those of the command line, or .DEFAULT_GOAL, or the
 * first target.  Returns 1 when any could not be made.
 */
static int
make_goals(
	const struct request *request)
{
	struct variable *variable;
	struct target *goal;
	const char *name;
	size_t index;
	size_t read;
	int failed;
	int any_failed;

	/* The command line's goals, in order. */
	any_failed = 0;
	if (request->goal_count > 0) {
		for (index = 0; index < request->goal_count; index++) {
			goal = target_enter(request->goals[index]);
			failed = update_goal(goal, 1);
			if (!failed)
				continue;
			any_failed = 1;
			if (!make_options.keep_going)
				break;
		}

		/* Reports whether any goal failed. */
		return any_failed;
	}

	/* .DEFAULT_GOAL, then the first target. */
	name = rule_default_goal();
	variable = variable_lookup(&make_global_scope, ".DEFAULT_GOAL", 13);
	if (variable != NULL && variable->value[0] != '\0')
		name = variable->value;
	if (name == NULL) {
		read = read_makefile_count();
		if (read == 0)
			make_fatal("No targets specified and no makefile found");
		make_fatal("No targets");
	}

	/* Succeeded or not: the one goal. */
	goal = target_enter(name);
	failed = update_goal(goal, 1);
	return failed;
}
