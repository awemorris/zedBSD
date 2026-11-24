/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Command-line target parser and evdev-to-PS/2 Set 1 scancode translation
 * for zedBSD's native RDP client.
 */

#include "rdp-client.h"

#include "userland/desktop/librdp/rdp.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static void copy_str_slice(char *dst, size_t dst_sz, const char *src, size_t len);
static int parse_port(const char *s, uint16_t *out);

/*
 * Parses CLI options and [domain\][user@]host[:port] into target.
 */
int
rdp_parse_args(
	int argc,
	char **argv,
	struct rdp_app_target *target)
{
	int i;
	const char *arg;
	const char *pos_arg;
	const char *at;
	const char *slash;
	const char *colon;
	unsigned long val;

	if (target == NULL)
		return EINVAL;

	memset(target, 0, sizeof(*target));
	target->port = RDP_DEFAULT_PORT;
	target->width = 1280U;
	target->height = 720U;
	pos_arg = NULL;

	for (i = 1; i < argc; i++) {
		arg = argv[i];
		if (strncmp(arg, "--width=", 8U) == 0) {
			val = strtoul(arg + 8, NULL, 10);
			if (val >= 320UL && val <= 7680UL)
				target->width = (uint32_t)val;
		} else if (strncmp(arg, "--height=", 9U) == 0) {
			val = strtoul(arg + 9, NULL, 10);
			if (val >= 200UL && val <= 4320UL)
				target->height = (uint32_t)val;
		} else if (strncmp(arg, "--domain=", 9U) == 0) {
			copy_str_slice(target->domain, sizeof(target->domain),
				       arg + 9, strlen(arg + 9));
		} else if (strncmp(arg, "--password=", 11U) == 0) {
			copy_str_slice(target->password,
				       sizeof(target->password), arg + 11,
				       strlen(arg + 11));
		} else if (arg[0] != '-') {
			pos_arg = arg;
		}
	}

	if (pos_arg == NULL || pos_arg[0] == '\0')
		return EINVAL;

	if (strncmp(pos_arg, "rdp://", 6U) == 0)
		pos_arg += 6;

	at = strchr(pos_arg, '@');
	if (at != NULL) {
		slash = strchr(pos_arg, '\\');
		if (slash != NULL && slash < at) {
			copy_str_slice(target->domain, sizeof(target->domain),
				       pos_arg, (size_t)(slash - pos_arg));
			copy_str_slice(target->user, sizeof(target->user),
				       slash + 1, (size_t)(at - (slash + 1)));
		} else {
			copy_str_slice(target->user, sizeof(target->user),
				       pos_arg, (size_t)(at - pos_arg));
		}

		pos_arg = at + 1;
	}

	colon = strrchr(pos_arg, ':');
	if (colon != NULL && strchr(colon, ']') == NULL) {
		if (parse_port(colon + 1, &target->port) != 0)
			return EINVAL;

		copy_str_slice(target->host, sizeof(target->host), pos_arg,
			       (size_t)(colon - pos_arg));
	} else {
		copy_str_slice(target->host, sizeof(target->host), pos_arg,
			       strlen(pos_arg));
	}

	if (target->host[0] == '\0')
		return EINVAL;

	/* Succeeded: CLI arguments parsed. */
	return 0;
}

/*
 * Maps an evdev keycode to a PS/2 Set 1 scancode and RDP Fast-Path flags.
 */
int
rdp_evdev_to_scancode(
	uint32_t evdev_code,
	uint16_t *scancode,
	uint16_t *flags)
{
	static const struct {
		uint32_t evdev;
		uint16_t scan;
		uint16_t ext;
	} ext_map[] = {
		{ 96U, 0x1CU, RDP_KBD_FLAG_EXTENDED },
		{ 97U, 0x1DU, RDP_KBD_FLAG_EXTENDED },
		{ 98U, 0x35U, RDP_KBD_FLAG_EXTENDED },
		{ 100U, 0x38U, RDP_KBD_FLAG_EXTENDED },
		{ 102U, 0x47U, RDP_KBD_FLAG_EXTENDED },
		{ 103U, 0x48U, RDP_KBD_FLAG_EXTENDED },
		{ 104U, 0x49U, RDP_KBD_FLAG_EXTENDED },
		{ 105U, 0x4BU, RDP_KBD_FLAG_EXTENDED },
		{ 106U, 0x4DU, RDP_KBD_FLAG_EXTENDED },
		{ 107U, 0x4FU, RDP_KBD_FLAG_EXTENDED },
		{ 108U, 0x50U, RDP_KBD_FLAG_EXTENDED },
		{ 109U, 0x51U, RDP_KBD_FLAG_EXTENDED },
		{ 110U, 0x52U, RDP_KBD_FLAG_EXTENDED },
		{ 111U, 0x53U, RDP_KBD_FLAG_EXTENDED },
		{ 125U, 0x5BU, RDP_KBD_FLAG_EXTENDED },
		{ 126U, 0x5CU, RDP_KBD_FLAG_EXTENDED },
		{ 127U, 0x5DU, RDP_KBD_FLAG_EXTENDED }
	};
	size_t i;

	*flags = 0U;
	if (evdev_code >= 1U && evdev_code <= 88U) {
		*scancode = (uint16_t)evdev_code;
		return 1;
	}

	for (i = 0U; i < sizeof(ext_map) / sizeof(ext_map[0]); i++) {
		if (ext_map[i].evdev == evdev_code) {
			*scancode = ext_map[i].scan;
			*flags = ext_map[i].ext;
			return 1;
		}
	}

	return 0;
}

/* Copies up to dst_sz - 1 bytes into dst and NUL-terminates it. */
static void
copy_str_slice(
	char *dst,
	size_t dst_sz,
	const char *src,
	size_t len)
{
	size_t n;

	if (dst_sz == 0U)
		return;

	n = len;
	if (n >= dst_sz)
		n = dst_sz - 1U;

	memcpy(dst, src, n);
	dst[n] = '\0';
}

/* Parses a TCP port number in [1, 65535]. */
static int
parse_port(
	const char *s,
	uint16_t *out)
{
	unsigned long v;
	size_t i;

	if (s == NULL || s[0] == '\0')
		return EINVAL;

	for (i = 0U; s[i] != '\0'; i++) {
		if (!isdigit((unsigned char)s[i]))
			return EINVAL;
	}

	v = strtoul(s, NULL, 10);
	if (v < 1UL || v > 65535UL)
		return EINVAL;

	*out = (uint16_t)v;
	return 0;
}
