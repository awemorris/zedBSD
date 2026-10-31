/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The unit test of the files of the operating-system layer
 * (base/os-file.h): reading and writing whole files, replacing a file in
 * one step, removing, directories, listing, the absolute and current paths,
 * the file: URL mapping and the directory lock.  It runs on POSIX and on
 * Windows; argv[1] says which.  Started with "lock-child <directory>" it is
 * the second process of the lock test.
 *
 * The expected values come from the header's contracts, from the C library
 * and the Windows API used separately from the module, and from hand
 * calculation.
 */

#include "check.h"

#include "base/os-file.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
#endif

/* The size of the biggest file the read and write tests use: 3 MiB. */
#define BIG_SIZE	(3U * 1024U * 1024U)

/* Whether this run is the Windows one (argv[1] is "win32"). */
static int is_win32;

/* The path this program was started with, for the lock test's second process. */
static const char *self_path;

/* Something for the failure checks to point an output at, so a function that skips the reset shows. */
static int sentinel_target;

/* A file's bytes as the test reads them without the module. */
struct raw_bytes {
	unsigned char *data;
	size_t length;
};

/* The names a listing found, copied out because a name is valid only during the call. */
struct name_set {
	char **items;
	size_t count;
	size_t capacity;
	size_t stop_at;
	int stop_value;
};

/* The state the threads of the replace test share. */
struct replace_race {
	const char *path;
	const unsigned char *first;
	size_t first_length;
	const unsigned char *second;
	size_t second_length;
	int writer_failures;
	int reader_failures;
	int torn_reads;
	int reads;
	int finished;
};

/* The arguments of the thread that opens a lock. */
struct lock_attempt {
	const char *directory;
	struct wb_os_lock *lock;
	int error;
};

#ifdef _WIN32
/*
 * Converts UTF-8 to a new UTF-16 string for the test's own calls of the
 * Windows API.
 */
static wchar_t *
to_wide(
	const char *text)
{
	wchar_t *wide;
	int units;

	units = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
	wide = (wchar_t *)malloc((size_t)units * sizeof(wchar_t));
	MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, units);
	return wide;
}
#endif

#ifdef _WIN32
/*
 * Tells whether the program runs under Wine.  Wine refuses to rename a file
 * over a destination another handle holds open even when that handle shares
 * deletes, which Windows allows; the checks that depend on it say so.
 */
static int
under_wine(void)
{
	HMODULE ntdll;

	ntdll = GetModuleHandleW(L"ntdll.dll");
	if (ntdll == NULL)
		return 0;
	return GetProcAddress(ntdll, "wine_get_version") != NULL;
}
#endif

/* Makes a directory without the module. */
static int
raw_mkdir(
	const char *path)
{
#ifdef _WIN32
	wchar_t *wide;
	int made;

	wide = to_wide(path);
	made = CreateDirectoryW(wide, NULL);
	free(wide);
	return made != 0;
#else
	return mkdir(path, 0755) == 0;
#endif
}

/* Tells whether something exists under a name, without the module. */
static int
raw_exists(
	const char *path)
{
#ifdef _WIN32
	wchar_t *wide;
	DWORD attributes;

	wide = to_wide(path);
	attributes = GetFileAttributesW(wide);
	free(wide);
	return attributes != INVALID_FILE_ATTRIBUTES;
#else
	struct stat status;

	return lstat(path, &status) == 0;
#endif
}

/* Tells whether a directory exists under a name, without the module. */
static int
raw_is_directory(
	const char *path)
{
#ifdef _WIN32
	wchar_t *wide;
	DWORD attributes;

	wide = to_wide(path);
	attributes = GetFileAttributesW(wide);
	free(wide);
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
	struct stat status;

	return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
#endif
}

/* Opens a file with the C library, for reading or writing in binary. */
static FILE *
raw_open(
	const char *path,
	int writing)
{
#ifdef _WIN32
	wchar_t *wide;
	FILE *file;

	wide = to_wide(path);
	file = _wfopen(wide, writing ? L"wb" : L"rb");
	free(wide);
	return file;
#else
	return fopen(path, writing ? "wb" : "rb");
#endif
}

/* Writes a file with the C library. */
static int
raw_write(
	const char *path,
	const void *bytes,
	size_t length)
{
	FILE *file;
	size_t written;

	file = raw_open(path, 1);
	if (file == NULL)
		return 0;
	written = 0;
	if (length > 0)
		written = fwrite(bytes, 1, length, file);
	if (fclose(file) != 0)
		return 0;
	return written == length;
}

/* Reads a whole file with the C library; the caller frees the bytes. */
static int
raw_read(
	const char *path,
	struct raw_bytes *out)
{
	FILE *file;
	size_t capacity;
	size_t got;

	out->data = NULL;
	out->length = 0;
	file = raw_open(path, 0);
	if (file == NULL)
		return 0;

	capacity = 1024;
	out->data = (unsigned char *)malloc(capacity);
	for (;;) {
		if (capacity - out->length < 512U) {
			capacity *= 2U;
			out->data = (unsigned char *)realloc(out->data, capacity);
		}
		got = fread(out->data + out->length, 1, capacity - out->length, file);
		if (got == 0)
			break;
		out->length += got;
	}
	fclose(file);
	return 1;
}

/* Tells whether a file holds exactly the given bytes, read without the module. */
static int
raw_holds(
	const char *path,
	const void *bytes,
	size_t length)
{
	struct raw_bytes read;
	int same;

	if (raw_read(path, &read) == 0)
		return 0;
	same = read.length == length && (length == 0 || memcmp(read.data, bytes, length) == 0);
	free(read.data);
	return same;
}

/* Gets the current directory the way the system's own call reports it, as UTF-8. */
static void
raw_current_directory(
	char *out,
	size_t size)
{
#ifdef _WIN32
	wchar_t wide[4096];
	DWORD units;

	units = GetCurrentDirectoryW(4096, wide);
	if (units == 0 || units >= 4096) {
		out[0] = '\0';
		return;
	}
	if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)size, NULL, NULL) == 0)
		out[0] = '\0';
#else
	if (getcwd(out, size) == NULL)
		out[0] = '\0';
#endif
}

/* Fills a run of bytes with a pattern that holds CR, LF, NUL and 0x1a (Ctrl-Z) in plenty. */
static void
fill_pattern(
	unsigned char *out,
	size_t length,
	unsigned int seed)
{
	static const unsigned char specials[4] = { 0x0dU, 0x0aU, 0x00U, 0x1aU };
	size_t i;

	for (i = 0; i < length; i++) {
		out[i] = (unsigned char)((i * 167U + seed * 29U + 13U) & 0xffU);
		if (i % 11U == 0)
			out[i] = specials[((i / 11U) + seed) % 4U];
	}
}

/* Tells whether a buffer holds exactly the text, with a NUL after it. */
static int
buffer_is(
	const struct wb_buffer *buffer,
	const void *bytes,
	size_t length)
{
	if (buffer->length != length)
		return 0;
	if (buffer->data == NULL)
		return 0;
	if (buffer->data[length] != '\0')
		return 0;
	if (length == 0)
		return 1;
	return memcmp(buffer->data, bytes, length) == 0;
}

/* Tells whether a buffer holds exactly the C string. */
static int
buffer_is_text(
	const struct wb_buffer *buffer,
	const char *text)
{
	return buffer_is(buffer, text, strlen(text));
}

/* Collects one name of a listing; stops with a value after a number of names when asked to. */
static int
collect_name(
	void *context,
	const char *name)
{
	struct name_set *set;

	set = (struct name_set *)context;
	if (set->count == set->capacity) {
		set->capacity = set->capacity == 0 ? 16U : set->capacity * 2U;
		set->items = (char **)realloc(set->items, set->capacity * sizeof(char *));
	}
	set->items[set->count] = strdup(name);
	set->count++;
	if (set->stop_at != 0 && set->count == set->stop_at)
		return set->stop_value;
	return 0;
}

/* Compares two names for qsort. */
static int
compare_names(
	const void *a,
	const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Frees a set of names. */
static void
names_release(
	struct name_set *set)
{
	size_t i;

	for (i = 0; i < set->count; i++)
		free(set->items[i]);
	free(set->items);
	memset(set, 0, sizeof(*set));
}

/* Lists a directory into a sorted set; returns the listing's own result. */
static int
names_of(
	const char *directory,
	struct name_set *set)
{
	int error;

	memset(set, 0, sizeof(*set));
	error = wb_os_list(directory, collect_name, set);
	if (set->count > 1)
		qsort(set->items, set->count, sizeof(char *), compare_names);
	return error;
}

/* Tells whether the sorted listing of a directory is exactly the sorted expected names. */
static int
listing_is(
	const char *directory,
	const char *const *expected,
	size_t count)
{
	struct name_set set;
	size_t i;
	int same;
	int error;

	error = names_of(directory, &set);
	same = error == 0 && set.count == count;
	for (i = 0; same && i < count; i++) {
		if (strcmp(set.items[i], expected[i]) != 0)
			same = 0;
	}
	names_release(&set);
	return same;
}

/* Tells whether a listing of a directory holds any name that starts with a prefix. */
static int
listing_has_prefix(
	const char *directory,
	const char *prefix)
{
	struct name_set set;
	size_t i;
	int found;

	names_of(directory, &set);
	found = 0;
	for (i = 0; i < set.count; i++) {
		if (strncmp(set.items[i], prefix, strlen(prefix)) == 0)
			found = 1;
	}
	names_release(&set);
	return found;
}

/* Makes a directory for a test group and checks that it was made. */
static void
make_group(
	const char *name)
{
	CHECK(raw_mkdir(name));
}

/* Checks that every function refuses NULL and the empty path, and leaves its outputs alone. */
static void
test_arguments(void)
{
	struct wb_buffer buffer;
	struct wb_buffer host;
	struct wb_os_lock *lock;
	const char *bad[2];
	size_t i;

	bad[0] = NULL;
	bad[1] = "";
	wb_buffer_init(&buffer);
	wb_buffer_init(&host);
	wb_buffer_append_string(&buffer, "keep");

	for (i = 0; i < 2; i++) {
		CHECK_EQ(wb_os_file_read(bad[i], &buffer), EINVAL);
		CHECK(buffer_is_text(&buffer, "keep"));
		CHECK_EQ(wb_os_file_write(bad[i], "x", 1), EINVAL);
		CHECK_EQ(wb_os_file_replace(bad[i], "x", 1), EINVAL);
		CHECK_EQ(wb_os_file_remove(bad[i]), EINVAL);
		CHECK_EQ(wb_os_make_directories(bad[i]), EINVAL);
		CHECK_EQ(wb_os_list(bad[i], collect_name, &sentinel_target), EINVAL);
		CHECK_EQ(wb_os_full_path(bad[i], &buffer), EINVAL);
		CHECK(buffer_is_text(&buffer, "keep"));
		lock = (struct wb_os_lock *)&sentinel_target;
		CHECK_EQ(wb_os_lock_open(bad[i], &lock), EINVAL);
		CHECK(lock == NULL);
	}

	/* A missing buffer, callback or output place is EINVAL too. */
	CHECK_EQ(wb_os_file_read("x", NULL), EINVAL);
	CHECK_EQ(wb_os_file_write("x", NULL, 1), EINVAL);
	CHECK_EQ(wb_os_file_replace("x", NULL, 1), EINVAL);
	CHECK_EQ(wb_os_list("x", NULL, NULL), EINVAL);
	CHECK_EQ(wb_os_full_path("x", NULL), EINVAL);
	CHECK_EQ(wb_os_current_directory(NULL), EINVAL);
	CHECK_EQ(wb_os_lock_open("x", NULL), EINVAL);
	CHECK_EQ(wb_os_path_from_url("", "/x", NULL), EINVAL);
	CHECK_EQ(wb_os_path_from_url("", NULL, &buffer), EINVAL);
	CHECK_EQ(wb_os_url_from_path(NULL, &host, &buffer), EINVAL);
	CHECK_EQ(wb_os_url_from_path("/x", NULL, &buffer), EINVAL);
	CHECK_EQ(wb_os_url_from_path("/x", &host, NULL), EINVAL);
	CHECK(buffer_is_text(&buffer, "keep"));
	CHECK_EQ(host.length, 0);

	/* Releasing NULL does nothing. */
	wb_os_lock_close(NULL);

	wb_buffer_release(&buffer);
	wb_buffer_release(&host);
}

/* Checks reading and writing whole files. */
static void
test_read_write(void)
{
	static const size_t sizes[4] = { 0U, 1U, 65536U, BIG_SIZE };
	struct wb_buffer buffer;
	unsigned char *bytes;
	size_t i;
	char name[32];

	make_group("rw");
	bytes = (unsigned char *)malloc(BIG_SIZE);
	fill_pattern(bytes, BIG_SIZE, 1U);

	/* Each size goes out through the module, is seen intact by the C library, and comes back. */
	for (i = 0; i < 4; i++) {
		snprintf(name, sizeof(name), "rw/size-%u", (unsigned)i);
		CHECK_EQ(wb_os_file_write(name, bytes, sizes[i]), 0);
		CHECK(raw_holds(name, bytes, sizes[i]));

		wb_buffer_init(&buffer);
		CHECK_EQ(wb_os_file_read(name, &buffer), 0);
		CHECK(buffer_is(&buffer, bytes, sizes[i]));
		wb_buffer_release(&buffer);
	}

	/* A file the C library wrote is read the same, byte for byte, and the NUL is not counted. */
	CHECK(raw_write("rw/raw", bytes, 65536U));
	wb_buffer_init(&buffer);
	CHECK_EQ(wb_os_file_read("rw/raw", &buffer), 0);
	CHECK(buffer_is(&buffer, bytes, 65536U));
	wb_buffer_release(&buffer);

	/* Reading appends after the bytes the buffer already holds. */
	wb_buffer_init(&buffer);
	wb_buffer_append_string(&buffer, "head:");
	CHECK_EQ(wb_os_file_write("rw/abc", "abc", 3), 0);
	CHECK_EQ(wb_os_file_read("rw/abc", &buffer), 0);
	CHECK(buffer_is_text(&buffer, "head:abc"));
	CHECK_EQ(wb_os_file_read("rw/abc", &buffer), 0);
	CHECK(buffer_is_text(&buffer, "head:abcabc"));
	wb_buffer_release(&buffer);

	/* An empty file leaves the buffer's bytes as they are and still gives it a NUL. */
	wb_buffer_init(&buffer);
	CHECK_EQ(wb_os_file_read("rw/size-0", &buffer), 0);
	CHECK(buffer_is(&buffer, "", 0));
	wb_buffer_release(&buffer);

	/* A file that is not there: ENOENT, and the buffer is as it was. */
	wb_buffer_init(&buffer);
	wb_buffer_append_string(&buffer, "keep");
	CHECK_EQ(wb_os_file_read("rw/missing", &buffer), ENOENT);
	CHECK(buffer_is_text(&buffer, "keep"));
	CHECK_EQ(wb_os_file_read("rw/abc/inside", &buffer), ENOENT);
	CHECK(buffer_is_text(&buffer, "keep"));
	CHECK_EQ(wb_os_file_read("missing-dir/file", &buffer), ENOENT);
	CHECK(buffer_is_text(&buffer, "keep"));

	/* A directory is EISDIR to read and to write over, and stays a directory. */
	CHECK_EQ(wb_os_file_read("rw", &buffer), EISDIR);
	CHECK(buffer_is_text(&buffer, "keep"));
	CHECK_EQ(wb_os_file_write("rw", "x", 1), EISDIR);
	CHECK(raw_is_directory("rw"));
	wb_buffer_release(&buffer);

	/* A missing directory is ENOENT to write into, and nothing is made. */
	CHECK_EQ(wb_os_file_write("no-such-dir/file", "x", 1), ENOENT);
	CHECK(!raw_exists("no-such-dir"));

	/* Writing again empties and rewrites; no bytes with length 0 makes an empty file; bytes are needed for a length. */
	CHECK_EQ(wb_os_file_write("rw/abc", "z", 1), 0);
	CHECK(raw_holds("rw/abc", "z", 1));
	CHECK_EQ(wb_os_file_write("rw/abc", NULL, 0), 0);
	CHECK(raw_holds("rw/abc", "", 0));
	CHECK_EQ(wb_os_file_write("rw/null", NULL, 5), EINVAL);
	CHECK(!raw_exists("rw/null"));

	/* A write with the exact bytes "\r\n\0\x1a" is kept as they are. */
	CHECK_EQ(wb_os_file_write("rw/ctrl", "\r\n\0\x1a\r\n", 6), 0);
	CHECK(raw_holds("rw/ctrl", "\r\n\0\x1a\r\n", 6));

	free(bytes);
}

/* Checks the replacement of a file in one step. */
static void
test_replace(void)
{
	unsigned char *big;
	const char *expected[2];

	make_group("rp");
	big = (unsigned char *)malloc(BIG_SIZE);
	fill_pattern(big, BIG_SIZE, 2U);

	/* A new file. */
	CHECK_EQ(wb_os_file_replace("rp/file", "one", 3), 0);
	CHECK(raw_holds("rp/file", "one", 3));

	/* An existing file overwritten by a big one, and then by a small one. */
	CHECK_EQ(wb_os_file_replace("rp/file", big, BIG_SIZE), 0);
	CHECK(raw_holds("rp/file", big, BIG_SIZE));
	CHECK_EQ(wb_os_file_replace("rp/file", "s", 1), 0);
	CHECK(raw_holds("rp/file", "s", 1));

	/* An empty replacement, and a missing byte pointer for a length. */
	CHECK_EQ(wb_os_file_replace("rp/file", NULL, 0), 0);
	CHECK(raw_holds("rp/file", "", 0));
	CHECK_EQ(wb_os_file_replace("rp/file", NULL, 3), EINVAL);
	CHECK(raw_holds("rp/file", "", 0));

	/* No temporary file stays, and the directory holds just the file. */
	expected[0] = "file";
	CHECK(listing_is("rp", expected, 1));
	CHECK(!listing_has_prefix("rp", "file.tmp"));

	/* A missing directory: ENOENT, and no directory or temporary file appears. */
	CHECK_EQ(wb_os_file_replace("rp-missing/file", "x", 1), ENOENT);
	CHECK(!raw_exists("rp-missing"));

	/* A directory in the way is EISDIR, it is left alone, and no temporary file stays behind. */
	CHECK(raw_mkdir("rp/dir"));
	CHECK_EQ(wb_os_file_replace("rp/dir", "x", 1), EISDIR);
	CHECK(raw_is_directory("rp/dir"));
	CHECK(!listing_has_prefix("rp", "dir.tmp"));

	free(big);
}

/* A writer thread of the replace test: it replaces the file by one content and the other, over and over. */
static void *
replace_writer(
	void *argument)
{
	struct replace_race *race;
	int i;
	int error;

	race = (struct replace_race *)argument;
	for (i = 0; i < 3; i++) {
		if (i % 2 == 0)
			error = wb_os_file_replace(race->path, race->first, race->first_length);
		else
			error = wb_os_file_replace(race->path, race->second, race->second_length);
		if (error != 0)
			__atomic_add_fetch(&race->writer_failures, 1, __ATOMIC_SEQ_CST);
	}
	return NULL;
}

/* The reader thread of the replace test: every read must see one whole content. */
static void *
replace_reader(
	void *argument)
{
	struct replace_race *race;
	struct wb_buffer buffer;
	int error;
	int whole;

	race = (struct replace_race *)argument;
	do {
		wb_buffer_init(&buffer);
		error = wb_os_file_read(race->path, &buffer);
		if (error != 0) {
			__atomic_add_fetch(&race->reader_failures, 1, __ATOMIC_SEQ_CST);
		} else {
			whole = 0;
			if (buffer.length == race->first_length && memcmp(buffer.data, race->first, race->first_length) == 0)
				whole = 1;
			if (buffer.length == race->second_length && memcmp(buffer.data, race->second, race->second_length) == 0)
				whole = 1;
			if (whole == 0)
				__atomic_add_fetch(&race->torn_reads, 1, __ATOMIC_SEQ_CST);
			__atomic_add_fetch(&race->reads, 1, __ATOMIC_SEQ_CST);
		}
		wb_buffer_release(&buffer);
	} while (__atomic_load_n(&race->finished, __ATOMIC_SEQ_CST) == 0);
	return NULL;
}

/* Checks that readers meeting replacing threads see the old or the new contents and never a mixture. */
static void
test_replace_threads(void)
{
	struct replace_race race;
	pthread_t writers[2];
	pthread_t reader;
	unsigned char *first;
	unsigned char *second;
	int i;

	make_group("race");
	first = (unsigned char *)malloc(150000U);
	second = (unsigned char *)malloc(90000U);
	memset(first, 'a', 150000U);
	memset(second, 'b', 90000U);
	memset(&race, 0, sizeof(race));
	race.path = "race/file";
	race.first = first;
	race.first_length = 150000U;
	race.second = second;
	race.second_length = 90000U;
	CHECK_EQ(wb_os_file_replace(race.path, first, 150000U), 0);

	for (i = 0; i < 2; i++)
		pthread_create(&writers[i], NULL, replace_writer, &race);
	pthread_create(&reader, NULL, replace_reader, &race);
	for (i = 0; i < 2; i++)
		pthread_join(writers[i], NULL);
	__atomic_store_n(&race.finished, 1, __ATOMIC_SEQ_CST);
	pthread_join(reader, NULL);

#ifdef _WIN32
	/* Wine cannot rename over a file that a reader has open; Windows can, so there no replacement may fail. */
	if (under_wine() == 0)
		CHECK_EQ(race.writer_failures, 0);
#else
	CHECK_EQ(race.writer_failures, 0);
#endif
	CHECK_EQ(race.reader_failures, 0);
	CHECK_EQ(race.torn_reads, 0);
	CHECK(race.reads >= 1);
	CHECK(!listing_has_prefix("race", "file.tmp"));
	free(first);
	free(second);
}

/* Checks removing a file. */
static void
test_remove(void)
{
	make_group("rm");
	CHECK_EQ(wb_os_file_write("rm/file", "x", 1), 0);

	/* A file goes; removing it again finds nothing. */
	CHECK_EQ(wb_os_file_remove("rm/file"), 0);
	CHECK(!raw_exists("rm/file"));
	CHECK_EQ(wb_os_file_remove("rm/file"), ENOENT);
	CHECK_EQ(wb_os_file_remove("rm-missing/file"), ENOENT);

	/* A path through a file names nothing. */
	CHECK_EQ(wb_os_file_write("rm/plain", "x", 1), 0);
	CHECK_EQ(wb_os_file_remove("rm/plain/inside"), ENOENT);
	CHECK(raw_exists("rm/plain"));

	/* A directory is EISDIR and stays, with what it holds. */
	CHECK(raw_mkdir("rm/dir"));
	CHECK_EQ(wb_os_file_write("rm/dir/inner", "x", 1), 0);
	CHECK_EQ(wb_os_file_remove("rm/dir"), EISDIR);
	CHECK(raw_is_directory("rm/dir"));
	CHECK(raw_exists("rm/dir/inner"));
}

/* Checks making directories and the errors of a file in the way. */
static void
test_make_directories(void)
{
	char deep[512];
	char cwd[4096];
	char root[8];
	size_t used;
	size_t i;
	int j;

	make_group("md");

	/* Several levels at once, then the same again. */
	CHECK_EQ(wb_os_make_directories("md/a/b/c"), 0);
	CHECK(raw_is_directory("md/a"));
	CHECK(raw_is_directory("md/a/b"));
	CHECK(raw_is_directory("md/a/b/c"));
	CHECK_EQ(wb_os_make_directories("md/a/b/c"), 0);
	CHECK_EQ(wb_os_make_directories("md/a"), 0);

	/* A trailing separator, a doubled one, and a leading "./". */
	CHECK_EQ(wb_os_make_directories("md/d/e/"), 0);
	CHECK(raw_is_directory("md/d/e"));
	CHECK_EQ(wb_os_make_directories("md/f//g"), 0);
	CHECK(raw_is_directory("md/f/g"));
	CHECK_EQ(wb_os_make_directories("./md/h"), 0);
	CHECK(raw_is_directory("md/h"));
	CHECK_EQ(wb_os_make_directories("md/i/../j"), 0);
	CHECK(raw_is_directory("md/i"));
	CHECK(raw_is_directory("md/j"));

	/* A file in the way, as the path itself and as a part of it. */
	CHECK_EQ(wb_os_file_write("md/file", "x", 1), 0);
	CHECK_EQ(wb_os_make_directories("md/file"), ENOTDIR);
	CHECK_EQ(wb_os_make_directories("md/file/"), ENOTDIR);
	CHECK_EQ(wb_os_make_directories("md/file/x"), ENOTDIR);
	CHECK_EQ(wb_os_make_directories("md/file/x/y"), ENOTDIR);
	CHECK(!raw_exists("md/file/x"));

	/* A path far longer than the old fixed limits is made, part by part. */
	used = 0;
	used += (size_t)snprintf(deep + used, sizeof(deep) - used, "md/deep");
	for (j = 0; j < 25; j++)
		used += (size_t)snprintf(deep + used, sizeof(deep) - used, "/part-%02d-123456789", j);
	CHECK(used > 400U);
	CHECK_EQ(wb_os_make_directories(deep), 0);
	CHECK(raw_is_directory(deep));

	/* Text that is not a path: too long, or not UTF-8. */
	CHECK_EQ(wb_os_make_directories("md/\xff"), EILSEQ);
	CHECK_EQ(wb_os_make_directories("md/\xed\xa0\x80"), EILSEQ);
	CHECK_EQ(wb_os_make_directories("md/\xe3\x81"), EILSEQ);
	{
		char *huge;

		huge = (char *)malloc((size_t)WB_OS_PATH_MAX + 2U);
		memset(huge, 'a', (size_t)WB_OS_PATH_MAX + 1U);
		huge[(size_t)WB_OS_PATH_MAX + 1U] = '\0';
		CHECK_EQ(wb_os_make_directories(huge), ENAMETOOLONG);
		free(huge);
	}

	/* The root of the current drive or of the file system exists already. */
	if (is_win32) {
		raw_current_directory(cwd, sizeof(cwd));
		for (i = 0; i < 3; i++)
			root[i] = cwd[i];
		root[3] = '\0';
		CHECK_EQ(wb_os_make_directories(root), 0);
		root[2] = '/';
		CHECK_EQ(wb_os_make_directories(root), 0);

		/* Both separators work in one path. */
		CHECK_EQ(wb_os_make_directories("md\\w1/w2\\w3"), 0);
		CHECK(raw_is_directory("md/w1/w2/w3"));

		/* A path through a file, written with the other separator. */
		CHECK_EQ(wb_os_make_directories("md\\file\\x"), ENOTDIR);
	} else {
		CHECK_EQ(wb_os_make_directories("/"), 0);
		CHECK_EQ(wb_os_make_directories("//"), 0);
	}
}

/* Checks listing a directory. */
static void
test_list(void)
{
	static const char *const names[] = {
		".hidden", "Zeta", "alpha", "name with space", "sub"
	};
	struct name_set set;
	char path[64];
	int i;
	int error;
	long total;

	make_group("ls");
	make_group("ls/sub");
	CHECK(raw_write("ls/alpha", "1", 1));
	CHECK(raw_write("ls/Zeta", "2", 1));
	CHECK(raw_write("ls/.hidden", "3", 1));
	CHECK(raw_write("ls/name with space", "4", 1));

	/* The names and only the names: no "." and no "..". */
	CHECK(listing_is("ls", names, 5));

	/* A trailing separator does not matter. */
	CHECK(listing_is("ls/", names, 5));

	/* An empty directory calls nothing and succeeds. */
	make_group("empty");
	memset(&set, 0, sizeof(set));
	CHECK_EQ(wb_os_list("empty", collect_name, &set), 0);
	CHECK_EQ(set.count, 0);
	names_release(&set);

	/* The callback's non-zero value stops the walk and comes back as the result. */
	memset(&set, 0, sizeof(set));
	set.stop_at = 1;
	set.stop_value = 5;
	CHECK_EQ(wb_os_list("ls", collect_name, &set), 5);
	CHECK_EQ(set.count, 1);
	names_release(&set);
	memset(&set, 0, sizeof(set));
	set.stop_at = 3;
	set.stop_value = 77;
	CHECK_EQ(wb_os_list("ls", collect_name, &set), 77);
	CHECK_EQ(set.count, 3);
	names_release(&set);

	/* A missing directory is ENOENT; a file is ENOTDIR. */
	memset(&set, 0, sizeof(set));
	CHECK_EQ(wb_os_list("ls-missing", collect_name, &set), ENOENT);
	CHECK_EQ(wb_os_list("ls/alpha", collect_name, &set), ENOTDIR);
	CHECK_EQ(set.count, 0);
	names_release(&set);

	/* A directory with many entries is listed whole. */
	make_group("many");
	total = 0;
	for (i = 0; i < 600; i++) {
		snprintf(path, sizeof(path), "many/entry-%04d", i);
		CHECK(raw_write(path, "m", 1));
		total += i;
	}
	error = names_of("many", &set);
	CHECK_EQ(error, 0);
	CHECK_EQ(set.count, 600);
	for (i = 0; i < 600 && (size_t)i < set.count; i++) {
		snprintf(path, sizeof(path), "entry-%04d", i);
		CHECK(strcmp(set.items[i], path) == 0);
	}
	CHECK_EQ(total, 179700);
	names_release(&set);

	/* POSIX: a name that is not UTF-8 is skipped, not an error. */
	if (!is_win32) {
		make_group("bad");
		CHECK(raw_write("bad/ok", "1", 1));
		CHECK(raw_write("bad/\xff.txt", "2", 1));
		CHECK(raw_write("bad/\xc0\x80", "3", 1));
		error = names_of("bad", &set);
		CHECK_EQ(error, 0);
		CHECK_EQ(set.count, 1);
		if (set.count == 1)
			CHECK(strcmp(set.items[0], "ok") == 0);
		names_release(&set);
	}
}

/* Checks names that are not ASCII and text that is not UTF-8. */
static void
test_utf8_names(void)
{
	static const char *const names[] = { "emoji-\xf0\x9f\x98\x80.bin", "\xe3\x83\x87\xe3\x83\xbc\xe3\x82\xbf.txt" };
	struct wb_buffer buffer;
	char path[128];
	char *huge;
	struct wb_os_lock *lock;
	size_t i;
	unsigned char bytes[300];

	make_group("u8");
	fill_pattern(bytes, sizeof(bytes), 3U);

	/* Each name is written, seen by the C library, listed, read, replaced and removed. */
	for (i = 0; i < 2; i++) {
		snprintf(path, sizeof(path), "u8/%s", names[i]);
		CHECK_EQ(wb_os_file_write(path, bytes, sizeof(bytes)), 0);
		CHECK(raw_holds(path, bytes, sizeof(bytes)));
		wb_buffer_init(&buffer);
		CHECK_EQ(wb_os_file_read(path, &buffer), 0);
		CHECK(buffer_is(&buffer, bytes, sizeof(bytes)));
		wb_buffer_release(&buffer);
		if (i == 0) {
			CHECK_EQ(wb_os_file_replace(path, "new", 3), 0);
			CHECK(raw_holds(path, "new", 3));
		}
	}
	CHECK(listing_is("u8", names, 2));
	for (i = 0; i < 2; i++) {
		snprintf(path, sizeof(path), "u8/%s", names[i]);
		CHECK_EQ(wb_os_file_remove(path), 0);
		CHECK(!raw_exists(path));
	}

	/* A directory with a non-ASCII name is made, listed inside, and can be locked. */
	CHECK_EQ(wb_os_make_directories("u8/\xe3\x83\x87\xe3\x83\xbc\xe3\x82\xbf/\xf0\x9f\x98\x80"), 0);
	CHECK(raw_is_directory("u8/\xe3\x83\x87\xe3\x83\xbc\xe3\x82\xbf/\xf0\x9f\x98\x80"));
	lock = NULL;
	CHECK_EQ(wb_os_lock_open("u8/\xe3\x83\x87\xe3\x83\xbc\xe3\x82\xbf", &lock), 0);
	CHECK(lock != NULL);
	wb_os_lock_close(lock);

	/* Text that is not UTF-8 is EILSEQ in every function that takes a path. */
	{
		static const char *const bad[] = {
			"u8/\xff.txt", "u8/\xc0\x80", "u8/\xe0\x80\x80", "u8/\xed\xa0\x80", "u8/\xf4\x90\x80\x80",
			"u8/\xf5\x80\x80\x80", "u8/ab\xe3\x81", "u8/\x80"
		};

		for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
			wb_buffer_init(&buffer);
			wb_buffer_append_string(&buffer, "keep");
			CHECK_EQ(wb_os_file_read(bad[i], &buffer), EILSEQ);
			CHECK(buffer_is_text(&buffer, "keep"));
			CHECK_EQ(wb_os_file_write(bad[i], "x", 1), EILSEQ);
			CHECK_EQ(wb_os_file_replace(bad[i], "x", 1), EILSEQ);
			CHECK_EQ(wb_os_file_remove(bad[i]), EILSEQ);
			CHECK_EQ(wb_os_make_directories(bad[i]), EILSEQ);
			CHECK_EQ(wb_os_list(bad[i], collect_name, &sentinel_target), EILSEQ);
			CHECK_EQ(wb_os_full_path(bad[i], &buffer), EILSEQ);
			CHECK(buffer_is_text(&buffer, "keep"));
			lock = (struct wb_os_lock *)&sentinel_target;
			CHECK_EQ(wb_os_lock_open(bad[i], &lock), EILSEQ);
			CHECK(lock == NULL);
			wb_buffer_release(&buffer);
		}
	}

	/* A path longer than the layer accepts is ENAMETOOLONG in every function. */
	huge = (char *)malloc((size_t)WB_OS_PATH_MAX + 2U);
	memset(huge, 'a', (size_t)WB_OS_PATH_MAX + 1U);
	huge[(size_t)WB_OS_PATH_MAX + 1U] = '\0';
	wb_buffer_init(&buffer);
	CHECK_EQ(wb_os_file_read(huge, &buffer), ENAMETOOLONG);
	CHECK_EQ(buffer.length, 0);
	CHECK_EQ(wb_os_file_write(huge, "x", 1), ENAMETOOLONG);
	CHECK_EQ(wb_os_file_replace(huge, "x", 1), ENAMETOOLONG);
	CHECK_EQ(wb_os_file_remove(huge), ENAMETOOLONG);
	CHECK_EQ(wb_os_list(huge, collect_name, &sentinel_target), ENAMETOOLONG);
	CHECK_EQ(wb_os_full_path(huge, &buffer), ENAMETOOLONG);
	CHECK_EQ(buffer.length, 0);
	lock = (struct wb_os_lock *)&sentinel_target;
	CHECK_EQ(wb_os_lock_open(huge, &lock), ENAMETOOLONG);
	CHECK(lock == NULL);
	wb_buffer_release(&buffer);
	free(huge);

	/* A single name longer than the system allows is ENAMETOOLONG (the file system decides, so POSIX only). */
	if (!is_win32) {
		char name[400];

		memcpy(name, "u8/", 3);
		memset(name + 3, 'n', 300);
		name[303] = '\0';
		CHECK_EQ(wb_os_file_write(name, "x", 1), ENAMETOOLONG);
	}
}

/* Checks the absolute form of a path. */
static void
test_full_path(void)
{
	struct wb_buffer buffer;
	char cwd[4096];
	char expected[4200];

	make_group("fp");
	CHECK_EQ(wb_os_file_write("fp/target.txt", "t", 1), 0);
	raw_current_directory(cwd, sizeof(cwd));
	CHECK(cwd[0] != '\0');

	if (!is_win32) {
		/* An existing file, with the dots resolved, and a symbolic link followed. */
		snprintf(expected, sizeof(expected), "%s/fp/target.txt", cwd);
		wb_buffer_init(&buffer);
		wb_buffer_append_string(&buffer, "pre:");
		CHECK_EQ(wb_os_full_path("fp/target.txt", &buffer), 0);
		snprintf(expected, sizeof(expected), "pre:%s/fp/target.txt", cwd);
		CHECK(buffer_is_text(&buffer, expected));
		wb_buffer_clear(&buffer);
		CHECK_EQ(wb_os_full_path("./fp/../fp/./target.txt", &buffer), 0);
		snprintf(expected, sizeof(expected), "%s/fp/target.txt", cwd);
		CHECK(buffer_is_text(&buffer, expected));
#ifndef _WIN32
		CHECK_EQ(symlink("target.txt", "fp/link"), 0);
		CHECK_EQ(symlink("loop2", "fp/loop1"), 0);
		CHECK_EQ(symlink("loop1", "fp/loop2"), 0);
		CHECK_EQ(symlink("missing-target", "fp/dangling"), 0);
#endif
		wb_buffer_clear(&buffer);
		CHECK_EQ(wb_os_full_path("fp/link", &buffer), 0);
		CHECK(buffer_is_text(&buffer, expected));

		/* A directory, and the root. */
		wb_buffer_clear(&buffer);
		CHECK_EQ(wb_os_full_path("fp", &buffer), 0);
		snprintf(expected, sizeof(expected), "%s/fp", cwd);
		CHECK(buffer_is_text(&buffer, expected));
		wb_buffer_clear(&buffer);
		CHECK_EQ(wb_os_full_path("/", &buffer), 0);
		CHECK(buffer_is_text(&buffer, "/"));

		/* A file that is not there, one behind a file, a dangling link and a loop: ENOENT, buffer as it was. */
		wb_buffer_clear(&buffer);
		wb_buffer_append_string(&buffer, "keep");
		CHECK_EQ(wb_os_full_path("fp/missing", &buffer), ENOENT);
		CHECK_EQ(wb_os_full_path("fp/target.txt/x", &buffer), ENOENT);
		CHECK_EQ(wb_os_full_path("fp/dangling", &buffer), ENOENT);
		CHECK_EQ(wb_os_full_path("fp/loop1", &buffer), ENOENT);
		CHECK_EQ(wb_os_full_path("fp-missing/x", &buffer), ENOENT);
		CHECK(buffer_is_text(&buffer, "keep"));
		wb_buffer_release(&buffer);
	} else {
		/* The current directory with parts resolved; the file need not exist. */
		wb_buffer_init(&buffer);
		wb_buffer_append_string(&buffer, "pre:");
		CHECK_EQ(wb_os_full_path("a\\..\\b", &buffer), 0);
		snprintf(expected, sizeof(expected), "pre:%s\\b", cwd);
		CHECK(buffer_is_text(&buffer, expected));
		wb_buffer_clear(&buffer);
		CHECK_EQ(wb_os_full_path("a/../b", &buffer), 0);
		snprintf(expected, sizeof(expected), "%s\\b", cwd);
		CHECK(buffer_is_text(&buffer, expected));
		wb_buffer_clear(&buffer);
		CHECK_EQ(wb_os_full_path(".", &buffer), 0);
		CHECK(buffer_is_text(&buffer, cwd));

		/* Absolute paths are cleaned: dots, forward slashes, doubled separators. */
		wb_buffer_clear(&buffer);
		CHECK_EQ(wb_os_full_path("C:\\x\\..\\y", &buffer), 0);
		CHECK(buffer_is_text(&buffer, "C:\\y"));
		wb_buffer_clear(&buffer);
		CHECK_EQ(wb_os_full_path("C:/x//y/./z", &buffer), 0);
		CHECK(buffer_is_text(&buffer, "C:\\x\\y\\z"));
		wb_buffer_clear(&buffer);
		CHECK_EQ(wb_os_full_path("\\\\srv\\share\\x\\..\\y", &buffer), 0);
		CHECK(buffer_is_text(&buffer, "\\\\srv\\share\\y"));

		/* A non-ASCII name survives. */
		wb_buffer_clear(&buffer);
		CHECK_EQ(wb_os_full_path("\xe3\x83\x87\xe3\x83\xbc\xe3\x82\xbf", &buffer), 0);
		snprintf(expected, sizeof(expected), "%s\\\xe3\x83\x87\xe3\x83\xbc\xe3\x82\xbf", cwd);
		CHECK(buffer_is_text(&buffer, expected));

		/* A path longer than MAX_PATH is returned in full. */
		{
			char longer[400];

			memcpy(longer, "C:\\", 3);
			memset(longer + 3, 'q', 330);
			memcpy(longer + 333, "\\z", 3);
			wb_buffer_clear(&buffer);
			CHECK_EQ(wb_os_full_path(longer, &buffer), 0);
			CHECK(buffer_is_text(&buffer, longer));
		}
		wb_buffer_release(&buffer);
	}
}

/* Checks the current working directory. */
static void
test_current_directory(void)
{
	struct wb_buffer buffer;
	char expected[4096];

	/* It is the system's own answer, appended after what the buffer holds. */
	raw_current_directory(expected, sizeof(expected));
	wb_buffer_init(&buffer);
	CHECK_EQ(wb_os_current_directory(&buffer), 0);
	CHECK(buffer_is_text(&buffer, expected));
	wb_buffer_clear(&buffer);
	wb_buffer_append_string(&buffer, "x");
	CHECK_EQ(wb_os_current_directory(&buffer), 0);
	CHECK_EQ(buffer.length, strlen(expected) + 1U);
	CHECK(memcmp(buffer.data + 1, expected, strlen(expected)) == 0);

	/* No trailing separator unless it is a root. */
	CHECK(buffer.data[buffer.length - 1U] != '/' || strlen(expected) == 1U);
	wb_buffer_release(&buffer);

#ifndef _WIN32
	{
		char saved[4096];
		char long_directory[1024];
		size_t used;
		int j;

		/* A directory deeper than the first buffer the module tries, and the root. */
		getcwd(saved, sizeof(saved));
		used = 0;
		used += (size_t)snprintf(long_directory + used, sizeof(long_directory) - used, "cd");
		for (j = 0; j < 30; j++)
			used += (size_t)snprintf(long_directory + used, sizeof(long_directory) - used, "/seg-%02d-abcdefghijk", j);
		CHECK_EQ(wb_os_make_directories(long_directory), 0);
		CHECK_EQ(chdir(long_directory), 0);
		raw_current_directory(expected, sizeof(expected));
		CHECK(strlen(expected) > 512U);
		wb_buffer_init(&buffer);
		CHECK_EQ(wb_os_current_directory(&buffer), 0);
		CHECK(buffer_is_text(&buffer, expected));
		wb_buffer_release(&buffer);

		CHECK_EQ(chdir("/"), 0);
		wb_buffer_init(&buffer);
		CHECK_EQ(wb_os_current_directory(&buffer), 0);
		CHECK(buffer_is_text(&buffer, "/"));
		wb_buffer_release(&buffer);
		CHECK_EQ(chdir(saved), 0);
	}
#else
	{
		char root[8];
		wchar_t saved[4096];
		wchar_t *wide_root;

		/* The root of the current drive keeps its separator. */
		raw_current_directory(expected, sizeof(expected));
		memcpy(root, expected, 3);
		root[3] = '\0';
		GetCurrentDirectoryW(4096, saved);
		wide_root = to_wide(root);
		CHECK(SetCurrentDirectoryW(wide_root) != 0);
		free(wide_root);
		wb_buffer_init(&buffer);
		CHECK_EQ(wb_os_current_directory(&buffer), 0);
		CHECK(buffer_is_text(&buffer, root));
		wb_buffer_release(&buffer);
		SetCurrentDirectoryW(saved);
	}
#endif
}

/* Checks the test for an absolute path. */
static void
test_absolute(void)
{
	size_t i;

	CHECK_EQ(wb_os_path_is_absolute(NULL), 0);
	CHECK_EQ(wb_os_path_is_absolute(""), 0);

	if (!is_win32) {
		static const char *const yes[] = { "/", "/a", "//", "/a/b/../c" };
		static const char *const no[] = { "a", "./a", "../a", " /a", "C:\\x", "C:/x" };

		for (i = 0; i < sizeof(yes) / sizeof(yes[0]); i++)
			CHECK_EQ(wb_os_path_is_absolute(yes[i]), 1);
		for (i = 0; i < sizeof(no) / sizeof(no[0]); i++)
			CHECK_EQ(wb_os_path_is_absolute(no[i]), 0);
	} else {
		static const char *const yes[] = {
			"C:\\x", "c:/x", "C:\\", "z:/", "\\\\srv\\x", "//srv/x", "\\\\srv", "\\/srv\\x",
			"\\\\?\\C:\\x", "\\\\.\\COM1"
		};
		static const char *const no[] = {
			"C:", "C:x", "\\x", "/x", "x", "\\", "/", "\\\\", "//", "\\\\\\x", "///x", "1:\\x", "\xe3\x81\x82:\\x",
			"CC:\\x", ".\\x", "C"
		};

		for (i = 0; i < sizeof(yes) / sizeof(yes[0]); i++)
			CHECK_EQ(wb_os_path_is_absolute(yes[i]), 1);
		for (i = 0; i < sizeof(no) / sizeof(no[0]); i++)
			CHECK_EQ(wb_os_path_is_absolute(no[i]), 0);
	}
}

/* Checks one file: URL to path conversion and leaves the buffer's length as it was on failure. */
static void
check_from_url(
	const char *host,
	const char *path,
	int expected_error,
	const char *expected_text)
{
	struct wb_buffer buffer;
	int error;

	wb_buffer_init(&buffer);
	wb_buffer_append_string(&buffer, "pre:");
	error = wb_os_path_from_url(host, path, &buffer);
	CHECK_EQ(error, expected_error);
	if (expected_error == 0) {
		char text[512];

		snprintf(text, sizeof(text), "pre:%s", expected_text);
		CHECK(buffer_is_text(&buffer, text));
	} else {
		CHECK(buffer_is_text(&buffer, "pre:"));
	}
	wb_buffer_release(&buffer);
}

/* Checks one path to file: URL conversion; both buffers keep their length on failure. */
static void
check_to_url(
	const char *local,
	int expected_error,
	const char *expected_host,
	const char *expected_path)
{
	struct wb_buffer host;
	struct wb_buffer path;
	char text[512];
	int error;

	wb_buffer_init(&host);
	wb_buffer_init(&path);
	wb_buffer_append_string(&host, "H:");
	wb_buffer_append_string(&path, "P:");
	error = wb_os_url_from_path(local, &host, &path);
	CHECK_EQ(error, expected_error);
	if (expected_error == 0) {
		snprintf(text, sizeof(text), "H:%s", expected_host);
		CHECK(buffer_is_text(&host, text));
		snprintf(text, sizeof(text), "P:%s", expected_path);
		CHECK(buffer_is_text(&path, text));
	} else {
		CHECK(buffer_is_text(&host, "H:"));
		CHECK(buffer_is_text(&path, "P:"));
	}
	wb_buffer_release(&host);
	wb_buffer_release(&path);
}

/* Checks the mapping between a local path and a file: URL. */
static void
test_urls(void)
{
	struct wb_buffer buffer;
	struct wb_buffer host;
	struct wb_buffer path;

	if (!is_win32) {
		check_from_url("", "/a b/c", 0, "/a b/c");
		check_from_url(NULL, "/a b/c", 0, "/a b/c");
		check_from_url("", "/", 0, "/");
		check_from_url("", "/\xe3\x81\x82/%41", 0, "/\xe3\x81\x82/%41");
		check_from_url("", "rel", 0, "rel");
		check_from_url("srv", "/a", ENOENT, NULL);
		check_from_url("localhost", "/a", ENOENT, NULL);

		check_to_url("/a/b", 0, "", "/a/b");
		check_to_url("/", 0, "", "/");
		check_to_url("//x", 0, "", "//x");
		check_to_url("rel", EINVAL, NULL, NULL);
		check_to_url("", EINVAL, NULL, NULL);
		check_to_url("C:\\x", EINVAL, NULL, NULL);
	} else {
		check_from_url("", "/C:/x/y", 0, "C:\\x\\y");
		check_from_url(NULL, "/C:/x/y", 0, "C:\\x\\y");
		check_from_url("", "/C:", 0, "C:\\");
		check_from_url("", "/C:/", 0, "C:\\");
		check_from_url("", "/c|/a", 0, "c:\\a");
		check_from_url("", "/z|", 0, "z:\\");
		check_from_url("", "/D:/a/b/", 0, "D:\\a\\b\\");
		check_from_url("", "/C:/a b/\xe3\x81\x82", 0, "C:\\a b\\\xe3\x81\x82");
		check_from_url("", "/x", ENOENT, NULL);
		check_from_url("", "/", ENOENT, NULL);
		check_from_url("", "", ENOENT, NULL);
		check_from_url("", "rel", ENOENT, NULL);
		check_from_url("", "C:/x", ENOENT, NULL);
		check_from_url("", "/C:x", ENOENT, NULL);
		check_from_url("", "/Cx/y", ENOENT, NULL);
		check_from_url("", "/1:/x", ENOENT, NULL);
		check_from_url("", "/share/a", ENOENT, NULL);
		check_from_url("srv", "/share/a", ENOENT, NULL);
		check_from_url("srv", "/C:/a", ENOENT, NULL);
		check_from_url("localhost", "/C:/a", ENOENT, NULL);

		check_to_url("C:\\a\\b", 0, "", "/C:/a/b");
		check_to_url("C:/a", 0, "", "/C:/a");
		check_to_url("c:\\", 0, "", "/c:/");
		check_to_url("C:\\a\\b\\", 0, "", "/C:/a/b/");
		check_to_url("C:\\a b\\\xe3\x81\x82", 0, "", "/C:/a b/\xe3\x81\x82");
		check_to_url("\\\\srv\\share\\x", 0, "srv", "/share/x");
		check_to_url("//srv/share/x", 0, "srv", "/share/x");
		check_to_url("\\\\srv\\share", 0, "srv", "/share");
		check_to_url("\\\\srv\\", 0, "srv", "/");
		check_to_url("\\\\srv", 0, "srv", "/");
		check_to_url("C:rel", EINVAL, NULL, NULL);
		check_to_url("C:", EINVAL, NULL, NULL);
		check_to_url("\\\\?\\C:\\x", EINVAL, NULL, NULL);
		check_to_url("//?/C:/x", EINVAL, NULL, NULL);
		check_to_url("\\\\.\\COM1", EINVAL, NULL, NULL);
		check_to_url("\\\\.", EINVAL, NULL, NULL);
		check_to_url("rel", EINVAL, NULL, NULL);
		check_to_url("rel\\x", EINVAL, NULL, NULL);
		check_to_url("\\x", EINVAL, NULL, NULL);
		check_to_url("/x", EINVAL, NULL, NULL);
		check_to_url("", EINVAL, NULL, NULL);
		check_to_url("\\\\", EINVAL, NULL, NULL);
		check_to_url("\\\\\\x", EINVAL, NULL, NULL);
	}

	/* A drive or absolute path survives the round trip through the URL form. */
	wb_buffer_init(&host);
	wb_buffer_init(&path);
	wb_buffer_init(&buffer);
	if (!is_win32) {
		CHECK_EQ(wb_os_url_from_path("/some/where", &host, &path), 0);
		CHECK_EQ(host.length, 0);
		CHECK_EQ(wb_os_path_from_url("", (const char *)path.data, &buffer), 0);
		CHECK(buffer_is_text(&buffer, "/some/where"));
	} else {
		CHECK_EQ(wb_os_url_from_path("D:\\some\\where", &host, &path), 0);
		CHECK_EQ(host.length, 0);
		CHECK_EQ(wb_os_path_from_url("", (const char *)path.data, &buffer), 0);
		CHECK(buffer_is_text(&buffer, "D:\\some\\where"));
	}
	wb_buffer_release(&buffer);
	wb_buffer_release(&host);
	wb_buffer_release(&path);
}

#ifndef _WIN32
/* Checks the modes of the files and directories the module makes (the umask is 022). */
static void
test_modes(void)
{
	struct stat status;
	struct wb_os_lock *lock;

	make_group("mode");

	CHECK_EQ(wb_os_file_write("mode/written", "x", 1), 0);
	CHECK_EQ(stat("mode/written", &status), 0);
	CHECK_EQ((long long)(status.st_mode & 07777), 0644);

	/* A write asks for 0644 and the umask takes bits away: with none taken 0644 is what is made. */
	umask(0);
	CHECK_EQ(wb_os_file_write("mode/open", "x", 1), 0);
	CHECK_EQ(stat("mode/open", &status), 0);
	CHECK_EQ((long long)(status.st_mode & 07777), 0644);
	umask(077);
	CHECK_EQ(wb_os_file_write("mode/closed", "x", 1), 0);
	CHECK_EQ(stat("mode/closed", &status), 0);
	CHECK_EQ((long long)(status.st_mode & 07777), 0600);
	umask(022);

	CHECK_EQ(wb_os_file_replace("mode/replaced", "x", 1), 0);
	CHECK_EQ(stat("mode/replaced", &status), 0);
	CHECK_EQ((long long)(status.st_mode & 07777), 0600);

	/* A file that was 0644 is private after a replacement. */
	CHECK_EQ(wb_os_file_replace("mode/written", "y", 1), 0);
	CHECK_EQ(stat("mode/written", &status), 0);
	CHECK_EQ((long long)(status.st_mode & 07777), 0600);

	CHECK_EQ(wb_os_make_directories("mode/d1/d2/d3"), 0);
	CHECK_EQ(stat("mode/d1", &status), 0);
	CHECK_EQ((long long)(status.st_mode & 07777), 0700);
	CHECK_EQ(stat("mode/d1/d2", &status), 0);
	CHECK_EQ((long long)(status.st_mode & 07777), 0700);
	CHECK_EQ(stat("mode/d1/d2/d3", &status), 0);
	CHECK_EQ((long long)(status.st_mode & 07777), 0700);

	lock = NULL;
	CHECK_EQ(wb_os_lock_open("mode/d1", &lock), 0);
	CHECK_EQ(stat("mode/d1/lock", &status), 0);
	CHECK_EQ((long long)(status.st_mode & 07777), 0600);
	wb_os_lock_close(lock);
}
#else
/* The state of the thread that lets go of a file after a short wait. */
struct release_later {
	HANDLE handle;
	unsigned long wait;
};

/* Closes a handle after a wait, so the retry of the replacement can be seen working. */
static void *
release_later_main(
	void *argument)
{
	struct release_later *release;

	release = (struct release_later *)argument;
	Sleep(release->wait);
	CloseHandle(release->handle);
	return NULL;
}

/* Checks how replacing a file behaves with other programs holding it open. */
static void
test_sharing(void)
{
	struct release_later release;
	pthread_t thread;
	wchar_t *wide;
	HANDLE handle;
	ULONGLONG started;
	ULONGLONG elapsed;
	const char *newest;
	size_t newest_length;
	int error;

	newest = "old";
	newest_length = 3;
	make_group("share");

	/* A reader that shares reads, writes and deletes never stops a replacement. */
	CHECK_EQ(wb_os_file_write("share/file", "old", 3), 0);
	wide = to_wide("share/file");
	handle = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(handle != INVALID_HANDLE_VALUE);
	error = wb_os_file_replace("share/file", "newer", 5);
	if (under_wine()) {
		/* Wine refuses with an access error and leaves the old file; Windows takes the replacement. */
		CHECK(error == 0 || error == EACCES);
	} else {
		CHECK_EQ(error, 0);
	}
	if (error == 0) {
		newest = "newer";
		newest_length = 5;
	}
	CHECK(raw_holds("share/file", newest, newest_length));
	CHECK(!listing_has_prefix("share", "file.tmp"));
	CloseHandle(handle);

	/* A holder that does not share deletes blocks the replacement, which is retried and then refused. */
	handle = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	    FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(handle != INVALID_HANDLE_VALUE);
	started = GetTickCount64();
	error = wb_os_file_replace("share/file", "blocked", 7);
	elapsed = GetTickCount64() - started;
	CHECK(error == EACCES || error == EBUSY);
	CHECK(elapsed >= 90U);
	CHECK(raw_holds("share/file", newest, newest_length));
	CHECK(!listing_has_prefix("share", "file.tmp"));

	/* The same holder letting go during the retries lets the replacement through. */
	release.handle = handle;
	release.wait = 30;
	pthread_create(&thread, NULL, release_later_main, &release);
	error = wb_os_file_replace("share/file", "late", 4);
	pthread_join(thread, NULL);
	CHECK_EQ(error, 0);
	CHECK(raw_holds("share/file", "late", 4));
	CHECK(!listing_has_prefix("share", "file.tmp"));

	/* A file held open without sharing reads is EBUSY to read. */
	handle = CreateFileW(wide, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(handle != INVALID_HANDLE_VALUE);
	{
		struct wb_buffer buffer;

		wb_buffer_init(&buffer);
		CHECK_EQ(wb_os_file_read("share/file", &buffer), EBUSY);
		CHECK_EQ(buffer.length, 0);
		wb_buffer_release(&buffer);
	}
	CloseHandle(handle);
	free(wide);
}
#endif

/* The thread that tries to take a lock. */
static void *
lock_attempt_main(
	void *argument)
{
	struct lock_attempt *attempt;

	attempt = (struct lock_attempt *)argument;
	attempt->error = wb_os_lock_open(attempt->directory, &attempt->lock);
	return NULL;
}

/* Starts this program again as the second process of the lock test; returns its exit code, or -1. */
static int
run_lock_child(
	const char *directory)
{
#ifdef _WIN32
	wchar_t exe[1024];
	wchar_t *wide_directory;
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
	wide_directory = to_wide(directory);
	_snwprintf(command, 2400, L"\"%ls\" lock-child \"%ls\"", exe, wide_directory);
	free(wide_directory);
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
	char *const arguments[4] = { (char *)self_path, (char *)"lock-child", (char *)directory, NULL };
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

/*
 * Starts a program that does nothing for a few seconds and inherits every
 * handle that can be inherited; the lock test uses it to see that the lock
 * is not inherited.  Returns a token for wait_sleeper, or 0.
 */
static uintptr_t
start_sleeper(void)
{
#ifdef _WIN32
	wchar_t exe[1024];
	wchar_t command[1100];
	STARTUPINFOW startup;
	PROCESS_INFORMATION process;
	DWORD units;

	units = GetModuleFileNameW(NULL, exe, 1024);
	if (units == 0 || units >= 1024)
		return 0;
	_snwprintf(command, 1100, L"\"%ls\" sleep-child", exe);
	memset(&startup, 0, sizeof(startup));
	startup.cb = sizeof(startup);
	memset(&process, 0, sizeof(process));
	if (CreateProcessW(NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process) == 0)
		return 0;
	CloseHandle(process.hThread);
	return (uintptr_t)process.hProcess;
#else
	char *const arguments[3] = { (char *)self_path, (char *)"sleep-child", NULL };
	pid_t pid;

	if (posix_spawn(&pid, self_path, NULL, NULL, arguments, environ) != 0)
		return 0;
	return (uintptr_t)pid;
#endif
}

/* Waits for the program start_sleeper started. */
static void
wait_sleeper(
	uintptr_t token)
{
#ifdef _WIN32
	WaitForSingleObject((HANDLE)token, 120000);
	CloseHandle((HANDLE)token);
#else
	int status;

	waitpid((pid_t)token, &status, 0);
#endif
}

/* Checks the directory lock, in one process and between two. */
static void
test_lock(void)
{
	struct wb_os_lock *first;
	struct wb_os_lock *second;
	struct lock_attempt attempts[8];
	pthread_t threads[8];
	uintptr_t sleeper;
	int winners;
	int busy;
	int i;

	make_group("lk");
	make_group("lk2");

	/* The first open takes the lock and makes the lock file. */
	first = NULL;
	CHECK_EQ(wb_os_lock_open("lk", &first), 0);
	CHECK(first != NULL);
	CHECK(raw_exists("lk/lock"));

	/* A second open in the same process is EBUSY, however the directory is spelled. */
	second = (struct wb_os_lock *)&sentinel_target;
	CHECK_EQ(wb_os_lock_open("lk", &second), EBUSY);
	CHECK(second == NULL);
	CHECK_EQ(wb_os_lock_open("lk/", &second), EBUSY);
	CHECK(second == NULL);
	CHECK_EQ(wb_os_lock_open("./lk/../lk", &second), EBUSY);
	CHECK(second == NULL);

	/* Another directory is free. */
	CHECK_EQ(wb_os_lock_open("lk2", &second), 0);
	CHECK(second != NULL);
	wb_os_lock_close(second);

	/* Another process cannot take it either. */
	CHECK_EQ(run_lock_child("lk"), EBUSY);

	/* After the release the lock file stays and the directory can be taken again, here and elsewhere. */
	wb_os_lock_close(first);
	CHECK(raw_exists("lk/lock"));
	CHECK_EQ(run_lock_child("lk"), 0);
	first = NULL;
	CHECK_EQ(wb_os_lock_open("lk", &first), 0);
	CHECK(first != NULL);
	wb_os_lock_close(first);

	/* A program started while the lock is held does not keep the lock after the holder lets go. */
	first = NULL;
	CHECK_EQ(wb_os_lock_open("lk", &first), 0);
	sleeper = start_sleeper();
	CHECK(sleeper != 0);
	wb_os_lock_close(first);
	CHECK_EQ(run_lock_child("lk"), 0);
	wait_sleeper(sleeper);

	/* A directory that is not there, or is a file. */
	second = (struct wb_os_lock *)&sentinel_target;
	CHECK_EQ(wb_os_lock_open("lk-missing", &second), ENOENT);
	CHECK(second == NULL);
	CHECK_EQ(wb_os_file_write("lk/plain", "x", 1), 0);
	second = (struct wb_os_lock *)&sentinel_target;
	CHECK_EQ(wb_os_lock_open("lk/plain", &second), ENOENT);
	CHECK(second == NULL);

	/* Several threads asking at once: exactly one gets it and the others find it busy. */
	for (i = 0; i < 8; i++) {
		attempts[i].directory = "lk";
		attempts[i].lock = NULL;
		attempts[i].error = -1;
	}
	for (i = 0; i < 8; i++)
		pthread_create(&threads[i], NULL, lock_attempt_main, &attempts[i]);
	for (i = 0; i < 8; i++)
		pthread_join(threads[i], NULL);
	winners = 0;
	busy = 0;
	for (i = 0; i < 8; i++) {
		if (attempts[i].error == 0 && attempts[i].lock != NULL)
			winners++;
		if (attempts[i].error == EBUSY && attempts[i].lock == NULL)
			busy++;
	}
	CHECK_EQ(winners, 1);
	CHECK_EQ(busy, 7);
	for (i = 0; i < 8; i++)
		wb_os_lock_close(attempts[i].lock);
}

/* The second process of the lock test: takes the lock, releases it, and exits with the result. */
static int
lock_child_main(
	const char *directory)
{
	struct wb_os_lock *lock;
	int error;

	lock = NULL;
	error = wb_os_lock_open(directory, &lock);
	if (error == 0)
		wb_os_lock_close(lock);
	return error;
}

int
main(
	int argc,
	char **argv)
{
	/* The second process of the lock test does its one job. */
	if (argc >= 3 && strcmp(argv[1], "lock-child") == 0)
		return lock_child_main(argv[2]);

	/* The program that only waits, to hold whatever it inherited. */
	if (argc >= 2 && strcmp(argv[1], "sleep-child") == 0) {
#ifdef _WIN32
		Sleep(3000);
#else
		sleep(3);
#endif
		return 0;
	}

	if (argc < 2) {
		printf("usage: test-os-file posix|win32\n");
		return 2;
	}
	is_win32 = strcmp(argv[1], "win32") == 0;
	self_path = argv[0];
#ifndef _WIN32
	umask(022);
#endif

	test_arguments();
	test_read_write();
	test_replace();
	test_replace_threads();
	test_remove();
	test_make_directories();
	test_list();
	test_utf8_names();
	test_full_path();
	test_current_directory();
	test_absolute();
	test_urls();
#ifndef _WIN32
	test_modes();
#else
	test_sharing();
#endif
	test_lock();

	return check_finish("os-file");
}
