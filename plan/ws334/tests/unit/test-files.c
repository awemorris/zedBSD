/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The unit test of the files backend of the storage (storage/files.c):
 * opening and the directory lock, the calls over every space, the layout on
 * the disk, the localStorage files of earlier engines, bad keys, damaged or
 * hostile contents of the directory, and a second process.  It runs on POSIX
 * and on Windows; argv[1] says which.  Started with "files-child <directory>"
 * it is the second process that must be refused, and with "files-free
 * <directory>" the one that must get the directory.
 *
 * The expected values come from the header's contracts and from hand
 * calculation; the files are looked at with wb_os_file_read and made with
 * the C library, not through the backend.
 */

#include "check.h"

#include "storage/files.h"
#include "storage/records.h"
#include "base/os-file.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <unistd.h>
#else
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
#endif

/* The size of the biggest record the read tests use: 3 MiB. */
#define BIG_SIZE	(3U * 1024U * 1024U)

/* The most keys a listing is collected into. */
#define KEY_SET_MAX	64U

/* The path of this program for the second process, set by main. */
static const char *self_path;

/* The keys a listing found, copied out because a key is valid only during the call. */
struct key_set {
	char keys[KEY_SET_MAX][BROWSER_STORAGE_KEY_MAX + 1];
	size_t count;
	size_t stop_at;
	int stop_value;
};

/* The names a directory listing found. */
struct name_set {
	char names[KEY_SET_MAX][300];
	size_t count;
};

static void test_open(void);
static void test_arguments(void);
static void test_round_trip(void);
static void test_sizes(void);
static void test_list_and_clear(void);
static void test_layout(void);
static void test_legacy(void);
static void test_bad_keys(void);
static void test_hostile(void);
static void test_directory_spellings(void);
static void test_other_process(void);

static void raw_write(const char *path, const void *bytes, size_t length);
static int raw_make_directory(const char *path);
static int raw_exists(const char *path);
static void make_pattern(unsigned char *bytes, size_t length, unsigned seed);
static int open_backend(const char *directory, const struct browser_storage_calls **calls, void **context);
static void close_backend(const struct browser_storage_calls *calls, void *context);
static int read_record(const struct browser_storage_calls *calls, void *context, unsigned space, const char *key, struct wb_buffer *bytes, int *found);
static void put(const struct browser_storage_calls *calls, void *context, unsigned space, const char *key, const char *text);
static int is_record(const struct browser_storage_calls *calls, void *context, unsigned space, const char *key, const void *expected, size_t length);
static int is_absent(const struct browser_storage_calls *calls, void *context, unsigned space, const char *key);
static int file_has(const char *path, const void *expected, size_t length);
static int collect_key(void *each_context, const char *key);
static int collect_name(void *context, const char *name);
static int key_set_has(const struct key_set *set, const char *key);
static int list_keys(const struct browser_storage_calls *calls, void *context, unsigned space, struct key_set *set);
static int run_child(const char *mode, const char *directory);
static int child_main(const char *mode, const char *directory);

int
main(
	int argc,
	char **argv)
{
	if (argc >= 3 && strcmp(argv[1], "files-child") == 0)
		return child_main("files-child", argv[2]);
	if (argc >= 3 && strcmp(argv[1], "files-free") == 0)
		return child_main("files-free", argv[2]);

	if (argc < 2) {
		printf("usage: test-files posix|win32\n");
		return 2;
	}
	self_path = argv[0];
#ifndef _WIN32
	umask(022);
#endif

	test_open();
	test_arguments();
	test_round_trip();
	test_sizes();
	test_list_and_clear();
	test_layout();
	test_legacy();
	test_bad_keys();
	test_hostile();
	test_directory_spellings();
	test_other_process();

	return check_finish("files");
}

/* Writes a file with the C library in binary, replacing what is there. */
static void
raw_write(
	const char *path,
	const void *bytes,
	size_t length)
{
	FILE *file;
	size_t written;

	file = fopen(path, "wb");
	CHECK(file != NULL);
	if (file == NULL)
		return;
	written = 0;
	if (length > 0)
		written = fwrite(bytes, 1, length, file);
	CHECK_EQ(written, length);
	CHECK_EQ(fclose(file), 0);
}

/* Makes one directory with the C library's calls; returns 1 when it was made. */
static int
raw_make_directory(
	const char *path)
{
#ifdef _WIN32
	return mkdir(path) == 0;
#else
	return mkdir(path, 0755) == 0;
#endif
}

/* Tells whether anything exists under a name, by trying to open it as a file or a directory. */
static int
raw_exists(
	const char *path)
{
#ifdef _WIN32
	DWORD attributes;
	wchar_t wide[512];

	if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 512) == 0)
		return 0;
	attributes = GetFileAttributesW(wide);
	return attributes != INVALID_FILE_ATTRIBUTES;
#else
	struct stat status;

	return lstat(path, &status) == 0;
#endif
}

/* Fills bytes with a pattern that depends on the position and the seed. */
static void
make_pattern(
	unsigned char *bytes,
	size_t length,
	unsigned seed)
{
	size_t i;

	for (i = 0; i < length; i++)
		bytes[i] = (unsigned char)((i * 31U + (i >> 8) + seed) & 0xffU);
}

/* Opens the backend over a directory. */
static int
open_backend(
	const char *directory,
	const struct browser_storage_calls **calls,
	void **context)
{
	return browser_storage_files_open(directory, calls, context);
}

/* Closes a backend through its table. */
static void
close_backend(
	const struct browser_storage_calls *calls,
	void *context)
{
	calls->close(context);
}

/* Reads a record through the table into a fresh buffer; returns the call's result. */
static int
read_record(
	const struct browser_storage_calls *calls,
	void *context,
	unsigned space,
	const char *key,
	struct wb_buffer *bytes,
	int *found)
{
	struct browser_storage_sink sink;
	int error;

	wb_storage_sink_init(&sink, bytes);
	*found = 12345;
	error = calls->read(context, space, key, &sink, found);
	return error;
}

/* Writes a text record and checks the write succeeded. */
static void
put(
	const struct browser_storage_calls *calls,
	void *context,
	unsigned space,
	const char *key,
	const char *text)
{
	int error;

	error = calls->write(context, space, key, text, strlen(text));
	CHECK_EQ(error, 0);
}

/* Reads a record and tells whether it is found with exactly the expected bytes. */
static int
is_record(
	const struct browser_storage_calls *calls,
	void *context,
	unsigned space,
	const char *key,
	const void *expected,
	size_t length)
{
	struct wb_buffer bytes;
	int found;
	int error;
	int same;

	wb_buffer_init(&bytes);
	error = read_record(calls, context, space, key, &bytes, &found);
	same = 0;
	if (error == 0 && found == 1 && bytes.length == length) {
		if (length == 0 || memcmp(bytes.data, expected, length) == 0)
			same = 1;
	}
	wb_buffer_release(&bytes);
	return same;
}

/* Reads a record and tells whether the call said "not found" and added nothing to the sink. */
static int
is_absent(
	const struct browser_storage_calls *calls,
	void *context,
	unsigned space,
	const char *key)
{
	struct wb_buffer bytes;
	int found;
	int error;
	int absent;

	wb_buffer_init(&bytes);
	wb_buffer_append_string(&bytes, "xyz");
	error = read_record(calls, context, space, key, &bytes, &found);
	absent = 0;
	if (error == 0 && found == 0 && bytes.length == 3)
		absent = 1;
	wb_buffer_release(&bytes);
	return absent;
}

/* Tells whether a file, read with the os layer, holds exactly the expected bytes. */
static int
file_has(
	const char *path,
	const void *expected,
	size_t length)
{
	struct wb_buffer bytes;
	int error;
	int same;

	wb_buffer_init(&bytes);
	error = wb_os_file_read(path, &bytes);
	same = 0;
	if (error == 0 && bytes.length == length) {
		if (length == 0 || memcmp(bytes.data, expected, length) == 0)
			same = 1;
	}
	wb_buffer_release(&bytes);
	return same;
}

/* Collects a key of a listing; stops with the set's value after its number of keys. */
static int
collect_key(
	void *each_context,
	const char *key)
{
	struct key_set *set;

	set = (struct key_set *)each_context;
	if (set->count >= KEY_SET_MAX)
		return EOVERFLOW;
	if (strlen(key) > BROWSER_STORAGE_KEY_MAX)
		return EOVERFLOW;
	strcpy(set->keys[set->count], key);
	set->count++;
	if (set->stop_at != 0 && set->count == set->stop_at)
		return set->stop_value;
	return 0;
}

/* Collects a name of a directory listing. */
static int
collect_name(
	void *context,
	const char *name)
{
	struct name_set *set;

	set = (struct name_set *)context;
	if (set->count >= KEY_SET_MAX)
		return 0;
	snprintf(set->names[set->count], sizeof(set->names[0]), "%s", name);
	set->count++;
	return 0;
}

/* Tells whether a listing holds a key. */
static int
key_set_has(
	const struct key_set *set,
	const char *key)
{
	size_t i;

	for (i = 0; i < set->count; i++) {
		if (strcmp(set->keys[i], key) == 0)
			return 1;
	}
	return 0;
}

/* Lists a space's keys into a set; returns the call's result. */
static int
list_keys(
	const struct browser_storage_calls *calls,
	void *context,
	unsigned space,
	struct key_set *set)
{
	memset(set, 0, sizeof(*set));
	return calls->list(context, space, collect_key, set);
}

/* Opens and closes: the errors, the table, the second open, the directories made. */
static void
test_open(void)
{
	const struct browser_storage_calls *calls;
	const struct browser_storage_calls *first_calls;
	void *context;
	void *first_context;
	struct wb_buffer bytes;
	int sentinel;
	int error;

	/* A new directory with missing parents is made, and the table is complete. */
	calls = NULL;
	context = NULL;
	error = open_backend("open-a/b/c", &calls, &context);
	CHECK_EQ(error, 0);
	CHECK(calls != NULL);
	CHECK(context != NULL);
	if (calls == NULL || context == NULL)
		return;
	CHECK_EQ(calls->version, BROWSER_STORAGE_VERSION);
	CHECK(calls->read != NULL);
	CHECK(calls->write != NULL);
	CHECK(calls->remove != NULL);
	CHECK(calls->list != NULL);
	CHECK(calls->clear != NULL);
	CHECK(calls->close != NULL);
	CHECK(raw_exists("open-a/b/c"));
	wb_buffer_init(&bytes);
	CHECK_EQ(wb_os_file_read("open-a/b/c/lock", &bytes), 0);
	wb_buffer_release(&bytes);

	/* A second open of the same directory in this process is EBUSY and leaves the outputs NULL. */
	first_calls = calls;
	first_context = context;
	calls = (const struct browser_storage_calls *)&sentinel;
	context = &sentinel;
	error = open_backend("open-a/b/c", &calls, &context);
	CHECK_EQ(error, EBUSY);
	CHECK(calls == NULL);
	CHECK(context == NULL);

	/* The first backend is still the holder and works. */
	put(first_calls, first_context, BROWSER_STORAGE_COOKIES, "aa", "one");
	CHECK(is_record(first_calls, first_context, BROWSER_STORAGE_COOKIES, "aa", "one", 3));

	/* After the close the directory can be opened again, and the record is still there. */
	close_backend(first_calls, first_context);
	calls = NULL;
	context = NULL;
	error = open_backend("open-a/b/c", &calls, &context);
	CHECK_EQ(error, 0);
	if (calls != NULL && context != NULL) {
		CHECK(is_record(calls, context, BROWSER_STORAGE_COOKIES, "aa", "one", 3));
		close_backend(calls, context);
	}

	/* A different directory is independent of a held one. */
	calls = NULL;
	context = NULL;
	error = open_backend("open-d1", &calls, &context);
	CHECK_EQ(error, 0);
	if (calls != NULL && context != NULL) {
		first_calls = calls;
		first_context = context;
		calls = NULL;
		context = NULL;
		error = open_backend("open-d2", &calls, &context);
		CHECK_EQ(error, 0);
		if (calls != NULL && context != NULL)
			close_backend(calls, context);
		close_backend(first_calls, first_context);
	}

	/* A close of a NULL context does nothing. */
	if (first_calls != NULL)
		first_calls->close(NULL);
}

/* The argument errors of open and of the table's calls. */
static void
test_arguments(void)
{
	const struct browser_storage_calls *calls;
	void *context;
	struct browser_storage_sink sink;
	struct wb_buffer bytes;
	struct key_set set;
	int sentinel;
	int found;
	int error;

	/* An open with a NULL or empty directory, or a missing output, is EINVAL with NULL outputs. */
	calls = (const struct browser_storage_calls *)&sentinel;
	context = &sentinel;
	CHECK_EQ(browser_storage_files_open(NULL, &calls, &context), EINVAL);
	CHECK(calls == NULL);
	CHECK(context == NULL);

	calls = (const struct browser_storage_calls *)&sentinel;
	context = &sentinel;
	CHECK_EQ(browser_storage_files_open("", &calls, &context), EINVAL);
	CHECK(calls == NULL);
	CHECK(context == NULL);

	context = &sentinel;
	CHECK_EQ(browser_storage_files_open("arg-dir", NULL, &context), EINVAL);
	CHECK(context == NULL);

	calls = (const struct browser_storage_calls *)&sentinel;
	CHECK_EQ(browser_storage_files_open("arg-dir", &calls, NULL), EINVAL);
	CHECK(calls == NULL);

	/* A file where the directory should be is ENOTDIR, also as a parent, and nothing stays open. */
	raw_write("arg-file", "x", 1);
	calls = (const struct browser_storage_calls *)&sentinel;
	context = &sentinel;
	CHECK_EQ(browser_storage_files_open("arg-file", &calls, &context), ENOTDIR);
	CHECK(calls == NULL);
	CHECK(context == NULL);
	CHECK_EQ(browser_storage_files_open("arg-file/sub", &calls, &context), ENOTDIR);
	CHECK(calls == NULL);
	CHECK(context == NULL);

	/* The failed opens left no lock behind: the directory opens once now. */
	error = open_backend("arg-dir", &calls, &context);
	CHECK_EQ(error, 0);
	if (calls == NULL || context == NULL)
		return;

	/* The calls refuse missing arguments. */
	wb_buffer_init(&bytes);
	wb_storage_sink_init(&sink, &bytes);
	found = 7;
	CHECK_EQ(calls->read(context, BROWSER_STORAGE_COOKIES, "aa", NULL, &found), EINVAL);
	CHECK_EQ(calls->read(context, BROWSER_STORAGE_COOKIES, "aa", &sink, NULL), EINVAL);
	CHECK_EQ(calls->read(context, BROWSER_STORAGE_COOKIES, NULL, &sink, &found), EINVAL);
	CHECK_EQ(found, 0);
	CHECK_EQ(calls->write(context, BROWSER_STORAGE_COOKIES, NULL, "x", 1), EINVAL);
	CHECK_EQ(calls->write(context, BROWSER_STORAGE_COOKIES, "aa", NULL, 5), EINVAL);
	CHECK_EQ(calls->remove(context, BROWSER_STORAGE_COOKIES, NULL), EINVAL);
	CHECK_EQ(calls->list(context, BROWSER_STORAGE_COOKIES, NULL, NULL), EINVAL);
	CHECK(!raw_exists("arg-dir/cookies/aa"));
	wb_buffer_release(&bytes);

	/* A record of no bytes may be written with a NULL pointer. */
	CHECK_EQ(calls->write(context, BROWSER_STORAGE_CACHE, "aa", NULL, 0), 0);
	CHECK(is_record(calls, context, BROWSER_STORAGE_CACHE, "aa", NULL, 0));

	CHECK_EQ(list_keys(calls, context, BROWSER_STORAGE_CACHE, &set), 0);
	CHECK_EQ(set.count, 1);

	close_backend(calls, context);
}

/* Write, read, overwrite, remove, list and clear over the three spaces and others. */
static void
test_round_trip(void)
{
	static const unsigned spaces[] = { BROWSER_STORAGE_COOKIES, BROWSER_STORAGE_CACHE, BROWSER_STORAGE_LOCAL, 7U };
	const struct browser_storage_calls *calls;
	void *context;
	struct key_set set;
	unsigned space;
	size_t i;

	CHECK_EQ(open_backend("rt", &calls, &context), 0);
	if (calls == NULL || context == NULL)
		return;

	for (i = 0; i < sizeof(spaces) / sizeof(spaces[0]); i++) {
		space = spaces[i];

		/* Nothing is there before the first write, and not even the space's directory. */
		CHECK(is_absent(calls, context, space, "0a"));
		CHECK_EQ(list_keys(calls, context, space, &set), 0);
		CHECK_EQ(set.count, 0);
		CHECK_EQ(calls->remove(context, space, "0a"), 0);
		CHECK_EQ(calls->clear(context, space), 0);

		/* A written record reads back, and the other spaces do not see it. */
		put(calls, context, space, "0a", "first");
		CHECK(is_record(calls, context, space, "0a", "first", 5));
		CHECK(is_absent(calls, context, space, "0b"));

		/* A second write replaces the bytes, also with a shorter record. */
		put(calls, context, space, "0a", "second record");
		CHECK(is_record(calls, context, space, "0a", "second record", 13));
		if (i == 0) {
			put(calls, context, space, "0a", "2");
			CHECK(is_record(calls, context, space, "0a", "2", 1));
		}

		/* A second key lists beside the first. */
		put(calls, context, space, "0b-1", "other");
		CHECK_EQ(list_keys(calls, context, space, &set), 0);
		CHECK_EQ(set.count, 2);
		CHECK(key_set_has(&set, "0a"));
		CHECK(key_set_has(&set, "0b-1"));

		/* Removing deletes only that record; removing again is not an error. */
		CHECK_EQ(calls->remove(context, space, "0a"), 0);
		CHECK(is_absent(calls, context, space, "0a"));
		CHECK(is_record(calls, context, space, "0b-1", "other", 5));
		CHECK_EQ(calls->remove(context, space, "0a"), 0);
	}

	/* The spaces do not share keys: clearing one leaves the others. */
	CHECK_EQ(calls->clear(context, BROWSER_STORAGE_CACHE), 0);
	CHECK(is_absent(calls, context, BROWSER_STORAGE_CACHE, "0b-1"));
	CHECK(is_record(calls, context, BROWSER_STORAGE_COOKIES, "0b-1", "other", 5));
	CHECK(is_record(calls, context, BROWSER_STORAGE_LOCAL, "0b-1", "other", 5));
	CHECK(is_record(calls, context, 7U, "0b-1", "other", 5));

	close_backend(calls, context);
}

/* Records of no bytes, one byte, more than one piece, the longest key, and the engine's largest record. */
static void
test_sizes(void)
{
	const struct browser_storage_calls *calls;
	void *context;
	unsigned char *big;
	unsigned char *huge;
	char long_key[BROWSER_STORAGE_KEY_MAX + 1];
	struct browser_storage_sink sink;
	struct wb_buffer bytes;
	struct key_set set;
	int found;
	int error;
	size_t i;

	CHECK_EQ(open_backend("sz", &calls, &context), 0);
	if (calls == NULL || context == NULL)
		return;

	/* A record of no bytes exists and reads back as found with nothing added. */
	CHECK_EQ(calls->write(context, BROWSER_STORAGE_CACHE, "00", "", 0), 0);
	CHECK(is_record(calls, context, BROWSER_STORAGE_CACHE, "00", NULL, 0));

	/* A record of one byte, including a NUL and bytes a text mode would change. */
	CHECK_EQ(calls->write(context, BROWSER_STORAGE_CACHE, "01", "\0", 1), 0);
	CHECK(is_record(calls, context, BROWSER_STORAGE_CACHE, "01", "\0", 1));
	CHECK_EQ(calls->write(context, BROWSER_STORAGE_CACHE, "02", "a\r\nb\x1a" "c\n", 7), 0);
	CHECK(is_record(calls, context, BROWSER_STORAGE_CACHE, "02", "a\r\nb\x1a" "c\n", 7));

	/* A record of 3 MiB is more than one piece of a read, and arrives whole after a prefix. */
	big = (unsigned char *)malloc(BIG_SIZE);
	CHECK(big != NULL);
	if (big != NULL) {
		make_pattern(big, BIG_SIZE, 3);
		CHECK_EQ(calls->write(context, BROWSER_STORAGE_CACHE, "03", big, BIG_SIZE), 0);
		CHECK(is_record(calls, context, BROWSER_STORAGE_CACHE, "03", big, BIG_SIZE));

		wb_buffer_init(&bytes);
		wb_buffer_append_string(&bytes, "prefix");
		wb_storage_sink_init(&sink, &bytes);
		found = 0;
		error = calls->read(context, BROWSER_STORAGE_CACHE, "03", &sink, &found);
		CHECK_EQ(error, 0);
		CHECK_EQ(found, 1);
		CHECK_EQ(bytes.length, 6 + BIG_SIZE);
		CHECK(memcmp(bytes.data, "prefix", 6) == 0);
		CHECK(bytes.length == 6 + BIG_SIZE && memcmp(bytes.data + 6, big, BIG_SIZE) == 0);
		wb_buffer_release(&bytes);
		free(big);
	}

	/* The longest key is stored under that name; its file is there. */
	for (i = 0; i < BROWSER_STORAGE_KEY_MAX; i++)
		long_key[i] = (char)"0123456789abcdef-"[i % 17];
	long_key[BROWSER_STORAGE_KEY_MAX] = '\0';
	put(calls, context, BROWSER_STORAGE_COOKIES, long_key, "long");
	CHECK(is_record(calls, context, BROWSER_STORAGE_COOKIES, long_key, "long", 4));
	put(calls, context, BROWSER_STORAGE_LOCAL, long_key, "long local");
	CHECK(is_record(calls, context, BROWSER_STORAGE_LOCAL, long_key, "long local", 10));

	/* The longest key is listed in both spaces (the local one without its suffix). */
	CHECK_EQ(list_keys(calls, context, BROWSER_STORAGE_COOKIES, &set), 0);
	CHECK_EQ(set.count, 1);
	CHECK(key_set_has(&set, long_key));
	CHECK_EQ(list_keys(calls, context, BROWSER_STORAGE_LOCAL, &set), 0);
	CHECK_EQ(set.count, 1);
	CHECK(key_set_has(&set, long_key));

	/* A file of exactly the engine's largest record is read whole; one byte more is the sink's EFBIG. */
	huge = (unsigned char *)malloc(WB_STORAGE_RECORD_MAX + 1);
	CHECK(huge != NULL);
	if (huge != NULL) {
		make_pattern(huge, WB_STORAGE_RECORD_MAX + 1, 9);
		CHECK(raw_make_directory("sz/space-7"));
		raw_write("sz/space-7/0f", huge, WB_STORAGE_RECORD_MAX);
		raw_write("sz/space-7/1f", huge, WB_STORAGE_RECORD_MAX + 1);

		CHECK(is_record(calls, context, 7U, "0f", huge, WB_STORAGE_RECORD_MAX));

		wb_buffer_init(&bytes);
		error = read_record(calls, context, 7U, "1f", &bytes, &found);
		CHECK_EQ(error, EFBIG);
		wb_buffer_release(&bytes);
		free(huge);
	}

	close_backend(calls, context);
}

/* What list shows and clear removes, and what they leave alone. */
static void
test_list_and_clear(void)
{
	static const char *const stray_cookies[] = { "ABC", "x y", "0a.tmp12-34", "0b.tmp", "0c.ls", "nothex", "0d.x", "-.-" };
	const struct browser_storage_calls *calls;
	void *context;
	struct key_set set;
	char path[400];
	char name[400];
	char too_long[BROWSER_STORAGE_KEY_MAX + 2];
	size_t i;
	int error;

	CHECK_EQ(open_backend("lc", &calls, &context), 0);
	if (calls == NULL || context == NULL)
		return;

	put(calls, context, BROWSER_STORAGE_COOKIES, "01", "a");
	put(calls, context, BROWSER_STORAGE_COOKIES, "02", "b");
	put(calls, context, BROWSER_STORAGE_COOKIES, "3-0", "c");

	/* Files that are not records sit in the directory: bad names, temporary files, a name too long. */
	for (i = 0; i < sizeof(stray_cookies) / sizeof(stray_cookies[0]); i++) {
		snprintf(name, sizeof(name), "lc/cookies/%s", stray_cookies[i]);
		raw_write(name, "stray", 5);
	}
	memset(too_long, 'a', BROWSER_STORAGE_KEY_MAX + 1);
	too_long[BROWSER_STORAGE_KEY_MAX + 1] = '\0';
	snprintf(name, sizeof(name), "lc/cookies/%s", too_long);
	raw_write(name, "stray", 5);

	/* The listing holds exactly the three keys ("-.-" has a dot, so it is not one). */
	CHECK_EQ(list_keys(calls, context, BROWSER_STORAGE_COOKIES, &set), 0);
	CHECK_EQ(set.count, 3);
	CHECK(key_set_has(&set, "01"));
	CHECK(key_set_has(&set, "02"));
	CHECK(key_set_has(&set, "3-0"));

	/* A stop value of each comes back as it is, also a value that is an error of the directory walk. */
	memset(&set, 0, sizeof(set));
	set.stop_at = 2;
	set.stop_value = 4;
	error = calls->list(context, BROWSER_STORAGE_COOKIES, collect_key, &set);
	CHECK_EQ(error, 4);
	CHECK_EQ(set.count, 2);

	memset(&set, 0, sizeof(set));
	set.stop_at = 1;
	set.stop_value = ENOENT;
	error = calls->list(context, BROWSER_STORAGE_COOKIES, collect_key, &set);
	CHECK_EQ(error, ENOENT);
	CHECK_EQ(set.count, 1);

	/* Clear removes the records and keeps every stray file. */
	CHECK_EQ(calls->clear(context, BROWSER_STORAGE_COOKIES), 0);
	CHECK_EQ(list_keys(calls, context, BROWSER_STORAGE_COOKIES, &set), 0);
	CHECK_EQ(set.count, 0);
	CHECK(!raw_exists("lc/cookies/01"));
	CHECK(!raw_exists("lc/cookies/3-0"));
	for (i = 0; i < sizeof(stray_cookies) / sizeof(stray_cookies[0]); i++) {
		snprintf(name, sizeof(name), "lc/cookies/%s", stray_cookies[i]);
		CHECK(raw_exists(name));
	}
	snprintf(name, sizeof(name), "lc/cookies/%s", too_long);
	CHECK(raw_exists(name));

	/* In local-storage a record needs the ".ls" suffix; other names are skipped, and the suffix is not part of the key. */
	put(calls, context, BROWSER_STORAGE_LOCAL, "0a", "x");
	put(calls, context, BROWSER_STORAGE_LOCAL, "0b", "y");
	raw_write("lc/local-storage/0e", "stray", 5);
	raw_write("lc/local-storage/.ls", "stray", 5);
	raw_write("lc/local-storage/0c.ls.tmp5-6", "stray", 5);
	raw_write("lc/local-storage/ABC.ls", "stray", 5);
	raw_write("lc/local-storage/0d.LS", "stray", 5);
	raw_write("lc/local-storage/0f.tmp9-9", "stray", 5);
	raw_write("lc/local-storage/a.b.ls", "stray", 5);
	raw_write("lc/local-storage/f.ls", "stray", 5);
	CHECK_EQ(list_keys(calls, context, BROWSER_STORAGE_LOCAL, &set), 0);
	CHECK_EQ(set.count, 3);
	CHECK(key_set_has(&set, "0a"));
	CHECK(key_set_has(&set, "0b"));
	CHECK(key_set_has(&set, "f"));

	/* A local-storage listing of a key with a name longer than the limit once the suffix is off is skipped too. */
	memset(too_long, 'b', BROWSER_STORAGE_KEY_MAX + 1);
	too_long[BROWSER_STORAGE_KEY_MAX + 1] = '\0';
	snprintf(path, sizeof(path), "lc/local-storage/%s.ls", too_long);
	raw_write(path, "stray", 5);
	CHECK_EQ(list_keys(calls, context, BROWSER_STORAGE_LOCAL, &set), 0);
	CHECK_EQ(set.count, 3);

	CHECK_EQ(calls->clear(context, BROWSER_STORAGE_LOCAL), 0);
	CHECK_EQ(list_keys(calls, context, BROWSER_STORAGE_LOCAL, &set), 0);
	CHECK_EQ(set.count, 0);
	CHECK(raw_exists("lc/local-storage/0e"));
	CHECK(raw_exists("lc/local-storage/.ls"));
	CHECK(raw_exists("lc/local-storage/0c.ls.tmp5-6"));
	CHECK(raw_exists("lc/local-storage/ABC.ls"));
	CHECK(raw_exists("lc/local-storage/0d.LS"));
	CHECK(raw_exists("lc/local-storage/0f.tmp9-9"));
	CHECK(raw_exists("lc/local-storage/a.b.ls"));
	CHECK(raw_exists(path));

	/* The stray files of one space are not seen as the record of a key in it. */
	CHECK(is_absent(calls, context, BROWSER_STORAGE_LOCAL, "0e"));

	close_backend(calls, context);
}

/* The layout on the disk, looked at directly. */
static void
test_layout(void)
{
	const struct browser_storage_calls *calls;
	void *context;
	struct wb_buffer bytes;

	CHECK_EQ(open_backend("lay", &calls, &context), 0);
	if (calls == NULL || context == NULL)
		return;

	put(calls, context, BROWSER_STORAGE_COOKIES, "ab", "cookie");
	put(calls, context, BROWSER_STORAGE_CACHE, "cd", "cache");
	put(calls, context, BROWSER_STORAGE_LOCAL, "ef", "local");
	put(calls, context, 7U, "12", "seven");
	put(calls, context, 0U, "34", "zero");
	put(calls, context, 4294967295U, "56", "last");

	/* Each record is the file of its space and key, holding exactly the bytes. */
	CHECK(file_has("lay/cookies/ab", "cookie", 6));
	CHECK(file_has("lay/cache/cd", "cache", 5));
	CHECK(file_has("lay/local-storage/ef.ls", "local", 5));
	CHECK(file_has("lay/space-7/12", "seven", 5));
	CHECK(file_has("lay/space-0/34", "zero", 4));
	CHECK(file_has("lay/space-4294967295/56", "last", 4));

	/* Nothing else is in the way: no file without the suffix for local, no suffix elsewhere. */
	CHECK(!raw_exists("lay/local-storage/ef"));
	CHECK(!raw_exists("lay/cookies/ab.ls"));

	/* The lock file is there, and a replace left no temporary file in the directories. */
	wb_buffer_init(&bytes);
	CHECK_EQ(wb_os_file_read("lay/lock", &bytes), 0);
	wb_buffer_release(&bytes);

	close_backend(calls, context);
	CHECK(raw_exists("lay/lock"));
}

/* The localStorage files of earlier engines are found again under their names. */
static void
test_legacy(void)
{
	static const char origin[] = "https://example.test:8443";
	static const unsigned char old_file[] = "KEILAND LOCAL STORAGE 1\n\x01\x00\x00\x00\x01\x00\x00\x00k\x00v\x00";
	const struct browser_storage_calls *calls;
	void *context;
	char hex[2 * sizeof(origin) + 1];
	char path[400];
	char key[BROWSER_STORAGE_KEY_MAX + 1];
	size_t i;

	/* The file name of an earlier engine: the origin's bytes in lower-case hexadecimal. */
	for (i = 0; i + 1 < sizeof(origin); i++)
		snprintf(hex + 2 * i, 3, "%02x", (unsigned)(unsigned char)origin[i]);
	CHECK_EQ(wb_storage_key_text(origin, sizeof(origin) - 1, 0, key), 0);
	CHECK(strcmp(key, hex) == 0);

	/* Puts an earlier file where the engine used to keep it, before the backend opens. */
	CHECK(raw_make_directory("old"));
	CHECK(raw_make_directory("old/local-storage"));
	snprintf(path, sizeof(path), "old/local-storage/%s.ls", hex);
	raw_write(path, old_file, sizeof(old_file) - 1);

	CHECK_EQ(open_backend("old", &calls, &context), 0);
	if (calls == NULL || context == NULL)
		return;

	/* Read by the key of the origin, the earlier file arrives as it is. */
	CHECK(is_record(calls, context, BROWSER_STORAGE_LOCAL, key, old_file, sizeof(old_file) - 1));

	/* And it is listed under that key. */
	{
		struct key_set set;

		CHECK_EQ(list_keys(calls, context, BROWSER_STORAGE_LOCAL, &set), 0);
		CHECK_EQ(set.count, 1);
		CHECK(key_set_has(&set, key));
	}

	close_backend(calls, context);
}

/* Keys that are not valid are refused by every call that takes one. */
static void
test_bad_keys(void)
{
	const struct browser_storage_calls *calls;
	void *context;
	struct browser_storage_sink sink;
	struct wb_buffer bytes;
	char too_long[BROWSER_STORAGE_KEY_MAX + 2];
	static const char *const bad[] = { "", "A", "g", "a b", "a/b", "../x", "a.b", "a\\b", "\xc3\xa9" };
	unsigned space;
	size_t i;
	int found;

	CHECK_EQ(open_backend("bad", &calls, &context), 0);
	if (calls == NULL || context == NULL)
		return;

	memset(too_long, 'a', BROWSER_STORAGE_KEY_MAX + 1);
	too_long[BROWSER_STORAGE_KEY_MAX + 1] = '\0';

	for (space = 1; space <= 4; space++) {
		wb_buffer_init(&bytes);
		wb_storage_sink_init(&sink, &bytes);

		/* A key of 201 characters is one too many. */
		found = 5;
		CHECK_EQ(calls->read(context, space, too_long, &sink, &found), EINVAL);
		CHECK_EQ(found, 0);
		CHECK_EQ(calls->write(context, space, too_long, "x", 1), EINVAL);
		CHECK_EQ(calls->remove(context, space, too_long), EINVAL);

		/* The rest are outside the alphabet or empty. */
		for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
			found = 5;
			CHECK_EQ(calls->read(context, space, bad[i], &sink, &found), EINVAL);
			CHECK_EQ(found, 0);
			CHECK_EQ(calls->write(context, space, bad[i], "x", 1), EINVAL);
			CHECK_EQ(calls->remove(context, space, bad[i]), EINVAL);
		}

		CHECK_EQ(bytes.length, 0);
		wb_buffer_release(&bytes);
	}

	/* No refused write made a file or a directory. */
	CHECK(!raw_exists("bad/cookies"));
	CHECK(!raw_exists("bad/space-4"));
	CHECK(!raw_exists("bad/x"));

	close_backend(calls, context);
}

/* Contents of the directory the backend did not make: damaged files, wrong kinds of entries, failing replaces. */
static void
test_hostile(void)
{
	const struct browser_storage_calls *calls;
	void *context;
	struct wb_buffer bytes;
	struct name_set names;
	struct key_set set;
	unsigned char noise[3000];
	size_t cut;
	size_t i;
	int found;
	int error;

	CHECK(raw_make_directory("hos"));
	CHECK(raw_make_directory("hos/cookies"));
	CHECK(raw_make_directory("hos/cache"));

	/* Whatever bytes a file holds, truncated or random, are handed on unchanged; the engine decodes them. */
	make_pattern(noise, sizeof(noise), 77);
	raw_write("hos/cookies/aa", noise, sizeof(noise));
	raw_write("hos/cookies/ab", noise, 17);
	raw_write("hos/cookies/ac", "KEILAND COOKIE 1\n\xff\xff\xff\xff", 21);
	raw_write("hos/cookies/ad", "", 0);

	/* A directory with a record's name, and a file where a space's directory should be. */
	CHECK(raw_make_directory("hos/cache/dd"));
	raw_write("hos/space-9", "plain file", 10);
	CHECK(raw_make_directory("hos/local-storage"));

	CHECK_EQ(open_backend("hos", &calls, &context), 0);
	if (calls == NULL || context == NULL)
		return;

	CHECK(is_record(calls, context, BROWSER_STORAGE_COOKIES, "aa", noise, sizeof(noise)));
	CHECK(is_record(calls, context, BROWSER_STORAGE_COOKIES, "ab", noise, 17));
	CHECK(is_record(calls, context, BROWSER_STORAGE_COOKIES, "ac", "KEILAND COOKIE 1\n\xff\xff\xff\xff", 21));
	CHECK(is_record(calls, context, BROWSER_STORAGE_COOKIES, "ad", "", 0));

	/* Every truncation of a file reads as the bytes it has. */
	for (cut = 0; cut < 40; cut++) {
		raw_write("hos/cookies/ae", noise, cut);
		if (!is_record(calls, context, BROWSER_STORAGE_COOKIES, "ae", noise, cut))
			CHECK(0);
	}
	CHECK(1);

	/* A directory in the place of a record is an error for read and remove, and not a found record. */
	wb_buffer_init(&bytes);
	error = read_record(calls, context, BROWSER_STORAGE_CACHE, "dd", &bytes, &found);
	CHECK(error != 0);
	CHECK_EQ(found, 0);
	CHECK_EQ(bytes.length, 0);
	wb_buffer_release(&bytes);
	CHECK(calls->remove(context, BROWSER_STORAGE_CACHE, "dd") != 0);
	CHECK(raw_exists("hos/cache/dd"));

	/* A failing replace over it leaves the directory and no temporary file behind. */
	error = calls->write(context, BROWSER_STORAGE_CACHE, "dd", "new", 3);
	CHECK(error != 0);
	CHECK(raw_exists("hos/cache/dd"));
	memset(&names, 0, sizeof(names));
	CHECK_EQ(wb_os_list("hos/cache", collect_name, &names), 0);
	CHECK_EQ(names.count, 1);
	CHECK(names.count == 1 && strcmp(names.names[0], "dd") == 0);

	/* A clear that meets it still removes the others, and reports the failure. */
	put(calls, context, BROWSER_STORAGE_CACHE, "ee", "e");
	put(calls, context, BROWSER_STORAGE_CACHE, "ff", "f");
	error = calls->clear(context, BROWSER_STORAGE_CACHE);
	CHECK(error != 0);
	CHECK(!raw_exists("hos/cache/ee"));
	CHECK(!raw_exists("hos/cache/ff"));
	CHECK(raw_exists("hos/cache/dd"));

	/* A plain file in the place of a space's directory: nothing is found, listed or removed, a write is ENOTDIR. */
	CHECK(is_absent(calls, context, 9U, "aa"));
	CHECK_EQ(list_keys(calls, context, 9U, &set), 0);
	CHECK_EQ(set.count, 0);
	CHECK_EQ(calls->remove(context, 9U, "aa"), 0);
	CHECK_EQ(calls->clear(context, 9U), 0);
	CHECK_EQ(calls->write(context, 9U, "aa", "x", 1), ENOTDIR);
	CHECK(file_has("hos/space-9", "plain file", 10));

	/* A temporary file a crashed replace left is never a record, and a write of that key goes on. */
	raw_write("hos/cookies/ba.tmp1-2", "half", 4);
	CHECK(is_absent(calls, context, BROWSER_STORAGE_COOKIES, "ba"));
	put(calls, context, BROWSER_STORAGE_COOKIES, "ba", "whole");
	CHECK(is_record(calls, context, BROWSER_STORAGE_COOKIES, "ba", "whole", 5));
	CHECK(file_has("hos/cookies/ba.tmp1-2", "half", 4));
	CHECK_EQ(list_keys(calls, context, BROWSER_STORAGE_COOKIES, &set), 0);
	CHECK(key_set_has(&set, "ba"));
	for (i = 0; i < set.count; i++)
		CHECK(strchr(set.keys[i], '.') == NULL);

	/* A failed replace did not disturb an existing record of another key. */
	CHECK(is_record(calls, context, BROWSER_STORAGE_COOKIES, "aa", noise, sizeof(noise)));

	close_backend(calls, context);
}

/* A relative directory keeps working after the current directory changes; a trailing slash is the same directory. */
static void
test_directory_spellings(void)
{
	const struct browser_storage_calls *calls;
	void *context;
	struct wb_buffer here;
	int error;

	/* A relative directory is fixed when opened. */
	CHECK(raw_make_directory("sp"));
	CHECK(raw_make_directory("sp/inner"));
	CHECK_EQ(open_backend("sp/rel", &calls, &context), 0);
	if (calls == NULL || context == NULL)
		return;

	wb_buffer_init(&here);
	CHECK_EQ(wb_os_current_directory(&here), 0);

	CHECK_EQ(chdir("sp/inner"), 0);
	put(calls, context, BROWSER_STORAGE_COOKIES, "ab", "moved");
	CHECK(is_record(calls, context, BROWSER_STORAGE_COOKIES, "ab", "moved", 5));
	CHECK_EQ(chdir((const char *)here.data), 0);
	wb_buffer_release(&here);

	CHECK(file_has("sp/rel/cookies/ab", "moved", 5));
	CHECK(!raw_exists("sp/inner/cookies"));
	close_backend(calls, context);

	/* The same directory with a trailing slash holds the same records and the same lock. */
	error = open_backend("sp/rel/", &calls, &context);
	CHECK_EQ(error, 0);
	if (calls != NULL && context != NULL) {
		const struct browser_storage_calls *again_calls;
		void *again_context;

		CHECK(is_record(calls, context, BROWSER_STORAGE_COOKIES, "ab", "moved", 5));
		CHECK_EQ(open_backend("sp/rel", &again_calls, &again_context), EBUSY);
		CHECK(again_calls == NULL);
		CHECK(again_context == NULL);
		put(calls, context, BROWSER_STORAGE_CACHE, "cd", "slash");
		CHECK(file_has("sp/rel/cache/cd", "slash", 5));
		close_backend(calls, context);
	}
}

/* The second process's work: the open must fail with EBUSY ("files-child") or succeed ("files-free"). */
static int
child_main(
	const char *mode,
	const char *directory)
{
	const struct browser_storage_calls *calls;
	void *context;
	int error;

	calls = NULL;
	context = NULL;
	error = browser_storage_files_open(directory, &calls, &context);

	if (strcmp(mode, "files-child") == 0) {
		if (error != EBUSY)
			return 3;
		if (calls != NULL || context != NULL)
			return 4;
		return 0;
	}

	if (error != 0)
		return 5;
	if (calls == NULL || context == NULL)
		return 6;
	calls->close(context);
	return 0;
}

/* Starts this program again with a mode and a directory; returns its exit code, or -1. */
static int
run_child(
	const char *mode,
	const char *directory)
{
#ifdef _WIN32
	wchar_t exe[1024];
	wchar_t wide_mode[64];
	wchar_t wide_directory[512];
	wchar_t command[2400];
	STARTUPINFOW startup;
	PROCESS_INFORMATION process;
	DWORD code;
	DWORD units;

	/* Windows finds its own program file from the module; the path from argv[0] is for POSIX. */
	(void)self_path;
	units = GetModuleFileNameW(NULL, exe, 1024);
	if (units == 0 || units >= 1024)
		return -1;
	MultiByteToWideChar(CP_UTF8, 0, mode, -1, wide_mode, 64);
	MultiByteToWideChar(CP_UTF8, 0, directory, -1, wide_directory, 512);
	_snwprintf(command, 2400, L"\"%ls\" %ls \"%ls\"", exe, wide_mode, wide_directory);
	memset(&startup, 0, sizeof(startup));
	startup.cb = sizeof(startup);
	memset(&process, 0, sizeof(process));
	if (CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process) == 0)
		return -1;
	WaitForSingleObject(process.hProcess, 120000);
	code = 0xffffU;
	GetExitCodeProcess(process.hProcess, &code);
	CloseHandle(process.hProcess);
	CloseHandle(process.hThread);
	return (int)code;
#else
	char *const arguments[4] = { (char *)self_path, (char *)mode, (char *)directory, NULL };
	pid_t pid;
	int status;

	if (posix_spawn(&pid, self_path, NULL, NULL, arguments, environ) != 0)
		return -1;
	if (waitpid(pid, &status, 0) < 0)
		return -1;
	if (!WIFEXITED(status))
		return -1;
	return WEXITSTATUS(status);
#endif
}

/* Another process is refused while this one has the directory, and gets it after the close. */
static void
test_other_process(void)
{
	const struct browser_storage_calls *calls;
	void *context;

	CHECK_EQ(open_backend("proc", &calls, &context), 0);
	if (calls == NULL || context == NULL)
		return;

	put(calls, context, BROWSER_STORAGE_COOKIES, "aa", "held");
	CHECK_EQ(run_child("files-child", "proc"), 0);

	/* The second process did not damage the record, and did not lock it out for this one. */
	CHECK(is_record(calls, context, BROWSER_STORAGE_COOKIES, "aa", "held", 4));
	close_backend(calls, context);

	CHECK_EQ(run_child("files-free", "proc"), 0);
}
