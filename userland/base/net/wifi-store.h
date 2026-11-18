/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef KERN_WIFI_STORE_H
#define KERN_WIFI_STORE_H

#include "userland/base/net/wifi-conf.h"

#include <stddef.h>
#include <sys/types.h>

/*
 * Select /etc/wifi.conf for effective UID zero, otherwise select the passwd
 * record home's .wifi.conf.  Neither function consults HOME.
 */
int wifi_store_set_key_for_effective_user(const char *, const char *, int,
					  char *, size_t);
int wifi_store_load_for_effective_user(struct wifi_conf_model *, char *,
				       size_t);

/* What one change to a credential store does. */
enum wifi_store_edit_kind {
	WIFI_STORE_EDIT_SET,		/* add the SSID, or replace its key and mode */
	WIFI_STORE_EDIT_ADD,		/* add an SSID that is not saved (EEXIST otherwise) */
	WIFI_STORE_EDIT_MODIFY,		/* change a saved SSID (ENOENT otherwise) */
	WIFI_STORE_EDIT_DELETE		/* remove a saved SSID (ENOENT otherwise) */
};

/*
 * One change to a credential store.  ssid and passphrase are NUL-terminated
 * text; a modify with a NULL passphrase keeps the saved key, and one with an
 * automatic of -1 keeps the saved mode.  A delete uses neither.
 */
struct wifi_store_edit {
	enum wifi_store_edit_kind kind;
	const char *ssid;
	const char *passphrase;
	int automatic;
};

/*
 * Apply one change to the store of the invoking process's effective UID
 * (/etc/wifi.conf for root, the passwd home's .wifi.conf otherwise), as one
 * locked, atomic rewrite.
 */
int wifi_store_edit_for_effective_user(const struct wifi_store_edit *, char *,
				       size_t);
int wifi_store_load_for_user(uid_t, struct wifi_conf_model *, char *, size_t);

enum wifi_store_test_stage {
	WIFI_STORE_TEST_NONE,
	WIFI_STORE_TEST_LOCK_OPEN,
	WIFI_STORE_TEST_LOCK_ACQUIRE,
	WIFI_STORE_TEST_TARGET_OPEN,
	WIFI_STORE_TEST_TARGET_READ,
	WIFI_STORE_TEST_PARSE,
	WIFI_STORE_TEST_TEMP_CREATE,
	WIFI_STORE_TEST_TEMP_WRITE,
	WIFI_STORE_TEST_TEMP_SYNC,
	WIFI_STORE_TEST_TEMP_CLOSE,
	WIFI_STORE_TEST_STAGE_OPEN,
	WIFI_STORE_TEST_STAGE_READ,
	WIFI_STORE_TEST_STAGE_VALIDATE,
	WIFI_STORE_TEST_RENAME,
	WIFI_STORE_TEST_DIRECTORY_SYNC,
	WIFI_STORE_TEST_CLEANUP,
	WIFI_STORE_TEST_UNLOCK
};

#ifdef WIFI_STORE_TESTING
/* Test-only stable-dirfd entry points and deterministic failure boundaries. */
typedef int (*wifi_store_test_load_after_read_hook_t)(int, const char *);

int wifi_store_set_key_at(int, const char *, uid_t, gid_t, const void *,
			  size_t, const void *, size_t, int, char *, size_t);
int wifi_store_update_at(int, const char *, uid_t, gid_t,
			 const struct wifi_store_edit *, char *, size_t);
int wifi_store_load_at(int, const char *, uid_t, gid_t,
		       struct wifi_conf_model *, char *, size_t);

void wifi_store_test_fail_once(enum wifi_store_test_stage, int);
void wifi_store_test_set_load_after_read_hook(
	wifi_store_test_load_after_read_hook_t);
int wifi_store_test_open_directory(const char *, uid_t);
#endif

#endif
