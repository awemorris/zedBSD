/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The system services of the operating-system layer on Windows.
 */

#include "base/os-system.h"
#include "base/os-win32.h"
#include "base/base.h"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <errno.h>
#include <limits.h>
#include <malloc.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* The largest alignment wb_os_aligned_alloc accepts, in bytes. */
#define OS_ALIGNMENT_MAX	65536U

/* The most bytes a thread name keeps (the limit of Linux's thread names, without the NUL). */
#define OS_THREAD_NAME_MAX	15U

/* The first size, in UTF-16 units, of the buffer that receives the path of the running program. */
#define OS_MODULE_PATH_START	260U

/* The largest size, in UTF-16 units, the buffer for the path of the running program grows to. */
#define OS_MODULE_PATH_LIMIT	65536U

/* The error CertEnumCertificatesInStore sets when the store has no more certificates. */
#define OS_NO_MORE_CERTIFICATES	((DWORD)0x80092004UL)

/* The suffix after the directory of the running program, where the bundled data lives. */
#define OS_DATA_SUFFIX		L"\\share"

/*
 * One system certificate store the trust enumeration reads.
 *
 * The enumeration walks a fixed list of these, so the order in which
 * certificates are handed over is the order of the list.
 */
struct os_trust_store {
	/* The system store's name. */
	const wchar_t *name;

	/* The location flag: the current user's stores or the machine's. */
	DWORD location;

	/* What the callback is told about each certificate of the store. */
	int distrusted;
};

/*
 * The system stores read for trust, in the order they are handed over.
 *
 * The trusted roots come first, the current user's and then the machine's,
 * and the stores of certificates never to be trusted follow, so that a
 * caller sees every root before the certificates that revoke trust.
 */
static const struct os_trust_store os_trust_stores[] = {
	{ L"ROOT", CERT_SYSTEM_STORE_CURRENT_USER, 0 },
	{ L"ROOT", CERT_SYSTEM_STORE_LOCAL_MACHINE, 0 },
	{ L"Disallowed", CERT_SYSTEM_STORE_CURRENT_USER, 1 },
	{ L"Disallowed", CERT_SYSTEM_STORE_LOCAL_MACHINE, 1 }
};

static int os_trust_store_read(const struct os_trust_store *description, wb_os_trust_each each, void *context);

/*
 * Allocates memory whose address is a multiple of alignment.
 *
 * The block is released with wb_os_aligned_free.
 */
int
wb_os_aligned_alloc(
	size_t alignment,
	size_t size,
	void **memory)
{
	void *block;

	/* Refuses a call that names no place for the block. */
	if (memory == NULL)
		return EINVAL;

	/* Leaves the output in its failure state until the allocation has worked. */
	*memory = NULL;

	/* Refuses an empty block. */
	if (size == 0)
		return EINVAL;

	/* Refuses an alignment below a pointer's or above the largest the layer offers. */
	if (alignment < sizeof(void *) || alignment > OS_ALIGNMENT_MAX)
		return EINVAL;

	/* Refuses an alignment that is not a power of two. */
	if ((alignment & (alignment - 1U)) != 0)
		return EINVAL;

	/* Allocates the block at the alignment. */
	block = _aligned_malloc(size, alignment);
	if (block == NULL)
		return ENOMEM;

	/* Hands the block to the caller. */
	*memory = block;

	/* Succeeded: the caller owns the block. */
	return 0;
}

/*
 * Frees a block from wb_os_aligned_alloc.
 */
void
wb_os_aligned_free(
	void *memory)
{
	/* Does nothing for no block. */
	if (memory == NULL)
		return;

	/* A block from _aligned_malloc is released with _aligned_free. */
	_aligned_free(memory);
}

/*
 * Fills bytes with random bytes from the system's preferred generator.
 *
 * The request is served in pieces no larger than the generator takes at once.
 */
int
wb_os_random(
	void *bytes,
	size_t length)
{
	unsigned char *cursor;
	size_t left;
	ULONG piece;
	NTSTATUS status;

	/* Does nothing for a request for no bytes, whatever the pointer is. */
	if (length == 0)
		return 0;

	/* Refuses a request for bytes with no place to put them. */
	if (bytes == NULL)
		return EINVAL;

	/* Fills the bytes a piece at a time. */
	cursor = (unsigned char *)bytes;
	left = length;
	while (left > 0) {
		piece = ULONG_MAX;
		if (left < (size_t)ULONG_MAX)
			piece = (ULONG)left;

		/* Asks the system's preferred generator for the piece. */
		status = BCryptGenRandom(NULL, cursor, piece, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
		if (status < 0)
			return EIO;

		cursor += piece;
		left -= piece;
	}

	/* Succeeded: every byte came from the system's generator. */
	return 0;
}

/*
 * Names the calling thread for debuggers and the system's tools.
 *
 * The facility exists from Windows 10 version 1607 and is looked up at run
 * time, so the program still starts on an older system.
 */
void
wb_os_thread_name(
	const char *name)
{
	HRESULT (WINAPI *set_description)(HANDLE, PCWSTR);
	FARPROC address;
	HMODULE kernel;
	wchar_t *wide;
	char kept[OS_THREAD_NAME_MAX + 1U];
	size_t length;
	int error;

	/* Ignores a call that gives no name. */
	if (name == NULL)
		return;

	/* Keeps at most the first bytes of the name, as the other systems do. */
	length = 0;
	while (length < OS_THREAD_NAME_MAX && name[length] != '\0') {
		kept[length] = name[length];
		length++;
	}
	kept[length] = '\0';

	/* Finds the system library that holds the facility, which is always loaded. */
	kernel = GetModuleHandleW(L"kernel32.dll");
	if (kernel == NULL)
		return;

	/* Looks the facility up; a system without it ignores the name. */
	address = GetProcAddress(kernel, "SetThreadDescription");
	if (address == NULL)
		return;

	/* Turns the address into the function's type by copying its bytes. */
	memcpy(&set_description, &address, sizeof(set_description));

	/* Converts the name to the UTF-16 the facility takes; a name that does not convert is dropped. */
	error = wb_os_wide_from_utf8(kept, &wide, NULL);
	if (error != 0)
		return;

	/* Names the calling thread; a failure leaves the thread unnamed, which is harmless. */
	(void)set_description(GetCurrentThread(), wide);
	free(wide);
}

/*
 * Loads the first library of a list that the system can load.
 *
 * Every name is checked before the first one is tried, so a bad list loads
 * nothing.  Only the application's directory and the system directory are
 * searched, never the current directory or PATH.
 */
int
wb_os_library_open(
	const char *const *names,
	struct wb_os_library **library)
{
	HMODULE module;
	wchar_t *wide;
	size_t i;
	int error;

	/* Leaves the output in its failure state until a library has loaded. */
	if (library != NULL)
		*library = NULL;

	/* Refuses a call that names no list or no place for the library. */
	if (names == NULL || library == NULL)
		return EINVAL;

	/* Refuses an empty list. */
	if (names[0] == NULL)
		return EINVAL;

	/* Refuses a list that holds an empty name. */
	for (i = 0; names[i] != NULL; i++) {
		if (names[i][0] == '\0')
			return EINVAL;
	}

	/* Tries the names in order and keeps the first library that loads. */
	for (i = 0; names[i] != NULL; i++) {
		/* Converts the name; a name that is not UTF-8 cannot name a file, so it is skipped. */
		error = wb_os_wide_from_utf8(names[i], &wide, NULL);
		if (error == EILSEQ || error == ENAMETOOLONG)
			continue;
		if (error != 0)
			return error;

		/* Loads the library from the application's directory or the system directory only. */
		module = LoadLibraryExW(wide, NULL, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
		free(wide);
		if (module != NULL) {
			*library = (struct wb_os_library *)module;
			return 0;
		}
	}

	/* No name in the list could be loaded. */
	return ENOENT;
}

/*
 * Looks up a function in a loaded library.
 */
wb_os_function
wb_os_library_symbol(
	struct wb_os_library *library,
	const char *name)
{
	FARPROC address;
	wb_os_function function;

	/* Finds nothing for a call that lacks the library or the name. */
	if (library == NULL || name == NULL)
		return NULL;

	/* Looks the name up in the library's exports. */
	address = GetProcAddress((HMODULE)library, name);
	if (address == NULL)
		return NULL;

	/* Turns the address into the layer's function type by copying its bytes. */
	memcpy(&function, &address, sizeof(function));

	/* The function the library exports. */
	return function;
}

/*
 * Unloads a library.
 */
void
wb_os_library_close(
	struct wb_os_library *library)
{
	/* Does nothing for no library. */
	if (library == NULL)
		return;

	/* Drops the library's reference; the system unloads it with its last user. */
	(void)FreeLibrary((HMODULE)library);
}

/*
 * Reports the value of navigator.platform.
 */
const char *
wb_os_platform_name(
	void)
{
	/* What browsers on Windows report on every processor. */
	return "Win32";
}

/*
 * Reports the system part of the User-Agent.
 */
const char *
wb_os_user_agent_system(
	void)
{
	/* What browsers on Windows send on every processor. */
	return "Windows NT 10.0; Win64; x64";
}

/*
 * Writes the directory of the bundled read-only data into buffer.
 *
 * It is the directory of the running program followed by "\share".
 */
int
wb_os_data_directory(
	char *buffer,
	size_t size)
{
	wchar_t *module_path;
	wchar_t *directory;
	char *text;
	size_t capacity;
	size_t length;
	size_t directory_length;
	size_t text_length;
	size_t suffix_length;
	DWORD got;
	int error;

	/* Refuses a call that names no buffer or a buffer of no bytes. */
	if (buffer == NULL || size == 0)
		return EINVAL;

	/* Asks for the path of the running program, with a larger buffer each time it did not fit. */
	module_path = NULL;
	capacity = OS_MODULE_PATH_START;
	for (;;) {
		module_path = (wchar_t *)malloc(capacity * sizeof(wchar_t));
		if (module_path == NULL)
			return ENOMEM;

		got = GetModuleFileNameW(NULL, module_path, (DWORD)capacity);
		if (got == 0) {
			free(module_path);
			return EIO;
		}

		/* A result shorter than the buffer is the whole path. */
		if (got < capacity)
			break;

		/* The path was cut to fit; tries again with twice the room, up to the limit. */
		free(module_path);
		if (capacity >= OS_MODULE_PATH_LIMIT)
			return EIO;
		capacity *= 2U;
	}
	length = got;

	/* Cuts the path at its last separator, which leaves the directory without a trailing one. */
	directory_length = length;
	while (directory_length > 0) {
		if (module_path[directory_length - 1U] == L'\\' || module_path[directory_length - 1U] == L'/')
			break;
		directory_length--;
	}
	if (directory_length > 0)
		directory_length--;

	/* Builds the directory followed by the data suffix in a new string. */
	suffix_length = wcslen(OS_DATA_SUFFIX);
	directory = (wchar_t *)malloc((directory_length + suffix_length + 1U) * sizeof(wchar_t));
	if (directory == NULL) {
		free(module_path);
		return ENOMEM;
	}
	wmemcpy(directory, module_path, directory_length);
	wmemcpy(directory + directory_length, OS_DATA_SUFFIX, suffix_length + 1U);
	free(module_path);

	/* Converts the path to UTF-8. */
	error = wb_os_utf8_from_wide(directory, (size_t)-1, &text, &text_length);
	free(directory);
	if (error == ENAMETOOLONG)
		return EIO;
	if (error != 0)
		return error;

	/* Reports a buffer too small for the path and its NUL, leaving an empty string. */
	if (text_length >= size) {
		buffer[0] = '\0';
		free(text);
		return ERANGE;
	}

	/* Copies the path with its NUL. */
	memcpy(buffer, text, text_length + 1U);
	free(text);

	/* Succeeded: the buffer holds the data directory. */
	return 0;
}

/*
 * Hands each certificate of the system's trust stores to each.
 *
 * A store the system does not have is skipped.
 */
int
wb_os_trust_roots(
	wb_os_trust_each each,
	void *context)
{
	size_t i;
	int error;

	/* Refuses a call that names no function to hand certificates to. */
	if (each == NULL)
		return EINVAL;

	/* Reads each store in order and stops at the first failure or stop request. */
	for (i = 0; i < sizeof(os_trust_stores) / sizeof(os_trust_stores[0]); i++) {
		error = os_trust_store_read(&os_trust_stores[i], each, context);
		if (error != 0)
			return error;
	}

	/* Succeeded: every certificate of every store that exists was handed over. */
	return 0;
}

/*
 * Hands the certificates of one system store to each.
 *
 * The store is opened read-only and only if it exists; one that cannot be
 * opened is treated as missing.  A certificate that is not X.509 ASN.1 is
 * skipped.
 */
static int
os_trust_store_read(
	const struct os_trust_store *description,
	wb_os_trust_each each,
	void *context)
{
	HCERTSTORE store;
	PCCERT_CONTEXT certificate;
	PCCERT_CONTEXT next;
	DWORD flags;
	DWORD last_error;
	int stop;
	int result;

	/* Opens the store read-only if it exists; a store that does not open is skipped. */
	flags = description->location | CERT_STORE_READONLY_FLAG | CERT_STORE_OPEN_EXISTING_FLAG;
	store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, flags, description->name);
	if (store == NULL)
		return 0;

	/* Walks the certificates; each call to the enumeration releases the previous one. */
	result = 0;
	certificate = NULL;
	for (;;) {
		SetLastError(0);
		next = CertEnumCertificatesInStore(store, certificate);
		if (next == NULL) {
			/* The end of the store sets "not found"; any other failure means the store could not be read. */
			last_error = GetLastError();
			if (last_error != 0 && last_error != OS_NO_MORE_CERTIFICATES)
				result = EIO;
			break;
		}
		certificate = next;

		/* Skips a certificate that is not X.509 ASN.1 or that holds no bytes. */
		if ((certificate->dwCertEncodingType & X509_ASN_ENCODING) == 0)
			continue;
		if (certificate->pbCertEncoded == NULL || certificate->cbCertEncoded == 0)
			continue;

		/* Hands the certificate's DER to the caller; a nonzero answer stops the walk. */
		stop = each(context, certificate->pbCertEncoded, (size_t)certificate->cbCertEncoded, description->distrusted);
		if (stop != 0) {
			CertFreeCertificateContext(certificate);
			result = stop;
			break;
		}
	}

	/* Closes the store. */
	CertCloseStore(store, 0);

	/* Reports why the walk ended: 0 at the end of the store, else the failure or the caller's stop value. */
	if (result != 0)
		return result;

	/* Succeeded: every certificate of the store was handed over. */
	return 0;
}
