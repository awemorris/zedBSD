/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The people's accounts as Settings' Users shows them (ws188-p002, moved
 * from Settings' page-users.c of ws089-p026): the accounts with a user ID
 * from 1000 (not nobody's) and a shell to log in with, each with its full
 * name and whether it is an administrator or may control Wi-Fi, and the
 * account of the compositor's own user (the client's, since the
 * compositor shows its system extension only to its own user), with its
 * home.
 *
 * The passwd and group databases are POSIX and the same on zedBSD, Linux
 * and FreeBSD; each operating system names its administrators' groups
 * (users-*.c, account-zedbsd.c).  The groups are copied with getgrnam_r
 * and the own account read with getpwuid_r, so that nothing here shares
 * the static storage of the compositor's other lookups; the enumeration
 * (getpwent) is the machine thread's alone.  A directory service may make
 * the enumeration wait, so the compositor calls this on that thread.
 */

#include "userland/desktop/libkeiland-backend/backend-private.h"

#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The first user ID of a person's account, and nobody's, which is not one. */
#define USERS_FIRST_UID		1000U
#define USERS_NOBODY_UID	65534U

/* The most groups of administrators an operating system names, and the group of Wi-Fi's control. */
#define USERS_ADMIN_GROUPS	4U
#define USERS_NETWORK_GROUP	"network"

/* The most members of a group kept, and the longest member's name with its NUL. */
#define USERS_MEMBERS_MAX	64U
#define USERS_MEMBER_NAME	64U

/* The room a lookup starts with, and the most it grows to when the entry does not fit. */
#define USERS_BUFFER_FIRST	4096U
#define USERS_BUFFER_MAX	65536U

/*
 * A group copied with its members: whether it was found, its ID, and its
 * members' names as far as there is room (a longer name is left out, it
 * cannot be a listed account's).
 */
struct users_group {
	int found;
	gid_t gid;
	unsigned member_count;
	char members[USERS_MEMBERS_MAX][USERS_MEMBER_NAME];
};

/*
 * The groups one reading looks at: the administrators' (as many as the
 * operating system names) and Wi-Fi's.
 */
struct users_groups {
	unsigned admin_count;
	struct users_group admin[USERS_ADMIN_GROUPS];
	struct users_group network;
};

static void users_group_copy(const char *name, struct users_group *copy);
static int users_person(const struct passwd *account);
static int users_member(const struct passwd *account, const struct users_group *group);
static unsigned users_flags(const struct passwd *account, const struct users_groups *groups);
static int users_fill(struct kl_backend_user *user, const struct passwd *account, unsigned flags);
static int users_self(struct kl_backend_user *user, const struct users_groups *groups);

/*
 * Reads the people's accounts and the own user's with the administrators'
 * groups named; returns how many were copied.
 */
size_t
kl_backend_users_posix(
	struct kl_backend_user *list,
	size_t capacity,
	unsigned *skipped,
	const char *const *admin_groups)
{
	struct users_groups *groups;
	struct kl_backend_user self;
	struct passwd *account;
	unsigned flags;
	size_t count;
	size_t index;
	int have_self;
	int filled;
	int same;

	/* Nothing left out yet. */
	*skipped = 0;
	count = 0;
	if (capacity == 0U)
		return 0;

	/* The groups' room (their members are too many for a thread's stack). */
	groups = calloc(1, sizeof(*groups));
	if (groups == NULL)
		return 0;

	/* The groups, copied before the accounts are read. */
	for (index = 0; admin_groups[index] != NULL && index < USERS_ADMIN_GROUPS; index++)
		users_group_copy(admin_groups[index], &groups->admin[index]);
	groups->admin_count = (unsigned)index;
	users_group_copy(USERS_NETWORK_GROUP, &groups->network);

	/* The own user's account, kept apart until the people are listed. */
	have_self = users_self(&self, groups);

	/* Each person's account, keeping the list's last place for the own user. */
	setpwent();
	for (;;) {
		/* The next account, or the end. */
		account = getpwent();
		if (account == NULL)
			break;

		/* Only the people's accounts. */
		filled = users_person(account);
		if (!filled)
			continue;

		/* A person without room is left out. */
		if (count + 1U >= capacity) {
			(*skipped)++;
			continue;
		}

		/* The person; a name that does not fit leaves it out. */
		flags = users_flags(account, groups);
		filled = users_fill(&list[count], account, flags);
		if (!filled) {
			(*skipped)++;
			continue;
		}

		/* The own user's row says so, with its home. */
		if (have_self) {
			same = strcmp(list[count].name, self.name);
			if (same == 0) {
				list[count].flags |= KL_BACKEND_USER_SELF;
				memcpy(list[count].home, self.home, sizeof(list[count].home));
				have_self = 0;
			}
		}

		/* Counted. */
		count++;
	}

	/* The database is closed. */
	endpwent();

	/* The own user's account when it is no person's listed. */
	if (have_self) {
		list[count] = self;
		count++;
	}

	/* The groups are not needed any more. */
	free(groups);

	/* Succeeded: the accounts that could be read. */
	return count;
}

/*
 * Copies a group and its members (none found: found is 0).
 */
static void
users_group_copy(
	const char *name,
	struct users_group *copy)
{
	struct group entry;
	struct group *group;
	char *buffer;
	char *grown;
	size_t size;
	size_t length;
	size_t i;
	int error;

	/* Not found until the database says otherwise. */
	memset(copy, 0, sizeof(*copy));

	/* The group, in a room that grows while the entry does not fit. */
	size = USERS_BUFFER_FIRST;
	buffer = malloc(size);
	group = NULL;
	for (;;) {
		/* No room: the group stays not found. */
		if (buffer == NULL)
			return;

		/* The lookup. */
		error = getgrnam_r(name, &entry, buffer, size, &group);
		if (error != ERANGE || size >= USERS_BUFFER_MAX)
			break;

		/* A larger room. */
		size *= 2U;
		grown = realloc(buffer, size);
		if (grown == NULL)
			free(buffer);
		buffer = grown;
	}

	/* No such group, or it could not be read. */
	if (error != 0 || group == NULL) {
		free(buffer);
		return;
	}

	/* Its ID and its members, as far as there is room. */
	copy->found = 1;
	copy->gid = group->gr_gid;
	for (i = 0; group->gr_mem != NULL && group->gr_mem[i] != NULL; i++) {
		if (copy->member_count == USERS_MEMBERS_MAX)
			break;

		/* A name too long to be a listed account's is left out. */
		length = strlen(group->gr_mem[i]);
		if (length >= USERS_MEMBER_NAME)
			continue;

		/* The member. */
		memcpy(copy->members[copy->member_count], group->gr_mem[i], length + 1U);
		copy->member_count++;
	}

	/* The entry's room is not needed any more. */
	free(buffer);
}

/*
 * Says whether an account is a person's: a user ID from 1000 (not
 * nobody's) and a shell that lets it log in.
 */
static int
users_person(
	const struct passwd *account)
{
	const char *shell;
	const char *refusing;

	/* The system's accounts and nobody. */
	if (account->pw_uid < USERS_FIRST_UID || account->pw_uid == USERS_NOBODY_UID)
		return 0;

	/* No shell named: the system's default shell, which logs in. */
	shell = account->pw_shell;
	if (shell == NULL)
		return 1;

	/* Nologin. */
	refusing = strstr(shell, "nologin");
	if (refusing != NULL)
		return 0;

	/* False, at the end of the path. */
	refusing = strstr(shell, "/false");
	if (refusing != NULL)
		return 0;

	/* A person's. */
	return 1;
}

/*
 * Says whether an account is in a group: it is its group, or it is among
 * its members.
 */
static int
users_member(
	const struct passwd *account,
	const struct users_group *group)
{
	unsigned i;
	int differs;

	/* No such group: not in it. */
	if (!group->found)
		return 0;

	/* The group as its own. */
	if (account->pw_gid == group->gid)
		return 1;

	/* Among the members. */
	for (i = 0; i < group->member_count; i++) {
		/* The same name. */
		differs = strcmp(group->members[i], account->pw_name);
		if (differs == 0)
			return 1;
	}

	/* Not in it. */
	return 0;
}

/* Works out an account's flags: a person's, an administrator's, and allowed to control Wi-Fi. */
static unsigned
users_flags(
	const struct passwd *account,
	const struct users_groups *groups)
{
	unsigned flags;
	unsigned index;
	int member;

	/* A person's account. */
	flags = 0;
	member = users_person(account);
	if (member)
		flags |= KL_BACKEND_USER_PERSON;

	/* An administrator: in any of the administrators' groups. */
	for (index = 0; index < groups->admin_count; index++) {
		member = users_member(account, &groups->admin[index]);
		if (member)
			flags |= KL_BACKEND_USER_ADMIN;
	}

	/* Allowed to control Wi-Fi. */
	member = users_member(account, &groups->network);
	if (member)
		flags |= KL_BACKEND_USER_NETWORK;

	/* Succeeded: the account's flags. */
	return flags;
}

/*
 * Fills a row from an account: its name (an account whose name does not
 * fit is refused: 0), its full name (the comment's first field) and its
 * flags; the home stays empty.  Returns 1 when filled.
 */
static int
users_fill(
	struct kl_backend_user *user,
	const struct passwd *account,
	unsigned flags)
{
	size_t length;

	/* A name that does not fit whole is never cut. */
	memset(user, 0, sizeof(*user));
	length = strlen(account->pw_name);
	if (length >= sizeof(user->name))
		return 0;
	memcpy(user->name, account->pw_name, length + 1U);

	/* The full name: the comment up to its first comma, cut at a character's boundary. */
	if (account->pw_gecos != NULL) {
		length = strcspn(account->pw_gecos, ",");
		kl_backend_machine_copy(user->full_name, sizeof(user->full_name), account->pw_gecos, length);
	}

	/* Succeeded: the row is the account's. */
	user->flags = flags;
	return 1;
}

/*
 * Reads the own user's account (the process's real user ID) into a row
 * with its home and KL_BACKEND_USER_SELF.  Returns 1 when read.
 */
static int
users_self(
	struct kl_backend_user *user,
	const struct users_groups *groups)
{
	struct passwd entry;
	struct passwd *account;
	unsigned flags;
	char *buffer;
	char *grown;
	size_t size;
	int filled;
	int error;

	/* The account, in a room that grows while the entry does not fit. */
	size = USERS_BUFFER_FIRST;
	buffer = malloc(size);
	account = NULL;
	for (;;) {
		/* No room: not read. */
		if (buffer == NULL)
			return 0;

		/* The lookup. */
		error = getpwuid_r(getuid(), &entry, buffer, size, &account);
		if (error != ERANGE || size >= USERS_BUFFER_MAX)
			break;

		/* A larger room. */
		size *= 2U;
		grown = realloc(buffer, size);
		if (grown == NULL)
			free(buffer);
		buffer = grown;
	}

	/* No account, or it could not be read. */
	if (error != 0 || account == NULL) {
		free(buffer);
		return 0;
	}

	/* The row, with the own user's flag and home. */
	flags = users_flags(account, groups);
	flags |= KL_BACKEND_USER_SELF;
	filled = users_fill(user, account, flags);
	if (filled && account->pw_dir != NULL)
		kl_backend_machine_copy(user->home, sizeof(user->home), account->pw_dir, strlen(account->pw_dir));

	/* The entry's room is not needed any more. */
	free(buffer);

	/* Reports a name that does not fit. */
	if (!filled)
		return 0;

	/* Succeeded: the own user's row. */
	return 1;
}
