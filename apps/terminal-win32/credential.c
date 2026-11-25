/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The passphrases of the SSH keys mikuterm keeps for the user's Windows
 * session, in Windows' Credential Manager: generic credentials with
 * CRED_PERSIST_SESSION, which Windows holds in memory for the logon
 * session and drops when the user signs out (nothing is written to the
 * disk).  Each is named "mikuterm:passphrase:" and the key file's full
 * path (GetFullPathNameW), so the same file in the same folder finds it
 * again and nothing else of the user's is touched; its secret is the
 * passphrase's UTF-8 bytes.  Every copy of a passphrase made here is
 * wiped (SecureZeroMemory) before its memory goes.
 */

#include "mikuterm.h"

#include <windows.h>
#include <wincred.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* The start of the name of every credential mikuterm keeps. */
#define CREDENTIAL_PREFIX	L"mikuterm:passphrase:"

/* The filter that lists them (the prefix and a star). */
#define CREDENTIAL_FILTER	L"mikuterm:passphrase:*"

/* The most UTF-16 units of a key file's path and of a credential's name. */
#define CREDENTIAL_WIDE_MAX	32768U

/* What Credential Manager shows as the credential's user and comment. */
#define CREDENTIAL_USER		L"mikuterm"
#define CREDENTIAL_COMMENT	L"The passphrase of an SSH key, kept by mikuterm until you sign out"

static int credential_name(const char *path, wchar_t **name);

/*
 * Finds the passphrase kept for the key file at path and copies it, with
 * its NUL, into passphrase (size bytes).  Returns 0, ENOENT (none is
 * kept), ENOBUFS (it does not fit; nothing is copied), ENOMEM or EIO.
 */
int
mikuterm_passphrase_recall(
	const char *path,
	char *passphrase,
	size_t size)
{
	PCREDENTIALW credential;
	wchar_t *name;
	BOOL found;
	DWORD error;
	int failed;

	/* The credential's name. */
	failed = credential_name(path, &name);
	if (failed != 0)
		return failed;

	/* The credential, if Windows keeps one of that name. */
	credential = NULL;
	found = CredReadW(name, CRED_TYPE_GENERIC, 0U, &credential);
	error = GetLastError();
	free(name);
	if (!found) {
		if (error == ERROR_NOT_FOUND)
			return ENOENT;
		return EIO;
	}

	/* Refuses a secret that does not fit with its NUL. */
	if ((size_t)credential->CredentialBlobSize + 1U > size) {
		SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
		CredFree(credential);
		return ENOBUFS;
	}

	/* Copies the passphrase and wipes Windows' copy before it is freed. */
	memcpy(passphrase, credential->CredentialBlob, credential->CredentialBlobSize);
	passphrase[credential->CredentialBlobSize] = '\0';
	SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
	CredFree(credential);

	/* Succeeded: passphrase holds the kept passphrase. */
	return 0;
}

/*
 * Keeps passphrase (NUL-terminated) for the key file at path until the
 * user signs out of Windows, replacing one kept before.  Returns 0,
 * EINVAL (empty, or longer than a credential holds), ENOMEM or EIO.
 */
int
mikuterm_passphrase_keep(
	const char *path,
	const char *passphrase)
{
	CREDENTIALW credential;
	wchar_t *name;
	size_t length;
	BOOL written;
	int failed;

	/* Refuses an empty passphrase and one Credential Manager cannot hold. */
	length = strlen(passphrase);
	if (length == 0U || length > CRED_MAX_CREDENTIAL_BLOB_SIZE)
		return EINVAL;

	/* The credential's name. */
	failed = credential_name(path, &name);
	if (failed != 0)
		return failed;

	/* A generic credential for the logon session only, whose secret is the passphrase. */
	memset(&credential, 0, sizeof(credential));
	credential.Type = CRED_TYPE_GENERIC;
	credential.TargetName = name;
	credential.Comment = (LPWSTR)CREDENTIAL_COMMENT;
	credential.CredentialBlobSize = (DWORD)length;
	credential.CredentialBlob = (LPBYTE)passphrase;
	credential.Persist = CRED_PERSIST_SESSION;
	credential.UserName = (LPWSTR)CREDENTIAL_USER;
	written = CredWriteW(&credential, 0U);
	free(name);
	if (!written)
		return EIO;

	/* Succeeded: Windows keeps the passphrase until the user signs out. */
	return 0;
}

/*
 * Forgets the passphrase kept for the key file at path; one that is not
 * kept is no error.
 */
void
mikuterm_passphrase_forget(
	const char *path)
{
	wchar_t *name;
	int failed;

	/* The credential's name. */
	failed = credential_name(path, &name);
	if (failed != 0)
		return;

	/* Deletes it, if there. */
	(void)CredDeleteW(name, CRED_TYPE_GENERIC, 0U);
	free(name);
}

/*
 * Forgets every passphrase mikuterm keeps (the credentials whose name
 * starts with mikuterm's prefix; the user's others are not touched).
 * Returns how many were forgotten.
 */
unsigned
mikuterm_passphrases_forget_all(void)
{
	PCREDENTIALW *credentials;
	DWORD count;
	DWORD index;
	BOOL listed;
	BOOL deleted;
	unsigned forgotten;

	/* The ones whose name has the prefix. */
	credentials = NULL;
	count = 0U;
	listed = CredEnumerateW(CREDENTIAL_FILTER, 0U, &count, &credentials);
	if (!listed)
		return 0U;

	/* Deletes each, wiping the copy of its secret the list holds. */
	forgotten = 0U;
	for (index = 0U; index < count; index++) {
		/* The list's copy of the passphrase goes first. */
		SecureZeroMemory(credentials[index]->CredentialBlob, credentials[index]->CredentialBlobSize);

		/* Then the credential itself. */
		deleted = CredDeleteW(credentials[index]->TargetName, CRED_TYPE_GENERIC, 0U);
		if (deleted)
			forgotten++;
	}

	/* The list is Windows' memory. */
	CredFree(credentials);

	/* How many went. */
	return forgotten;
}

/*
 * Returns the words of the line that says how long a passphrase is kept,
 * shown after the box that chooses whether to keep it.
 */
static int
rdp_credential_name(
	const char *host,
	uint16_t port,
	const char *user,
	wchar_t **name)
{
	char utf8[512];
	int units;
	wchar_t *wide;
	uint16_t p;

	*name = NULL;
	if (host == NULL || host[0] == '\0')
		return EINVAL;
	p = port != 0U ? port : 3389U;
	if (user != NULL && user[0] != '\0')
		(void)snprintf(utf8, sizeof(utf8), "mikuterm:rdp:%s@%s:%u", user, host, (unsigned)p);
	else
		(void)snprintf(utf8, sizeof(utf8), "mikuterm:rdp:%s:%u", host, (unsigned)p);
	units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
	if (units <= 0)
		return EINVAL;
	wide = malloc((size_t)units * sizeof(*wide));
	if (wide == NULL)
		return ENOMEM;
	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, wide, units) <= 0) {
		free(wide);
		return EINVAL;
	}
	*name = wide;
	return 0;
}

int
mikuterm_rdp_password_recall(
	const char *host,
	uint16_t port,
	const char *user,
	char *password,
	size_t size)
{
	PCREDENTIALW credential;
	wchar_t *name;
	BOOL found;
	DWORD error;
	int failed;

	failed = rdp_credential_name(host, port, user, &name);
	if (failed != 0)
		return failed;
	credential = NULL;
	found = CredReadW(name, CRED_TYPE_GENERIC, 0U, &credential);
	error = GetLastError();
	free(name);
	if (!found) {
		if (error == ERROR_NOT_FOUND)
			return ENOENT;
		return EIO;
	}
	if ((size_t)credential->CredentialBlobSize + 1U > size) {
		SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
		CredFree(credential);
		return ENOBUFS;
	}
	memcpy(password, credential->CredentialBlob, credential->CredentialBlobSize);
	password[credential->CredentialBlobSize] = '\0';
	SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
	CredFree(credential);
	return 0;
}

int
mikuterm_rdp_password_keep(
	const char *host,
	uint16_t port,
	const char *user,
	const char *password)
{
	CREDENTIALW credential;
	wchar_t *name;
	size_t length;
	BOOL written;
	int failed;

	if (password == NULL)
		return EINVAL;
	length = strlen(password);
	if (length == 0U || length > CRED_MAX_CREDENTIAL_BLOB_SIZE)
		return EINVAL;
	failed = rdp_credential_name(host, port, user, &name);
	if (failed != 0)
		return failed;
	memset(&credential, 0, sizeof(credential));
	credential.Type = CRED_TYPE_GENERIC;
	credential.TargetName = name;
	credential.Comment = L"RDP password saved by mikuterm";
	credential.CredentialBlobSize = (DWORD)length;
	credential.CredentialBlob = (LPBYTE)password;
	credential.Persist = CRED_PERSIST_ENTERPRISE;
	credential.UserName = (LPWSTR)CREDENTIAL_USER;
	written = CredWriteW(&credential, 0U);
	if (!written) {
		credential.Persist = CRED_PERSIST_SESSION;
		written = CredWriteW(&credential, 0U);
	}
	free(name);
	if (!written)
		return EIO;
	return 0;
}

void
mikuterm_rdp_password_forget(
	const char *host,
	uint16_t port,
	const char *user)
{
	wchar_t *name;

	if (rdp_credential_name(host, port, user, &name) != 0)
		return;
	(void)CredDeleteW(name, CRED_TYPE_GENERIC, 0U);
	free(name);
}

const char *
mikuterm_passphrase_keep_text(void)
{
	/* How long this system keeps a passphrase. */
	return "Remember the passphrase until you sign out of Windows (Tab switches this)\r\n";
}

/*
 * Makes the credential's name for the key file at path (UTF-8): the
 * prefix and the file's full path, in memory the caller frees.  Returns 0,
 * EINVAL, ENOMEM or EIO.
 */
static int
credential_name(
	const char *path,
	wchar_t **name)
{
	wchar_t *wide;
	wchar_t *full;
	size_t prefix;
	int units;
	DWORD length;

	/* Nothing made yet. */
	*name = NULL;

	/* Room for the path and the full path. */
	wide = malloc(CREDENTIAL_WIDE_MAX * sizeof(*wide));
	if (wide == NULL)
		return ENOMEM;
	full = malloc(CREDENTIAL_WIDE_MAX * sizeof(*full));
	if (full == NULL) {
		free(wide);
		return ENOMEM;
	}

	/* The path in UTF-16. */
	units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, (int)CREDENTIAL_WIDE_MAX);
	if (units <= 0) {
		free(wide);
		free(full);
		return EINVAL;
	}

	/* The full path after the prefix: the same file always has the same name ("." and ".." resolved). */
	prefix = wcslen(CREDENTIAL_PREFIX);
	memcpy(full, CREDENTIAL_PREFIX, prefix * sizeof(*full));
	length = GetFullPathNameW(wide, (DWORD)(CREDENTIAL_WIDE_MAX - prefix), full + prefix, NULL);
	free(wide);
	if (length == 0U || length >= CREDENTIAL_WIDE_MAX - prefix) {
		free(full);
		return EIO;
	}

	/* Succeeded: the caller has the name. */
	*name = full;
	return 0;
}
