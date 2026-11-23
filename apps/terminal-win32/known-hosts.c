/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What mikuterm tells the user about the hosts' keys besides what the SSH
 * library's known_hosts check answers: the names a host goes by in
 * known_hosts ([host]:port for a port other than 22, in lower case), the
 * line that holds the key a changed host had, the other names the same
 * key is known by, which paths are Windows' own and how a path is named on
 * the screen.
 *
 * The known_hosts text is OpenSSH's (sshd(8) SSH_KNOWN_HOSTS FILE
 * FORMAT): one "[marker] names key-type key [comment]" a line.  As the
 * library does, lines with a hashed name ("|1|...") cannot be matched
 * against a name; they are counted as other names only by their key.
 */

#include "mikuterm.h"

#include "userland/base/ssh/ssh-hostfile.h"
#include "userland/base/ssh/ssh-key.h"

#include <keiland/keiland.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* What a hashed name is shown as, since the name itself cannot be read back. */
#define KNOWN_HASHED_NAME	"(a hashed name)"

/*
 * One line of a known_hosts text, as far as mikuterm reads it: whether it
 * has a marker (@revoked, @cert-authority), its names, and its key when
 * the key is an ssh-ed25519 one.
 */
struct known_line {
	int marked;
	int revoked;
	const char *names;
	size_t names_size;
	int hashed;
	int has_key;
	uint8_t key[SSH_ED25519_PUBLIC];
};

static void known_line_read(const char *line, size_t size, struct known_line *parsed);
static const char *known_skip_blanks(const char *at, const char *end);
static const char *known_skip_word(const char *at, const char *end);
static void known_other_add(struct mikuterm_known_scan *scan, const char *path, unsigned number, const struct known_line *parsed);
static char known_lower(char character);

/*
 * Writes the name known_hosts gives a host at a port: the host in lower
 * case, in brackets with the port after it when the port is not 22.
 * Returns 0 or ENAMETOOLONG.
 */
int
mikuterm_host_port_name(
	const char *host,
	uint16_t port,
	char *name,
	size_t size)
{
	size_t length;
	size_t index;
	int written;

	/* The host as it is, or in brackets with its port. */
	if (port == 22U)
		written = snprintf(name, size, "%s", host);
	else
		written = snprintf(name, size, "[%s]:%u", host, (unsigned)port);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* The letters in lower case, as OpenSSH writes and looks up a name. */
	length = (size_t)written;
	for (index = 0U; index < length; index++)
		name[index] = known_lower(name[index]);

	/* Succeeded: the name. */
	return 0;
}

/*
 * Writes the address a connection reached as OpenSSH shows it beside the
 * host's name: the address alone for port 22, else in brackets with the
 * port after it.  Returns 0, or the error of writing the address.
 */
int
mikuterm_address_name(
	const struct kl_address *address,
	uint16_t port,
	char *name,
	size_t size)
{
	char text[KL_ADDRESS_TEXT];
	char *colon;
	char *start;
	size_t length;
	int error;

	/* The address with its port ("192.0.2.1:22", "[2001:db8::1]:22"). */
	error = kl_address_format(address, text, sizeof(text));
	if (error != 0)
		return error;

	/* The address alone: the port after the last colon goes, and the brackets round an IPv6 address. */
	colon = strrchr(text, ':');
	if (colon != NULL)
		*colon = '\0';
	start = text;
	length = strlen(text);
	if (length >= 2U &&
	    text[0] == '[' &&
	    text[length - 1U] == ']') {
		text[length - 1U] = '\0';
		start = text + 1;
	}

	/* The name for the port. */
	error = mikuterm_host_port_name(start, port, name, size);
	if (error != 0)
		return error;

	/* Succeeded: the address's name. */
	return 0;
}

/*
 * Looks through a known_hosts text (from the file at path, as it is shown
 * to the user) for what the user is told about a host's key: the first
 * line that names the host with another ssh-ed25519 key, the @revoked line
 * of the key, and the lines that hold the same key under other names.
 * name is the host's name in known_hosts (mikuterm_host_port_name).  The
 * scan adds to what earlier texts found (the first line found stays).
 */
void
mikuterm_known_hosts_scan(
	const char *text,
	size_t size,
	const char *path,
	const char *name,
	const uint8_t *key,
	struct mikuterm_known_scan *scan)
{
	struct known_line parsed;
	const char *line;
	const char *end;
	size_t length;
	unsigned number;
	int listed;
	int differs;
	int names_match;
	int same_key;

	/* Each line, counted from 1. */
	line = text;
	number = 0U;
	while (line < text + size) {
		/* The line, without its LF. */
		end = memchr(line, '\n', (size_t)(text + size - line));
		if (end == NULL)
			end = text + size;
		length = (size_t)(end - line);
		number++;
		known_line_read(line, length, &parsed);
		line = end + 1;

		/* A line without an ssh-ed25519 key says nothing of this key. */
		if (!parsed.has_key)
			continue;

		/* Whether its names name the host (a hashed name cannot be told). */
		listed = 0;
		if (!parsed.hashed)
			listed = ssh_pattern_list_match(parsed.names, parsed.names_size, name, 1);
		names_match = 0;
		if (listed == 1)
			names_match = 1;

		/* Whether it holds this key. */
		differs = memcmp(parsed.key, key, sizeof(parsed.key));
		same_key = 0;
		if (differs == 0)
			same_key = 1;

		/* The key revoked. */
		if (parsed.revoked) {
			if (same_key && scan->revoked_line == 0U) {
				scan->revoked_line = number;
				(void)snprintf(scan->revoked_path, sizeof(scan->revoked_path), "%s", path);
			}

			continue;
		}

		/* A certificate authority's line is no host's key. */
		if (parsed.marked)
			continue;

		/* The host with another key: the line that stands in the way. */
		if (names_match && !same_key) {
			if (scan->changed_line == 0U) {
				scan->changed_line = number;
				(void)snprintf(scan->changed_path, sizeof(scan->changed_path), "%s", path);
			}

			continue;
		}

		/* The same key under another name. */
		if (!names_match && same_key)
			known_other_add(scan, path, number, &parsed);
	}
}

/*
 * Tells whether a path is a Windows path from a drive or a server (1) or
 * not (0): a drive's letter, its colon and a separator (C:\..., C:/...);
 * two backslashes (\\server\...); or two slashes, a server's name and a
 * separator after it (//server/share/..., as mikuterm shows a server's
 * path).  Two slashes with no name, or no separator after the name, are a
 * POSIX path from the root ("//etc" is "/etc"), not a server's.
 */
int
mikuterm_path_absolute(
	const char *path)
{
	size_t index;

	/* A drive's letter, its colon and a separator. */
	if (((path[0] >= 'A' &&
	      path[0] <= 'Z') ||
	     (path[0] >= 'a' &&
	      path[0] <= 'z')) &&
	    path[1] == ':' &&
	    (path[2] == '\\' ||
	     path[2] == '/'))
		return 1;

	/* A server's share, written with backslashes. */
	if (path[0] == '\\' && path[1] == '\\')
		return 1;

	/* Anything else that does not start with two slashes is not a Windows path. */
	if (path[0] != '/' || path[1] != '/')
		return 0;

	/* With slashes: the server's name runs to the second separator, which must be there. */
	for (index = 2U; path[index] != '\0'; index++) {
		/* The separator after the name; one right after the slashes leaves no name. */
		if (path[index] == '/' || path[index] == '\\') {
			if (index == 2U)
				return 0;
			return 1;
		}
	}

	/* A name with nothing after it is not a server's path. */
	return 0;
}

/*
 * Tells where a path's directory ends: the place of its last separator (a
 * slash or a backslash, as Windows takes either), or 0 when it has none or
 * only the first character is one (a path with no directory to make).
 */
size_t
mikuterm_path_directory(
	const wchar_t *path)
{
	size_t directory;
	size_t index;

	/* The last separator. */
	directory = 0U;
	for (index = 0U; path[index] != L'\0'; index++) {
		/* A separator of either kind. */
		if (path[index] == L'/' || path[index] == L'\\')
			directory = index;
	}

	/* Succeeded: where the directory ends. */
	return directory;
}

/*
 * Writes a path as the user is shown it, the one way every path mikuterm
 * shows goes: its part inside the folder ("ssh/known_hosts") when it is
 * there, as it is otherwise (a name already inside the folder, such as
 * "ssh/config", or a path elsewhere on Windows), with slashes between its
 * names either way.  Windows takes a slash as a backslash, so a path shown
 * can be opened as it reads; the path mikuterm opens is not changed.
 */
void
mikuterm_path_shown(
	const char *path,
	char *shown,
	size_t size)
{
	const char *folder;
	const char *inside;
	size_t folder_length;
	size_t index;
	int differs;

	/* The part after the folder and its separator, when the path is in the folder. */
	folder = mikuterm_folder();
	folder_length = strlen(folder);
	inside = path;
	differs = strncmp(path, folder, folder_length);
	if (folder_length != 0U &&
	    differs == 0 &&
	    (path[folder_length] == '\\' ||
	     path[folder_length] == '/'))
		inside = path + folder_length + 1U;

	/* That part, with slashes. */
	(void)snprintf(shown, size, "%s", inside);
	for (index = 0U; shown[index] != '\0'; index++) {
		/* A backslash is a slash. */
		if (shown[index] == '\\')
			shown[index] = '/';
	}
}

/* Reads one known_hosts line (size bytes, no LF) as far as mikuterm needs it. */
static void
known_line_read(
	const char *line,
	size_t size,
	struct known_line *parsed)
{
	struct ssh_key key;
	const char *at;
	const char *end;
	const char *word;
	int differs;
	int error;

	/* Nothing found yet; a CR before the LF is not part of the line. */
	memset(parsed, 0, sizeof(*parsed));
	end = line + size;
	if (end > line && end[-1] == '\r')
		end--;

	/* A line too long for the library holds no key. */
	if (size > SSH_HOSTFILE_LINE_MAX)
		return;

	/* A blank line or a comment holds none either. */
	at = known_skip_blanks(line, end);
	if (at == end || *at == '#')
		return;

	/* A marker before the names: @revoked, or another (@cert-authority). */
	if (*at == '@') {
		word = at;
		at = known_skip_word(at, end);
		parsed->marked = 1;
		differs = 1;
		if ((size_t)(at - word) == 8U)
			differs = memcmp(word, "@revoked", 8U);
		if (differs == 0)
			parsed->revoked = 1;
		at = known_skip_blanks(at, end);
	}

	/* The names, up to the white space before the key; a hashed name starts with '|'. */
	parsed->names = at;
	at = known_skip_word(at, end);
	parsed->names_size = (size_t)(at - parsed->names);
	if (parsed->names_size == 0U)
		return;
	if (parsed->names[0] == '|')
		parsed->hashed = 1;
	at = known_skip_blanks(at, end);

	/* The key, when it is an ssh-ed25519 one. */
	error = ssh_key_parse_public_line(at, (size_t)(end - at), &key);
	if (error != 0)
		return;
	memcpy(parsed->key, key.public_key, sizeof(parsed->key));
	parsed->has_key = 1;
}

/* Returns where the spaces and tabs from at end (end at most). */
static const char *
known_skip_blanks(
	const char *at,
	const char *end)
{
	/* Each space or tab. */
	while (at < end &&
	       (*at == ' ' ||
	        *at == '\t'))
		at++;

	/* Succeeded: the first other character. */
	return at;
}

/* Returns where the word at at ends: its first space or tab (end at most). */
static const char *
known_skip_word(
	const char *at,
	const char *end)
{
	/* Each character that is not a space or a tab. */
	while (at < end &&
	       *at != ' ' &&
	       *at != '\t')
		at++;

	/* Succeeded: the end of the word. */
	return at;
}

/* Counts a line that holds the key under another name, and keeps it while there is room. */
static void
known_other_add(
	struct mikuterm_known_scan *scan,
	const char *path,
	unsigned number,
	const struct known_line *parsed)
{
	struct mikuterm_known_other *other;
	size_t length;

	/* One more; only the first few are shown. */
	scan->other_count++;
	if (scan->other_count > MIKUTERM_KNOWN_OTHERS)
		return;
	other = &scan->others[scan->other_count - 1U];

	/* Where it is. */
	(void)snprintf(other->path, sizeof(other->path), "%s", path);
	other->line = number;

	/* Its names, or what a hashed one is shown as; a long field is cut. */
	if (parsed->hashed) {
		(void)snprintf(other->names, sizeof(other->names), "%s", KNOWN_HASHED_NAME);
		return;
	}

	/* The names, cut at the room for them. */
	length = parsed->names_size;
	if (length >= sizeof(other->names))
		length = sizeof(other->names) - 1U;
	memcpy(other->names, parsed->names, length);
	other->names[length] = '\0';
}

/* Returns an ASCII letter in lower case, any other byte as it is. */
static char
known_lower(
	char character)
{
	/* A capital letter. */
	if (character >= 'A' && character <= 'Z')
		return (char)(character - 'A' + 'a');

	/* Anything else. */
	return character;
}
