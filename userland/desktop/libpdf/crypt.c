/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The standard security handler of libpdf's reader (stage 3 of
 * design-pdf.md): the decryption of an encrypted document with its user
 * password or its owner password.  Most documents that only restrict
 * printing or copying have an empty user password, which is what the
 * reader tries when it is given none (ws079-p015 added the passwords a
 * person types).
 *
 * Revisions 2 to 4 (RC4 of 40 to 128 bits, and AES-128 through a crypt
 * filter) derive the file key from the padded user password, /O, /P and
 * the first /ID string (PDF 1.7 algorithm 2) and check it against /U
 * (algorithms 4 and 5); an owner password first gives the padded user
 * password by decrypting /O (algorithm 7).  Each object's key mixes in its
 * number and generation (algorithm 1).  Revisions 5 and 6 (AES-256) check
 * the password against /U, or with /U against /O, with SHA-256 (revision
 * 5) or the hash of ISO 32000-2 algorithm 2.B (revision 6), and decrypt
 * /UE or /OE into the file key, which every object uses as it is.  Strings
 * and streams follow the crypt filters /StrF and /StmF name (/Identity
 * leaves them as they are).
 *
 * A password that is neither is refused (EACCES).  The permissions (/P)
 * are not enforced: the reader only displays.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <md5.h>
#include <sha2.h>

#include <pdf.h>

#include "internal.h"

/* The largest RC4 and AES key, and AES's block. */
#define CRYPT_KEY_MAX 32
#define CRYPT_BLOCK 16

/* The most rounds of AES-256 (14) and their round keys. */
#define CRYPT_ROUNDS_MAX 14
#define CRYPT_ROUND_KEYS ((CRYPT_ROUNDS_MAX + 1) * 16)

/* The longest password revisions 5 and 6 take, in bytes of UTF-8; a longer one counts its first bytes. */
#define CRYPT_PASSWORD_MAX 127

/* The user data revision 6's hash of an owner password takes: /U's 48 bytes. */
#define CRYPT_USER_DATA 48

/* The longest input of revision 6's hash round: the password, a 64-byte hash and the user data, 64 times. */
#define CRYPT_HASH_INPUT ((CRYPT_PASSWORD_MAX + 64 + CRYPT_USER_DATA) * 64)

/*
 * The ciphers a crypt filter names.
 */
enum crypt_method {
	CRYPT_NONE = 0,
	CRYPT_RC4,
	CRYPT_AES_128,
	CRYPT_AES_256
};

/*
 * AES's substitution tables, computed once per handler.
 */
struct crypt_tables {
	unsigned char sbox[256];
	unsigned char inverse[256];
};

/*
 * An expanded AES key: the round keys and the count of rounds.
 */
struct crypt_aes {
	unsigned char round_keys[CRYPT_ROUND_KEYS];
	int rounds;
};

/*
 * The security handler of an open document: the file key, the ciphers of
 * strings and streams, and whether metadata streams are encrypted.
 */
struct pdf_crypt {
	unsigned char key[CRYPT_KEY_MAX];
	size_t key_length;
	int revision;
	enum crypt_method strings;
	enum crypt_method streams;
	int encrypt_metadata;
	struct crypt_tables tables;
};

/* The padding a password is completed with to 32 bytes (PDF 1.7 algorithm 2, step a). */
static const unsigned char crypt_padding[32] = {
	0x28, 0xbf, 0x4e, 0x5e, 0x4e, 0x75, 0x8a, 0x41, 0x64, 0x00, 0x4e, 0x56, 0xff, 0xfa, 0x01, 0x08,
	0x2e, 0x2e, 0x00, 0xb6, 0xd0, 0x68, 0x3e, 0x80, 0x2f, 0x0c, 0xa9, 0xfe, 0x64, 0x53, 0x69, 0x7a
};

static int read_handler(struct pdf_document *document, struct pdf_object *encrypt, const unsigned char *id, size_t id_length, const unsigned char *password, size_t password_length, struct pdf_crypt **crypt);
static int read_legacy_key(struct pdf_document *document, struct pdf_object *encrypt, struct pdf_crypt *crypt, long permissions, const unsigned char *id, size_t id_length, const unsigned char *password, size_t password_length);
static int read_aes256_key(struct pdf_document *document, struct pdf_object *encrypt, struct pdf_crypt *crypt, const unsigned char *password, size_t password_length);
static int read_integer(struct pdf_document *document, struct pdf_object *encrypt, const char *key, long fallback, long *value);
static int read_string(struct pdf_document *document, struct pdf_object *encrypt, const char *key, size_t minimum, const unsigned char **bytes);
static int read_methods(struct pdf_document *document, struct pdf_object *encrypt, struct pdf_crypt *crypt);
static int filter_method(struct pdf_document *document, struct pdf_object *filters, struct pdf_object *name, enum crypt_method *method, size_t *length);
static void pad_password(const unsigned char *password, size_t length, unsigned char padded[32]);
static int legacy_key(struct pdf_crypt *crypt, const unsigned char *padded, const unsigned char *owner, const unsigned char *user, long permissions, const unsigned char *id, size_t id_length);
static void legacy_owner_to_user(const struct pdf_crypt *crypt, const unsigned char *padded_owner, const unsigned char *owner, unsigned char padded_user[32]);
static int aes256_key(struct pdf_crypt *crypt, const unsigned char *password, size_t password_length, const unsigned char *check, const unsigned char *encrypted_key, const unsigned char *user_data, size_t user_data_length);
static void aes256_hash(const struct pdf_crypt *crypt, const unsigned char *password, size_t password_length, const unsigned char *salt, const unsigned char *user_data, size_t user_data_length, unsigned char hash[32]);
static void revision6_hash(const struct crypt_tables *tables, const unsigned char *password, size_t password_length, const unsigned char *salt, const unsigned char *user_data, size_t user_data_length, unsigned char hash[32]);
static void rc4(const unsigned char *key, size_t key_length, const unsigned char *input, size_t size, unsigned char *output);
static void make_tables(struct crypt_tables *tables);
static unsigned char multiply(unsigned char a, unsigned char b);
static void aes_expand(const struct crypt_tables *tables, const unsigned char *key, size_t key_length, struct crypt_aes *aes);
static void aes_encrypt_block(const struct crypt_tables *tables, const struct crypt_aes *aes, const unsigned char input[16], unsigned char output[16]);
static void aes_decrypt_block(const struct crypt_tables *tables, const struct crypt_aes *aes, const unsigned char input[16], unsigned char output[16]);
static int aes_cbc_decrypt(const struct crypt_tables *tables, const unsigned char *key, size_t key_length, const unsigned char *input, size_t size, unsigned char *output, size_t *output_size);

/*
 * Opens the standard security handler an /Encrypt dictionary describes,
 * with a password: the user's or the owner's (an empty one when the
 * document is opened without).
 *
 * ENOTSUP is a handler or revision the reader does not have, or an
 * encryption dictionary it cannot read; EACCES a password that is neither
 * the user's nor the owner's.
 */
int
pdf_crypt_open(
	struct pdf_document *document,
	struct pdf_object *encrypt,
	const unsigned char *id,
	size_t id_length,
	const unsigned char *password,
	size_t password_length,
	struct pdf_crypt **crypt)
{
	int error;

	/* Reads the handler. */
	error = read_handler(document, encrypt, id, id_length, password, password_length, crypt);

	/* A dictionary that cannot be read is an encryption the reader does not have; a password and memory are themselves. */
	if (error == ENOMEM)
		return ENOMEM;
	if (error == EACCES)
		return EACCES;
	if (error != 0)
		return ENOTSUP;

	/* Succeeded: the caller owns the handler. */
	return 0;
}

/* Reads the handler of an /Encrypt dictionary with a password (see pdf_crypt_open()). */
static int
read_handler(
	struct pdf_document *document,
	struct pdf_object *encrypt,
	const unsigned char *id,
	size_t id_length,
	const unsigned char *password,
	size_t password_length,
	struct pdf_crypt **crypt)
{
	struct pdf_crypt *created;
	struct pdf_object *filter;
	long version;
	long revision;
	long length;
	long permissions;
	int is_standard;
	int whole_bytes;
	int error;

	/* Only the standard security handler is read. */
	if (encrypt->type != PDF_OBJECT_DICTIONARY)
		return PDF_EFORMAT;
	error = pdf_reader_resolve_key(document, encrypt, "Filter", &filter);
	if (error != 0)
		return error;
	is_standard = pdf_object_is_name(filter, "Standard");
	if (!is_standard)
		return ENOTSUP;

	/* The version, revision, key length and permissions. */
	error = read_integer(document, encrypt, "V", 0, &version);
	if (error != 0)
		return error;
	error = read_integer(document, encrypt, "R", 0, &revision);
	if (error != 0)
		return error;
	error = read_integer(document, encrypt, "Length", 40, &length);
	if (error != 0)
		return error;
	error = read_integer(document, encrypt, "P", 0, &permissions);
	if (error != 0)
		return error;
	if (revision < 2 || revision > 6)
		return ENOTSUP;
	if (version < 1 || version > 5)
		return ENOTSUP;
	if (version == 3)
		return ENOTSUP;

	/* Allocates the handler with AES's tables. */
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	created->revision = (int)revision;
	created->encrypt_metadata = 1;
	make_tables(&created->tables);

	/* The key length: 40 bits for version 1, /Length for 2 and 4, 256 bits for 5. */
	created->key_length = 5;
	if (version == 2 || version == 4) {
		whole_bytes = 0;
		if (length >= 40 && length <= 128)
			whole_bytes = (length % 8 == 0);
		if (!whole_bytes) {
			free(created);
			return PDF_EFORMAT;
		}

		/* The key is that many bytes. */
		created->key_length = (size_t)length / 8;
	}

	/* Version 5's key is always 256 bits. */
	if (version == 5)
		created->key_length = 32;

	/* The ciphers of strings and streams: RC4 before version 4, the crypt filters from it. */
	created->strings = CRYPT_RC4;
	created->streams = CRYPT_RC4;
	if (version >= 4) {
		error = read_methods(document, encrypt, created);
		if (error != 0) {
			free(created);
			return error;
		}
	}

	/* The file key from the password, by the revision's algorithms. */
	if (revision <= 4) {
		error = read_legacy_key(document, encrypt, created, permissions, id, id_length, password, password_length);
	} else {
		error = read_aes256_key(document, encrypt, created, password, password_length);
	}

	/* Reports a key that could not be made or checked. */
	if (error != 0) {
		free(created);
		return error;
	}

	/* Succeeded: the caller owns the handler. */
	*crypt = created;
	return 0;
}

/*
 * Makes a revision 2 to 4 file key from a password: as the user's, or
 * else as the owner's.
 *
 * Returns 0, EACCES for a password that is neither, or the error of an
 * encryption dictionary without /O or /U.
 */
static int
read_legacy_key(
	struct pdf_document *document,
	struct pdf_object *encrypt,
	struct pdf_crypt *crypt,
	long permissions,
	const unsigned char *id,
	size_t id_length,
	const unsigned char *password,
	size_t password_length)
{
	const unsigned char *owner;
	const unsigned char *user;
	unsigned char padded[32];
	unsigned char padded_user[32];
	int error;

	/* /O, the owner's check. */
	error = read_string(document, encrypt, "O", 32, &owner);
	if (error != 0)
		return error;

	/* /U, the user's check. */
	error = read_string(document, encrypt, "U", 32, &user);
	if (error != 0)
		return error;

	/* The password as the user's. */
	pad_password(password, password_length, padded);
	error = legacy_key(crypt, padded, owner, user, permissions, id, id_length);
	if (error != EACCES) {
		memset(padded, 0, sizeof(padded));
		return error;
	}

	/* The password as the owner's: /O decrypted with it is the padded user password. */
	legacy_owner_to_user(crypt, padded, owner, padded_user);
	error = legacy_key(crypt, padded_user, owner, user, permissions, id, id_length);
	memset(padded, 0, sizeof(padded));
	memset(padded_user, 0, sizeof(padded_user));

	/* Reports a password that is neither. */
	if (error != 0)
		return error;

	/* Succeeded: the owner's password gave the file key. */
	return 0;
}

/*
 * Makes a revision 5 or 6 file key from a password: as the user's (/U and
 * /UE), or else as the owner's (/O and /OE, with /U as the user data).
 *
 * Returns 0, EACCES for a password that is neither, or the error of an
 * encryption dictionary without the strings.
 */
static int
read_aes256_key(
	struct pdf_document *document,
	struct pdf_object *encrypt,
	struct pdf_crypt *crypt,
	const unsigned char *password,
	size_t password_length)
{
	const unsigned char *user;
	const unsigned char *user_key;
	const unsigned char *owner;
	const unsigned char *owner_key;
	int error;

	/* A longer password counts its first bytes, as revisions 5 and 6 define. */
	if (password_length > CRYPT_PASSWORD_MAX)
		password_length = CRYPT_PASSWORD_MAX;

	/* /U, the user's check and salts. */
	error = read_string(document, encrypt, "U", 48, &user);
	if (error != 0)
		return error;

	/* /UE, the file key encrypted for the user. */
	error = read_string(document, encrypt, "UE", 32, &user_key);
	if (error != 0)
		return error;

	/* The password as the user's. */
	error = aes256_key(crypt, password, password_length, user, user_key, NULL, 0);
	if (error != EACCES)
		return error;

	/* /O, the owner's check and salts. */
	error = read_string(document, encrypt, "O", 48, &owner);
	if (error != 0)
		return error;

	/* /OE, the file key encrypted for the owner. */
	error = read_string(document, encrypt, "OE", 32, &owner_key);
	if (error != 0)
		return error;

	/* The password as the owner's, whose hashes take /U's 48 bytes too. */
	error = aes256_key(crypt, password, password_length, owner, owner_key, user, CRYPT_USER_DATA);
	if (error != 0)
		return error;

	/* Succeeded: the owner's password gave the file key. */
	return 0;
}

/*
 * Frees a security handler.
 */
void
pdf_crypt_close(
	struct pdf_crypt *crypt)
{
	/* The key is cleared before the memory goes. */
	if (crypt == NULL)
		return;
	memset(crypt->key, 0, sizeof(crypt->key));
	free(crypt);
}

/*
 * Reports whether a document's metadata streams are encrypted
 * (/EncryptMetadata).
 */
int
pdf_crypt_metadata(
	const struct pdf_crypt *crypt)
{
	/* Encrypted unless the dictionary said false. */
	return crypt->encrypt_metadata;
}

/*
 * Decrypts a string or a stream's data of an object into output (which
 * has room for size bytes): RC4 keeps the length, AES drops the vector
 * and the padding.  A method of none copies the bytes.
 */
int
pdf_crypt_decrypt(
	const struct pdf_crypt *crypt,
	int stream,
	unsigned long number,
	unsigned long generation,
	const unsigned char *input,
	size_t size,
	unsigned char *output,
	size_t *output_size)
{
	unsigned char material[CRYPT_KEY_MAX + 9];
	unsigned char digest[MD5_DIGEST_LENGTH];
	enum crypt_method method;
	MD5_CTX context;
	size_t length;
	size_t key_length;
	int error;

	/* The method of strings or of streams. */
	method = crypt->strings;
	if (stream)
		method = crypt->streams;

	/* No method leaves the bytes as they are. */
	if (method == CRYPT_NONE) {
		memcpy(output, input, size);
		*output_size = size;
		return 0;
	}

	/* AES-256 uses the file key itself. */
	if (method == CRYPT_AES_256) {
		error = aes_cbc_decrypt(&crypt->tables, crypt->key, 32, input, size, output, output_size);
		if (error != 0)
			return error;
		return 0;
	}

	/* The object's key: the file key, the number and generation, and "sAlT" for AES (algorithm 1). */
	length = crypt->key_length;
	memcpy(material, crypt->key, length);
	material[length] = (unsigned char)(number & 0xff);
	material[length + 1] = (unsigned char)((number >> 8) & 0xff);
	material[length + 2] = (unsigned char)((number >> 16) & 0xff);
	material[length + 3] = (unsigned char)(generation & 0xff);
	material[length + 4] = (unsigned char)((generation >> 8) & 0xff);
	length += 5;
	if (method == CRYPT_AES_128) {
		memcpy(material + length, "sAlT", 4);
		length += 4;
	}

	/* Its MD5, at most 16 bytes of it. */
	MD5Init(&context);
	MD5Update(&context, material, length);
	MD5Final(digest, &context);
	key_length = crypt->key_length + 5;
	if (key_length > 16)
		key_length = 16;

	/* Decrypts by the method. */
	if (method == CRYPT_RC4) {
		rc4(digest, key_length, input, size, output);
		*output_size = size;
		return 0;
	}

	/* AES-128 in CBC with the object's key. */
	error = aes_cbc_decrypt(&crypt->tables, digest, 16, input, size, output, output_size);
	if (error != 0)
		return error;

	/* Succeeded: output holds the plain bytes. */
	return 0;
}

/* Reads an integer of the /Encrypt dictionary, or a default when it is absent. */
static int
read_integer(
	struct pdf_document *document,
	struct pdf_object *encrypt,
	const char *key,
	long fallback,
	long *value)
{
	struct pdf_object *found;
	int error;

	/* Finds the key's value. */
	*value = fallback;
	error = pdf_reader_resolve_key(document, encrypt, key, &found);
	if (error != 0)
		return error;

	/* A missing key gives the default; anything but an integer is malformed. */
	if (found->type == PDF_OBJECT_NULL)
		return 0;
	if (found->type != PDF_OBJECT_INTEGER)
		return PDF_EFORMAT;

	/* Succeeded: the integer. */
	*value = found->integer;
	return 0;
}

/* Reads a string of the /Encrypt dictionary that must be at least minimum bytes long. */
static int
read_string(
	struct pdf_document *document,
	struct pdf_object *encrypt,
	const char *key,
	size_t minimum,
	const unsigned char **bytes)
{
	struct pdf_object *found;
	int error;

	/* Finds the string. */
	error = pdf_reader_resolve_key(document, encrypt, key, &found);
	if (error != 0)
		return error;
	if (found->type != PDF_OBJECT_STRING)
		return PDF_EFORMAT;
	if (found->length < minimum)
		return PDF_EFORMAT;

	/* Succeeded: the string's bytes. */
	*bytes = found->bytes;
	return 0;
}

/*
 * Reads a version 4 or 5 handler's crypt filters: the methods /StrF and
 * /StmF name in /CF (/Identity when they name none).
 */
static int
read_methods(
	struct pdf_document *document,
	struct pdf_object *encrypt,
	struct pdf_crypt *crypt)
{
	struct pdf_object *filters;
	struct pdf_object *name;
	struct pdf_object *metadata;
	size_t length;
	int error;

	/* The crypt filters. */
	error = pdf_reader_resolve_key(document, encrypt, "CF", &filters);
	if (error != 0)
		return error;

	/* The strings' filter. */
	error = pdf_reader_resolve_key(document, encrypt, "StrF", &name);
	if (error != 0)
		return error;
	length = crypt->key_length;
	error = filter_method(document, filters, name, &crypt->strings, &length);
	if (error != 0)
		return error;

	/* The streams' filter, which gives the key length. */
	error = pdf_reader_resolve_key(document, encrypt, "StmF", &name);
	if (error != 0)
		return error;
	error = filter_method(document, filters, name, &crypt->streams, &length);
	if (error != 0)
		return error;
	crypt->key_length = length;

	/* Whether metadata streams are encrypted. */
	error = pdf_reader_resolve_key(document, encrypt, "EncryptMetadata", &metadata);
	if (error != 0)
		return error;
	if (metadata->type == PDF_OBJECT_BOOLEAN)
		crypt->encrypt_metadata = metadata->boolean;

	/* Succeeded: the methods are known. */
	return 0;
}

/*
 * Finds the method of the crypt filter a name names: /Identity (or none)
 * leaves the bytes, and a filter of /CF gives /V2 (RC4), /AESV2 or
 * /AESV3; its /Length (bits, or bytes as some writers put it) updates the
 * key length.
 */
static int
filter_method(
	struct pdf_document *document,
	struct pdf_object *filters,
	struct pdf_object *name,
	enum crypt_method *method,
	size_t *length)
{
	struct pdf_object *filter;
	struct pdf_object *cfm;
	struct pdf_object *bits;
	size_t index;
	int is_name;
	int differs;
	int error;

	/* No filter, or /Identity, leaves the bytes. */
	*method = CRYPT_NONE;
	if (name->type != PDF_OBJECT_NAME)
		return 0;
	is_name = pdf_object_is_name(name, "Identity");
	if (is_name)
		return 0;

	/* Finds the filter of that name. */
	if (filters->type != PDF_OBJECT_DICTIONARY)
		return PDF_EFORMAT;
	filter = NULL;
	for (index = 0; index < filters->count; index++) {
		if (filters->keys[index]->length != name->length)
			continue;
		differs = memcmp(filters->keys[index]->bytes, name->bytes, name->length);
		if (differs != 0)
			continue;
		error = pdf_reader_resolve(document, filters->values[index], &filter);
		if (error != 0)
			return error;
		break;
	}

	/* A name /CF does not have, or not a dictionary, is malformed. */
	if (filter == NULL || filter->type != PDF_OBJECT_DICTIONARY)
		return PDF_EFORMAT;

	/* Its method. */
	error = pdf_reader_resolve_key(document, filter, "CFM", &cfm);
	if (error != 0)
		return error;
	is_name = pdf_object_is_name(cfm, "V2");
	if (is_name)
		*method = CRYPT_RC4;
	is_name = pdf_object_is_name(cfm, "AESV2");
	if (is_name)
		*method = CRYPT_AES_128;
	is_name = pdf_object_is_name(cfm, "AESV3");
	if (is_name)
		*method = CRYPT_AES_256;
	is_name = pdf_object_is_name(cfm, "None");
	if (is_name)
		return 0;
	if (*method == CRYPT_NONE)
		return ENOTSUP;

	/* Its key length, in bits (or bytes, as some writers give it). */
	error = pdf_reader_resolve_key(document, filter, "Length", &bits);
	if (error != 0)
		return error;
	if (bits->type == PDF_OBJECT_INTEGER) {
		if (bits->integer >= 40 && bits->integer <= 256) {
			if (bits->integer % 8 == 0)
				*length = (size_t)bits->integer / 8;
		}

		/* A small number is the length in bytes. */
		if (bits->integer >= 5 && bits->integer <= 32)
			*length = (size_t)bits->integer;
	}

	/* AES's key lengths are its own; RC4's is at most 128 bits. */
	if (*method == CRYPT_AES_128)
		*length = 16;
	if (*method == CRYPT_AES_256)
		*length = 32;
	if (*length > 16 && *method == CRYPT_RC4)
		*length = 16;

	/* Succeeded: the filter's method. */
	return 0;
}

/* Pads a password to 32 bytes with the padding string (algorithm 2, step a): its first 32 bytes, then the padding. */
static void
pad_password(
	const unsigned char *password,
	size_t length,
	unsigned char padded[32])
{
	/* The password's bytes, at most 32. */
	if (length > 32)
		length = 32;
	if (length > 0)
		memcpy(padded, password, length);

	/* The padding's first bytes after them. */
	memcpy(padded + length, crypt_padding, 32 - length);
}

/*
 * Computes a revision 2 to 4 file key from a padded user password
 * (algorithm 2) and checks it against /U (algorithms 4 and 5).
 *
 * Returns 0 with the key in the handler, or EACCES when /U says the
 * password is not the user's.
 */
static int
legacy_key(
	struct pdf_crypt *crypt,
	const unsigned char *padded,
	const unsigned char *owner,
	const unsigned char *user,
	long permissions,
	const unsigned char *id,
	size_t id_length)
{
	static const unsigned char no_metadata[4] = { 0xff, 0xff, 0xff, 0xff };
	unsigned char digest[MD5_DIGEST_LENGTH];
	unsigned char check[32];
	unsigned char step_key[16];
	unsigned char bytes[4];
	unsigned long value;
	MD5_CTX context;
	size_t index;
	int round;
	int differs;

	/* The padded password, /O, /P low byte first, the first /ID string, and the metadata flag. */
	value = (unsigned long)permissions & 0xffffffffUL;
	bytes[0] = (unsigned char)(value & 0xff);
	bytes[1] = (unsigned char)((value >> 8) & 0xff);
	bytes[2] = (unsigned char)((value >> 16) & 0xff);
	bytes[3] = (unsigned char)((value >> 24) & 0xff);
	MD5Init(&context);
	MD5Update(&context, padded, 32);
	MD5Update(&context, owner, 32);
	MD5Update(&context, bytes, 4);
	if (id != NULL)
		MD5Update(&context, id, id_length);
	if (crypt->revision >= 4 && !crypt->encrypt_metadata)
		MD5Update(&context, no_metadata, 4);
	MD5Final(digest, &context);

	/* Revision 3 and later hash the key fifty times more. */
	if (crypt->revision >= 3) {
		for (round = 0; round < 50; round++) {
			MD5Init(&context);
			MD5Update(&context, digest, crypt->key_length);
			MD5Final(digest, &context);
		}
	}

	/* The key is the digest's first bytes. */
	memcpy(crypt->key, digest, crypt->key_length);

	/* Revision 2: /U is the padding encrypted with the key. */
	if (crypt->revision == 2) {
		rc4(crypt->key, crypt->key_length, crypt_padding, 32, check);
		differs = memcmp(check, user, 32);
		if (differs != 0)
			return EACCES;
		return 0;
	}

	/* Revisions 3 and 4: the hash of the padding and the /ID, encrypted twenty times with changed keys. */
	MD5Init(&context);
	MD5Update(&context, crypt_padding, 32);
	if (id != NULL)
		MD5Update(&context, id, id_length);
	MD5Final(digest, &context);
	rc4(crypt->key, crypt->key_length, digest, 16, check);
	for (round = 1; round <= 19; round++) {
		for (index = 0; index < crypt->key_length; index++)
			step_key[index] = (unsigned char)(crypt->key[index] ^ round);
		rc4(step_key, crypt->key_length, check, 16, check);
	}

	/* Its first 16 bytes are /U's. */
	differs = memcmp(check, user, 16);
	if (differs != 0)
		return EACCES;

	/* Succeeded: the key is the file's. */
	return 0;
}

/*
 * Decrypts /O with a padded owner password into the padded user password
 * (algorithm 7): the key is the MD5 of the owner password (hashed fifty
 * times more from revision 3), and /O is decrypted with RC4 once
 * (revision 2) or twenty times with the key changed by 19 down to 0.
 */
static void
legacy_owner_to_user(
	const struct pdf_crypt *crypt,
	const unsigned char *padded_owner,
	const unsigned char *owner,
	unsigned char padded_user[32])
{
	unsigned char digest[MD5_DIGEST_LENGTH];
	unsigned char step_key[16];
	MD5_CTX context;
	size_t index;
	int round;

	/* The hash of the padded owner password. */
	MD5Init(&context);
	MD5Update(&context, padded_owner, 32);
	MD5Final(digest, &context);

	/* Revision 3 and later hash the whole digest fifty times more. */
	if (crypt->revision >= 3) {
		for (round = 0; round < 50; round++) {
			MD5Init(&context);
			MD5Update(&context, digest, MD5_DIGEST_LENGTH);
			MD5Final(digest, &context);
		}
	}

	/* Revision 2: /O decrypted once with the key. */
	if (crypt->revision == 2) {
		rc4(digest, crypt->key_length, owner, 32, padded_user);
		memset(digest, 0, sizeof(digest));
		return;
	}

	/* Revisions 3 and 4: /O decrypted twenty times, the key changed by 19 down to 0. */
	memcpy(padded_user, owner, 32);
	for (round = 19; round >= 0; round--) {
		for (index = 0; index < crypt->key_length; index++)
			step_key[index] = (unsigned char)(digest[index] ^ round);
		rc4(step_key, crypt->key_length, padded_user, 32, padded_user);
	}

	/* The keys are cleared before the memory is reused. */
	memset(digest, 0, sizeof(digest));
	memset(step_key, 0, sizeof(step_key));
}

/*
 * Checks a password against a revision 5 or 6 check string (/U, or /O
 * with /U as the user data) and decrypts the matching encrypted key (/UE
 * or /OE) into the file key.
 *
 * The check string is a 32-byte hash, an 8-byte validation salt and an
 * 8-byte key salt.  Returns 0 with the key in the handler, or EACCES when
 * the password's hash with the validation salt is not the check's.
 */
static int
aes256_key(
	struct pdf_crypt *crypt,
	const unsigned char *password,
	size_t password_length,
	const unsigned char *check,
	const unsigned char *encrypted_key,
	const unsigned char *user_data,
	size_t user_data_length)
{
	unsigned char hash[32];
	unsigned char plain[32];
	unsigned char vector[CRYPT_BLOCK];
	unsigned char block[CRYPT_BLOCK];
	struct crypt_aes aes;
	size_t index;
	size_t byte;
	int differs;

	/* The hash of the password with the validation salt, bytes 32 to 40, must be the check's first 32. */
	aes256_hash(crypt, password, password_length, check + 32, user_data, user_data_length, hash);
	differs = memcmp(hash, check, 32);
	if (differs != 0)
		return EACCES;

	/* The hash with the key salt, bytes 40 to 48, is the key that decrypts the file key (CBC, a zero vector, no padding). */
	aes256_hash(crypt, password, password_length, check + 40, user_data, user_data_length, hash);

	/* Decrypts the key block by block. */
	aes_expand(&crypt->tables, hash, 32, &aes);
	memset(vector, 0, sizeof(vector));
	for (index = 0; index < 32; index += CRYPT_BLOCK) {
		aes_decrypt_block(&crypt->tables, &aes, encrypted_key + index, block);
		for (byte = 0; byte < CRYPT_BLOCK; byte++)
			plain[index + byte] = (unsigned char)(block[byte] ^ vector[byte]);
		memcpy(vector, encrypted_key + index, CRYPT_BLOCK);
	}

	/* Succeeded: the file key. */
	memcpy(crypt->key, plain, 32);
	crypt->key_length = 32;
	memset(plain, 0, sizeof(plain));
	memset(hash, 0, sizeof(hash));
	memset(&aes, 0, sizeof(aes));
	return 0;
}

/*
 * Hashes a password with a salt and the user data (none for the user's
 * password): SHA-256 of the three for revision 5, the hash of ISO 32000-2
 * algorithm 2.B for revision 6.
 */
static void
aes256_hash(
	const struct pdf_crypt *crypt,
	const unsigned char *password,
	size_t password_length,
	const unsigned char *salt,
	const unsigned char *user_data,
	size_t user_data_length,
	unsigned char hash[32])
{
	SHA2_CTX context;

	/* Revision 6's hash. */
	if (crypt->revision != 5) {
		revision6_hash(&crypt->tables, password, password_length, salt, user_data, user_data_length, hash);
		return;
	}

	/* Revision 5's SHA-256 of the password, the salt and the user data. */
	SHA256Init(&context);
	if (password_length > 0)
		SHA256Update(&context, password, password_length);
	SHA256Update(&context, salt, 8);
	if (user_data_length > 0)
		SHA256Update(&context, user_data, user_data_length);
	SHA256Final(hash, &context);
}

/*
 * Computes revision 6's hash of a password with a salt and the user data
 * (ISO 32000-2 algorithm 2.B): SHA-256 of the three, then rounds that
 * encrypt the password, the hash and the user data repeated 64 times with
 * AES-128 and hash the result with SHA-256, SHA-384 or SHA-512 by its
 * bytes, at least 64 rounds and until the last byte allows.
 */
static void
revision6_hash(
	const struct crypt_tables *tables,
	const unsigned char *password,
	size_t password_length,
	const unsigned char *salt,
	const unsigned char *user_data,
	size_t user_data_length,
	unsigned char hash[32])
{
	unsigned char repeated[CRYPT_HASH_INPUT];
	unsigned char key[64];
	unsigned char vector[CRYPT_BLOCK];
	unsigned char *encrypted;
	unsigned char *place;
	struct crypt_aes aes;
	SHA2_CTX context;
	size_t key_length;
	size_t piece;
	size_t total;
	size_t index;
	size_t byte;
	unsigned sum;
	int round;

	/* The first hash: the password, the salt and the user data. */
	SHA256Init(&context);
	if (password_length > 0)
		SHA256Update(&context, password, password_length);
	SHA256Update(&context, salt, 8);
	if (user_data_length > 0)
		SHA256Update(&context, user_data, user_data_length);
	SHA256Final(key, &context);
	key_length = 32;

	/* The rounds. */
	encrypted = repeated;
	for (round = 0;; round++) {
		/* The password, the hash and the user data, 64 times (a multiple of the block, since the hash is). */
		piece = password_length + key_length + user_data_length;
		total = piece * 64;
		for (index = 0; index < 64; index++) {
			place = repeated + index * piece;
			if (password_length > 0)
				memcpy(place, password, password_length);
			memcpy(place + password_length, key, key_length);
			if (user_data_length > 0)
				memcpy(place + password_length + key_length, user_data, user_data_length);
		}

		/* Encrypted with AES-128 in CBC, the hash's first half the key and its second the vector. */
		aes_expand(tables, key, 16, &aes);
		memcpy(vector, key + 16, CRYPT_BLOCK);
		for (index = 0; index + CRYPT_BLOCK <= total; index += CRYPT_BLOCK) {
			for (byte = 0; byte < CRYPT_BLOCK; byte++)
				repeated[index + byte] ^= vector[byte];
			aes_encrypt_block(tables, &aes, repeated + index, encrypted + index);
			memcpy(vector, encrypted + index, CRYPT_BLOCK);
		}

		/* The next hash by the first 16 bytes' sum modulo 3. */
		sum = 0;
		for (byte = 0; byte < 16; byte++)
			sum += encrypted[byte];
		if (sum % 3 == 0) {
			SHA256Init(&context);
			SHA256Update(&context, encrypted, total);
			SHA256Final(key, &context);
			key_length = 32;
		} else if (sum % 3 == 1) {
			SHA384Init(&context);
			SHA384Update(&context, encrypted, total);
			SHA384Final(key, &context);
			key_length = 48;
		} else {
			SHA512Init(&context);
			SHA512Update(&context, encrypted, total);
			SHA512Final(key, &context);
			key_length = 64;
		}

		/* At least 64 rounds, then until the last encrypted byte is at most the round less 32. */
		if (round >= 63) {
			if ((int)encrypted[total - 1] <= round - 31)
				break;
		}
	}

	/* The hash is the first 32 bytes. */
	memcpy(hash, key, 32);
	memset(repeated, 0, sizeof(repeated));
	memset(key, 0, sizeof(key));
}

/* Encrypts or decrypts with RC4 (the same operation); input and output may be the same. */
static void
rc4(
	const unsigned char *key,
	size_t key_length,
	const unsigned char *input,
	size_t size,
	unsigned char *output)
{
	unsigned char state[256];
	unsigned char swap;
	unsigned i;
	unsigned j;
	size_t index;

	/* The key schedule. */
	for (i = 0; i < 256; i++)
		state[i] = (unsigned char)i;
	j = 0;
	for (i = 0; i < 256; i++) {
		j = (j + state[i] + key[i % key_length]) & 0xff;
		swap = state[i];
		state[i] = state[j];
		state[j] = swap;
	}

	/* The stream, XORed with the input. */
	i = 0;
	j = 0;
	for (index = 0; index < size; index++) {
		i = (i + 1) & 0xff;
		j = (j + state[i]) & 0xff;
		swap = state[i];
		state[i] = state[j];
		state[j] = swap;
		output[index] = (unsigned char)(input[index] ^ state[(state[i] + state[j]) & 0xff]);
	}
}

/*
 * Computes AES's S-box and its inverse: each byte's inverse in GF(2^8)
 * through the affine transformation, walking the field by powers of 3.
 */
static void
make_tables(
	struct crypt_tables *tables)
{
	unsigned p;
	unsigned q;
	unsigned x;

	/* p runs through the powers of 3 while q runs through their inverses. */
	p = 1;
	q = 1;
	do {
		/* p times 3. */
		x = p << 1;
		if (p & 0x80)
			x ^= 0x1b;
		p = (p ^ x) & 0xff;

		/* q divided by 3. */
		q ^= q << 1;
		q ^= q << 2;
		q ^= q << 4;
		q &= 0xff;
		if (q & 0x80)
			q ^= 0x09;

		/* The affine transformation of the inverse. */
		x = q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4));
		tables->sbox[p] = (unsigned char)((x ^ 0x63) & 0xff);
	} while (p != 1);

	/* 0 has no inverse. */
	tables->sbox[0] = 0x63;

	/* The inverse table. */
	for (x = 0; x < 256; x++)
		tables->inverse[tables->sbox[x]] = (unsigned char)x;
}

/* Multiplies two elements of GF(2^8) modulo AES's polynomial. */
static unsigned char
multiply(
	unsigned char a,
	unsigned char b)
{
	unsigned char product;
	int bit;

	/* Shifts and adds. */
	product = 0;
	for (bit = 0; bit < 8; bit++) {
		if (b & 1)
			product ^= a;
		if (a & 0x80) {
			a = (unsigned char)((a << 1) ^ 0x1b);
		} else {
			a = (unsigned char)(a << 1);
		}

		/* The next bit of b. */
		b >>= 1;
	}

	/* The product. */
	return product;
}

/* Expands an AES key of 16 or 32 bytes into its round keys. */
static void
aes_expand(
	const struct crypt_tables *tables,
	const unsigned char *key,
	size_t key_length,
	struct crypt_aes *aes)
{
	unsigned char word[4];
	unsigned char swap;
	unsigned char round_constant;
	size_t words;
	size_t total;
	size_t index;
	int byte;

	/* 10 rounds for a 128-bit key, 14 for a 256-bit one. */
	words = key_length / 4;
	aes->rounds = (int)words + 6;
	total = (size_t)(aes->rounds + 1) * 4;
	memcpy(aes->round_keys, key, key_length);

	/* Each further word from the one before and the one a key's length back. */
	round_constant = 1;
	for (index = words; index < total; index++) {
		memcpy(word, aes->round_keys + (index - 1) * 4, 4);
		if (index % words == 0) {
			/* Rotated, substituted, and the round constant added. */
			swap = word[0];
			word[0] = tables->sbox[word[1]];
			word[1] = tables->sbox[word[2]];
			word[2] = tables->sbox[word[3]];
			word[3] = tables->sbox[swap];
			word[0] ^= round_constant;
			round_constant = multiply(round_constant, 2);
		} else if (words > 6 && index % words == 4) {
			/* A 256-bit key substitutes halfway too. */
			for (byte = 0; byte < 4; byte++)
				word[byte] = tables->sbox[word[byte]];
		}

		/* Added to the word a key's length back. */
		for (byte = 0; byte < 4; byte++)
			aes->round_keys[index * 4 + (size_t)byte] = (unsigned char)(aes->round_keys[(index - words) * 4 + (size_t)byte] ^ word[byte]);
	}
}

/* Encrypts one block with an expanded key. */
static void
aes_encrypt_block(
	const struct crypt_tables *tables,
	const struct crypt_aes *aes,
	const unsigned char input[16],
	unsigned char output[16])
{
	unsigned char state[16];
	unsigned char shifted[16];
	unsigned char column[4];
	int round;
	int index;
	int c;

	/* The first round key. */
	for (index = 0; index < 16; index++)
		state[index] = (unsigned char)(input[index] ^ aes->round_keys[index]);

	/* The rounds: substitute, shift the rows, mix the columns (not in the last), add the round key. */
	for (round = 1; round <= aes->rounds; round++) {
		/* Each byte through the S-box. */
		for (index = 0; index < 16; index++)
			state[index] = tables->sbox[state[index]];

		/* Row r turns left by r (the state is by columns). */
		for (index = 0; index < 16; index++)
			shifted[index] = state[(index + 4 * (index % 4)) % 16];
		memcpy(state, shifted, 16);

		/* Each column multiplied by the MixColumns matrix. */
		if (round != aes->rounds) {
			for (c = 0; c < 4; c++) {
				memcpy(column, state + c * 4, 4);
				state[c * 4 + 0] = (unsigned char)(multiply(column[0], 2) ^ multiply(column[1], 3) ^ column[2] ^ column[3]);
				state[c * 4 + 1] = (unsigned char)(column[0] ^ multiply(column[1], 2) ^ multiply(column[2], 3) ^ column[3]);
				state[c * 4 + 2] = (unsigned char)(column[0] ^ column[1] ^ multiply(column[2], 2) ^ multiply(column[3], 3));
				state[c * 4 + 3] = (unsigned char)(multiply(column[0], 3) ^ column[1] ^ column[2] ^ multiply(column[3], 2));
			}
		}

		/* The round key. */
		for (index = 0; index < 16; index++)
			state[index] ^= aes->round_keys[round * 16 + index];
	}

	/* The cipher block. */
	memcpy(output, state, 16);
}

/* Decrypts one block with an expanded key. */
static void
aes_decrypt_block(
	const struct crypt_tables *tables,
	const struct crypt_aes *aes,
	const unsigned char input[16],
	unsigned char output[16])
{
	unsigned char state[16];
	unsigned char shifted[16];
	unsigned char column[4];
	int round;
	int index;
	int c;

	/* The last round key. */
	for (index = 0; index < 16; index++)
		state[index] = (unsigned char)(input[index] ^ aes->round_keys[aes->rounds * 16 + index]);

	/* The rounds backwards: unshift the rows, unsubstitute, add the round key, unmix the columns (not in the last). */
	for (round = aes->rounds - 1; round >= 0; round--) {
		/* Row r turns right by r. */
		for (index = 0; index < 16; index++)
			shifted[(index + 4 * (index % 4)) % 16] = state[index];

		/* Each byte through the inverse S-box. */
		for (index = 0; index < 16; index++)
			state[index] = tables->inverse[shifted[index]];

		/* The round key. */
		for (index = 0; index < 16; index++)
			state[index] ^= aes->round_keys[round * 16 + index];

		/* Each column multiplied by the inverse MixColumns matrix. */
		if (round != 0) {
			for (c = 0; c < 4; c++) {
				memcpy(column, state + c * 4, 4);
				state[c * 4 + 0] = (unsigned char)(multiply(column[0], 14) ^ multiply(column[1], 11) ^ multiply(column[2], 13) ^ multiply(column[3], 9));
				state[c * 4 + 1] = (unsigned char)(multiply(column[0], 9) ^ multiply(column[1], 14) ^ multiply(column[2], 11) ^ multiply(column[3], 13));
				state[c * 4 + 2] = (unsigned char)(multiply(column[0], 13) ^ multiply(column[1], 9) ^ multiply(column[2], 14) ^ multiply(column[3], 11));
				state[c * 4 + 3] = (unsigned char)(multiply(column[0], 11) ^ multiply(column[1], 13) ^ multiply(column[2], 9) ^ multiply(column[3], 14));
			}
		}
	}

	/* The plain block. */
	memcpy(output, state, 16);
}

/*
 * Decrypts AES in CBC mode: the first block is the vector, and the
 * padding (PKCS #5) is dropped when it is well formed.  Data shorter than
 * two blocks gives nothing; a partial last block is left out.
 */
static int
aes_cbc_decrypt(
	const struct crypt_tables *tables,
	const unsigned char *key,
	size_t key_length,
	const unsigned char *input,
	size_t size,
	unsigned char *output,
	size_t *output_size)
{
	unsigned char vector[CRYPT_BLOCK];
	unsigned char block[CRYPT_BLOCK];
	struct crypt_aes aes;
	size_t blocks;
	size_t index;
	size_t byte;
	unsigned padding;

	/* Nothing but a vector, or less, decrypts to nothing. */
	*output_size = 0;
	if (size < 2 * CRYPT_BLOCK)
		return 0;
	blocks = size / CRYPT_BLOCK - 1;

	/* Decrypts each block after the vector. */
	aes_expand(tables, key, key_length, &aes);
	memcpy(vector, input, CRYPT_BLOCK);
	for (index = 0; index < blocks; index++) {
		aes_decrypt_block(tables, &aes, input + (index + 1) * CRYPT_BLOCK, block);
		for (byte = 0; byte < CRYPT_BLOCK; byte++)
			output[index * CRYPT_BLOCK + byte] = (unsigned char)(block[byte] ^ vector[byte]);
		memcpy(vector, input + (index + 1) * CRYPT_BLOCK, CRYPT_BLOCK);
	}

	/* Drops a well-formed padding. */
	*output_size = blocks * CRYPT_BLOCK;
	padding = output[*output_size - 1];
	if (padding >= 1 && padding <= CRYPT_BLOCK)
		*output_size -= padding;

	/* Succeeded: the plain bytes. */
	memset(&aes, 0, sizeof(aes));
	return 0;
}
