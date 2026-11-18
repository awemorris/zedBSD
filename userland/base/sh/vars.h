/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Shell variables: the table of names and values with their export and
 * read-only attributes, the local variables of functions, and the
 * environment built from the table for a command.
 */

#ifndef KERN_USERLAND_SH_VARS_H
#define KERN_USERLAND_SH_VARS_H

#include <stddef.h>

/* The attributes of a variable. */
#define SH_VAR_EXPORT	0x01
#define SH_VAR_READONLY	0x02

/*
 * The attributes of declare (bash): a value assigned is evaluated as an
 * arithmetic expression, or made lower or upper case.
 */
#define SH_VAR_INTEGER	0x04
#define SH_VAR_LOWER	0x08
#define SH_VAR_UPPER	0x10

/* Reports whether a string is a name (XBD 3.216). */
int sh_var_name(const char *);

/* Reports how long the name at the start of a string is. */
size_t sh_var_name_length(const char *);

/* Returns a variable's value, or NULL when it is unset. */
const char *sh_var_get(const char *);

/* Returns a variable's attributes, or -1 when there is no such variable. */
int sh_var_flags(const char *);

/*
 * Sets a variable, adding the attributes given.  Returns -1 when it is
 * read-only (and changes nothing), 0 otherwise.
 */
int sh_var_set(const char *, const char *, int);

/* Sets a variable from "name=value", with the same result as sh_var_set. */
int sh_var_set_assignment(const char *, int);

/* Unsets a variable.  Returns -1 when it is read-only. */
int sh_var_unset(const char *);

/* Adds attributes to a variable, creating it unset when there is none. */
void sh_var_add_flags(const char *, int);

/* Takes the declare attributes (not read-only) off a variable. */
void sh_var_remove_flags(const char *, int);

/* Imports the environment the shell was started with. */
void sh_var_import_environment(char **);

/* Takes the export attribute off every variable. */
void sh_var_clear_exports(void);

/*
 * Builds the environment of a command: a NULL-ended array, freed by
 * sh_var_environment_free.
 */
char **sh_var_environment(void);
void sh_var_environment_free(char **);

/*
 * Prints the variables with an attribute, as the given command would read
 * them back (export -p, readonly -p), or every set variable (set).
 */
void sh_var_print(int, const char *);

/*
 * Prints variables as declare -p does: one name (returning 0 when there is
 * no such variable), or every variable with all the attributes given.
 */
int sh_var_print_declare(const char *);
void sh_var_print_declared(int);

/* Local variables of functions. */
void sh_var_local_push(void);
void sh_var_local_pop(void);
int sh_var_local_depth(void);
void sh_var_local_unwind(int);
int sh_var_make_local(const char *);
void sh_var_local_options(void);

/* Tells the table which variables the shell itself watches. */
void sh_var_hook(const char *, void (*)(const char *));

/* Reports whether the shell watches a variable (sh_var_hook). */
int sh_var_hooked(const char *);

#endif
