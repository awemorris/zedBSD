/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The bringing of targets up to date.
 *
 * A target is made after its prerequisites, left to right and depth
 * first, and at most once in a run.  It is remade when it is phony, when
 * its file does not exist, or when a prerequisite is newer (or was just
 * remade and has no file).  A target without a recipe gets one from an
 * implicit rule or from .DEFAULT; one without either and without a file
 * is an error unless some rule names it as a target.  -q, -t and -k
 * change what happens then.
 *
 * With -j several recipes run at once.  The goal's graph is walked again
 * each time a recipe ends: a walk starts the recipe of every target
 * whose prerequisites are done while a job is free, and a target whose
 * prerequisites or recipe are still running is pending until a later
 * walk finds them done.  A walk stops at the first target that needs a
 * job when none is free, so that with one job the targets are looked at
 * in the same order, and at the same moments, as a serial make does.
 */

#include "make.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Set when -q finds a target that is not up to date: make ends with status 1. */
int make_question_failed;

/*
 * The time of a target that was made but has no file (a phony target, or
 * a rule that makes nothing): newer than any file.
 */
#define UPDATE_TIME_NEW 0x7fffffffffffffffLL

static int update_target(struct target *target, const struct variable_scope *scope, const char *parent);
static void first_visit(struct target *target, const struct variable_scope *scope);
static int update_double_colon(struct target *target);
static int update_rule(struct target *target, const struct variable_scope *scope, const char *parent, int always);
static int update_prerequisites(struct target *target, const struct variable_scope *scope);
static int is_newer(const struct target *prerequisite, const struct target *target);
static int no_rule(struct target *target, const char *parent);
static int remake(struct target *target, const struct variable_scope *scope);
static void remade_time(struct target *target);
static void build_automatic(struct target *target, struct automatic *automatic, struct buffer *newer, struct buffer *unique, struct buffer *all, struct buffer *order_only);
static int listed(const struct buffer *list, const char *name);
static void explicit_stem(struct target *target);
static int touch_target(struct target *target);
static const char *file_of(const struct target *target);

/*
 * Brings a goal up to date; returns 0, or 1 when it could not be made.
 * With report set, says so when there was nothing to do.
 */
int
update_goal(
	struct target *goal,
	int report)
{
	int failed;
	int present;

	/* The goal, in the global scope, walked again each time a recipe ends. */
	for (;;) {
		failed = update_target(goal, &make_global_scope, NULL);
		if (failed != UPDATE_PENDING)
			break;
		job_wait();
	}

	/* A failure lets the recipes that are running finish, as GNU make does. */
	if (failed && job_running() > 0) {
		if (!make_options.keep_going) {
			fflush(stdout);
			fprintf(stderr, "%s: *** Waiting for unfinished jobs....\n", make_program());
		}
		while (job_running() > 0)
			job_wait();
	}

	/* The goal could not be made. */
	if (failed) {
		if (make_options.keep_going)
			make_message("Target '%s' not remade because of errors.", goal->name);
		return 1;
	}

	/* Nothing was run: say why, as GNU make does, unless asked a question. */
	if (report && !goal->remade && !make_options.question) {
		present = target_has_recipe(goal);
		fflush(stdout);
		if (goal->phony || !present) {
			printf("%s: Nothing to be done for '%s'.\n", make_program(), goal->name);
		} else {
			printf("%s: '%s' is up to date.\n", make_program(), goal->name);
		}
	}

	/* Succeeded. */
	return 0;
}

/*
 * Takes the end of a recipe that ran as a job: the target's time is read
 * again, and the target is done.
 */
void
update_recipe_done(
	struct target *target,
	int failed)
{
	/* The time of what the recipe made. */
	remade_time(target);

	/* The target is done, made or not. */
	target->failed = failed;
	target->state = TARGET_DONE;
}

/*
 * Reads the modification time of a target's file into the target.
 */
void
update_time_of(
	struct target *target)
{
	struct stat status;
	int error;

	/* The file where its name says, or where VPATH found it. */
	error = stat(target->name, &status);
	if (error != 0 && !target->phony && target->path == NULL)
		target->path = rule_vpath_search(target->name);
	if (error != 0 && target->path != NULL)
		error = stat(target->path, &status);

	/* A file that is not there. */
	if (error != 0) {
		target->exists = 0;
		target->seconds = 0;
		target->nanoseconds = 0;
		return;
	}

	/* Succeeded: its time, to the nanosecond. */
	target->exists = 1;
	target->seconds = (long long)status.st_mtim.tv_sec;
	target->nanoseconds = (long)status.st_mtim.tv_nsec;
}

/*
 * Brings one target up to date; returns 0, 1 when it could not be made,
 * or UPDATE_PENDING while its prerequisites or its recipe are running.
 * parent names the target that needs it, for messages.
 */
static int
update_target(
	struct target *target,
	const struct variable_scope *scope,
	const char *parent)
{
	int result;

	/* Once a run. */
	if (target->state == TARGET_DONE)
		return target->failed;

	/* Its recipe is running. */
	if (target->state == TARGET_RUNNING)
		return UPDATE_PENDING;

	/* The first time, the target takes its scope, its time and any implicit rule. */
	if (target->state == TARGET_UNVISITED)
		first_visit(target, scope);

	/* The target is on the walk's path while its prerequisites are walked. */
	target->in_walk = 1;
	if (target->is_double_colon) {
		result = update_double_colon(target);
	} else {
		result = update_rule(target, target->scope, parent, 0);
	}
	target->in_walk = 0;

	/* Still waiting, or its recipe started. */
	if (result == UPDATE_PENDING)
		return UPDATE_PENDING;

	/* The target is done. */
	target->failed = result;
	target->state = TARGET_DONE;
	return result;
}

/*
 * Looks at a target for the first time: the variables its prerequisites
 * and recipe see (its own in front of those of the target that needed
 * it), its file's time, and an implicit rule when it has no recipe.
 */
static void
first_visit(
	struct target *target,
	const struct variable_scope *scope)
{
	int present;

	/* The target is pending from now on. */
	target->state = TARGET_PENDING;

	/* The target's own variables reach its prerequisites and its recipe. */
	target->scope = scope;
	if (target->variables != NULL) {
		target->own_scope.set = target->variables;
		target->own_scope.parent = scope;
		target->scope = &target->own_scope;
	}

	/* A double-colon target looks at each rule when it comes to it. */
	if (target->is_double_colon)
		return;

	/* A target without a recipe may get one from an implicit rule. */
	update_time_of(target);
	present = target_has_recipe(target);
	if (!present && !target->phony)
		rule_find_implicit(target);
}

/*
 * Brings a double-colon target up to date: each of its rules, in the
 * order they were read, runs its recipe when its own prerequisites say
 * so (a rule without prerequisites always runs).  A rule starts only
 * after the one before it is done.  Returns 0, 1 or UPDATE_PENDING.
 */
static int
update_double_colon(
	struct target *target)
{
	struct target *rules[256];
	struct target *rule;
	size_t count;
	size_t index;
	int failed;
	int always;

	/* The rules were gathered newest first; they run oldest first. */
	count = 0;
	for (rule = target->double_colon; rule != NULL && count < sizeof(rules) / sizeof(rules[0]); rule = rule->double_colon) {
		rules[count] = rule;
		count++;
	}

	/* Each rule, against the target's file as it stands. */
	failed = 0;
	for (index = count; index > 0; index--) {
		rule = rules[index - 1U];

		/* A rule that is done counts as it ended. */
		if (rule->state == TARGET_RUNNING)
			return UPDATE_PENDING;
		if (rule->state != TARGET_DONE) {
			if (rule->state == TARGET_UNVISITED) {
				rule->state = TARGET_PENDING;
				rule->scope = target->scope;
				update_time_of(rule);
			}

			/* The rule's prerequisites, then its recipe. */
			always = 0;
			if (rule->dependencies == NULL)
				always = 1;
			failed = update_rule(rule, target->scope, NULL, always);
			if (failed == UPDATE_PENDING)
				return UPDATE_PENDING;
			rule->failed = failed;
			rule->state = TARGET_DONE;
		}

		/* The rule is done. */
		failed = rule->failed;
		if (failed && !make_options.keep_going)
			break;
		if (rule->remade)
			target->remade = 1;
	}

	/* The target's time is its file's now. */
	update_time_of(target);
	if (target->remade && !target->exists) {
		target->seconds = UPDATE_TIME_NEW;
		target->nanoseconds = 0;
	}

	/* Reports a rule that failed. */
	if (failed)
		return 1;

	/* Succeeded. */
	return 0;
}

/*
 * Makes a target's prerequisites, then its recipe when the target is out
 * of date (always set: in any case).  Returns 0, 1 or UPDATE_PENDING.
 */
static int
update_rule(
	struct target *target,
	const struct variable_scope *scope,
	const char *parent,
	int always)
{
	struct dependency *dependency;
	int failed;
	int must;
	int present;
	int newer;

	/* The prerequisites first. */
	failed = update_prerequisites(target, scope);
	if (failed == UPDATE_PENDING)
		return UPDATE_PENDING;
	if (failed)
		return 1;

	/* The target is out of date when it is phony, missing, or older than a prerequisite. */
	must = always;
	if (target->phony || !target->exists)
		must = 1;
	for (dependency = target->dependencies; dependency != NULL && !must; dependency = dependency->next) {
		if (dependency->order_only)
			continue;
		newer = is_newer(dependency->target, target);
		if (newer)
			must = 1;
	}

	/* A target that is up to date needs nothing more. */
	if (!must)
		return 0;

	/* Without a recipe: .DEFAULT's, or nothing to do for a target some rule names or a file that is there. */
	present = target_has_recipe(target);
	if (!present && !target->phony && !target->exists) {
		if (!target->is_target && target->dependencies == NULL) {
			target->recipe = rule_default_recipe();
			present = target_has_recipe(target);
			if (!present) {
				failed = no_rule(target, parent);
				return failed;
			}
		}
	}

	/* Succeeded or not: the recipe, or what -q and -t make of it. */
	failed = remake(target, scope);
	return failed;
}

/*
 * Makes each prerequisite of a target in order; with -k the others are
 * still made after one fails.  One that waits for .WAIT, or for a
 * .NOTPARALLEL target's earlier prerequisites, and one that finds no free
 * job end the walk of the list for now.  Returns 0, 1 when any failed, or
 * UPDATE_PENDING while any is not done.
 */
static int
update_prerequisites(
	struct target *target,
	const struct variable_scope *scope)
{
	struct dependency *dependency;
	int result;
	int any_failed;
	int any_pending;
	int slot;

	/* Each one, left to right. */
	any_failed = 0;
	any_pending = 0;
	for (dependency = target->dependencies; dependency != NULL; dependency = dependency->next) {
		/* A prerequisite that closed a loop is not followed again. */
		if (dependency->dropped)
			continue;

		/* .WAIT and .NOTPARALLEL hold it until those before it are done. */
		if (any_pending && (dependency->wait || target->not_parallel))
			break;

		/* One that is on the walk's path needs itself: the loop is dropped. */
		if (dependency->target->in_walk) {
			make_message("Circular %s <- %s dependency dropped.", target->name, dependency->target->name);
			dependency->dropped = 1;
			continue;
		}

		/* The prerequisite, as far as it can go now. */
		result = update_target(dependency->target, scope, target->name);
		if (result == UPDATE_PENDING) {
			any_pending = 1;

			/* Without a free job the walk looks no further, as a serial make would not. */
			slot = job_slot_free();
			if (!slot) {
				if (dependency->next != NULL)
					job_want_slot();
				break;
			}
			continue;
		}

		/* One that failed. */
		if (!result)
			continue;
		any_failed = 1;
		if (!make_options.keep_going)
			break;
	}

	/* Reports a prerequisite that failed, then one that is not done. */
	if (any_failed && !make_options.keep_going)
		return 1;
	if (any_pending)
		return UPDATE_PENDING;
	if (any_failed)
		return 1;

	/* Succeeded. */
	return 0;
}

/* Reports whether a prerequisite makes a target out of date. */
static int
is_newer(
	const struct target *prerequisite,
	const struct target *target)
{
	/* A target that is not there needs every prerequisite. */
	if (!target->exists)
		return 1;

	/* A prerequisite remade without a file (phony, or a rule that makes nothing) is new. */
	if (prerequisite->remade && !prerequisite->exists)
		return 1;
	if (prerequisite->phony)
		return 1;

	/* A prerequisite that is not there does not date the target. */
	if (!prerequisite->exists)
		return 0;

	/* Succeeded: by the times, to the nanosecond. */
	if (prerequisite->seconds > target->seconds)
		return 1;
	if (prerequisite->seconds == target->seconds && prerequisite->nanoseconds > target->nanoseconds)
		return 1;
	return 0;
}

/*
 * Reports a target that nothing can make; ends make unless -k.  Returns
 * 1 (the target failed).
 */
static int
no_rule(
	struct target *target,
	const char *parent)
{
	/* With -k the message has no Stop, and make goes on. */
	if (make_options.keep_going) {
		fflush(stdout);
		if (parent != NULL) {
			fprintf(stderr, "%s: *** No rule to make target '%s', needed by '%s'.\n", make_program(), target->name, parent);
		} else {
			fprintf(stderr, "%s: *** No rule to make target '%s'.\n", make_program(), target->name);
		}

		/* Reports the failure. */
		return 1;
	}

	/* Otherwise make stops. */
	if (parent != NULL)
		make_fatal("No rule to make target '%s', needed by '%s'", target->name, parent);
	make_fatal("No rule to make target '%s'", target->name);
	return 1;
}

/*
 * Remakes a target that is out of date: with -q only notes it, with -t
 * touches its file, and otherwise runs its recipe as a job.  The target's
 * time is read again after.  Returns 0, 1, or UPDATE_PENDING while the
 * recipe runs or waits for a free job.
 */
static int
remake(
	struct target *target,
	const struct variable_scope *scope)
{
	struct recipe_variables *variables;
	int present;
	int failed;
	int ignore;
	int silent;
	int slot;

	/* -q: the answer is no. */
	target->remade = 1;
	present = target_has_recipe(target);
	if (make_options.question) {
		if (present && !target->phony)
			make_question_failed = 1;
		return 0;
	}

	/* A target with nothing to run is made as it is. */
	if (!present)
		return 0;

	/* -t: the file is touched, not made. */
	if (make_options.touch && !target->phony) {
		failed = touch_target(target);
		update_time_of(target);
		return failed;
	}

	/* The recipe waits for a free job. */
	slot = job_slot_free();
	if (!slot) {
		job_want_slot();
		return UPDATE_PENDING;
	}

	/* The automatic variables, which the job owns from here. */
	variables = make_malloc(sizeof(*variables));
	memset(variables, 0, sizeof(*variables));
	explicit_stem(target);
	build_automatic(target, &variables->automatic, &variables->newer, &variables->unique, &variables->all, &variables->order_only);

	/* A target that VPATH found elsewhere is made again where its name says. */
	free(target->path);
	target->path = NULL;

	/* The recipe, which may still be running when this returns. */
	ignore = make_options.ignore_errors || make_ignore_all || target->ignore_errors;
	silent = make_options.silent || make_silent_all || target->silent;
	failed = job_start(target, target->recipe, variables, scope, ignore, silent);
	if (failed == UPDATE_PENDING) {
		target->state = TARGET_RUNNING;
		return UPDATE_PENDING;
	}

	/* The recipe ended at once: the time of what it made. */
	remade_time(target);

	/* Reports a failed recipe. */
	if (failed)
		return 1;

	/* Succeeded. */
	return 0;
}

/*
 * Reads a target's time after its recipe: a target the recipe did not
 * make is new.
 */
static void
remade_time(
	struct target *target)
{
	/* The file as the recipe left it. */
	update_time_of(target);

	/* A target that is not there is newer than any file. */
	if (!target->exists) {
		target->seconds = UPDATE_TIME_NEW;
		target->nanoseconds = 0;
	}
}

/*
 * Fills the automatic variables of a target's recipe: $@, $< (the
 * prerequisite of the implicit rule, or the first), $? (the newer
 * prerequisites), $^ (each once), $+ (all), $* and $|.
 */
static void
build_automatic(
	struct target *target,
	struct automatic *automatic,
	struct buffer *newer,
	struct buffer *unique,
	struct buffer *all,
	struct buffer *order_only)
{
	struct dependency *dependency;
	const char *name;
	int is_listed;
	int is_new;

	/* Each prerequisite into the lists it belongs to. */
	automatic->first = NULL;
	for (dependency = target->dependencies; dependency != NULL; dependency = dependency->next) {
		name = file_of(dependency->target);
		if (dependency->order_only) {
			is_listed = listed(order_only, name);
			if (!is_listed) {
				if (order_only->length > 0)
					buffer_add_char(order_only, ' ');
				buffer_add_string(order_only, name);
			}

			continue;
		}

		/* $< is the first ordinary prerequisite. */
		if (automatic->first == NULL)
			automatic->first = name;

		/* $+ keeps every one; $^ each once. */
		if (all->length > 0)
			buffer_add_char(all, ' ');
		buffer_add_string(all, name);
		is_listed = listed(unique, name);
		if (is_listed)
			continue;
		if (unique->length > 0)
			buffer_add_char(unique, ' ');
		buffer_add_string(unique, name);

		/* $? the ones that are newer than the target. */
		is_new = is_newer(dependency->target, target);
		if (!is_new)
			continue;
		if (newer->length > 0)
			buffer_add_char(newer, ' ');
		buffer_add_string(newer, name);
	}

	/* An implicit rule's prerequisite is $<. */
	if (target->implicit_first != NULL)
		automatic->first = file_of(target->implicit_first);

	/* Succeeded: the fields. */
	automatic->target = target->name;
	automatic->newer = buffer_text(newer);
	automatic->unique = buffer_text(unique);
	automatic->all = buffer_text(all);
	automatic->stem = target->stem;
	automatic->order_only = buffer_text(order_only);
}

/* Reports whether a name is a word of a space-separated list. */
static int
listed(
	const struct buffer *list,
	const char *name)
{
	const char *cursor;
	const char *word;
	size_t length;
	size_t name_length;
	int compare;

	/* Each word. */
	name_length = strlen(name);
	cursor = buffer_text(list);
	for (;;) {
		word = make_next_word(&cursor, &length);
		if (word == NULL)
			break;
		if (length != name_length)
			continue;
		compare = strncmp(word, name, length);
		if (compare == 0)
			return 1;
	}

	/* Not in it. */
	return 0;
}

/*
 * Gives a target of an explicit rule its $*: its name without a known
 * suffix, as GNU make does, when an implicit rule did not set one.
 */
static void
explicit_stem(
	struct target *target)
{
	const char *dot;
	const char *slash;

	/* An implicit rule set it already. */
	if (target->stem != NULL)
		return;

	/* The name up to its last dot, when there is one after the last slash. */
	dot = strrchr(target->name, '.');
	slash = NULL;
	if (dot != NULL)
		slash = strchr(dot, '/');
	if (dot == NULL || slash != NULL) {
		target->stem = make_strdup("");
		return;
	}

	/* Succeeded. */
	target->stem = make_strndup(target->name, (size_t)(dot - target->name));
}

/* Returns the file of a target: where VPATH found it, or its name. */
static const char *
file_of(
	const struct target *target)
{
	/* Found elsewhere. */
	if (target->path != NULL)
		return target->path;

	/* Succeeded: its name. */
	return target->name;
}

/* Touches a target's file for -t, creating it when it is missing; returns 0 or 1. */
static int
touch_target(
	struct target *target)
{
	int descriptor;
	int error;

	/* The command it stands for, unless silent. */
	if (!make_options.silent) {
		printf("touch %s\n", target->name);
		fflush(stdout);
	}

	/* The file, created if needed, gets the time of now. */
	descriptor = open(target->name, O_WRONLY | O_CREAT, 0666);
	if (descriptor < 0) {
		make_message("touch %s: %s", target->name, strerror(errno));
		return 1;
	}

	/* The descriptor was only for creating it. */
	close(descriptor);
	error = utimensat(AT_FDCWD, target->name, NULL, 0);
	if (error != 0) {
		make_message("touch %s: %s", target->name, strerror(errno));
		return 1;
	}

	/* Succeeded. */
	return 0;
}
