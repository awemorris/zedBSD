/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SSH keys and the known hosts of the settings tab's Keys pane: an
 * ssh-ed25519 key made as ssh-keygen makes one (OpenSSH's private key
 * file, encrypted with aes256-ctr and the bcrypt KDF when a passphrase is
 * given, and the one-line public key beside it), the public key of a key
 * there already, and the lines of known_hosts listed and removed.
 *
 * The files are the program's (struct terminal_page_program): it says
 * where they are and writes them, a new private key so that only the user
 * may read it.  The random bytes are the SSH library's (ssh_random), which
 * come from the operating system.
 */

#include "settings-page.h"

#include "userland/base/ssh/ssh-key.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The salt's size and the bcrypt rounds of a new encrypted key: what
 * ssh-keygen of OpenSSH writes by default (ssh-keygen -a).
 */
#define KEY_SALT_SIZE		16U
#define KEY_ROUNDS		24U

/* The suffix of a public key file next to its private key file. */
#define KEY_PUBLIC_SUFFIX	".pub"

/* The longest path of a key file, with its NUL. */
#define KEY_PATH_MAX		1100U

/* The largest public key file read. */
#define KEY_PUBLIC_MAX		8192U

static int key_paths(const struct terminal_page_program *program, const char *name, char *private_path, char *public_path, size_t size);
static int key_write(const struct ssh_key *key, const char *passphrase, struct ssh_buf *file);
static int key_line_of(const uint8_t *public_key, const char *comment, char *line, size_t line_size);
static size_t key_line_end(const char *text, size_t size, size_t start);
static size_t key_line_next(const char *text, size_t size, size_t start);
static size_t key_field_end(const char *text, size_t at, size_t end);
static size_t key_blank_end(const char *text, size_t at, size_t end);
static void key_copy(char *to, size_t size, const char *from, size_t length);
static void key_why(char *why, size_t size, const char *text);

/*
 * Tells whether a name may be a key file's: 1 to TERMINAL_KEY_NAME_MAX - 1
 * letters, digits, '.', '_' and '-', not starting with '.' and not ending
 * with ".pub" (the public key's).  Returns 0 or EINVAL.
 */
int
terminal_key_name_valid(
	const char *name)
{
	size_t length;
	size_t index;
	unsigned char byte;
	int same;

	/* Not empty, and not longer than a name is kept. */
	length = strlen(name);
	if (length == 0U || length >= TERMINAL_KEY_NAME_MAX)
		return EINVAL;

	/* Not hidden, so the user sees the file. */
	if (name[0] == '.')
		return EINVAL;

	/* Each character one a file's name has on every system. */
	for (index = 0U; index < length; index++) {
		byte = (unsigned char)name[index];
		if (byte >= 'a' && byte <= 'z')
			continue;
		if (byte >= 'A' && byte <= 'Z')
			continue;
		if (byte >= '0' && byte <= '9')
			continue;
		if (byte == '.' ||
		    byte == '_' ||
		    byte == '-')
			continue;
		return EINVAL;
	}

	/* Not a public key's name. */
	if (length >= sizeof(KEY_PUBLIC_SUFFIX) - 1U) {
		same = strcmp(name + length - (sizeof(KEY_PUBLIC_SUFFIX) - 1U), KEY_PUBLIC_SUFFIX);
		if (same == 0)
			return EINVAL;
	}

	/* Succeeded: the name may be a key's. */
	return 0;
}

/*
 * Makes a new ssh-ed25519 key named name in the program's key directory:
 * the private key file (OpenSSH's form, encrypted when passphrase is not
 * empty) that only the user may read, and the public key file beside it
 * (name.pub).  A file of either name that is there already is not
 * touched (EEXIST).  line receives the public key's line (without its
 * line break); why says what went wrong, for the user.
 *
 * Returns 0, EINVAL (a name or passphrase that cannot be), EEXIST,
 * ENAMETOOLONG, EIO, or the program's error of writing a file.
 */
int
terminal_key_make(
	const struct terminal_page_program *program,
	const char *name,
	const char *comment,
	const char *passphrase,
	char *line,
	size_t line_size,
	char *why,
	size_t why_size)
{
	char private_path[KEY_PATH_MAX];
	char public_path[KEY_PATH_MAX];
	char text[TERMINAL_ITEM_VALUE];
	struct ssh_key key;
	struct ssh_buf file;
	struct ssh_buf public_line;
	size_t passphrase_length;
	int exists;
	int error;

	/* Nothing made yet. */
	line[0] = '\0';
	why[0] = '\0';

	/* A name a file may have. */
	error = terminal_key_name_valid(name);
	if (error != 0) {
		key_why(why, why_size, "A key's name is letters, digits, '.', '_' and '-' (not starting with '.' or ending with .pub).");
		return error;
	}

	/* A passphrase a reader of the file takes. */
	passphrase_length = strlen(passphrase);
	if (passphrase_length > SSH_KEY_PASSPHRASE_MAX) {
		key_why(why, why_size, "The passphrase is too long.");
		return EINVAL;
	}

	/* Its two files' paths. */
	error = key_paths(program, name, private_path, public_path, sizeof(private_path));
	if (error != 0) {
		key_why(why, why_size, "The key's path is too long.");
		return error;
	}

	/* A key of the name is never written over. */
	exists = program->exists(program->data, private_path);
	if (!exists)
		exists = program->exists(program->data, public_path);
	if (exists) {
		(void)snprintf(text, sizeof(text), "%s%s is there already: choose another name.", program->key_directory_shown, name);
		key_why(why, why_size, text);
		return EEXIST;
	}

	/* The key, from the system's random bytes. */
	error = ssh_key_generate(&key, comment);
	if (error != 0) {
		key_why(why, why_size, "No random bytes for the key.");
		return EIO;
	}

	/* Its private key file, which holds the seed. */
	ssh_buf_init(&file, SSH_KEY_FILE_MAX, 1);
	error = key_write(&key, passphrase, &file);
	if (error != 0) {
		ssh_buf_free(&file);
		ssh_key_clear(&key);
		key_why(why, why_size, "The key could not be written out.");
		return error;
	}

	/* The private key file, made new and readable by the user alone. */
	error = program->create(program->data, private_path, file.data, file.size, 1);
	ssh_buf_free(&file);
	if (error != 0) {
		ssh_key_clear(&key);
		(void)snprintf(text, sizeof(text), "%s%s could not be made (%s).", program->key_directory_shown, name, strerror(error));
		key_why(why, why_size, text);
		return error;
	}

	/* The public key's line, for its file. */
	ssh_buf_init(&public_line, KEY_PUBLIC_MAX, 0);
	ssh_key_put_public_line(key.public_key, key.comment, &public_line);
	error = ssh_buf_error(&public_line);
	if (error == 0)
		error = program->create(program->data, public_path, public_line.data, public_line.size, 0);
	if (error != 0) {
		ssh_buf_free(&public_line);
		ssh_key_clear(&key);
		(void)snprintf(text, sizeof(text), "%s%s%s could not be made (%s).", program->key_directory_shown, name, KEY_PUBLIC_SUFFIX, strerror(error));
		key_why(why, why_size, text);
		return error;
	}

	/* The line for the user, without its line break. */
	key_copy(line, line_size, (const char *)public_line.data, key_line_end((const char *)public_line.data, public_line.size, 0U));
	ssh_buf_free(&public_line);
	ssh_key_clear(&key);

	/* Succeeded: the key's two files are made. */
	return 0;
}

/*
 * Reads the public key of the key named name in the program's key
 * directory: its .pub file's line, or, without one, the public half of
 * its private key file (encrypted or not).  line receives the line
 * (without its line break) and fingerprint its SHA256 fingerprint.
 *
 * Returns 0, ENOENT when there is no such key, EINVAL or ENOTSUP for a
 * file that holds no ssh-ed25519 key, or ENAMETOOLONG.
 */
int
terminal_key_public(
	const struct terminal_page_program *program,
	const char *name,
	char *line,
	size_t line_size,
	char *fingerprint,
	size_t fingerprint_size)
{
	char private_path[KEY_PATH_MAX];
	char public_path[KEY_PATH_MAX];
	struct ssh_key key;
	char *text;
	size_t size;
	size_t end;
	int encrypted;
	int error;

	/* Nothing found yet. */
	line[0] = '\0';
	fingerprint[0] = '\0';

	/* The key's two files' paths. */
	error = key_paths(program, name, private_path, public_path, sizeof(private_path));
	if (error != 0)
		return error;

	/* The public key file's first line. */
	error = program->read(program->data, public_path, KEY_PUBLIC_MAX, &text, &size);
	if (error == 0) {
		end = key_line_end(text, size, 0U);
		error = ssh_key_parse_public_line(text, end, &key);
		if (error == 0)
			key_copy(line, line_size, text, end);
		free(text);
	} else {
		/* Without it, the public half of the private key file. */
		error = program->read(program->data, private_path, SSH_KEY_FILE_MAX, &text, &size);
		if (error != 0)
			return error;
		error = ssh_key_parse_private_public(text, size, &key, &encrypted);
		free(text);
		if (error == 0)
			error = key_line_of(key.public_key, "", line, line_size);
	}

	/* A file that is not an ssh-ed25519 key's. */
	if (error != 0)
		return error;

	/* Its fingerprint, as ssh-keygen -l shows it. */
	error = ssh_key_fingerprint(key.public_key, fingerprint, fingerprint_size);
	if (error != 0)
		fingerprint[0] = '\0';

	/* Succeeded: the key's public line. */
	return 0;
}

/*
 * Lists the lines of a known_hosts file's text that hold a key (not the
 * comments and the blank lines), the first capacity of them, each with
 * its number, marker, names, key type and fingerprint (sshd(8)
 * SSH_KNOWN_HOSTS FILE FORMAT).  *total receives how many there are.
 * Returns how many were listed.
 */
unsigned
terminal_known_list(
	const char *text,
	size_t size,
	struct terminal_known_line *lines,
	unsigned capacity,
	unsigned *total)
{
	struct terminal_known_line *made;
	struct ssh_key key;
	size_t start;
	size_t end;
	size_t at;
	size_t field;
	unsigned number;
	unsigned count;
	int error;

	/* Each line, counted from 1. */
	count = 0U;
	*total = 0U;
	number = 0U;
	for (start = 0U;
	     start < size;
	     start = key_line_next(text, size, start)) {
		end = key_line_end(text, size, start);
		number++;

		/* A blank line and a comment hold no key. */
		at = key_blank_end(text, start, end);
		if (at == end || text[at] == '#')
			continue;

		/* A line kept: one more, and listed while there is room. */
		(*total)++;
		if (count >= capacity)
			continue;
		made = &lines[count];
		memset(made, 0, sizeof(*made));
		made->line = number;

		/* The marker, when the line starts with one. */
		if (text[at] == '@') {
			field = key_field_end(text, at, end);
			key_copy(made->marker, sizeof(made->marker), text + at, field - at);
			at = key_blank_end(text, field, end);
		}

		/* The names. */
		field = key_field_end(text, at, end);
		key_copy(made->names, sizeof(made->names), text + at, field - at);
		at = key_blank_end(text, field, end);

		/* The key's type, and the fingerprint of a key the terminal reads (the type, the key and any comment). */
		field = key_field_end(text, at, end);
		key_copy(made->type, sizeof(made->type), text + at, field - at);
		error = ssh_key_parse_public_line(text + at, end - at, &key);
		if (error == 0)
			(void)ssh_key_fingerprint(key.public_key, made->fingerprint, sizeof(made->fingerprint));
		count++;
	}

	/* Succeeded: the lines listed. */
	return count;
}

/*
 * Makes a known_hosts file's text without one of its lines (counted from
 * 1, with its line break), into a buffer *out of *out_size bytes the
 * caller frees.  Returns 0, ENOENT when the text has no such line, or
 * ENOMEM.
 */
int
terminal_known_remove(
	const char *text,
	size_t size,
	unsigned line,
	char **out,
	size_t *out_size)
{
	char *made;
	size_t start;
	size_t after;
	unsigned number;

	/* The line's start. */
	*out = NULL;
	*out_size = 0U;
	number = 1U;
	start = 0U;
	while (number < line && start < size) {
		start = key_line_next(text, size, start);
		number++;
	}

	/* A file that ends before the line; otherwise the start of the one after it. */
	if (line == 0U || start >= size)
		return ENOENT;
	after = key_line_next(text, size, start);

	/* The text before the line and after it. */
	made = malloc(size + 1U);
	if (made == NULL)
		return ENOMEM;
	memcpy(made, text, start);
	memcpy(made + start, text + after, size - after);

	/* Succeeded: the text without the line. */
	*out = made;
	*out_size = start + size - after;
	return 0;
}

/* Joins the key directory and a key's name into its private and public files' paths; 0 or ENAMETOOLONG. */
static int
key_paths(
	const struct terminal_page_program *program,
	const char *name,
	char *private_path,
	char *public_path,
	size_t size)
{
	int written;

	/* The private key file's path. */
	written = snprintf(private_path, size, "%s%s", program->key_directory, name);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* The public key file's, beside it. */
	written = snprintf(public_path, size, "%s%s%s", program->key_directory, name, KEY_PUBLIC_SUFFIX);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded: both paths. */
	return 0;
}

/*
 * Writes a key's private key file into file: encrypted with a passphrase
 * (a fresh salt, ssh-keygen's rounds) unless it is empty; the check
 * integer is random either way.  Returns 0, EIO or the buffer's error.
 */
static int
key_write(
	const struct ssh_key *key,
	const char *passphrase,
	struct ssh_buf *file)
{
	uint8_t salt[KEY_SALT_SIZE];
	uint32_t checkint;
	size_t length;
	int error;

	/* The check integer, which a reader compares after the decryption. */
	error = ssh_random(&checkint, sizeof(checkint));
	if (error != 0)
		return EIO;

	/* A key without a passphrase is written in the clear. */
	length = strlen(passphrase);
	if (length == 0U) {
		error = ssh_key_put_private(key, checkint, file);
		if (error != 0)
			return error;
		return 0;
	}

	/* An encrypted one takes a salt of its own. */
	error = ssh_random(salt, sizeof(salt));
	if (error != 0)
		return EIO;

	/* The file, encrypted. */
	error = ssh_key_put_private_passphrase(key, checkint, (const uint8_t *)passphrase, length, salt, sizeof(salt), KEY_ROUNDS, file);
	if (error != 0)
		return error;

	/* Succeeded: the private key file. */
	return 0;
}

/* Writes a public key's line (without its line break) into line; 0 or the buffer's error. */
static int
key_line_of(
	const uint8_t *public_key,
	const char *comment,
	char *line,
	size_t line_size)
{
	struct ssh_buf made;
	int error;

	/* The line as a .pub file holds it. */
	ssh_buf_init(&made, KEY_PUBLIC_MAX, 0);
	ssh_key_put_public_line(public_key, comment, &made);
	error = ssh_buf_error(&made);
	if (error != 0) {
		ssh_buf_free(&made);
		return error;
	}

	/* Without its line break. */
	key_copy(line, line_size, (const char *)made.data, key_line_end((const char *)made.data, made.size, 0U));
	ssh_buf_free(&made);

	/* Succeeded: the line. */
	return 0;
}

/* Finds where the line starting at start ends: its LF, a CR before the LF, or the text's end. */
static size_t
key_line_end(
	const char *text,
	size_t size,
	size_t start)
{
	size_t end;

	/* The LF, or the end of the text. */
	end = start;
	while (end < size && text[end] != '\n')
		end++;

	/* A CR before it belongs to the line break. */
	if (end > start && text[end - 1U] == '\r')
		return end - 1U;

	/* Reports the line's end. */
	return end;
}

/* Finds where the line after the one starting at start starts: past its LF, or the text's end. */
static size_t
key_line_next(
	const char *text,
	size_t size,
	size_t start)
{
	size_t at;

	/* Up to the LF. */
	at = start;
	while (at < size && text[at] != '\n')
		at++;

	/* Past it, when there is one. */
	if (at < size)
		return at + 1U;

	/* Reports the text's end. */
	return size;
}

/* Finds where the field starting at at ends: the first space or tab, or end. */
static size_t
key_field_end(
	const char *text,
	size_t at,
	size_t end)
{
	/* Up to the white space. */
	while (at < end &&
	       text[at] != ' ' &&
	       text[at] != '\t')
		at++;

	/* Reports the field's end. */
	return at;
}

/* Finds the first byte from at that is not a space or a tab, or end. */
static size_t
key_blank_end(
	const char *text,
	size_t at,
	size_t end)
{
	/* Past the white space. */
	while (at < end &&
	       (text[at] == ' ' ||
	        text[at] == '\t'))
		at++;

	/* Reports where the next field starts. */
	return at;
}

/* Copies length bytes as a C string as far as they fit, never cutting a UTF-8 character in two. */
static void
key_copy(
	char *to,
	size_t size,
	const char *from,
	size_t length)
{
	/* The bytes that fit, less a character's leading part the cut would leave. */
	if (length >= size) {
		length = size - 1U;
		while (length > 0U && ((unsigned char)from[length] & 0xc0U) == 0x80U)
			length--;
	}

	/* The copy, ended. */
	memcpy(to, from, length);
	to[length] = '\0';
}

/* Writes why something failed for the user, as far as it fits. */
static void
key_why(
	char *why,
	size_t size,
	const char *text)
{
	/* The text, cut to fit. */
	key_copy(why, size, text, strlen(text));
}
