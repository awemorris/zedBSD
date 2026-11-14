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
 *   storage-test DIRECTORY PORT CACHE_PORT
 *
 * DIRECTORY is the test's own (empty) directory; the instances keep
 * their records under DIRECTORY/a and DIRECTORY/b.  PORT is the port of
 * an HTTP server on 127.0.0.1 that serves DIRECTORY/pages (set.html and
 * get.html, written by storage-test.sh): pages whose scripts set and read
 * document.cookie and localStorage and say what they read with
 * console.log.  DIRECTORY/pages/old.html is a file: page for the
 * localStorage record of the earlier format.  CACHE_PORT is the port of
 * the server of cache-server.py (the cache part, which also keeps its
 * instances under DIRECTORY/c and DIRECTORY/d).
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
 *     set (the engine reads no environment);
 *  6. the persistent HTTP cache: loaders of an instance store the
 *     responses that may be kept (a max-age, an ETag) and not the
 *     no-store ones, plain responses and POSTs; a new instance answers
 *     the same URLs from the records, without the server being asked
 *     (and, at the end, with the server stopped); a stale ETag entry is
 *     revalidated (304) and kept fresh again across instances, replaced
 *     by a 200 with a new body, and dropped by a 200 that may not be
 *     kept (no-store), in memory and in its record; a record
 *     that does not decode is removed; records the index does not list
 *     and an index that does not decode are swept when the instance is
 *     made; the records' total is kept under the limit by dropping the
 *     least recently used ones, also when the limit was lowered.
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
#include <unistd.h>

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

/* The test's directory and the two servers' ports, from the command line; set once by main. */
static const char *test_directory;
static const char *test_port;
static const char *test_cache_port;

/*
 * What a loader's request found out: whether its callback ran, how it
 * ended, the status and the first bytes and the length of the body.
 */
struct test_fetch {
	int done;
	int error;
	int status;
	char body[64];
	size_t length;
};

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
static int test_files_instance_limit(const char *name, size_t limit, struct browser_instance **instance);
static void test_fetch_done(void *context, struct net_request *request);
static int test_fetch(struct net_loader *loader, const char *path, const struct net_send *send, struct test_fetch *result);
static int test_server_count(const char *path, int *hits, int *conditional);
static void test_cache_url(const char *path, char *url, size_t size);
static void test_cache_path(const char *name, const char *path, char *file, size_t size);
static int test_cache_has(struct browser_instance *instance, const char *path);
static int test_file_exists(const char *path);
static void test_write_file(const char *path, const char *bytes, size_t length);
static void test_pause(long milliseconds);
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
static void test_cache(void);
static void test_cache_limit(void);
static void test_cache_changed(void);
static void test_cache_gone(void);
static void test_cache_offline(void);

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
	if (argc != 4) {
		fprintf(stderr, "usage: storage-test DIRECTORY PORT CACHE_PORT\n");
		return 2;
	}
	test_directory = argv[1];
	test_port = argv[2];
	test_cache_port = argv[3];
	test_stack_base = __builtin_frame_address(0);

	/* The parts, in order: each makes its instances and destroys them. */
	test_cookies();
	test_local_storage();
	test_two_instances();
	test_fake_backend();
	test_no_instance();
	test_cache();
	test_cache_limit();
	test_cache_changed();
	test_cache_gone();
	test_cache_offline();

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
	CHECK_EQ(test_store(instance, "http://example.test/", "past=6; Expires=Sun, 06 Nov 1994 08:49:37 GMT", 0), 0);
	CHECK_EQ(test_store(instance, "http://example.test/", "replaced=7; Max-Age=3600", 0), 0);
	CHECK_EQ(test_store(instance, "http://example.test/", "replaced=8", 0), 0);
	CHECK_EQ(test_store(instance, "http://example.test/", "script=9; max-age=3600", 1), 0);

	/* The page's script sets one too, through document.cookie on its own origin. */
	snprintf(location, sizeof(location), "http://127.0.0.1:%s/set.html", test_port);
	error = test_page(instance, location, &console);
	CHECK_EQ(error, 0);
	CHECK(console.count >= 1 && strcmp(console.lines[0], "set") == 0);

	/*
	 * The short-lived one only now: the page's localStorage waited for the
	 * records queued before it, so a slow disk could have used up its
	 * Max-Age before the header below was made.  Nothing between the two
	 * waits for the storage.
	 */
	CHECK_EQ(test_store(instance, "http://example.test/", "short=5; Max-Age=2", 0), 0);

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

	/* The records written: max, expires, both, short, script and the page's (the session and the replaced ones have none). */
	wb_storage_queue_flush(instance->queue);
	count = test_count_files("a", "cookies");
	CHECK_EQ(count, 6);
	CHECK_EQ(browser_instance_destroy(instance), 0);

	/* After "short" has run out, the instance again: only the persistent ones still alive come back. */
	nanosleep(&(struct timespec){ 2, 300000000 }, NULL);
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
	int error;

	/* The default limit of the persistent HTTP cache. */
	error = test_files_instance_limit(name, 0, instance);
	if (error != 0)
		return error;

	/* Succeeded: the instance. */
	return 0;
}

/* Makes an instance over the files backend on DIRECTORY/name whose persistent HTTP cache keeps at most limit bytes (0 for the default). */
static int
test_files_instance_limit(
	const char *name,
	size_t limit,
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
	options.cache_limit = limit;
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

/* Notes a loader's request ended: its outcome and the first bytes of its body. */
static void
test_fetch_done(
	void *context,
	struct net_request *request)
{
	struct test_fetch *result;
	const struct net_response *response;
	size_t length;

	/* The outcome. */
	result = context;
	result->done = 1;
	result->error = net_request_error(request);
	if (result->error != 0)
		return;

	/* The status and as much of the body as the result holds. */
	response = net_request_response(request);
	result->status = response->status;
	result->length = response->body.length;
	length = response->body.length;
	if (length > sizeof(result->body) - 1U)
		length = sizeof(result->body) - 1U;
	memcpy(result->body, response->body.data, length);
	result->body[length] = '\0';
}

/* Fetches a path of the cache server through a loader (a GET, or what send says), waiting up to 30 s. */
static int
test_fetch(
	struct net_loader *loader,
	const char *path,
	const struct net_send *send,
	struct test_fetch *result)
{
	char url[TEST_PATH_MAX];
	int error;
	int tries;

	/* The request. */
	memset(result, 0, sizeof(*result));
	test_cache_url(path, url, sizeof(url));
	error = net_loader_send(loader, url, NET_FETCH_DOCUMENT, send, test_fetch_done, result, NULL);
	if (error != 0)
		return error;

	/* Until it ends. */
	tries = 0;
	while (!result->done && tries < 30) {
		net_loader_wait(loader, 1000);
		net_loader_dispatch(loader);
		tries++;
	}
	if (!result->done)
		return ETIMEDOUT;

	/* Succeeded: the request ended, well or with result->error. */
	return result->error;
}

/* Asks the cache server how many requests a path had and how many of them were conditional. */
static int
test_server_count(
	const char *path,
	int *hits,
	int *conditional)
{
	struct net_response response;
	char url[TEST_PATH_MAX];
	char text[TEST_PATH_MAX];
	int error;
	int matched;

	/* The count, by a fetch that no cache and no jar is part of. */
	snprintf(text, sizeof(text), "/count?path=%s", path);
	test_cache_url(text, url, sizeof(url));
	net_response_init(&response);
	error = net_http_fetch(NULL, url, &response);
	if (error != 0) {
		net_response_release(&response);
		return error;
	}

	/* Its two numbers. */
	*hits = -1;
	*conditional = -1;
	snprintf(text, sizeof(text), "%.*s", (int)response.body.length, (const char *)response.body.data);
	matched = sscanf(text, "hits=%d conditional=%d", hits, conditional);
	net_response_release(&response);
	if (matched != 2)
		return EINVAL;

	/* Succeeded: the numbers. */
	return 0;
}

/* Writes the URL of a path of the cache server. */
static void
test_cache_url(
	const char *path,
	char *url,
	size_t size)
{
	/* The server is on the loopback. */
	snprintf(url, size, "http://127.0.0.1:%s%s", test_cache_port, path);
}

/* Writes where the files backend keeps the cache record of a URL's path under DIRECTORY/name. */
static void
test_cache_path(
	const char *name,
	const char *path,
	char *file,
	size_t size)
{
	char url[TEST_PATH_MAX];
	char key[BROWSER_STORAGE_KEY_MAX + 1];

	/* The record's key is the URL's, collision 0. */
	test_cache_url(path, url, sizeof(url));
	wb_storage_key_text(url, strlen(url), 0, key);
	snprintf(file, size, "%s/%s/cache/%s", test_directory, name, key);
}

/* Reports 1 when the instance's persistent cache answers a path (read through the index, as a loader does), else 0. */
static int
test_cache_has(
	struct browser_instance *instance,
	const char *path)
{
	struct wb_buffer record;
	char url[TEST_PATH_MAX];
	int found;
	int error;

	/* The read, which also counts as a use. */
	test_cache_url(path, url, sizeof(url));
	wb_buffer_init(&record);
	found = 0;
	error = net_cache_read(instance->cache, url, strlen(url), &record, &found);
	wb_buffer_release(&record);
	if (error != 0)
		return 0;

	/* Whether it was found. */
	return found;
}

/* Reports 1 when a file exists. */
static int
test_file_exists(
	const char *path)
{
	struct stat status;
	int error;

	/* The file's status. */
	error = stat(path, &status);
	if (error != 0)
		return 0;

	/* It exists. */
	return 1;
}

/* Writes bytes to a file, replacing it. */
static void
test_write_file(
	const char *path,
	const char *bytes,
	size_t length)
{
	FILE *file;

	/* The file. */
	file = fopen(path, "wb");
	if (file == NULL) {
		CHECK(file != NULL);
		return;
	}
	fwrite(bytes, 1, length, file);
	fclose(file);
}

/* Waits some milliseconds. */
static void
test_pause(
	long milliseconds)
{
	struct timespec time;

	/* The sleep. */
	time.tv_sec = milliseconds / 1000;
	time.tv_nsec = (milliseconds % 1000) * 1000000L;
	nanosleep(&time, NULL);
}

/*
 * The persistent cache over instances on DIRECTORY/c: what is stored,
 * what answers from the records, revalidation, damaged records, and at
 * the end the server stopped.
 */
static void
test_cache(void)
{
	struct browser_instance *instance;
	struct net_loader *loader;
	struct test_fetch result;
	struct net_send post;
	char file[TEST_PATH_MAX];
	char other[TEST_PATH_MAX];
	char url[TEST_PATH_MAX];
	char record[1024];
	size_t record_length;
	int hits;
	int conditional;
	int error;

	/* The first instance and its loader, which share the instance's jar and persistent cache. */
	error = test_files_instance("c", &instance);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	CHECK(instance->cache != NULL);
	error = net_loader_create(NULL, NULL, &loader);
	CHECK_EQ(error, 0);
	if (error != 0) {
		browser_instance_destroy(instance);
		return;
	}
	net_loader_set_cookies(loader, instance->cookies);
	net_loader_set_cache(loader, instance->cache);

	/* A response with a max-age is fetched, and the memory cache answers the second time. */
	CHECK_EQ(test_fetch(loader, "/max-age", NULL, &result), 0);
	CHECK_EQ(result.status, 200);
	CHECK(strcmp(result.body, "max-age-body") == 0);
	CHECK_EQ(test_fetch(loader, "/max-age", NULL, &result), 0);
	CHECK(strcmp(result.body, "max-age-body") == 0);
	CHECK_EQ(test_server_count("/max-age", &hits, &conditional), 0);
	CHECK_EQ(hits, 1);

	/* An ETag response with a 1 s max-age, a no-store one, a plain one and two POSTs. */
	CHECK_EQ(test_fetch(loader, "/etag", NULL, &result), 0);
	CHECK(strcmp(result.body, "etag-body") == 0);
	CHECK_EQ(test_fetch(loader, "/no-store", NULL, &result), 0);
	CHECK_EQ(test_fetch(loader, "/plain", NULL, &result), 0);
	memset(&post, 0, sizeof(post));
	post.method = "POST";
	post.body = (const unsigned char *)"x";
	post.body_length = 1;
	CHECK_EQ(test_fetch(loader, "/post", &post, &result), 0);
	CHECK_EQ(test_fetch(loader, "/post", &post, &result), 0);
	CHECK_EQ(test_server_count("/post", &hits, &conditional), 0);
	CHECK_EQ(hits, 2);

	/* The instance and loader end; the records are written. */
	net_loader_destroy(loader);
	CHECK_EQ(browser_instance_destroy(instance), 0);
	sync();

	/* Only the two responses that may be kept are stored: their records and the index. */
	test_cache_path("c", "/max-age", file, sizeof(file));
	CHECK(test_file_exists(file));
	test_cache_path("c", "/etag", file, sizeof(file));
	CHECK(test_file_exists(file));
	test_cache_path("c", "/no-store", file, sizeof(file));
	CHECK(!test_file_exists(file));
	test_cache_path("c", "/plain", file, sizeof(file));
	CHECK(!test_file_exists(file));
	test_cache_path("c", "/post", file, sizeof(file));
	CHECK(!test_file_exists(file));
	CHECK_EQ(test_count_files("c", "cache"), 3);

	/* A second instance on the same directory answers the fresh URL from its records: the server is not asked. */
	test_pause(1300);
	error = test_files_instance("c", &instance);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	error = net_loader_create(NULL, NULL, &loader);
	CHECK_EQ(error, 0);
	if (error != 0) {
		browser_instance_destroy(instance);
		return;
	}
	net_loader_set_cookies(loader, instance->cookies);
	net_loader_set_cache(loader, instance->cache);
	CHECK_EQ(test_fetch(loader, "/max-age", NULL, &result), 0);
	CHECK_EQ(result.status, 200);
	CHECK(strcmp(result.body, "max-age-body") == 0);
	CHECK_EQ(test_server_count("/max-age", &hits, &conditional), 0);
	CHECK_EQ(hits, 1);

	/* The ETag one is stale (1 s): it is revalidated, and the 304 answers with the kept body. */
	CHECK_EQ(test_fetch(loader, "/etag", NULL, &result), 0);
	CHECK_EQ(result.status, 200);
	CHECK(strcmp(result.body, "etag-body") == 0);
	CHECK_EQ(test_server_count("/etag", &hits, &conditional), 0);
	CHECK_EQ(hits, 2);
	CHECK_EQ(conditional, 1);

	/* The 304 said max-age 3600: it is fresh now, in memory and in its record. */
	CHECK_EQ(test_fetch(loader, "/etag", NULL, &result), 0);
	CHECK_EQ(test_server_count("/etag", &hits, &conditional), 0);
	CHECK_EQ(hits, 2);
	net_loader_destroy(loader);
	CHECK_EQ(browser_instance_destroy(instance), 0);
	sync();
	error = test_files_instance("c", &instance);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	error = net_loader_create(NULL, NULL, &loader);
	CHECK_EQ(error, 0);
	if (error != 0) {
		browser_instance_destroy(instance);
		return;
	}
	net_loader_set_cookies(loader, instance->cookies);
	net_loader_set_cache(loader, instance->cache);
	CHECK_EQ(test_fetch(loader, "/etag", NULL, &result), 0);
	CHECK(strcmp(result.body, "etag-body") == 0);
	CHECK_EQ(test_server_count("/etag", &hits, &conditional), 0);
	CHECK_EQ(hits, 2);
	net_loader_destroy(loader);
	CHECK_EQ(browser_instance_destroy(instance), 0);
	sync();

	/* A record that holds another URL's response (a shared key) is no answer, and is left for its owner. */
	test_cache_path("c", "/etag", other, sizeof(other));
	test_cache_path("c", "/max-age", file, sizeof(file));
	CHECK_EQ(test_read_file(other, record, sizeof(record), &record_length), 0);
	CHECK(record_length > 0);
	test_write_file(file, record, record_length);
	error = test_files_instance("c", &instance);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	CHECK(!test_cache_has(instance, "/max-age"));
	wb_storage_queue_flush(instance->queue);
	CHECK(test_file_exists(file));
	CHECK(test_cache_has(instance, "/etag"));
	CHECK_EQ(browser_instance_destroy(instance), 0);
	sync();

	/* A record that does not decode is removed when it is read, and the URL is fetched again. */
	test_write_file(file, "garbage", 7);
	error = test_files_instance("c", &instance);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	CHECK(!test_cache_has(instance, "/max-age"));
	wb_storage_queue_flush(instance->queue);
	CHECK(!test_file_exists(file));
	CHECK(test_cache_has(instance, "/etag"));
	CHECK_EQ(browser_instance_destroy(instance), 0);
	sync();

	/* A record the index does not list (the index was not written) is removed when the instance is made. */
	test_cache_url("/stray", url, sizeof(url));
	snprintf(file, sizeof(file), "%s/c/cache/abcd", test_directory);
	test_write_file(file, "stray", 5);
	CHECK(test_file_exists(file));
	error = test_files_instance("c", &instance);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	wb_storage_queue_flush(instance->queue);
	CHECK(!test_file_exists(file));
	CHECK(test_cache_has(instance, "/etag"));
	CHECK_EQ(browser_instance_destroy(instance), 0);
	sync();

	/* An index that does not decode (its magic line altered) is dropped as empty: the records it listed go. */
	snprintf(file, sizeof(file), "%s/c/cache/0", test_directory);
	CHECK_EQ(test_read_file(file, record, sizeof(record), &record_length), 0);
	CHECK(record_length > 22);
	record[0] = 'X';
	test_write_file(file, record, record_length);
	error = test_files_instance("c", &instance);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	wb_storage_queue_flush(instance->queue);
	test_cache_path("c", "/etag", file, sizeof(file));
	CHECK(!test_file_exists(file));
	CHECK(!test_cache_has(instance, "/etag"));

	/* The cache works again on the emptied index: a fetch stores and survives the instance. */
	error = net_loader_create(NULL, NULL, &loader);
	CHECK_EQ(error, 0);
	if (error != 0) {
		browser_instance_destroy(instance);
		return;
	}
	net_loader_set_cookies(loader, instance->cookies);
	net_loader_set_cache(loader, instance->cache);
	CHECK_EQ(test_fetch(loader, "/max-age", NULL, &result), 0);
	CHECK_EQ(test_server_count("/max-age", &hits, &conditional), 0);
	CHECK_EQ(hits, 2);
	net_loader_destroy(loader);
	CHECK_EQ(browser_instance_destroy(instance), 0);
	sync();
}

/*
 * The limit of the persistent cache, on DIRECTORY/d: the records' total
 * stays under it by dropping the least recently used response, and a
 * lower limit at the next opening brings it down.
 */
static void
test_cache_limit(void)
{
	struct browser_instance *instance;
	struct net_loader *loader;
	struct test_fetch result;
	char file[TEST_PATH_MAX];
	int error;

	/* An instance that keeps about three 100000-byte responses, and its loader. */
	error = test_files_instance_limit("d", 350000, &instance);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	error = net_loader_create(NULL, NULL, &loader);
	CHECK_EQ(error, 0);
	if (error != 0) {
		browser_instance_destroy(instance);
		return;
	}
	net_loader_set_cookies(loader, instance->cookies);
	net_loader_set_cache(loader, instance->cache);

	/* Three responses fit; each is used a little later than the one before. */
	CHECK_EQ(test_fetch(loader, "/blob/1", NULL, &result), 0);
	CHECK_EQ(result.length, 100000U);
	test_pause(30);
	CHECK_EQ(test_fetch(loader, "/blob/2", NULL, &result), 0);
	test_pause(30);
	CHECK_EQ(test_fetch(loader, "/blob/3", NULL, &result), 0);
	test_pause(30);
	CHECK(test_cache_has(instance, "/blob/1"));
	CHECK(test_cache_has(instance, "/blob/2"));
	CHECK(test_cache_has(instance, "/blob/3"));

	/* Reading the first one makes the second the least recently used. */
	test_pause(30);
	CHECK(test_cache_has(instance, "/blob/1"));
	test_pause(30);
	CHECK(test_cache_has(instance, "/blob/3"));
	test_pause(30);

	/* A fourth pushes the total over the limit: the second goes, from the index and from the storage. */
	CHECK_EQ(test_fetch(loader, "/blob/4", NULL, &result), 0);
	wb_storage_queue_flush(instance->queue);
	CHECK(test_cache_has(instance, "/blob/1"));
	test_pause(30);
	CHECK(!test_cache_has(instance, "/blob/2"));
	CHECK(test_cache_has(instance, "/blob/3"));
	test_pause(30);
	CHECK(test_cache_has(instance, "/blob/4"));
	test_cache_path("d", "/blob/2", file, sizeof(file));
	CHECK(!test_file_exists(file));
	CHECK_EQ(test_count_files("d", "cache"), 4);
	net_loader_destroy(loader);
	CHECK_EQ(browser_instance_destroy(instance), 0);
	sync();

	/* The index came back with the times: a lower limit keeps only the most recently used. */
	error = test_files_instance_limit("d", 150000, &instance);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	wb_storage_queue_flush(instance->queue);
	CHECK(!test_cache_has(instance, "/blob/1"));
	CHECK(!test_cache_has(instance, "/blob/3"));
	CHECK(test_cache_has(instance, "/blob/4"));
	CHECK_EQ(test_count_files("d", "cache"), 2);

	CHECK_EQ(browser_instance_destroy(instance), 0);
	sync();
}

/*
 * A stale response revalidated by If-None-Match that comes back as a 200
 * with a new body (not a 304) replaces the kept one, in memory and in
 * its record, so the next request is answered from the cache with the new
 * body, in the same instance and in a later one.
 */
static void
test_cache_changed(void)
{
	struct browser_instance *instance;
	struct net_loader *loader;
	struct test_fetch result;
	int hits;
	int conditional;
	int error;

	/* The first body, kept with a 1 s max-age. */
	error = test_files_instance("e", &instance);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	error = net_loader_create(NULL, NULL, &loader);
	CHECK_EQ(error, 0);
	if (error != 0) {
		browser_instance_destroy(instance);
		return;
	}
	net_loader_set_cookies(loader, instance->cookies);
	net_loader_set_cache(loader, instance->cache);
	CHECK_EQ(test_fetch(loader, "/etag-change", NULL, &result), 0);
	CHECK(strcmp(result.body, "change-v1") == 0);

	/* Stale: revalidated, and the server answers 200 with the new body. */
	test_pause(1300);
	CHECK_EQ(test_fetch(loader, "/etag-change", NULL, &result), 0);
	CHECK_EQ(result.status, 200);
	CHECK(strcmp(result.body, "change-v2") == 0);
	CHECK_EQ(test_server_count("/etag-change", &hits, &conditional), 0);
	CHECK_EQ(hits, 2);
	CHECK_EQ(conditional, 1);

	/* The new body is fresh (3600 s): the next request is answered from the cache. */
	CHECK_EQ(test_fetch(loader, "/etag-change", NULL, &result), 0);
	CHECK(strcmp(result.body, "change-v2") == 0);
	CHECK_EQ(test_server_count("/etag-change", &hits, &conditional), 0);
	CHECK_EQ(hits, 2);
	net_loader_destroy(loader);
	CHECK_EQ(browser_instance_destroy(instance), 0);
	sync();

	/* A later instance has the new body in its record, fresh: the server is not asked. */
	error = test_files_instance("e", &instance);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	error = net_loader_create(NULL, NULL, &loader);
	CHECK_EQ(error, 0);
	if (error != 0) {
		browser_instance_destroy(instance);
		return;
	}
	net_loader_set_cookies(loader, instance->cookies);
	net_loader_set_cache(loader, instance->cache);
	CHECK_EQ(test_fetch(loader, "/etag-change", NULL, &result), 0);
	CHECK(strcmp(result.body, "change-v2") == 0);
	CHECK_EQ(test_server_count("/etag-change", &hits, &conditional), 0);
	CHECK_EQ(hits, 2);
	net_loader_destroy(loader);
	CHECK_EQ(browser_instance_destroy(instance), 0);
	sync();
}

/*
 * A stale response revalidated by If-None-Match that comes back as a 200
 * that may not be kept (no-store) takes the kept one out of the memory
 * cache and its record: the next request asks without If-None-Match, in
 * the same instance and in a later one.
 */
static void
test_cache_gone(void)
{
	struct browser_instance *instance;
	struct net_loader *loader;
	struct test_fetch result;
	int hits;
	int conditional;
	int error;

	/* The first body, kept with a 1 s max-age and an ETag. */
	error = test_files_instance("g", &instance);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	error = net_loader_create(NULL, NULL, &loader);
	CHECK_EQ(error, 0);
	if (error != 0) {
		browser_instance_destroy(instance);
		return;
	}
	net_loader_set_cookies(loader, instance->cookies);
	net_loader_set_cache(loader, instance->cache);
	CHECK_EQ(test_fetch(loader, "/etag-gone", NULL, &result), 0);
	CHECK(strcmp(result.body, "gone-v1") == 0);
	net_loader_destroy(loader);
	CHECK_EQ(browser_instance_destroy(instance), 0);
	sync();

	/* A later instance has the record; stale, it is revalidated and the server answers a no-store 200. */
	error = test_files_instance("g", &instance);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	CHECK(test_cache_has(instance, "/etag-gone"));
	error = net_loader_create(NULL, NULL, &loader);
	CHECK_EQ(error, 0);
	if (error != 0) {
		browser_instance_destroy(instance);
		return;
	}
	net_loader_set_cookies(loader, instance->cookies);
	net_loader_set_cache(loader, instance->cache);
	test_pause(1300);
	CHECK_EQ(test_fetch(loader, "/etag-gone", NULL, &result), 0);
	CHECK_EQ(result.status, 200);
	CHECK(strcmp(result.body, "gone-v2") == 0);
	CHECK_EQ(test_server_count("/etag-gone", &hits, &conditional), 0);
	CHECK_EQ(hits, 2);
	CHECK_EQ(conditional, 1);

	/* The old entry is gone from memory: the next request does not revalidate it. */
	CHECK_EQ(test_fetch(loader, "/etag-gone", NULL, &result), 0);
	CHECK(strcmp(result.body, "gone-v3") == 0);
	CHECK_EQ(test_server_count("/etag-gone", &hits, &conditional), 0);
	CHECK_EQ(hits, 3);
	CHECK_EQ(conditional, 1);
	net_loader_destroy(loader);
	CHECK_EQ(browser_instance_destroy(instance), 0);
	sync();

	/* And from the records: a later instance has none, and asks plainly. */
	error = test_files_instance("g", &instance);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	CHECK(!test_cache_has(instance, "/etag-gone"));
	error = net_loader_create(NULL, NULL, &loader);
	CHECK_EQ(error, 0);
	if (error != 0) {
		browser_instance_destroy(instance);
		return;
	}
	net_loader_set_cookies(loader, instance->cookies);
	net_loader_set_cache(loader, instance->cache);
	CHECK_EQ(test_fetch(loader, "/etag-gone", NULL, &result), 0);
	CHECK(strcmp(result.body, "gone-v3") == 0);
	CHECK_EQ(test_server_count("/etag-gone", &hits, &conditional), 0);
	CHECK_EQ(hits, 4);
	CHECK_EQ(conditional, 1);
	net_loader_destroy(loader);
	CHECK_EQ(browser_instance_destroy(instance), 0);
	sync();
}

/*
 * The server is stopped; a new instance on DIRECTORY/c still answers a
 * stored URL from its record.
 */
static void
test_cache_offline(void)
{
	struct browser_instance *instance;
	struct net_response response;
	struct net_loader *loader;
	struct test_fetch result;
	char url[TEST_PATH_MAX];
	int hits;
	int conditional;
	int error;

	/* The server stops, and no longer answers. */
	test_cache_url("/quit", url, sizeof(url));
	net_response_init(&response);
	(void)net_http_fetch(NULL, url, &response);
	net_response_release(&response);
	test_pause(300);
	error = test_server_count("/max-age", &hits, &conditional);
	CHECK(error != 0);

	/* A new instance over the same records, and its loader. */
	error = test_files_instance("c", &instance);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	error = net_loader_create(NULL, NULL, &loader);
	CHECK_EQ(error, 0);
	if (error != 0) {
		browser_instance_destroy(instance);
		return;
	}
	net_loader_set_cookies(loader, instance->cookies);
	net_loader_set_cache(loader, instance->cache);

	/* The stored URL answers from its record. */
	CHECK_EQ(test_fetch(loader, "/max-age", NULL, &result), 0);
	CHECK_EQ(result.status, 200);
	CHECK(strcmp(result.body, "max-age-body") == 0);
	net_loader_destroy(loader);
	CHECK_EQ(browser_instance_destroy(instance), 0);
	sync();
}
