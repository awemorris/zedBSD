/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_WIFI_CONF_H
#define ZEDBSD_WIFI_CONF_H

#include <stddef.h>

/* These limits are part of the wifi-conf version 1 interface. */
#define WIFI_CONF_FILE_MAX 32768U
#define WIFI_CONF_LINE_MAX 512U
#define WIFI_CONF_PROFILE_MAX 64U
#define WIFI_CONF_PASSPHRASE_TOTAL_MAX 4096U
#define WIFI_CONF_SSID_MAX 32U
#define WIFI_CONF_PASSPHRASE_MAX 63U
#define WIFI_CONF_DIAGNOSTIC_MAX 512U

struct wifi_conf_profile {
	unsigned char ssid[WIFI_CONF_SSID_MAX];
	size_t ssid_length;
	unsigned char passphrase[WIFI_CONF_PASSPHRASE_MAX];
	size_t passphrase_length;
	int automatic;
};

struct wifi_conf_model {
	struct wifi_conf_profile profiles[WIFI_CONF_PROFILE_MAX];
	size_t profile_count;
	size_t passphrase_bytes;
};

/* Clear secret-bearing storage through volatile accesses. */
void wifi_conf_explicit_clear(void *, size_t);

void wifi_conf_model_init(struct wifi_conf_model *);
void wifi_conf_model_clear(struct wifi_conf_model *);

/*
 * Parsing is transactional: a failure leaves model unchanged.  Input is a
 * counted byte sequence and need not have a trailing NUL.
 */
int wifi_conf_parse(const void *, size_t, struct wifi_conf_model *, char *,
		    size_t);

int wifi_conf_validate(const struct wifi_conf_model *, char *, size_t);

int wifi_conf_validate_profile(const void *, size_t, const void *, size_t,
			       char *, size_t);

/*
 * Replace an existing profile without moving it, or append a new profile.
 * Callers pass counted bytes so an SSID remains opaque; embedded NUL is still
 * rejected by the v1 semantic contract.
 */
int wifi_conf_set_key(struct wifi_conf_model *, const void *, size_t,
		      const void *, size_t, int, char *, size_t);

/*
 * Serialize one canonical v1 generation.  output_length excludes an optional
 * convenience NUL.  The final newline is included in output_length.
 */
int wifi_conf_serialize(const struct wifi_conf_model *, void *, size_t,
			size_t *, char *, size_t);

#endif
