/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The authorized_keys and known_hosts files and host patterns.
 */

#include <string.h>

#include "ssh-hostfile.h"

/* The most bytes of the name a known_hosts line is matched against: a host name, "[", "]:" and a port of five digits, with a NUL. */
#define HOSTFILE_NAME_MAX	272U

/* The most bytes of one public key line as ssh_key_put_public_line writes it: "ssh-ed25519", a space, 68 characters of base64 and an LF. */
#define HOSTFILE_KEY_LINE_MAX	96U

/* The port that a known_hosts line names by the host alone (sshd(8) SSH_KNOWN_HOSTS FILE FORMAT: the brackets are for a non-standard port). */
#define HOSTFILE_STANDARD_PORT	22U

/* What a line of a file is, for the readers of this file. */
#define HOSTFILE_LINE_IGNORED	0
#define HOSTFILE_LINE_TOO_LONG	1
#define HOSTFILE_LINE_BAD	2
#define HOSTFILE_LINE_CONTENT	3

/* The marker of a known_hosts line (sshd(8) SSH_KNOWN_HOSTS FILE FORMAT). */
#define HOSTFILE_MARKER_NONE	0
#define HOSTFILE_MARKER_REVOKED	1
#define HOSTFILE_MARKER_OTHER	2

/*
 * The fields of one known_hosts line, pointing into the text it was read
 * from: the marker, the host name field, whether that field is hashed, and
 * the rest of the line from the key type on.
 */
struct hostfile_known_line {
	int marker;
	int hashed;
	const char *hosts;
	size_t hosts_size;
	const char *key;
	size_t key_size;
};

static int hostfile_is_blank(char byte);
static size_t hostfile_skip_blank(const char *text, size_t position, size_t end);
static size_t hostfile_skip_field(const char *text, size_t position, size_t end);
static size_t hostfile_trim_end(const char *text, size_t size);
static unsigned hostfile_fold(unsigned char byte, int fold);
static int hostfile_next_line(const char *text, size_t size, size_t *offset, size_t *start, size_t *length);
static int hostfile_classify(const char *line, size_t length, size_t *content_start, size_t *content_end);
static int hostfile_split_known(const char *line, size_t start, size_t end, struct hostfile_known_line *parsed);
static int hostfile_host_check(const char *host, int writable);
static int hostfile_make_name(const char *host, uint16_t port, int writable, char *name, size_t *name_size);
static int hostfile_arguments_check(const char *text, size_t size);
static int hostfile_same_key(const uint8_t *left, const uint8_t *right);
static int hostfile_text_is(const char *text, size_t size, const char *expected);

/*
 * Matches a string against one pattern of size bytes (ssh_config(5)
 * PATTERNS): '*' matches any run of characters (also empty), '?' any one
 * character, every other byte itself; host names compare without regard
 * to the case of ASCII letters (fold is 1) or exactly (fold is 0).
 *
 * When a byte differs, the match goes back to the last '*' and lets it
 * take one more character; earlier stars are never revisited, so no
 * recursion is needed and the time is at most the product of the lengths.
 */
int
ssh_pattern_match(
	const char *pattern,
	size_t size,
	const char *string,
	int fold)
{
	size_t pattern_at;
	size_t string_at;
	size_t star_pattern;
	size_t star_string;
	unsigned pattern_byte;
	unsigned string_byte;
	int have_star;

	/* Starts both scans at the beginning with no star seen. */
	pattern_at = 0;
	string_at = 0;
	star_pattern = 0;
	star_string = 0;
	have_star = 0;

	/* A missing string or pattern matches nothing. */
	if (string == NULL)
		return 0;
	if (pattern == NULL && size != 0)
		return 0;

	/* Walks the string, taking one pattern byte for each string byte and going back to the last star on a mismatch. */
	while (string[string_at] != '\0') {
		/* A star is remembered together with the string position it started at. */
		if (pattern_at < size && pattern[pattern_at] == '*') {
			pattern_at++;
			star_pattern = pattern_at;
			star_string = string_at;
			have_star = 1;
			continue;
		}

		/* A '?' and an equal byte each consume one byte of both. */
		if (pattern_at < size) {
			pattern_byte = hostfile_fold((unsigned char)pattern[pattern_at], fold);
			string_byte = hostfile_fold((unsigned char)string[string_at], fold);
			if (pattern[pattern_at] == '?' || pattern_byte == string_byte) {
				pattern_at++;
				string_at++;
				continue;
			}
		}

		/* A mismatch lets the last star take one more byte of the string. */
		if (have_star) {
			star_string++;
			string_at = star_string;
			pattern_at = star_pattern;
			continue;
		}

		/* A mismatch with no star to go back to ends the match. */
		return 0;
	}

	/* The string is used up: stars may match the empty rest, any other pattern byte left means no match. */
	while (pattern_at < size && pattern[pattern_at] == '*')
		pattern_at++;
	if (pattern_at < size)
		return 0;

	/* Succeeded: the whole pattern matched the whole string. */
	return 1;
}

/*
 * Matches a string against a comma-separated list of patterns of size
 * bytes, where a pattern that starts with '!' negates.
 *
 * Empty entries (two commas in a row, a lone '!') are passed over.
 */
int
ssh_pattern_list_match(
	const char *list,
	size_t size,
	const char *string,
	int fold)
{
	size_t position;
	size_t end;
	size_t length;
	int positive;
	int matched;

	/* Starts at the first entry of the list. */
	position = 0;
	positive = 0;

	/* A missing list or string matches nothing. */
	if (string == NULL)
		return 0;
	if (list == NULL && size != 0)
		return 0;

	/* Takes each entry between commas; a negated match ends the search at once, a positive one is only remembered. */
	for (;;) {
		end = position;
		while (end < size && list[end] != ',')
			end++;
		length = end - position;

		/* A negated entry that matches refuses the string whatever else matches. */
		if (length > 1 && list[position] == '!') {
			matched = ssh_pattern_match(list + position + 1U, length - 1U, string, fold);
			if (matched)
				return -1;
		} else if (length > 0 && list[position] != '!') {
			matched = ssh_pattern_match(list + position, length, string, fold);
			if (matched)
				positive = 1;
		}

		/* The last entry has been taken. */
		if (end >= size)
			break;
		position = end + 1U;
	}

	/* Succeeded or not: a positive match with no negated one is 1. */
	if (positive)
		return 1;
	return 0;
}

/*
 * Looks for public_key in the authorized_keys text.
 *
 * Every line is read, so *skipped counts all the lines passed over.
 */
int
ssh_authorized_keys_find(
	const char *text,
	size_t size,
	const uint8_t *public_key,
	int *found,
	unsigned *skipped)
{
	struct ssh_key key;
	size_t offset;
	size_t line_start;
	size_t length;
	size_t content_start;
	size_t content_end;
	unsigned count;
	int same_key;
	int kind;
	int more;
	int error;

	/* Starts at the beginning of the file with no entries counted. */
	offset = 0;
	count = 0;

	/* Refuses arguments that are missing. */
	if (found == NULL || public_key == NULL)
		return EINVAL;

	/* The answers start as "not found, none skipped". */
	*found = 0;
	if (skipped != NULL)
		*skipped = 0;

	/* Refuses text that is missing or too large. */
	error = hostfile_arguments_check(text, size);
	if (error != 0)
		return error;

	/* Reads each line: a key line of this type is compared, any other content line is counted as passed over. */
	for (;;) {
		more = hostfile_next_line(text, size, &offset, &line_start, &length);
		if (!more)
			break;

		/* Blank lines and comments are not counted. */
		kind = hostfile_classify(text + line_start, length, &content_start, &content_end);
		if (kind == HOSTFILE_LINE_IGNORED)
			continue;

		/* A line that is too long or holds a NUL byte is passed over. */
		if (kind != HOSTFILE_LINE_CONTENT) {
			count++;
			continue;
		}

		/* Reads the key; options before the type, another type and a malformed key all fail here and are passed over. */
		error = ssh_key_parse_public_line(text + line_start + content_start, content_end - content_start, &key);
		if (error != 0) {
			count++;
			continue;
		}

		/* The key is compared. */
		same_key = hostfile_same_key(key.public_key, public_key);
		if (same_key)
			*found = 1;
	}

	/* Reports how many lines were passed over. */
	if (skipped != NULL)
		*skipped = count;

	/* Succeeded: the whole text was read. */
	return 0;
}

/*
 * Checks a host key against the known_hosts text.
 *
 * Lines that are not understood (hashed host names, @cert-authority and
 * other markers, a line that is malformed or too long, an @revoked line
 * of another key type) are counted in *skipped.  A line of another key
 * type is not counted: it is used for SSH_KNOWN_OTHER_TYPES when it names
 * the host and ignored otherwise.  An @revoked line is not matched
 * against the host: the key is refused everywhere, even when the host
 * name field of the line is hashed.
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
	struct hostfile_known_line parsed;
	struct ssh_key key;
	char name[HOSTFILE_NAME_MAX];
	size_t name_size;
	size_t offset;
	size_t line_start;
	size_t length;
	size_t content_start;
	size_t content_end;
	unsigned count;
	int revoked;
	int matched_key;
	int changed;
	int other_types;
	int host_matches;
	int same_key;
	int kind;
	int more;
	int error;

	/* Starts at the beginning of the file with no entries counted. */
	offset = 0;
	count = 0;
	revoked = 0;
	matched_key = 0;
	changed = 0;
	other_types = 0;

	/* Refuses arguments that are missing. */
	if (host == NULL || public_key == NULL || result == NULL)
		return EINVAL;

	/* The answers start as "absent, none skipped". */
	*result = SSH_KNOWN_ABSENT;
	if (skipped != NULL)
		*skipped = 0;

	/* Refuses a host name that cannot be looked up. */
	error = hostfile_make_name(host, port, 0, name, &name_size);
	if (error != 0)
		return error;

	/* Refuses text that is missing or too large. */
	error = hostfile_arguments_check(text, size);
	if (error != 0)
		return error;

	/* Reads each line and notes what it says; the answer is chosen after the last line. */
	for (;;) {
		more = hostfile_next_line(text, size, &offset, &line_start, &length);
		if (!more)
			break;

		/* Blank lines and comments are not counted. */
		kind = hostfile_classify(text + line_start, length, &content_start, &content_end);
		if (kind == HOSTFILE_LINE_IGNORED)
			continue;

		/* A line that is too long or holds a NUL byte is passed over. */
		if (kind != HOSTFILE_LINE_CONTENT) {
			count++;
			continue;
		}

		/* A line without the fields of a key line is passed over, and so is a marker these programs do not use. */
		error = hostfile_split_known(text + line_start, content_start, content_end, &parsed);
		if (error != 0 || parsed.marker == HOSTFILE_MARKER_OTHER) {
			count++;
			continue;
		}

		/* Reads the key of the line; the host name field is judged after it. */
		error = ssh_key_parse_public_line(parsed.key, parsed.key_size, &key);

		/* An @revoked line counts only when it holds an ssh-ed25519 key, whatever host it names. */
		if (parsed.marker == HOSTFILE_MARKER_REVOKED) {
			if (error != 0) {
				count++;
				continue;
			}

			/* Marks the key revoked when the line names the same key. */
			same_key = hostfile_same_key(key.public_key, public_key);
			if (same_key)
				revoked = 1;
			continue;
		}

		/* A hashed host name field cannot be matched here. */
		if (parsed.hashed) {
			count++;
			continue;
		}

		/* A key that is malformed is passed over; the host is not judged known by it. */
		if (error != 0 && error != ENOTSUP) {
			count++;
			continue;
		}

		/* Tells whether the line names this host and port. */
		host_matches = ssh_pattern_list_match(parsed.hosts, parsed.hosts_size, name, 1);
		if (host_matches != 1)
			continue;

		/* A line of another key type only says that the host is known. */
		if (error == ENOTSUP) {
			other_types = 1;
			continue;
		}

		/* An ssh-ed25519 line is the same key or a changed one. */
		same_key = hostfile_same_key(key.public_key, public_key);
		if (same_key)
			matched_key = 1;
		else
			changed = 1;
	}

	/* Chooses the answer by its order of strength: revoked, match, changed, other types, absent. */
	if (revoked)
		*result = SSH_KNOWN_REVOKED;
	else if (matched_key)
		*result = SSH_KNOWN_MATCH;
	else if (changed)
		*result = SSH_KNOWN_CHANGED;
	else if (other_types)
		*result = SSH_KNOWN_OTHER_TYPES;
	else
		*result = SSH_KNOWN_ABSENT;

	/* Reports how many lines were passed over. */
	if (skipped != NULL)
		*skipped = count;

	/* Succeeded: the whole text was read. */
	return 0;
}

/*
 * Appends a known_hosts line for host and port (named as in
 * ssh_known_hosts_check, not hashed) and public_key, ending with LF, to
 * out.
 *
 * A host that holds a byte with a meaning in a known_hosts line (a
 * comma, '*', '?', '!', brackets, or a first byte of '|', '@' or '#') is
 * EINVAL, so that the line says exactly one host.  Nothing is appended
 * unless the whole line fits.
 */
int
ssh_known_hosts_put_line(
	const char *host,
	uint16_t port,
	const uint8_t *public_key,
	struct ssh_buf *out)
{
	char name[HOSTFILE_NAME_MAX];
	size_t name_size;
	int error;

	/* Refuses arguments that are missing. */
	if (host == NULL || public_key == NULL || out == NULL)
		return EINVAL;

	/* Refuses a host name that cannot be written as one name. */
	error = hostfile_make_name(host, port, 1, name, &name_size);
	if (error != 0)
		return error;

	/* Makes room for the whole line first, so that a failure appends nothing. */
	error = ssh_buf_reserve(out, name_size + 1U + HOSTFILE_KEY_LINE_MAX);
	if (error != 0)
		return error;

	/* Writes the name, a space and the key line (type, base64 and LF). */
	ssh_buf_put_bytes(out, name, name_size);
	ssh_buf_put_u8(out, ' ');
	ssh_key_put_public_line(public_key, NULL, out);

	/* Reports an error that the writes recorded. */
	error = ssh_buf_error(out);
	if (error != 0)
		return error;

	/* Succeeded: the line is appended. */
	return 0;
}

/*
 * Appends to out the known_hosts text without the lines that name host
 * and port (as ssh_known_hosts_check matches them, any key type, also
 * @revoked lines; hashed lines are kept), every other line kept byte for
 * byte (ssh-keygen -R).
 *
 * Lines that are not understood (@cert-authority and other markers,
 * malformed and too long lines) are kept.  A line that has a host name
 * field, a key type and a key field is removed when the host name field
 * matches, whatever the key is.  Nothing is appended unless the whole
 * result fits.
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
	struct hostfile_known_line parsed;
	char name[HOSTFILE_NAME_MAX];
	size_t name_size;
	size_t offset;
	size_t line_start;
	size_t length;
	size_t content_start;
	size_t content_end;
	unsigned count;
	int names_host;
	int kind;
	int more;
	int error;

	/* Starts at the beginning of the file with no entries counted. */
	offset = 0;
	count = 0;

	/* Refuses arguments that are missing. */
	if (host == NULL || out == NULL || removed == NULL)
		return EINVAL;

	/* The answer starts as "none removed". */
	*removed = 0;

	/* Refuses a host name that cannot be looked up. */
	error = hostfile_make_name(host, port, 0, name, &name_size);
	if (error != 0)
		return error;

	/* Refuses text that is missing or too large. */
	error = hostfile_arguments_check(text, size);
	if (error != 0)
		return error;

	/* Makes room for the whole text first, so that a failure appends nothing. */
	error = ssh_buf_reserve(out, size);
	if (error != 0)
		return error;

	/* Copies each line, with its LF, unless it names the host and port. */
	for (;;) {
		more = hostfile_next_line(text, size, &offset, &line_start, &length);
		if (!more)
			break;
		names_host = 0;

		/* Only a line with content can name a host. */
		kind = hostfile_classify(text + line_start, length, &content_start, &content_end);
		if (kind == HOSTFILE_LINE_CONTENT) {
			error = hostfile_split_known(text + line_start, content_start, content_end, &parsed);
			if (error == 0 && parsed.marker != HOSTFILE_MARKER_OTHER && !parsed.hashed) {
				names_host = ssh_pattern_list_match(parsed.hosts, parsed.hosts_size, name, 1);
				if (names_host != 1)
					names_host = 0;
			}
		}

		/* A line that names the host is left out; every other line is kept byte for byte. */
		if (names_host) {
			count++;
		} else {
			ssh_buf_put_bytes(out, text + line_start, offset - line_start);
		}
	}

	/* Reports an error that the writes recorded. */
	error = ssh_buf_error(out);
	if (error != 0)
		return error;

	/* Reports how many lines were left out. */
	*removed = count;

	/* Succeeded: the remaining text is appended. */
	return 0;
}

/*
 * Tells whether a byte separates the fields of a line (a space or a tab).
 */
static int
hostfile_is_blank(
	char byte)
{
	/* A space and a tab separate the fields. */
	if (byte == ' ')
		return 1;
	if (byte == '\t')
		return 1;

	/* Any other byte is part of a field. */
	return 0;
}

/*
 * Returns the position of the first byte at or after position, before end,
 * that is not a space or a tab (end when there is none).
 */
static size_t
hostfile_skip_blank(
	const char *text,
	size_t position,
	size_t end)
{
	int blank;

	/* Moves over each space and tab. */
	while (position < end) {
		blank = hostfile_is_blank(text[position]);
		if (!blank)
			break;
		position++;
	}

	/* The first byte that is part of a field, or the end. */
	return position;
}

/*
 * Returns the position of the first space or tab at or after position,
 * before end (end when there is none).
 */
static size_t
hostfile_skip_field(
	const char *text,
	size_t position,
	size_t end)
{
	int blank;

	/* Moves over the bytes of one field. */
	while (position < end) {
		blank = hostfile_is_blank(text[position]);
		if (blank)
			break;
		position++;
	}

	/* The byte that ends the field, or the end. */
	return position;
}

/*
 * Returns the size of the line without its trailing spaces, tabs and CRs.
 */
static size_t
hostfile_trim_end(
	const char *text,
	size_t size)
{
	int blank;

	/* Moves back over each trailing space, tab and CR. */
	while (size > 0) {
		blank = hostfile_is_blank(text[size - 1U]);
		if (!blank && text[size - 1U] != '\r')
			break;
		size--;
	}

	/* The end of the content. */
	return size;
}

/*
 * Returns a byte, with an ASCII capital letter turned into its small
 * letter when fold is not 0 (other bytes, whatever their value, are not
 * changed).
 */
static unsigned
hostfile_fold(
	unsigned char byte,
	int fold)
{
	/* Only the 26 ASCII capitals change. */
	if (fold && byte >= 'A' && byte <= 'Z')
		return (unsigned)byte + ('a' - 'A');

	/* Any other byte is itself. */
	return byte;
}

/*
 * Finds the next line of text from *offset: *start is its first byte and
 * *length its size without the LF; *offset moves past the LF (or to the
 * end for a last line without one).  Returns 1 when there was a line, 0
 * at the end of the text.
 */
static int
hostfile_next_line(
	const char *text,
	size_t size,
	size_t *offset,
	size_t *start,
	size_t *length)
{
	size_t position;

	/* No bytes are left. */
	if (*offset >= size)
		return 0;

	/* Looks for the LF that ends the line. */
	*start = *offset;
	position = *offset;
	while (position < size && text[position] != '\n')
		position++;
	*length = position - *start;

	/* Moves past the LF, when the line has one. */
	if (position < size)
		position++;
	*offset = position;

	/* Succeeded: a line was found. */
	return 1;
}

/*
 * Tells what a line of length bytes is: blank or a comment (ignored),
 * longer than SSH_HOSTFILE_LINE_MAX, holding a NUL byte, or a line with
 * content, whose first and one-past-last byte (without surrounding blanks
 * and the CR) are put in *content_start and *content_end.
 */
static int
hostfile_classify(
	const char *line,
	size_t length,
	size_t *content_start,
	size_t *content_end)
{
	size_t start;
	size_t end;
	size_t index;

	/* Finds the content between the blanks. */
	end = hostfile_trim_end(line, length);
	start = hostfile_skip_blank(line, 0, end);

	/* A blank line and a comment say nothing, however long. */
	if (start >= end || line[start] == '#')
		return HOSTFILE_LINE_IGNORED;

	/* A line over the limit is not read. */
	if (length > SSH_HOSTFILE_LINE_MAX)
		return HOSTFILE_LINE_TOO_LONG;

	/* A NUL byte would hide the rest of the line from a reader of C strings. */
	for (index = 0; index < length; index++) {
		if (line[index] == '\0')
			return HOSTFILE_LINE_BAD;
	}

	/* Succeeded: there is a line to read. */
	*content_start = start;
	*content_end = end;
	return HOSTFILE_LINE_CONTENT;
}

/*
 * Splits a known_hosts line (sshd(8) SSH_KNOWN_HOSTS FILE FORMAT) into
 * its marker, host name field and the rest from the key type on.
 *
 * line[start..end) is the content of the line.  Returns 0, or EINVAL when
 * the host name field, the key type or the key field is missing.
 */
static int
hostfile_split_known(
	const char *line,
	size_t start,
	size_t end,
	struct hostfile_known_line *parsed)
{
	size_t position;
	size_t field_end;
	size_t key_start;
	int is_revoked;

	/* Starts at the beginning of the line with no marker and no hash. */
	position = start;
	parsed->marker = HOSTFILE_MARKER_NONE;
	parsed->hashed = 0;

	/* A first field that starts with '@' is a marker; only @revoked is used. */
	field_end = hostfile_skip_field(line, position, end);
	if (line[position] == '@') {
		parsed->marker = HOSTFILE_MARKER_OTHER;
		is_revoked = hostfile_text_is(line + position, field_end - position, "@revoked");
		if (is_revoked)
			parsed->marker = HOSTFILE_MARKER_REVOKED;
		position = hostfile_skip_blank(line, field_end, end);
		field_end = hostfile_skip_field(line, position, end);
	}

	/* The host name field is required. */
	if (position >= end)
		return EINVAL;
	parsed->hosts = line + position;
	parsed->hosts_size = field_end - position;
	if (line[position] == '|')
		parsed->hashed = 1;

	/* The key type is required. */
	key_start = hostfile_skip_blank(line, field_end, end);
	if (key_start >= end)
		return EINVAL;

	/* The key field is required too. */
	field_end = hostfile_skip_field(line, key_start, end);
	position = hostfile_skip_blank(line, field_end, end);
	if (position >= end)
		return EINVAL;

	/* Succeeded: the rest of the line from the type on is the public key line. */
	parsed->key = line + key_start;
	parsed->key_size = end - key_start;
	return 0;
}

/*
 * Checks a host name: 1 to SSH_HOST_MAX bytes of visible ASCII (0x21 to
 * 0x7e; a space or a control byte would break the line).  When writable
 * is not 0 the name must also be one plain name: no comma, '*', '?', '!',
 * '[' or ']', and not starting with '|', '@' or '#'.  Returns 0 or EINVAL.
 */
static int
hostfile_host_check(
	const char *host,
	int writable)
{
	size_t length;
	size_t index;
	unsigned char byte;

	/* Finds the length, stopping one past the limit so that a long text is not read to its end. */
	length = 0;
	while (length <= SSH_HOST_MAX && host[length] != '\0')
		length++;

	/* A name has 1 to SSH_HOST_MAX bytes. */
	if (length == 0 || length > SSH_HOST_MAX)
		return EINVAL;

	/* Every byte must be visible ASCII. */
	for (index = 0; index < length; index++) {
		byte = (unsigned char)host[index];
		if (byte < 0x21U || byte > 0x7eU)
			return EINVAL;
	}

	/* A name that is to be written must not mean more than one host. */
	if (writable) {
		if (host[0] == '|' || host[0] == '@' || host[0] == '#')
			return EINVAL;
		for (index = 0; index < length; index++) {
			byte = (unsigned char)host[index];
			if (byte == ',' || byte == '*' || byte == '?' || byte == '!' || byte == '[' || byte == ']')
				return EINVAL;
		}
	}

	/* Succeeded: the name is acceptable. */
	return 0;
}

/*
 * Builds the name a known_hosts line is matched against: host for port 22,
 * "[host]:port" otherwise.  name has room for HOSTFILE_NAME_MAX bytes; the
 * name is NUL-terminated and *name_size is its length.  Returns 0 or EINVAL.
 */
static int
hostfile_make_name(
	const char *host,
	uint16_t port,
	int writable,
	char *name,
	size_t *name_size)
{
	char digits[5];
	size_t length;
	size_t count;
	unsigned value;
	int error;

	/* Refuses a host that is not acceptable. */
	error = hostfile_host_check(host, writable);
	if (error != 0)
		return error;
	length = strlen(host);

	/* Port 22 is written by the host alone. */
	if (port == HOSTFILE_STANDARD_PORT) {
		memcpy(name, host, length);
		name[length] = '\0';
		*name_size = length;
		return 0;
	}

	/* Takes the decimal digits of the port, last one first. */
	value = port;
	count = 0;
	do {
		digits[count] = (char)('0' + (value % 10U));
		count++;
		value /= 10U;
	} while (value != 0);

	/* Writes "[", the host, "]:" and the digits in order. */
	name[0] = '[';
	memcpy(name + 1U, host, length);
	name[1U + length] = ']';
	name[2U + length] = ':';
	length += 3U;
	while (count > 0) {
		count--;
		name[length] = digits[count];
		length++;
	}

	/* Ends the name and reports its length. */
	name[length] = '\0';
	*name_size = length;

	/* Succeeded: the name is built. */
	return 0;
}

/*
 * Checks the text arguments shared by the readers: text may be NULL only
 * when size is 0, and size is at most SSH_HOSTFILE_MAX.  Returns 0,
 * EINVAL or EMSGSIZE.
 */
static int
hostfile_arguments_check(
	const char *text,
	size_t size)
{
	/* Bytes that are missing. */
	if (text == NULL && size != 0)
		return EINVAL;

	/* A file larger than the limit is not read. */
	if (size > SSH_HOSTFILE_MAX)
		return EMSGSIZE;

	/* Succeeded: the arguments are in range. */
	return 0;
}

/*
 * Tells whether two public keys are equal (a public key is not secret, so
 * a plain comparison is enough).
 */
static int
hostfile_same_key(
	const uint8_t *left,
	const uint8_t *right)
{
	int different;

	/* A difference in any byte makes the keys different. */
	different = memcmp(left, right, SSH_ED25519_PUBLIC);
	if (different != 0)
		return 0;

	/* The keys are equal. */
	return 1;
}

/*
 * Tells whether size bytes at text are exactly the NUL-terminated string
 * expected.
 */
static int
hostfile_text_is(
	const char *text,
	size_t size,
	const char *expected)
{
	size_t expected_size;
	int different;

	/* A different length is a different text. */
	expected_size = strlen(expected);
	if (size != expected_size)
		return 0;

	/* The same length with different bytes is a different text. */
	different = memcmp(text, expected, size);
	if (different != 0)
		return 0;

	/* The two texts are equal. */
	return 1;
}
