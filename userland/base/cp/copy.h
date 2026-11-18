/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the file-hierarchy copy that cp uses and that mv uses to move
 * across file systems.
 */

#ifndef USERLAND_BASE_CP_COPY_H
#define USERLAND_BASE_CP_COPY_H

#include <stddef.h>
#include <sys/stat.h>

/* Symbolic links are copied as links (-P). */
#define COPY_FOLLOW_NONE 0

/* Symbolic links named as operands are followed (-H). */
#define COPY_FOLLOW_OPERANDS 1

/* Every symbolic link is followed (-L). */
#define COPY_FOLLOW_ALL 2

/*
 * One copied file that other names of the same source file are linked to.
 *
 * With -a, the first copy of a file with several hard links is remembered
 * here, and later names of the same source file become links to it.  The
 * records live until copy_finish().
 */
struct copy_link {
	struct copy_link *next;
	struct stat source;
	struct stat destination;
	char *path;
};

/*
 * How one run of cp or mv copies.
 *
 * The caller fills the policy fields before the first copy_operand() and
 * zeroes the rest; verbose (GNU's -v) writes each copy as it is made.  The
 * hard-link records and the report descriptor are owned by the run and
 * released by copy_finish().
 */
struct copy_options {
	const char *program;
	int follow;
	int recursive;
	int interactive;
	int force;
	int exclusive;
	int conflict_fails;
	int attributes_only;
	int preserve_mode;
	int preserve_owner;
	int preserve_times;
	int preserve_links;
	int verbose;
	int report_descriptor;
	int report_failed;
	struct copy_link *links;
};

void copy_options_init(struct copy_options *options, const char *program);
int copy_operand(struct copy_options *options, const char *source, const char *destination);
int copy_report_open(struct copy_options *options, const char *path, int count, char **operands);
int copy_report_record(struct copy_options *options, const char *source, int directory);
int copy_report_close(struct copy_options *options, const char *path, int failed);
void copy_finish(struct copy_options *options);
int copy_ask(const char *program, const char *question, const char *path);
const char *copy_leaf(const char *path, char *buffer, size_t size);

#endif
