/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test of an instance's storage: the cookie jar and Web Storage of
 * libbrowser's instances over the files backend and over a fake backend.
 *
 *   storage-test DIRECTORY PORT
 *
 * DIRECTORY is the test's own (empty) directory; the instances keep
 * their records under DIRECTORY/a and DIRECTORY/b.  PORT is the port of
 * an HTTP server on 127.0.0.1 that serves DIRECTORY/pages (set.html and
 * get.html, written by storage-test.sh): pages whose scripts set and read
 * document.cookie and localStorage and say what they read with
 * console.log.  DIRECTORY/pages/old.html is a file: page for the
 * localStorage record of the earlier format.
 *
 * What it checks:
 *  1. cookies of a response (net_cookie_store) and of a script
 *     (net_cookie_store_script, and a page's document.cookie): after the
 *     instance is destroyed and made again on the same directory only the
 *     persistent ones (Max-Age, Expires) come back; one whose Max-Age has
 *     run out does not, and neither does one a session cookie replaced;
 *  2. a page's localStorage comes back after the instance is made again,
 *     and a record of the earlier format (no origin) is read and written
 *     again in the new one at the next change;
 *  3. two instances on two directories share neither cookies nor
 *     localStorage; a second files backend on a directory in use is
 *     refused (EBUSY), and an instance without a backend still works;
 *  4. an instance with a view is not destroyed (EBUSY); the backend's
 *     close is called once, also when the instance cannot be made; a write
 *     the backend fails (EIO) is told on the view's console once;
 *  5. a view without an instance keeps nothing, even with XDG_DATA_HOME
 *     set (the engine reads no environment).
 * Last line: test-storage: PASS (<n> checks), or the failures.
 */

#include "check.h"

#include "net/net.h"
#include "storage/instance.h"
#include "storage/queue.h"
#include "storage/records.h"

#include <browser/browser.h>
#include <browser/storage.h>

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* The longest path the test makes. */
#define TEST_PATH_MAX		1024

/* The console lines a view of the test keeps. */
#define TEST_LINES_MAX		16

/* The origin of the file: pages, in hexadecimal: the name of its localStorage file of earlier engines. */
#define TEST_FILE_ORIGIN_HEX	"66696c653a2f2f"

/*
 * What a test view's console said: its lines (level and text) in order.
 */
struct test_console {
	char lines[TEST_LINES_MAX][256];
	int levels[TEST_LINES_MAX];
	int count;
};

/*
 * The fake backend's counters: the calls of each kind, and what write
 * returns.
 */
struct test_fake {
	int reads;
	int writes;
	int removes;
	int lists;
	int clears;
	int closes;
	int write_error;
};

/* The test's directory and the server's port, from the command line; set once by main. */
static const char *test_directory;
static const char *test_port;

/* The outermost frame of the main thread, which the views' heaps scan up to; set once by main. */
static const void *test_stack_base;

static int test_fake_read(void *context, unsigned space, const char *key, struct browser_storage_sink *sink, int *found);
static int test_fake_write(void *context, unsigned space, const char *key, const void *bytes, size_t length);
static int test_fake_remove(void *context, unsigned space, const char *key);
static int test_fake_list(void *context, unsigned space, int (*each)(void *each_context, const char *key), void *each_context);
static int test_fake_clear(void *context, unsigned space);
static void test_fake_close(void *context);
static void test_console_line(void *context, struct browser_view *view, int level, const char *text, size_t length);
static int test_files_instance(const char *name, struct browser_instance **instance);
static int test_view(struct browser_instance *instance, struct test_console *console, struct browser_view **view);
static int test_page(struct browser_instance *instance, const char *location, struct test_console *console);
static int test_cookie_header(struct browser_instance *instance, const char *url, char *header, size_t size);
static int test_store(struct browser_instance *instance, const char *url, const char *header, int from_script);
static int test_count_files(const char *name, const char *space);
static int test_read_file(const char *path, char *bytes, size_t size, size_t *length);
static int test_write_v1(const char *name);
static void test_cookies(void);
static void test_local_storage(void);
static void test_two_instances(void);
static void test_fake_backend(void);
static void test_no_instance(void);

/*
 * The fake backend's table: every call counted; reads find nothing, and
 * writes fail with the fake's write_error.  Constant for the life of the
 * program.
 */
static const struct browser_storage_calls test_fake_calls = {
	BROWSER_STORAGE_VERSION,
	test_fake_read,
	test_fake_write,
	test_fake_remove,
	test_fake_list,
	test_fake_clear,
	test_fake_close
};

/*
 * Runs the checks.
 */
int
main(
	int argc,
	char **argv)
{
	int status;

	/* The directory and the port. */
	if (argc != 3) {
		fprintf(stderr, "usage: storage-test DIRECTORY PORT\n");
		return 2;
	}
	test_directory = argv[1];
	test_port = argv[2];
	test_stack_base = __builtin_frame_address(0);

	/* The five parts, in order: each makes its instances and destroys them. */
	test_cookies();
	test_local_storage();
	test_two_instances();
	test_fake_backend();
	test_no_instance();

	/* Succeeded or not: the summary line. */
	status = check_finish("storage");
	return status;
}

/* Cookies set four ways, and which of them come back when the instance is made again. */
static void
test_cookies(void)
{
	struct browser_instance *instance;
	struct test_console console;
	char location[TEST_PATH_MAX];
	char header[1024];
	int error;
	int count;

	/* The instance on directory a, and the cookies of a response and of a script. */
	error = test_files_instance("a", &instance);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	CHECK_EQ(test_store(instance, "http://example.test/", "max=1; Max-Age=3600", 0), 0);
	CHECK_EQ(test_store(instance, "http://example.test/", "expires=2; Expires=Wed, 21 Oct 2099 07:28:00 GMT", 0), 0);
	CHECK_EQ(test_store(instance, "http://example.test/", "both=3; Expires=Thu, 01 Jan 1970 00:00:00 GMT; Max-Age=3600", 0), 0);
	CHECK_EQ(test_store(instance, "http://example.test/", "session=4", 0), 0);
	CHECK_EQ(test_store(instance, "http://example.test/", "short=5; Max-Age=1", 0), 0);
	CHECK_EQ(test_store(instance, "http://example.test/", "past=6; Expires=Sun, 06 Nov 1994 08:49:37 GMT", 0), 0);
	CHECK_EQ(test_store(instance, "http://example.test/", "replaced=7; Max-Age=3600", 0), 0);
	CHECK_EQ(test_store(instance, "http://example.test/", "replaced=8", 0), 0);
	CHECK_EQ(test_store(instance, "http://example.test/", "script=9; max-age=3600", 1), 0);

	/* The page's script sets one too, through document.cookie on its own origin. */
	snprintf(location, sizeof(location), "http://127.0.0.1:%s/set.html", test_port);
	error = test_page(instance, location, &console);
	CHECK_EQ(error, 0);
	CHECK(console.count >= 1 && strcmp(console.lines[0], "set") == 0);

	/* All but the past one are sent now; the Expires before the Max-Age does not end "both". */
	error = test_cookie_header(instance, "http://example.test/", header, sizeof(header));
	CHECK_EQ(error, 0);
	CHECK(strstr(header, "max=1") != NULL);
	CHECK(strstr(header, "expires=2") != NULL);
	CHECK(strstr(header, "both=3") != NULL);
	CHECK(strstr(header, "session=4") != NULL);
	CHECK(strstr(header, "short=5") != NULL);
	CHECK(strstr(header, "past=6") == NULL);
	CHECK(strstr(header, "replaced=8") != NULL);
	CHECK(strstr(header, "script=9") != NULL);

	/* The records written: max, expires, both, short, script (the session and the replaced ones have none). */
	wb_storage_queue_flush(instance->queue);
	count = test_count_files("a", "cookies");
	CHECK_EQ(count, 6);
	CHECK_EQ(browser_instance_destroy(instance), 0);

	/* After "short" has run out, the instance again: only the persistent ones still alive come back. */
	nanosleep(&(struct timespec){ 1, 300000000 }, NULL);
	error = test_files_instance("a", &instance);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	error = test_cookie_header(instance, "http://example.test/", header, sizeof(header));
	CHECK_EQ(error, 0);
	CHECK(strstr(header, "max=1") != NULL);
	CHECK(strstr(header, "expires=2") != NULL);
	CHECK(strstr(header, "both=3") != NULL);
	CHECK(strstr(header, "session=4") == NULL);
	CHECK(strstr(header, "short=5") == NULL);
	CHECK(strstr(header, "replaced=") == NULL);
	CHECK(strstr(header, "script=9") != NULL);

	/* The page's cookie, on its own origin, came back too, and the expired record was removed. */
	snprintf(location, sizeof(location), "http://127.0.0.1:%s/", test_port);
	error = test_cookie_header(instance, location, header, sizeof(header));
	CHECK_EQ(error, 0);
	CHECK(strstr(header, "page=10") != NULL);
	wb_storage_queue_flush(instance->queue);
	count = test_count_files("a", "cookies");
	CHECK_EQ(count, 5);
	CHECK_EQ(browser_instance_destroy(instance), 0);
}

/* A page's localStorage after the instance is made again, and the record of the earlier format. */
static void
test_local_storage(void)
{
	struct browser_instance *instance;
	struct test_console console;
	char location[TEST_PATH_MAX];
	char path[TEST_PATH_MAX];
	char bytes[256];
	size_t length;
	int error;

	/* The record of an earlier engine for the file: pages, then the instance on directory a. */
	error = test_write_v1("a");
	CHECK_EQ(error, 0);
	error = test_files_instance("a", &instance);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;

	/* set.html stored its item in the first part; get.html reads it and the cookie. */
	snprintf(location, sizeof(location), "http://127.0.0.1:%s/get.html", test_port);
	error = test_page(instance, location, &console);
	CHECK_EQ(error, 0);
	CHECK(console.count >= 1 && strcmp(console.lines[0], "item=kept cookie=page=10") == 0);

	/* The file: page reads the old record's item and adds one, which writes the record again. */
	snprintf(location, sizeof(location), "%s/pages/old.html", test_directory);
	error = test_page(instance, location, &console);
	CHECK_EQ(error, 0);
	CHECK(console.count >= 1 && strcmp(console.lines[0], "old=from-v1") == 0);
	wb_storage_queue_flush(instance->queue);
	CHECK_EQ(browser_instance_destroy(instance), 0);

	/* The record is of the new format now, with the origin and both items. */
	snprintf(path, sizeof(path), "%s/a/local-storage/%s.ls", test_directory, TEST_FILE_ORIGIN_HEX);
	error = test_read_file(path, bytes, sizeof(bytes), &length);
	CHECK_EQ(error, 0);
	CHECK(length > strlen(WB_STORAGE_MAGIC_LOCAL_2) && memcmp(bytes, WB_STORAGE_MAGIC_LOCAL_2, strlen(WB_STORAGE_MAGIC_LOCAL_2)) == 0);

	/* Made again, the file: page sees both items. */
	error = test_files_instance("a", &instance);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	error = test_page(instance, location, &console);
	CHECK_EQ(error, 0);
	CHECK(console.count >= 2 && strcmp(console.lines[1], "new=written") == 0);
	CHECK_EQ(browser_instance_destroy(instance), 0);
}

/* Two instances keep apart; a directory in use is refused; an instance without a backend works. */
static void
test_two_instances(void)
{
	const struct browser_storage_calls *calls;
	struct browser_instance_options options;
	struct browser_instance *first;
	struct browser_instance *second;
	struct browser_instance *bare;
	struct test_console console;
	char location[TEST_PATH_MAX];
	char path[TEST_PATH_MAX];
	char header[1024];
	void *context;
	int error;

	/* Directory a (with what the parts before left) and directory b (empty). */
	error = test_files_instance("a", &first);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	error = test_files_instance("b", &second);
	CHECK_EQ(error, 0);
	if (error != 0) {
		browser_instance_destroy(first);
		return;
	}

	/* b has none of a's cookies, and a cookie set in b stays in b. */
	CHECK_EQ(test_cookie_header(second, "http://example.test/", header, sizeof(header)), 0);
	CHECK_EQ(strlen(header), 0);
	CHECK_EQ(test_store(second, "http://example.test/", "only_b=1; Max-Age=3600", 0), 0);
	CHECK_EQ(test_cookie_header(first, "http://example.test/", header, sizeof(header)), 0);
	CHECK(strstr(header, "only_b") == NULL);

	/* b's page sees no item and no cookie of a's; a's still sees its own. */
	snprintf(location, sizeof(location), "http://127.0.0.1:%s/get.html", test_port);
	CHECK_EQ(test_page(second, location, &console), 0);
	CHECK(console.count >= 1 && strcmp(console.lines[0], "item=null cookie=") == 0);
	CHECK_EQ(test_page(first, location, &console), 0);
	CHECK(console.count >= 1 && strcmp(console.lines[0], "item=kept cookie=page=10") == 0);

	/* A second backend on a, which the first instance holds, is refused. */
	snprintf(path, sizeof(path), "%s/a", test_directory);
	calls = NULL;
	context = NULL;
	error = browser_storage_files_open(path, &calls, &context);
	CHECK_EQ(error, EBUSY);
	CHECK(calls == NULL && context == NULL);

	/* An instance without a backend works and keeps nothing of a's. */
	memset(&options, 0, sizeof(options));
	options.version = BROWSER_API_VERSION;
	error = browser_instance_create(&options, &bare);
	CHECK_EQ(error, 0);
	if (error == 0) {
		CHECK_EQ(test_page(bare, location, &console), 0);
		CHECK(console.count >= 1 && strcmp(console.lines[0], "item=null cookie=") == 0);
		CHECK_EQ(browser_instance_destroy(bare), 0);
	}

	/* Both go. */
	CHECK_EQ(browser_instance_destroy(second), 0);
	CHECK_EQ(browser_instance_destroy(first), 0);
}

/* A fake backend: the close is called once, the instance with a view refuses to go, and a failed write is told once. */
static void
test_fake_backend(void)
{
	struct browser_storage_calls broken;
	struct browser_instance_options options;
	struct browser_instance *instance;
	struct browser_view *view;
	struct test_console console;
	struct test_fake fake;
	char location[TEST_PATH_MAX];
	int storage_lines;
	int index;
	int error;

	/* Options of another version: refused, and the backend closed once. */
	memset(&fake, 0, sizeof(fake));
	memset(&options, 0, sizeof(options));
	options.version = BROWSER_API_VERSION - 1;
	options.storage = &test_fake_calls;
	options.storage_context = &fake;
	error = browser_instance_create(&options, &instance);
	CHECK_EQ(error, ENOTSUP);
	CHECK(instance == NULL);
	CHECK_EQ(fake.closes, 1);

	/* A table of another version, and one missing a call: the same. */
	broken = test_fake_calls;
	broken.version = BROWSER_STORAGE_VERSION + 1;
	options.version = BROWSER_API_VERSION;
	options.storage = &broken;
	fake.closes = 0;
	error = browser_instance_create(&options, &instance);
	CHECK_EQ(error, ENOTSUP);
	CHECK_EQ(fake.closes, 1);
	broken = test_fake_calls;
	broken.write = NULL;
	fake.closes = 0;
	error = browser_instance_create(&options, &instance);
	CHECK_EQ(error, ENOTSUP);
	CHECK_EQ(fake.closes, 1);

	/* A good table whose writes fail. */
	memset(&fake, 0, sizeof(fake));
	fake.write_error = EIO;
	options.storage = &test_fake_calls;
	error = browser_instance_create(&options, &instance);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	CHECK(fake.lists >= 1);

	/* A view on the file: page, whose new item is written, and fails. */
	error = test_view(instance, &console, &view);
	CHECK_EQ(error, 0);
	if (error != 0) {
		browser_instance_destroy(instance);
		return;
	}
	snprintf(location, sizeof(location), "%s/pages/old.html", test_directory);
	CHECK_EQ(browser_view_load(view, location), 0);
	CHECK_EQ(browser_view_settle(view, 0, 0), 0);
	wb_storage_queue_flush(instance->queue);
	CHECK(fake.writes >= 1);

	/* The view tells it on its console once, however often it runs. */
	browser_view_process(view);
	browser_view_process(view);
	storage_lines = 0;
	for (index = 0; index < console.count; index++) {
		if (strncmp(console.lines[index], "storage: could not save (", 25) == 0 && console.levels[index] == 1)
			storage_lines++;
	}
	CHECK_EQ(storage_lines, 1);

	/* The instance refuses to go while the view lives, and closes the backend once after. */
	CHECK_EQ(browser_instance_destroy(instance), EBUSY);
	CHECK_EQ(fake.closes, 0);
	browser_view_destroy(view);
	CHECK_EQ(browser_instance_destroy(instance), 0);
	CHECK_EQ(fake.closes, 1);
	CHECK_EQ(browser_instance_destroy(NULL), EINVAL);
}

/* A view made without an instance writes nothing, whatever the environment says. */
static void
test_no_instance(void)
{
	struct browser_view *view;
	struct test_console console;
	char location[TEST_PATH_MAX];
	char data[TEST_PATH_MAX];
	int count;
	int error;

	/* XDG_DATA_HOME, where earlier engines wrote localStorage. */
	snprintf(data, sizeof(data), "%s/xdg", test_directory);
	setenv("XDG_DATA_HOME", data, 1);

	/* The file: page sets an item in the thread's default instance. */
	error = test_view(NULL, &console, &view);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	snprintf(location, sizeof(location), "%s/pages/old.html", test_directory);
	CHECK_EQ(browser_view_load(view, location), 0);
	CHECK_EQ(browser_view_settle(view, 0, 0), 0);
	CHECK(console.count >= 1 && strcmp(console.lines[0], "old=null") == 0);
	browser_view_destroy(view);

	/* Nothing was written under it. */
	count = test_count_files("xdg/keiland/browser", "local-storage");
	CHECK_EQ(count, -1);
}

/* Makes an instance over the files backend on DIRECTORY/name. */
static int
test_files_instance(
	const char *name,
	struct browser_instance **instance)
{
	const struct browser_storage_calls *calls;
	struct browser_instance_options options;
	char path[TEST_PATH_MAX];
	void *context;
	int error;

	/* The backend. */
	snprintf(path, sizeof(path), "%s/%s", test_directory, name);
	error = browser_storage_files_open(path, &calls, &context);
	if (error != 0)
		return error;

	/* The instance over it. */
	memset(&options, 0, sizeof(options));
	options.version = BROWSER_API_VERSION;
	options.storage = calls;
	options.storage_context = context;
	error = browser_instance_create(&options, instance);
	if (error != 0)
		return error;

	/* Succeeded: the instance. */
	return 0;
}

/* Makes a view of an instance (NULL for the thread's default) whose console goes to a test console. */
static int
test_view(
	struct browser_instance *instance,
	struct test_console *console,
	struct browser_view **view)
{
	static struct browser_callbacks callbacks;
	struct browser_view_options options;
	int error;

	/* The console, empty. */
	memset(console, 0, sizeof(*console));
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.console = test_console_line;
	callbacks.context = console;

	/* The view, every page read at once. */
	memset(&options, 0, sizeof(options));
	options.version = BROWSER_API_VERSION;
	options.callbacks = &callbacks;
	options.stack_base = test_stack_base;
	options.width = 320;
	options.height = 240;
	options.fetch = BROWSER_FETCH_AT_ONCE;
	options.instance = instance;
	error = browser_view_create(&options, view);
	if (error != 0)
		return error;

	/* Succeeded: the view. */
	return 0;
}

/* Loads a page in a new view of an instance, runs its scripts and ends the view. */
static int
test_page(
	struct browser_instance *instance,
	const char *location,
	struct test_console *console)
{
	struct browser_view *view;
	int error;

	/* The view. */
	error = test_view(instance, console, &view);
	if (error != 0)
		return error;

	/* The page, settled. */
	error = browser_view_load(view, location);
	if (error == 0)
		error = browser_view_settle(view, 0, 0);
	browser_view_destroy(view);
	if (error != 0)
		return error;

	/* Succeeded: the page ran. */
	return 0;
}

/* Writes the Cookie header an instance sends to a URL, without "Cookie: " and the CR LF ("" for none). */
static int
test_cookie_header(
	struct browser_instance *instance,
	const char *url,
	char *header,
	size_t size)
{
	struct net_url parsed;
	struct wb_buffer out;
	const char *text;
	size_t length;
	int error;

	/* The header. */
	header[0] = '\0';
	error = net_url_parse(url, strlen(url), NULL, &parsed);
	if (error != 0)
		return error;
	wb_buffer_init(&out);
	error = net_cookie_header(instance->cookies, &parsed, &out);
	net_url_release(&parsed);
	if (error != 0) {
		wb_buffer_release(&out);
		return error;
	}

	/* Its value. */
	text = wb_buffer_string(&out);
	length = out.length;
	if (length >= 10U && strncmp(text, "Cookie: ", 8) == 0) {
		snprintf(header, size, "%.*s", (int)(length - 10U), text + 8);
	}
	wb_buffer_release(&out);

	/* Succeeded: the pairs. */
	return 0;
}

/* Stores a cookie in an instance's jar as a response (or as a script) of a URL would. */
static int
test_store(
	struct browser_instance *instance,
	const char *url,
	const char *header,
	int from_script)
{
	struct net_url parsed;
	int error;

	/* The URL. */
	error = net_url_parse(url, strlen(url), NULL, &parsed);
	if (error != 0)
		return error;

	/* The cookie. */
	if (from_script) {
		error = net_cookie_store_script(instance->cookies, &parsed, header, strlen(header));
	} else {
		error = net_cookie_store(instance->cookies, &parsed, header, strlen(header));
	}
	net_url_release(&parsed);
	if (error != 0)
		return error;

	/* Succeeded: the jar has it. */
	return 0;
}

/* Counts the files of a space's directory under DIRECTORY/name (-1 when there is no such directory). */
static int
test_count_files(
	const char *name,
	const char *space)
{
	char path[TEST_PATH_MAX];
	struct dirent *entry;
	DIR *directory;
	int count;

	/* The directory. */
	snprintf(path, sizeof(path), "%s/%s/%s", test_directory, name, space);
	directory = opendir(path);
	if (directory == NULL)
		return -1;

	/* Its entries but . and .. (and the replace's temporary files, whose names have a dot). */
	count = 0;
	while ((entry = readdir(directory)) != NULL) {
		if (entry->d_name[0] == '.')
			continue;
		if (strstr(entry->d_name, ".tmp") != NULL)
			continue;
		count++;
	}
	closedir(directory);

	/* Succeeded: the count. */
	return count;
}

/* Reads a whole small file. */
static int
test_read_file(
	const char *path,
	char *bytes,
	size_t size,
	size_t *length)
{
	FILE *file;

	/* The bytes. */
	*length = 0;
	file = fopen(path, "rb");
	if (file == NULL)
		return errno;
	*length = fread(bytes, 1, size, file);
	fclose(file);

	/* Succeeded: the bytes are read. */
	return 0;
}

/*
 * Writes the localStorage file an earlier engine kept for the file:
 * pages: the magic of the first format and one item, old = from-v1.
 */
static int
test_write_v1(
	const char *name)
{
	static const char key[] = "old";
	static const char value[] = "from-v1";
	unsigned char bytes[128];
	char path[TEST_PATH_MAX];
	size_t length;
	size_t index;
	FILE *file;

	/* The magic, the two lengths and the UTF-16 units of the key and the value. */
	length = strlen(WB_STORAGE_MAGIC_LOCAL_1);
	memcpy(bytes, WB_STORAGE_MAGIC_LOCAL_1, length);
	bytes[length++] = (unsigned char)(sizeof(key) - 1U);
	bytes[length++] = 0;
	bytes[length++] = 0;
	bytes[length++] = 0;
	bytes[length++] = (unsigned char)(sizeof(value) - 1U);
	bytes[length++] = 0;
	bytes[length++] = 0;
	bytes[length++] = 0;
	for (index = 0; index < sizeof(key) - 1U; index++) {
		bytes[length++] = (unsigned char)key[index];
		bytes[length++] = 0;
	}
	for (index = 0; index < sizeof(value) - 1U; index++) {
		bytes[length++] = (unsigned char)value[index];
		bytes[length++] = 0;
	}

	/* The file, where earlier engines kept it under the directory. */
	snprintf(path, sizeof(path), "%s/%s/local-storage", test_directory, name);
	(void)mkdir(path, 0700);
	snprintf(path, sizeof(path), "%s/%s/local-storage/%s.ls", test_directory, name, TEST_FILE_ORIGIN_HEX);
	file = fopen(path, "wb");
	if (file == NULL)
		return errno;
	if (fwrite(bytes, 1, length, file) != length) {
		fclose(file);
		return EIO;
	}
	fclose(file);

	/* Succeeded: the old record is there. */
	return 0;
}

/* The fake's read: counted, finds nothing. */
static int
test_fake_read(
	void *context,
	unsigned space,
	const char *key,
	struct browser_storage_sink *sink,
	int *found)
{
	struct test_fake *fake;

	UNUSED_PARAMETER(space);
	UNUSED_PARAMETER(key);
	UNUSED_PARAMETER(sink);

	/* Nothing is there. */
	fake = context;
	fake->reads++;
	*found = 0;
	return 0;
}

/* The fake's write: counted, fails with write_error. */
static int
test_fake_write(
	void *context,
	unsigned space,
	const char *key,
	const void *bytes,
	size_t length)
{
	struct test_fake *fake;

	UNUSED_PARAMETER(space);
	UNUSED_PARAMETER(key);
	UNUSED_PARAMETER(bytes);
	UNUSED_PARAMETER(length);

	/* The error the test chose. */
	fake = context;
	fake->writes++;
	return fake->write_error;
}

/* The fake's remove: counted. */
static int
test_fake_remove(
	void *context,
	unsigned space,
	const char *key)
{
	struct test_fake *fake;

	UNUSED_PARAMETER(space);
	UNUSED_PARAMETER(key);

	/* Nothing to remove. */
	fake = context;
	fake->removes++;
	return 0;
}

/* The fake's list: counted, lists nothing. */
static int
test_fake_list(
	void *context,
	unsigned space,
	int (*each)(void *each_context, const char *key),
	void *each_context)
{
	struct test_fake *fake;

	UNUSED_PARAMETER(space);
	UNUSED_PARAMETER(each);
	UNUSED_PARAMETER(each_context);

	/* No key. */
	fake = context;
	fake->lists++;
	return 0;
}

/* The fake's clear: counted. */
static int
test_fake_clear(
	void *context,
	unsigned space)
{
	struct test_fake *fake;

	UNUSED_PARAMETER(space);

	/* Nothing to clear. */
	fake = context;
	fake->clears++;
	return 0;
}

/* The fake's close: counted (the instance must call it once). */
static void
test_fake_close(
	void *context)
{
	struct test_fake *fake;

	/* One more close. */
	fake = context;
	fake->closes++;
}

/* A view's console: each line kept with its level. */
static void
test_console_line(
	void *context,
	struct browser_view *view,
	int level,
	const char *text,
	size_t length)
{
	struct test_console *console;

	UNUSED_PARAMETER(view);

	/* The line, cut to its room. */
	console = context;
	if (console->count == TEST_LINES_MAX)
		return;
	snprintf(console->lines[console->count], sizeof(console->lines[0]), "%.*s", (int)length, text);
	console->levels[console->count] = level;
	console->count++;
}
