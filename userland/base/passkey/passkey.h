/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * /sbin/passkey's pure parts (ws172-p002; docs/architecture/security.md,
 * "Login authentication"): the request's fields (request.c) and the
 * records of /etc/passkey (record.c).  Neither touches a file, so the host
 * test runs them alone.
 */

#ifndef USERLAND_BASE_PASSKEY_H
#define USERLAND_BASE_PASSKEY_H

#include <stddef.h>
#include <sys/types.h>

/* The longest request, the most fields, and the longest field. */
#define PASSKEY_REQUEST_MAX	4096U
#define PASSKEY_FIELDS_MAX	6U
#define PASSKEY_FIELD_MAX	1024U

/* The operations. */
#define PASSKEY_OP_AUTH		1
#define PASSKEY_OP_STYLES	2
#define PASSKEY_OP_ENROLLED	3
#define PASSKEY_OP_ENROLL_PIN	4
#define PASSKEY_OP_REMOVE_PIN	5
#define PASSKEY_OP_ENROLL_FIDO2	6
#define PASSKEY_OP_REMOVE_FIDO2	7
#define PASSKEY_OP_KEY_INFO	8
#define PASSKEY_OP_KEY_SET_PIN	9
#define PASSKEY_OP_KEY_CHANGE_PIN	10
#define PASSKEY_OP_KEY_RESET	11
#define PASSKEY_OP_SET_OPTIONS	12
#define PASSKEY_OP_AUTH_FIDO2	13
#define PASSKEY_OP_KEY_OWNER	14
#define PASSKEY_OP_SET_METHODS	15

/* The file, its first line, and the version this passkey writes. */
#ifndef PASSKEY_FILE
#define PASSKEY_FILE		"/etc/passkey"
#endif
#define PASSKEY_HEADER		"# zedBSD passkey "
#define PASSKEY_VERSION		1

/* The digits of a PIN, the most security keys of an account, and a key's reference's size (with its NUL). */
#define PASSKEY_PIN_DIGITS	6U
#define PASSKEY_FIDO2_MAX	5U
#define PASSKEY_REF_SIZE	17U

/*
 * A request taken apart: its operation and its fields (the operation's
 * word is field 0, the account's name field 1), each ended by a NUL in
 * the request's own buffer.
 */
struct passkey_request {
	int operation;
	unsigned count;
	char *fields[PASSKEY_FIELDS_MAX];
};

/*
 * An account's options (ws199-p001 section 2, shared with WS200): the
 * methods it signs in with (Sign-in Methods, WS200), whether its key's
 * PIN is asked, and whether its key must be touched to unlock.  The line
 * is "name:uid:options:methods=M:key-pin=0|1:key-touch=0|1"; none, two or
 * a line that does not read are the defaults (every method, the PIN and
 * the touch).  key-touch=0 only with key-pin=0.
 */
#define PASSKEY_METHODS_DEFAULT	"password,pin,fido2"
#define PASSKEY_METHODS_MAX	64U
struct passkey_options {
	char methods[PASSKEY_METHODS_MAX];
	int key_pin;
	int key_touch;
};

/*
 * The sign-in methods as bits (WS200): the password, the PIN, a security
 * key.  The login and locked screens offer and take only the methods of
 * the options line that the account has set up; when none of them is the
 * password or a key, the password is taken too (the PIN alone cannot be
 * the first sign-in after a start).  The console, su, sudo and SSH always
 * take the password: they do not ask passkey.
 */
#define PASSKEY_METHOD_PASSWORD	0x1U
#define PASSKEY_METHOD_PIN	0x2U
#define PASSKEY_METHOD_FIDO2	0x4U
#define PASSKEY_METHODS_ALL	0x7U

int passkey_request_parse(char *text, size_t length, struct passkey_request *request);
void passkey_options_default(struct passkey_options *options);
int passkey_options_read(const char *text, size_t length, const char *name, uid_t uid, struct passkey_options *options);
int passkey_options_line(const char *name, uid_t uid, const struct passkey_options *options, char *line, size_t size);
int passkey_options_is_default(const struct passkey_options *options);
int passkey_methods_parse(const char *text, unsigned *methods);
void passkey_methods_text(unsigned methods, char *text, size_t size);
unsigned passkey_methods_effective(unsigned methods, int pin_enrolled, int key_enrolled);
unsigned passkey_options_methods(const struct passkey_options *options);
int passkey_is_pin(const char *text);
void passkey_wipe(void *memory, size_t size);
int passkey_record_version(const char *text, size_t length);
int passkey_record_find(const char *text, size_t length, const char *name, uid_t uid, const char *kind,
    unsigned index, char *line, size_t size);
int passkey_record_count(const char *text, size_t length, const char *name, uid_t uid, const char *kind);
int passkey_record_replace(const char *text, size_t length, const char *name, const char *kind, const char *added,
    char *output, size_t capacity, size_t *written);
int passkey_record_field(const char *line, unsigned index, char *field, size_t size);
void passkey_record_ref(const char *id, char *ref, size_t size);
int passkey_record_edit(const char *text, size_t length, const char *name, const char *kind, const char *field,
    const char *added, char *output, size_t capacity, size_t *written);

#endif
