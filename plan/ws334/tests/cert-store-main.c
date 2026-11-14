/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A small program for a check under Wine that puts a certificate into one
 * of the current user's system stores, or takes it out again:
 *   cert-store add|remove STORE DER
 * STORE is a store's name (Root, Disallowed); DER a certificate's file.
 * It prints one line, "cert-store: added", "cert-store: removed" or why it
 * failed, and the exit status is 0 when it did what it was asked (taking
 * out a certificate the store does not hold is done too).
 */

#include <windows.h>
#include <wincrypt.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* The largest certificate file read. */
#define DER_MAX		16384

/* The longest store's name. */
#define STORE_NAME_MAX	64

static int store_change(int add, HCERTSTORE store, const unsigned char *der, size_t length);

/*
 * Reads the arguments and the certificate, and changes the store.
 */
int
main(
	int argc,
	char **argv)
{
	unsigned char der[DER_MAX];
	wchar_t store_name[STORE_NAME_MAX];
	HCERTSTORE store;
	size_t length;
	FILE *file;
	int add;
	int status;
	int converted;
	int usable;
	int is_add;
	int is_remove;

	/* The arguments. */
	usable = 0;
	if (argc == 4) {
		is_add = strcmp(argv[1], "add");
		is_remove = strcmp(argv[1], "remove");
		if (is_add == 0 || is_remove == 0)
			usable = 1;
	}
	if (!usable) {
		fprintf(stderr, "usage: cert-store add|remove STORE DER\n");
		return 2;
	}
	add = strcmp(argv[1], "add") == 0;
	converted = MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, store_name, STORE_NAME_MAX);
	if (converted == 0) {
		printf("cert-store: the store's name %s is not usable\n", argv[2]);
		return 1;
	}

	/* The certificate. */
	file = fopen(argv[3], "rb");
	if (file == NULL) {
		printf("cert-store: cannot open %s\n", argv[3]);
		return 1;
	}
	length = fread(der, 1, sizeof(der), file);
	fclose(file);
	if (length == 0 || length == sizeof(der)) {
		printf("cert-store: cannot use %s\n", argv[3]);
		return 1;
	}

	/* The store, made when it does not exist yet. */
	store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER, store_name);
	if (store == NULL) {
		printf("cert-store: cannot open the store %s (error %lu)\n", argv[2], (unsigned long)GetLastError());
		return 1;
	}

	/* The change, written out now: Wine's registry-backed store shows it to other programs only after a commit. */
	status = store_change(add, store, der, length);
	if (status == 0)
		CertControlStore(store, 0, CERT_STORE_CTRL_COMMIT, NULL);
	CertCloseStore(store, 0);
	if (status == 0 && add)
		printf("cert-store: added\n");
	if (status == 0 && !add)
		printf("cert-store: removed\n");
	return status;
}

/*
 * Adds the certificate (der, length bytes) to the store, or takes it out
 * (add 0).  Returns 0 when done, else 1 after printing why.
 */
static int
store_change(
	int add,
	HCERTSTORE store,
	const unsigned char *der,
	size_t length)
{
	PCCERT_CONTEXT wanted;
	PCCERT_CONTEXT held;
	BOOL deleted;
	BOOL added;

	/* Adding replaces the same certificate when the store holds it. */
	if (add) {
		added = CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, der, (DWORD)length,
			CERT_STORE_ADD_REPLACE_EXISTING, NULL);
		if (!added) {
			printf("cert-store: cannot add the certificate (error %lu)\n", (unsigned long)GetLastError());
			return 1;
		}
		return 0;
	}

	/* Taking out: the store's copy of the same certificate, if it has one (the delete frees that context). */
	wanted = CertCreateCertificateContext(X509_ASN_ENCODING, der, (DWORD)length);
	if (wanted == NULL) {
		printf("cert-store: the file is not a certificate (error %lu)\n", (unsigned long)GetLastError());
		return 1;
	}
	held = CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_EXISTING, wanted, NULL);
	CertFreeCertificateContext(wanted);
	deleted = TRUE;
	if (held != NULL)
		deleted = CertDeleteCertificateFromStore(held);
	if (!deleted) {
		printf("cert-store: cannot take the certificate out (error %lu)\n", (unsigned long)GetLastError());
		return 1;
	}
	return 0;
}
