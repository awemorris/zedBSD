/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the computer's reading (ws188-p002): the parts that
 * know no Wayland and no thread.
 *
 *   - libkeiland-backend's machine area: os-release's PRETTY_NAME (the
 *     cases of ws089-p027's host-about, moved with the parser), the UTF-8
 *     cut, the system's names, the file systems and the accounts of this
 *     host (the own account always kept, a list without room).
 *   - the compositor's queries waiting (machine-wait.c): the bounds, an
 *     object that goes, the parts a reading reads together, the order.
 *   - libkeiland's view of the answers (system-view.c): an answer put
 *     into effect at its result, a failure or another request's result
 *     dropping it, events outside an answer ignored, the serials.
 *
 *   sh plan/ws188/tests/host-machine.sh
 */

#include "userland/desktop/libkeiland-backend/backend-private.h"
#include "userland/desktop/libkeiland/system/system-private.h"
#include "userland/desktop/wayland/machine-wait.h"
#include "userland/desktop/files/mounts.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void check(int condition, const char *format, ...);
static int name_of(const char *text, char *name, size_t size);
static void test_pretty_name(void);
static void test_copy(void);
static void test_host(void);
static void test_wait(void);
static void test_view(void);
static void test_mounts(void);

/* The checks that failed and those that ran, the directory the files go to, and the next file's number. */
static int failures;
static int checks;
static const char *directory;
static unsigned file_number;

/* Counts one check, and reports it when it fails. */
static void
check(
	int condition,
	const char *format,
	...)
{
	va_list arguments;

	/* Counted. */
	checks++;
	if (condition)
		return;

	/* Failed: said. */
	failures++;
	va_start(arguments, format);
	fprintf(stderr, "FAIL: ");
	vfprintf(stderr, format, arguments);
	fprintf(stderr, "\n");
	va_end(arguments);
}

/* Writes a new os-release file (each a name of its own; nothing is removed) and reads its name. */
static int
name_of(
	const char *text,
	char *name,
	size_t size)
{
	char path[512];
	FILE *file;
	int result;

	/* The file. */
	(void)snprintf(path, sizeof(path), "%s/os-release.%u", directory, file_number);
	file_number++;
	file = fopen(path, "w");
	if (file == NULL)
		return -2;
	(void)fputs(text, file);
	(void)fclose(file);

	/* Its name. */
	result = kl_backend_machine_pretty_name(path, name, size);
	return result;
}

/* os-release's PRETTY_NAME (ws089-p027's cases). */
static void
test_pretty_name(void)
{
	char name[128];
	char small[8];
	char path[512];
	int result;

	/* zedBSD's own file (the top-level Makefile's lines). */
	result = name_of("NAME=\"zedBSD\"\nID=zedbsd\nVERSION=\"1.0.0 Beta 1\"\nVERSION_ID=1.0.0-beta1\n"
			 "PRETTY_NAME=\"Kei/zedBSD 1.0.0 Beta 1\"\nBUILD_ID=g1234567\n", name, sizeof(name));
	check(result == 0 && strcmp(name, "Kei/zedBSD 1.0.0 Beta 1") == 0, "zedBSD's PRETTY_NAME");

	/* Single quotes, no quotes, escapes. */
	result = name_of("PRETTY_NAME='Debian GNU/Linux 13 (trixie)'\n", name, sizeof(name));
	check(result == 0 && strcmp(name, "Debian GNU/Linux 13 (trixie)") == 0, "single quotes");
	result = name_of("PRETTY_NAME=Plain\n", name, sizeof(name));
	check(result == 0 && strcmp(name, "Plain") == 0, "no quotes");
	result = name_of("PRETTY_NAME=\"A \\\"quoted\\\" \\\\ name\"\n", name, sizeof(name));
	check(result == 0 && strcmp(name, "A \"quoted\" \\ name") == 0, "backslash escapes");

	/* A comment and a key that only begins like it come before it. */
	result = name_of("# PRETTY_NAME=\"no\"\nPRETTY_NAMES=\"no\"\nPRETTY_NAME=\"yes\"\r\n", name, sizeof(name));
	check(result == 0 && strcmp(name, "yes") == 0, "comment and a longer key skipped, CR dropped");

	/* No PRETTY_NAME, an empty one, an empty file, no file. */
	result = name_of("NAME=\"zedBSD\"\n", name, sizeof(name));
	check(result == -1 && name[0] == '\0', "no PRETTY_NAME");
	result = name_of("PRETTY_NAME=\"\"\n", name, sizeof(name));
	check(result == -1 && name[0] == '\0', "an empty PRETTY_NAME counts as none");
	result = name_of("", name, sizeof(name));
	check(result == -1 && name[0] == '\0', "an empty file");
	(void)snprintf(path, sizeof(path), "%s/no-such-os-release", directory);
	result = kl_backend_machine_pretty_name(path, name, sizeof(name));
	check(result == -1 && name[0] == '\0', "no file");

	/* Cut short to the buffer, and never inside a character. */
	result = name_of("PRETTY_NAME=\"Kei/zedBSD 1.0.0 Beta 1\"\n", small, sizeof(small));
	check(result == 0 && strcmp(small, "Kei/zed") == 0, "cut short to fit");
	result = name_of("PRETTY_NAME=\"Kei\xe3\x81\x82\xe3\x81\x84\"\n", small, sizeof(small));
	check(result == 0 && strcmp(small, "Kei\xe3\x81\x82") == 0, "cut before a character that does not fit (%s)", small);
}

/* The UTF-8 cut. */
static void
test_copy(void)
{
	char room[6];

	/* Whole when it fits; cut at a character's start otherwise. */
	kl_backend_machine_copy(room, sizeof(room), "abc", 3U);
	check(strcmp(room, "abc") == 0, "copy: fits");
	kl_backend_machine_copy(room, sizeof(room), "abcdefg", 7U);
	check(strcmp(room, "abcde") == 0, "copy: ASCII cut");
	kl_backend_machine_copy(room, sizeof(room), "ab\xe3\x81\x82\xe3\x81\x84", 8U);
	check(strcmp(room, "ab\xe3\x81\x82") == 0, "copy: three-byte character kept whole");
	kl_backend_machine_copy(room, sizeof(room), "abcd\xe3\x81\x82", 7U);
	check(strcmp(room, "abcd") == 0, "copy: three-byte character not cut");
	kl_backend_machine_copy(room, sizeof(room), "a,b", 1U);
	check(strcmp(room, "a") == 0, "copy: a length shorter than the text");
}

/* This host's names, file systems and accounts. */
static void
test_host(void)
{
	static const char *const no_groups[] = { "zedbsd-no-such-group", NULL };
	struct kl_backend_machine machine;
	struct kl_backend_filesystem filesystems[KL_BACKEND_FILESYSTEMS_MAX];
	struct kl_backend_user users[KL_BACKEND_USERS_MAX];
	size_t count;
	size_t index;
	size_t other;
	unsigned skipped;
	unsigned selves;
	int status;

	/* The names: the kernel and the architecture are always there, the processors counted. */
	status = kl_backend_machine_read(&machine);
	check(status == 0 && machine.kernel[0] != '\0' && machine.architecture[0] != '\0', "names: %s / %s", machine.kernel, machine.architecture);
	check(machine.cpus > 0U, "names: %u processors", machine.cpus);
	printf("host: system=%s kernel=%s arch=%s processor=%s host=%s cpus=%u\n", machine.system, machine.kernel,
	       machine.architecture, machine.processor, machine.host, machine.cpus);

	/* The file systems: the root first, each once. */
	count = kl_backend_filesystems_read(filesystems, KL_BACKEND_FILESYSTEMS_MAX);
	check(count >= 1U && strcmp(filesystems[0].path, "/") == 0, "file systems: the root first");
	check(count >= 1U && filesystems[0].total > 0U && filesystems[0].used <= filesystems[0].total, "file systems: the root's sizes");
	for (index = 0; index < count; index++) {
		for (other = index + 1U; other < count; other++)
			check(strcmp(filesystems[index].path, filesystems[other].path) != 0, "file systems: %s twice", filesystems[index].path);
	}
	count = kl_backend_filesystems_read(filesystems, 1U);
	check(count == 1U, "file systems: one with room for one");

	/* The accounts: the own one exactly once, with its home; no other has a home. */
	count = kl_backend_users_posix(users, KL_BACKEND_USERS_MAX, &skipped, no_groups);
	selves = 0U;
	for (index = 0; index < count; index++) {
		if ((users[index].flags & KL_BACKEND_USER_SELF) != 0U) {
			selves++;
			check(users[index].home[0] != '\0', "accounts: the own one's home");
			continue;
		}
		check(users[index].home[0] == '\0', "accounts: %s has no home sent", users[index].name);
		check((users[index].flags & KL_BACKEND_USER_PERSON) != 0U, "accounts: %s only a person's", users[index].name);
		check((users[index].flags & KL_BACKEND_USER_ADMIN) == 0U, "accounts: %s not in a group that does not exist", users[index].name);
	}
	check(selves == 1U, "accounts: the own one once (%u of %u)", selves, (unsigned)count);

	/* With room for one, the own account is the one. */
	count = kl_backend_users_posix(users, 1U, &skipped, no_groups);
	check(count == 1U && (users[0].flags & KL_BACKEND_USER_SELF) != 0U, "accounts: the own one kept with room for one");

	/* Without room, none. */
	count = kl_backend_users_posix(users, 0U, &skipped, no_groups);
	check(count == 0U && skipped == 0U, "accounts: none without room");
}

/* The compositor's queries waiting. */
static void
test_wait(void)
{
	struct kwl_machine_wait wait;
	struct kwl_machine_query query;
	unsigned index;
	uint32_t what;
	int error;
	int taken;

	/* Four of a client, then busy; another client still has room. */
	memset(&wait, 0, sizeof(wait));
	for (index = 0; index < KWL_MACHINE_CLIENT_MAX; index++) {
		error = kwl_machine_wait_add(&wait, 1U, 10U, 100U + index, 1U << (index % 4U));
		check(error == 0, "wait: query %u of client 1", index);
	}
	error = kwl_machine_wait_add(&wait, 1U, 10U, 200U, 1U);
	check(error == EBUSY, "wait: a fifth of client 1 busy");
	error = kwl_machine_wait_add(&wait, 2U, 20U, 300U, 2U);
	check(error == 0, "wait: client 2 has room");

	/* A reading takes them all, with their parts together. */
	what = kwl_machine_wait_start(&wait);
	check(what == 0xfU, "wait: the parts together 0x%x", what);
	check(!kwl_machine_wait_pending(&wait), "wait: none for the next reading");

	/* A query during the reading waits for the next; the taken ones still count for the bound. */
	error = kwl_machine_wait_add(&wait, 1U, 10U, 400U, 1U);
	check(error == EBUSY, "wait: client 1 still holds four during the reading");
	error = kwl_machine_wait_add(&wait, 2U, 21U, 500U, 8U);
	check(error == 0, "wait: client 2's second during the reading");
	check(kwl_machine_wait_pending(&wait), "wait: one for the next reading");

	/* An object that goes: its queries go, taken or not. */
	kwl_machine_wait_drop(&wait, 2U, 20U);

	/* The reading's queries in order; the one that came during it is not among them. */
	for (index = 0; index < KWL_MACHINE_CLIENT_MAX; index++) {
		taken = kwl_machine_wait_take(&wait, &query);
		check(taken && query.request == 100U + index, "wait: taken in order (%u)", taken ? query.request : 0U);
	}
	taken = kwl_machine_wait_take(&wait, &query);
	check(!taken, "wait: the dropped and the new one not taken");

	/* The next reading takes the one that waited. */
	what = kwl_machine_wait_start(&wait);
	check(what == 8U, "wait: the next reading reads 0x%x", what);
	taken = kwl_machine_wait_take(&wait, &query);
	check(taken && query.request == 500U && query.object == 21U, "wait: the next reading's query");
	what = kwl_machine_wait_start(&wait);
	check(what == 0U, "wait: nothing more to read");

	/* Sixteen in all, then busy for anyone. */
	memset(&wait, 0, sizeof(wait));
	for (index = 0; index < KWL_MACHINE_WAITING_MAX; index++) {
		error = kwl_machine_wait_add(&wait, 100U + index / 4U, 1U, index, 1U);
		check(error == 0, "wait: query %u of the sixteen", index);
	}
	error = kwl_machine_wait_add(&wait, 999U, 1U, 999U, 1U);
	check(error == EBUSY, "wait: the seventeenth busy");
}

/* libkeiland's view of the answers. */
static void
test_view(void)
{
	static struct system_view view;
	struct kl_machine_about about;
	struct kl_machine_filesystem filesystem;
	struct kl_machine_user user;
	uint32_t request;
	unsigned changed;
	int error;
	int taken;

	/* Nothing known, and events outside an answer ignored. */
	system_view_init(&view);
	memset(&user, 0, sizeof(user));
	(void)snprintf(user.name, sizeof(user.name), "early");
	system_view_machine_user(&view, &user);
	check(view.machine_users_pending_count == 0U, "view: a user before parts ignored");

	/* A whole answer of the users and the language: in effect at its result. */
	system_view_machine_parts(&view, 7U, KL_SYSTEM_MACHINE_USERS | KL_SYSTEM_MACHINE_LOGIN_LANGUAGE);
	(void)snprintf(user.name, sizeof(user.name), "kei");
	user.flags = KL_MACHINE_USER_PERSON | KL_MACHINE_USER_SELF;
	system_view_machine_user(&view, &user);
	system_view_machine_login_language(&view, "ja");
	memset(&about, 0, sizeof(about));
	(void)snprintf(about.kernel, sizeof(about.kernel), "not asked");
	system_view_machine_about(&view, &about);
	check(view.machine_user_count == 0U, "view: not in effect before the result");
	system_view_machine_result(&view, 7U, KL_SYSTEM_RESULT_OK);
	changed = system_view_take_changed(&view);
	check((changed & KL_SYSTEM_CHANGED_MACHINE) != 0U && (changed & KL_SYSTEM_CHANGED_RESULT) != 0U, "view: changed and answered");
	check(view.machine_user_count == 1U && strcmp(view.machine_users[0].name, "kei") == 0, "view: the user in effect");
	check(strcmp(view.machine_language, "ja") == 0, "view: the language in effect");
	check(view.machine_known == (KL_SYSTEM_MACHINE_USERS | KL_SYSTEM_MACHINE_LOGIN_LANGUAGE), "view: known 0x%x", view.machine_known);
	check(view.machine_about.kernel[0] == '\0' && view.machine_serials[0] == 0U, "view: a part not asked not taken");
	check(view.machine_serials[2] == 1U && view.machine_serials[3] == 1U, "view: the serials of the parts answered");
	taken = system_view_take_result(&view, &request, &error);
	check(taken && request == 7U && error == 0, "view: the result taken");

	/* An answer replaces the list whole: none means none. */
	system_view_machine_parts(&view, 8U, KL_SYSTEM_MACHINE_USERS);
	system_view_machine_result(&view, 8U, KL_SYSTEM_RESULT_OK);
	check(view.machine_user_count == 0U && view.machine_serials[2] == 2U, "view: an empty list replaces");

	/* A failure, and another request's result, drop what was received. */
	system_view_machine_parts(&view, 9U, KL_SYSTEM_MACHINE_FILESYSTEMS);
	memset(&filesystem, 0, sizeof(filesystem));
	(void)snprintf(filesystem.path, sizeof(filesystem.path), "/");
	system_view_machine_filesystem(&view, &filesystem);
	system_view_machine_result(&view, 9U, KL_SYSTEM_RESULT_UNAVAILABLE);
	check(view.machine_filesystem_count == 0U && (view.machine_known & KL_SYSTEM_MACHINE_FILESYSTEMS) == 0U, "view: a failure drops");
	taken = system_view_take_result(&view, &request, &error);
	check(taken && request == 8U && error == 0, "view: the empty answer's result first");
	taken = system_view_take_result(&view, &request, &error);
	check(taken && request == 9U && error == ENODEV, "view: unavailable is ENODEV (%d)", error);
	system_view_machine_parts(&view, 10U, KL_SYSTEM_MACHINE_FILESYSTEMS);
	system_view_machine_filesystem(&view, &filesystem);
	system_view_machine_result(&view, 11U, KL_SYSTEM_RESULT_OK);
	check(view.machine_filesystem_count == 0U, "view: another request's result drops");

	/* A new parts drops an answer cut short; a failure alone (busy) changes nothing. */
	system_view_machine_parts(&view, 12U, KL_SYSTEM_MACHINE_FILESYSTEMS);
	system_view_machine_filesystem(&view, &filesystem);
	system_view_machine_parts(&view, 13U, KL_SYSTEM_MACHINE_FILESYSTEMS);
	system_view_machine_result(&view, 13U, KL_SYSTEM_RESULT_OK);
	check(view.machine_filesystem_count == 0U && view.machine_serials[1] == 1U, "view: the cut answer dropped, the new one empty");
	system_view_machine_result(&view, 14U, KL_SYSTEM_RESULT_BUSY);
	check(view.machine_serials[1] == 1U, "view: busy alone changes nothing");

	/* More than the room is not kept. */
	system_view_machine_parts(&view, 15U, KL_SYSTEM_MACHINE_FILESYSTEMS);
	for (request = 0; request < KL_MACHINE_FILESYSTEMS_MAX + 2U; request++)
		system_view_machine_filesystem(&view, &filesystem);
	system_view_machine_result(&view, 15U, KL_SYSTEM_RESULT_OK);
	check(view.machine_filesystem_count == KL_MACHINE_FILESYSTEMS_MAX, "view: the room's file systems kept");
}

/* The mounts (ws188-p004): the backend's choice of file systems, the view's fifth part, the file manager's copy. */
static void
test_mounts(void)
{
	static struct kl_backend_mount mounts[KL_BACKEND_MOUNTS_MAX];
	static struct system_view view;
	struct kl_machine_mount machine[2];
	struct fm_mounts *walk;
	struct fm_mount mount;
	char long_path[400];
	size_t count;
	size_t index;
	unsigned skipped;
	int root;
	int added;
	int error;
	int next;

	/* The virtual file systems are left out, the file systems of files kept. */
	check(!kl_backend_mounts_keep("proc") && !kl_backend_mounts_keep("tmpfs") && !kl_backend_mounts_keep("squashfs"), "mounts: virtual ones left out");
	check(kl_backend_mounts_keep("ext4") && kl_backend_mounts_keep("ufs") && kl_backend_mounts_keep("msdosfs") && kl_backend_mounts_keep("vfat"), "mounts: file systems of files kept");

	/* A path too long is left out, never cut. */
	memset(long_path, 'a', sizeof(long_path) - 1U);
	long_path[0] = '/';
	long_path[sizeof(long_path) - 1U] = '\0';
	added = kl_backend_mounts_add(&mounts[0], long_path, "ext4");
	check(!added, "mounts: a path too long refused");

	/* This host's table: the root is there, nothing virtual, each with a type. */
	count = kl_backend_mounts_read(mounts, KL_BACKEND_MOUNTS_MAX, &skipped);
	root = 0;
	for (index = 0; index < count; index++) {
		if (strcmp(mounts[index].path, "/") == 0)
			root = 1;
		check(kl_backend_mounts_keep(mounts[index].type), "mounts: %s (%s) is not virtual", mounts[index].path, mounts[index].type);
	}
	check(root, "mounts: the root among %u (skipped %u)", (unsigned)count, skipped);
	count = kl_backend_mounts_read(mounts, 1U, &skipped);
	check(count == 1U, "mounts: one with room for one");

	/* The view's fifth part, its own serial. */
	system_view_init(&view);
	memset(machine, 0, sizeof(machine));
	(void)snprintf(machine[0].path, sizeof(machine[0].path), "/");
	(void)snprintf(machine[0].type, sizeof(machine[0].type), "ufs");
	(void)snprintf(machine[1].path, sizeof(machine[1].path), "/media/USB");
	(void)snprintf(machine[1].type, sizeof(machine[1].type), "msdosfs");
	system_view_machine_mount(&view, &machine[0]);
	check(view.machine_mounts_pending_count == 0U, "view: a mount before parts ignored");
	system_view_machine_parts(&view, 1U, KL_SYSTEM_MACHINE_MOUNTS);
	system_view_machine_mount(&view, &machine[0]);
	system_view_machine_mount(&view, &machine[1]);
	system_view_machine_result(&view, 1U, KL_SYSTEM_RESULT_OK);
	check(view.machine_mount_count == 2U && view.machine_serials[4] == 1U && view.machine_serials[1] == 0U, "view: the mounts in effect, their serial alone");

	/* The file manager's copy: empty before an answer, then the walk over a copy. */
	error = fm_mounts_open(&walk);
	check(error == 0, "files: a walk before an answer");
	next = fm_mounts_next(walk, &mount);
	check(next == 0, "files: nothing before an answer");
	fm_mounts_close(walk);
	fm_mounts_set(machine, 2U);
	error = fm_mounts_open(&walk);
	fm_mounts_set(machine, 1U);
	next = fm_mounts_next(walk, &mount);
	check(error == 0 && next == 1 && strcmp(mount.path, "/") == 0 && strcmp(mount.type, "ufs") == 0, "files: the first mount");
	next = fm_mounts_next(walk, &mount);
	check(next == 1 && strcmp(mount.path, "/media/USB") == 0, "files: the walk keeps its copy after a new answer");
	next = fm_mounts_next(walk, &mount);
	check(next == 0, "files: the walk's end");
	fm_mounts_close(walk);
	next = fm_mounts_next(NULL, &mount);
	check(next == -1 && errno == EINVAL, "files: no walk refused");
}

/* Runs the checks with the directory for the test's files. */
int
main(
	int argc,
	char **argv)
{
	/* The directory is given. */
	if (argc < 2) {
		fprintf(stderr, "usage: host-machine DIRECTORY\n");
		return 2;
	}
	directory = argv[1];

	/* Each part. */
	test_pretty_name();
	test_copy();
	test_host();
	test_wait();
	test_view();
	test_mounts();

	/* Reports failed checks. */
	if (failures != 0) {
		fprintf(stderr, "host-machine: %d of %d checks failed\n", failures, checks);
		return 1;
	}

	/* Succeeded: every check passed. */
	printf("host-machine: %d checks passed\n", checks);
	return 0;
}
