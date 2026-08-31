/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_WIFI_STORE_H
#define ZEDBSD_WIFI_STORE_H

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
int wifi_store_set_key_at(int, const char *, uid_t, gid_t, const void *,
			  size_t, const void *, size_t, int, char *, size_t);
int wifi_store_load_at(int, const char *, uid_t, gid_t,
		       struct wifi_conf_model *, char *, size_t);

void wifi_store_test_fail_once(enum wifi_store_test_stage, int);
int wifi_store_test_open_directory(const char *, uid_t);
#endif

#endif
