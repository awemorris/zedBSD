/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Returns user identity (POSIX XCU id).
 *
 *	id [user]
 *	id -G [-n] [user]
 *	id -g [-nr] [user]
 *	id -u [-nr] [user]
 *
 * As on other systems, several users may be named; each gets its line.
 * Without a user operand the IDs are those of the process: real and
 * effective user and group IDs and the supplementary groups.  With one,
 * they are the user's from the user and group databases.  The default
 * output is
 *
 *	uid=%u(%s) gid=%u(%s)[ euid=%u(%s)][ egid=%u(%s)][ groups=%u(%s),...]
 *
 * where a name that cannot be found is left out with its parentheses.
 * The group list starts with the real group, then the effective group when
 * it differs, then the other supplementary groups.  -u, -g and -G write
 * only the effective user, the effective group or the group list; -r
 * chooses the real ID and -n the name.
 */

#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* The most groups the list holds. */
#define ID_GROUPS_MAX 256

/* Only one field is written: nothing chosen, -u, -g or -G. */
#define ID_SHOW_ALL 0
#define ID_SHOW_USER 1
#define ID_SHOW_GROUP 2
#define ID_SHOW_GROUPS 3

/*
 * The identity id reports.
 *
 * One instance lives for the run, filled from the process or from the
 * user operand; groups holds count entries without duplicates.
 */
struct identity {
	uid_t real_user;
	uid_t effective_user;
	gid_t real_group;
	gid_t effective_group;
	gid_t groups[ID_GROUPS_MAX];
	int count;
};

static int read_options(int argc, char **argv, int *show, int *names, int *real);
static int report_identity(const char *user, int show, int names, int real);
static int read_process(struct identity *identity);
static int read_user(const char *name, struct identity *identity);
static void add_group(struct identity *identity, gid_t group);
static int write_default(const struct identity *identity);
static int write_group_list(const struct identity *identity, int names);
static int write_user(uid_t user, int name);
static int write_group(gid_t group, int name);
static void usage(void);

/*
 * Runs id.
 */
int
main(
	int argc,
	char **argv)
{
	int first;
	int show;
	int names;
	int real;
	int index;
	int status;
	int failed;

	/* Reads the options; the users follow them. */
	first = read_options(argc, argv, &show, &names, &real);

	/* Without a user, the process; otherwise each user in turn. */
	failed = 0;
	if (first >= argc) {
		status = report_identity(NULL, show, names, real);
		if (status != 0)
			failed = 1;
	}

	/* Each user named gets a line of its own. */
	for (index = first; index < argc; index++) {
		status = report_identity(argv[index], show, names, real);
		if (status != 0)
			failed = 1;
	}

	/* Pushes the output out; a write that failed is an error. */
	fflush(stdout);
	status = ferror(stdout);
	if (status) {
		fprintf(stderr, "id: write error\n");
		return 1;
	}

	/* An unknown user or a missing name fails the run. */
	if (failed)
		return 1;

	/* Succeeded: every identity was written. */
	return 0;
}

/*
 * Writes one line about the process (user NULL) or about a user.
 * Returns -1 for an unknown user or a name that could not be found.
 */
static int
report_identity(
	const char *user,
	int show,
	int names,
	int real)
{
	struct identity identity;
	int status;

	/* Collects the identity of the process or of the user. */
	if (user != NULL)
		status = read_user(user, &identity);
	else
		status = read_process(&identity);
	if (status != 0)
		return -1;

	/* Writes what was asked for. */
	if (show == ID_SHOW_USER) {
		if (real)
			status = write_user(identity.real_user, names);
		else
			status = write_user(identity.effective_user, names);
	} else if (show == ID_SHOW_GROUP) {
		if (real)
			status = write_group(identity.real_group, names);
		else
			status = write_group(identity.effective_group, names);
	} else if (show == ID_SHOW_GROUPS) {
		status = write_group_list(&identity, names);
	} else {
		status = write_default(&identity);
	}

	/* Ends the line. */
	putchar('\n');

	/* A name that could not be found fails the line. */
	if (status != 0)
		return -1;

	/* Succeeded: the line was written. */
	return 0;
}

/*
 * Reads the options and returns the index of the user operand.  -n and
 * -r need -u, -g or -G, and only one of those may be given.
 */
static int
read_options(
	int argc,
	char **argv,
	int *show,
	int *names,
	int *real)
{
	int option;
	int chosen;

	/* Nothing asked for yet. */
	*show = ID_SHOW_ALL;
	*names = 0;
	*real = 0;
	chosen = 0;

	/* Reads the options. */
	for (;;) {
		option = getopt(argc, argv, "Ggnru");
		if (option == -1)
			break;

		/* Records what the option asks for. */
		switch (option) {
		case 'G':
			*show = ID_SHOW_GROUPS;
			chosen++;
			break;
		case 'g':
			*show = ID_SHOW_GROUP;
			chosen++;
			break;
		case 'u':
			*show = ID_SHOW_USER;
			chosen++;
			break;
		case 'n':
			*names = 1;
			break;
		case 'r':
			*real = 1;
			break;
		default:
			usage();
			break;
		}
	}

	/* Only one field can be written. */
	if (chosen > 1) {
		fprintf(stderr, "id: -G, -g and -u exclude each other\n");
		usage();
	}

	/* -n and -r change a field; the default output has several. */
	if (*show == ID_SHOW_ALL && (*names || *real)) {
		fprintf(stderr, "id: -n and -r need -G, -g or -u\n");
		usage();
	}

	/* Reports where the operand starts. */
	return optind;
}

/* Collects the IDs and the supplementary groups of the process. */
static int
read_process(
	struct identity *identity)
{
	gid_t groups[ID_GROUPS_MAX];
	int count;
	int index;

	/* The real and effective IDs. */
	identity->real_user = getuid();
	identity->effective_user = geteuid();
	identity->real_group = getgid();
	identity->effective_group = getegid();

	/* The real group, then the effective one, start the list. */
	identity->count = 0;
	add_group(identity, identity->real_group);
	add_group(identity, identity->effective_group);

	/* Then the supplementary groups. */
	count = getgroups(ID_GROUPS_MAX, groups);
	if (count < 0) {
		fprintf(stderr, "id: getgroups: %s\n", strerror(errno));
		return -1;
	}

	/* Adds each; the real and effective groups are not repeated. */
	for (index = 0; index < count; index++)
		add_group(identity, groups[index]);

	/* Succeeded: the process identity. */
	return 0;
}

/*
 * Collects the IDs of a user from the user database and the groups that
 * list the user from the group database.  A user that has no name may be
 * given by a numeric user ID.
 */
static int
read_user(
	const char *name,
	struct identity *identity)
{
	struct passwd *user;
	struct group *group;
	char *end;
	unsigned long number;
	char **member;
	int compare;

	/* Finds the user by name, then by number. */
	user = getpwnam(name);
	if (user == NULL && name[0] >= '0' && name[0] <= '9') {
		number = strtoul(name, &end, 10);
		if (*end == '\0')
			user = getpwuid((uid_t)number);
	}

	/* A user found neither way is an error. */
	if (user == NULL) {
		fprintf(stderr, "id: '%s': no such user\n", name);
		return -1;
	}

	/* The user's IDs; real and effective are the same. */
	identity->real_user = user->pw_uid;
	identity->effective_user = user->pw_uid;
	identity->real_group = user->pw_gid;
	identity->effective_group = user->pw_gid;
	identity->count = 0;
	add_group(identity, user->pw_gid);

	/* Adds every group whose member list names the user. */
	setgrent();
	for (;;) {
		group = getgrent();
		if (group == NULL)
			break;

		/* Looks for the user among the members. */
		for (member = group->gr_mem; member != NULL && *member != NULL; member++) {
			compare = strcmp(*member, user->pw_name);
			if (compare == 0) {
				add_group(identity, group->gr_gid);
				break;
			}
		}
	}

	/* Closes the group database. */
	endgrent();

	/* Succeeded: the user's identity. */
	return 0;
}

/* Adds a group to the list unless it is there already or the list is full. */
static void
add_group(
	struct identity *identity,
	gid_t group)
{
	int index;

	/* A group already listed is not listed again. */
	for (index = 0; index < identity->count; index++) {
		if (identity->groups[index] == group)
			return;
	}

	/* A full list keeps what it has. */
	if (identity->count >= ID_GROUPS_MAX)
		return;

	/* Appends the group. */
	identity->groups[identity->count] = group;
	identity->count++;
}

/*
 * Writes the default output.  Returns 0; a name that is missing is simply
 * left out.
 */
static int
write_default(
	const struct identity *identity)
{
	int index;

	/* The real user and group. */
	printf("uid=");
	write_user(identity->real_user, -1);
	printf(" gid=");
	write_group(identity->real_group, -1);

	/* The effective IDs, when they differ. */
	if (identity->effective_user != identity->real_user) {
		printf(" euid=");
		write_user(identity->effective_user, -1);
	}

	/* The effective group, when it differs. */
	if (identity->effective_group != identity->real_group) {
		printf(" egid=");
		write_group(identity->effective_group, -1);
	}

	/* The group list. */
	printf(" groups=");
	for (index = 0; index < identity->count; index++) {
		if (index > 0)
			putchar(',');
		write_group(identity->groups[index], -1);
	}

	/* Succeeded: the output was written. */
	return 0;
}

/*
 * Writes the group list separated by spaces, as numbers or names.
 * Returns -1 when a name could not be found.
 */
static int
write_group_list(
	const struct identity *identity,
	int names)
{
	int index;
	int status;
	int failed;

	/* Writes each group. */
	failed = 0;
	for (index = 0; index < identity->count; index++) {
		if (index > 0)
			putchar(' ');
		status = write_group(identity->groups[index], names);
		if (status != 0)
			failed = 1;
	}

	/* Reports a missing name. */
	if (failed)
		return -1;

	/* Succeeded: the list was written. */
	return 0;
}

/*
 * Writes a user ID: the number (name 0), the name (name 1), or the number
 * and the name in parentheses (name -1).  A missing name is written as the
 * number; with name 1 that is reported with -1.
 */
static int
write_user(
	uid_t user,
	int name)
{
	struct passwd *entry;

	/* The number alone. */
	if (name == 0) {
		printf("%lu", (unsigned long)user);
		return 0;
	}

	/* Looks the name up. */
	entry = getpwuid(user);

	/* The number with its name in parentheses, when there is one. */
	if (name < 0) {
		printf("%lu", (unsigned long)user);
		if (entry != NULL)
			printf("(%s)", entry->pw_name);
		return 0;
	}

	/* The name, or the number when there is none. */
	if (entry == NULL) {
		fprintf(stderr, "id: cannot find name for user ID %lu\n", (unsigned long)user);
		printf("%lu", (unsigned long)user);
		return -1;
	}

	/* Succeeded: the name. */
	printf("%s", entry->pw_name);
	return 0;
}

/* Writes a group ID in the forms write_user() describes for users. */
static int
write_group(
	gid_t group,
	int name)
{
	struct group *entry;

	/* The number alone. */
	if (name == 0) {
		printf("%lu", (unsigned long)group);
		return 0;
	}

	/* Looks the name up. */
	entry = getgrgid(group);

	/* The number with its name in parentheses, when there is one. */
	if (name < 0) {
		printf("%lu", (unsigned long)group);
		if (entry != NULL)
			printf("(%s)", entry->gr_name);
		return 0;
	}

	/* The name, or the number when there is none. */
	if (entry == NULL) {
		fprintf(stderr, "id: cannot find name for group ID %lu\n", (unsigned long)group);
		printf("%lu", (unsigned long)group);
		return -1;
	}

	/* Succeeded: the name. */
	printf("%s", entry->gr_name);
	return 0;
}

/* Writes the usage message and exits with an error status. */
static void
usage(void)
{
	/* Names the POSIX forms. */
	fprintf(stderr,
		"usage: id [user]\n"
		"       id -G [-n] [user]\n"
		"       id -g [-nr] [user]\n"
		"       id -u [-nr] [user]\n");
	exit(1);
}
