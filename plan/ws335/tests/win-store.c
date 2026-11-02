/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * win-store: puts a test certificate into, or takes it out of, the
 * current user's Disallowed store of Windows (or of a Wine prefix), for
 * the trust test of run-interop.sh --win32.
 *
 *   win-store add certificate.der       adds it (replacing a copy)
 *   win-store remove certificate.der    removes every copy of it
 *   win-store count certificate.der     prints how many copies there are
 *
 * The removal goes through the registry store of the same key: under
 * Wine a certificate found through the logical system store is reported
 * deleted but stays in the registry, while the registry store really
 * removes it.  The last line is "win-store: PASS" (exit 0) or
 * "win-store: FAIL <why>" (exit 1).
 */

#include <windows.h>
#include <wincrypt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The largest certificate the tool reads. */
#define CERTIFICATE_MAX	65536

/*
 * The key of the current user's Disallowed store.
 *
 * The registry store of the tool is opened on it, and it never changes.
 */
static const WCHAR disallowed_key[] = L"Software\\Microsoft\\SystemCertificates\\Disallowed";

/* Prints the result line and returns the exit status. */
static int finish(const char *why);

/* Reads the DER file into data; returns its size, or 0. */
static size_t read_file(const char *path, unsigned char *data);

/* Removes (remove 1) or counts (remove 0) the copies of the certificate in the registry store; returns the count or -1. */
static long walk_registry(const unsigned char *data, size_t size, int remove);

/* Runs the command named by the first argument on the certificate file named by the second. */
int
main(
	int argc,
	char **argv)
{
	static unsigned char data[CERTIFICATE_MAX];
	HCERTSTORE store;
	size_t size;
	long count;
	int status;
	int added;
	int cmp;
	const char *why;

	/* Makes the output unbuffered, so every line reaches the caller. */
	setvbuf(stdout, NULL, _IONBF, 0);

	/* Refuses a command line of the wrong shape. */
	if (argc != 3) {
		status = finish("usage: win-store add|remove|count certificate.der");
		return status;
	}

	/* Reads the certificate the command works on. */
	size = read_file(argv[2], data);
	if (size == 0) {
		status = finish("cannot read the certificate");
		return status;
	}

	/* Adds through the logical system store of the current user (created when absent). */
	cmp = strcmp(argv[1], "add");
	if (cmp == 0) {
		store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER, L"Disallowed");
		if (store == NULL) {
			status = finish("CertOpenStore");
			return status;
		}

		/* Adds the certificate, replacing a copy that is already there. */
		added = CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, data, (DWORD)size,
		    CERT_STORE_ADD_REPLACE_EXISTING, NULL);
		if (!added) {
			CertCloseStore(store, 0);
			status = finish("CertAddEncodedCertificateToStore");
			return status;
		}

		/* Closes the store. */
		CertCloseStore(store, 0);

		/* Counts the copies in the registry store, which must now hold one. */
		count = walk_registry(data, size, 0);
		printf("win-store: %ld copies in Disallowed\n", count);
		why = NULL;
		if (count < 1)
			why = "not in the store after adding";

		/* Reports whether the certificate is in the store. */
		status = finish(why);
		return status;
	}

	/* Removes every copy, then checks that none is left. */
	cmp = strcmp(argv[1], "remove");
	if (cmp == 0) {
		count = walk_registry(data, size, 1);
		printf("win-store: %ld copies removed from Disallowed\n", count);
		if (count < 0) {
			status = finish("removing");
			return status;
		}

		/* Counts what is left, which must be nothing. */
		count = walk_registry(data, size, 0);
		why = NULL;
		if (count != 0)
			why = "still in the store";

		/* Reports whether the certificate is gone. */
		status = finish(why);
		return status;
	}

	/* Counts the copies. */
	cmp = strcmp(argv[1], "count");
	if (cmp == 0) {
		count = walk_registry(data, size, 0);
		printf("win-store: %ld copies in Disallowed\n", count);
		why = NULL;
		if (count < 0)
			why = "counting";

		/* Reports whether the count could be made. */
		status = finish(why);
		return status;
	}

	/* Refuses a command that is not known. */
	status = finish("usage: win-store add|remove|count certificate.der");
	return status;
}

/* Prints the result line and returns the exit status. */
static int
finish(
	const char *why)
{
	/* Reports the pass. */
	if (why == NULL) {
		printf("win-store: PASS\n");
		return 0;
	}

	/* Reports the failure and why. */
	printf("win-store: FAIL %s\n", why);
	return 1;
}

/* Reads the DER file into data; returns its size, or 0. */
static size_t
read_file(
	const char *path,
	unsigned char *data)
{
	FILE *file;
	size_t size;

	/* Opens the file. */
	file = fopen(path, "rb");
	if (file == NULL)
		return 0;

	/* Reads the file, then closes it. */
	size = fread(data, 1, CERTIFICATE_MAX, file);
	fclose(file);

	/* Refuses a file that fills the buffer, which may be cut. */
	if (size == CERTIFICATE_MAX)
		return 0;

	/* The number of bytes read. */
	return size;
}

/* Removes (remove 1) or counts (remove 0) the copies of the certificate in the registry store; returns the count or -1. */
static long
walk_registry(
	const unsigned char *data,
	size_t size,
	int remove)
{
	HKEY key;
	HCERTSTORE store;
	PCCERT_CONTEXT wanted;
	PCCERT_CONTEXT found;
	long count;
	LONG opened;
	BOOL deleted;

	/* Opens the registry key of the store; no key means no copy. */
	opened = RegOpenKeyExW(HKEY_CURRENT_USER, disallowed_key, 0, KEY_ALL_ACCESS, &key);
	if (opened != ERROR_SUCCESS)
		return 0;

	/* Opens the registry store on the key. */
	store = CertOpenStore(CERT_STORE_PROV_REG, 0, 0, 0, key);
	if (store == NULL) {
		RegCloseKey(key);
		return -1;
	}

	/* Makes the certificate to look for, then finds each copy of it in the store. */
	wanted = CertCreateCertificateContext(X509_ASN_ENCODING, data, (DWORD)size);
	count = 0;
	found = NULL;
	while (wanted != NULL) {
		found = CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_EXISTING, wanted, found);
		if (found == NULL)
			break;

		/* Counts the copy. */
		count++;

		/* Deletes the copy when asked to. */
		if (remove) {
			/* The delete frees the context passed in; the search starts again. */
			deleted = CertDeleteCertificateFromStore(found);
			if (!deleted) {
				count = -1;
				break;
			}

			/* Starts the search again from the beginning. */
			found = NULL;
		}
	}

	/* Releases the context left from the search. */
	if (found != NULL)
		CertFreeCertificateContext(found);

	/* Releases the certificate looked for, or reports that it could not be made. */
	if (wanted != NULL) {
		CertFreeCertificateContext(wanted);
	} else {
		count = -1;
	}

	/* Closes the store and the key. */
	CertCloseStore(store, 0);
	RegCloseKey(key);

	/* The number of copies, or -1 on a failure. */
	return count;
}
