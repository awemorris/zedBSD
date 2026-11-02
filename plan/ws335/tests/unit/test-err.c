/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the error queue (cryc-err.h and the ERR_ functions), RAND_bytes,
 * OPENSSL_cleanse and CRYPTO_memcmp.
 *
 * The expected codes are worked out by hand from the layout (library in
 * the bits from 23, reason below): library 20 and reason 134 give
 * 20 * 2^23 + 134 = 0x0A000000 + 0x86.  The queue is compared with a
 * reference queue kept in the test through a long pseudo-random sequence
 * of puts, gets, peeks and clears.  Every text is compared with the full
 * expected string, and with every cut of it.  Each thread has a queue of
 * its own (pthreads, or CreateThread under Windows).
 */

#include "check.h"

#include "userland/base/libcrypto-compat/cryc-err.h"
#include "userland/base/libcrypto-compat/cryc-os.h"

#include <compat/crypto/crypto.h>
#include <compat/crypto/err.h>
#include <compat/crypto/rand.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

/* The byte of the guard that surrounds a buffer a function must not overrun. */
#define GUARD_BYTE	0x5aU

/* The room of the guards before and after a buffer. */
#define GUARD_SIZE	32U

/* The threads that run one after another, each making a queue and ending (the leak of a queue shows up in the sanitizer). */
#define MANY_THREADS	1000U

/* The threads that run together. */
#define PARALLEL_THREADS	8U

/* A reference queue: the codes the real queue must hold, oldest first. */
struct model {
	unsigned long codes[CRYC_ERR_QUEUE];
	unsigned count;
};

/* What a test thread is given and gives back. */
struct thread_job {
	unsigned long own_code;
	unsigned long first_seen;
	unsigned long peeked_after_put;
	unsigned long got;
	unsigned long after_get;
	unsigned long other_seen;
	unsigned rounds;
	unsigned ok;
};

/* The reasons and texts a registration is tested with; the second entry has no text. */
static const unsigned test_reasons[] = { 134U, 276U, 294U, 1U };
static const char *const test_texts[] = { "certificate verify failed", "uninitialised", NULL, "x" };

/* A different table, which a later registration must not replace. */
static const unsigned other_reasons[] = { 134U };
static const char *const other_texts[] = { "replaced" };

/* Starts a function in a new thread and waits for it. */
#ifdef _WIN32
static DWORD WINAPI
thread_entry(
	LPVOID argument);
#else
static void *thread_entry(void *argument);
#endif
static void thread_run(struct thread_job *jobs, unsigned count);
static void thread_body(struct thread_job *job);
static void model_put(struct model *model, unsigned long code);
static unsigned long model_get(struct model *model);
static unsigned long next_random(unsigned long *state);
static void check_string(unsigned long code, const char *expected);
static void check_cuts(unsigned long code, const char *expected);
static void check_reason_scope(void);
static void test_pack(void);
static void test_queue_basic(void);
static void test_queue_model(void);
static void test_strings_unregistered(void);
static void test_registration(void);
static void test_threads(void);
static void test_rand(void);
static void test_cleanse(void);
static void test_memcmp(void);

#ifdef _WIN32
/* The thread function of Windows. */
static DWORD WINAPI
thread_entry(
	LPVOID argument)
{
	/* Runs the body. */
	thread_body((struct thread_job *)argument);

	/* Succeeded. */
	return 0;
}
#else
/* The thread function of POSIX. */
static void *
thread_entry(
	void *argument)
{
	/* Runs the body. */
	thread_body((struct thread_job *)argument);

	/* Succeeded. */
	return NULL;
}
#endif

/* Runs count threads together, each with its own job, and waits for all. */
static void
thread_run(
	struct thread_job *jobs,
	unsigned count)
{
	unsigned index;
#ifdef _WIN32
	HANDLE handles[PARALLEL_THREADS];
#else
	pthread_t handles[PARALLEL_THREADS];
#endif

	/* Starts the threads. */
	for (index = 0; index < count; index++) {
#ifdef _WIN32
		handles[index] = CreateThread(NULL, 0, thread_entry, &jobs[index], 0, NULL);
		CHECK(handles[index] != NULL);
#else
		CHECK(pthread_create(&handles[index], NULL, thread_entry, &jobs[index]) == 0);
#endif
	}

	/* Waits for them. */
	for (index = 0; index < count; index++) {
#ifdef _WIN32
		WaitForSingleObject(handles[index], INFINITE);
		CloseHandle(handles[index]);
#else
		pthread_join(handles[index], NULL);
#endif
	}
}

/*
 * What a thread does: sees an empty queue whatever the creator queued,
 * queues its own codes (rounds of them), reads them back oldest first.
 */
static void
thread_body(
	struct thread_job *job)
{
	unsigned long expected;
	unsigned index;

	/* A new thread's queue is empty, whatever other threads hold. */
	job->first_seen = ERR_get_error();
	job->ok = 1;

	/* Queues the thread's codes. */
	for (index = 0; index < job->rounds; index++)
		cryc_err_put(CRYC_LIB_SSL, (unsigned)(job->own_code + index));

	/* Looks at the code at the head of the queue, which is the thread's first. */
	job->peeked_after_put = ERR_peek_error();

	/* Reads the codes back and notes any that is not the one queued at that place. */
	for (index = 0; index < job->rounds; index++) {
		expected = cryc_err_pack(CRYC_LIB_SSL, (unsigned)(job->own_code + index));
		job->got = ERR_get_error();
		if (job->got != expected)
			job->ok = 0;
	}

	/* Looks at what is left once the thread's codes are drained: nothing of its own, nothing of the creator's. */
	job->after_get = ERR_get_error();
	job->other_seen = ERR_peek_error();
}

/* Appends code to the reference queue, dropping the oldest when it holds CRYC_ERR_QUEUE. */
static void
model_put(
	struct model *model,
	unsigned long code)
{
	unsigned index;

	/* Drops the oldest code when full. */
	if (model->count == CRYC_ERR_QUEUE) {
		for (index = 1; index < CRYC_ERR_QUEUE; index++)
			model->codes[index - 1U] = model->codes[index];
		model->count--;
	}

	/* Appends. */
	model->codes[model->count] = code;
	model->count++;
}

/* Removes and returns the oldest code of the reference queue, 0 when empty. */
static unsigned long
model_get(
	struct model *model)
{
	unsigned long code;
	unsigned index;

	/* Empty. */
	if (model->count == 0)
		return 0;

	/* Takes the first and shifts the rest. */
	code = model->codes[0];
	for (index = 1; index < model->count; index++)
		model->codes[index - 1U] = model->codes[index];
	model->count--;

	/* The oldest code. */
	return code;
}

/* A linear congruential generator for the test's choices (fixed seed, so a failure repeats). */
static unsigned long
next_random(
	unsigned long *state)
{
	/* The 32-bit generator of the C standard's example, kept to 32 bits. */
	*state = (*state * 1103515245UL + 12345UL) & 0xffffffffUL;

	/* The high bits are the better ones. */
	return (*state >> 8) & 0xffffffUL;
}

/* Checks the text of code against the whole expected string. */
static void
check_string(
	unsigned long code,
	const char *expected)
{
	char buffer[256];
	int difference;

	/* Writes into a large buffer. */
	memset(buffer, 'X', sizeof(buffer));
	ERR_error_string_n(code, buffer, sizeof(buffer));

	/* Compares the written text with the expected one. */
	difference = strcmp(buffer, expected);
	CHECK(difference == 0);

	/* Shows both texts when they differ. */
	if (difference != 0)
		printf("  got      %s\n  expected %s\n", buffer, expected);
}

/*
 * Checks every length of buffer from 0 to a few over the text: the output
 * is the expected text cut to length - 1 characters and a NUL, and the
 * guard bytes around the buffer are untouched.
 */
static void
check_cuts(
	unsigned long code,
	const char *expected)
{
	unsigned char area[GUARD_SIZE + 128U + GUARD_SIZE];
	size_t expected_length;
	size_t length;
	size_t cut;
	size_t index;
	int guards_good;

	/* The text must fit the test's area with room to spare. */
	expected_length = strlen(expected);
	CHECK(expected_length + 4U < 128U);

	/* Every length from 0 to 3 more than the text. */
	for (length = 0; length <= expected_length + 3U; length++) {
		memset(area, GUARD_BYTE, sizeof(area));
		memset(area + GUARD_SIZE, 0xee, 128U);
		ERR_error_string_n(code, (char *)(area + GUARD_SIZE), length);

		/* The guards before the buffer are intact. */
		guards_good = 1;
		for (index = 0; index < GUARD_SIZE; index++) {
			if (area[index] != GUARD_BYTE)
				guards_good = 0;
		}

		/* Nothing beyond length changed, and the guards after the buffer are intact. */
		for (index = GUARD_SIZE + length; index < sizeof(area); index++) {
			if (index < GUARD_SIZE + 128U) {
				if (area[index] != 0xee)
					guards_good = 0;
			} else if (area[index] != GUARD_BYTE) {
				guards_good = 0;
			}
		}

		/* Reports whether every guard and every byte beyond length held. */
		CHECK(guards_good);

		/* A zero length writes nothing. */
		if (length == 0)
			continue;

		/* The text is cut to length - 1 characters, then the NUL. */
		cut = expected_length;
		if (cut > length - 1U)
			cut = length - 1U;
		CHECK(memcmp(area + GUARD_SIZE, expected, cut) == 0);
		CHECK(area[GUARD_SIZE + cut] == 0);
	}
}

/*
 * Checks that every reason owned by a library reads as a number in each
 * other library and as text in its own, and that every common reason reads
 * as text in every library.
 */
static void
check_reason_scope(void)
{
	static const unsigned libraries[] = {
		1U, CRYC_LIB_SYS, CRYC_LIB_EVP, 9U, CRYC_LIB_X509, CRYC_LIB_ASN1, CRYC_LIB_CRYPTO, CRYC_LIB_SSL, CRYC_LIB_RAND, 77U
	};
	static const struct {
		unsigned library;
		unsigned reason;
	} owned[] = {
		{ CRYC_LIB_ASN1, CRYC_R_ASN1_WRONG_TAG },
		{ CRYC_LIB_ASN1, CRYC_R_ASN1_HEADER_TOO_LONG },
		{ CRYC_LIB_X509, CRYC_R_X509_INVALID_ATTRIBUTES },
		{ CRYC_LIB_EVP, CRYC_R_EVP_DIFFERENT_KEY_TYPES },
		{ CRYC_LIB_EVP, CRYC_R_EVP_INVALID_KEY_LENGTH },
		{ CRYC_LIB_EVP, CRYC_R_EVP_OPERATION_NOT_INITIALIZED },
		{ CRYC_LIB_EVP, CRYC_R_EVP_BUFFER_TOO_SMALL },
		{ CRYC_LIB_EVP, CRYC_R_EVP_UNSUPPORTED_ALGORITHM },
		{ CRYC_LIB_EVP, CRYC_R_EVP_INVALID_KEY }
	};
	static const unsigned common[] = {
		CRYC_R_MALLOC_FAILURE, CRYC_R_PASSED_NULL_PARAMETER, CRYC_R_INTERNAL_ERROR, CRYC_R_PASSED_INVALID_ARGUMENT, CRYC_R_UNSUPPORTED
	};
	char buffer[256];
	char number[32];
	size_t entry;
	size_t library;
	size_t length;
	size_t tail;
	int difference;
	int is_number;

	/* An owned reason is a number except in its own library. */
	for (entry = 0; entry < sizeof(owned) / sizeof(owned[0]); entry++) {
		for (library = 0; library < sizeof(libraries) / sizeof(libraries[0]); library++) {
			/* Writes the text of the code, and the form a bare number would take. */
			ERR_error_string_n(cryc_err_pack(libraries[library], owned[entry].reason), buffer, sizeof(buffer));
			sprintf(number, "::reason(%u)", owned[entry].reason);
			length = strlen(buffer);
			tail = strlen(number);

			/* Decides whether the text ends in the bare number form. */
			is_number = 0;
			if (length >= tail) {
				difference = strcmp(buffer + length - tail, number);
				if (difference == 0)
					is_number = 1;
			}

			/* A number is expected outside the reason's own library, a text inside it. */
			if (libraries[library] == owned[entry].library)
				CHECK(!is_number);
			else
				CHECK(is_number);
		}
	}

	/* A common reason is a text in every library, whatever the library. */
	for (entry = 0; entry < sizeof(common) / sizeof(common[0]); entry++) {
		for (library = 0; library < sizeof(libraries) / sizeof(libraries[0]); library++) {
			/* Writes the text of the code. */
			ERR_error_string_n(cryc_err_pack(libraries[library], common[entry]), buffer, sizeof(buffer));
			CHECK(strstr(buffer, "::reason(") == NULL);
		}
	}
}

/* The packing of a library and a reason, with the values worked out by hand. */
static void
test_pack(void)
{
	/* 20 * 2^23 = 0x0A000000, plus the reason 134 = 0x86. */
	CHECK(cryc_err_pack(CRYC_LIB_SSL, 134U) == 0x0A000086UL);
	CHECK(ERR_GET_LIB(0x0A000086UL) == 20);
	CHECK(ERR_GET_REASON(0x0A000086UL) == 134);

	/* Library 6 (EVP) and the reason 163: 6 * 2^23 = 0x03000000, plus 0xA3. */
	CHECK(cryc_err_pack(CRYC_LIB_EVP, CRYC_R_EVP_INVALID_KEY) == 0x030000A3UL);

	/* The common reason with its flag bits: library 15 = 0x07800000, reason 256 | 0xc0000 = 0xC0100. */
	CHECK(cryc_err_pack(CRYC_LIB_CRYPTO, CRYC_R_MALLOC_FAILURE) == 0x078C0100UL);
	CHECK(CRYC_R_MALLOC_FAILURE == 0xC0100U);
	CHECK(CRYC_R_PASSED_NULL_PARAMETER == 0xC0102U);
	CHECK(CRYC_R_INTERNAL_ERROR == 0xC0103U);
	CHECK(CRYC_R_PASSED_INVALID_ARGUMENT == 0x80106U);
	CHECK(CRYC_R_UNSUPPORTED == 0x8010CU);

	/* Library 36 (RAND) = 0x12000000; reason 0. */
	CHECK(cryc_err_pack(CRYC_LIB_RAND, 0U) == 0x12000000UL);

	/* The largest library (0xff) and the largest reason (0x7fffff) fill the 31 bits. */
	CHECK(cryc_err_pack(0xffU, 0x7fffffU) == 0x7fffffffUL);

	/* A library above 8 bits and a reason above 23 bits are cut to their fields. */
	CHECK(cryc_err_pack(0x114U, 5U) == (20UL << 23) + 5UL);
	CHECK(cryc_err_pack(1U, 0x80000005U) == (1UL << 23) + 5UL);
	CHECK(cryc_err_pack(0U, 0U) == 0UL);

	/* The library numbers are the ones of the public header. */
	CHECK(CRYC_LIB_SYS == (unsigned)ERR_LIB_SYS);
	CHECK(CRYC_LIB_EVP == (unsigned)ERR_LIB_EVP);
	CHECK(CRYC_LIB_X509 == (unsigned)ERR_LIB_X509);
	CHECK(CRYC_LIB_ASN1 == (unsigned)ERR_LIB_ASN1);
	CHECK(CRYC_LIB_CRYPTO == (unsigned)ERR_LIB_CRYPTO);
	CHECK(CRYC_LIB_SSL == (unsigned)ERR_LIB_SSL);
	CHECK(CRYC_LIB_RAND == (unsigned)ERR_LIB_RAND);
	CHECK(CRYC_ERR_QUEUE == 16U);
}

/* The queue by hand: empty, order, peek, clear, the edge of being full. */
static void
test_queue_basic(void)
{
	unsigned long code;
	unsigned index;
	unsigned total;

	/* A thread that has queued nothing sees an empty queue (the main thread, before anything). */
	CHECK(ERR_get_error() == 0);
	CHECK(ERR_peek_error() == 0);
	CHECK(cryc_err_get() == 0);
	CHECK(cryc_err_peek() == 0);

	/* Clearing an empty (or never made) queue is harmless. */
	ERR_clear_error();
	CHECK(ERR_get_error() == 0);

	/* One code: peek does not remove, get removes, then empty. */
	cryc_err_put(CRYC_LIB_SSL, 134U);
	CHECK(ERR_peek_error() == 0x0A000086UL);
	CHECK(ERR_peek_error() == 0x0A000086UL);
	CHECK(ERR_get_error() == 0x0A000086UL);
	CHECK(ERR_get_error() == 0);
	CHECK(ERR_peek_error() == 0);

	/* Two codes come out oldest first, and the peek follows. */
	cryc_err_put(CRYC_LIB_SSL, 1U);
	cryc_err_put(CRYC_LIB_EVP, 2U);
	CHECK(cryc_err_peek() == cryc_err_pack(CRYC_LIB_SSL, 1U));
	CHECK(cryc_err_get() == cryc_err_pack(CRYC_LIB_SSL, 1U));
	CHECK(cryc_err_peek() == cryc_err_pack(CRYC_LIB_EVP, 2U));
	CHECK(cryc_err_get() == cryc_err_pack(CRYC_LIB_EVP, 2U));
	CHECK(cryc_err_get() == 0);

	/* A cleared queue is empty and works again. */
	cryc_err_put(CRYC_LIB_SSL, 7U);
	cryc_err_put(CRYC_LIB_SSL, 8U);
	ERR_clear_error();
	CHECK(ERR_peek_error() == 0);
	CHECK(ERR_get_error() == 0);
	cryc_err_put(CRYC_LIB_SSL, 9U);
	CHECK(ERR_get_error() == cryc_err_pack(CRYC_LIB_SSL, 9U));

	/* Exactly full, then drained: 1 to 16 all stay. */
	for (index = 1; index <= CRYC_ERR_QUEUE; index++)
		cryc_err_put(CRYC_LIB_X509, index);
	for (index = 1; index <= CRYC_ERR_QUEUE; index++)
		CHECK(ERR_get_error() == cryc_err_pack(CRYC_LIB_X509, index));
	CHECK(ERR_get_error() == 0);

	/* One over: the oldest (1) is dropped, 2 to 17 stay. */
	for (index = 1; index <= CRYC_ERR_QUEUE + 1U; index++)
		cryc_err_put(CRYC_LIB_X509, index);
	CHECK(ERR_peek_error() == cryc_err_pack(CRYC_LIB_X509, 2U));
	for (index = 2; index <= CRYC_ERR_QUEUE + 1U; index++)
		CHECK(ERR_get_error() == cryc_err_pack(CRYC_LIB_X509, index));
	CHECK(ERR_get_error() == 0);

	/* Many over: the newest 16 stay, in order, whatever the lap of the ring. */
	for (total = 1; total <= 100U; total++) {
		ERR_clear_error();
		for (index = 1; index <= total; index++)
			cryc_err_put(CRYC_LIB_ASN1, index);
		if (total > CRYC_ERR_QUEUE)
			code = (unsigned long)(total - CRYC_ERR_QUEUE + 1U);
		else
			code = 1UL;
		for (index = (unsigned)code; index <= total; index++)
			CHECK(ERR_get_error() == cryc_err_pack(CRYC_LIB_ASN1, index));
		CHECK(ERR_get_error() == 0);
	}
}

/* The queue against a reference queue through a long sequence of operations. */
static void
test_queue_model(void)
{
	struct model model;
	unsigned long state;
	unsigned long choice;
	unsigned long expected;
	unsigned long got;
	unsigned step;
	unsigned library;
	unsigned reason;
	int mismatch;

	/* Starts both empty. */
	ERR_clear_error();
	model.count = 0;
	state = 12345UL;
	mismatch = 0;

	/* 200000 operations, the puts more often than the gets so that the queue is often full. */
	for (step = 0; step < 200000U; step++) {
		choice = next_random(&state) % 100UL;
		if (choice < 45UL) {
			/* Puts a random code in both queues. */
			library = (unsigned)(next_random(&state) % 256UL);
			reason = (unsigned)(next_random(&state) % 0x800000UL);
			cryc_err_put(library, reason);
			model_put(&model, cryc_err_pack(library, reason));
		} else if (choice < 80UL) {
			/* Takes the oldest code from both queues and compares them. */
			expected = model_get(&model);
			got = ERR_get_error();
			if (got != expected)
				mismatch++;
		} else if (choice < 97UL) {
			/* Looks at the oldest code of both queues and compares them. */
			expected = 0UL;
			if (model.count != 0)
				expected = model.codes[0];
			got = ERR_peek_error();
			if (got != expected)
				mismatch++;
		} else {
			/* Empties both queues. */
			ERR_clear_error();
			model.count = 0;
		}
	}

	/* Reports whether the queue ever differed from the reference. */
	CHECK(mismatch == 0);

	/* The rest drains the same. */
	while (model.count > 0) {
		expected = model_get(&model);
		got = ERR_get_error();
		CHECK(got == expected);
	}

	/* Reports that the queue ends empty. */
	CHECK(ERR_get_error() == 0);
}

/* The texts before any libssl-compat reason is registered. */
static void
test_strings_unregistered(void)
{
	unsigned long wide;

	/* The text of the code of the well-known certificate failure: no text for the reason yet. */
	check_string(0x0A000086UL, "error:0A000086:SSL routines::reason(134)");

	/* Every library name. */
	check_string(cryc_err_pack(1U, 5U), "error:00800005:unknown library::reason(5)");
	check_string(cryc_err_pack(CRYC_LIB_SYS, 5U), "error:01000005:system library::reason(5)");
	check_string(cryc_err_pack(CRYC_LIB_EVP, 5U), "error:03000005:digital envelope routines::reason(5)");
	check_string(cryc_err_pack(9U, 5U), "error:04800005:PEM routines::reason(5)");
	check_string(cryc_err_pack(CRYC_LIB_X509, 5U), "error:05800005:x509 certificate routines::reason(5)");
	check_string(cryc_err_pack(CRYC_LIB_ASN1, 5U), "error:06800005:asn1 encoding routines::reason(5)");
	check_string(cryc_err_pack(CRYC_LIB_CRYPTO, 5U), "error:07800005:common libcrypto routines::reason(5)");
	check_string(cryc_err_pack(CRYC_LIB_SSL, 5U), "error:0A000005:SSL routines::reason(5)");
	check_string(cryc_err_pack(CRYC_LIB_RAND, 5U), "error:12000005:random number generator::reason(5)");

	/* A library without a name, and the extremes of the numbers. */
	check_string(0UL, "error:00000000:lib(0)::reason(0)");
	check_string(cryc_err_pack(3U, 77U), "error:0180004D:lib(3)::reason(77)");
	check_string(0x7fffffffUL, "error:7FFFFFFF:lib(255)::reason(8388607)");
	check_string(cryc_err_pack(0xffU, 0U), "error:7F800000:lib(255)::reason(0)");

	/* A code wider than 32 bits prints all its digits (on a system with a 64-bit long). */
	if (sizeof(unsigned long) > 4U) {
		wide = 0x10000UL * 0x10000UL;
		check_string(wide, "error:100000000:lib(0)::reason(0)");
		wide = wide * 0x100UL + 0xABUL;
		check_string(wide, "error:100000000AB:lib(0)::reason(171)");
		wide = 0xffffffffffffffffUL & ((0x10000UL * 0x10000UL) * 0x10000UL * 0x10000UL - 1UL);
		check_string(wide, "error:FFFFFFFFFFFFFFFF:lib(255)::reason(8388607)");
	}

	/* The common reasons, in any library, with the flag bits in the number. */
	check_string(cryc_err_pack(CRYC_LIB_CRYPTO, CRYC_R_MALLOC_FAILURE), "error:078C0100:common libcrypto routines::out of memory");
	check_string(cryc_err_pack(CRYC_LIB_SSL, CRYC_R_MALLOC_FAILURE), "error:0A0C0100:SSL routines::out of memory");
	check_string(cryc_err_pack(CRYC_LIB_RAND, CRYC_R_PASSED_NULL_PARAMETER), "error:120C0102:random number generator::a required argument is NULL");
	check_string(cryc_err_pack(CRYC_LIB_RAND, CRYC_R_INTERNAL_ERROR), "error:120C0103:random number generator::internal error");
	check_string(cryc_err_pack(CRYC_LIB_RAND, CRYC_R_PASSED_INVALID_ARGUMENT), "error:12080106:random number generator::an argument is not valid");
	check_string(cryc_err_pack(CRYC_LIB_EVP, CRYC_R_UNSUPPORTED), "error:0308010C:digital envelope routines::not supported");

	/* The reasons of each library, in their own library. */
	check_string(cryc_err_pack(CRYC_LIB_ASN1, CRYC_R_ASN1_WRONG_TAG), "error:068000A8:asn1 encoding routines::unexpected ASN.1 tag");
	check_string(cryc_err_pack(CRYC_LIB_ASN1, CRYC_R_ASN1_HEADER_TOO_LONG), "error:0680007B:asn1 encoding routines::ASN.1 header too long");
	check_string(cryc_err_pack(CRYC_LIB_X509, CRYC_R_X509_INVALID_ATTRIBUTES), "error:0580008A:x509 certificate routines::invalid attributes");
	check_string(cryc_err_pack(CRYC_LIB_EVP, CRYC_R_EVP_DIFFERENT_KEY_TYPES), "error:03000065:digital envelope routines::the key types differ");
	check_string(cryc_err_pack(CRYC_LIB_EVP, CRYC_R_EVP_INVALID_KEY_LENGTH), "error:03000082:digital envelope routines::invalid key length");
	check_string(cryc_err_pack(CRYC_LIB_EVP, CRYC_R_EVP_OPERATION_NOT_INITIALIZED), "error:03000097:digital envelope routines::operation not initialized");
	check_string(cryc_err_pack(CRYC_LIB_EVP, CRYC_R_EVP_BUFFER_TOO_SMALL), "error:0300009B:digital envelope routines::buffer too small");
	check_string(cryc_err_pack(CRYC_LIB_EVP, CRYC_R_EVP_UNSUPPORTED_ALGORITHM), "error:0300009C:digital envelope routines::unsupported algorithm");
	check_string(cryc_err_pack(CRYC_LIB_EVP, CRYC_R_EVP_INVALID_KEY), "error:030000A3:digital envelope routines::invalid key");

	/* A reason of one library does not read as the text of another library's reason of the same number. */
	check_string(cryc_err_pack(CRYC_LIB_SSL, CRYC_R_ASN1_WRONG_TAG), "error:0A0000A8:SSL routines::reason(168)");
	check_string(cryc_err_pack(CRYC_LIB_EVP, CRYC_R_ASN1_WRONG_TAG), "error:030000A8:digital envelope routines::reason(168)");
	check_string(cryc_err_pack(CRYC_LIB_ASN1, CRYC_R_EVP_INVALID_KEY), "error:068000A3:asn1 encoding routines::reason(163)");
	check_string(cryc_err_pack(CRYC_LIB_X509, CRYC_R_EVP_INVALID_KEY_LENGTH), "error:05800082:x509 certificate routines::reason(130)");
	check_string(cryc_err_pack(CRYC_LIB_EVP, CRYC_R_X509_INVALID_ATTRIBUTES), "error:0300008A:digital envelope routines::reason(138)");

	/* A common reason's number without its flag bits is not the common reason. */
	check_string(cryc_err_pack(CRYC_LIB_CRYPTO, 256U), "error:07800100:common libcrypto routines::reason(256)");

	/* Every hex digit, upper case, in the code. */
	check_string(0x000BCDEFUL, "error:000BCDEF:lib(0)::reason(773615)");
	check_string(0x00123456UL, "error:00123456:lib(0)::reason(1193046)");
	check_string(0x00789ABCUL, "error:00789ABC:lib(0)::reason(7903932)");
	check_string(0x000FEDCBUL, "error:000FEDCB:lib(0)::reason(1043915)");

	/* Each reason of a library has its text in that library alone; each common reason in every library. */
	check_reason_scope();

	/* Every cut of a short, a long and an unknown text. */
	check_cuts(0x0A000086UL, "error:0A000086:SSL routines::reason(134)");
	check_cuts(cryc_err_pack(CRYC_LIB_EVP, CRYC_R_EVP_OPERATION_NOT_INITIALIZED), "error:03000097:digital envelope routines::operation not initialized");
	check_cuts(0x7fffffffUL, "error:7FFFFFFF:lib(255)::reason(8388607)");
	check_cuts(0UL, "error:00000000:lib(0)::reason(0)");

	/* A NULL buffer is not written whatever the length. */
	ERR_error_string_n(0x0A000086UL, NULL, 0);
	ERR_error_string_n(0x0A000086UL, NULL, 10);
	CHECK(1);
}

/* The registration of libssl-compat's reasons, and the texts after it. */
static void
test_registration(void)
{
	/* Calls that give no table register nothing, and do not use up the registration. */
	cryc_err_register_ssl(NULL, test_texts, 4U);
	cryc_err_register_ssl(test_reasons, NULL, 4U);
	cryc_err_register_ssl(test_reasons, test_texts, 0U);
	check_string(0x0A000086UL, "error:0A000086:SSL routines::reason(134)");

	/* A real registration gives the texts of the SSL library. */
	cryc_err_register_ssl(test_reasons, test_texts, 4U);
	check_string(0x0A000086UL, "error:0A000086:SSL routines::certificate verify failed");
	check_string(cryc_err_pack(CRYC_LIB_SSL, 276U), "error:0A000114:SSL routines::uninitialised");
	check_string(cryc_err_pack(CRYC_LIB_SSL, 1U), "error:0A000001:SSL routines::x");

	/* A reason whose text is NULL reads as a number. */
	check_string(cryc_err_pack(CRYC_LIB_SSL, 294U), "error:0A000126:SSL routines::reason(294)");

	/* A reason not in the table reads as a number. */
	check_string(cryc_err_pack(CRYC_LIB_SSL, 135U), "error:0A000087:SSL routines::reason(135)");

	/* The texts belong to the SSL library alone. */
	check_string(cryc_err_pack(CRYC_LIB_X509, 134U), "error:05800086:x509 certificate routines::reason(134)");
	check_string(cryc_err_pack(CRYC_LIB_EVP, 276U), "error:03000114:digital envelope routines::reason(276)");
	check_string(cryc_err_pack(CRYC_LIB_CRYPTO, 1U), "error:07800001:common libcrypto routines::reason(1)");

	/* The built-in texts still hold, also in the SSL library. */
	check_string(cryc_err_pack(CRYC_LIB_SSL, CRYC_R_MALLOC_FAILURE), "error:0A0C0100:SSL routines::out of memory");

	/* Every cut of a registered text. */
	check_cuts(0x0A000086UL, "error:0A000086:SSL routines::certificate verify failed");

	/* The same table again, and another table, change nothing. */
	cryc_err_register_ssl(test_reasons, test_texts, 4U);
	cryc_err_register_ssl(other_reasons, other_texts, 1U);
	check_string(0x0A000086UL, "error:0A000086:SSL routines::certificate verify failed");
	check_string(cryc_err_pack(CRYC_LIB_SSL, 276U), "error:0A000114:SSL routines::uninitialised");
}

/* Each thread has a queue of its own; threads that end give their queue back. */
static void
test_threads(void)
{
	struct thread_job jobs[PARALLEL_THREADS];
	unsigned index;
	unsigned round;
	unsigned bad;

	/* The main thread holds two codes. */
	ERR_clear_error();
	cryc_err_put(CRYC_LIB_SSL, 1001U);
	cryc_err_put(CRYC_LIB_SSL, 1002U);

	/* A new thread sees an empty queue, queues and reads back its own codes, and the main thread's are not touched. */
	memset(jobs, 0, sizeof(jobs));
	jobs[0].own_code = 2000U;
	jobs[0].rounds = 3U;
	thread_run(jobs, 1U);
	CHECK(jobs[0].first_seen == 0);
	CHECK(jobs[0].ok == 1U);
	CHECK(jobs[0].peeked_after_put == cryc_err_pack(CRYC_LIB_SSL, 2000U));
	CHECK(jobs[0].after_get == 0);
	CHECK(jobs[0].other_seen == 0);
	CHECK(ERR_get_error() == cryc_err_pack(CRYC_LIB_SSL, 1001U));
	CHECK(ERR_get_error() == cryc_err_pack(CRYC_LIB_SSL, 1002U));
	CHECK(ERR_get_error() == 0);

	/* The main thread's codes are not seen by a thread that starts later, and a thread's are not seen by main. */
	cryc_err_put(CRYC_LIB_SSL, 1003U);
	memset(jobs, 0, sizeof(jobs));
	jobs[0].own_code = 3000U;
	jobs[0].rounds = 20U;
	thread_run(jobs, 1U);
	CHECK(jobs[0].first_seen == 0);
	CHECK(jobs[0].peeked_after_put == cryc_err_pack(CRYC_LIB_SSL, 3004U));
	CHECK(ERR_peek_error() == cryc_err_pack(CRYC_LIB_SSL, 1003U));
	ERR_clear_error();

	/* Eight threads at once, each with its own run of codes, none seeing another's. */
	cryc_err_put(CRYC_LIB_SSL, 1004U);
	memset(jobs, 0, sizeof(jobs));
	for (index = 0; index < PARALLEL_THREADS; index++) {
		jobs[index].own_code = 10000U + index * 100U;
		jobs[index].rounds = 5U + index;
	}

	/* Runs the eight threads together and waits for them. */
	thread_run(jobs, PARALLEL_THREADS);

	/* Counts the threads that saw a foreign code or lost one of their own. */
	bad = 0;
	for (index = 0; index < PARALLEL_THREADS; index++) {
		if (jobs[index].first_seen != 0 || jobs[index].ok != 1U || jobs[index].after_get != 0)
			bad++;
		if (jobs[index].other_seen != 0)
			bad++;
	}

	/* Reports that no thread was wrong and the main thread's code is untouched. */
	CHECK(bad == 0);
	CHECK(ERR_get_error() == cryc_err_pack(CRYC_LIB_SSL, 1004U));
	CHECK(ERR_get_error() == 0);

	/* A thousand threads one after another, each making a queue and ending (a leaked queue is a sanitizer failure). */
	bad = 0;
	for (round = 0; round < MANY_THREADS; round++) {
		memset(jobs, 0, sizeof(jobs[0]));
		jobs[0].own_code = round;
		jobs[0].rounds = 1U + (round % CRYC_ERR_QUEUE);
		thread_run(jobs, 1U);
		if (jobs[0].first_seen != 0 || jobs[0].ok != 1U || jobs[0].after_get != 0)
			bad++;
	}

	/* Reports that no short-lived thread was wrong. */
	CHECK(bad == 0);
}

/* RAND_bytes at many lengths, with guards, fills, and the error cases. */
static void
test_rand(void)
{
	static const unsigned char fills[4] = { 0x00U, 0xffU, 0x5aU, 0xa5U };
	static const int counts[] = {
		1, 2, 3, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 254, 255, 256, 257, 258, 300, 511, 512, 513,
		1000, 1024, 1025, 4096, 4097, 100000
	};
	unsigned char *areas[4];
	unsigned char histogram_source[65536];
	unsigned long histogram[256];
	unsigned char area[GUARD_SIZE + 8U + GUARD_SIZE];
	unsigned char first[64];
	unsigned char second[64];
	size_t which;
	size_t fill;
	size_t index;
	size_t bytes;
	unsigned long total;
	unsigned long low;
	unsigned long high;
	int count;
	int guards_good;
	int unwritten;
	int same_as_first;

	/* Zero bytes: success, and nothing is touched, also with a NULL buffer. */
	memset(area, GUARD_BYTE, sizeof(area));
	CHECK(RAND_bytes(area + GUARD_SIZE, 0) == 1);
	CHECK(RAND_bytes(NULL, 0) == 1);

	/* Checks that the whole area still holds the guard byte. */
	guards_good = 1;
	for (index = 0; index < sizeof(area); index++) {
		if (area[index] != GUARD_BYTE)
			guards_good = 0;
	}

	/* Reports whether the guards stayed intact. */
	CHECK(guards_good);

	/* Every length: guards intact; no byte position stays at the fill in all four fills. */
	for (which = 0; which < sizeof(counts) / sizeof(counts[0]); which++) {
		/* Has RAND_bytes overwrite four buffers that start with different fills. */
		count = counts[which];
		bytes = (size_t)count;
		for (fill = 0; fill < 4U; fill++) {
			areas[fill] = (unsigned char *)malloc(GUARD_SIZE + bytes + GUARD_SIZE);
			CHECK(areas[fill] != NULL);
			if (areas[fill] == NULL)
				return;
			memset(areas[fill], GUARD_BYTE, GUARD_SIZE + bytes + GUARD_SIZE);
			memset(areas[fill] + GUARD_SIZE, fills[fill], bytes);
			CHECK(RAND_bytes(areas[fill] + GUARD_SIZE, count) == 1);
		}

		/* Checks the guards around every buffer. */
		guards_good = 1;
		unwritten = 0;
		for (fill = 0; fill < 4U; fill++) {
			for (index = 0; index < GUARD_SIZE; index++) {
				if (areas[fill][index] != GUARD_BYTE || areas[fill][GUARD_SIZE + bytes + index] != GUARD_BYTE)
					guards_good = 0;
			}
		}

		/* Counts the byte positions that stayed at the fill in all four buffers. */
		for (index = 0; index < bytes; index++) {
			same_as_first = 1;
			for (fill = 0; fill < 4U; fill++) {
				if (areas[fill][GUARD_SIZE + index] != fills[fill])
					same_as_first = 0;
			}

			/* A position that matches every fill was not written. */
			if (same_as_first)
				unwritten++;
		}

		/* Reports that the guards held and every byte position was written. */
		CHECK(guards_good);
		CHECK(unwritten == 0);

		/* Gives the buffers back. */
		for (fill = 0; fill < 4U; fill++)
			free(areas[fill]);
	}

	/* Two calls give different bytes (64 bytes: the chance of equality is 2^-512). */
	CHECK(RAND_bytes(first, 64) == 1);
	CHECK(RAND_bytes(second, 64) == 1);
	CHECK(memcmp(first, second, 64U) != 0);

	/* A byte histogram of 65536 bytes: each value 256 times on average with a standard deviation of 16; the bounds are 6 sigma. */
	CHECK(RAND_bytes(histogram_source, 65536) == 1);

	/* Counts how often each byte value occurs. */
	memset(histogram, 0, sizeof(histogram));
	for (index = 0; index < sizeof(histogram_source); index++)
		histogram[histogram_source[index]]++;

	/* Finds the lowest and the highest count, and the total. */
	low = 65536UL;
	high = 0;
	total = 0;
	for (index = 0; index < 256U; index++) {
		if (histogram[index] < low)
			low = histogram[index];
		if (histogram[index] > high)
			high = histogram[index];
		total += histogram[index];
	}

	/* Reports that every byte was counted and no value strays far from the average. */
	CHECK(total == 65536UL);
	CHECK(low > 160UL);
	CHECK(high < 352UL);

	/* A negative count fails, leaves the buffer alone and queues an error of the RAND library. */
	ERR_clear_error();
	memset(first, 0x33, sizeof(first));
	CHECK(RAND_bytes(first, -1) == 0);
	CHECK(ERR_get_error() == cryc_err_pack(CRYC_LIB_RAND, CRYC_R_PASSED_INVALID_ARGUMENT));
	CHECK(ERR_get_error() == 0);
	CHECK(RAND_bytes(first, -2147483647 - 1) == 0);
	CHECK(first[0] == 0x33U && first[63] == 0x33U);
	ERR_clear_error();

	/* A NULL buffer with a count fails and queues an error. */
	CHECK(RAND_bytes(NULL, 1) == 0);
	CHECK(ERR_get_error() == cryc_err_pack(CRYC_LIB_RAND, CRYC_R_PASSED_NULL_PARAMETER));
	CHECK(ERR_get_error() == 0);

	/* A success queues nothing. */
	CHECK(RAND_bytes(first, 16) == 1);
	CHECK(ERR_peek_error() == 0);
}

/* OPENSSL_cleanse at many lengths, with guards. */
static void
test_cleanse(void)
{
	unsigned char area[GUARD_SIZE + 300U + GUARD_SIZE];
	volatile unsigned char secret[48];
	size_t length;
	size_t index;
	int zero_good;
	int guards_good;

	/* NULL with length 0 is allowed. */
	OPENSSL_cleanse(NULL, 0);

	/* A NULL with a length is a caller's mistake; it does not crash. */
	OPENSSL_cleanse(NULL, 10);
	CHECK(1);

	/* Every length from 0 to 300: the bytes become zero, everything around stays. */
	for (length = 0; length <= 300U; length++) {
		/* Fills the area with guards and the bytes under test with a pattern. */
		memset(area, GUARD_BYTE, sizeof(area));
		for (index = 0; index < length; index++)
			area[GUARD_SIZE + index] = (unsigned char)(0xa0U + index);

		/* Wipes the bytes under test. */
		OPENSSL_cleanse(area + GUARD_SIZE, length);

		/* Checks that every wiped byte is zero. */
		zero_good = 1;
		for (index = 0; index < length; index++) {
			if (area[GUARD_SIZE + index] != 0)
				zero_good = 0;
		}

		/* Checks the guards before the bytes. */
		guards_good = 1;
		for (index = 0; index < GUARD_SIZE; index++) {
			if (area[index] != GUARD_BYTE)
				guards_good = 0;
		}

		/* Checks the guards after the bytes. */
		for (index = GUARD_SIZE + length; index < sizeof(area); index++) {
			if (area[index] != GUARD_BYTE)
				guards_good = 0;
		}

		/* Reports whether the bytes are zero and the guards intact. */
		CHECK(zero_good);
		CHECK(guards_good);
	}

	/* A local array that is never read again is still wiped (read through a volatile). */
	for (index = 0; index < sizeof(secret); index++)
		secret[index] = (unsigned char)(index + 1U);

	/* Wipes the array. */
	OPENSSL_cleanse((void *)secret, sizeof(secret));

	/* Checks that every byte of the array is zero. */
	zero_good = 1;
	for (index = 0; index < sizeof(secret); index++) {
		if (secret[index] != 0)
			zero_good = 0;
	}

	/* Reports whether the array is wiped. */
	CHECK(zero_good);
}

/* CRYPTO_memcmp over lengths, positions and bit flips. */
static void
test_memcmp(void)
{
	unsigned char left_area[160];
	unsigned char right_area[160];
	unsigned char *left;
	unsigned char *right;
	size_t offset;
	size_t length;
	size_t position;
	unsigned step;
	unsigned bit;
	int bad_equal;
	int bad_different;
	int result;

	/* Length 0 is equal, also for NULL. */
	CHECK(CRYPTO_memcmp(NULL, NULL, 0) == 0);
	CHECK(CRYPTO_memcmp(left_area, right_area, 0) == 0);

	/* The same pointer, and a missing pointer with a length (not equal). */
	memset(left_area, 0x42, sizeof(left_area));
	CHECK(CRYPTO_memcmp(left_area, left_area, 100U) == 0);
	CHECK(CRYPTO_memcmp(NULL, left_area, 10U) != 0);
	CHECK(CRYPTO_memcmp(left_area, NULL, 10U) != 0);

	/* Lengths 1 to 129 at several alignments: equal buffers compare equal, a single flipped bit anywhere differs. */
	bad_equal = 0;
	bad_different = 0;
	for (offset = 0; offset < 4U; offset++) {
		left = left_area + offset;
		right = right_area + 3U - offset;
		for (length = 1; length <= 129U; length++) {
			/* Makes the two buffers equal. */
			for (position = 0; position < length; position++) {
				left[position] = (unsigned char)(position * 5U + length);
				right[position] = left[position];
			}

			/* Counts an equal pair that does not compare equal. */
			result = CRYPTO_memcmp(left, right, length);
			if (result != 0)
				bad_equal++;

			/* Flips each bit of every position of a short buffer, or of every seventh of a long one. */
			step = 7U;
			if (length < 40U)
				step = 1U;
			for (position = 0; position < length; position += step) {
				for (bit = 0; bit < 8U; bit++) {
					right[position] = (unsigned char)(right[position] ^ (1U << bit));
					result = CRYPTO_memcmp(left, right, length);
					if (result == 0)
						bad_different++;
					right[position] = left[position];
				}
			}

			/* The bytes after length are not compared: a difference there leaves the answer equal. */
			if (length < 129U) {
				right[length] = (unsigned char)(left[length] ^ 0xffU);

				/* Compares only the first length bytes. */
				result = CRYPTO_memcmp(left, right, length);
				if (result != 0)
					bad_equal++;
			}
		}
	}

	/* Reports that equal buffers compared equal and every flipped bit was seen. */
	CHECK(bad_equal == 0);
	CHECK(bad_different == 0);

	/* Bytes that differ only in the sign bit, and 0x00 against 0xff, and a difference in the last byte only. */
	left_area[0] = 0x80U;
	right_area[0] = 0x00U;
	CHECK(CRYPTO_memcmp(left_area, right_area, 1U) != 0);
	CHECK(CRYPTO_memcmp(right_area, left_area, 1U) != 0);
	left_area[0] = 0x00U;
	right_area[0] = 0xffU;
	CHECK(CRYPTO_memcmp(left_area, right_area, 1U) != 0);
	memset(left_area, 0x11, 64U);
	memset(right_area, 0x11, 64U);
	right_area[63] = 0x10U;
	CHECK(CRYPTO_memcmp(left_area, right_area, 64U) != 0);
	CHECK(CRYPTO_memcmp(left_area, right_area, 63U) == 0);

	/* Every byte different. */
	memset(left_area, 0x00, 100U);
	memset(right_area, 0xff, 100U);
	CHECK(CRYPTO_memcmp(left_area, right_area, 100U) != 0);
}

/* Runs the checks. */
int
main(void)
{
	/* The checks, in an order that keeps the registration after the unregistered texts. */
	test_pack();
	test_queue_basic();
	test_queue_model();
	test_strings_unregistered();
	test_registration();
	test_threads();
	test_rand();
	test_cleanse();
	test_memcmp();

	/* The last line run-unit.sh reads. */
	return check_finish("test-err");
}
