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

/* The key of the current user's Disallowed store. */
static const WCHAR disallowed_key[] = L"Software\\Microsoft\\SystemCertificates\\Disallowed";

/* Prints the result line and returns the exit status. */
static int
finish(
	const char *why)
{
	if (why == NULL) {
		printf("win-store: PASS\n");
		return 0;
	}
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

	file = fopen(path, "rb");
	if (file == NULL)
		return 0;
	size = fread(data, 1, CERTIFICATE_MAX, file);
	fclose(file);
	if (size == CERTIFICATE_MAX)
		return 0;
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

	if (RegOpenKeyExW(HKEY_CURRENT_USER, disallowed_key, 0, KEY_ALL_ACCESS, &key) != ERROR_SUCCESS)
		return 0;
	store = CertOpenStore(CERT_STORE_PROV_REG, 0, 0, 0, key);
	if (store == NULL) {
		RegCloseKey(key);
		return -1;
	}
	wanted = CertCreateCertificateContext(X509_ASN_ENCODING, data, (DWORD)size);
	count = 0;
	found = NULL;
	while (wanted != NULL) {
		found = CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_EXISTING, wanted, found);
		if (found == NULL)
			break;
		count++;
		if (remove) {
			/* The delete frees the context passed in; the search starts again. */
			if (!CertDeleteCertificateFromStore(found)) {
				count = -1;
				break;
			}
			found = NULL;
		}
	}
	if (found != NULL)
		CertFreeCertificateContext(found);
	if (wanted != NULL)
		CertFreeCertificateContext(wanted);
	else
		count = -1;
	CertCloseStore(store, 0);
	RegCloseKey(key);
	return count;
}

int
main(
	int argc,
	char **argv)
{
	static unsigned char data[CERTIFICATE_MAX];
	HCERTSTORE store;
	size_t size;
	long count;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc != 3)
		return finish("usage: win-store add|remove|count certificate.der");
	size = read_file(argv[2], data);
	if (size == 0)
		return finish("cannot read the certificate");

	/* Adds through the logical system store of the current user (created when absent). */
	if (strcmp(argv[1], "add") == 0) {
		store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER, L"Disallowed");
		if (store == NULL)
			return finish("CertOpenStore");
		if (!CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, data, (DWORD)size,
		    CERT_STORE_ADD_REPLACE_EXISTING, NULL)) {
			CertCloseStore(store, 0);
			return finish("CertAddEncodedCertificateToStore");
		}
		CertCloseStore(store, 0);
		count = walk_registry(data, size, 0);
		printf("win-store: %ld copies in Disallowed\n", count);
		return finish(count >= 1 ? NULL : "not in the store after adding");
	}

	/* Removes every copy, or counts them. */
	if (strcmp(argv[1], "remove") == 0) {
		count = walk_registry(data, size, 1);
		printf("win-store: %ld copies removed from Disallowed\n", count);
		if (count < 0)
			return finish("removing");
		return finish(walk_registry(data, size, 0) == 0 ? NULL : "still in the store");
	}
	if (strcmp(argv[1], "count") == 0) {
		count = walk_registry(data, size, 0);
		printf("win-store: %ld copies in Disallowed\n", count);
		return finish(count >= 0 ? NULL : "counting");
	}
	return finish("usage: win-store add|remove|count certificate.der");
}
