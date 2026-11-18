/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The targets and rules of make.
 *
 * A rule names targets and prerequisites; each target is kept once in a
 * hash table and gathers the prerequisites of every rule that names it.
 * The special targets (.PHONY, .SUFFIXES and the others) mark targets
 * instead.  A target with % is a pattern rule.  A target whose name is
 * one or two known suffixes (.c.o, .sh) is a suffix rule, which the
 * implicit search uses as POSIX says: for a target that ends with a
 * suffix, each other suffix in the order of .SUFFIXES.
 */

#include "make.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* How many suffixes .SUFFIXES may hold. */
#define RULE_SUFFIX_MAX 128

/* The special targets that .IGNORE, .SILENT, .POSIX and .NOTPARALLEL set without prerequisites. */
int make_ignore_all;
int make_silent_all;
int make_posix;
int make_not_parallel;

/* The target table, which holds every target until make ends. */
static struct target *target_chains[MAKE_HASH_SIZE];

/* The pattern rules of the makefiles, in the order they were read. */
static struct pattern_rule *pattern_rules;
static struct pattern_rule **pattern_tail = &pattern_rules;

/* The known suffixes (.SUFFIXES), in order; the list owns the strings. */
static char *suffixes[RULE_SUFFIX_MAX];
static size_t suffix_count;

/* The first target of the makefiles, which is the default goal, or NULL. */
static char *default_goal;

/* Set while the built-in rules are read: their rules give no default goal and yield to the makefiles'. */
static int reading_builtin;

/*
 * A vpath directive: the files a pattern matches are looked for in its
 * directories, in order.  The list lives until make ends.
 */
struct vpath_entry {
	struct vpath_entry *next;
	char *pattern;
	char **directories;
	size_t count;
};

/* The vpath directives in the order they were read. */
static struct vpath_entry *vpath_entries;

static unsigned hash_name(const char *name);
static int is_special(const char *name);
static void define_special(const char *name, char **prerequisites, size_t prerequisite_count);
static struct target *new_rule_target(const char *name);
static void add_prerequisites(struct target *target, char **names, size_t count, int order_only);
static void define_pattern(struct target *holder, const char *pattern, char **prerequisites, size_t count, int double_colon);
static int try_pattern_rules(struct target *target);
static int pattern_stem(const char *pattern, const char *name, char **stem, char **directory);
static char *apply_stem(const char *pattern, const char *stem, const char *directory);
static int try_suffix_rules(struct target *target);
static int use_rule(struct target *target, struct target *rule, const char *prerequisite, const char *stem);
static void prepend_dependency(struct target *target, struct target *prerequisite);
static int ought_to_exist(const char *name);
static int ends_with(const char *name, const char *suffix);
static struct target *suffix_rule(const char *first, const char *second);
static char *search_directory(const char *directory, size_t length, const char *name);
static int vpath_matches(const char *pattern, const char *name);

/*
 * Returns the target of a name, or NULL when no rule or prerequisite
 * named it.
 */
struct target *
target_lookup(
	const char *name)
{
	struct target *target;
	unsigned chain;
	int compare;

	/* The chain of the name. */
	chain = hash_name(name);
	for (target = target_chains[chain]; target != NULL; target = target->next) {
		compare = strcmp(target->name, name);
		if (compare == 0)
			return target;
	}

	/* Not named. */
	return NULL;
}

/*
 * Returns the target of a name, entering a new one when there is none.
 */
struct target *
target_enter(
	const char *name)
{
	struct target *target;
	unsigned chain;

	/* An existing target. */
	target = target_lookup(name);
	if (target != NULL)
		return target;

	/* A new one at the head of its chain. */
	chain = hash_name(name);
	target = new_rule_target(name);
	target->next = target_chains[chain];
	target_chains[chain] = target;

	/* Succeeded. */
	return target;
}

/*
 * Adds a prerequisite to the end of a target's list.
 */
struct dependency *
target_add_dependency(
	struct target *target,
	struct target *prerequisite,
	int order_only)
{
	struct dependency *dependency;

	/* A new entry at the tail. */
	dependency = make_malloc(sizeof(*dependency));
	dependency->next = NULL;
	dependency->target = prerequisite;
	dependency->order_only = order_only;
	dependency->wait = 0;
	dependency->dropped = 0;
	*target->dependencies_tail = dependency;
	target->dependencies_tail = &dependency->next;

	/* Succeeded: the new entry. */
	return dependency;
}

/*
 * Defines a rule: each target gets the prerequisites (and the order-only
 * ones).  Returns, for each target, what receives the rule's recipe when
 * the rule ends: the target, a rule of a double-colon target, or the
 * holder of a pattern rule.  The caller frees the array.
 */
struct target **
rule_define(
	char **targets,
	size_t target_count,
	char **prerequisites,
	size_t prerequisite_count,
	char **order_only,
	size_t order_only_count,
	int double_colon)
{
	struct target **receivers;
	struct target *target;
	struct target *rule;
	size_t index;
	int special;
	const char *percent;
	const char *slash;

	/* One receiver for each target. */
	receivers = make_malloc(target_count * sizeof(*receivers));
	for (index = 0; index < target_count; index++) {
		/* A special target marks others; it still receives a recipe (.DEFAULT uses it). */
		special = is_special(targets[index]);
		if (special)
			define_special(targets[index], prerequisites, prerequisite_count);

		/* A target with % is a pattern rule. */
		percent = strchr(targets[index], '%');
		if (percent != NULL) {
			rule = new_rule_target(targets[index]);
			define_pattern(rule, targets[index], prerequisites, prerequisite_count, double_colon);
			receivers[index] = rule;
			continue;
		}

		/* The target, named as a target now. */
		target = target_enter(targets[index]);
		target->is_target = 1;

		/* The first ordinary target of the makefiles is the default goal (a dot only with a slash). */
		slash = strchr(targets[index], '/');
		if (default_goal == NULL && !reading_builtin && !special) {
			if (targets[index][0] != '.' || slash != NULL)
				default_goal = make_strdup(targets[index]);
		}

		/* A double-colon rule is a rule of its own under the target. */
		if (double_colon) {
			target->is_double_colon = 1;
			rule = new_rule_target(targets[index]);
			rule->is_target = 1;
			rule->double_colon = target->double_colon;
			target->double_colon = rule;
			add_prerequisites(rule, prerequisites, prerequisite_count, 0);
			add_prerequisites(rule, order_only, order_only_count, 1);
			receivers[index] = rule;
			continue;
		}

		/* An ordinary rule adds to the target's prerequisites. */
		add_prerequisites(target, prerequisites, prerequisite_count, 0);
		add_prerequisites(target, order_only, order_only_count, 1);
		receivers[index] = target;
	}

	/* Succeeded. */
	return receivers;
}

/*
 * Gives the recipe of a rule that has ended to the targets that receive
 * it.  An empty recipe (no lines and no ;) changes nothing.  A later
 * recipe replaces an earlier one, with a warning unless the earlier was
 * built in.
 */
void
rule_set_recipe(
	struct target **targets,
	size_t count,
	struct recipe *recipe)
{
	size_t index;
	int present;
	int old_present;

	/* A rule without a recipe only added prerequisites. */
	present = 0;
	if (recipe->count > 0 || recipe->semicolon)
		present = 1;
	if (!present)
		return;

	/* Each target takes it. */
	for (index = 0; index < count; index++) {
		old_present = target_has_recipe(targets[index]);
		if (old_present && !targets[index]->builtin && !targets[index]->is_double_colon) {
			make_message("warning: overriding recipe for target '%s'", targets[index]->name);
			make_message("warning: ignoring old recipe for target '%s'", targets[index]->name);
		}

		/* The target takes the recipe. */
		targets[index]->recipe = recipe;
		targets[index]->builtin = reading_builtin;
	}
}

/*
 * Reports whether a target has a recipe (possibly an empty one given
 * after ;).
 */
int
target_has_recipe(
	const struct target *target)
{
	/* No recipe at all. */
	if (target->recipe == NULL)
		return 0;

	/* Succeeded: lines, or an explicit empty recipe. */
	if (target->recipe->count > 0 || target->recipe->semicolon)
		return 1;
	return 0;
}

/*
 * Returns a new, empty recipe that starts at a line of a makefile.
 */
struct recipe *
recipe_new(
	const char *file,
	long line)
{
	struct recipe *recipe;

	/* No lines yet. */
	recipe = make_malloc(sizeof(*recipe));
	memset(recipe, 0, sizeof(*recipe));
	recipe->file = make_strdup(file);
	recipe->line = line;

	/* Succeeded. */
	return recipe;
}

/*
 * Adds a line, and the makefile line it came from, to a recipe.
 */
void
recipe_add_line(
	struct recipe *recipe,
	const char *text,
	size_t length,
	long line)
{
	/* Room for one more, doubling. */
	if (recipe->count == recipe->capacity) {
		recipe->capacity = recipe->capacity * 2U + 4U;
		recipe->lines = make_realloc(recipe->lines, recipe->capacity * sizeof(*recipe->lines));
		recipe->line_numbers = make_realloc(recipe->line_numbers, recipe->capacity * sizeof(*recipe->line_numbers));
	}

	/* The line. */
	recipe->lines[recipe->count] = make_strndup(text, length);
	recipe->line_numbers[recipe->count] = line;
	recipe->count++;
}

/*
 * Marks the rules that follow as built in, or as the makefiles' own.
 */
void
rule_set_reading_builtin(
	int builtin)
{
	/* rule_define() and rule_set_recipe() read it. */
	reading_builtin = builtin;
}

/*
 * Finishes the rules after every makefile is read.  Nothing is left to
 * do: suffix rules are found when they are needed, against the final
 * .SUFFIXES.
 */
void
rule_finish_reading(
	void)
{
	/* The rules are complete as read. */
}

/*
 * Returns the default goal: the first ordinary target of the makefiles,
 * or NULL when there is none.
 */
const char *
rule_default_goal(
	void)
{
	/* What rule_define() recorded. */
	return default_goal;
}

/*
 * Returns the recipe of .DEFAULT, for a target that has no rule, or NULL.
 */
struct recipe *
rule_default_recipe(
	void)
{
	struct target *special;
	int present;

	/* .DEFAULT must have been given a recipe. */
	special = target_lookup(".DEFAULT");
	if (special == NULL)
		return NULL;
	present = target_has_recipe(special);
	if (!present)
		return NULL;

	/* Succeeded. */
	return special->recipe;
}

/*
 * Looks for an implicit rule to make a target that has no recipe: the
 * pattern rules first, then the suffix rules.  On success the target gets
 * the recipe, the stem ($*) and the new prerequisite ($<) first among
 * its prerequisites; returns 1.  Returns 0 when no rule applies.
 */
int
rule_find_implicit(
	struct target *target)
{
	int found;

	/* A pattern rule of the makefiles. */
	found = try_pattern_rules(target);
	if (found)
		return 1;

	/* A suffix rule. */
	found = try_suffix_rules(target);
	if (found)
		return 1;

	/* None applies. */
	return 0;
}

/*
 * Assigns a variable of some targets' own.  A += on a target that has no
 * value of its own adds, when it is used, to the value the target
 * inherits.
 */
void
rule_define_variable(
	char **targets,
	size_t target_count,
	const char *name,
	const char *value,
	enum assign_kind kind,
	enum variable_origin origin,
	const struct expansion *context)
{
	struct variable_scope own;
	struct target *target;
	struct variable *variable;
	size_t index;
	size_t length;

	/* Each target, with a table of its own. */
	length = strlen(name);
	for (index = 0; index < target_count; index++) {
		target = target_enter(targets[index]);
		if (target->variables == NULL)
			target->variables = variable_new_set();

		/* += with nothing of its own marks the variable to add to the inherited one. */
		variable = NULL;
		if (kind == ASSIGN_APPEND) {
			own.set = target->variables;
			own.parent = NULL;
			variable = variable_lookup(&own, name, length);
			if (variable == NULL) {
				variable = variable_set_value(target->variables, name, value, FLAVOR_RECURSIVE, origin);
				variable->append = 1;
				continue;
			}
		}

		/* Any other assignment in the target's table. */
		variable_assign(target->variables, name, value, kind, origin, context);
	}
}

/*
 * Carries out a vpath directive: with directories, adds them for the
 * pattern; without, forgets the pattern (or, with no pattern, every
 * pattern).
 */
void
rule_vpath_set(
	const char *pattern,
	char **directories,
	size_t count)
{
	struct vpath_entry **link;
	struct vpath_entry *entry;
	size_t index;
	int compare;

	/* Forgetting: every entry, or those of the pattern. */
	if (count == 0) {
		link = &vpath_entries;
		while (*link != NULL) {
			entry = *link;
			compare = 0;
			if (pattern != NULL)
				compare = strcmp(entry->pattern, pattern);
			if (compare != 0) {
				link = &entry->next;
				continue;
			}

			/* The entry is taken out and freed. */
			*link = entry->next;
			for (index = 0; index < entry->count; index++)
				free(entry->directories[index]);
			free(entry->directories);
			free(entry->pattern);
			free(entry);
		}

		/* The patterns are forgotten. */
		return;
	}

	/* A new entry at the end of the list. */
	entry = make_malloc(sizeof(*entry));
	entry->next = NULL;
	entry->pattern = make_strdup(pattern);
	entry->directories = make_malloc(count * sizeof(*entry->directories));
	for (index = 0; index < count; index++)
		entry->directories[index] = make_strdup(directories[index]);
	entry->count = count;
	for (link = &vpath_entries; *link != NULL; link = &(*link)->next)
		continue;
	*link = entry;
}

/*
 * Looks for a file that is not where its name says in the vpath
 * directories whose pattern matches it, then in $(VPATH).  Returns the
 * allocated path where it is, or NULL.  An absolute name is not looked
 * for.
 */
char *
rule_vpath_search(
	const char *name)
{
	struct vpath_entry *entry;
	struct variable *vpath;
	struct expansion context;
	const char *cursor;
	const char *start;
	char *directories;
	char *found;
	size_t index;
	int matched;

	/* An absolute name is where it says. */
	if (name[0] == '/')
		return NULL;

	/* The vpath directives whose pattern matches, in order. */
	for (entry = vpath_entries; entry != NULL; entry = entry->next) {
		matched = vpath_matches(entry->pattern, name);
		if (!matched)
			continue;
		for (index = 0; index < entry->count; index++) {
			found = search_directory(entry->directories[index], strlen(entry->directories[index]), name);
			if (found != NULL)
				return found;
		}
	}

	/* $(VPATH): directories separated by : or blanks. */
	vpath = variable_lookup(&make_global_scope, "VPATH", 5);
	if (vpath == NULL)
		return NULL;
	context.scope = &make_global_scope;
	context.automatic = NULL;
	context.file = NULL;
	context.line = 0;
	directories = expand(&context, vpath->value);
	cursor = directories;
	found = NULL;
	while (*cursor != '\0' && found == NULL) {
		while (*cursor == ':' || *cursor == ' ' || *cursor == '\t')
			cursor++;
		start = cursor;
		while (*cursor != '\0' && *cursor != ':' && *cursor != ' ' && *cursor != '\t')
			cursor++;
		if (cursor > start)
			found = search_directory(start, (size_t)(cursor - start), name);
	}

	/* The directories are done with. */
	free(directories);

	/* Succeeded or not: where it was found. */
	return found;
}

/* Returns the hash chain of a target's name. */
static unsigned
hash_name(
	const char *name)
{
	unsigned hash;
	const char *cursor;

	/* A multiplicative hash of the bytes. */
	hash = 5381U;
	for (cursor = name; *cursor != '\0'; cursor++)
		hash = hash * 33U + (unsigned char)*cursor;

	/* Succeeded: the chain. */
	return hash % MAKE_HASH_SIZE;
}

/* Reports whether a name is a special target, which is not a goal. */
static int
is_special(
	const char *name)
{
	static const char *const names[] = {
		".PHONY", ".SUFFIXES", ".DEFAULT", ".PRECIOUS", ".IGNORE", ".SILENT",
		".POSIX", ".NOTPARALLEL", ".MAKE", ".NOEXPORT", ".INTERMEDIATE",
		".SECONDARY", ".DELETE_ON_ERROR", ".EXPORT_ALL_VARIABLES",
		".LOW_RESOLUTION_TIME", ".ONESHELL", ".WAIT", ".NOTINTERMEDIATE"
	};
	size_t index;
	int compare;

	/* One of the names. */
	for (index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
		compare = strcmp(name, names[index]);
		if (compare == 0)
			return 1;
	}

	/* Not special. */
	return 0;
}

/*
 * Carries out a special target: .PHONY, .PRECIOUS, .IGNORE and .SILENT
 * mark their prerequisites (.IGNORE and .SILENT alone mark everything),
 * .SUFFIXES adds suffixes (alone, clears them), .POSIX and
 * .EXPORT_ALL_VARIABLES set a mode, and .NOTPARALLEL makes recipes run
 * one at a time (with prerequisites, only those targets' prerequisites
 * are made one at a time).  The others are accepted and do nothing more.
 */
static void
define_special(
	const char *name,
	char **prerequisites,
	size_t prerequisite_count)
{
	struct target *target;
	size_t index;
	int compare;

	/* .SUFFIXES: alone it empties the list; otherwise it adds to it. */
	compare = strcmp(name, ".SUFFIXES");
	if (compare == 0) {
		if (prerequisite_count == 0) {
			for (index = 0; index < suffix_count; index++)
				free(suffixes[index]);
			suffix_count = 0;
			return;
		}

		/* Otherwise each suffix is added at the end. */
		for (index = 0; index < prerequisite_count && suffix_count < RULE_SUFFIX_MAX; index++) {
			suffixes[suffix_count] = make_strdup(prerequisites[index]);
			suffix_count++;
		}

		/* The suffixes are added. */
		return;
	}

	/* The modes. */
	compare = strcmp(name, ".POSIX");
	if (compare == 0) {
		make_posix = 1;
		return;
	}

	/* .EXPORT_ALL_VARIABLES. */
	compare = strcmp(name, ".EXPORT_ALL_VARIABLES");
	if (compare == 0) {
		variable_export_all();
		return;
	}

	/* .NOTPARALLEL alone makes the whole run serial. */
	compare = strcmp(name, ".NOTPARALLEL");
	if (compare == 0 && prerequisite_count == 0) {
		make_not_parallel = 1;
		return;
	}

	/* .IGNORE and .SILENT without prerequisites cover every target. */
	compare = strcmp(name, ".IGNORE");
	if (compare == 0 && prerequisite_count == 0)
		make_ignore_all = 1;
	compare = strcmp(name, ".SILENT");
	if (compare == 0 && prerequisite_count == 0)
		make_silent_all = 1;

	/* The marks on each prerequisite. */
	for (index = 0; index < prerequisite_count; index++) {
		target = target_enter(prerequisites[index]);
		compare = strcmp(name, ".PHONY");
		if (compare == 0)
			target->phony = 1;
		compare = strcmp(name, ".PRECIOUS");
		if (compare == 0)
			target->precious = 1;
		compare = strcmp(name, ".IGNORE");
		if (compare == 0)
			target->ignore_errors = 1;
		compare = strcmp(name, ".SILENT");
		if (compare == 0)
			target->silent = 1;
		compare = strcmp(name, ".NOTPARALLEL");
		if (compare == 0)
			target->not_parallel = 1;
	}
}

/* Returns a new target that is in no table (a rule of a double-colon target, or a pattern rule's holder). */
static struct target *
new_rule_target(
	const char *name)
{
	struct target *target;

	/* Nothing known yet. */
	target = make_malloc(sizeof(*target));
	memset(target, 0, sizeof(*target));
	target->name = make_strdup(name);
	target->dependencies_tail = &target->dependencies;

	/* Succeeded. */
	return target;
}

/*
 * Adds the named prerequisites to a target, entering each as a target.
 * .WAIT among them is not a prerequisite: it marks the next one to wait
 * for those before it.
 */
static void
add_prerequisites(
	struct target *target,
	char **names,
	size_t count,
	int order_only)
{
	struct dependency *dependency;
	struct target *prerequisite;
	size_t index;
	int wait;
	int compare;

	/* Each name, in order. */
	wait = 0;
	for (index = 0; index < count; index++) {
		compare = strcmp(names[index], ".WAIT");
		if (compare == 0) {
			wait = 1;
			continue;
		}

		/* The prerequisite, carrying a .WAIT that came before it. */
		prerequisite = target_enter(names[index]);
		dependency = target_add_dependency(target, prerequisite, order_only);
		dependency->wait = wait;
		wait = 0;
	}
}

/* Adds a pattern rule, whose recipe will go to its holder. */
static void
define_pattern(
	struct target *holder,
	const char *pattern,
	char **prerequisites,
	size_t count,
	int double_colon)
{
	struct pattern_rule *rule;
	size_t index;

	/* The patterns, copied. */
	rule = make_malloc(sizeof(*rule));
	memset(rule, 0, sizeof(*rule));
	rule->target_pattern = make_strdup(pattern);
	rule->prerequisites = make_malloc((count + 1U) * sizeof(*rule->prerequisites));
	for (index = 0; index < count; index++)
		rule->prerequisites[index] = make_strdup(prerequisites[index]);
	rule->prerequisite_count = count;
	rule->holder = holder;
	rule->terminal = double_colon;

	/* The rule joins the list. */
	*pattern_tail = rule;
	pattern_tail = &rule->next;
}

/*
 * Tries each pattern rule with a recipe whose target pattern matches the
 * target and whose prerequisites exist or ought to; returns 1 when one is
 * used.
 */
static int
try_pattern_rules(
	struct target *target)
{
	struct pattern_rule *rule;
	struct target *prerequisite;
	char **names;
	char *stem;
	char *directory;
	size_t index;
	int matched;
	int present;
	int exists;
	int usable;

	/* Each rule in the order it was read. */
	for (rule = pattern_rules; rule != NULL; rule = rule->next) {
		present = target_has_recipe(rule->holder);
		if (!present)
			continue;
		matched = pattern_stem(rule->target_pattern, target->name, &stem, &directory);
		if (!matched)
			continue;

		/* Every prerequisite, named from the stem, must exist or ought to. */
		names = make_malloc((rule->prerequisite_count + 1U) * sizeof(*names));
		usable = 1;
		for (index = 0; index < rule->prerequisite_count; index++) {
			names[index] = apply_stem(rule->prerequisites[index], stem, directory);
			exists = ought_to_exist(names[index]);
			if (!exists)
				usable = 0;
		}

		/* The rule is used: its recipe, the stem, and its prerequisites before the target's own. */
		if (usable) {
			target->recipe = rule->holder->recipe;
			target->stem = make_malloc(strlen(directory) + strlen(stem) + 1U);
			strcpy(target->stem, directory);
			strcat(target->stem, stem);
			for (index = rule->prerequisite_count; index > 0; index--) {
				prerequisite = target_enter(names[index - 1U]);
				prepend_dependency(target, prerequisite);
			}

			/* The first of them is $<. */
			if (rule->prerequisite_count > 0)
				target->implicit_first = target_lookup(names[0]);
		}

		/* The names and the stem are done with. */
		for (index = 0; index < rule->prerequisite_count; index++)
			free(names[index]);
		free(names);
		free(stem);
		free(directory);
		if (usable)
			return 1;
	}

	/* None applies. */
	return 0;
}

/*
 * Matches a name against a pattern with one %.  A pattern without a
 * slash matches the file part of the name, and the directory part is
 * returned apart.  Returns 1 with the stem and the directory (both
 * allocated), or 0.
 */
static int
pattern_stem(
	const char *pattern,
	const char *name,
	char **stem,
	char **directory)
{
	const char *percent;
	const char *base;
	const char *slash;
	size_t prefix_length;
	size_t suffix_length;
	size_t length;
	int compare;

	/* Without a slash in the pattern, the directory is set apart. */
	base = name;
	slash = strchr(pattern, '/');
	if (slash == NULL) {
		slash = strrchr(name, '/');
		if (slash != NULL)
			base = slash + 1;
	}

	/* The text before and after the %. */
	percent = strchr(pattern, '%');
	prefix_length = (size_t)(percent - pattern);
	suffix_length = strlen(percent + 1);
	length = strlen(base);
	if (length < prefix_length + suffix_length)
		return 0;
	compare = strncmp(base, pattern, prefix_length);
	if (compare != 0)
		return 0;
	compare = strcmp(base + length - suffix_length, percent + 1);
	if (compare != 0)
		return 0;

	/* Succeeded: what the % matched, and the directory set apart. */
	*stem = make_strndup(base + prefix_length, length - prefix_length - suffix_length);
	*directory = make_strndup(name, (size_t)(base - name));
	return 1;
}

/* Returns a prerequisite pattern with its % replaced by the stem, after the directory. */
static char *
apply_stem(
	const char *pattern,
	const char *stem,
	const char *directory)
{
	struct buffer name;
	const char *percent;

	/* The directory, then the pattern with the stem in place of %. */
	memset(&name, 0, sizeof(name));
	buffer_add_string(&name, directory);
	percent = strchr(pattern, '%');
	if (percent == NULL) {
		buffer_add_string(&name, pattern);
	} else {
		buffer_add(&name, pattern, (size_t)(percent - pattern));
		buffer_add_string(&name, stem);
		buffer_add_string(&name, percent + 1);
	}

	/* Succeeded. */
	return buffer_finish(&name);
}

/*
 * Tries the suffix rules as POSIX orders them: for a target that ends
 * with a known suffix, each other known suffix in order (a double-suffix
 * rule .s.t); then, for any target, each known suffix for a
 * single-suffix rule (.s).  A rule applies when the prerequisite exists
 * or ought to.  Returns 1 when one is used.
 */
static int
try_suffix_rules(
	struct target *target)
{
	struct target *rule;
	struct buffer prerequisite;
	char *stem;
	size_t target_suffix;
	size_t source;
	size_t length;
	int ends;
	int exists;
	int used;

	/* Double-suffix rules, for each known suffix the target ends with. */
	for (target_suffix = 0; target_suffix < suffix_count; target_suffix++) {
		ends = ends_with(target->name, suffixes[target_suffix]);
		if (!ends)
			continue;
		length = strlen(target->name) - strlen(suffixes[target_suffix]);
		stem = make_strndup(target->name, length);

		/* Each source suffix in order. */
		for (source = 0; source < suffix_count; source++) {
			rule = suffix_rule(suffixes[source], suffixes[target_suffix]);
			if (rule == NULL)
				continue;
			memset(&prerequisite, 0, sizeof(prerequisite));
			buffer_add_string(&prerequisite, stem);
			buffer_add_string(&prerequisite, suffixes[source]);
			exists = ought_to_exist(prerequisite.text);
			used = 0;
			if (exists)
				used = use_rule(target, rule, prerequisite.text, stem);
			free(prerequisite.text);
			if (used) {
				free(stem);
				return 1;
			}
		}

		/* The stem is done with. */
		free(stem);
	}

	/* Single-suffix rules: the target's name with each suffix added. */
	for (source = 0; source < suffix_count; source++) {
		rule = suffix_rule(suffixes[source], "");
		if (rule == NULL)
			continue;
		memset(&prerequisite, 0, sizeof(prerequisite));
		buffer_add_string(&prerequisite, target->name);
		buffer_add_string(&prerequisite, suffixes[source]);
		exists = ought_to_exist(prerequisite.text);
		used = 0;
		if (exists)
			used = use_rule(target, rule, prerequisite.text, target->name);
		free(prerequisite.text);
		if (used)
			return 1;
	}

	/* None applies. */
	return 0;
}

/* Gives a target a suffix rule's recipe, the stem and the prerequisite first; returns 1. */
static int
use_rule(
	struct target *target,
	struct target *rule,
	const char *prerequisite,
	const char *stem)
{
	struct target *source;

	/* The recipe and the stem. */
	target->recipe = rule->recipe;
	target->stem = make_strdup(stem);

	/* The prerequisite, first among the target's. */
	source = target_enter(prerequisite);
	prepend_dependency(target, source);
	target->implicit_first = source;

	/* Succeeded. */
	return 1;
}

/* Puts a prerequisite at the head of a target's list. */
static void
prepend_dependency(
	struct target *target,
	struct target *prerequisite)
{
	struct dependency *dependency;

	/* A new entry before the others; an empty list gets its tail too. */
	dependency = make_malloc(sizeof(*dependency));
	dependency->target = prerequisite;
	dependency->order_only = 0;
	dependency->wait = 0;
	dependency->dropped = 0;
	dependency->next = target->dependencies;
	if (target->dependencies == NULL)
		target->dependencies_tail = &dependency->next;
	target->dependencies = dependency;
}

/*
 * Reports whether a file exists or ought to exist: it is there, or some
 * rule names it as a target.
 */
static int
ought_to_exist(
	const char *name)
{
	struct target *target;
	struct stat status;
	char *found;
	int error;

	/* A target of some rule. */
	target = target_lookup(name);
	if (target != NULL && target->is_target)
		return 1;

	/* A file that is there. */
	error = stat(name, &status);
	if (error == 0)
		return 1;

	/* A file a vpath directory holds. */
	found = rule_vpath_search(name);
	if (found != NULL) {
		free(found);
		return 1;
	}

	/* Neither. */
	return 0;
}

/* Returns directory/name when that file exists, allocated, or NULL. */
static char *
search_directory(
	const char *directory,
	size_t length,
	const char *name)
{
	struct stat status;
	struct buffer path;
	int error;

	/* The joined path. */
	memset(&path, 0, sizeof(path));
	buffer_add(&path, directory, length);
	buffer_add_char(&path, '/');
	buffer_add_string(&path, name);

	/* It must be there. */
	error = stat(path.text, &status);
	if (error != 0) {
		free(path.text);
		return NULL;
	}

	/* Succeeded. */
	return buffer_finish(&path);
}

/* Reports whether a name matches a vpath pattern (one % for any text; without %, the name itself). */
static int
vpath_matches(
	const char *pattern,
	const char *name)
{
	const char *percent;
	size_t prefix_length;
	size_t suffix_length;
	size_t length;
	int compare;

	/* Without % the pattern is the name. */
	percent = strchr(pattern, '%');
	if (percent == NULL) {
		compare = strcmp(pattern, name);
		if (compare == 0)
			return 1;
		return 0;
	}

	/* The text before and after the % at the two ends of the name. */
	prefix_length = (size_t)(percent - pattern);
	suffix_length = strlen(percent + 1);
	length = strlen(name);
	if (length < prefix_length + suffix_length)
		return 0;
	compare = strncmp(name, pattern, prefix_length);
	if (compare != 0)
		return 0;
	compare = strcmp(name + length - suffix_length, percent + 1);
	if (compare != 0)
		return 0;

	/* Succeeded. */
	return 1;
}

/* Reports whether a name ends with a suffix and is longer than it. */
static int
ends_with(
	const char *name,
	const char *suffix)
{
	size_t length;
	size_t suffix_length;
	int compare;

	/* The name must hold more than the suffix. */
	length = strlen(name);
	suffix_length = strlen(suffix);
	if (length <= suffix_length)
		return 0;

	/* Succeeded when the end matches. */
	compare = strcmp(name + length - suffix_length, suffix);
	if (compare == 0)
		return 1;
	return 0;
}

/* Returns the suffix rule of two suffixes (the second empty for a single-suffix rule) when it has a recipe, or NULL. */
static struct target *
suffix_rule(
	const char *first,
	const char *second)
{
	struct target *rule;
	char *name;
	int present;

	/* The rule's name is the two suffixes together. */
	name = make_malloc(strlen(first) + strlen(second) + 1U);
	strcpy(name, first);
	strcat(name, second);
	rule = target_lookup(name);
	free(name);
	if (rule == NULL)
		return NULL;

	/* A suffix rule with prerequisites is an ordinary target (POSIX). */
	present = target_has_recipe(rule);
	if (!present || rule->dependencies != NULL)
		return NULL;

	/* Succeeded. */
	return rule;
}
