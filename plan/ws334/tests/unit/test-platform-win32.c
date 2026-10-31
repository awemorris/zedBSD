/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The unit test of libkeiland's Windows platform layer (platform/platform.h):
 * the wake as a manual-reset event object (what open leaves in the
 * structure, ring, clear, ringing from another thread, close), the lock
 * (mutual exclusion and blocking), and the threads (identity, a thread
 * nobody joins, a thread that is joined, the name the debugger shows).  The
 * expectations are the header's and the event, wait and thread-description
 * calls' documented behaviour; the Windows side is read with the system's
 * own calls (WaitForSingleObject, GetHandleInformation,
 * GetThreadDescription), not with the module under test.
 */

#include "check.h"

#include "userland/desktop/libkeiland/platform/platform.h"

#include <windows.h>

#include <errno.h>
#include <stddef.h>
#include <string.h>

/* How long a wait for another thread may take before the test calls it lost, in milliseconds. */
#define WAIT_LIMIT	30000UL

/* How many times one thread rings another awake. */
#define RING_ROUNDS	100U

/* How many times each of two threads takes the lock and counts. */
#define LOCK_ROUNDS	100000L

/* The UTF-16 units of the longest thread name (63 bytes of UTF-8, so at most 63 units). */
#define NAME_UNITS_MAX	63U

/* The system's call that reads a thread's name back, as GetProcAddress gives it. */
typedef HRESULT (WINAPI *describe_get_function)(HANDLE thread, PWSTR *description);

/*
 * What the threads of the wake tests share: the wake, how long a ringing
 * thread waits before it rings, and what a waiting thread saw.
 */
struct wake_job {
	struct keiui_wake *wake;
	DWORD delay;
	DWORD waited;
	HANDLE started;
};

/* What the threads of the lock tests share: the lock and the counter it guards. */
struct lock_job {
	struct keiui_lock *lock;
	long counter;
	volatile LONG started;
	volatile LONG got;
};

/* What the thread tests share: the identities seen from both sides and the events that order them. */
struct identity_job {
	keiui_thread_id parent;
	keiui_thread_id own;
	int own_is_self;
	int parent_is_not_self;
	HANDLE ready;
	HANDLE release;
};

/* What a thread nobody joins is given and leaves behind. */
struct detached_job {
	int seen;
	HANDLE done;
};

/* What the naming thread is given. */
struct name_job {
	describe_get_function get;
};

static void test_open(void);
static void test_ring_clear(void);
static void test_independent(void);
static void test_cross_thread(void);
static void test_many_waiters(void);
static void test_close(void);
static void test_lock(void);
static void test_lock_blocks(void);
static void test_threads(void);
static void test_detached(void);
static void test_refusals(void);
static void test_names(void);
static void *ring_thread(void *argument);
static void *wait_thread(void *argument);
static void *count_thread(void *argument);
static void *take_thread(void *argument);
static void *identity_thread(void *argument);
static void *detached_thread(void *argument);
static void *idle_thread(void *argument);
static void *name_thread(void *argument);
static void name_expect(const struct name_job *job, const char *name, const WCHAR *expected, size_t units);
static DWORD wait_flag(volatile LONG *flag);

int
main(
	int argc,
	char **argv)
{
	/* The harness names the system; this layer is Windows only. */
	CHECK(argc == 2);
	if (argc != 2)
		return check_finish("platform-win32");
	CHECK(strcmp(argv[1], "win32") == 0);
	if (strcmp(argv[1], "win32") != 0)
		return check_finish("platform-win32");

	/* Runs the groups. */
	test_open();
	test_ring_clear();
	test_independent();
	test_cross_thread();
	test_many_waiters();
	test_close();
	test_lock();
	test_lock_blocks();
	test_threads();
	test_detached();
	test_refusals();
	test_names();

	/* Reports the result. */
	return check_finish("platform-win32");
}

/*
 * Opens a wake over a structure holding garbage: the event exists, the
 * descriptors are -1, nothing is rung, and the event is not inherited.
 */
static void
test_open(void)
{
	struct keiui_wake wake;
	HANDLE event;
	DWORD flags;
	DWORD waited;
	BOOL got;
	int error;

	/* Garbage in every field, so that open must set them all. */
	wake.ends[0] = 7;
	wake.ends[1] = 9;
	wake.event = (void *)(size_t)0x1234U;

	/* Opens the wake. */
	error = keiui_wake_open(&wake);
	CHECK_EQ(error, 0);
	CHECK(wake.event != NULL);
	CHECK(wake.event != (void *)(size_t)0x1234U);
	CHECK_EQ(wake.ends[0], -1);
	CHECK_EQ(wake.ends[1], -1);
	CHECK_EQ(keiui_wake_descriptor(&wake), -1);
	CHECK(keiui_wake_event(&wake) == wake.event);

	/* A new wake is quiet. */
	event = (HANDLE)keiui_wake_event(&wake);
	waited = WaitForSingleObject(event, 0);
	CHECK_EQ(waited, WAIT_TIMEOUT);

	/* The handle is valid and a program the application starts does not receive it. */
	flags = 0xFFU;
	got = GetHandleInformation(event, &flags);
	CHECK(got != 0);
	CHECK_EQ(flags & HANDLE_FLAG_INHERIT, 0);

	/* An event object answers to ResetEvent and SetEvent; a handle of another kind would refuse. */
	CHECK(SetEvent(event) != 0);
	CHECK(ResetEvent(event) != 0);

	keiui_wake_close(&wake);
}

/*
 * Rings and clears a wake on one thread: rung it is signaled and stays so
 * (manual reset), cleared it is quiet, and ringing twice is cleared once.
 */
static void
test_ring_clear(void)
{
	struct keiui_wake wake;
	HANDLE event;
	DWORD waited;
	int error;

	error = keiui_wake_open(&wake);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	event = (HANDLE)keiui_wake_event(&wake);

	/* A ring signals the event, and a wait that finds it signaled does not reset it. */
	keiui_wake_ring(&wake);
	waited = WaitForSingleObject(event, 0);
	CHECK_EQ(waited, WAIT_OBJECT_0);
	waited = WaitForSingleObject(event, 0);
	CHECK_EQ(waited, WAIT_OBJECT_0);

	/* A clear makes it quiet. */
	keiui_wake_clear(&wake);
	waited = WaitForSingleObject(event, 0);
	CHECK_EQ(waited, WAIT_TIMEOUT);

	/* Two rings add nothing: one clear leaves it quiet. */
	keiui_wake_ring(&wake);
	keiui_wake_ring(&wake);
	keiui_wake_clear(&wake);
	waited = WaitForSingleObject(event, 0);
	CHECK_EQ(waited, WAIT_TIMEOUT);

	/* Clearing a quiet wake keeps it quiet. */
	keiui_wake_clear(&wake);
	keiui_wake_clear(&wake);
	waited = WaitForSingleObject(event, 0);
	CHECK_EQ(waited, WAIT_TIMEOUT);

	/* A ring after a clear signals again. */
	keiui_wake_ring(&wake);
	waited = WaitForSingleObject(event, 0);
	CHECK_EQ(waited, WAIT_OBJECT_0);

	/* The system's own reset and the wake's clear agree: set by the system, cleared by the wake. */
	CHECK(ResetEvent(event) != 0);
	CHECK(SetEvent(event) != 0);
	keiui_wake_clear(&wake);
	waited = WaitForSingleObject(event, 0);
	CHECK_EQ(waited, WAIT_TIMEOUT);

	/* Rung by the wake, reset by the system. */
	keiui_wake_ring(&wake);
	CHECK(ResetEvent(event) != 0);
	waited = WaitForSingleObject(event, 0);
	CHECK_EQ(waited, WAIT_TIMEOUT);

	keiui_wake_close(&wake);
}

/*
 * Two wakes are two events: ringing one leaves the other quiet.
 */
static void
test_independent(void)
{
	struct keiui_wake first;
	struct keiui_wake second;
	DWORD waited;
	int error;

	error = keiui_wake_open(&first);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	error = keiui_wake_open(&second);
	CHECK_EQ(error, 0);
	if (error != 0) {
		keiui_wake_close(&first);
		return;
	}

	/* Different objects. */
	CHECK(first.event != second.event);

	/* A ring of the first leaves the second quiet. */
	keiui_wake_ring(&first);
	waited = WaitForSingleObject((HANDLE)second.event, 0);
	CHECK_EQ(waited, WAIT_TIMEOUT);
	waited = WaitForSingleObject((HANDLE)first.event, 0);
	CHECK_EQ(waited, WAIT_OBJECT_0);

	/* A clear of the second leaves the first rung. */
	keiui_wake_clear(&second);
	waited = WaitForSingleObject((HANDLE)first.event, 0);
	CHECK_EQ(waited, WAIT_OBJECT_0);

	keiui_wake_close(&first);
	keiui_wake_close(&second);
}

/*
 * A thread rings the wake another thread waits on; the wait returns, one
 * hundred times, with the ring both before and after the wait begins.
 */
static void
test_cross_thread(void)
{
	struct keiui_wake wake;
	struct wake_job job;
	keiui_thread_id thread;
	unsigned round;
	DWORD waited;
	int error;

	error = keiui_wake_open(&wake);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	job.wake = &wake;
	job.waited = 0;
	job.started = NULL;

	/* Each round rings from a new thread, now or a moment later. */
	for (round = 0; round < RING_ROUNDS; round++) {
		keiui_wake_clear(&wake);
		job.delay = (DWORD)(round % 3U);
		error = keiui_thread_start_joined(ring_thread, &job, &thread);
		CHECK_EQ(error, 0);
		if (error != 0)
			break;

		waited = WaitForSingleObject((HANDLE)wake.event, WAIT_LIMIT);
		CHECK_EQ(waited, WAIT_OBJECT_0);
		keiui_thread_join(thread);
	}

	keiui_wake_close(&wake);
}

/*
 * One ring releases every thread that waits: two threads wait, the wake is
 * rung once, and both return.
 */
static void
test_many_waiters(void)
{
	struct keiui_wake wake;
	struct wake_job first;
	struct wake_job second;
	keiui_thread_id first_thread;
	keiui_thread_id second_thread;
	int error;

	error = keiui_wake_open(&wake);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;

	/* The two waiters, each telling when it began. */
	first.wake = &wake;
	first.waited = 0xFFFFU;
	first.started = CreateEventW(NULL, TRUE, FALSE, NULL);
	second.wake = &wake;
	second.waited = 0xFFFFU;
	second.started = CreateEventW(NULL, TRUE, FALSE, NULL);
	CHECK(first.started != NULL);
	CHECK(second.started != NULL);
	if (first.started == NULL || second.started == NULL) {
		keiui_wake_close(&wake);
		return;
	}

	error = keiui_thread_start_joined(wait_thread, &first, &first_thread);
	CHECK_EQ(error, 0);
	error = keiui_thread_start_joined(wait_thread, &second, &second_thread);
	CHECK_EQ(error, 0);

	/* Both are about to wait; a moment later the ring. */
	CHECK_EQ(WaitForSingleObject(first.started, WAIT_LIMIT), WAIT_OBJECT_0);
	CHECK_EQ(WaitForSingleObject(second.started, WAIT_LIMIT), WAIT_OBJECT_0);
	Sleep(50);
	keiui_wake_ring(&wake);

	keiui_thread_join(first_thread);
	keiui_thread_join(second_thread);
	CHECK_EQ(first.waited, WAIT_OBJECT_0);
	CHECK_EQ(second.waited, WAIT_OBJECT_0);

	CloseHandle(first.started);
	CloseHandle(second.started);
	keiui_wake_close(&wake);
}

/*
 * Closes a wake: the handle is gone, the structure says so, a second close
 * does nothing, and a ring or clear of the closed wake does not fail.
 */
static void
test_close(void)
{
	struct keiui_wake wake;
	HANDLE event;
	DWORD flags;
	BOOL got;
	int error;

	error = keiui_wake_open(&wake);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	event = (HANDLE)wake.event;

	/* The handle is valid before the close. */
	got = GetHandleInformation(event, &flags);
	CHECK(got != 0);

	/* It is invalid after. */
	keiui_wake_close(&wake);
	got = GetHandleInformation(event, &flags);
	CHECK(got == 0);
	CHECK(wake.event == NULL);
	CHECK(keiui_wake_event(&wake) == NULL);
	CHECK_EQ(wake.ends[0], -1);
	CHECK_EQ(wake.ends[1], -1);
	CHECK_EQ(keiui_wake_descriptor(&wake), -1);

	/* A closed wake takes a second close, a ring and a clear without effect. */
	keiui_wake_close(&wake);
	keiui_wake_ring(&wake);
	keiui_wake_clear(&wake);
	CHECK(wake.event == NULL);

	/* The same structure opens again. */
	error = keiui_wake_open(&wake);
	CHECK_EQ(error, 0);
	CHECK(wake.event != NULL);
	keiui_wake_close(&wake);
}

/*
 * Takes and gives a lock; two threads count a shared number up under it and
 * the total is exact; a lock opens and closes more than once.
 */
static void
test_lock(void)
{
	struct keiui_lock lock;
	struct lock_job job;
	keiui_thread_id first;
	keiui_thread_id second;
	unsigned cycle;
	int error;

	/* A lock that opens, is taken and given, and closes, several times. */
	for (cycle = 0; cycle < 3U; cycle++) {
		error = keiui_lock_open(&lock);
		CHECK_EQ(error, 0);
		if (error != 0)
			return;
		keiui_lock_take(&lock);
		keiui_lock_give(&lock);
		keiui_lock_take(&lock);
		keiui_lock_give(&lock);
		keiui_lock_close(&lock);
	}

	/* Two counting threads. */
	error = keiui_lock_open(&lock);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	job.lock = &lock;
	job.counter = 0;
	job.started = 0;
	job.got = 0;
	error = keiui_thread_start_joined(count_thread, &job, &first);
	CHECK_EQ(error, 0);
	if (error != 0) {
		keiui_lock_close(&lock);
		return;
	}
	error = keiui_thread_start_joined(count_thread, &job, &second);
	CHECK_EQ(error, 0);
	if (error != 0) {
		keiui_thread_join(first);
		keiui_lock_close(&lock);
		return;
	}
	keiui_thread_join(first);
	keiui_thread_join(second);

	/* Every increment of both counted. */
	CHECK_EQ(job.counter, 2L * LOCK_ROUNDS);

	keiui_lock_close(&lock);
}

/*
 * A lock that is held makes another thread wait, and the thread goes on
 * when the lock is given.
 */
static void
test_lock_blocks(void)
{
	struct keiui_lock lock;
	struct lock_job job;
	keiui_thread_id thread;
	DWORD result;
	int error;

	error = keiui_lock_open(&lock);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	job.lock = &lock;
	job.counter = 0;
	job.started = 0;
	job.got = 0;

	/* This thread holds the lock while the other tries to take it. */
	keiui_lock_take(&lock);
	error = keiui_thread_start_joined(take_thread, &job, &thread);
	CHECK_EQ(error, 0);
	if (error != 0) {
		keiui_lock_give(&lock);
		keiui_lock_close(&lock);
		return;
	}
	result = wait_flag(&job.started);
	CHECK_EQ(result, WAIT_OBJECT_0);
	Sleep(200);
	CHECK_EQ(job.got, 0);

	/* Given, the other thread gets it. */
	keiui_lock_give(&lock);
	result = wait_flag(&job.got);
	CHECK_EQ(result, WAIT_OBJECT_0);
	keiui_thread_join(thread);

	/* The lock is free again for this thread. */
	keiui_lock_take(&lock);
	keiui_lock_give(&lock);
	keiui_lock_close(&lock);
}

/*
 * Identities: a thread knows itself and not the thread that started it, the
 * starter knows itself and not the new thread, and the identity start
 * returns is the one the thread sees as its own.
 */
static void
test_threads(void)
{
	struct identity_job job;
	keiui_thread_id thread;
	keiui_thread_id self;
	DWORD waited;
	int error;

	/* The calling thread is itself. */
	self = keiui_thread_self();
	CHECK_EQ(keiui_thread_is(self), 1);
	job.parent = self;
	job.own = 0;
	job.own_is_self = 0;
	job.parent_is_not_self = 0;
	job.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
	job.release = CreateEventW(NULL, TRUE, FALSE, NULL);
	CHECK(job.ready != NULL);
	CHECK(job.release != NULL);
	if (job.ready == NULL || job.release == NULL)
		return;

	error = keiui_thread_start_joined(identity_thread, &job, &thread);
	CHECK_EQ(error, 0);
	if (error != 0) {
		CloseHandle(job.ready);
		CloseHandle(job.release);
		return;
	}

	/* While the thread lives, this thread is not it and it is not this thread. */
	waited = WaitForSingleObject(job.ready, WAIT_LIMIT);
	CHECK_EQ(waited, WAIT_OBJECT_0);
	CHECK_EQ(keiui_thread_is(job.own), 0);
	CHECK_EQ(keiui_thread_is(thread), 0);
	CHECK_EQ(keiui_thread_is(self), 1);
	CHECK(job.own != self);
	SetEvent(job.release);
	keiui_thread_join(thread);

	/* What the thread saw. */
	CHECK_EQ(job.own_is_self, 1);
	CHECK_EQ(job.parent_is_not_self, 1);
	CHECK(job.own == thread);

	CloseHandle(job.ready);
	CloseHandle(job.release);
}

/*
 * A thread nobody joins runs, is given its argument and leaves its mark.
 */
static void
test_detached(void)
{
	struct detached_job job;
	DWORD waited;
	int error;

	job.seen = 0;
	job.done = CreateEventW(NULL, TRUE, FALSE, NULL);
	CHECK(job.done != NULL);
	if (job.done == NULL)
		return;

	error = keiui_thread_start(detached_thread, &job);
	CHECK_EQ(error, 0);
	if (error != 0) {
		CloseHandle(job.done);
		return;
	}

	/* The thread's mark; the done event orders the write of seen before this read. */
	waited = WaitForSingleObject(job.done, WAIT_LIMIT);
	CHECK_EQ(waited, WAIT_OBJECT_0);
	CHECK_EQ(job.seen, 42);

	CloseHandle(job.done);
}

/*
 * A start without a function to run, or a joined start without a place for
 * the identity, is refused and starts nothing.
 */
static void
test_refusals(void)
{
	keiui_thread_id thread;
	int error;

	error = keiui_thread_start(NULL, NULL);
	CHECK_EQ(error, EINVAL);

	thread = 0;
	error = keiui_thread_start_joined(NULL, NULL, &thread);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(thread, 0);

	error = keiui_thread_start_joined(idle_thread, NULL, NULL);
	CHECK_EQ(error, EINVAL);
}

/*
 * Thread names: a missing name, a long name, a name with characters of
 * several widths, a name cut inside a character and a name that is not
 * UTF-8 do not fail, and the name the debugger reads is the UTF-16 of what
 * was given (up to the limit), when the system can read it.
 */
static void
test_names(void)
{
	struct name_job job;
	keiui_thread_id thread;
	HMODULE kernel;
	FARPROC found;
	int error;

	/* The system's call that reads a name back, when this Windows has it. */
	kernel = GetModuleHandleW(L"kernel32.dll");
	CHECK(kernel != NULL);
	job.get = NULL;
	if (kernel != NULL) {
		found = GetProcAddress(kernel, "GetThreadDescription");
		if (found != NULL)
			job.get = (describe_get_function)(void *)found;
	}

	/* A thread of its own, so that the names do not stay on the test's main thread. */
	error = keiui_thread_start_joined(name_thread, &job, &thread);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	keiui_thread_join(thread);
}

/*
 * Rings a wake from a thread after the delay it is given.
 */
static void *
ring_thread(
	void *argument)
{
	struct wake_job *job;

	job = (struct wake_job *)argument;
	if (job->delay != 0)
		Sleep(job->delay);
	keiui_wake_ring(job->wake);

	return NULL;
}

/*
 * Waits for a wake's event, telling first that it is about to.
 */
static void *
wait_thread(
	void *argument)
{
	struct wake_job *job;

	job = (struct wake_job *)argument;
	SetEvent(job->started);
	job->waited = WaitForSingleObject((HANDLE)keiui_wake_event(job->wake), WAIT_LIMIT);

	return NULL;
}

/*
 * Counts up under the lock, one hundred thousand times.
 */
static void *
count_thread(
	void *argument)
{
	struct lock_job *job;
	long round;

	job = (struct lock_job *)argument;
	for (round = 0; round < LOCK_ROUNDS; round++) {
		keiui_lock_take(job->lock);
		job->counter = job->counter + 1;
		keiui_lock_give(job->lock);
	}

	return NULL;
}

/*
 * Takes the lock, telling first that it is about to and then that it has.
 */
static void *
take_thread(
	void *argument)
{
	struct lock_job *job;

	job = (struct lock_job *)argument;
	InterlockedExchange(&job->started, 1);
	keiui_lock_take(job->lock);
	InterlockedExchange(&job->got, 1);
	keiui_lock_give(job->lock);

	return NULL;
}

/*
 * Records what the thread sees of identities, then waits to be released so
 * that its parent can look while it lives.
 */
static void *
identity_thread(
	void *argument)
{
	struct identity_job *job;

	job = (struct identity_job *)argument;
	job->own = keiui_thread_self();
	job->own_is_self = keiui_thread_is(job->own);
	if (keiui_thread_is(job->parent) == 0)
		job->parent_is_not_self = 1;
	SetEvent(job->ready);
	WaitForSingleObject(job->release, WAIT_LIMIT);

	return NULL;
}

/*
 * Leaves its mark for the thread that started it and tells it is done.
 */
static void *
detached_thread(
	void *argument)
{
	struct detached_job *job;

	job = (struct detached_job *)argument;
	job->seen = 42;
	SetEvent(job->done);

	return NULL;
}

/*
 * Does nothing; a thread the refusal tests would start if they were wrong.
 */
static void *
idle_thread(
	void *argument)
{
	(void)argument;

	return NULL;
}

/*
 * Names itself with each kind of name and reads each back.
 */
static void *
name_thread(
	void *argument)
{
	const struct name_job *job;
	char longer[200];
	char boundary[200];
	char cut[200];
	WCHAR expected[NAME_UNITS_MAX + 1U];
	unsigned index;

	job = (const struct name_job *)argument;

	/* A short name is the UTF-16 of itself. */
	name_expect(job, "keiland-test", L"keiland-test", 12U);

	/* A name of characters of one, two and three bytes. */
	name_expect(job, "a\303\251\343\201\202", L"aéあ", 3U);

	/* A name of 63 bytes is whole. */
	for (index = 0; index < 63U; index++) {
		boundary[index] = 'b';
		expected[index] = L'b';
	}
	boundary[63] = '\0';
	expected[63] = L'\0';
	name_expect(job, boundary, expected, 63U);

	/* A name of 200 bytes is cut to 63. */
	for (index = 0; index < 199U; index++)
		longer[index] = 'c';
	longer[199] = '\0';
	for (index = 0; index < 63U; index++)
		expected[index] = L'c';
	name_expect(job, longer, expected, 63U);

	/* A name that would be cut inside a character is cut before it: "a" and thirty three-byte characters. */
	cut[0] = 'a';
	for (index = 0; index < 30U; index++) {
		cut[1U + index * 3U] = (char)0xE3;
		cut[2U + index * 3U] = (char)0x81;
		cut[3U + index * 3U] = (char)0x82;
	}
	cut[91] = '\0';
	expected[0] = L'a';
	for (index = 0; index < 20U; index++)
		expected[1U + index] = L'あ';
	name_expect(job, cut, expected, 21U);

	/* A cut inside a two-byte character: 62 ASCII bytes and then two-byte characters. */
	for (index = 0; index < 62U; index++)
		cut[index] = 'd';
	for (index = 0; index < 5U; index++) {
		cut[62U + index * 2U] = (char)0xC3;
		cut[63U + index * 2U] = (char)0xA9;
	}
	cut[72] = '\0';
	for (index = 0; index < 62U; index++)
		expected[index] = L'd';
	name_expect(job, cut, expected, 62U);

	/* A name that is not UTF-8 changes nothing: the previous name stays. */
	name_expect(job, "\377\376", L"d", 0U);

	/* An empty name is an empty name. */
	name_expect(job, "", L"", 0U);

	/* A missing name is ignored. */
	keiui_thread_name(NULL);
	name_expect(job, "after-null", L"after-null", 10U);

	return NULL;
}

/*
 * Names the calling thread and, when the system reads names, checks that
 * what it reads is the units expected.  An expected name of 0 units for a
 * name that is not valid UTF-8 means that the name did not change, so the
 * caller passes the previous one as the expectation; the units count is
 * the length of the expected string except in that case.
 */
static void
name_expect(
	const struct name_job *job,
	const char *name,
	const WCHAR *expected,
	size_t units)
{
	static WCHAR previous[NAME_UNITS_MAX + 1U];
	PWSTR read;
	HRESULT status;
	size_t length;

	/* The name, whatever it is, must not fail. */
	keiui_thread_name(name);

	/* Without a way to read it back there is nothing more to see. */
	if (job->get == NULL)
		return;

	status = job->get(GetCurrentThread(), &read);
	CHECK(SUCCEEDED(status));
	if (!SUCCEEDED(status))
		return;

	/* A name that is not UTF-8 leaves the one before; any other replaces it. */
	if (units == 0U && name[0] != '\0') {
		CHECK(wcscmp(read, previous) == 0);
	} else {
		length = wcslen(read);
		CHECK_EQ(length, units);
		CHECK(wcsncmp(read, expected, units) == 0);
		wcsncpy(previous, read, NAME_UNITS_MAX);
		previous[NAME_UNITS_MAX] = L'\0';
	}
	LocalFree(read);
}

/*
 * Waits until a flag another thread sets becomes 1, for at most the limit.
 */
static DWORD
wait_flag(
	volatile LONG *flag)
{
	DWORD waited;
	LONG value;

	/* Polls every millisecond. */
	for (waited = 0; waited < WAIT_LIMIT; waited++) {
		value = InterlockedCompareExchange(flag, 0, 0);
		if (value != 0)
			return WAIT_OBJECT_0;
		Sleep(1);
	}

	return WAIT_TIMEOUT;
}
