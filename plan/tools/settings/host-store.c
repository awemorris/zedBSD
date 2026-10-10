/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws135-p002: the host tests of the compositor's settings store
 * (userland/desktop/wayland/settings-store.c) and of the settings' table
 * (userland/desktop/settings-keys/settings-keys.c): reading the file
 * (unknown keys, comments, a number out of range, a broken line, a file
 * too large), the checks of a set, the merge at the session's end (a hand
 * edit made during the session stays, two stores merge, nothing is
 * written when nothing changed, a file that could not be read is not
 * broken), the writer thread and the sound's values.
 */

#include "settings-store.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int test_passed;
static int test_failed;
static char test_home[256];
static char test_conf[512];

static void check(int ok, const char *format, ...);
static void put_file(const char *text);
static char *get_file(void);
static int file_has(const char *line);
static void test_keys(void);
static void test_read(void);
static void test_set(void);
static void test_merge(void);
static void test_hand_edit(void);
static void test_two_stores(void);
static void test_unread(void);
static void test_writer(void);
static void test_sound(void);

int
main(
	void)
{
	char folder[512];
	char *made;

	/* A home of its own. */
	snprintf(test_home, sizeof(test_home), "/tmp/ws135-host-store-XXXXXX");
	made = mkdtemp(test_home);
	if (made == NULL) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(folder, sizeof(folder), "%s/.config", test_home);
	(void)mkdir(folder, 0700);
	snprintf(folder, sizeof(folder), "%s/.config/keiland", test_home);
	(void)mkdir(folder, 0700);
	snprintf(test_conf, sizeof(test_conf), "%s/.config/keiland/desktop.conf", test_home);

	test_keys();
	test_read();
	test_set();
	test_merge();
	test_hand_edit();
	test_two_stores();
	test_unread();
	test_writer();
	test_sound();

	printf("host-store: %d passed, %d failed\n", test_passed, test_failed);
	if (test_failed != 0)
		return 1;
	return 0;
}

static void
check(
	int ok,
	const char *format,
	...)
{
	va_list arguments;

	if (ok) {
		test_passed++;
		printf("ok   ");
	} else {
		test_failed++;
		printf("FAIL ");
	}
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
	printf("\n");
}

static void
put_file(
	const char *text)
{
	FILE *file;

	file = fopen(test_conf, "w");
	if (file == NULL)
		return;
	fputs(text, file);
	fclose(file);
}

static char *
get_file(void)
{
	static char text[70000];
	FILE *file;
	size_t length;

	text[0] = '\0';
	file = fopen(test_conf, "r");
	if (file == NULL)
		return text;
	length = fread(text, 1, sizeof(text) - 1U, file);
	text[length] = '\0';
	fclose(file);
	return text;
}

static int
file_has(
	const char *line)
{
	char wanted[512];
	char *text;

	text = get_file();
	snprintf(wanted, sizeof(wanted), "%s\n", line);
	if (strncmp(text, wanted, strlen(wanted)) == 0)
		return 1;
	snprintf(wanted, sizeof(wanted), "\n%s\n", line);
	return strstr(text, wanted) != NULL;
}

static void
test_keys(void)
{
	const struct kl_settings_key *key;
	int number;
	int error;

	key = kl_settings_key_find("window.opacity");
	check(key != NULL && key->resolver == KL_SETTINGS_RESOLVER_COMPOSITOR, "keys: window.opacity is the compositor's");
	if (key == NULL)
		return;
	check(kl_settings_key_check(key, "90") == 0, "keys: 90 is a good opacity");
	check(kl_settings_key_check(key, "84") == EINVAL, "keys: 84 is below the range (not moved into it)");
	check(kl_settings_key_check(key, "9x") == EINVAL, "keys: 9x is not a number");
	error = kl_settings_key_number(key, "120", &number);
	check(error == 0 && number == 100, "keys: a read 120 is moved to 100 (%d)", number);
	key = kl_settings_key_find("wallpaper");
	check(key != NULL && kl_settings_key_check(key, "/a.ppm") == 0, "keys: an absolute wallpaper path is good");
	check(key != NULL && kl_settings_key_check(key, "a.ppm") == EINVAL, "keys: a relative path is not");
	check(key != NULL && kl_settings_key_check(key, "/a\nb") == EINVAL, "keys: a control character is not");
	key = kl_settings_key_find("terminal.ambiguous-wide");
	check(key != NULL && key->resolver == KL_SETTINGS_RESOLVER_APP, "keys: terminal.ambiguous-wide is the application's");
	check(kl_settings_key_find("no.such.key") == NULL, "keys: an unknown key is not found");
	check(kl_settings_name_valid("a.b-c_9") && !kl_settings_name_valid("A") && !kl_settings_name_valid(""), "keys: names are checked");
}

static void
test_read(void)
{
	struct kwl_settings_store store;
	struct kwl_settings_entry *entry;
	int error;

	put_file("# a comment\nunknown.key=1\npointer.speed=400\nwindow.opacity=x\nbroken line\nkeyboard.repeat.rate=30\nkeyboard.repeat.rate=40\nwallpaper=relative.ppm\nsound.volume=40\n");
	error = kwl_settings_store_open(&store, test_home);
	check(error == 0, "read: opens");
	kwl_settings_store_default(&store, "window.opacity", "97");
	error = kwl_settings_store_load(&store);
	check(error == 0, "read: loads");
	entry = kwl_settings_store_find(&store, "pointer.speed");
	check(entry != NULL && strcmp(entry->value, "300") == 0 && entry->chosen, "read: pointer.speed 400 is moved to 300 (%s)", entry != NULL ? entry->value : "-");
	entry = kwl_settings_store_find(&store, "window.opacity");
	check(entry != NULL && strcmp(entry->value, "97") == 0 && !entry->chosen, "read: a bad opacity leaves the command line's default 97");
	entry = kwl_settings_store_find(&store, "keyboard.repeat.rate");
	check(entry != NULL && strcmp(entry->value, "30") == 0, "read: a key given twice keeps its first value");
	entry = kwl_settings_store_find(&store, "wallpaper");
	check(entry != NULL && !entry->chosen, "read: a relative wallpaper is passed over");
	entry = kwl_settings_store_find(&store, "sound.volume");
	check(entry != NULL && !entry->known && entry->start_chosen && strcmp(entry->start, "40") == 0, "read: the sound's start is read, not in effect until audiod reports");
	check(kwl_settings_store_find(&store, "unknown.key") == NULL, "read: an unknown key is not held");
	kwl_settings_store_close(&store);
}

static void
test_set(void)
{
	struct kwl_settings_store store;
	struct kwl_settings_entry *entry;
	const struct kl_settings_key *key;
	size_t count;
	size_t index;
	size_t missing;
	int error;

	put_file("");
	(void)kwl_settings_store_open(&store, test_home);
	(void)kwl_settings_store_load(&store);

	/* Every compositor row of the table is held (BUG-287: phone.backend was not). */
	missing = 0;
	count = kl_settings_key_count();
	for (index = 0; index < count; index++) {
		key = kl_settings_key_at(index);
		if (key->resolver != KL_SETTINGS_RESOLVER_COMPOSITOR || (key->flags & KL_SETTINGS_KEY_PREFIX) != 0U)
			continue;
		if (kwl_settings_store_find(&store, key->name) == NULL) {
			printf("     not held: %s\n", key->name);
			missing++;
		}
	}
	check(missing == 0U, "set: every compositor row of the table is held (%zu missing)", missing);
	check(kwl_settings_store_choose(&store, "phone.backend", "2") == 0, "set: phone.backend 2");
	check(kwl_settings_store_choose(&store, "power.sleep.battery", "10") == 0, "set: power.sleep.battery 10");
	check(kwl_settings_store_choose(&store, "mouse.speed", "26") == 0, "set: mouse.speed 26");
	check(kwl_settings_store_choose(&store, "mouse.speed", "301") == EINVAL, "set: 301 is refused");
	check(kwl_settings_store_choose(&store, "no.key", "1") == ENOENT, "set: an unknown key is ENOENT");
	check(kwl_settings_store_choose(&store, "sound.available", "1") == EPERM, "set: sound.available is read only");
	check(kwl_settings_store_reset(&store, "sound.available") == EPERM, "set: sound.available cannot be reset");
	entry = kwl_settings_store_find(&store, "mouse.speed");
	check(entry != NULL && strcmp(entry->value, "26") == 0 && entry->chosen, "set: 26 is in effect");
	error = kwl_settings_store_reset(&store, "mouse.speed");
	check(error == 0 && entry != NULL && strcmp(entry->value, "150") == 0 && !entry->chosen, "set: reset gives the default 150");
	kwl_settings_store_close(&store);
}

static void
test_merge(void)
{
	struct kwl_settings_store store;
	struct stat before;
	struct stat after;
	int error;

	put_file("# keep me\nfoo.bar=keep\nmouse.speed=50\nwindow.opacity=90\n");
	(void)kwl_settings_store_open(&store, test_home);
	(void)kwl_settings_store_load(&store);

	/* Nothing changed: nothing written. */
	(void)stat(test_conf, &before);
	sleep(1);
	error = kwl_settings_store_finish(&store);
	(void)stat(test_conf, &after);
	check(error == 0 && before.st_mtime == after.st_mtime && before.st_ino == after.st_ino, "merge: nothing changed, the file is not written");

	/* A set, a reset and a set back to the start value. */
	(void)kwl_settings_store_choose(&store, "mouse.speed", "60");
	(void)kwl_settings_store_reset(&store, "window.opacity");
	(void)kwl_settings_store_choose(&store, "keyboard.repeat.rate", "33");
	(void)kwl_settings_store_choose(&store, "keyboard.repeat.rate", "25");
	(void)kwl_settings_store_reset(&store, "keyboard.repeat.rate");
	error = kwl_settings_store_finish(&store);
	check(error == 0, "merge: written");
	check(file_has("# keep me") && file_has("foo.bar=keep"), "merge: the comment and the unknown key stay");
	check(file_has("mouse.speed=60"), "merge: mouse.speed=60 is written");
	check(strstr(get_file(), "window.opacity") == NULL, "merge: the reset opacity leaves the file");
	check(strstr(get_file(), "keyboard.repeat.rate") == NULL, "merge: a key set and reset to its absent start is not written");
	kwl_settings_store_close(&store);
}

static void
test_hand_edit(void)
{
	struct kwl_settings_store store;
	int error;

	put_file("mouse.speed=50\n");
	(void)kwl_settings_store_open(&store, test_home);
	(void)kwl_settings_store_load(&store);
	(void)kwl_settings_store_choose(&store, "mouse.natural", "1");

	/* A hand edit during the session, of another key and of an unknown one. */
	put_file("mouse.speed=70\nmy.note=hello\n");
	error = kwl_settings_store_finish(&store);
	check(error == 0 && file_has("mouse.speed=70") && file_has("my.note=hello") && file_has("mouse.natural=1"),
	      "hand edit: the edit of a key the session did not change stays, and the session's change is merged");
	kwl_settings_store_close(&store);
}

static void
test_two_stores(void)
{
	struct kwl_settings_store first;
	struct kwl_settings_store second;
	int error;

	put_file("");
	(void)kwl_settings_store_open(&first, test_home);
	(void)kwl_settings_store_load(&first);
	(void)kwl_settings_store_open(&second, test_home);
	(void)kwl_settings_store_load(&second);
	(void)kwl_settings_store_choose(&first, "mouse.speed", "111");
	(void)kwl_settings_store_choose(&second, "keyboard.repeat.delay", "500");
	error = kwl_settings_store_finish(&first);
	error |= kwl_settings_store_finish(&second);
	check(error == 0 && file_has("mouse.speed=111") && file_has("keyboard.repeat.delay=500"), "two stores: both sessions' changes are kept");
	kwl_settings_store_close(&first);
	kwl_settings_store_close(&second);
}

static void
test_unread(void)
{
	struct kwl_settings_store store;
	char *big;
	FILE *file;
	size_t index;
	int error;

	/* A file over the limit: the start fails, and the end's merge leaves it whole. */
	file = fopen(test_conf, "w");
	big = malloc(70000);
	if (file == NULL || big == NULL)
		return;
	for (index = 0; index < 69999U; index++)
		big[index] = (index % 50U == 49U) ? '\n' : '#';
	big[69999] = '\0';
	fputs(big, file);
	fclose(file);
	(void)kwl_settings_store_open(&store, test_home);
	error = kwl_settings_store_load(&store);
	check(error == E2BIG && store.read_error == E2BIG, "unread: a file too large is E2BIG");
	(void)kwl_settings_store_choose(&store, "mouse.speed", "60");
	error = kwl_settings_store_finish(&store);
	check(error == E2BIG && strlen(get_file()) == 69999U, "unread: the merge does not break the file it cannot read");
	kwl_settings_store_close(&store);
	free(big);

	/* No home: nothing at all. */
	(void)kwl_settings_store_open(&store, NULL);
	(void)kwl_settings_store_choose(&store, "mouse.speed", "60");
	check(kwl_settings_store_load(&store) == 0 && kwl_settings_store_finish(&store) == 0, "unread: without a home nothing is read or written");
	kwl_settings_store_close(&store);
}

static void
test_writer(void)
{
	struct kwl_settings_store store;
	int error;

	put_file("");
	(void)kwl_settings_store_open(&store, test_home);
	(void)kwl_settings_store_load(&store);
	(void)kwl_settings_store_choose(&store, "mouse.speed", "80");
	error = kwl_settings_store_save_later(&store);
	check(error == 0, "writer: started");
	check(kwl_settings_store_save_later(&store) == EBUSY, "writer: one a session");

	/* A change after the writer took its copy is written at the end. */
	(void)kwl_settings_store_choose(&store, "mouse.natural", "1");
	error = kwl_settings_store_finish(&store);
	check(error == 0 && file_has("mouse.speed=80") && file_has("mouse.natural=1"), "writer: the writer's and the later change are both written");
	kwl_settings_store_close(&store);
}

static void
test_sound(void)
{
	struct kwl_settings_store store;
	struct kwl_settings_change changes[KWL_SETTINGS_ENTRIES];
	unsigned count;

	put_file("sound.volume=40\nsound.muted=0\n");
	(void)kwl_settings_store_open(&store, test_home);
	(void)kwl_settings_store_load(&store);
	count = kwl_settings_store_changes(&store, changes);
	check(count == 0U, "sound: nothing to write before audiod reports (%u)", count);
	kwl_settings_store_report(&store, "sound.volume", "40");
	kwl_settings_store_report(&store, "sound.muted", "0");
	count = kwl_settings_store_changes(&store, changes);
	check(count == 0U, "sound: audiod at the kept volume: nothing to write");
	kwl_settings_store_report(&store, "sound.volume", "65");
	kwl_settings_store_report(&store, "sound.available", "1");
	(void)kwl_settings_store_finish(&store);
	check(file_has("sound.volume=65") && file_has("sound.muted=0") && strstr(get_file(), "sound.available") == NULL,
	      "sound: the session's volume is kept, sound.available is never written");
	kwl_settings_store_close(&store);
}
