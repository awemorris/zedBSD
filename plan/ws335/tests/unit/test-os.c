/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the operating-system layer of cryc-os.h on POSIX and, built for
 * Windows, on Windows (under Wine).
 *
 * The random source is checked at every length around the 256-byte chunk,
 * with four different fill patterns (a byte the call did not write keeps
 * its fill, and a written byte equals all four fills with probability
 * 2^-32), with guard bytes around the buffer and with a byte histogram.
 * The clock is compared with time(NULL).  The mutex, the atomic addition
 * and the per-thread value run on several threads: the atomic results must
 * be exactly the numbers 1..N once each, the mutex must keep two threads
 * out of its critical section, and the destroy function must run once per
 * thread that stored a value, with the last value stored.  Files are read
 * at sizes around the buffer growth, with every byte value, at the limit
 * and one past it, with names in UTF-8.  Sockets are checked in pairs
 * (socketpair, or a loopback TCP connection on Windows): reads and
 * writes, partial transfers, the end of the stream, a non-blocking socket,
 * a closed peer and a bad descriptor; on POSIX also a pipe and an
 * interrupted call (a signal that arrives while the call blocks).  The
 * trust sources are checked with recording callbacks; on Windows with
 * certificates this test adds to and removes from the current user's
 * stores.
 */

#include "check.h"
#include "derwriter.h"

#include "userland/base/libcrypto-compat/cryc-os.h"

#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <wincrypt.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

/* The byte of the guard that surrounds a buffer a function must not overrun. */
#define GUARD_BYTE	0x5aU

/* The room of the directory of the test's files. */
#define DIRECTORY_ROOM	1024U

/* The threads of the multi-thread tests. */
#define THREAD_COUNT	4U

/* The additions each thread makes in the atomic test. */
#define ATOMIC_EACH	20000U

/* The enters of the critical section each thread makes in the mutex test. */
#define MUTEX_EACH	50000U

/* The bytes moved through a socket pair in the transfer test. */
#define TRANSFER_SIZE	(1024UL * 1024UL + 17UL)

#ifdef _WIN32
typedef HANDLE test_thread;
#define THREAD_RETURN	DWORD
#define THREAD_CALL	WINAPI
#define THREAD_RESULT	0
#else
typedef pthread_t test_thread;
#define THREAD_RETURN	void *
#define THREAD_CALL
#define THREAD_RESULT	NULL
#endif

#ifdef _WIN32
/* The test's own certificates (see make_certificate). */
#define CERT_DISALLOWED	0
#define CERT_PLAIN	1
#define CERT_CLIENT_ONLY	2
#define CERT_EMPTY_USAGE	3
#define CERT_BOTH_USAGES	4
#define CERT_EXTENSION_ONLY	5
#define CERT_COUNT	6
#endif

/* The fills a byte the random call did not write would keep. */
static const unsigned char fills[4] = { 0x00U, 0xffU, 0x5aU, 0xa5U };

/* The directory the test's files go in: the directory of the test program (a fresh one per run). */
static char test_dir[DIRECTORY_ROOM];

/* The state of the pseudo-random generator that makes file contents (xorshift32, Marsaglia 2003). */
static uint32_t rng_state = 0x2545f491U;

/* The shared state of the atomic test, and of the thread-value test. */
static volatile long atomic_total;

/* Which thread-test numbers were returned by an addition (index = value). */
static unsigned char *atomic_seen;

/* Threads of the mutex and atomic tests that have started, so that all of them run at the same time. */
static volatile long gate_ready;

/* The shared state of the mutex test. */
static struct cryc_os_mutex shared_mutex;
static volatile long shared_counter;
static volatile long shared_inside;
static volatile long shared_violations;

/* The destroy function of the thread-value test counts its calls here, and notes the last value. */
static volatile long destroy_calls;
static void *volatile destroy_last;

/* Threads that have reached the barrier of the thread-value test. */
static volatile long barrier_arrived;

#ifndef _WIN32
/* The state of a handler that runs while a socket call blocks: the ticks it has counted and the descriptors it acts on. */
static volatile sig_atomic_t signal_ticks;
static volatile int signal_read_fd;
static volatile int signal_write_fd;
#endif

#ifdef _WIN32
/* The DER of each test certificate and its length. */
static unsigned char cert_der[CERT_COUNT][700];
static size_t cert_size[CERT_COUNT];
#endif

/* One thread's work and result. */
struct worker {
	test_thread thread;
	int started;
	unsigned number;
	long failures;
	void *value;
};

/* The calls one run of cryc_os_system_roots made, as the callbacks see them. */
struct roots_log {
	int distrusted;
	int ders;
	int files;
	int order_broken;
	int last_kind;
	int stop_kind;
	int stop_error;
	int file_error;
	char file_path[1100];
	int hits[8];
	int hit_kind[8];
	const unsigned char *known[8];
	size_t known_size[8];
};

static uint32_t next_random(void);
static void fill_random_bytes(unsigned char *bytes, size_t size);
static void pause_milliseconds(unsigned milliseconds);
static void yield_thread(void);
static void wait_at_gate(void);
static int thread_start(test_thread *thread, THREAD_RETURN (THREAD_CALL *function)(void *), void *argument);
static void thread_join(test_thread thread);
static void make_path(char *out, size_t room, const char *name);
static int write_file(const char *path, const unsigned char *bytes, size_t size);
static int write_sparse_file(const char *path, size_t size, unsigned char last);
static void check_random_size(size_t size);
static void test_random(void);
static void test_time(void);
static THREAD_RETURN THREAD_CALL mutex_worker(void *argument);
static void test_mutex(void);
static THREAD_RETURN THREAD_CALL atomic_worker(void *argument);
static void test_atomic(void);
static void test_destroy(void *value);
static THREAD_RETURN THREAD_CALL value_worker_replace(void *argument);
static THREAD_RETURN THREAD_CALL value_worker_none(void *argument);
static THREAD_RETURN THREAD_CALL value_worker_barrier(void *argument);
static void test_thread_value(void);
static unsigned char pattern_byte(size_t index);
static void check_file_size(size_t size);
static void test_read_file(void);
static int make_pair(intptr_t *first, intptr_t *second);
static void set_nonblocking(intptr_t descriptor, int nonblocking);
static void close_descriptor(intptr_t descriptor);
static int read_settled(intptr_t descriptor, uint8_t *buffer, size_t size, size_t *done, int *os_error);
static void check_socket_basic(void);
static void check_socket_transfer(void);
static void check_socket_errors(void);
#ifndef _WIN32
static void alarm_handler(int number);
static void set_timer(int on);
static void check_socket_interrupted(void);
static void check_socket_pipe(void);
#endif
static void note_kind(struct roots_log *log, int kind);
static void note_certificate(struct roots_log *log, int kind, const uint8_t *der, size_t size);
static int log_distrusted(void *context, const uint8_t *der, size_t size);
static int log_der(void *context, const uint8_t *der, size_t size);
static int log_file(void *context, const char *path);
static void roots_log_clear(struct roots_log *log);
#ifdef _WIN32
static void write_name(struct der_writer *writer, const char *common_name);
static void write_algorithm(struct der_writer *writer, const char *oid);
static size_t make_certificate(unsigned char *out, size_t room, unsigned serial, const char *common_name, int client_only_extension);
static HCERTSTORE open_store(const WCHAR *name);
static void purge_store(const WCHAR *key_path);
static int add_certificate(HCERTSTORE store, int index, const char **usages, DWORD usage_count, int with_property);
static int expected_certificate_path(char *out, size_t room);
static void check_roots_windows(void);
#endif
static void test_system_roots(void);
static void find_test_directory(const char *program);

/* The next pseudo-random 32-bit number. */
static uint32_t
next_random(void)
{
	/* Advances the generator by one xorshift round. */
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;

	/* The new state is the number. */
	return rng_state;
}

/* Fills bytes with pseudo-random data for a file's contents. */
static void
fill_random_bytes(
	unsigned char *bytes,
	size_t size)
{
	size_t index;

	/* Draws each byte from the high bits of the generator. */
	for (index = 0; index < size; index++)
		bytes[index] = (unsigned char)(next_random() >> 11);
}

/* Sleeps for a number of milliseconds. */
static void
pause_milliseconds(
	unsigned milliseconds)
{
#ifdef _WIN32
	Sleep(milliseconds);
#else
	struct timespec request;

	/* Splits the wait into seconds and nanoseconds and sleeps. */
	request.tv_sec = (time_t)(milliseconds / 1000U);
	request.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
	nanosleep(&request, NULL);
#endif
}

/* Lets another thread run. */
static void
yield_thread(void)
{
#ifdef _WIN32
	SwitchToThread();
#else
	sched_yield();
#endif
}

/* Waits until every thread of the batch has started, so that none finishes before the next one begins. */
static void
wait_at_gate(void)
{
	unsigned spins;

	/* Counts this thread as started, then yields until every thread of the batch has started. */
	cryc_os_atomic_add(&gate_ready, 1);
	for (spins = 0; spins < 20000000U && gate_ready < (long)THREAD_COUNT; spins++)
		yield_thread();
}

/* Starts a thread; returns 1 when it started. */
static int
thread_start(
	test_thread *thread,
	THREAD_RETURN (THREAD_CALL *function)(void *),
	void *argument)
{
#ifndef _WIN32
	int error;
#endif

#ifdef _WIN32
	/* Starts the thread. */
	*thread = CreateThread(NULL, 0, function, argument, 0, NULL);
	if (*thread == NULL)
		return 0;
#else
	/* Starts the thread. */
	error = pthread_create(thread, NULL, function, argument);
	if (error != 0)
		return 0;
#endif

	/* Succeeded: the thread is running. */
	return 1;
}

/* Waits for a thread to end. */
static void
thread_join(
	test_thread thread)
{
#ifdef _WIN32
	WaitForSingleObject(thread, INFINITE);
	CloseHandle(thread);
#else
	pthread_join(thread, NULL);
#endif
}

/* Joins a directory and a file name with a slash (both systems accept it). */
static void
make_path(
	char *out,
	size_t room,
	const char *name)
{
	size_t length;
	size_t name_length;

	/* Measures the directory and the name. */
	length = strlen(test_dir);
	name_length = strlen(name);

	/* Leaves an empty string when the path does not fit. */
	if (length + 1U + name_length + 1U > room) {
		out[0] = 0;
		return;
	}

	/* Joins the directory, the slash and the name. */
	memcpy(out, test_dir, length);
	out[length] = '/';
	memcpy(out + length + 1U, name, name_length + 1U);
}

/* Writes a file in binary mode (the path is UTF-8); returns 1 on success. */
static int
write_file(
	const char *path,
	const unsigned char *bytes,
	size_t size)
{
	FILE *file;
	size_t put;
	int closed;
#ifdef _WIN32
	WCHAR wide[1100];
	int converted;
#endif

#ifdef _WIN32
	/* Converts the UTF-8 path to UTF-16 and opens the file. */
	converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 1100);
	if (converted <= 0)
		return 0;
	file = _wfopen(wide, L"wb");
#else
	/* Opens the file. */
	file = fopen(path, "wb");
#endif
	if (file == NULL)
		return 0;

	/* Writes the bytes; an empty file writes nothing. */
	put = 0;
	if (size != 0U)
		put = fwrite(bytes, 1, size, file);

	/* Closes the file, which fails when the data could not be flushed. */
	closed = fclose(file);
	if (closed != 0)
		return 0;

	/* Reports a short write. */
	if (put != size)
		return 0;

	/* Succeeded: the whole content is in the file. */
	return 1;
}

/* Writes a file of size bytes, all zero except the last, without writing the middle on POSIX. */
static int
write_sparse_file(
	const char *path,
	size_t size,
	unsigned char last)
{
	FILE *file;
	size_t written;
	int good;
	int seek_error;
	int closed;
#ifdef _WIN32
	WCHAR wide[1100];
	int converted;
#endif

#ifdef _WIN32
	/* Converts the UTF-8 path to UTF-16 and opens the file. */
	converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 1100);
	if (converted <= 0)
		return 0;
	file = _wfopen(wide, L"wb");
#else
	/* Opens the file. */
	file = fopen(path, "wb");
#endif
	if (file == NULL)
		return 0;

	/* Seeks to the last byte and writes it, which leaves the middle a hole. */
	good = 0;
	seek_error = fseek(file, (long)(size - 1U), SEEK_SET);
	if (seek_error == 0) {
		written = fwrite(&last, 1, 1, file);
		if (written == 1U)
			good = 1;
	}

	/* Closes the file; a close that fails makes the file bad. */
	closed = fclose(file);
	if (closed != 0)
		good = 0;

	/* Reports whether the file was written. */
	return good;
}

/*
 * ---- random ----
 */

/* Draws size bytes into four buffers that start with the four fills; every byte must differ from at least one fill. */
static void
check_random_size(
	size_t size)
{
	unsigned char *buffers[4];
	unsigned char *guarded;
	size_t index;
	size_t which;
	size_t before;
	size_t after;
	size_t room;
	int error;
	int untouched;

	/* Each buffer is exactly size bytes (at least one), so that AddressSanitizer sees an overrun. */
	room = size;
	if (size == 0U)
		room = 1U;
	for (which = 0; which < 4U; which++) {
		buffers[which] = (unsigned char *)malloc(room);
		memset(buffers[which], fills[which], room);
		error = cryc_os_random(NULL, buffers[which], size);
		CHECK(error == 0);
	}

	/* A byte equal to every fill was never written (probability of a written one: 2^-32). */
	untouched = 0;
	for (index = 0; index < size; index++) {
		if (buffers[0][index] == fills[0] && buffers[1][index] == fills[1] && buffers[2][index] == fills[2] &&
		    buffers[3][index] == fills[3])
			untouched++;
	}

	/* Reports that every byte was written. */
	CHECK(untouched == 0);

	/* A buffer of size 0 is left as it was. */
	if (size == 0U)
		CHECK(buffers[0][0] == fills[0]);

	/* Gives the buffers back. */
	for (which = 0; which < 4U; which++)
		free(buffers[which]);

	/* Guard bytes around the buffer stay as they were. */
	guarded = (unsigned char *)malloc(size + 64U);
	memset(guarded, GUARD_BYTE, size + 64U);
	error = cryc_os_random(NULL, guarded + 32U, size);
	CHECK(error == 0);

	/* Counts the guard bytes that survived on each side. */
	before = 0;
	after = 0;
	for (index = 0; index < 32U; index++) {
		if (guarded[index] == GUARD_BYTE)
			before++;
		if (guarded[32U + size + index] == GUARD_BYTE)
			after++;
	}

	/* Reports that every guard byte on both sides survived. */
	CHECK(before == 32U);
	CHECK(after == 32U);

	/* Gives the guarded buffer back. */
	free(guarded);
}

/* Checks cryc_os_random at the lengths around its chunk and beyond. */
static void
test_random(void)
{
	static const size_t sizes[] = {
		0, 1, 2, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 254, 255, 256, 257, 258, 300, 511, 512, 513,
		1000, 1023, 1024, 1025, 4096, 4097, 100000
	};
	cryc_random_fn function;
	unsigned char first[32];
	unsigned char second[32];
	unsigned char *big;
	unsigned long histogram[256];
	unsigned long ones;
	size_t index;
	size_t size;
	unsigned char scratch[4];
	int dummy;
	int error;

	/* The sizes. */
	for (index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index++)
		check_random_size(sizes[index]);

	/* The function has the type of a random source and ignores its context. */
	function = cryc_os_random;
	error = function(&dummy, scratch, sizeof(scratch));
	CHECK(error == 0);

	/* Size 0 with no buffer at all is fine; a missing buffer for real bytes is refused. */
	error = cryc_os_random(NULL, NULL, 0);
	CHECK(error == 0);
	error = cryc_os_random(NULL, NULL, 1);
	CHECK(error == EINVAL);

	/* Two draws differ (probability of a repeat 2^-256). */
	CHECK(cryc_os_random(NULL, first, sizeof(first)) == 0);
	CHECK(cryc_os_random(NULL, second, sizeof(second)) == 0);
	CHECK(memcmp(first, second, sizeof(first)) != 0);

	/* A large draw: every byte value occurs a plausible number of times, and about half the bits are set. */
	size = 1048576U + 3U;
	big = (unsigned char *)malloc(size);
	CHECK(cryc_os_random(NULL, big, size) == 0);

	/* Counts how often each byte value occurs and how many bits are set. */
	memset(histogram, 0, sizeof(histogram));
	ones = 0;
	for (index = 0; index < size; index++) {
		histogram[big[index]]++;
		ones += (unsigned long)((big[index] & 1U) + ((big[index] >> 1) & 1U) + ((big[index] >> 2) & 1U) +
		    ((big[index] >> 3) & 1U) + ((big[index] >> 4) & 1U) + ((big[index] >> 5) & 1U) +
		    ((big[index] >> 6) & 1U) + ((big[index] >> 7) & 1U));
	}

	/* Checks the count of every byte value. */
	for (index = 0; index < 256U; index++) {
		/* The mean is 4096 with a standard deviation of 64: 3500 to 4700 is more than nine of them. */
		CHECK(histogram[index] > 3500UL);
		CHECK(histogram[index] < 4700UL);
	}

	/* Reports that about half of the bits are set. */
	CHECK(ones > size * 4U - 5000U);
	CHECK(ones < size * 4U + 5000U);

	/* Gives the large draw back. */
	free(big);
}

/*
 * ---- time ----
 */

/* Checks cryc_os_time against the C library's clock. */
static void
test_time(void)
{
	int64_t first;
	int64_t second;
	int64_t reference;

	/* The type is 64 bits wide. */
	CHECK(sizeof(first) == 8U);

	/* A time after November 2023 and before 2100, within a few seconds of time(NULL). */
	first = cryc_os_time();
	reference = (int64_t)time(NULL);
	CHECK(first > 1700000000);
	CHECK(first < (int64_t)CRYC_U64(0, 0xf4865700U));
	CHECK(first >= reference - 3);
	CHECK(first <= reference + 3);

	/* Later reads do not go back by more than a second. */
	pause_milliseconds(1100);
	second = cryc_os_time();
	CHECK(second >= first + 1);
	CHECK(second <= first + 4);
}

/*
 * ---- mutex and atomic addition ----
 */

/* One thread of the mutex test: enters the critical section many times, checking nobody else is inside. */
static THREAD_RETURN THREAD_CALL
mutex_worker(
	void *argument)
{
	struct worker *worker;
	unsigned index;

	/* Takes the worker record and waits until every thread is running. */
	worker = (struct worker *)argument;
	wait_at_gate();

	/* Enters the critical section many times, noting any other thread found inside it. */
	for (index = 0; index < MUTEX_EACH; index++) {
		cryc_os_mutex_lock(&shared_mutex);

		if (shared_inside != 0)
			shared_violations++;
		shared_inside = 1;
		shared_counter = shared_counter + 1;
		shared_inside = 0;

		cryc_os_mutex_unlock(&shared_mutex);
	}

	/* The checks are made by the main thread, from the shared counters. */
	worker->failures = 0;

	/* Succeeded. */
	return THREAD_RESULT;
}

/* Checks the mutex alone and with several threads. */
static void
test_mutex(void)
{
	struct cryc_os_mutex mutex;
	struct worker workers[THREAD_COUNT];
	unsigned index;
	int error;

	/* A missing structure is refused; destroying and locking a zero structure does nothing. */
	CHECK(cryc_os_mutex_init(NULL) == EINVAL);
	memset(&mutex, 0, sizeof(mutex));
	cryc_os_mutex_lock(&mutex);
	cryc_os_mutex_unlock(&mutex);
	cryc_os_mutex_destroy(&mutex);
	CHECK(mutex.impl == NULL);
	cryc_os_mutex_destroy(NULL);

	/* Init, lock, unlock, destroy; destroy twice; init again. */
	error = cryc_os_mutex_init(&mutex);
	CHECK(error == 0);
	CHECK(mutex.impl != NULL);
	cryc_os_mutex_lock(&mutex);
	cryc_os_mutex_unlock(&mutex);
	cryc_os_mutex_lock(&mutex);
	cryc_os_mutex_unlock(&mutex);
	cryc_os_mutex_destroy(&mutex);
	CHECK(mutex.impl == NULL);
	cryc_os_mutex_destroy(&mutex);
	CHECK(mutex.impl == NULL);
	error = cryc_os_mutex_init(&mutex);
	CHECK(error == 0);
	CHECK(mutex.impl != NULL);
	cryc_os_mutex_destroy(&mutex);

	/* Several threads share a counter and a flag the critical section must keep exclusive. */
	CHECK(cryc_os_mutex_init(&shared_mutex) == 0);
	shared_counter = 0;
	shared_inside = 0;
	shared_violations = 0;
	gate_ready = 0;
	for (index = 0; index < THREAD_COUNT; index++) {
		workers[index].number = index;
		workers[index].started = thread_start(&workers[index].thread, mutex_worker, &workers[index]);
		CHECK(workers[index].started);
	}

	/* Waits for the threads that started. */
	for (index = 0; index < THREAD_COUNT; index++) {
		if (workers[index].started)
			thread_join(workers[index].thread);
	}

	/* Reports that no thread found another inside the section and no increment was lost. */
	CHECK(shared_violations == 0);
	CHECK(shared_counter == (long)(THREAD_COUNT * MUTEX_EACH));
	cryc_os_mutex_destroy(&shared_mutex);
}

/* One thread of the atomic test: adds delta (given in value) many times and marks each returned number. */
static THREAD_RETURN THREAD_CALL
atomic_worker(
	void *argument)
{
	struct worker *worker;
	long delta;
	long result;
	unsigned index;

	/* Takes the delta from the worker record and waits until every thread is running. */
	worker = (struct worker *)argument;
	delta = (long)(size_t)worker->value;
	worker->failures = 0;
	wait_at_gate();

	/* Adds the delta many times and marks each number the addition returns. */
	for (index = 0; index < ATOMIC_EACH; index++) {
		result = cryc_os_atomic_add(&atomic_total, delta);

		/* The number each addition returns is unique among all the additions of the same sign. */
		if (result < 0L || result > (long)(THREAD_COUNT * ATOMIC_EACH)) {
			worker->failures++;
			continue;
		}

		/* A number that was already seen is returned twice. */
		if (atomic_seen[result] != 0U)
			worker->failures++;
		atomic_seen[result] = 1;
	}

	/* Succeeded. */
	return THREAD_RESULT;
}

/* Checks cryc_os_atomic_add alone and across threads. */
static void
test_atomic(void)
{
	volatile long value;
	struct worker workers[THREAD_COUNT];
	unsigned index;
	unsigned long number;
	unsigned long missing;
	long failures;

	/* One thread: the new value comes back. */
	value = 0;
	CHECK(cryc_os_atomic_add(&value, 1) == 1);
	CHECK(cryc_os_atomic_add(&value, 1) == 2);
	CHECK(cryc_os_atomic_add(&value, -1) == 1);
	CHECK(cryc_os_atomic_add(&value, -1) == 0);
	CHECK(cryc_os_atomic_add(&value, -1) == -1);
	CHECK(value == -1);
	value = 0x7ffffffeL;
	CHECK(cryc_os_atomic_add(&value, 1) == 0x7fffffffL);
	value = -0x7ffffffeL;
	CHECK(cryc_os_atomic_add(&value, -1) == -0x7fffffffL);

	/* Four threads add +1: the returned numbers are exactly 1..N, each once. */
	atomic_total = 0;
	atomic_seen = (unsigned char *)calloc(THREAD_COUNT * ATOMIC_EACH + 1U, 1);
	gate_ready = 0;
	for (index = 0; index < THREAD_COUNT; index++) {
		workers[index].number = index;
		workers[index].value = (void *)(size_t)1;
		workers[index].started = thread_start(&workers[index].thread, atomic_worker, &workers[index]);
		CHECK(workers[index].started);
	}

	/* Waits for the threads and adds up their failures. */
	failures = 0;
	for (index = 0; index < THREAD_COUNT; index++) {
		if (workers[index].started) {
			thread_join(workers[index].thread);
			failures += workers[index].failures;
		}
	}

	/* Reports that no thread saw a bad number and the total is right. */
	CHECK(failures == 0);
	CHECK(atomic_total == (long)(THREAD_COUNT * ATOMIC_EACH));

	/* Counts the numbers no addition returned. */
	missing = 0;
	for (number = 1; number <= THREAD_COUNT * ATOMIC_EACH; number++) {
		if (atomic_seen[number] == 0U)
			missing++;
	}

	/* Reports that every number was returned once. */
	CHECK(missing == 0UL);

	/* Four threads add -1 from there: the returned numbers are exactly N-1..0, each once. */
	memset(atomic_seen, 0, THREAD_COUNT * ATOMIC_EACH + 1U);
	gate_ready = 0;
	for (index = 0; index < THREAD_COUNT; index++) {
		workers[index].number = index;
		workers[index].value = (void *)(size_t)-1;
		workers[index].started = thread_start(&workers[index].thread, atomic_worker, &workers[index]);
		CHECK(workers[index].started);
	}

	/* Waits for the threads and adds up their failures. */
	failures = 0;
	for (index = 0; index < THREAD_COUNT; index++) {
		if (workers[index].started) {
			thread_join(workers[index].thread);
			failures += workers[index].failures;
		}
	}

	/* Reports that no thread saw a bad number and the total is back to zero. */
	CHECK(failures == 0);
	CHECK(atomic_total == 0);

	/* Counts the numbers no addition returned. */
	missing = 0;
	for (number = 0; number < THREAD_COUNT * ATOMIC_EACH; number++) {
		if (atomic_seen[number] == 0U)
			missing++;
	}

	/* Reports that every number was returned once. */
	CHECK(missing == 0UL);

	/* Gives the table back. */
	free(atomic_seen);
	atomic_seen = NULL;
}

/*
 * ---- the value of each thread ----
 */

/* The destroy function of the tests: counts the call, notes the value, frees it. */
static void
test_destroy(
	void *value)
{
	/* Counts the call, notes the value, and frees it. */
	cryc_os_atomic_add(&destroy_calls, 1);
	destroy_last = value;
	free(value);
}

/* A thread that sets one value, replaces it with another, and ends. */
static THREAD_RETURN THREAD_CALL
value_worker_replace(
	void *argument)
{
	struct worker *worker;
	void *first;
	void *second;
	void *current;
	int error;

	/* A new thread starts with no value. */
	worker = (struct worker *)argument;
	worker->failures = 0;
	current = cryc_os_thread_value();
	if (current != NULL)
		worker->failures++;

	/* Stores the first value and reads it back. */
	first = malloc(16);
	second = malloc(16);
	error = cryc_os_thread_set_value(first, test_destroy);
	if (error != 0)
		worker->failures++;
	current = cryc_os_thread_value();
	if (current != first)
		worker->failures++;

	/* The first value is the owner's again once it is replaced: destroy is never called for it. */
	error = cryc_os_thread_set_value(second, test_destroy);
	if (error != 0)
		worker->failures++;
	current = cryc_os_thread_value();
	if (current != second)
		worker->failures++;

	/* Frees the replaced value and hands the stored one back for the destroy check. */
	free(first);
	worker->value = second;

	/* Succeeded. */
	return THREAD_RESULT;
}

/* A thread that never sets a value, and one that stores NULL. */
static THREAD_RETURN THREAD_CALL
value_worker_none(
	void *argument)
{
	struct worker *worker;
	void *current;
	int error;

	/* A thread that stored nothing reads NULL. */
	worker = (struct worker *)argument;
	worker->failures = 0;
	current = cryc_os_thread_value();
	if (current != NULL)
		worker->failures++;

	/* The second thread stores NULL, which still reads as NULL. */
	if (worker->number == 1U) {
		error = cryc_os_thread_set_value(NULL, test_destroy);
		if (error != 0)
			worker->failures++;
		current = cryc_os_thread_value();
		if (current != NULL)
			worker->failures++;
	}

	/* Succeeded. */
	return THREAD_RESULT;
}

/* A thread that waits until all its siblings stored their values, then checks it still reads its own. */
static THREAD_RETURN THREAD_CALL
value_worker_barrier(
	void *argument)
{
	struct worker *worker;
	void *mine;
	void *current;
	unsigned spins;
	int error;

	/* Stores this thread's own value. */
	worker = (struct worker *)argument;
	worker->failures = 0;
	mine = malloc(16);
	error = cryc_os_thread_set_value(mine, test_destroy);
	if (error != 0)
		worker->failures++;

	/* Waits at the barrier until every sibling has stored its value. */
	cryc_os_atomic_add(&barrier_arrived, 1);
	for (spins = 0; spins < 20000000U && barrier_arrived < 8; spins++)
		yield_thread();

	/* Notes a barrier that was never filled. */
	if (barrier_arrived < 8)
		worker->failures++;

	/* The value read is still this thread's own. */
	current = cryc_os_thread_value();
	if (current != mine)
		worker->failures++;

	/* Succeeded. */
	return THREAD_RESULT;
}

/* Checks the value of each thread, and the destroy function at the thread's end. */
static void
test_thread_value(void)
{
	struct worker workers[8];
	struct worker single;
	void *mine;
	unsigned index;
	long calls_before;

	/* Before anything was stored the value is NULL, in the main thread too. */
	CHECK(cryc_os_thread_value() == NULL);

	/* A thread that stores two values: the destroy function sees the last one, once, when it ends. */
	destroy_calls = 0;
	destroy_last = NULL;
	single.number = 0;
	single.value = NULL;
	single.started = thread_start(&single.thread, value_worker_replace, &single);
	CHECK(single.started);
	if (single.started)
		thread_join(single.thread);
	CHECK(single.failures == 0);
	CHECK(destroy_calls == 1);
	CHECK(destroy_last == single.value);

	/* The main thread never saw that thread's value. */
	CHECK(cryc_os_thread_value() == NULL);

	/* Threads that never set a value, or set NULL, do not call the destroy function. */
	calls_before = destroy_calls;
	for (index = 0; index < 2U; index++) {
		workers[index].number = index;
		workers[index].started = thread_start(&workers[index].thread, value_worker_none, &workers[index]);
		CHECK(workers[index].started);
		if (workers[index].started)
			thread_join(workers[index].thread);
		CHECK(workers[index].failures == 0);
	}

	/* Reports that no destroy function ran. */
	CHECK(destroy_calls == calls_before);

	/* The main thread has a value of its own, apart from the threads'. */
	mine = malloc(16);
	CHECK(cryc_os_thread_set_value(mine, test_destroy) == 0);
	CHECK(cryc_os_thread_value() == mine);

	/* 64 threads one after another: 64 destroy calls. */
	calls_before = destroy_calls;
	for (index = 0; index < 64U; index++) {
		single.number = index;
		single.started = thread_start(&single.thread, value_worker_replace, &single);
		CHECK(single.started);
		if (single.started)
			thread_join(single.thread);
		CHECK(single.failures == 0);
	}

	/* Reports one destroy call per thread, and that the main thread's value is untouched. */
	CHECK(destroy_calls == calls_before + 64);
	CHECK(cryc_os_thread_value() == mine);

	/* Eight threads at once, each reading its own value after all stored theirs. */
	calls_before = destroy_calls;
	barrier_arrived = 0;
	for (index = 0; index < 8U; index++) {
		workers[index].number = index;
		workers[index].started = thread_start(&workers[index].thread, value_worker_barrier, &workers[index]);
		CHECK(workers[index].started);
	}

	/* Waits for the eight threads and checks that none failed. */
	for (index = 0; index < 8U; index++) {
		if (workers[index].started) {
			thread_join(workers[index].thread);
			CHECK(workers[index].failures == 0);
		}
	}

	/* Reports one destroy call per thread again. */
	CHECK(destroy_calls == calls_before + 8);
	CHECK(cryc_os_thread_value() == mine);

	/* The main thread's value is released by its owner (a thread's destroy is not called when main returns). */
	CHECK(cryc_os_thread_set_value(NULL, test_destroy) == 0);
	CHECK(cryc_os_thread_value() == NULL);
	free(mine);
}

/*
 * ---- files ----
 */

/* The byte at index of the pattern file: every value occurs, including 0x00, 0x0d, 0x0a and 0x1a. */
static unsigned char
pattern_byte(
	size_t index)
{
	/* The byte of the pattern at the index. */
	return (unsigned char)((index * 7U + 3U + (index >> 8)) & 0xffU);
}

/* Writes size bytes of the pattern to name, then reads it with the limits around its size. */
static void
check_file_size(
	size_t size)
{
	char path[DIRECTORY_ROOM + 64U];
	unsigned char *content;
	uint8_t *data;
	size_t length;
	size_t index;
	size_t wrong;
	size_t room;
	int error;

	/* Writes the file. */
	make_path(path, sizeof(path), "os-pattern.bin");
	room = size;
	if (size == 0U)
		room = 1U;
	content = (unsigned char *)malloc(room);
	for (index = 0; index < size; index++)
		content[index] = pattern_byte(index);
	CHECK(write_file(path, content, size));

	/* Reads with max equal to the size: all of it, plus the zero byte after. */
	data = (uint8_t *)1;
	length = 77;
	error = cryc_os_read_file(path, size, &data, &length);
	CHECK(error == 0);
	CHECK(length == size);
	if (error == 0 && length == size) {
		CHECK(data != NULL);
		wrong = 0;
		for (index = 0; index < size; index++) {
			if (data[index] != content[index])
				wrong++;
		}

		/* Reports that every byte matches and the zero byte follows. */
		CHECK(wrong == 0U);
		CHECK(data[size] == 0U);
		free(data);
	}

	/* One byte of room more, and the library's own limit, read it too. */
	error = cryc_os_read_file(path, size + 1U, &data, &length);
	CHECK(error == 0);
	CHECK(length == size);
	if (error == 0)
		free(data);
	error = cryc_os_read_file(path, CRYC_OS_FILE_MAX, &data, &length);
	CHECK(error == 0);
	CHECK(length == size);
	if (error == 0)
		free(data);

	/* One byte of room less: too large, and the outputs are cleared. */
	if (size > 0U) {
		data = (uint8_t *)1;
		length = 77;
		error = cryc_os_read_file(path, size - 1U, &data, &length);
		CHECK(error == EFBIG);
		CHECK(data == NULL);
		CHECK(length == 0U);
	}

	/* Gives the content back. */
	free(content);
}

/* Checks cryc_os_read_file. */
static void
test_read_file(void)
{
	static const size_t sizes[] = {
		0, 1, 2, 255, 256, 4095, 4096, 4097, 16382, 16383, 16384, 16385, 32766, 32767, 32768, 32769, 65535, 65536,
		65537, 100000, 131071, 131072, 131073, 1048576, 1048577
	};
	static const unsigned char crlf[] = { 'a', 0x0dU, 0x0aU, 'b', 0x1aU, 'c', 0x00U, 'd', 0x0aU, 0x0dU };
	static const char japanese[] = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e-\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88.bin";
	static const char emoji[] = "\xf0\x9f\x94\x92-lock.bin";
	char path[DIRECTORY_ROOM + 64U];
	char other[DIRECTORY_ROOM + 128U];
	uint8_t *data;
	size_t length;
	size_t index;
	int created;
	int error;

	/* The sizes around the buffer's growth. */
	for (index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index++)
		check_file_size(sizes[index]);

	/* Binary mode: carriage returns, line feeds, Ctrl-Z and zero bytes come through unchanged. */
	make_path(path, sizeof(path), "os-crlf.bin");
	CHECK(write_file(path, crlf, sizeof(crlf)));
	error = cryc_os_read_file(path, 100, &data, &length);
	CHECK(error == 0);
	CHECK(length == sizeof(crlf));
	if (error == 0 && length == sizeof(crlf)) {
		CHECK_BYTES(data, crlf, sizeof(crlf));
		free(data);
	}

	/* A file that is not there: ENOENT, outputs cleared. */
	make_path(path, sizeof(path), "os-no-such-file.bin");
	data = (uint8_t *)1;
	length = 77;
	error = cryc_os_read_file(path, 100, &data, &length);
	CHECK(error == ENOENT);
	CHECK(data == NULL);
	CHECK(length == 0U);

	/* A directory below which there is a file as a component: not there either. */
	make_path(path, sizeof(path), "os-crlf.bin/inside");
	error = cryc_os_read_file(path, 100, &data, &length);
	CHECK(error == ENOENT);

	/* A directory cannot be read as a file: EIO. */
	data = (uint8_t *)1;
	length = 77;
	error = cryc_os_read_file(test_dir, 100, &data, &length);
	CHECK(error == EIO);
	CHECK(data == NULL);
	CHECK(length == 0U);

	/* Bad arguments. */
	CHECK(cryc_os_read_file(NULL, 100, &data, &length) == EINVAL);
	CHECK(cryc_os_read_file(path, CRYC_OS_FILE_MAX + 1U, &data, &length) == EINVAL);
	CHECK(data == NULL);
	CHECK(cryc_os_read_file(path, 100, NULL, &length) == EINVAL);
	CHECK(cryc_os_read_file(path, 100, &data, NULL) == EINVAL);

	/* Names in UTF-8: Japanese (three bytes a character) and an emoji (a surrogate pair on Windows). */
	make_path(other, sizeof(other), japanese);
	created = write_file(other, crlf, sizeof(crlf));
	if (!created)
		printf("note: a file with a UTF-8 name could not be created here (under Wine run with LC_ALL=C.UTF-8)\n");
	CHECK(write_file(other, crlf, sizeof(crlf)));
	error = cryc_os_read_file(other, 100, &data, &length);
	CHECK(error == 0);
	CHECK(length == sizeof(crlf));
	if (error == 0 && length == sizeof(crlf)) {
		CHECK_BYTES(data, crlf, sizeof(crlf));
		free(data);
	}

	/* Does the same with a name that holds an emoji, and reads a name that was never written. */
	make_path(other, sizeof(other), emoji);
	CHECK(write_file(other, crlf, sizeof(crlf)));
	error = cryc_os_read_file(other, 100, &data, &length);
	CHECK(error == 0);
	CHECK(length == sizeof(crlf));
	if (error == 0)
		free(data);
	make_path(other, sizeof(other), "\xe6\x97\xa5-not-written.bin");
	error = cryc_os_read_file(other, 100, &data, &length);
	CHECK(error == ENOENT);

#ifdef _WIN32
	/* Text that is not UTF-8 names no file: a stray byte, an overlong form, a surrogate, a cut sequence. */
	CHECK(cryc_os_read_file("\xff", 100, &data, &length) == EINVAL);
	CHECK(cryc_os_read_file("\xc0\x80", 100, &data, &length) == EINVAL);
	CHECK(cryc_os_read_file("\xed\xa0\x80", 100, &data, &length) == EINVAL);
	CHECK(cryc_os_read_file("a\xe3\x83", 100, &data, &length) == EINVAL);
	CHECK(data == NULL);
#endif

	/* The size limit itself: exactly CRYC_OS_FILE_MAX reads, one byte more is too large. */
	make_path(path, sizeof(path), "os-limit.bin");
	CHECK(write_sparse_file(path, CRYC_OS_FILE_MAX, 0x7eU));
	error = cryc_os_read_file(path, CRYC_OS_FILE_MAX, &data, &length);
	CHECK(error == 0);
	CHECK(length == CRYC_OS_FILE_MAX);
	if (error == 0 && length == CRYC_OS_FILE_MAX) {
		CHECK(data[0] == 0U);
		CHECK(data[CRYC_OS_FILE_MAX - 2U] == 0U);
		CHECK(data[CRYC_OS_FILE_MAX - 1U] == 0x7eU);
		CHECK(data[CRYC_OS_FILE_MAX] == 0U);
		free(data);
	}

	/* One byte of room less than the file holds is too large. */
	error = cryc_os_read_file(path, CRYC_OS_FILE_MAX - 1U, &data, &length);
	CHECK(error == EFBIG);
	make_path(path, sizeof(path), "os-over-limit.bin");
	CHECK(write_sparse_file(path, CRYC_OS_FILE_MAX + 1U, 0x7fU));
	error = cryc_os_read_file(path, CRYC_OS_FILE_MAX, &data, &length);
	CHECK(error == EFBIG);
	CHECK(data == NULL);
	CHECK(length == 0U);
}

/*
 * ---- sockets ----
 */

/* Makes a connected pair of stream sockets; returns 1 on success. */
static int
make_pair(
	intptr_t *first,
	intptr_t *second)
{
#ifdef _WIN32
	SOCKET listener;
	SOCKET client;
	SOCKET server;
	struct sockaddr_in address;
	int length;
	int status;

	/* A loopback listener on a port the system picks. */
	listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener == INVALID_SOCKET)
		return 0;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = 0;
	length = (int)sizeof(address);
	status = bind(listener, (struct sockaddr *)&address, sizeof(address));
	if (status != 0) {
		closesocket(listener);
		return 0;
	}

	/* Starts listening for the client. */
	status = listen(listener, 1);
	if (status != 0) {
		closesocket(listener);
		return 0;
	}

	/* Learns the port the system picked. */
	status = getsockname(listener, (struct sockaddr *)&address, &length);
	if (status != 0) {
		closesocket(listener);
		return 0;
	}

	/* The client connects (the backlog completes it) and the server accepts. */
	client = socket(AF_INET, SOCK_STREAM, 0);
	if (client == INVALID_SOCKET) {
		closesocket(listener);
		return 0;
	}

	/* Connects the client to the listener. */
	status = connect(client, (struct sockaddr *)&address, sizeof(address));
	if (status != 0) {
		closesocket(listener);
		return 0;
	}

	/* Takes the connection and no longer needs the listener. */
	server = accept(listener, NULL, NULL);
	closesocket(listener);
	if (server == INVALID_SOCKET) {
		closesocket(client);
		return 0;
	}

	/* Hands both ends to the caller. */
	*first = (intptr_t)client;
	*second = (intptr_t)server;

	/* Succeeded: the two ends are connected. */
	return 1;
#else
	int fds[2];
	int status;

	/* Makes the pair. */
	status = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
	if (status != 0)
		return 0;

	/* Hands both ends to the caller. */
	*first = fds[0];
	*second = fds[1];

	/* Succeeded: the two ends are connected. */
	return 1;
#endif
}

/* Puts a descriptor in non-blocking mode. */
static void
set_nonblocking(
	intptr_t descriptor,
	int nonblocking)
{
#ifdef _WIN32
	u_long mode;

	/* Turns the non-blocking mode on or off. */
	mode = 0UL;
	if (nonblocking)
		mode = 1UL;
	ioctlsocket((SOCKET)descriptor, FIONBIO, &mode);
#else
	int flags;

	/* Reads the flags, sets or clears the non-blocking flag, and writes them back. */
	flags = fcntl((int)descriptor, F_GETFL, 0);
	if (nonblocking)
		flags |= O_NONBLOCK;
	else
		flags &= ~O_NONBLOCK;
	fcntl((int)descriptor, F_SETFL, flags);
#endif
}

/* Closes a descriptor. */
static void
close_descriptor(
	intptr_t descriptor)
{
#ifdef _WIN32
	closesocket((SOCKET)descriptor);
#else
	close((int)descriptor);
#endif
}

/* Reads from a non-blocking socket until the result is not EAGAIN (the peer's close takes a moment to arrive). */
static int
read_settled(
	intptr_t descriptor,
	uint8_t *buffer,
	size_t size,
	size_t *done,
	int *os_error)
{
	unsigned tries;
	int error;

	/* Reads again, a little later each time, while the socket still says EAGAIN. */
	error = EAGAIN;
	for (tries = 0; tries < 200U && error == EAGAIN; tries++) {
		error = cryc_os_socket_read(descriptor, buffer, size, done, os_error);
		if (error == EAGAIN)
			pause_milliseconds(10);
	}

	/* Reports the last answer of the socket (EAGAIN if it never settled). */
	return error;
}

/* Basic reads and writes, short and long, then the end of the stream. */
static void
check_socket_basic(void)
{
	intptr_t first;
	intptr_t second;
	uint8_t buffer[100];
	size_t done;
	int os_error;
	int error;

	/* Makes the connected pair. */
	CHECK(make_pair(&first, &second));

	/* A short write arrives whole; a read with more room than data returns what there is. */
	done = 77;
	os_error = 77;
	error = cryc_os_socket_write(first, (const uint8_t *)"hello", 5, &done, &os_error);
	CHECK(error == 0);
	CHECK(done == 5U);
	CHECK(os_error == 0);
	done = 77;
	os_error = 77;
	error = cryc_os_socket_read(second, buffer, sizeof(buffer), &done, &os_error);
	CHECK(error == 0);
	CHECK(done == 5U);
	CHECK(os_error == 0);
	CHECK_BYTES(buffer, "hello", 5);

	/* A read with less room than data returns the room, and the rest follows. */
	error = cryc_os_socket_write(first, (const uint8_t *)"abcdefgh", 8, &done, &os_error);
	CHECK(error == 0);
	CHECK(done == 8U);
	error = cryc_os_socket_read(second, buffer, 3, &done, &os_error);
	CHECK(error == 0);
	CHECK(done == 3U);
	CHECK_BYTES(buffer, "abc", 3);
	error = cryc_os_socket_read(second, buffer, sizeof(buffer), &done, &os_error);
	CHECK(error == 0);
	CHECK(done == 5U);
	CHECK_BYTES(buffer, "defgh", 5);

	/* Both directions work, and a missing os_error is allowed. */
	error = cryc_os_socket_write(second, (const uint8_t *)"back", 4, &done, NULL);
	CHECK(error == 0);
	CHECK(done == 4U);
	error = cryc_os_socket_read(first, buffer, sizeof(buffer), &done, NULL);
	CHECK(error == 0);
	CHECK(done == 4U);
	CHECK_BYTES(buffer, "back", 4);

	/* Zero bytes: a write of nothing is done at once; a read of nothing is refused (it would look like the end). */
	done = 77;
	error = cryc_os_socket_write(first, (const uint8_t *)"x", 0, &done, &os_error);
	CHECK(error == 0);
	CHECK(done == 0U);
	done = 77;
	error = cryc_os_socket_read(second, buffer, 0, &done, &os_error);
	CHECK(error == EINVAL);
	CHECK(done == 0U);

	/* A missing count or buffer is refused. */
	CHECK(cryc_os_socket_read(second, buffer, 1, NULL, &os_error) == EINVAL);
	CHECK(cryc_os_socket_write(first, (const uint8_t *)"x", 1, NULL, &os_error) == EINVAL);
	CHECK(cryc_os_socket_read(second, NULL, 1, &done, &os_error) == EINVAL);
	CHECK(cryc_os_socket_write(first, NULL, 1, &done, &os_error) == EINVAL);

	/* A non-blocking socket with nothing to read: EAGAIN and the system's code. */
	set_nonblocking(second, 1);
	done = 77;
	os_error = 0;
	error = cryc_os_socket_read(second, buffer, sizeof(buffer), &done, &os_error);
	CHECK(error == EAGAIN);
	CHECK(done == 0U);
	CHECK(os_error != 0);

	/* The end of the stream: zero bytes with success, again and again. */
	close_descriptor(first);
	done = 77;
	error = read_settled(second, buffer, sizeof(buffer), &done, &os_error);
	CHECK(error == 0);
	CHECK(done == 0U);
	done = 77;
	error = cryc_os_socket_read(second, buffer, sizeof(buffer), &done, &os_error);
	CHECK(error == 0);
	CHECK(done == 0U);
	close_descriptor(second);
}

/* Moves a large block through a pair of non-blocking sockets, in partial writes and reads. */
static void
check_socket_transfer(void)
{
	intptr_t first;
	intptr_t second;
	unsigned char *source;
	unsigned char *sink;
	unsigned char *chunk;
	size_t sent;
	size_t received;
	size_t done;
	size_t pending;
	size_t wanted;
	size_t compare_size;
	unsigned long iterations;
	int partial_seen;
	int write_blocked;
	int read_blocked;
	int broken;
	int os_error;
	int error;
	int difference;

	/* Makes the pair, sets both ends non-blocking, and prepares the buffers. */
	CHECK(make_pair(&first, &second));
	set_nonblocking(first, 1);
	set_nonblocking(second, 1);
	source = (unsigned char *)malloc(TRANSFER_SIZE);
	sink = (unsigned char *)malloc(TRANSFER_SIZE);
	chunk = (unsigned char *)malloc(65536U);
	fill_random_bytes(source, TRANSFER_SIZE);
	memset(chunk, 0x6b, 65536U);
	partial_seen = 0;
	write_blocked = 0;
	read_blocked = 0;
	broken = 0;

	/* Writes without reading until the socket has no room: EAGAIN, nothing written, the system's code. */
	pending = 0;
	for (iterations = 0; iterations < 4096UL && !write_blocked && !broken; iterations++) {
		error = cryc_os_socket_write(first, chunk, 65536U, &done, &os_error);
		if (error == 0 && done <= 65536U) {
			pending += done;
			if (done < 65536U)
				partial_seen = 1;
		} else if (error == EAGAIN && done == 0U && os_error != 0) {
			write_blocked = 1;
		} else {
			broken = 1;
		}
	}

	/* Reports that no write failed and the socket ran out of room. */
	CHECK(!broken);
	CHECK(write_blocked);

	/* Reads all of that back (the bytes are the chunk's), then the socket is empty: EAGAIN. */
	received = 0;
	for (iterations = 0; iterations < 5000000UL && received < pending && !broken; iterations++) {
		/* Asks for no more than is still pending. */
		wanted = pending - received;
		if (wanted > TRANSFER_SIZE)
			wanted = TRANSFER_SIZE;
		error = cryc_os_socket_read(second, sink, wanted, &done, &os_error);
		if (error == 0 && done > 0U) {
			/* Compares the bytes with the chunk, as far as the chunk reaches. */
			compare_size = done;
			if (compare_size > 65536U)
				compare_size = 65536U;
			difference = memcmp(sink, chunk, compare_size);
			if (difference != 0 && done <= 65536U)
				broken = 1;
			received += done;
		} else if (error != EAGAIN) {
			broken = 1;
		}
	}

	/* Reports that the reads matched and everything written came back. */
	CHECK(!broken);
	CHECK(received == pending);

	/* The empty socket answers EAGAIN. */
	error = cryc_os_socket_read(second, sink, 1, &done, &os_error);
	if (error == EAGAIN && done == 0U)
		read_blocked = 1;
	CHECK(read_blocked);

	/* Writes whatever the socket takes, reads whatever has arrived, until a large block has crossed. */
	sent = 0;
	received = 0;
	for (iterations = 0; iterations < 5000000UL && received < TRANSFER_SIZE && !broken; iterations++) {
		if (sent < TRANSFER_SIZE) {
			error = cryc_os_socket_write(first, source + sent, TRANSFER_SIZE - sent, &done, &os_error);
			if (error == 0) {
				if (done > TRANSFER_SIZE - sent)
					broken = 1;
				else if (done < TRANSFER_SIZE - sent)
					partial_seen = 1;
				sent += done;
			} else if (error != EAGAIN || done != 0U) {
				broken = 1;
			}
		}

		/* Reads whatever has arrived. */
		error = cryc_os_socket_read(second, sink + received, TRANSFER_SIZE - received, &done, &os_error);
		if (error == 0) {
			if (done == 0U || done > TRANSFER_SIZE - received)
				broken = 1;
			received += done;
		} else if (error != EAGAIN) {
			broken = 1;
		}
	}

	/* Reports that the whole block crossed intact. */
	CHECK(!broken);
	CHECK(sent == TRANSFER_SIZE);
	CHECK(received == TRANSFER_SIZE);
	if (received == TRANSFER_SIZE)
		CHECK_BYTES(sink, source, TRANSFER_SIZE);

	/* A write larger than the socket takes is partial, not a loop that waits.  Windows accepts a whole large non-blocking send, so only POSIX is held to it. */
#ifndef _WIN32
	CHECK(partial_seen);
#else
	UNUSED_PARAMETER(partial_seen);
#endif

	/* Gives the buffers back and closes the pair. */
	free(source);
	free(sink);
	free(chunk);
	close_descriptor(first);
	close_descriptor(second);
}

/* A write to a socket whose peer closed is EIO with codes; a bad descriptor is EIO too. */
static void
check_socket_errors(void)
{
	intptr_t first;
	intptr_t second;
	uint8_t bytes[1024];
	uint8_t buffer[16];
	size_t done;
	unsigned tries;
	int os_error;
	int error;
	int saved_errno;

	/* Fills the bytes the writes send. */
	memset(bytes, 0x33, sizeof(bytes));

	/* The peer closes: sooner or later a write fails (a TCP peer answers the first one with a reset). */
	CHECK(make_pair(&first, &second));
	close_descriptor(second);
	error = 0;
	os_error = 0;
	saved_errno = 0;
	for (tries = 0; tries < 100U && error == 0; tries++) {
		error = cryc_os_socket_write(first, bytes, sizeof(bytes), &done, &os_error);
		if (error == 0)
			pause_milliseconds(20);
	}

	/* Notes the errno the failed write left. */
	saved_errno = errno;
	CHECK(error == EIO);
	CHECK(os_error != 0);
	CHECK(saved_errno == EPIPE || saved_errno == ECONNRESET || saved_errno == ECONNABORTED);
	close_descriptor(first);

	/* A descriptor that is not open: EIO with the system's code. */
#ifdef _WIN32
	done = 77;
	os_error = 0;
	error = cryc_os_socket_read((intptr_t)INVALID_SOCKET, buffer, sizeof(buffer), &done, &os_error);
	saved_errno = errno;
	CHECK(error == EIO);
	CHECK(done == 0U);
	CHECK(os_error == WSAENOTSOCK);
	CHECK(saved_errno == ENOTSOCK);
	done = 77;
	os_error = 0;
	error = cryc_os_socket_write((intptr_t)INVALID_SOCKET, bytes, sizeof(bytes), &done, &os_error);
	saved_errno = errno;
	CHECK(error == EIO);
	CHECK(done == 0U);
	CHECK(os_error == WSAENOTSOCK);
	CHECK(saved_errno == ENOTSOCK);
#else
	CHECK(make_pair(&first, &second));
	close_descriptor(first);
	close_descriptor(second);
	done = 77;
	os_error = 0;
	error = cryc_os_socket_read(first, buffer, sizeof(buffer), &done, &os_error);
	CHECK(error == EIO);
	CHECK(done == 0U);
	CHECK(os_error == EBADF);
	done = 77;
	os_error = 0;
	error = cryc_os_socket_write(first, bytes, sizeof(bytes), &done, &os_error);
	CHECK(error == EIO);
	CHECK(done == 0U);
	CHECK(os_error == EBADF);

	/* Values that are no descriptor at all. */
	os_error = 0;
	error = cryc_os_socket_read(-1, buffer, sizeof(buffer), &done, &os_error);
	CHECK(error == EIO);
	CHECK(os_error == EBADF);
	os_error = 0;
	error = cryc_os_socket_write(-1, bytes, sizeof(bytes), &done, &os_error);
	CHECK(error == EIO);
	CHECK(os_error == EBADF);
	if (sizeof(intptr_t) > 4U) {
		/* A value above the range of an int must not wrap to a descriptor that is open. */
		os_error = 0;
		error = cryc_os_socket_read((intptr_t)(((uint64_t)1 << 32) + 1U), buffer, sizeof(buffer), &done, &os_error);
		CHECK(error == EIO);
		CHECK(os_error == EBADF);
		os_error = 0;
		error = cryc_os_socket_write((intptr_t)(((uint64_t)1 << 32) + 1U), bytes, sizeof(bytes), &done, &os_error);
		CHECK(error == EIO);
		CHECK(os_error == EBADF);
	}
#endif
}

#ifndef _WIN32

/* The handler of SIGALRM: on the third tick it makes the blocked call able to finish. */
static void
alarm_handler(
	int number)
{
	char junk[65536];
	ssize_t got;

	/* Counts the tick. */
	got = 0;
	(void)number;
	signal_ticks++;

	/* The third tick lets the blocked call finish: it writes a byte or drains the socket. */
	if (signal_ticks == 3) {
		if (signal_write_fd >= 0)
			got = write(signal_write_fd, "z", 1);
		if (signal_read_fd >= 0)
			got = read(signal_read_fd, junk, sizeof(junk));
		(void)got;
	}
}

/* Starts (or, with 0, stops) a timer that sends SIGALRM every 20 ms. */
static void
set_timer(
	int on)
{
	struct itimerval timer;

	/* Fills the timer: running when asked, stopped otherwise. */
	memset(&timer, 0, sizeof(timer));
	if (on) {
		timer.it_interval.tv_usec = 20000;
		timer.it_value.tv_usec = 20000;
	}

	/* Applies the timer. */
	setitimer(ITIMER_REAL, &timer, NULL);
}

/* A call that blocks and is interrupted by signals is retried until it can finish. */
static void
check_socket_interrupted(void)
{
	struct sigaction action;
	struct sigaction old_action;
	intptr_t first;
	intptr_t second;
	uint8_t buffer[16];
	uint8_t chunk[4096];
	size_t done;
	size_t filled;
	int os_error;
	int error;

	/* The handler is installed without SA_RESTART, so a blocked read or write ends with EINTR. */
	memset(&action, 0, sizeof(action));
	action.sa_handler = alarm_handler;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	sigaction(SIGALRM, &action, &old_action);

	/* A blocking read with no data: two signals interrupt it, the third makes a byte arrive. */
	CHECK(make_pair(&first, &second));
	signal_ticks = 0;
	signal_write_fd = (int)first;
	signal_read_fd = -1;
	set_timer(1);
	done = 77;
	error = cryc_os_socket_read(second, buffer, sizeof(buffer), &done, &os_error);
	set_timer(0);
	CHECK(error == 0);
	CHECK(done == 1U);
	CHECK(buffer[0] == (uint8_t)'z');
	CHECK(signal_ticks >= 3);
	close_descriptor(first);
	close_descriptor(second);

	/* A blocking write into a full socket: the signals interrupt it, the third drains the other end. */
	CHECK(make_pair(&first, &second));
	memset(chunk, 0x44, sizeof(chunk));
	set_nonblocking(first, 1);
	filled = 0;
	do {
		error = cryc_os_socket_write(first, chunk, sizeof(chunk), &done, &os_error);
		filled += done;
	} while (error == 0 && filled < 64UL * 1024UL * 1024UL);
	CHECK(error == EAGAIN);
	set_nonblocking(first, 0);
	signal_ticks = 0;
	signal_write_fd = -1;
	signal_read_fd = (int)second;
	set_nonblocking(second, 1);
	set_timer(1);
	done = 77;
	error = cryc_os_socket_write(first, chunk, 1, &done, &os_error);
	set_timer(0);
	CHECK(error == 0);
	CHECK(done == 1U);
	CHECK(signal_ticks >= 3);
	close_descriptor(first);
	close_descriptor(second);

	/* The handler is put back. */
	sigaction(SIGALRM, &old_action, NULL);
	signal_read_fd = -1;
	signal_write_fd = -1;
}

/* A pipe is a descriptor that is not a socket: writes and reads still work; descriptors that wrap an int are refused. */
static void
check_socket_pipe(void)
{
	int fds[2];
	uint8_t buffer[16];
	size_t done;
	intptr_t high_read;
	intptr_t high_write;
	intptr_t low_read;
	intptr_t low_write;
	int os_error;
	int error;

	/* Makes the pipe. */
	CHECK(pipe(fds) == 0);

	/* The read end does not block, so a call that fails to move bytes cannot hang the test. */
	set_nonblocking(fds[0], 1);
	error = cryc_os_socket_write(fds[1], (const uint8_t *)"pipe!", 5, &done, &os_error);
	CHECK(error == 0);
	CHECK(done == 5U);
	error = cryc_os_socket_read(fds[0], buffer, sizeof(buffer), &done, &os_error);
	CHECK(error == 0);
	CHECK(done == 5U);
	CHECK_BYTES(buffer, "pipe!", 5);

	/*
	 * A value above the range of an int, or below it, that would wrap to the open end of this pipe is no descriptor:
	 * the bytes written first must still be in the pipe afterwards, and nothing may be written through the wrapped end.
	 */
	if (sizeof(intptr_t) > 4U) {
		/* Builds the values that wrap to each end of the pipe. */
		high_read = (intptr_t)(((uint64_t)1 << 32) + (uint64_t)fds[0]);
		high_write = (intptr_t)(((uint64_t)1 << 32) + (uint64_t)fds[1]);
		low_read = (intptr_t)((int64_t)fds[0] - ((int64_t)1 << 32));
		low_write = (intptr_t)((int64_t)fds[1] - ((int64_t)1 << 32));
		CHECK(cryc_os_socket_write(fds[1], (const uint8_t *)"kept", 4, &done, &os_error) == 0);
		os_error = 0;
		error = cryc_os_socket_read(high_read, buffer, sizeof(buffer), &done, &os_error);
		CHECK(error == EIO);
		CHECK(done == 0U);
		CHECK(os_error == EBADF);
		os_error = 0;
		error = cryc_os_socket_read(low_read, buffer, sizeof(buffer), &done, &os_error);
		CHECK(error == EIO);
		CHECK(done == 0U);
		CHECK(os_error == EBADF);
		os_error = 0;
		error = cryc_os_socket_write(high_write, (const uint8_t *)"lost", 4, &done, &os_error);
		CHECK(error == EIO);
		CHECK(done == 0U);
		CHECK(os_error == EBADF);
		os_error = 0;
		error = cryc_os_socket_write(low_write, (const uint8_t *)"lost", 4, &done, &os_error);
		CHECK(error == EIO);
		CHECK(done == 0U);
		CHECK(os_error == EBADF);
		error = cryc_os_socket_read(fds[0], buffer, sizeof(buffer), &done, &os_error);
		CHECK(error == 0);
		CHECK(done == 4U);
		CHECK_BYTES(buffer, "kept", 4);
	}

	/* The end of the stream once the writer closed. */
	close(fds[1]);
	error = cryc_os_socket_read(fds[0], buffer, sizeof(buffer), &done, &os_error);
	CHECK(error == 0);
	CHECK(done == 0U);
	close(fds[0]);
}

#endif

/*
 * ---- the system's trust sources ----
 */

/* Notes which kind of call came now: 0 distrusted, 1 DER, 2 file; they must come in that order. */
static void
note_kind(
	struct roots_log *log,
	int kind)
{
	/* A call of an earlier kind after a later one breaks the order. */
	if (kind < log->last_kind)
		log->order_broken = 1;
	log->last_kind = kind;
}

/* Remembers a certificate when it is one of the test's own. */
static void
note_certificate(
	struct roots_log *log,
	int kind,
	const uint8_t *der,
	size_t size)
{
	int index;
	int difference;

	/* Compares the certificate with each known one, and counts a hit with the kind of call that carried it. */
	for (index = 0; index < 8; index++) {
		/* Skips an unused slot and a certificate of another size. */
		if (log->known[index] == NULL)
			continue;
		if (log->known_size[index] != size)
			continue;

		/* A match is a hit, recorded with the kind of call that carried it. */
		difference = memcmp(log->known[index], der, size);
		if (difference == 0) {
			log->hits[index]++;
			log->hit_kind[index] = kind;
		}
	}
}

/* The distrusted callback of the tests. */
static int
log_distrusted(
	void *context,
	const uint8_t *der,
	size_t size)
{
	struct roots_log *log;

	/* Counts the call and notes its kind and the certificate it carries. */
	log = (struct roots_log *)context;
	log->distrusted++;
	note_kind(log, 0);
	note_certificate(log, 0, der, size);

	/* Fails when the test asked this kind of call to fail. */
	if (log->stop_kind == 0)
		return log->stop_error;

	/* Succeeded: the call is recorded. */
	return 0;
}

/* The DER callback of the tests. */
static int
log_der(
	void *context,
	const uint8_t *der,
	size_t size)
{
	struct roots_log *log;

	/* Counts the call and notes its kind and the certificate it carries. */
	log = (struct roots_log *)context;
	log->ders++;
	note_kind(log, 1);
	note_certificate(log, 1, der, size);

	/* Fails when the test asked this kind of call to fail. */
	if (log->stop_kind == 1)
		return log->stop_error;

	/* Succeeded: the call is recorded. */
	return 0;
}

/* The file callback of the tests. */
static int
log_file(
	void *context,
	const char *path)
{
	struct roots_log *log;
	size_t length;

	/* Counts the call and notes its kind. */
	log = (struct roots_log *)context;
	log->files++;
	note_kind(log, 2);

	/* Keeps the path when it fits. */
	length = strlen(path);
	if (length < sizeof(log->file_path))
		strcpy(log->file_path, path);

	/* Reports the error the test chose for the file call. */
	return log->file_error;
}

/* Clears a log (keeping nothing) before a run. */
static void
roots_log_clear(
	struct roots_log *log)
{
	/* Zeroes the log, then marks that no call came yet and no call is to fail. */
	memset(log, 0, sizeof(*log));
	log->last_kind = -1;
	log->stop_kind = -1;
}

#ifdef _WIN32

/* Writes a Name with one common name (X.690 8.1; X.501 Name, RDN, AttributeTypeAndValue). */
static void
write_name(
	struct der_writer *writer,
	const char *common_name)
{
	unsigned char inner[100];
	unsigned char pair[120];
	unsigned char set[130];
	struct der_writer name_writer;
	struct der_writer pair_writer;
	struct der_writer set_writer;

	/* Builds the attribute (the common name), wraps it in a SET, then in the Name. */
	der_init(&pair_writer, pair, sizeof(pair));
	der_oid(&pair_writer, "2.5.4.3");
	der_put(&pair_writer, 0x0cU, (const unsigned char *)common_name, strlen(common_name));
	der_init(&set_writer, set, sizeof(set));
	der_wrap(&set_writer, 0x30U, &pair_writer);
	der_init(&name_writer, inner, sizeof(inner));
	der_wrap(&name_writer, 0x31U, &set_writer);
	der_wrap(writer, 0x30U, &name_writer);
}

/* Writes an AlgorithmIdentifier with a NULL parameter. */
static void
write_algorithm(
	struct der_writer *writer,
	const char *oid)
{
	unsigned char inner[40];
	struct der_writer algorithm;

	/* Builds the identifier and its NULL parameter, and wraps them. */
	der_init(&algorithm, inner, sizeof(inner));
	der_oid(&algorithm, oid);
	der_put(&algorithm, 0x05U, NULL, 0);
	der_wrap(writer, 0x30U, &algorithm);
}

/*
 * Builds one X.509 certificate (RFC 5280 4.1) that is well formed but not signed by anyone: a distinct serial
 * and subject, a 1024-bit RSA public key of no particular value, and optionally an extended key usage extension
 * (4.2.1.12) that lists clientAuth only.
 */
static size_t
make_certificate(
	unsigned char *out,
	size_t room,
	unsigned serial,
	const char *common_name,
	int client_only_extension)
{
	unsigned char tbs_bytes[600];
	unsigned char piece[400];
	unsigned char key_bytes[200];
	unsigned char modulus[128];
	unsigned char exponent[3];
	unsigned char serial_byte[1];
	unsigned char version[1];
	unsigned char key_bits[200];
	unsigned char extension_bytes[100];
	unsigned char usage_bytes[40];
	unsigned char extension_list[100];
	unsigned char fake_signature[65];
	unsigned char body[900];
	struct der_writer body_writer;
	struct der_writer tbs;
	struct der_writer part;
	struct der_writer key;
	struct der_writer spki;
	struct der_writer validity;
	struct der_writer extension;
	struct der_writer usage;
	struct der_writer extension_wrap;
	struct der_writer outer;
	size_t index;

	/* The TBSCertificate: version v3, serial, signature algorithm, issuer, validity, subject, key. */
	der_init(&tbs, tbs_bytes, sizeof(tbs_bytes));
	der_init(&part, piece, sizeof(piece));
	version[0] = 2;
	der_put(&part, 0x02U, version, 1);
	der_wrap(&tbs, 0xa0U, &part);
	serial_byte[0] = (unsigned char)serial;
	der_unsigned(&tbs, serial_byte, 1);
	write_algorithm(&tbs, "1.2.840.113549.1.1.11");
	write_name(&tbs, "ws335-os-test-issuer");
	der_init(&validity, piece, sizeof(piece));
	der_put(&validity, 0x17U, (const unsigned char *)"200101000000Z", 13);
	der_put(&validity, 0x17U, (const unsigned char *)"491231235959Z", 13);
	der_wrap(&tbs, 0x30U, &validity);
	write_name(&tbs, common_name);

	/* SubjectPublicKeyInfo: rsaEncryption, then the key as a BIT STRING of an RSAPublicKey. */
	for (index = 0; index < sizeof(modulus); index++)
		modulus[index] = (unsigned char)(0x81U + index * 3U + serial);
	modulus[sizeof(modulus) - 1U] |= 1U;
	exponent[0] = 0x01U;
	exponent[1] = 0x00U;
	exponent[2] = 0x01U;
	der_init(&key, key_bytes, sizeof(key_bytes));
	der_unsigned(&key, modulus, sizeof(modulus));
	der_unsigned(&key, exponent, sizeof(exponent));
	key_bits[0] = 0;
	der_init(&part, key_bits + 1, sizeof(key_bits) - 1U);
	der_wrap(&part, 0x30U, &key);
	der_init(&spki, piece, sizeof(piece));
	write_algorithm(&spki, "1.2.840.113549.1.1.1");
	der_put(&spki, 0x03U, key_bits, part.used + 1U);
	der_wrap(&tbs, 0x30U, &spki);

	/* Extensions [3]: extKeyUsage with clientAuth only. */
	if (client_only_extension) {
		der_init(&usage, usage_bytes, sizeof(usage_bytes));
		der_oid(&usage, "1.3.6.1.5.5.7.3.2");
		der_init(&part, piece, sizeof(piece));
		der_wrap(&part, 0x30U, &usage);
		der_init(&extension, extension_bytes, sizeof(extension_bytes));
		der_oid(&extension, "2.5.29.37");
		der_put(&extension, 0x04U, piece, part.used);
		der_init(&extension_wrap, extension_list, sizeof(extension_list));
		der_wrap(&extension_wrap, 0x30U, &extension);
		der_init(&part, piece, sizeof(piece));
		der_wrap(&part, 0x30U, &extension_wrap);
		der_put(&tbs, 0xa3U, piece, part.used);
	}

	/* The Certificate: TBS, algorithm, signature (not a real one). */
	der_init(&outer, out, room);
	der_init(&body_writer, body, sizeof(body));
	der_wrap(&body_writer, 0x30U, &tbs);
	write_algorithm(&body_writer, "1.2.840.113549.1.1.11");
	fake_signature[0] = 0;
	for (index = 1; index < sizeof(fake_signature); index++)
		fake_signature[index] = (unsigned char)(index * 5U + serial);
	der_put(&body_writer, 0x03U, fake_signature, sizeof(fake_signature));
	der_wrap(&outer, 0x30U, &body_writer);

	/* A writer that ran out of room yields no certificate. */
	if (outer.error)
		return 0;

	/* Succeeded: the length of the certificate. */
	return outer.used;
}

/* Opens a logical store of the current user for adding (created when absent). */
static HCERTSTORE
open_store(
	const WCHAR *name)
{
	HCERTSTORE store;

	/* Opens the logical store. */
	store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER, name);

	/* The store, NULL when it could not be opened. */
	return store;
}

/*
 * Deletes every copy of the test certificates from a store of the current user (the prefix keeps its stores between
 * runs).  The delete goes through the registry store of the same key: under Wine a certificate found through the
 * logical system store is reported deleted but stays in the registry, while the registry store really removes it.
 */
static void
purge_store(
	const WCHAR *key_path)
{
	HKEY key;
	HCERTSTORE store;
	PCCERT_CONTEXT wanted;
	PCCERT_CONTEXT found;
	LONG status;
	BOOL deleted;
	int index;

	/* Opens the registry key and the registry store on it. */
	status = RegOpenKeyExW(HKEY_CURRENT_USER, key_path, 0, KEY_ALL_ACCESS, &key);
	if (status != ERROR_SUCCESS)
		return;
	store = CertOpenStore(CERT_STORE_PROV_REG, 0, 0, 0, key);
	if (store == NULL) {
		RegCloseKey(key);
		return;
	}

	/* Removes every copy of every test certificate. */
	for (index = 0; index < CERT_COUNT; index++) {
		wanted = CertCreateCertificateContext(X509_ASN_ENCODING, cert_der[index], (DWORD)cert_size[index]);
		if (wanted == NULL)
			continue;
		for (;;) {
			found = CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_EXISTING, wanted, NULL);
			if (found == NULL)
				break;

			/* The delete frees the context passed in. */
			deleted = CertDeleteCertificateFromStore(found);
			if (!deleted)
				break;
		}

		/* Gives back the context of the wanted certificate. */
		CertFreeCertificateContext(wanted);
	}

	/* Closes the store and the key. */
	CertCloseStore(store, 0);
	RegCloseKey(key);
}

/* Adds a test certificate to a store, optionally with an enhanced key usage property; returns 1 on success. */
static int
add_certificate(
	HCERTSTORE store,
	int index,
	const char **usages,
	DWORD usage_count,
	int with_property)
{
	PCCERT_CONTEXT added;
	CERT_ENHKEY_USAGE usage;
	BOOL good;
	BOOL stored;

	/* Adds the certificate, replacing a copy that is already there. */
	added = NULL;
	stored = CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, cert_der[index], (DWORD)cert_size[index],
	    CERT_STORE_ADD_REPLACE_EXISTING, &added);
	if (!stored)
		return 0;

	/* Sets the enhanced key usage property when asked. */
	good = TRUE;
	if (with_property) {
		usage.cUsageIdentifier = usage_count;
		usage.rgpszUsageIdentifier = (LPSTR *)usages;
		good = CertSetEnhancedKeyUsage(added, &usage);
	}

	/* Gives back the context the add returned. */
	CertFreeCertificateContext(added);

	/* A property that could not be set fails the call. */
	if (!good)
		return 0;

	/* Succeeded: the certificate is in the store. */
	return 1;
}

/* The directory of the running program and the path cert.pem beside it, in UTF-8 (the way the library builds it). */
static int
expected_certificate_path(
	char *out,
	size_t room)
{
	WCHAR path[1100];
	DWORD length;
	DWORD directory;
	WCHAR full[1200];
	static const WCHAR name[] = { 'c', 'e', 'r', 't', '.', 'p', 'e', 'm', 0 };
	int converted;

	/* Finds the path of the running program. */
	length = GetModuleFileNameW(NULL, path, 1100);
	if (length == 0 || length >= 1100)
		return 0;

	/* Cuts the path at its last separator. */
	directory = length;
	while (directory > 0 && path[directory - 1] != L'\\')
		directory--;
	if (directory == 0)
		return 0;

	/* Appends the file name and converts the result to UTF-8. */
	memcpy(full, path, directory * sizeof(WCHAR));
	memcpy(full + directory, name, sizeof(name));
	converted = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, full, -1, out, (int)room, NULL, NULL);
	if (converted <= 0)
		return 0;

	/* Succeeded: the path is in out. */
	return 1;
}

/* The Windows trust sources against certificates this test adds to the current user's stores. */
static void
check_roots_windows(void)
{
	static const char *client_only[1] = { "1.3.6.1.5.5.7.3.2" };
	static const char *both[2] = { "1.3.6.1.5.5.7.3.1", "1.3.6.1.5.5.7.3.2" };
	static const WCHAR disallowed[] = { 'D', 'i', 's', 'a', 'l', 'l', 'o', 'w', 'e', 'd', 0 };
	static const WCHAR root[] = { 'R', 'O', 'O', 'T', 0 };
	static const WCHAR disallowed_key[] = L"Software\\Microsoft\\SystemCertificates\\Disallowed";
	static const WCHAR root_key[] = L"Software\\Microsoft\\SystemCertificates\\ROOT";
	static const unsigned char pem_text[] = "ws335-os-test-cert.pem\n";
	struct roots_log log;
	HCERTSTORE disallowed_store;
	HCERTSTORE root_store;
	char expected[1100];
	char beside[DIRECTORY_ROOM + 64U];
	uint8_t *data;
	size_t length;
	int index;
	int error;

	/* The test's own certificates. */
	cert_size[CERT_DISALLOWED] = make_certificate(cert_der[CERT_DISALLOWED], sizeof(cert_der[0]), 1, "ws335 disallowed", 0);
	cert_size[CERT_PLAIN] = make_certificate(cert_der[CERT_PLAIN], sizeof(cert_der[0]), 2, "ws335 plain root", 0);
	cert_size[CERT_CLIENT_ONLY] = make_certificate(cert_der[CERT_CLIENT_ONLY], sizeof(cert_der[0]), 3, "ws335 client only", 0);
	cert_size[CERT_EMPTY_USAGE] = make_certificate(cert_der[CERT_EMPTY_USAGE], sizeof(cert_der[0]), 4, "ws335 empty usage", 0);
	cert_size[CERT_BOTH_USAGES] = make_certificate(cert_der[CERT_BOTH_USAGES], sizeof(cert_der[0]), 5, "ws335 both usages", 0);
	cert_size[CERT_EXTENSION_ONLY] = make_certificate(cert_der[CERT_EXTENSION_ONLY], sizeof(cert_der[0]), 6, "ws335 extension only", 1);
	for (index = 0; index < CERT_COUNT; index++)
		CHECK(cert_size[index] > 100U);

	/* Removes what an earlier run may have left, adds the certificates, keeps going only when that worked. */
	purge_store(disallowed_key);
	purge_store(root_key);
	disallowed_store = open_store(disallowed);
	root_store = open_store(root);
	CHECK(disallowed_store != NULL);
	CHECK(root_store != NULL);
	if (disallowed_store == NULL || root_store == NULL) {
		if (disallowed_store != NULL)
			CertCloseStore(disallowed_store, 0);
		if (root_store != NULL)
			CertCloseStore(root_store, 0);
		return;
	}

	/* Adds the certificates to the stores, some with an enhanced key usage property. */
	CHECK(add_certificate(disallowed_store, CERT_DISALLOWED, NULL, 0, 0));
	CHECK(add_certificate(root_store, CERT_DISALLOWED, NULL, 0, 0));
	CHECK(add_certificate(root_store, CERT_PLAIN, NULL, 0, 0));
	CHECK(add_certificate(root_store, CERT_CLIENT_ONLY, client_only, 1, 1));
	CHECK(add_certificate(root_store, CERT_EMPTY_USAGE, NULL, 0, 1));
	CHECK(add_certificate(root_store, CERT_BOTH_USAGES, both, 2, 1));
	CHECK(add_certificate(root_store, CERT_EXTENSION_ONLY, NULL, 0, 0));
	CertCloseStore(disallowed_store, 0);
	CertCloseStore(root_store, 0);

	/* A cert.pem beside the program, for the file callback to point at. */
	make_path(beside, sizeof(beside), "cert.pem");
	CHECK(write_file(beside, pem_text, sizeof(pem_text) - 1U));
	CHECK(expected_certificate_path(expected, sizeof(expected)));

	/* A full run: the distrusted certificate comes through the first callback, the roots through the second. */
	roots_log_clear(&log);
	for (index = 0; index < CERT_COUNT; index++) {
		log.known[index] = cert_der[index];
		log.known_size[index] = cert_size[index];
	}

	/* Runs the trust sources. */
	error = cryc_os_system_roots(log_distrusted, log_der, log_file, &log);
	CHECK(error == 0);
	CHECK(log.order_broken == 0);
	CHECK(log.files == 1);
	CHECK(strcmp(log.file_path, expected) == 0);
	CHECK(log.hits[CERT_DISALLOWED] == 2);
	CHECK(log.hit_kind[CERT_DISALLOWED] == 1);
	CHECK(log.hits[CERT_PLAIN] == 1);
	CHECK(log.hit_kind[CERT_PLAIN] == 1);
	CHECK(log.hits[CERT_CLIENT_ONLY] == 0);
	CHECK(log.hits[CERT_EMPTY_USAGE] == 0);
	CHECK(log.hits[CERT_BOTH_USAGES] == 1);
	CHECK(log.hit_kind[CERT_BOTH_USAGES] == 1);
	CHECK(log.hits[CERT_EXTENSION_ONLY] == 1);
	CHECK(log.distrusted >= 1);
	CHECK(log.ders >= 4);

	/* The file the callback was given is the one beside the program, and it can be read. */
	error = cryc_os_read_file(log.file_path, 100, &data, &length);
	CHECK(error == 0);
	CHECK(length == sizeof(pem_text) - 1U);
	if (error == 0) {
		CHECK_BYTES(data, pem_text, length);
		free(data);
	}

	/* The distrusted callback failing ends the run before any root. */
	roots_log_clear(&log);
	log.stop_kind = 0;
	log.stop_error = ENOMEM;
	error = cryc_os_system_roots(log_distrusted, log_der, log_file, &log);
	CHECK(error == ENOMEM);
	CHECK(log.distrusted == 1);
	CHECK(log.ders == 0);
	CHECK(log.files == 0);

	/* The root callback failing ends the run before the file. */
	roots_log_clear(&log);
	log.stop_kind = 1;
	log.stop_error = EIO;
	error = cryc_os_system_roots(log_distrusted, log_der, log_file, &log);
	CHECK(error == EIO);
	CHECK(log.ders == 1);
	CHECK(log.files == 0);

	/* A missing file is not an error; any other error is passed back. */
	roots_log_clear(&log);
	log.file_error = ENOENT;
	error = cryc_os_system_roots(log_distrusted, log_der, log_file, &log);
	CHECK(error == 0);
	CHECK(log.files == 1);
	roots_log_clear(&log);
	log.file_error = EIO;
	error = cryc_os_system_roots(log_distrusted, log_der, log_file, &log);
	CHECK(error == EIO);
	CHECK(log.files == 1);

	/* Removes the test certificates again. */
	purge_store(disallowed_key);
	purge_store(root_key);
	roots_log_clear(&log);
	for (index = 0; index < CERT_COUNT; index++) {
		log.known[index] = cert_der[index];
		log.known_size[index] = cert_size[index];
	}

	/* Runs the trust sources again: none of the removed certificates comes back. */
	error = cryc_os_system_roots(log_distrusted, log_der, log_file, &log);
	CHECK(error == 0);
	for (index = 0; index < CERT_COUNT; index++)
		CHECK(log.hits[index] == 0);
}

#endif

/* Checks cryc_os_system_roots. */
static void
test_system_roots(void)
{
	struct roots_log log;
#ifndef _WIN32
	int error;
#endif

	/* A missing callback is refused, and nothing is called. */
	roots_log_clear(&log);
	CHECK(cryc_os_system_roots(NULL, log_der, log_file, &log) == EINVAL);
	CHECK(cryc_os_system_roots(log_distrusted, NULL, log_file, &log) == EINVAL);
	CHECK(cryc_os_system_roots(log_distrusted, log_der, NULL, &log) == EINVAL);
	CHECK(log.distrusted == 0 && log.ders == 0 && log.files == 0);

#ifdef _WIN32
	check_roots_windows();
#else
	/* The bundle's path is the only thing handed over, once. */
	roots_log_clear(&log);
	error = cryc_os_system_roots(log_distrusted, log_der, log_file, &log);
	CHECK(error == 0);
	CHECK(log.files == 1);
	CHECK(strcmp(log.file_path, CRYC_OS_DEFAULT_CA_FILE) == 0);
	CHECK(strcmp(log.file_path, "/etc/ssl/cert.pem") == 0);
	CHECK(log.distrusted == 0);
	CHECK(log.ders == 0);

	/* A bundle that is not there is no error; another error is passed back unchanged. */
	roots_log_clear(&log);
	log.file_error = ENOENT;
	error = cryc_os_system_roots(log_distrusted, log_der, log_file, &log);
	CHECK(error == 0);
	CHECK(log.files == 1);
	roots_log_clear(&log);
	log.file_error = EIO;
	error = cryc_os_system_roots(log_distrusted, log_der, log_file, &log);
	CHECK(error == EIO);
	roots_log_clear(&log);
	log.file_error = ENOMEM;
	error = cryc_os_system_roots(log_distrusted, log_der, log_file, &log);
	CHECK(error == ENOMEM);
	CHECK(log.files == 1);
#endif
}

/* Sets test_dir to the directory of the running program. */
static void
find_test_directory(
	const char *program)
{
	size_t length;
#ifdef _WIN32
	WCHAR path[1100];
	char utf8[3300];
	DWORD wide_length;
	int bytes;

	UNUSED_PARAMETER(program);
	wide_length = GetModuleFileNameW(NULL, path, 1100);
	bytes = 0;
	if (wide_length > 0 && wide_length < 1100)
		bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1, utf8, (int)sizeof(utf8), NULL, NULL);
	if (bytes <= 0) {
		strcpy(test_dir, ".");
		return;
	}

	/* Measures the path of the program. */
	length = strlen(utf8);
#else
	char utf8[DIRECTORY_ROOM];

	/* Falls back to the current directory when the program name does not fit. */
	length = strlen(program);
	if (length >= sizeof(utf8)) {
		strcpy(test_dir, ".");
		return;
	}

	/* Copies the program name and measures it. */
	strcpy(utf8, program);
	length = strlen(utf8);
#endif
	/* Cuts at the last separator; a name without one means the current directory. */
	while (length > 0U && utf8[length - 1U] != '/' && utf8[length - 1U] != '\\')
		length--;
	if (length == 0U || length >= DIRECTORY_ROOM) {
		strcpy(test_dir, ".");
		return;
	}

	/* Keeps the directory without its trailing separator. */
	memcpy(test_dir, utf8, length - 1U);
	test_dir[length - 1U] = 0;
}

int
main(
	int argc,
	char **argv)
{
#ifdef _WIN32
	WSADATA wsa;
	int status;
#endif

	UNUSED_PARAMETER(argc);

	/* Lines reach the log at once, even if a sanitizer ends the process before the C library flushes. */
	setvbuf(stdout, NULL, _IOLBF, 0);

	/* The test's files go beside the program; a closed peer must not kill the test with SIGPIPE. */
	find_test_directory(argv[0]);
#ifdef _WIN32
	/* Starts the socket library. */
	status = WSAStartup(MAKEWORD(2, 2), &wsa);
	if (status != 0) {
		printf("WSAStartup failed\n");
		return EXIT_FAILURE;
	}
#else
	signal(SIGPIPE, SIG_IGN);
#endif

	/* The thread value first, so that "nothing stored yet" is really the first thing the process does with it. */
	test_thread_value();
	test_random();
	test_time();
	test_mutex();
	test_atomic();
	test_read_file();
	check_socket_basic();
	check_socket_transfer();
	check_socket_errors();
#ifndef _WIN32
	check_socket_pipe();
	check_socket_interrupted();
#endif
	test_system_roots();

#ifdef _WIN32
	WSACleanup();
#endif

	/* The last line run-unit.sh reads. */
	return check_finish("test-os");
}
