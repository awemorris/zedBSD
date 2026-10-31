/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The authorized_keys and known_hosts files and host patterns.
 *
 * Skeleton: every function answers ENOSYS (or does nothing) until it is
 * implemented.
 */

#include "ssh-hostfile.h"

/*
 * Matches a string against one pattern of size bytes (ssh_config(5)
 * PATTERNS): '*' matches any run of characters (also empty), '?' any one
 * character, every other byte itself; host names compare without regard
 * to the case of ASCII letters (fold is 1) or exactly (fold is 0).
 */
int
ssh_pattern_match(
	const char *pattern,
	size_t size,
	const char *string,
	int fold)
{
	/* Marks the arguments used until the function is implemented. */
	(void)pattern;
	(void)size;
	(void)string;
	(void)fold;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Matches a string against a comma-separated list of patterns of size
 * bytes, where a pattern that starts with '!' negates.
 */
int
ssh_pattern_list_match(
	const char *list,
	size_t size,
	const char *string,
	int fold)
{
	/* Marks the arguments used until the function is implemented. */
	(void)list;
	(void)size;
	(void)string;
	(void)fold;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Looks for public_key in the authorized_keys text.
 */
int
ssh_authorized_keys_find(
	const char *text,
	size_t size,
	const uint8_t *public_key,
	int *found,
	unsigned *skipped)
{
	/* Marks the arguments used until the function is implemented. */
	(void)text;
	(void)size;
	(void)public_key;
	(void)found;
	(void)skipped;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Checks a host key against the known_hosts text.
 */
int
ssh_known_hosts_check(
	const char *text,
	size_t size,
	const char *host,
	uint16_t port,
	const uint8_t *public_key,
	enum ssh_known_result *result,
	unsigned *skipped)
{
	/* Marks the arguments used until the function is implemented. */
	(void)text;
	(void)size;
	(void)host;
	(void)port;
	(void)public_key;
	(void)result;
	(void)skipped;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Appends a known_hosts line for host and port (named as in
 * ssh_known_hosts_check, not hashed) and public_key, ending with LF, to
 * out.
 */
int
ssh_known_hosts_put_line(
	const char *host,
	uint16_t port,
	const uint8_t *public_key,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)host;
	(void)port;
	(void)public_key;
	(void)out;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Appends to out the known_hosts text without the lines that name host
 * and port (as ssh_known_hosts_check matches them, any key type, also
 * @revoked lines; hashed lines are kept), every other line kept byte for
 * byte (ssh-keygen -R).
 */
int
ssh_known_hosts_remove(
	const char *text,
	size_t size,
	const char *host,
	uint16_t port,
	struct ssh_buf *out,
	unsigned *removed)
{
	/* Marks the arguments used until the function is implemented. */
	(void)text;
	(void)size;
	(void)host;
	(void)port;
	(void)out;
	(void)removed;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}
