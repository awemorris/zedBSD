/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The text files that say which keys are trusted: authorized_keys (which
 * user keys may log in to an account, sshd(8) AUTHORIZED_KEYS FILE
 * FORMAT) and known_hosts (which host keys belong to which hosts, sshd(8)
 * SSH_KNOWN_HOSTS FILE FORMAT), and the host patterns of known_hosts and
 * of ssh_config's Host lines (ssh_config(5) PATTERNS).
 *
 * The files are OpenSSH's, so that a file written by one implementation
 * is read by the other.  Only the parts these programs need are
 * understood, and a line that uses another part is passed over (counted
 * in *skipped), never taken as a match:
 *
 *   authorized_keys: a line that starts with options (anything before the
 *   key type) is passed over; options are not supported, so a key the
 *   owner restricted is not accepted without its restriction.  Lines of
 *   another key type are passed over.
 *
 *   known_hosts: hashed host names ("|1|...", which need HMAC-SHA1) and
 *   @cert-authority lines are passed over.  @revoked lines are honoured:
 *   a revoked ssh-ed25519 key is refused for every host.  Lines of another
 *   key type say only that the host is known with some other key.
 *
 * Text is the whole file as read (at most SSH_HOSTFILE_MAX bytes; lines
 * end with LF, a CR before it is ignored).  Blank lines and lines whose
 * first non-blank character is '#' are ignored.  These functions open no
 * file and keep no state.
 */

#ifndef SSH_SSH_HOSTFILE_H
#define SSH_SSH_HOSTFILE_H

#include "ssh-key.h"

/* The largest authorized_keys or known_hosts file read, in bytes. */
#define SSH_HOSTFILE_MAX	(1024U * 1024U)

/* The longest line, in bytes; a longer line is passed over. */
#define SSH_HOSTFILE_LINE_MAX	8192U

/* The longest host name these programs handle (a DNS name, RFC 1035 2.3.4). */
#define SSH_HOST_MAX		255U

/* The answers of ssh_known_hosts_check. */
enum ssh_known_result {
	/* No line names the host. */
	SSH_KNOWN_ABSENT = 0,

	/* A line names the host with this key. */
	SSH_KNOWN_MATCH = 1,

	/* Lines name the host, with other ssh-ed25519 keys only (a changed host key: refuse). */
	SSH_KNOWN_CHANGED = 2,

	/* The key is on an @revoked line (refuse). */
	SSH_KNOWN_REVOKED = 3,

	/* Lines name the host, but only with keys of other types. */
	SSH_KNOWN_OTHER_TYPES = 4
};

/*
 * Matches a string against one pattern of size bytes (ssh_config(5)
 * PATTERNS): '*' matches any run of characters (also empty), '?' any one
 * character, every other byte itself; host names compare without regard
 * to the case of ASCII letters (fold is 1) or exactly (fold is 0).
 *
 * The matching is iterative with backtracking over the last '*' only, so
 * its time is at most the product of the lengths.  Returns 1 or 0.
 */
int ssh_pattern_match(const char *pattern, size_t size, const char *string, int fold);

/*
 * Matches a string against a comma-separated list of patterns of size
 * bytes, where a pattern that starts with '!' negates.
 *
 * Returns 1 when some pattern matches and no negated one does, -1 when a
 * negated pattern matches (a negated match wins over every other), 0
 * otherwise.
 */
int ssh_pattern_list_match(const char *list, size_t size, const char *string, int fold);

/*
 * Looks for public_key in the authorized_keys text.
 *
 * A line counts when it is "ssh-ed25519 <base64> [comment]" and its key
 * (ssh_key_parse_public_line) equals public_key.  *found receives 1 or 0;
 * *skipped (may be NULL) the number of lines passed over (options, other
 * types, malformed, too long), for a log line.  size above
 * SSH_HOSTFILE_MAX is EMSGSIZE.  Returns 0 or EMSGSIZE.
 */
int ssh_authorized_keys_find(const char *text, size_t size, const uint8_t *public_key, int *found, unsigned *skipped);

/*
 * Checks a host key against the known_hosts text.
 *
 * host is the name ssh connects to (the HostName of ssh_config when one
 * applies, otherwise the name the user gave; OpenSSH looks up the same
 * name), port the port.  The name looked up is host when port is 22 and
 * "[host]:port" otherwise
 * (sshd(8) SSH_KNOWN_HOSTS FILE FORMAT); a line's first field is a
 * pattern list (ssh_pattern_list_match, fold 1) that must match it.  The
 * answer is SSH_KNOWN_REVOKED when an @revoked ssh-ed25519 line has
 * public_key, otherwise SSH_KNOWN_MATCH when a matching line has
 * public_key, otherwise SSH_KNOWN_CHANGED when a matching line has
 * another ssh-ed25519 key, otherwise SSH_KNOWN_OTHER_TYPES when a matching
 * line has a key of another type, otherwise SSH_KNOWN_ABSENT.
 *
 * *skipped (may be NULL) as for ssh_authorized_keys_find.  Returns 0,
 * EINVAL (host empty, longer than SSH_HOST_MAX, or holding a byte that is
 * not printable ASCII), or EMSGSIZE.
 */
int ssh_known_hosts_check(const char *text, size_t size, const char *host, uint16_t port, const uint8_t *public_key, enum ssh_known_result *result, unsigned *skipped);

/*
 * Appends a known_hosts line for host and port (named as in
 * ssh_known_hosts_check, not hashed) and public_key, ending with LF, to
 * out.  Returns 0, EINVAL (host as above) or the error recorded in out.
 */
int ssh_known_hosts_put_line(const char *host, uint16_t port, const uint8_t *public_key, struct ssh_buf *out);

/*
 * Appends to out the known_hosts text without the lines that name host
 * and port (as ssh_known_hosts_check matches them, any key type, also
 * @revoked lines; hashed lines are kept), every other line kept byte for
 * byte (ssh-keygen -R).  *removed receives the number of lines left out.
 * Returns 0, EINVAL (host as above), EMSGSIZE or the error recorded in
 * out.
 */
int ssh_known_hosts_remove(const char *text, size_t size, const char *host, uint16_t port, struct ssh_buf *out, unsigned *removed);

#endif
