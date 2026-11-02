/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The termination of a task of the JavaScript engine from another thread
 * (vm_heap_request_termination): a second thread asks the running task to
 * end 200 ms after it began, the way the window's thread will on a
 * navigation or a close, and each script must leave within 300 ms of the
 * request with VM_TERMINATED, running no catch, no finally and no
 * microtask.  A request that names an earlier task ends nothing, and after
 * a terminated task the next task's script runs as usual.  The last case
 * runs through a page's window: the console says so once for the task, and
 * the window is marked (its page is not kept for going back).
 *
 *   host-terminate      (prints one line per case, then "host-terminate: N/M passed")
 */

#include "bind/internal.h"
#include "js/builtin.h"
#include "page/page.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* The most live bytes the test's heap may keep (a case builds a long JSON text and its array). */
#define TERMINATE_HEAP_LIMIT	((size_t)2048U * 1024U * 1024U)

/* How long after a task's beginning the other thread asks it to end, and the longest the task may take to leave. */
#define TERMINATE_DELAY_NS	200000000LL
#define TERMINATE_LIMIT_NS	300000000LL

/*
 * One case: its name, the script (which runs longer than the delay), and
 * whether the microtasks after it run in the same task (an async
 * function's awaits, a promise chain).
 */
struct terminate_case {
	const char *name;
	const char *source;
	int jobs;
};

/*
 * The other thread's errand: the heap whose running task it ends after
 * the delay, and the moment it asked (CLOCK_MONOTONIC, ns).  stale, when
 * not 0, is the number it asks for instead of the running task's.
 */
struct terminate_errand {
	struct vm_heap *heap;
	uint32_t stale;
	long long asked_at;
};

/* What the scripts printed during the running case; print appends, each case empties it. */
static struct wb_buffer terminate_printed;

/* The console lines the page's window wrote, and how many. */
static struct wb_buffer terminate_console;
static unsigned terminate_console_lines;

/* The cases that passed and that ran. */
static unsigned terminate_passed;
static unsigned terminate_ran;

/* The scripts that must be ended within the limit. */
static const struct terminate_case terminate_cases[] = {
	{ "for (;;) {}", "for (;;) {}", 0 },
	{ "while (1) try {} finally {}", "while (1) try {} finally {}", 0 },
	{ "try { for (;;) {} } catch", "try { for (;;) {} } catch (e) { print('catch'); } finally { print('finally'); }", 0 },
	{ "sort with a slow comparator",
	  "var a = []; for (var i = 0; i < 100000; i++) a.push(100000 - i);"
	  " try { a.sort(function (x, y) { for (var k = 0; k < 100; k++) {} return x - y; }); } finally { print('finally'); }", 0 },
	{ "sort without a comparator (native compares)",
	  "var a = Array(4000000).fill(7); try { a.sort(); } catch (e) { print('catch'); } finally { print('finally'); }", 0 },
	{ "recursion",
	  "function f(n) { if (n > 0) { f(n - 1); f(n - 1); } } try { f(40); } catch (e) { print('catch'); } finally { print('finally'); }", 0 },
	{ "loop in a generator",
	  "function* g() { for (;;) yield 1; } try { for (var x of g()) {} } finally { print('finally'); }", 0 },
	{ "generator's own loop",
	  "function* h() { try { for (;;) {} } finally { print('finally'); } } h().next();", 0 },
	{ "loop in an async function",
	  "async function f() { try { for (;;) {} } finally { print('finally'); } } f().catch(function () { print('catch'); });", 1 },
	{ "async function awaiting in a loop",
	  "async function f() { try { for (;;) await 0; } finally { print('finally'); } } f().catch(function () { print('catch'); });", 1 },
	{ "promise chain in microtasks", "function f() { Promise.resolve().then(f); } f();", 1 },
	{ "regular expression backtracking",
	  "try { /(a+)+$/.test('a'.repeat(40) + 'b'); } catch (e) { print('catch ' + e); }", 0 },
	{ "JSON.parse of a long text",
	  "try { JSON.parse('[' + '0,'.repeat(40000000) + '0]'); } catch (e) { print('catch'); }", 0 },
	{ "JSON.stringify of a long array",
	  "var a = Array(40000000).fill(0); try { JSON.stringify(a); } finally { print('finally'); }", 0 },
	{ "String.prototype.repeat", "try { 'a'.repeat(260000000); } finally { print('finally'); }", 0 },
	{ "listener-like native callback",
	  "try { [1, 2, 3].forEach(function () { for (;;) {} }); } catch (e) { print('catch'); }", 0 }
};

static long long terminate_now(void);
static void *terminate_thread(void *context);
static int terminate_print(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int terminate_units(const char *text, struct wb_units *units);
static void terminate_check(int passed, const char *name, const char *detail);
static int terminate_run(struct vm_realm *realm, const char *source, int jobs);
static void terminate_case_run(struct vm_realm *realm, const struct terminate_case *test);
static void terminate_next_task(struct vm_realm *realm, const char *after);
static void terminate_stale(struct vm_realm *realm);
static void terminate_page(void);
static void terminate_page_console(void *context, int level, const char *text, size_t length);

/* Runs every case and prints the summary; the exit status is 0 only when all passed. */
int
main(
	void)
{
	struct vm_heap *heap;
	struct vm_realm *realm;
	size_t index;
	int status;

	/* The heap and the realm, with print. */
	wb_buffer_init(&terminate_printed);
	wb_buffer_init(&terminate_console);
	status = vm_heap_create(&heap, TERMINATE_HEAP_LIMIT);
	if (status != 0)
		return 2;
	vm_heap_set_stack_base(heap, __builtin_frame_address(0));
	status = vm_realm_create(heap, &realm);
	if (status == 0)
		status = js_install_builtins(realm);
	if (status == 0)
		status = js_builtin_method(realm, realm->global, "print", 1, terminate_print);
	if (status != 0)
		return 2;

	/* Each case ends within the limit, and the next task runs. */
	for (index = 0; index < sizeof(terminate_cases) / sizeof(terminate_cases[0]); index++) {
		terminate_case_run(realm, &terminate_cases[index]);
		terminate_next_task(realm, terminate_cases[index].name);
	}

	/* A request for an earlier task ends nothing. */
	terminate_stale(realm);

	/* The same through a page's window. */
	vm_realm_destroy(realm);
	vm_heap_destroy(heap);
	terminate_page();

	/* The summary. */
	printf("host-terminate: %u/%u passed\n", terminate_passed, terminate_ran);
	wb_buffer_release(&terminate_printed);
	wb_buffer_release(&terminate_console);
	if (terminate_passed != terminate_ran)
		return 1;

	/* Succeeded: every case passed. */
	return 0;
}

/* The monotonic clock in nanoseconds. */
static long long
terminate_now(void)
{
	struct timespec now;

	/* The clock. */
	clock_gettime(CLOCK_MONOTONIC, &now);

	/* Succeeded: the time. */
	return (long long)now.tv_sec * 1000000000LL + now.tv_nsec;
}

/* The other thread: waits the delay, then asks the heap's running task (or the stale number) to end. */
static void *
terminate_thread(
	void *context)
{
	struct terminate_errand *errand;
	struct timespec delay;
	uint32_t task;

	/* The delay. */
	errand = context;
	delay.tv_sec = 0;
	delay.tv_nsec = TERMINATE_DELAY_NS;
	nanosleep(&delay, NULL);

	/* The task that runs now, as the window's thread reads it, or the stale number. */
	task = vm_heap_task_current(errand->heap);
	if (errand->stale != 0U)
		task = errand->stale;
	errand->asked_at = terminate_now();
	vm_heap_request_termination(errand->heap, task);

	/* Done. */
	return NULL;
}

/* print(...): appends its arguments' strings and a line's end to what the case printed. */
static int
terminate_print(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	unsigned index;
	int status;

	UNUSED_PARAMETER(this_value);

	/* Each argument's string. */
	*result = VM_VALUE_UNDEFINED;
	for (index = 0; index < count; index++) {
		status = vm_to_string(realm, args[index], &string);
		if (status != 0)
			return status;
		vm_string_to_utf8(string, &terminate_printed);
	}

	/* The line's end. */
	wb_buffer_append_string(&terminate_printed, "\n");

	/* Succeeded: print returns undefined. */
	return 0;
}

/* Makes the UTF-16 units of an ASCII text. */
static int
terminate_units(
	const char *text,
	struct wb_units *units)
{
	uint16_t unit;
	size_t index;
	int status;

	/* Each byte is one unit. */
	wb_units_init(units);
	for (index = 0; text[index] != '\0'; index++) {
		unit = (uint16_t)(unsigned char)text[index];
		status = wb_units_append(units, &unit, 1);
		if (status != 0)
			return status;
	}

	/* Succeeded: the units. */
	return 0;
}

/* Counts one check and prints its line. */
static void
terminate_check(
	int passed,
	const char *name,
	const char *detail)
{
	/* The count. */
	terminate_ran++;
	if (passed)
		terminate_passed++;

	/* The line. */
	printf("%s %s%s%s\n", passed ? "PASS" : "FAIL", name, detail[0] != '\0' ? ": " : "", detail);
}

/* Runs a script (and, when asked, its microtasks) in the running task; returns the engine's status. */
static int
terminate_run(
	struct vm_realm *realm,
	const char *source,
	int jobs)
{
	struct js_syntax_error syntax;
	struct wb_units units;
	vm_value completion;
	int status;

	/* The script. */
	status = terminate_units(source, &units);
	if (status != 0)
		return status;
	status = js_run_script(realm, units.data, units.length, 0, &completion, &syntax);
	wb_units_release(&units);

	/* Its microtasks in the same task. */
	if (status == 0 && jobs)
		status = vm_run_jobs(realm, NULL, NULL);

	/* The exception of a script that threw is the case's to report. */
	if (status == VM_THROWN)
		realm->exception = VM_VALUE_UNDEFINED;

	/* Succeeded or not: the engine's status. */
	return status;
}

/* Runs one case in a task of its own while the other thread asks the task to end. */
static void
terminate_case_run(
	struct vm_realm *realm,
	const struct terminate_case *test)
{
	struct terminate_errand errand;
	pthread_t thread;
	char detail[256];
	long long ended;
	long long late;
	int passed;
	int status;

	/* The task, and the other thread. */
	wb_buffer_clear(&terminate_printed);
	memset(&errand, 0, sizeof(errand));
	errand.heap = realm->heap;
	vm_heap_task_begin(realm->heap);
	status = pthread_create(&thread, NULL, terminate_thread, &errand);
	if (status != 0) {
		terminate_check(0, test->name, "no thread");
		vm_heap_task_end(realm->heap);
		return;
	}

	/* The script until it leaves. */
	status = terminate_run(realm, test->source, test->jobs);
	ended = terminate_now();
	pthread_join(thread, NULL);
	vm_heap_task_end(realm->heap);

	/* It left by the request, soon enough, ran no catch or finally and left the engine as it was. */
	late = ended - errand.asked_at;
	passed = 1;
	if (status != VM_TERMINATED || late > TERMINATE_LIMIT_NS || errand.asked_at == 0)
		passed = 0;
	if (terminate_printed.length != 0 || realm->depth != 0 || realm->stack_top != 0)
		passed = 0;
	if (realm->jobs.count != 0 || realm->exception != VM_VALUE_UNDEFINED)
		passed = 0;
	snprintf(detail, sizeof(detail), "status %d, left %.1f ms after the request, printed \"%s\", depth %u, stack %u, jobs %zu",
	    status, (double)late / 1e6, wb_buffer_string(&terminate_printed), realm->depth, (unsigned)realm->stack_top,
	    realm->jobs.count);
	terminate_check(passed, test->name, detail);
}

/* Runs an ordinary script in the next task: it must run to its end. */
static void
terminate_next_task(
	struct vm_realm *realm,
	const char *after)
{
	char name[256];
	char detail[256];
	int passed;
	int status;

	/* The task with a loop and a print. */
	wb_buffer_clear(&terminate_printed);
	vm_heap_task_begin(realm->heap);
	status = terminate_run(realm, "var n = 0; for (var i = 0; i < 1000; i++) n += i; print('next ' + n);", 0);
	vm_heap_task_end(realm->heap);

	/* It ran to its end. */
	passed = 0;
	if (status == 0 && strcmp(wb_buffer_string(&terminate_printed), "next 499500\n") == 0)
		passed = 1;
	snprintf(name, sizeof(name), "the next task after \"%s\"", after);
	snprintf(detail, sizeof(detail), "status %d, printed \"%s\"", status, wb_buffer_string(&terminate_printed));
	terminate_check(passed, name, detail);
}

/* A request that names an earlier task (written late) ends neither the task that runs nor the next one. */
static void
terminate_stale(
	struct vm_realm *realm)
{
	struct terminate_errand errand;
	pthread_t thread;
	char detail[256];
	uint32_t earlier;
	uint32_t running;
	int passed;
	int status;

	/* An earlier task, ended. */
	earlier = vm_heap_task_begin(realm->heap);
	vm_heap_task_end(realm->heap);

	/* A request for it arrives while the next task runs a loop longer than the delay. */
	wb_buffer_clear(&terminate_printed);
	memset(&errand, 0, sizeof(errand));
	errand.heap = realm->heap;
	errand.stale = earlier;
	running = vm_heap_task_begin(realm->heap);
	status = pthread_create(&thread, NULL, terminate_thread, &errand);
	if (status != 0) {
		terminate_check(0, "a request for an earlier task", "no thread");
		vm_heap_task_end(realm->heap);
		return;
	}
	status = terminate_run(realm, "var t = Date.now(); while (Date.now() - t < 500) {} print('done');", 0);
	pthread_join(thread, NULL);

	/* The running task went to its end. */
	passed = 0;
	if (status == 0 && strcmp(wb_buffer_string(&terminate_printed), "done\n") == 0 && running != earlier)
		passed = 1;
	snprintf(detail, sizeof(detail), "status %d, printed \"%s\"", status, wb_buffer_string(&terminate_printed));
	terminate_check(passed, "a request for an earlier task leaves the running one", detail);

	/* A request for the running task that comes after its end does not fall on the next task. */
	vm_heap_task_end(realm->heap);
	vm_heap_request_termination(realm->heap, running);
	wb_buffer_clear(&terminate_printed);
	vm_heap_task_begin(realm->heap);
	status = terminate_run(realm, "for (var i = 0; i < 100000; i++) {} print('done');", 0);
	vm_heap_task_end(realm->heap);
	passed = 0;
	if (status == 0 && strcmp(wb_buffer_string(&terminate_printed), "done\n") == 0)
		passed = 1;
	snprintf(detail, sizeof(detail), "status %d, printed \"%s\"", status, wb_buffer_string(&terminate_printed));
	terminate_check(passed, "a late request does not fall on the next task", detail);

	/* No task, no request: a request between tasks is ignored by the next one too. */
	vm_heap_request_termination(realm->heap, 0);
	vm_heap_task_begin(realm->heap);
	status = terminate_run(realm, "1 + 1;", 0);
	vm_heap_task_end(realm->heap);
	terminate_check(status == 0, "a request for no task asks nothing", "");
}

/* Keeps a page's console lines. */
static void
terminate_page_console(
	void *context,
	int level,
	const char *text,
	size_t length)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(level);

	/* The line. */
	wb_buffer_append(&terminate_console, text, length);
	wb_buffer_append_string(&terminate_console, "\n");
	terminate_console_lines++;
}

/* A page's script ended through its window: one console line, no microtask, the window marked, the next task runs. */
static void
terminate_page(void)
{
	static const unsigned char html[] = "<!doctype html><html><body></body></html>";
	static const char looping[] = "Promise.resolve().then(function () { print('microtask'); }); for (;;) {}";
	struct terminate_errand errand;
	struct wb_units units;
	struct page *page;
	pthread_t thread;
	char detail[512];
	long long ended;
	int passed;
	int status;

	/* The page, with print and the console kept. */
	status = page_create(&page, __builtin_frame_address(0));
	if (status != 0) {
		terminate_check(0, "page", "no page");
		return;
	}
	status = page_load_html(page, html, sizeof(html) - 1U);
	if (status == 0)
		status = js_builtin_method(page->realm, page->realm->global, "print", 1, terminate_print);
	if (status != 0) {
		terminate_check(0, "page", "no document");
		page_destroy(page);
		return;
	}
	page_set_console(page, terminate_page_console, NULL);

	/* A script that queues a microtask and loops, ended from the other thread. */
	wb_buffer_clear(&terminate_printed);
	memset(&errand, 0, sizeof(errand));
	errand.heap = page->heap;
	vm_heap_task_begin(page->heap);
	status = pthread_create(&thread, NULL, terminate_thread, &errand);
	if (status != 0) {
		terminate_check(0, "page", "no thread");
		vm_heap_task_end(page->heap);
		page_destroy(page);
		return;
	}
	status = terminate_units(looping, &units);
	if (status == 0)
		status = bind_run_script(page->window, units.data, units.length, "looping");
	wb_units_release(&units);
	ended = terminate_now();
	pthread_join(thread, NULL);
	passed = 0;
	if (status == VM_TERMINATED &&
	    ended - errand.asked_at <= TERMINATE_LIMIT_NS &&
	    terminate_printed.length == 0 &&
	    terminate_console_lines == 1 &&
	    page->realm->jobs.count == 0)
		passed = 1;
	snprintf(detail, sizeof(detail), "status %d, left %.1f ms after the request, printed \"%s\", console %u \"%s\"",
	    status, (double)(ended - errand.asked_at) / 1e6, wb_buffer_string(&terminate_printed), terminate_console_lines,
	    wb_buffer_string(&terminate_console));
	terminate_check(passed, "page: the script ends, its microtask does not run, one console line", detail);

	/* Another script of the same task stops at once, without a second line. */
	status = terminate_units("print('same task');", &units);
	if (status == 0)
		status = bind_run_script(page->window, units.data, units.length, "same task");
	wb_units_release(&units);
	vm_heap_task_end(page->heap);
	passed = 0;
	if (status == VM_TERMINATED && terminate_printed.length == 0 && terminate_console_lines == 1)
		passed = 1;
	snprintf(detail, sizeof(detail), "status %d, printed \"%s\", console %u", status, wb_buffer_string(&terminate_printed),
	    terminate_console_lines);
	terminate_check(passed, "page: a later script of the same task does not run", detail);

	/* The window is marked, so that its page is not kept for going back. */
	status = bind_window_was_terminated(page->window);
	terminate_check(status == 1, "page: the window is marked as stopped", "");

	/* The next task's script runs. */
	vm_heap_task_begin(page->heap);
	status = terminate_units("print('after');", &units);
	if (status == 0)
		status = bind_run_script(page->window, units.data, units.length, "after");
	wb_units_release(&units);
	vm_heap_task_end(page->heap);
	passed = 0;
	if (status == 0 && strcmp(wb_buffer_string(&terminate_printed), "after\n") == 0 && terminate_console_lines == 1)
		passed = 1;
	snprintf(detail, sizeof(detail), "status %d, printed \"%s\", console %u", status, wb_buffer_string(&terminate_printed),
	    terminate_console_lines);
	terminate_check(passed, "page: the next task's script runs", detail);

	/* The page goes. */
	page_destroy(page);
}
