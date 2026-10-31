/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The unit test of the system services of the operating-system layer
 * (base/os-system.h): aligned memory, random bytes, thread names, loading a
 * library, the platform names, the data directory and the trusted
 * certificates.  It runs on POSIX and on Windows; argv[1] says which.
 *
 * The expected values come from the header's contracts, from the C library
 * and the Windows API used separately from the module, and from hand
 * calculation.
 */

#include "check.h"

#include "base/os-system.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#else
#include <sys/utsname.h>
#include "include/kern/os-name.h"
#endif

/* Whether this run is the Windows one (argv[1] is "win32"). */
static int is_win32;

/* Something for the failure checks to point an output at, so a function that skips the reset shows. */
static int sentinel_target;

/* The alignments the allocation test tries, from a pointer to the largest the layer offers. */
static const size_t good_alignments[] = { sizeof(void *), 16U, 64U, 4096U, 65536U };

/* The sizes the allocation test tries at each alignment. */
static const size_t good_sizes[] = { 1U, 7U, 100U, 5000U, 70000U };

/* Checks wb_os_aligned_alloc and wb_os_aligned_free. */
static void
test_aligned(
	void)
{
	static const size_t bad_alignments[] = {
		0U, 3U, sizeof(void *) / 2U, sizeof(void *) + 1U, 24U, 65537U, 131072U, 262144U
	};
	void *memory;
	unsigned char *bytes;
	size_t a;
	size_t s;
	size_t i;
	int error;
	int intact;

	/* Every good alignment and size gives an aligned block that holds all its bytes. */
	for (a = 0; a < sizeof(good_alignments) / sizeof(good_alignments[0]); a++) {
		for (s = 0; s < sizeof(good_sizes) / sizeof(good_sizes[0]); s++) {
			memory = NULL;
			error = wb_os_aligned_alloc(good_alignments[a], good_sizes[s], &memory);
			CHECK_EQ(error, 0);
			CHECK(memory != NULL);
			if (error != 0 || memory == NULL)
				continue;
			CHECK_EQ((uintptr_t)memory % good_alignments[a], 0);

			bytes = (unsigned char *)memory;
			for (i = 0; i < good_sizes[s]; i++)
				bytes[i] = (unsigned char)(i * 7U + a);
			intact = 1;
			for (i = 0; i < good_sizes[s]; i++) {
				if (bytes[i] != (unsigned char)(i * 7U + a))
					intact = 0;
			}
			CHECK(intact);
			wb_os_aligned_free(memory);
		}
	}

	/* An alignment out of range or not a power of two is EINVAL and leaves *memory NULL. */
	for (a = 0; a < sizeof(bad_alignments) / sizeof(bad_alignments[0]); a++) {
		memory = &sentinel_target;
		error = wb_os_aligned_alloc(bad_alignments[a], 64U, &memory);
		CHECK_EQ(error, EINVAL);
		CHECK(memory == NULL);
	}

	/* A size of 0 and a missing place for the block are EINVAL. */
	memory = &sentinel_target;
	error = wb_os_aligned_alloc(16U, 0U, &memory);
	CHECK_EQ(error, EINVAL);
	CHECK(memory == NULL);
	error = wb_os_aligned_alloc(16U, 64U, NULL);
	CHECK_EQ(error, EINVAL);

	/* A request no machine can serve is ENOMEM (the Windows allocator refuses it without aborting). */
	if (is_win32) {
		memory = &sentinel_target;
		error = wb_os_aligned_alloc(64U, (size_t)1 << 62, &memory);
		CHECK_EQ(error, ENOMEM);
		CHECK(memory == NULL);
	}

	/* Freeing NULL does nothing. */
	wb_os_aligned_free(NULL);
}

/* Counts the distinct byte values in a buffer. */
static unsigned
distinct_values(
	const unsigned char *bytes,
	size_t length)
{
	unsigned char seen[256];
	unsigned count;
	size_t i;

	memset(seen, 0, sizeof(seen));
	for (i = 0; i < length; i++)
		seen[bytes[i]] = 1;
	count = 0;
	for (i = 0; i < 256U; i++) {
		if (seen[i])
			count++;
	}

	return count;
}

/* Checks wb_os_random. */
static void
test_random(
	void)
{
	unsigned char *first;
	unsigned char *second;
	unsigned char small[17];
	size_t length;
	size_t i;
	int error;
	int all_zero;

	/* No bytes asked for is success whatever the pointer is; bytes asked for with no pointer is EINVAL. */
	error = wb_os_random(NULL, 0);
	CHECK_EQ(error, 0);
	error = wb_os_random(small, 0);
	CHECK_EQ(error, 0);
	error = wb_os_random(NULL, 1);
	CHECK_EQ(error, EINVAL);

	/* Small sizes work and do not write past the end. */
	for (length = 1; length <= 16U; length++) {
		memset(small, 0xa5, sizeof(small));
		error = wb_os_random(small, length);
		CHECK_EQ(error, 0);
		CHECK_EQ(small[length], 0xa5);
	}

	/* Two requests of 1 MiB are neither all zero, nor equal, and each holds every byte value. */
	length = (size_t)1 << 20;
	first = (unsigned char *)calloc(length, 1U);
	second = (unsigned char *)calloc(length, 1U);
	CHECK(first != NULL);
	CHECK(second != NULL);
	if (first == NULL || second == NULL) {
		free(first);
		free(second);
		return;
	}
	error = wb_os_random(first, length);
	CHECK_EQ(error, 0);
	error = wb_os_random(second, length);
	CHECK_EQ(error, 0);
	all_zero = 1;
	for (i = 0; i < length; i++) {
		if (first[i] != 0)
			all_zero = 0;
	}
	CHECK(all_zero == 0);
	all_zero = 1;
	for (i = 0; i < length; i++) {
		if (second[i] != 0)
			all_zero = 0;
	}
	CHECK(all_zero == 0);
	CHECK(memcmp(first, second, length) != 0);
	CHECK_EQ(distinct_values(first, length), 256);

	/* The last bytes of a request are filled too, not only the first ones. */
	CHECK(distinct_values(first + length - 4096U, 4096U) > 200U);
	CHECK(distinct_values(second + length - 4096U, 4096U) > 200U);
	CHECK(distinct_values(first, 4096U) > 200U);
	CHECK_EQ(distinct_values(second, length), 256);
	free(first);
	free(second);

	/* A request of several megabytes is filled in every part, so the reads it is served in join up. */
	length = ((size_t)3 << 20) + 5U;
	first = (unsigned char *)calloc(length, 1U);
	CHECK(first != NULL);
	if (first == NULL)
		return;
	error = wb_os_random(first, length);
	CHECK_EQ(error, 0);
	CHECK(distinct_values(first, 4096U) > 200U);
	CHECK(distinct_values(first + ((size_t)1 << 20) - 2048U, 4096U) > 200U);
	CHECK(distinct_values(first + ((size_t)2 << 20) - 2048U, 4096U) > 200U);
	CHECK(distinct_values(first + length - 4096U, 4096U) > 200U);
	CHECK(distinct_values(first + length - 5U, 5U) > 1U);
	free(first);
}

/* The thread the name test starts: it names itself with every kind of name. */
static void *
name_thread(
	void *argument)
{
	(void)argument;

	wb_os_thread_name(NULL);
	wb_os_thread_name("");
	wb_os_thread_name("browser-network");
	wb_os_thread_name("a-name-of-forty-characters-0123456789abcd");

	return NULL;
}

/* Checks wb_os_thread_name from the main thread and from a started thread (it must only not fail). */
static void
test_thread_name(
	void)
{
	pthread_t thread;
	void *result;
	int error;

	wb_os_thread_name(NULL);
	wb_os_thread_name("");
	wb_os_thread_name("browser-network");
	wb_os_thread_name("a-name-of-forty-characters-0123456789abcd");

	result = NULL;
	error = pthread_create(&thread, NULL, name_thread, NULL);
	CHECK_EQ(error, 0);
	if (error == 0) {
		error = pthread_join(thread, &result);
		CHECK_EQ(error, 0);
	}

	/* Reaching this line is the check: naming a thread returns nothing to compare. */
	CHECK(1);
}

/* Checks wb_os_library_open, wb_os_library_symbol and wb_os_library_close. */
static void
test_library(
	void)
{
	static const char *const posix_names[] = { "libzedbsd-no-such.so", "libm.so.6", NULL };
	static const char *const win32_names[] = { "no-such-ws334.dll", "kernel32.dll", NULL };
	static const char *const missing[] = { "libzedbsd-no-such.so", "no-such-ws334.dll", NULL };
	static const char *const nothing[] = { NULL };
	static const char *const blank[] = { "", NULL };
	struct wb_os_library *library;
	wb_os_function function;
	int error;

	/* The first name is missing and the second loads. */
	library = NULL;
	if (is_win32)
		error = wb_os_library_open(win32_names, &library);
	else
		error = wb_os_library_open(posix_names, &library);
	CHECK_EQ(error, 0);
	CHECK(library != NULL);
	if (library != NULL) {
		/* A symbol that exists is found and works. */
		if (is_win32) {
#ifdef _WIN32
			typedef ULONGLONG (WINAPI *tick_function)(void);
			tick_function tick;
			ULONGLONG before;
			ULONGLONG called;
			ULONGLONG after;

			function = wb_os_library_symbol(library, "GetTickCount64");
			CHECK(function != NULL);
			if (function != NULL) {
				tick = (tick_function)function;
				before = GetTickCount64();
				called = tick();
				after = GetTickCount64();
				CHECK(called >= before);
				CHECK(called <= after);
			}
#endif
		} else {
			double (*cosine)(double);

			function = wb_os_library_symbol(library, "cos");
			CHECK(function != NULL);
			if (function != NULL) {
				cosine = (double (*)(double))function;
				CHECK(cosine(0.0) == 1.0);
			}
		}

		/* A symbol that does not exist is NULL, and so is a call with a NULL argument. */
		function = wb_os_library_symbol(library, "ws334_no_such_symbol");
		CHECK(function == NULL);
		function = wb_os_library_symbol(library, NULL);
		CHECK(function == NULL);
		function = wb_os_library_symbol(NULL, "cos");
		CHECK(function == NULL);
		wb_os_library_close(library);
	}

	/* No name that loads is ENOENT and leaves *library NULL. */
	library = (struct wb_os_library *)&sentinel_target;
	error = wb_os_library_open(missing, &library);
	CHECK_EQ(error, ENOENT);
	CHECK(library == NULL);

	/* A bad list is EINVAL. */
	library = (struct wb_os_library *)&sentinel_target;
	error = wb_os_library_open(NULL, &library);
	CHECK_EQ(error, EINVAL);
	CHECK(library == NULL);
	library = (struct wb_os_library *)&sentinel_target;
	error = wb_os_library_open(nothing, &library);
	CHECK_EQ(error, EINVAL);
	CHECK(library == NULL);
	library = (struct wb_os_library *)&sentinel_target;
	error = wb_os_library_open(blank, &library);
	CHECK_EQ(error, EINVAL);
	CHECK(library == NULL);
	error = wb_os_library_open(missing, NULL);
	CHECK_EQ(error, EINVAL);

	/* Closing NULL does nothing. */
	wb_os_library_close(NULL);

#ifdef _WIN32
	/* A library that sits in the current directory only is not loaded: the search skips that directory. */
	{
		static const char *const planted[] = { "ws334-planted.dll", NULL };
		wchar_t system_directory[512];
		wchar_t source[600];
		UINT got;
		BOOL copied;

		got = GetSystemDirectoryW(system_directory, 512U);
		CHECK(got > 0 && got < 400U);
		if (got > 0 && got < 400U) {
			wcscpy(source, system_directory);
			wcscat(source, L"\\version.dll");
			copied = CopyFileW(source, L"ws334-planted.dll", FALSE);
			CHECK(copied);
			if (copied) {
				library = (struct wb_os_library *)&sentinel_target;
				error = wb_os_library_open(planted, &library);
				CHECK_EQ(error, ENOENT);
				CHECK(library == NULL);
			}
		}
	}
#endif
}

/* Checks wb_os_platform_name and wb_os_user_agent_system. */
static void
test_names(
	void)
{
#ifdef _WIN32
	CHECK(strcmp(wb_os_platform_name(), "Win32") == 0);
	CHECK(strcmp(wb_os_user_agent_system(), "Windows NT 10.0; Win64; x64") == 0);
#else
	struct utsname machine;
	char expected[128];
	int error;

	/* The platform name is the system's name and the machine's, joined by a space. */
	error = uname(&machine);
	CHECK_EQ(error, 0);
	snprintf(expected, sizeof(expected), "%s %s", ZEDBSD_OS_NAME, machine.machine);
	CHECK(strcmp(wb_os_platform_name(), expected) == 0);
	CHECK(strcmp(wb_os_platform_name(), "zedBSD x86_64") == 0);
	CHECK(strcmp(wb_os_user_agent_system(), "X11; Linux x86_64") == 0);
#endif
}

/* Checks the three small-buffer cases of wb_os_data_directory against a path the test knows. */
static void
check_data_directory(
	const char *expected)
{
	size_t length;
	char *buffer;
	int error;

	length = strlen(expected);

	/* A buffer of exactly the needed size holds the path and its NUL. */
	buffer = (char *)malloc(length + 1U);
	CHECK(buffer != NULL);
	if (buffer != NULL) {
		error = wb_os_data_directory(buffer, length + 1U);
		CHECK_EQ(error, 0);
		CHECK(strcmp(buffer, expected) == 0);
		free(buffer);
	}

	/* A buffer one byte short is ERANGE and holds an empty string. */
	buffer = (char *)malloc(length);
	CHECK(buffer != NULL);
	if (buffer != NULL) {
		memset(buffer, 'x', length);
		error = wb_os_data_directory(buffer, length);
		CHECK_EQ(error, ERANGE);
		CHECK_EQ(buffer[0], 0);
		free(buffer);
	}

	/* A buffer of one byte is ERANGE too. */
	buffer = (char *)malloc(1U);
	CHECK(buffer != NULL);
	if (buffer != NULL) {
		buffer[0] = 'x';
		error = wb_os_data_directory(buffer, 1U);
		CHECK_EQ(error, ERANGE);
		CHECK_EQ(buffer[0], 0);
		free(buffer);
	}

	/* A larger buffer works and the path has no trailing separator. */
	buffer = (char *)malloc(length + 100U);
	CHECK(buffer != NULL);
	if (buffer != NULL) {
		error = wb_os_data_directory(buffer, length + 100U);
		CHECK_EQ(error, 0);
		CHECK(strcmp(buffer, expected) == 0);
		CHECK(buffer[strlen(buffer) - 1U] != '/');
		CHECK(buffer[strlen(buffer) - 1U] != '\\');
		free(buffer);
	}
}

/* Checks wb_os_data_directory. */
static void
test_data_directory(
	void)
{
	char small[8];
	int error;
#ifdef _WIN32
	wchar_t module[32768];
	char expected[32768 * 3];
	size_t length;
	DWORD got;
	int converted;
#endif

	/* The arguments. */
	error = wb_os_data_directory(NULL, 8U);
	CHECK_EQ(error, EINVAL);
	error = wb_os_data_directory(small, 0U);
	CHECK_EQ(error, EINVAL);

#ifdef _WIN32
	/* The directory of the running program, found by the system's own call, followed by "\share". */
	got = GetModuleFileNameW(NULL, module, 32768U);
	CHECK(got > 0 && got < 32768U);
	if (got == 0 || got >= 32768U)
		return;
	length = got;
	while (length > 0 && module[length - 1U] != L'\\')
		length--;
	CHECK(length > 0);
	if (length == 0)
		return;
	module[length - 1U] = 0;
	wcscat(module, L"\\share");
	converted = WideCharToMultiByte(CP_UTF8, 0, module, -1, expected, (int)sizeof(expected), NULL, NULL);
	CHECK(converted > 0);
	if (converted > 0)
		check_data_directory(expected);
#else
	check_data_directory("/usr/share");
#endif
}

/*
 * What the certificate callback counts: how many certificates of each kind
 * it was given and whether anything it was given was wrong.
 */
struct trust_context {
	unsigned roots;
	unsigned disallowed;
	unsigned bad;
	unsigned calls;
	unsigned stop_at;
};

/* The callback: counts, checks each certificate, and stops at the call named by stop_at. */
static int
trust_each(
	void *context,
	const unsigned char *der,
	size_t length,
	int distrusted)
{
	struct trust_context *counts;

	counts = (struct trust_context *)context;
	counts->calls++;
	if (length < 1U || der[0] != 0x30)
		counts->bad++;
	if (distrusted != 0 && distrusted != 1)
		counts->bad++;
	if (distrusted == 0 && counts->disallowed != 0)
		counts->bad++;
	if (distrusted == 0)
		counts->roots++;
	else
		counts->disallowed++;
	if (counts->stop_at != 0 && counts->calls == counts->stop_at)
		return 77;

	return 0;
}

#ifdef _WIN32
/* Counts the certificates of one system store with the system's own calls, as an oracle; 0 when it does not exist. */
static unsigned
count_store(
	const wchar_t *name,
	DWORD location)
{
	HCERTSTORE store;
	PCCERT_CONTEXT certificate;
	PCCERT_CONTEXT next;
	unsigned count;

	store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
	    location | CERT_STORE_READONLY_FLAG | CERT_STORE_OPEN_EXISTING_FLAG, name);
	if (store == NULL)
		return 0;
	count = 0;
	certificate = NULL;
	for (;;) {
		next = CertEnumCertificatesInStore(store, certificate);
		if (next == NULL)
			break;
		certificate = next;
		if ((certificate->dwCertEncodingType & X509_ASN_ENCODING) != 0 && certificate->cbCertEncoded > 0)
			count++;
	}
	CertCloseStore(store, 0);

	return count;
}
#endif

/* Checks wb_os_trust_roots. */
static void
test_trust_roots(
	void)
{
	struct trust_context counts;
	int error;
#ifdef _WIN32
	unsigned total;
#endif

	/* No function to call is EINVAL. */
	error = wb_os_trust_roots(NULL, NULL);
	CHECK_EQ(error, EINVAL);

	/* Hand over everything. */
	memset(&counts, 0, sizeof(counts));
	error = wb_os_trust_roots(trust_each, &counts);
	CHECK_EQ(error, 0);
	CHECK_EQ(counts.bad, 0);

	if (!is_win32) {
		/* The POSIX system hands nothing over: OpenSSL reads the system's file itself. */
		CHECK_EQ(counts.calls, 0);
		memset(&counts, 0, sizeof(counts));
		counts.stop_at = 1;
		error = wb_os_trust_roots(trust_each, &counts);
		CHECK_EQ(error, 0);
		CHECK_EQ(counts.calls, 0);
		return;
	}

#ifdef _WIN32
	/* The counts match what the system's own enumeration finds in the stores. */
	CHECK_EQ(counts.roots, count_store(L"ROOT", CERT_SYSTEM_STORE_CURRENT_USER) +
	    count_store(L"ROOT", CERT_SYSTEM_STORE_LOCAL_MACHINE));
	CHECK_EQ(counts.disallowed, count_store(L"Disallowed", CERT_SYSTEM_STORE_CURRENT_USER) +
	    count_store(L"Disallowed", CERT_SYSTEM_STORE_LOCAL_MACHINE));
	total = counts.calls;
	printf("note: %u roots and %u disallowed certificates\n", counts.roots, counts.disallowed);

	/* A callback that answers 77 stops the walk at once and 77 is the result. */
	if (total >= 1U) {
		memset(&counts, 0, sizeof(counts));
		counts.stop_at = 1;
		error = wb_os_trust_roots(trust_each, &counts);
		CHECK_EQ(error, 77);
		CHECK_EQ(counts.calls, 1);
	} else {
		printf("note: no certificate in the stores, the stop check is skipped\n");
	}
	if (total >= 3U) {
		memset(&counts, 0, sizeof(counts));
		counts.stop_at = 3;
		error = wb_os_trust_roots(trust_each, &counts);
		CHECK_EQ(error, 77);
		CHECK_EQ(counts.calls, 3);
	}
#endif
}

/* Runs the checks. */
int
main(
	int argc,
	char **argv)
{
	/* The harness passes the system's name. */
	CHECK(argc == 2);
	if (argc != 2)
		return check_finish("os-system");
	is_win32 = strcmp(argv[1], "win32") == 0;
	CHECK(is_win32 || strcmp(argv[1], "posix") == 0);

	test_aligned();
	test_random();
	test_thread_name();
	test_library();
	test_names();
	test_data_directory();
	test_trust_roots();

	return check_finish("os-system");
}
