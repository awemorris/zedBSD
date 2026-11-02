/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the transcript hash and the TLS 1.3 and TLS 1.2 key schedules
 * (sslc-keysched.h).
 *
 * TLS 1.3 against RFC 8448: every HKDF-Expand-Label of sections 3 to 7
 * (the labelled values the RFC prints: the PRK, the info, the expansion),
 * the traffic keys and IVs, the PSK binder's finished key, and, for the
 * handshakes without a PSK (sections 3, 5, 6 and 7), the whole run: the
 * transcript hash at each point (section 5 with its HelloRetryRequest),
 * the Early, Handshake and Master Secrets, the four traffic secrets and
 * the Finished messages.
 *
 * Where RFC 8448 has no value (SHA-384, the 32-byte keys, the traffic
 * update, TLS 1.2), the expected values are built in this file from the
 * HMAC and HKDF primitives (checked in test-hash against RFC 2104 and RFC
 * 5869) by the formulas of RFC 8446 7.1 and RFC 5246 5, written a second
 * time here in a different shape from the implementation.
 */

#include "check.h"
#include "vecfile.h"

#include "userland/base/libssl-compat/sslc-keysched.h"

/*
 * What a record of the RFC 8448 trace holds, told by its label.
 *
 * The walk over the trace acts on the labels of the keys and secrets and
 * ignores every other one.
 */
enum label_kind {
	LABEL_OTHER,
	LABEL_PRK,
	LABEL_BINDER_HASH,
	LABEL_INFO,
	LABEL_EXPANDED,
	LABEL_KEY_EXPANDED,
	LABEL_IV_EXPANDED,
	LABEL_FINISHED
};

/* The state of the deterministic generator of the test inputs. */
static uint32_t random_state = 0x2f6e2b1dU;

/* Returns the next pseudo-random 32-bit value (xorshift). */
static uint32_t random_next(void);

/* Reads rfc8448.vec into recs; returns 0, or -1 when the file is missing. */
static int load_rfc8448(void);

/* Frees what load_rfc8448 made. */
static void free_rfc8448(void);

/* Classifies a record of the trace by its label. */
static enum label_kind label_kind_of(const char *label);

/* Walks every labelled step of the RFC: the expansions, the keys, the binder. */
static void test_rfc8448_steps(void);

/* Takes a pointer as it is, or in one draw in eight as NULL. */
static const void * maybe_missing(const void *pointer);

/* Takes an output pointer as it is, or in one draw in eight as NULL. */
static void * maybe_missing_output(void *pointer);

/* Runs the RFC 8448 handshakes. */
static void test_rfc8448(void);

/* Checks the transcript over messages of many sizes, split in many ways, either hash. */
static void test_transcript_hashing(void);

/* Checks the selection rules, the state after init, select and clear. */
static void test_transcript_select(void);

/* Checks the HelloRetryRequest replacement of the transcript for both hashes. */
static void test_transcript_hrr(void);

/* Checks HKDF-Expand-Label against the formula for both hashes, many labels, contexts and lengths. */
static void test_expand_label(void);

/* Checks every refusal of HKDF-Expand-Label, and that the output is then untouched. */
static void test_expand_label_errors(void);

/* Checks the whole schedule against the formulas for both hashes and many random inputs. */
static void test_schedule(void);

/* Checks the refusals of the schedule's steps, and that a refusal changes nothing. */
static void test_schedule_errors(void);

/* Checks the traffic keys for every TLS 1.3 suite against the formula, and the refusals. */
static void test_traffic_keys(void);

/* Checks Finished, the traffic update and their refusals against the formulas. */
static void test_finished_and_update(void);

/* Checks the PRF against the formula over sizes, labels and both hashes. */
static void test_prf(void);

/* Checks the refusals of the PRF, and that the output is then untouched. */
static void test_prf_errors(void);

/* Checks the master secret, both forms, against the PRF formula, and its refusals. */
static void test_master(void);

/* Checks the key block of every TLS 1.2 suite against the formula, with the order of the halves, and the refusals. */
static void test_key_block(void);

/* Checks the TLS 1.2 Finished against the PRF formula, both senders and hashes, and its refusals. */
static void test_tls12_finished(void);

/* Returns a random hash id, often a valid one. */
static enum cryc_hash_id random_hash(void);

/* Runs random calls of every function. */
static void test_random_calls(void);

/* Returns the next pseudo-random 32-bit value (xorshift). */
static uint32_t
random_next(void)
{
	/* The three xorshift steps. */
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}

/* Fills size bytes at out with pseudo-random bytes. */
static void random_fill(uint8_t *out, size_t size);

/* Returns 1 when size bytes at bytes are all zero. */
static int is_zero(const uint8_t *bytes, size_t size);

static size_t ref_info(const char *label, const uint8_t *context, size_t context_size, size_t length, uint8_t *info);

/* HKDF-Expand-Label by the formula, with the HKDF of libcrypto-compat. */
static int ref_expand_label(enum cryc_hash_id hash, const uint8_t *secret, const char *label, const uint8_t *context, size_t context_size, uint8_t *out, size_t length);

/* Derive-Secret(secret, label, hash of messages) by the formula. */
static void ref_derive(enum cryc_hash_id hash, const uint8_t *secret, const char *label, const uint8_t *transcript_hash, uint8_t *out);

static void ref_prf(enum cryc_hash_id hash, const uint8_t *secret, size_t secret_size, const char *label, const uint8_t *seed, size_t seed_size, uint8_t *out, size_t out_size);

/* Copies a string to the heap. */
static char * copy_string(const char *text);

static const struct rec * find(const char *section, const char *step_part, const char *label, unsigned nth);

static void run_trace(const char *section, int expect_hrr);

/* Hashes size bytes of data with hash into out (the reference). */
static void digest_of(enum cryc_hash_id hash, const uint8_t *data, size_t size, uint8_t *out);

/* Checks that the transcript hashes to the hash of data. */
static void check_transcript(const struct sslc_transcript *transcript, enum cryc_hash_id hash, const uint8_t *data, size_t size);

/* Allocates size bytes (at least 1) filled with random bytes. */
static uint8_t * random_block(size_t size);

/* Fills size bytes at out with pseudo-random bytes. */
static void
random_fill(
	uint8_t *out,
	size_t size)
{
	size_t index;

	/* One byte at a time. */
	for (index = 0; index < size; index++)
		out[index] = (uint8_t)(random_next() >> 24);
}

/* The hashes under test and their sizes. */
static const enum cryc_hash_id hash_ids[2] = { CRYC_HASH_SHA256, CRYC_HASH_SHA384 };
static const size_t hash_sizes[2] = { 32, 48 };

/* Returns 1 when size bytes at bytes are all zero. */
static int
is_zero(
	const uint8_t *bytes,
	size_t size)
{
	size_t index;

	/* Any non-zero byte. */
	for (index = 0; index < size; index++) {
		if (bytes[index] != 0)
			return 0;
	}

	/* Succeeded: every byte is zero. */
	return 1;
}

/*
 * The expected values built here.
 */

/*
 * Builds an HkdfLabel (RFC 8446 7.1) into info and returns its size:
 * uint16 length, then the label prefixed "tls13 " with a one-byte length,
 * then the context with a one-byte length.
 */
static size_t
ref_info(
	const char *label,
	const uint8_t *context,
	size_t context_size,
	size_t length,
	uint8_t *info)
{
	static const char prefix[] = "tls13 ";
	size_t position;
	size_t total;

	/* The output length, big-endian. */
	info[0] = (uint8_t)(length / 256U);
	info[1] = (uint8_t)(length % 256U);

	/* The label with its prefix. */
	total = strlen(prefix) + strlen(label);
	info[2] = (uint8_t)total;
	memcpy(info + 3, prefix, strlen(prefix));
	memcpy(info + 3 + strlen(prefix), label, strlen(label));
	position = 3 + total;

	/* The context. */
	info[position++] = (uint8_t)context_size;
	if (context_size != 0)
		memcpy(info + position, context, context_size);
	position += context_size;
	return position;
}

/* HKDF-Expand-Label by the formula, with the HKDF of libcrypto-compat. */
static int
ref_expand_label(
	enum cryc_hash_id hash,
	const uint8_t *secret,
	const char *label,
	const uint8_t *context,
	size_t context_size,
	uint8_t *out,
	size_t length)
{
	uint8_t info[600];
	size_t info_size;

	/* The info, then the expansion. */
	info_size = ref_info(label, context, context_size, length, info);
	return cryc_hkdf_expand(hash, secret, cryc_hash_size(hash), info, info_size, out, length);
}

/* Derive-Secret(secret, label, hash of messages) by the formula. */
static void
ref_derive(
	enum cryc_hash_id hash,
	const uint8_t *secret,
	const char *label,
	const uint8_t *transcript_hash,
	uint8_t *out)
{
	size_t size;
	int error;

	/* An expand-label over the transcript hash. */
	size = cryc_hash_size(hash);
	error = ref_expand_label(hash, secret, label, transcript_hash, size, out, size);
	CHECK(error == 0);
}

/* The values of a whole TLS 1.3 schedule made by the formulas. */
struct ref_schedule {
	uint8_t early[CRYC_HASH_MAX_SIZE];
	uint8_t handshake[CRYC_HASH_MAX_SIZE];
	uint8_t client_handshake[CRYC_HASH_MAX_SIZE];
	uint8_t server_handshake[CRYC_HASH_MAX_SIZE];
	uint8_t master[CRYC_HASH_MAX_SIZE];
	uint8_t client_application[CRYC_HASH_MAX_SIZE];
	uint8_t server_application[CRYC_HASH_MAX_SIZE];
};

/* Makes the schedule of RFC 8446 7.1 for a shared secret and the two transcript hashes. */
static void ref_schedule_make(enum cryc_hash_id hash, const uint8_t *shared, size_t shared_size, const uint8_t *hash_hello, const uint8_t *hash_finished, struct ref_schedule *out);

/* Makes the schedule of RFC 8446 7.1 for a shared secret and the two transcript hashes. */
static void
ref_schedule_make(
	enum cryc_hash_id hash,
	const uint8_t *shared,
	size_t shared_size,
	const uint8_t *hash_hello,
	const uint8_t *hash_finished,
	struct ref_schedule *out)
{
	uint8_t zeros[CRYC_HASH_MAX_SIZE];
	uint8_t empty[CRYC_HASH_MAX_SIZE];
	uint8_t salt[CRYC_HASH_MAX_SIZE];
	size_t size;
	int error;

	/* The Early Secret: extract with zero salt and zero input. */
	size = cryc_hash_size(hash);
	memset(zeros, 0, sizeof(zeros));
	error = cryc_hkdf_extract(hash, zeros, size, zeros, size, out->early);
	CHECK(error == 0);

	/* The hash of the empty string. */
	error = cryc_hash_digest(hash, zeros, 0, empty);
	CHECK(error == 0);

	/* The Handshake Secret: the salt is Derive-Secret(Early, "derived", ""). */
	ref_derive(hash, out->early, "derived", empty, salt);
	error = cryc_hkdf_extract(hash, salt, size, shared, shared_size, out->handshake);
	CHECK(error == 0);
	ref_derive(hash, out->handshake, "c hs traffic", hash_hello, out->client_handshake);
	ref_derive(hash, out->handshake, "s hs traffic", hash_hello, out->server_handshake);

	/* The Master Secret: the salt is Derive-Secret(Handshake, "derived", ""), the input zeros. */
	ref_derive(hash, out->handshake, "derived", empty, salt);
	error = cryc_hkdf_extract(hash, salt, size, zeros, size, out->master);
	CHECK(error == 0);
	ref_derive(hash, out->master, "c ap traffic", hash_finished, out->client_application);
	ref_derive(hash, out->master, "s ap traffic", hash_finished, out->server_application);
}

/*
 * P_hash and the PRF of RFC 5246 5 written with the one-shot HMAC over
 * concatenated buffers: A(0) = label || seed; A(i) = HMAC(secret,
 * A(i-1)); output = HMAC(secret, A(1) || A(0)) || HMAC(secret, A(2) ||
 * A(0)) ...
 */
static void
ref_prf(
	enum cryc_hash_id hash,
	const uint8_t *secret,
	size_t secret_size,
	const char *label,
	const uint8_t *seed,
	size_t seed_size,
	uint8_t *out,
	size_t out_size)
{
	static uint8_t a0[1200];
	static uint8_t joined[1300];
	uint8_t a[CRYC_HASH_MAX_SIZE];
	uint8_t next[CRYC_HASH_MAX_SIZE];
	size_t a0_size;
	size_t size;
	size_t done;
	size_t take;
	int error;

	/* A(0) is the label and the seed. */
	size = cryc_hash_size(hash);
	a0_size = strlen(label) + seed_size;
	memcpy(a0, label, strlen(label));
	if (seed_size != 0)
		memcpy(a0 + strlen(label), seed, seed_size);

	/* A(1). */
	error = cryc_hmac(hash, secret, secret_size, a0, a0_size, a);
	CHECK(error == 0);

	/* Block after block. */
	done = 0;
	while (done < out_size) {
		memcpy(joined, a, size);
		memcpy(joined + size, a0, a0_size);
		error = cryc_hmac(hash, secret, secret_size, joined, size + a0_size, next);
		CHECK(error == 0);
		take = out_size - done;
		if (take > size)
			take = size;
		memcpy(out + done, next, take);
		done += take;
		error = cryc_hmac(hash, secret, secret_size, a, size, a);
		CHECK(error == 0);
	}
}

/*
 * The RFC 8448 values.
 */

/* One labelled value of rfc8448.vec. */
struct rec {
	char *section;
	char *step;
	char *label;
	uint8_t *bytes;
	size_t size;
};

/* Checks that size bytes at got equal the record's value (and that the record is that size). */
static void check_rec(const uint8_t *got, size_t size, const struct rec *rec);

static void check_expand(const struct rec *prk, const struct rec *info, const struct rec *expanded);

/* Checks the record keys the RFC prints for a traffic secret (AES-128-GCM, SHA-256). */
static void check_traffic_keys(const struct rec *prk, const struct rec *key, const struct rec *iv);

/* All the records of rfc8448.vec, in order. */
static struct rec *recs;
static size_t rec_count;
static size_t rec_room;

/* Copies a string to the heap. */
static char *
copy_string(
	const char *text)
{
	char *copy;

	/* One allocation of the text and its end. */
	copy = malloc(strlen(text) + 1U);
	if (copy == NULL) {
		printf("FAIL out of memory\n");
		exit(EXIT_FAILURE);
	}

	/* Copies the text including its terminator. */
	strcpy(copy, text);
	return copy;
}

/* Reads rfc8448.vec into recs; returns 0, or -1 when the file is missing. */
static int
load_rfc8448(void)
{
	struct vec_file file;
	const char *text;
	size_t size;
	struct rec *grown;
	struct rec *rec;
	uint8_t *bytes;
	int error;
	int more;

	/* The file. */
	error = vec_open(&file, "rfc8448");
	if (error != 0) {
		CHECK(0);
		return -1;
	}

	/* Each record is copied (the file's buffer is reused). */
	for (;;) {
		more = vec_next(&file);
		if (!more)
			break;
		if (rec_count == rec_room) {
			rec_room = rec_room * 2U;
			if (rec_room == 0)
				rec_room = 256U;
			grown = realloc(recs, rec_room * sizeof(*recs));
			if (grown == NULL) {
				printf("FAIL out of memory\n");
				exit(EXIT_FAILURE);
			}

			/* Keeps the grown table. */
			recs = grown;
		}

		/* Takes the next free record. */
		rec = &recs[rec_count++];
		rec->section = copy_string(vec_get(&file, "section"));
		rec->step = copy_string(vec_get(&file, "step"));
		rec->label = copy_string(vec_get(&file, "label"));
		text = vec_get(&file, "hex");
		bytes = malloc(strlen(text) / 2U + 1U);
		if (bytes == NULL) {
			printf("FAIL out of memory\n");
			exit(EXIT_FAILURE);
		}

		/* Decodes the hex into the record's bytes and checks the size the file states. */
		size = hex_decode(text, bytes, strlen(text) / 2U + 1U);
		CHECK(size != (size_t)-1);
		CHECK(size == (size_t)atoi(vec_get(&file, "octets")));
		rec->bytes = bytes;
		rec->size = size;
	}

	/* Closes the vector file; succeeded: the records are loaded. */
	vec_close(&file);
	return 0;
}

/* Frees what load_rfc8448 made. */
static void
free_rfc8448(void)
{
	size_t index;

	/* Each record's strings and bytes, then the array. */
	for (index = 0; index < rec_count; index++) {
		free(recs[index].section);
		free(recs[index].step);
		free(recs[index].label);
		free(recs[index].bytes);
	}

	/* Frees the table of records. */
	free(recs);
	recs = NULL;
	rec_count = 0;
	rec_room = 0;
}

/*
 * Returns the nth record of a section (its number, "3") whose step
 * contains step_part and whose label is label, or NULL (and a failed
 * check).
 */
static const struct rec *
find(
	const char *section,
	const char *step_part,
	const char *label,
	unsigned nth)
{
	const char *contained;
	size_t index;
	size_t section_length;
	int different;

	/* The records in order. */
	section_length = strlen(section);
	for (index = 0; index < rec_count; index++) {
		different = strncmp(recs[index].section, section, section_length);
		if (different != 0)
			continue;
		if (recs[index].section[section_length] != ' ')
			continue;
		contained = strstr(recs[index].step, step_part);
		if (contained == NULL)
			continue;
		different = strcmp(recs[index].label, label);
		if (different != 0)
			continue;
		if (nth == 0)
			return &recs[index];
		nth--;
	}

	/* Reports the missing value and fails the test. */
	printf("  missing %s | %s | %s\n", section, step_part, label);
	CHECK(0);
	return NULL;
}

/* Checks that size bytes at got equal the record's value (and that the record is that size). */
static void
check_rec(
	const uint8_t *got,
	size_t size,
	const struct rec *rec)
{
	/* A missing record has already failed. */
	if (rec == NULL)
		return;
	CHECK(rec->size == size);
	if (rec->size == size)
		CHECK_BYTES(got, rec->bytes, size);
}

/*
 * Checks one HKDF-Expand-Label of the RFC: the info is parsed into its
 * label and context, the expansion by the implementation and by the HKDF
 * directly over the printed info is compared with the printed output.
 */
static void
check_expand(
	const struct rec *prk,
	const struct rec *info,
	const struct rec *expanded)
{
	uint8_t out[128];
	uint8_t direct[128];
	uint8_t rebuilt[600];
	char label[300];
	size_t label_size;
	size_t context_size;
	size_t length;
	size_t rebuilt_size;
	int error;

	/* The info's own structure: length, "tls13 " label, context. */
	if (prk == NULL || info == NULL || expanded == NULL) {
		CHECK(0);
		return;
	}

	/* Checks the sizes of the secret and the info. */
	CHECK(prk->size == 32U);
	CHECK(info->size >= 4U);
	length = (size_t)info->bytes[0] * 256U + info->bytes[1];
	label_size = info->bytes[2];
	CHECK(label_size > 6U);
	CHECK(info->size >= 4U + label_size);
	context_size = info->bytes[3U + label_size];
	CHECK(info->size == 4U + label_size + context_size);
	CHECK(length == expanded->size);
	CHECK(memcmp(info->bytes + 3, "tls13 ", 6) == 0);
	memcpy(label, info->bytes + 3 + 6, label_size - 6U);
	label[label_size - 6U] = '\0';

	/* The printed info is what the formula builds from the label and the context. */
	rebuilt_size = ref_info(label, info->bytes + 4U + label_size, context_size, length, rebuilt);
	CHECK(rebuilt_size == info->size);
	CHECK(memcmp(rebuilt, info->bytes, info->size) == 0);

	/* The HKDF over the printed info gives the printed output. */
	error = cryc_hkdf_expand(CRYC_HASH_SHA256, prk->bytes, prk->size, info->bytes, info->size, direct, length);
	CHECK(error == 0);
	check_rec(direct, length, expanded);

	/* The implementation gives it from the label and the context. */
	memset(out, 0xa5, sizeof(out));
	error = sslc_tls13_expand_label(CRYC_HASH_SHA256, prk->bytes, label, info->bytes + 4U + label_size, context_size, out, length);
	CHECK(error == 0);
	check_rec(out, length, expanded);
	CHECK(out[length] == 0xa5);
}

/* Checks the record keys the RFC prints for a traffic secret (AES-128-GCM, SHA-256). */
static void
check_traffic_keys(
	const struct rec *prk,
	const struct rec *key,
	const struct rec *iv)
{
	struct sslc_traffic_keys keys;
	const struct sslc_suite *suite;
	int error;

	/* The suite of the RFC's traces. */
	suite = sslc_tls_suite(SSLC_SUITE_AES_128_GCM_SHA256);
	CHECK(suite != NULL);
	if (suite == NULL || prk == NULL || key == NULL || iv == NULL)
		return;

	/* The key and the IV. */
	memset(&keys, 0xa5, sizeof(keys));
	error = sslc_tls13_traffic_keys(CRYC_HASH_SHA256, suite, prk->bytes, &keys);
	CHECK(error == 0);
	CHECK(keys.suite == suite);
	CHECK(keys.key_size == 16U);
	CHECK(keys.iv_size == 12U);
	check_rec(keys.key, 16U, key);
	check_rec(keys.iv, 12U, iv);
	CHECK(is_zero(keys.key + 16, sizeof(keys.key) - 16U));
}

/* Classifies a record of the trace by its label. */
static enum label_kind
label_kind_of(
	const char *label)
{
	int different;

	/* The secret the expansions start from. */
	different = strcmp(label, "PRK");
	if (different == 0)
		return LABEL_PRK;

	/* The hash a PSK binder is computed over. */
	different = strcmp(label, "binder hash");
	if (different == 0)
		return LABEL_BINDER_HASH;

	/* The info of an expansion, for a plain one, a key and an IV. */
	different = strcmp(label, "info");
	if (different == 0)
		return LABEL_INFO;
	different = strcmp(label, "key info");
	if (different == 0)
		return LABEL_INFO;
	different = strcmp(label, "iv info");
	if (different == 0)
		return LABEL_INFO;

	/* The result of an expansion, for a plain one, a key and an IV. */
	different = strcmp(label, "expanded");
	if (different == 0)
		return LABEL_EXPANDED;
	different = strcmp(label, "key expanded");
	if (different == 0)
		return LABEL_KEY_EXPANDED;
	different = strcmp(label, "iv expanded");
	if (different == 0)
		return LABEL_IV_EXPANDED;

	/* The verify_data of a PSK binder. */
	different = strcmp(label, "finished");
	if (different == 0)
		return LABEL_FINISHED;

	/* A label the walk does not act on. */
	return LABEL_OTHER;
}

/* Walks every labelled step of the RFC: the expansions, the keys, the binder. */
static void
test_rfc8448_steps(void)
{
	const struct rec *prk;
	const struct rec *info;
	const struct rec *key;
	const struct rec *binder_hash;
	const char *last_step;
	const char *last_section;
	enum label_kind kind;
	unsigned expansions;
	size_t index;
	uint8_t verify[32];
	int different;
	int new_group;
	int error;

	/* Each record in order; a new step starts a new group. */
	prk = NULL;
	info = NULL;
	key = NULL;
	binder_hash = NULL;
	last_step = "";
	last_section = "";
	expansions = 0;
	for (index = 0; index < rec_count; index++) {
		new_group = 0;
		different = strcmp(recs[index].step, last_step);
		if (different != 0)
			new_group = 1;
		different = strcmp(recs[index].section, last_section);
		if (different != 0)
			new_group = 1;
		if (new_group) {
			prk = NULL;
			info = NULL;
			key = NULL;
			binder_hash = NULL;
			last_step = recs[index].step;
			last_section = recs[index].section;
		}

		/* Notes the PRK and the binder hash by their labels. */
		kind = label_kind_of(recs[index].label);
		if (kind == LABEL_PRK) {
			prk = &recs[index];
		} else if (kind == LABEL_BINDER_HASH) {
			binder_hash = &recs[index];
		} else if (kind == LABEL_INFO) {
			info = &recs[index];
		} else if (kind == LABEL_EXPANDED ||
		    kind == LABEL_KEY_EXPANDED ||
		    kind == LABEL_IV_EXPANDED) {
			check_expand(prk, info, &recs[index]);
			expansions++;
			if (kind == LABEL_KEY_EXPANDED)
				key = &recs[index];
			if (kind == LABEL_IV_EXPANDED)
				check_traffic_keys(prk, key, &recs[index]);
		} else if (kind == LABEL_FINISHED && binder_hash != NULL) {
			/* The PSK binder: the verify_data over the binder hash with the binder key. */
			CHECK(prk != NULL);
			CHECK(binder_hash->size == 32U);
			if (prk != NULL && binder_hash->size == 32U) {
				error = sslc_tls13_finished(CRYC_HASH_SHA256, prk->bytes, binder_hash->bytes, verify);
				CHECK(error == 0);
				check_rec(verify, 32U, &recs[index]);
			}
		}
	}

	/* The RFC prints many dozens of expansions (a count so a changed vector file is noticed). */
	CHECK(expansions > 100U);
}

/* One handshake message of a trace. */
struct message {
	const struct rec *rec;
	int from_client;
};

/*
 * Runs one RFC 8448 handshake without a PSK through the transcript and
 * the schedule, comparing each secret the RFC prints.
 */
static void
run_trace(
	const char *section,
	int expect_hrr)
{
	struct message messages[16];
	struct sslc_transcript transcript;
	struct sslc_tls13_schedule schedule;
	uint8_t current[CRYC_HASH_MAX_SIZE];
	uint8_t expected[CRYC_HASH_MAX_SIZE];
	uint8_t synthetic[4 + 32];
	uint8_t digest[32];
	uint8_t verify[32];
	const struct rec *rec;
	const struct rec *vec;
	size_t count;
	size_t index;
	size_t size;
	size_t first_hello;
	size_t section_length;
	const char *contained;
	int hrr_seen;
	int different;
	int is_server_hello;
	int is_finished;
	int is_retry;
	int error;

	/* Collects the constructed handshake messages of the section in order. */
	count = 0;
	section_length = strlen(section);
	for (index = 0; index < rec_count; index++) {
		rec = &recs[index];
		different = strncmp(rec->section, section, section_length);
		if (different != 0)
			continue;
		contained = strstr(rec->step, " handshake message:");
		if (contained == NULL)
			continue;
		contained = strstr(rec->step, "construct a");
		if (contained == NULL)
			continue;
		CHECK(count < 16U);
		if (count >= 16U)
			break;
		messages[count].rec = rec;
		different = strncmp(rec->step, "{client}", 8);
		messages[count].from_client = 0;
		if (different == 0)
			messages[count].from_client = 1;
		count++;
	}

	/* Checks that the messages of the trace were all found. */
	CHECK(count >= 7U);

	/* Starts the transcript and the schedule. */
	sslc_transcript_init(&transcript);
	CHECK(sslc_transcript_hash(&transcript, current) == 0);
	error = sslc_tls13_start(&schedule, CRYC_HASH_SHA256);
	CHECK(error == 0);
	vec = find(section, "extract secret \"early\"", "secret", 0);
	check_rec(schedule.secret, 32U, vec);

	/* Each message in order. */
	hrr_seen = 0;
	first_hello = 0;
	for (index = 0; index < count; index++) {
		rec = messages[index].rec;
		size = rec->size;
		different = strcmp(rec->label, "ServerHello");
		is_server_hello = 0;
		if (different == 0)
			is_server_hello = 1;
		different = strcmp(rec->label, "Finished");
		is_finished = 0;
		if (different == 0)
			is_finished = 1;
		if (is_server_hello) {
			/* Is it a HelloRetryRequest (the special random, RFC 8446 4.1.3)? */
			is_retry = 0;
			if (size >= 38U) {
				different = memcmp(rec->bytes + 6, sslc_tls_hrr_random, 32);
				if (different == 0)
					is_retry = 1;
			}

			/* The ServerHello is a HelloRetryRequest. */
			if (is_retry) {
				/* The suite is known; the transcript is replaced by message_hash || H(ClientHello1). */
				CHECK(expect_hrr);
				CHECK(sslc_transcript_select(&transcript, CRYC_HASH_SHA256) == 0);
				error = cryc_hash_digest(CRYC_HASH_SHA256, messages[first_hello].rec->bytes, messages[first_hello].rec->size, digest);
				CHECK(error == 0);
				CHECK(sslc_transcript_hash(&transcript, current) == 32U);
				CHECK_BYTES(current, digest, 32);
				synthetic[0] = 254;
				synthetic[1] = 0;
				synthetic[2] = 0;
				synthetic[3] = 32;
				memcpy(synthetic + 4, digest, 32);
				error = cryc_hash_digest(CRYC_HASH_SHA256, synthetic, sizeof(synthetic), expected);
				CHECK(error == 0);
				CHECK(sslc_transcript_restart_hrr(&transcript) == 0);
				CHECK(sslc_transcript_hash(&transcript, current) == 32U);
				CHECK_BYTES(current, expected, 32);
				sslc_transcript_update(&transcript, rec->bytes, size);
				hrr_seen = 1;
				continue;
			}

			/* The real ServerHello: the suite is chosen, then the hello's hash is the RFC's. */
			sslc_transcript_update(&transcript, rec->bytes, size);
			CHECK(sslc_transcript_select(&transcript, CRYC_HASH_SHA256) == 0);
			CHECK(sslc_transcript_hash(&transcript, current) == 32U);
			vec = find(section, "derive secret \"tls13 c hs traffic\"", "hash", 0);
			check_rec(current, 32U, vec);
			vec = find(section, "extract secret \"handshake\"", "IKM", 0);
			if (vec == NULL)
				return;
			error = sslc_tls13_handshake(&schedule, vec->bytes, vec->size, current);
			CHECK(error == 0);
			vec = find(section, "extract secret \"handshake\"", "secret", 0);
			check_rec(schedule.secret, 32U, vec);
			vec = find(section, "derive secret \"tls13 c hs traffic\"", "expanded", 0);
			check_rec(schedule.client_handshake, 32U, vec);
			vec = find(section, "derive secret \"tls13 s hs traffic\"", "expanded", 0);
			check_rec(schedule.server_handshake, 32U, vec);
			continue;
		}

		/* The server's Finished. */
		if (is_finished && !messages[index].from_client) {
			/* The server's Finished: the verify_data over the transcript before it. */
			CHECK(sslc_transcript_hash(&transcript, current) == 32U);
			error = sslc_tls13_finished(CRYC_HASH_SHA256, schedule.server_handshake, current, verify);
			CHECK(error == 0);
			CHECK(size == 36U);
			CHECK(rec->bytes[0] == 20U);
			CHECK_BYTES(verify, rec->bytes + 4, 32);
			vec = find(section, "{server} calculate finished", "finished", 0);
			check_rec(verify, 32U, vec);

			/* The Master Secret and the application secrets over the transcript through this message. */
			sslc_transcript_update(&transcript, rec->bytes, size);
			CHECK(sslc_transcript_hash(&transcript, current) == 32U);
			vec = find(section, "derive secret \"tls13 c ap traffic\"", "hash", 0);
			check_rec(current, 32U, vec);
			error = sslc_tls13_master(&schedule, current);
			CHECK(error == 0);
			vec = find(section, "extract secret \"master\"", "secret", 0);
			check_rec(schedule.secret, 32U, vec);
			vec = find(section, "derive secret \"tls13 c ap traffic\"", "expanded", 0);
			check_rec(schedule.client_application, 32U, vec);
			vec = find(section, "derive secret \"tls13 s ap traffic\"", "expanded", 0);
			check_rec(schedule.server_application, 32U, vec);
			continue;
		}

		/* The client's Finished. */
		if (is_finished && messages[index].from_client) {
			/* The client's Finished: over the transcript before it (the client's certificate included). */
			CHECK(sslc_transcript_hash(&transcript, current) == 32U);
			error = sslc_tls13_finished(CRYC_HASH_SHA256, schedule.client_handshake, current, verify);
			CHECK(error == 0);
			CHECK(size == 36U);
			CHECK_BYTES(verify, rec->bytes + 4, 32);
			vec = find(section, "{client} calculate finished", "finished", 0);
			check_rec(verify, 32U, vec);

			/* The resumption master secret's context is the transcript through this message. */
			sslc_transcript_update(&transcript, rec->bytes, size);
			CHECK(sslc_transcript_hash(&transcript, current) == 32U);
			vec = find(section, "derive secret \"tls13 res master\"", "hash", 0);
			check_rec(current, 32U, vec);
			continue;
		}

		/* Any other message is only absorbed (the first ClientHello is remembered for a retry). */
		different = strcmp(rec->label, "ClientHello");
		if (different == 0 && index == 0)
			first_hello = index;
		sslc_transcript_update(&transcript, rec->bytes, size);
	}

	/* Checks that a HelloRetryRequest was seen when one was expected. */
	CHECK(hrr_seen == expect_hrr);

	/* The empty-string hash the "derived" steps use. */
	error = cryc_hash_digest(CRYC_HASH_SHA256, synthetic, 0, expected);
	CHECK(error == 0);
	vec = find(section, "derive secret for handshake \"tls13 derived\"", "hash", 0);
	check_rec(expected, 32U, vec);

	/* Wipes. */
	sslc_tls13_clear(&schedule);
	CHECK(is_zero((const uint8_t *)&schedule, sizeof(schedule)));
	sslc_transcript_clear(&transcript);
	CHECK(is_zero((const uint8_t *)&transcript, sizeof(transcript)));
}

/* Runs the RFC 8448 handshakes. */
static void
test_rfc8448(void)
{
	int error;

	/* The vectors are in the cache; a missing file is a failure. */
	error = load_rfc8448();
	if (error != 0)
		return;
	test_rfc8448_steps();

	/* Section 3 (simple 1-RTT), 5 (HelloRetryRequest), 6 (client authentication), 7 (compatibility mode). */
	run_trace("3", 0);
	run_trace("5", 1);
	run_trace("6", 0);
	run_trace("7", 0);
	free_rfc8448();
}

/*
 * The transcript.
 */

/* Hashes size bytes of data with hash into out (the reference). */
static void
digest_of(
	enum cryc_hash_id hash,
	const uint8_t *data,
	size_t size,
	uint8_t *out)
{
	int error;

	/* One-shot hash; the data pointer may be anything for size 0. */
	error = cryc_hash_digest(hash, data, size, out);
	CHECK(error == 0);
}

/* Checks that the transcript hashes to the hash of data. */
static void
check_transcript(
	const struct sslc_transcript *transcript,
	enum cryc_hash_id hash,
	const uint8_t *data,
	size_t size)
{
	uint8_t got[CRYC_HASH_MAX_SIZE];
	uint8_t expected[CRYC_HASH_MAX_SIZE];
	size_t written;

	/* The size and the bytes. */
	digest_of(hash, data, size, expected);
	written = sslc_transcript_hash(transcript, got);
	CHECK(written == cryc_hash_size(hash));
	CHECK_BYTES(got, expected, cryc_hash_size(hash));
}

/* Checks the transcript over messages of many sizes, split in many ways, either hash. */
static void
test_transcript_hashing(void)
{
	static const size_t sizes[] = { 0, 1, 4, 63, 64, 65, 111, 112, 119, 120, 127, 128, 129, 255, 256, 1000 };
	static uint8_t data[2200];
	struct sslc_transcript transcript;
	struct sslc_transcript snapshot;
	size_t which;
	size_t first;
	size_t second;
	size_t cut;
	unsigned round;
	int pick;

	/* The data to hash. */
	random_fill(data, sizeof(data));

	/* Each hash, selected before and after the first message, over each pair of sizes. */
	for (pick = 0; pick < 2; pick++) {
		for (first = 0; first < sizeof(sizes) / sizeof(sizes[0]); first++) {
			for (second = 0; second < sizeof(sizes) / sizeof(sizes[0]); second++) {
				/* Selected after the first message: both hashes absorbed it. */
				sslc_transcript_init(&transcript);
				sslc_transcript_update(&transcript, data, sizes[first]);
				CHECK(sslc_transcript_select(&transcript, hash_ids[pick]) == 0);
				sslc_transcript_update(&transcript, data + sizes[first], sizes[second]);
				check_transcript(&transcript, hash_ids[pick], data, sizes[first] + sizes[second]);
				sslc_transcript_clear(&transcript);

				/* Selected before any message. */
				sslc_transcript_init(&transcript);
				CHECK(sslc_transcript_select(&transcript, hash_ids[pick]) == 0);
				sslc_transcript_update(&transcript, data, sizes[first]);
				sslc_transcript_update(&transcript, data + sizes[first], sizes[second]);
				check_transcript(&transcript, hash_ids[pick], data, sizes[first] + sizes[second]);
				sslc_transcript_clear(&transcript);
			}
		}
	}

	/* A message of size 0 may come without a pointer. */
	sslc_transcript_init(&transcript);
	sslc_transcript_update(&transcript, NULL, 0);
	CHECK(sslc_transcript_select(&transcript, CRYC_HASH_SHA256) == 0);
	sslc_transcript_update(&transcript, NULL, 0);
	check_transcript(&transcript, CRYC_HASH_SHA256, data, 0);

	/* Reading the hash does not end the transcript, and a copy goes its own way. */
	sslc_transcript_update(&transcript, data, 100);
	check_transcript(&transcript, CRYC_HASH_SHA256, data, 100);
	check_transcript(&transcript, CRYC_HASH_SHA256, data, 100);
	snapshot = transcript;
	sslc_transcript_update(&transcript, data + 100, 100);
	check_transcript(&transcript, CRYC_HASH_SHA256, data, 200);
	check_transcript(&snapshot, CRYC_HASH_SHA256, data, 100);
	sslc_transcript_clear(&transcript);
	sslc_transcript_clear(&snapshot);

	/* Random splits into many messages. */
	for (round = 0; round < 200U; round++) {
		pick = (int)(random_next() % 2U);
		sslc_transcript_init(&transcript);
		cut = 0;
		which = 0;
		while (cut < 1500U) {
			first = random_next() % 200U;
			sslc_transcript_update(&transcript, data + cut, first);
			cut += first;
			if (which == 2U)
				CHECK(sslc_transcript_select(&transcript, hash_ids[pick]) == 0);
			which++;
		}

		/* Selects the hash and checks the transcript against the reference. */
		CHECK(sslc_transcript_select(&transcript, hash_ids[pick]) == 0);
		check_transcript(&transcript, hash_ids[pick], data, cut);
		sslc_transcript_clear(&transcript);
	}
}

/* Checks the selection rules, the state after init, select and clear. */
static void
test_transcript_select(void)
{
	static uint8_t zeros[sizeof(struct cryc_hash)];
	struct sslc_transcript transcript;
	struct sslc_transcript before;
	uint8_t out[64];
	uint8_t data[10];

	/* A started transcript has both hashes running and none selected. */
	random_fill(data, sizeof(data));
	sslc_transcript_init(&transcript);
	CHECK(transcript.selected == CRYC_HASH_NONE);
	CHECK(transcript.sha256.id == CRYC_HASH_SHA256);
	CHECK(transcript.sha384.id == CRYC_HASH_SHA384);

	/* No hash can be read before one is selected, and the output is left alone. */
	memset(out, 0xa5, sizeof(out));
	CHECK(sslc_transcript_hash(&transcript, out) == 0);
	CHECK(out[0] == 0xa5);
	CHECK(sslc_transcript_restart_hrr(&transcript) == EINVAL);

	/* Only the two suite hashes can be selected, and a refusal changes nothing. */
	before = transcript;
	CHECK(sslc_transcript_select(&transcript, CRYC_HASH_NONE) == EINVAL);
	CHECK(sslc_transcript_select(&transcript, CRYC_HASH_SHA512) == EINVAL);
	CHECK(sslc_transcript_select(&transcript, (enum cryc_hash_id)99) == EINVAL);
	CHECK(memcmp(&before, &transcript, sizeof(before)) == 0);

	/* After selecting SHA-256 the SHA-384 state is wiped. */
	CHECK(sslc_transcript_select(&transcript, CRYC_HASH_SHA256) == 0);
	CHECK(transcript.selected == CRYC_HASH_SHA256);
	CHECK(memcmp(&transcript.sha384, zeros, sizeof(zeros)) == 0);
	CHECK(transcript.sha256.id == CRYC_HASH_SHA256);

	/* The same hash again is allowed; the other cannot be brought back. */
	CHECK(sslc_transcript_select(&transcript, CRYC_HASH_SHA256) == 0);
	CHECK(sslc_transcript_select(&transcript, CRYC_HASH_SHA384) == EINVAL);
	CHECK(transcript.selected == CRYC_HASH_SHA256);
	sslc_transcript_update(&transcript, data, sizeof(data));
	check_transcript(&transcript, CRYC_HASH_SHA256, data, sizeof(data));

	/* The other way: SHA-384 selected wipes SHA-256. */
	sslc_transcript_init(&transcript);
	CHECK(sslc_transcript_select(&transcript, CRYC_HASH_SHA384) == 0);
	CHECK(memcmp(&transcript.sha256, zeros, sizeof(zeros)) == 0);
	CHECK(sslc_transcript_select(&transcript, CRYC_HASH_SHA384) == 0);
	CHECK(sslc_transcript_select(&transcript, CRYC_HASH_SHA256) == EINVAL);
	sslc_transcript_update(&transcript, data, sizeof(data));
	check_transcript(&transcript, CRYC_HASH_SHA384, data, sizeof(data));

	/* A cleared transcript is all zero, selects nothing and can be started again. */
	sslc_transcript_clear(&transcript);
	CHECK(is_zero((const uint8_t *)&transcript, sizeof(transcript)));
	memset(out, 0xa5, sizeof(out));
	CHECK(sslc_transcript_hash(&transcript, out) == 0);
	sslc_transcript_init(&transcript);
	CHECK(sslc_transcript_select(&transcript, CRYC_HASH_SHA256) == 0);
	check_transcript(&transcript, CRYC_HASH_SHA256, data, 0);
}

/* Checks the HelloRetryRequest replacement of the transcript for both hashes. */
static void
test_transcript_hrr(void)
{
	static uint8_t first_hello[500];
	static uint8_t retry[200];
	static uint8_t second_hello[300];
	static uint8_t synthetic[4 + CRYC_HASH_MAX_SIZE];
	static uint8_t all[1000];
	struct sslc_transcript transcript;
	uint8_t digest[CRYC_HASH_MAX_SIZE];
	size_t size;
	size_t used;
	int pick;
	int early;

	/* Fixed messages. */
	random_fill(first_hello, sizeof(first_hello));
	random_fill(retry, sizeof(retry));
	random_fill(second_hello, sizeof(second_hello));

	/* Each hash, with the suite chosen before or after ClientHello1 was absorbed. */
	for (pick = 0; pick < 2; pick++) {
		for (early = 0; early < 2; early++) {
			size = hash_sizes[pick];
			sslc_transcript_init(&transcript);
			if (early)
				CHECK(sslc_transcript_select(&transcript, hash_ids[pick]) == 0);
			sslc_transcript_update(&transcript, first_hello, sizeof(first_hello));
			if (!early)
				CHECK(sslc_transcript_select(&transcript, hash_ids[pick]) == 0);

			/* message_hash (254), a 24-bit length, then the hash of ClientHello1. */
			digest_of(hash_ids[pick], first_hello, sizeof(first_hello), digest);
			synthetic[0] = 254;
			synthetic[1] = 0;
			synthetic[2] = 0;
			synthetic[3] = (uint8_t)size;
			memcpy(synthetic + 4, digest, size);
			CHECK(sslc_transcript_restart_hrr(&transcript) == 0);
			check_transcript(&transcript, hash_ids[pick], synthetic, 4U + size);

			/* The HelloRetryRequest, the second ClientHello and the rest follow it. */
			sslc_transcript_update(&transcript, retry, sizeof(retry));
			sslc_transcript_update(&transcript, second_hello, sizeof(second_hello));
			used = 0;
			memcpy(all + used, synthetic, 4U + size);
			used += 4U + size;
			memcpy(all + used, retry, sizeof(retry));
			used += sizeof(retry);
			memcpy(all + used, second_hello, sizeof(second_hello));
			used += sizeof(second_hello);
			check_transcript(&transcript, hash_ids[pick], all, used);
			sslc_transcript_clear(&transcript);
		}
	}
}

/*
 * The TLS 1.3 functions.
 */

/* Checks HKDF-Expand-Label against the formula for both hashes, many labels, contexts and lengths. */
static void
test_expand_label(void)
{
	static const char *labels[] = { "a", "key", "iv", "finished", "traffic upd", "c hs traffic", "derived", "resumption" };
	static const size_t lengths[] = { 0, 1, 12, 16, 31, 32, 33, 48, 64, 100, 255, 1000 };
	static const size_t contexts[] = { 0, 1, 12, 32, 48, 100, 255 };
	uint8_t secret[CRYC_HASH_MAX_SIZE];
	uint8_t context[255];
	uint8_t got[2000];
	uint8_t expected[2000];
	static uint8_t big_got[12300];
	static uint8_t big_expected[12300];
	char long_label[300];
	size_t which_label;
	size_t which_length;
	size_t which_context;
	int pick;
	int error;

	/* Inputs. */
	random_fill(secret, sizeof(secret));
	random_fill(context, sizeof(context));
	CHECK(sizeof(labels) / sizeof(labels[0]) == 8U);

	/* Each combination. */
	for (pick = 0; pick < 2; pick++) {
		for (which_label = 0; which_label < sizeof(labels) / sizeof(labels[0]); which_label++) {
			for (which_length = 0; which_length < sizeof(lengths) / sizeof(lengths[0]); which_length++) {
				for (which_context = 0; which_context < sizeof(contexts) / sizeof(contexts[0]); which_context++) {
					error = ref_expand_label(hash_ids[pick], secret, labels[which_label], context, contexts[which_context], expected, lengths[which_length]);
					CHECK(error == 0);
					memset(got, 0xa5, sizeof(got));
					error = sslc_tls13_expand_label(hash_ids[pick], secret, labels[which_label], context, contexts[which_context], got, lengths[which_length]);
					CHECK(error == 0);
					CHECK_BYTES(got, expected, lengths[which_length]);
					CHECK(got[lengths[which_length]] == 0xa5);
				}
			}
		}
	}

	/* A context may be NULL when it is empty. */
	error = ref_expand_label(CRYC_HASH_SHA256, secret, "key", NULL, 0, expected, 16);
	CHECK(error == 0);
	error = sslc_tls13_expand_label(CRYC_HASH_SHA256, secret, "key", NULL, 0, got, 16);
	CHECK(error == 0);
	CHECK_BYTES(got, expected, 16);

	/* The longest label (249 characters) and the longest output (255 blocks). */
	memset(long_label, 'x', 249);
	long_label[249] = '\0';
	for (pick = 0; pick < 2; pick++) {
		error = ref_expand_label(hash_ids[pick], secret, long_label, context, 255, big_expected, 255U * hash_sizes[pick]);
		CHECK(error == 0);
		error = sslc_tls13_expand_label(hash_ids[pick], secret, long_label, context, 255, big_got, 255U * hash_sizes[pick]);
		CHECK(error == 0);
		CHECK_BYTES(big_got, big_expected, 255U * hash_sizes[pick]);
	}
}

/* Checks every refusal of HKDF-Expand-Label, and that the output is then untouched. */
static void
test_expand_label_errors(void)
{
	static uint8_t big[16400];
	uint8_t secret[CRYC_HASH_MAX_SIZE];
	uint8_t context[300];
	uint8_t out[64];
	char label[300];
	int pick;

	/* Inputs. */
	random_fill(secret, sizeof(secret));
	random_fill(context, sizeof(context));
	memset(label, 'y', 250);
	label[250] = '\0';

	/* The refusals, each leaving the output as it was. */
	memset(out, 0xa5, sizeof(out));
	CHECK(sslc_tls13_expand_label(CRYC_HASH_NONE, secret, "key", NULL, 0, out, 16) == EINVAL);
	CHECK(sslc_tls13_expand_label(CRYC_HASH_SHA512, secret, "key", NULL, 0, out, 16) == EINVAL);
	CHECK(sslc_tls13_expand_label((enum cryc_hash_id)77, secret, "key", NULL, 0, out, 16) == EINVAL);
	CHECK(sslc_tls13_expand_label(CRYC_HASH_SHA256, NULL, "key", NULL, 0, out, 16) == EINVAL);
	CHECK(sslc_tls13_expand_label(CRYC_HASH_SHA256, secret, NULL, NULL, 0, out, 16) == EINVAL);
	CHECK(sslc_tls13_expand_label(CRYC_HASH_SHA256, secret, "", NULL, 0, out, 16) == EINVAL);
	CHECK(sslc_tls13_expand_label(CRYC_HASH_SHA256, secret, label, NULL, 0, out, 16) == EINVAL);
	CHECK(sslc_tls13_expand_label(CRYC_HASH_SHA256, secret, "key", context, 256, out, 16) == EINVAL);
	CHECK(sslc_tls13_expand_label(CRYC_HASH_SHA256, secret, "key", NULL, 1, out, 16) == EINVAL);
	CHECK(sslc_tls13_expand_label(CRYC_HASH_SHA256, secret, "key", NULL, 0, NULL, 16) == EINVAL);
	CHECK(sslc_tls13_expand_label(CRYC_HASH_SHA256, secret, "key", NULL, 0, out, 255U * 32U + 1U) == EINVAL);
	CHECK(sslc_tls13_expand_label(CRYC_HASH_SHA384, secret, "key", NULL, 0, out, 255U * 48U + 1U) == EINVAL);
	CHECK(sslc_tls13_expand_label(CRYC_HASH_SHA256, secret, "key", NULL, 0, big, 70000U) == EINVAL);
	CHECK(out[0] == 0xa5 && out[63] == 0xa5);
	CHECK(big[0] == 0);

	/* A zero length with no output is fine. */
	for (pick = 0; pick < 2; pick++)
		CHECK(sslc_tls13_expand_label(hash_ids[pick], secret, "key", NULL, 0, NULL, 0) == 0);
}

/* Checks the whole schedule against the formulas for both hashes and many random inputs. */
static void
test_schedule(void)
{
	static const size_t shared_sizes[] = { 1, 31, 32, 48, 64, 65, 128 };
	struct sslc_tls13_schedule schedule;
	struct ref_schedule expected;
	uint8_t shared[128];
	uint8_t hash_hello[CRYC_HASH_MAX_SIZE];
	uint8_t hash_finished[CRYC_HASH_MAX_SIZE];
	size_t size;
	size_t which;
	unsigned round;
	int pick;
	int error;

	/* Each hash and shared-secret size, with random transcript hashes. */
	for (pick = 0; pick < 2; pick++) {
		size = hash_sizes[pick];
		for (which = 0; which < sizeof(shared_sizes) / sizeof(shared_sizes[0]); which++) {
			for (round = 0; round < 10U; round++) {
				random_fill(shared, sizeof(shared));
				random_fill(hash_hello, sizeof(hash_hello));
				random_fill(hash_finished, sizeof(hash_finished));
				ref_schedule_make(hash_ids[pick], shared, shared_sizes[which], hash_hello, hash_finished, &expected);

				/* Starts: the Early Secret; the rest of the structure is zero. */
				memset(&schedule, 0xa5, sizeof(schedule));
				error = sslc_tls13_start(&schedule, hash_ids[pick]);
				CHECK(error == 0);
				CHECK(schedule.hash == hash_ids[pick]);
				CHECK(schedule.size == size);
				CHECK_BYTES(schedule.secret, expected.early, size);
				CHECK(is_zero(schedule.secret + size, sizeof(schedule.secret) - size));
				CHECK(is_zero(schedule.client_handshake, sizeof(schedule.client_handshake)));
				CHECK(is_zero(schedule.server_application, sizeof(schedule.server_application)));

				/* The Handshake Secret and the two handshake traffic secrets. */
				error = sslc_tls13_handshake(&schedule, shared, shared_sizes[which], hash_hello);
				CHECK(error == 0);
				CHECK_BYTES(schedule.secret, expected.handshake, size);
				CHECK_BYTES(schedule.client_handshake, expected.client_handshake, size);
				CHECK_BYTES(schedule.server_handshake, expected.server_handshake, size);
				CHECK(is_zero(schedule.client_application, sizeof(schedule.client_application)));

				/* The Master Secret and the two application traffic secrets. */
				error = sslc_tls13_master(&schedule, hash_finished);
				CHECK(error == 0);
				CHECK_BYTES(schedule.secret, expected.master, size);
				CHECK_BYTES(schedule.client_application, expected.client_application, size);
				CHECK_BYTES(schedule.server_application, expected.server_application, size);
				CHECK_BYTES(schedule.client_handshake, expected.client_handshake, size);
				sslc_tls13_clear(&schedule);
				CHECK(is_zero((const uint8_t *)&schedule, sizeof(schedule)));
			}
		}
	}
}

/* Checks the refusals of the schedule's steps, and that a refusal changes nothing. */
static void
test_schedule_errors(void)
{
	struct sslc_tls13_schedule schedule;
	struct sslc_tls13_schedule before;
	uint8_t shared[200];
	uint8_t transcript_hash[CRYC_HASH_MAX_SIZE];

	/* Inputs. */
	random_fill(shared, sizeof(shared));
	random_fill(transcript_hash, sizeof(transcript_hash));

	/* Only the suites' hashes start a schedule. */
	memset(&schedule, 0x5a, sizeof(schedule));
	before = schedule;
	CHECK(sslc_tls13_start(&schedule, CRYC_HASH_NONE) == EINVAL);
	CHECK(sslc_tls13_start(&schedule, CRYC_HASH_SHA512) == EINVAL);
	CHECK(sslc_tls13_start(&schedule, (enum cryc_hash_id)9) == EINVAL);
	CHECK(memcmp(&schedule, &before, sizeof(schedule)) == 0);

	/* A schedule that was never started refuses the steps. */
	memset(&schedule, 0, sizeof(schedule));
	CHECK(sslc_tls13_handshake(&schedule, shared, 32, transcript_hash) == EINVAL);
	CHECK(sslc_tls13_master(&schedule, transcript_hash) == EINVAL);
	CHECK(is_zero((const uint8_t *)&schedule, sizeof(schedule)));

	/* A schedule whose size disagrees with its hash is refused. */
	CHECK(sslc_tls13_start(&schedule, CRYC_HASH_SHA256) == 0);
	schedule.size = 48;
	before = schedule;
	CHECK(sslc_tls13_handshake(&schedule, shared, 32, transcript_hash) == EINVAL);
	CHECK(sslc_tls13_master(&schedule, transcript_hash) == EINVAL);
	CHECK(memcmp(&schedule, &before, sizeof(schedule)) == 0);

	/* Bad arguments of the Handshake step leave the schedule as it was. */
	CHECK(sslc_tls13_start(&schedule, CRYC_HASH_SHA384) == 0);
	before = schedule;
	CHECK(sslc_tls13_handshake(&schedule, NULL, 32, transcript_hash) == EINVAL);
	CHECK(sslc_tls13_handshake(&schedule, shared, 0, transcript_hash) == EINVAL);
	CHECK(sslc_tls13_handshake(&schedule, shared, 129, transcript_hash) == EINVAL);
	CHECK(sslc_tls13_handshake(&schedule, shared, 200, transcript_hash) == EINVAL);
	CHECK(sslc_tls13_handshake(&schedule, shared, 32, NULL) == EINVAL);
	CHECK(memcmp(&schedule, &before, sizeof(schedule)) == 0);

	/* The Master step refuses a missing transcript hash and changes nothing. */
	CHECK(sslc_tls13_handshake(&schedule, shared, 128, transcript_hash) == 0);
	before = schedule;
	CHECK(sslc_tls13_master(&schedule, NULL) == EINVAL);
	CHECK(memcmp(&schedule, &before, sizeof(schedule)) == 0);
	sslc_tls13_clear(&schedule);
}

/* Checks the traffic keys for every TLS 1.3 suite against the formula, and the refusals. */
static void
test_traffic_keys(void)
{
	static const uint16_t suite_ids[3] = {
		SSLC_SUITE_AES_128_GCM_SHA256,
		SSLC_SUITE_AES_256_GCM_SHA384,
		SSLC_SUITE_CHACHA20_POLY1305_SHA256
	};
	struct sslc_traffic_keys keys;
	const struct sslc_suite *suite;
	const struct sslc_suite *suite12;
	uint8_t secret[CRYC_HASH_MAX_SIZE];
	uint8_t key[32];
	uint8_t iv[12];
	size_t which;
	unsigned round;
	int error;

	/* Each TLS 1.3 suite over random secrets. */
	for (which = 0; which < 3U; which++) {
		suite = sslc_tls_suite(suite_ids[which]);
		CHECK(suite != NULL);
		if (suite == NULL)
			continue;
		for (round = 0; round < 20U; round++) {
			random_fill(secret, sizeof(secret));
			error = ref_expand_label(suite->hash, secret, "key", NULL, 0, key, suite->key_size);
			CHECK(error == 0);
			error = ref_expand_label(suite->hash, secret, "iv", NULL, 0, iv, 12);
			CHECK(error == 0);
			memset(&keys, 0xa5, sizeof(keys));
			error = sslc_tls13_traffic_keys(suite->hash, suite, secret, &keys);
			CHECK(error == 0);
			CHECK(keys.suite == suite);
			CHECK(keys.key_size == suite->key_size);
			CHECK(keys.iv_size == 12U);
			CHECK_BYTES(keys.key, key, suite->key_size);
			CHECK_BYTES(keys.iv, iv, 12);
			CHECK(is_zero(keys.key + suite->key_size, sizeof(keys.key) - suite->key_size));
		}
	}

	/* The refusals: a suite of another version, a hash that is not the suite's, missing inputs. */
	suite = sslc_tls_suite(SSLC_SUITE_AES_128_GCM_SHA256);
	suite12 = sslc_tls_suite(SSLC_SUITE_ECDHE_RSA_WITH_AES_128_GCM_SHA256);
	CHECK(suite != NULL && suite12 != NULL);
	if (suite == NULL || suite12 == NULL)
		return;
	memset(&keys, 0xa5, sizeof(keys));
	CHECK(sslc_tls13_traffic_keys(CRYC_HASH_SHA256, suite12, secret, &keys) == EINVAL);

	/* A TLS 1.2 ChaCha20-Poly1305 suite has the hash, the 12-byte IV and the key size of a TLS 1.3 one, but is not one. */
	suite12 = sslc_tls_suite(SSLC_SUITE_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256);
	CHECK(suite12 != NULL);
	if (suite12 != NULL)
		CHECK(sslc_tls13_traffic_keys(CRYC_HASH_SHA256, suite12, secret, &keys) == EINVAL);
	suite12 = sslc_tls_suite(SSLC_SUITE_ECDHE_RSA_WITH_AES_128_GCM_SHA256);
	CHECK(sslc_tls13_traffic_keys(CRYC_HASH_SHA384, suite, secret, &keys) == EINVAL);
	CHECK(sslc_tls13_traffic_keys(CRYC_HASH_NONE, suite, secret, &keys) == EINVAL);
	CHECK(sslc_tls13_traffic_keys(CRYC_HASH_SHA512, suite, secret, &keys) == EINVAL);
	CHECK(sslc_tls13_traffic_keys(CRYC_HASH_SHA256, NULL, secret, &keys) == EINVAL);
	CHECK(sslc_tls13_traffic_keys(CRYC_HASH_SHA256, suite, NULL, &keys) == EINVAL);
	CHECK(sslc_tls13_traffic_keys(CRYC_HASH_SHA256, suite, secret, NULL) == EINVAL);
	CHECK(keys.key_size != 0U);
}

/* Checks Finished, the traffic update and their refusals against the formulas. */
static void
test_finished_and_update(void)
{
	uint8_t base_key[CRYC_HASH_MAX_SIZE];
	uint8_t transcript_hash[CRYC_HASH_MAX_SIZE];
	uint8_t finished_key[CRYC_HASH_MAX_SIZE];
	uint8_t expected[CRYC_HASH_MAX_SIZE];
	uint8_t got[CRYC_HASH_MAX_SIZE + 1];
	uint8_t secret[CRYC_HASH_MAX_SIZE];
	uint8_t next[CRYC_HASH_MAX_SIZE];
	size_t size;
	unsigned round;
	unsigned step;
	int pick;
	int error;

	/* Each hash over random keys. */
	for (pick = 0; pick < 2; pick++) {
		size = hash_sizes[pick];
		for (round = 0; round < 20U; round++) {
			random_fill(base_key, sizeof(base_key));
			random_fill(transcript_hash, sizeof(transcript_hash));

			/* Finished: the key by the formula, then the HMAC. */
			error = ref_expand_label(hash_ids[pick], base_key, "finished", NULL, 0, finished_key, size);
			CHECK(error == 0);
			error = cryc_hmac(hash_ids[pick], finished_key, size, transcript_hash, size, expected);
			CHECK(error == 0);
			memset(got, 0xa5, sizeof(got));
			error = sslc_tls13_finished(hash_ids[pick], base_key, transcript_hash, got);
			CHECK(error == 0);
			CHECK_BYTES(got, expected, size);
			CHECK(got[size] == 0xa5);

			/* The traffic update, three generations in place. */
			memcpy(secret, base_key, size);
			for (step = 0; step < 3U; step++) {
				error = ref_expand_label(hash_ids[pick], secret, "traffic upd", NULL, 0, next, size);
				CHECK(error == 0);
				error = sslc_tls13_update_secret(hash_ids[pick], secret);
				CHECK(error == 0);
				CHECK_BYTES(secret, next, size);
			}
		}
	}

	/* The refusals. */
	memset(got, 0xa5, sizeof(got));
	CHECK(sslc_tls13_finished(CRYC_HASH_NONE, base_key, transcript_hash, got) == EINVAL);
	CHECK(sslc_tls13_finished(CRYC_HASH_SHA512, base_key, transcript_hash, got) == EINVAL);
	CHECK(sslc_tls13_finished(CRYC_HASH_SHA256, NULL, transcript_hash, got) == EINVAL);
	CHECK(sslc_tls13_finished(CRYC_HASH_SHA256, base_key, NULL, got) == EINVAL);
	CHECK(sslc_tls13_finished(CRYC_HASH_SHA256, base_key, transcript_hash, NULL) == EINVAL);
	CHECK(got[0] == 0xa5);
	memcpy(secret, base_key, sizeof(secret));
	CHECK(sslc_tls13_update_secret(CRYC_HASH_NONE, secret) == EINVAL);
	CHECK(sslc_tls13_update_secret(CRYC_HASH_SHA512, secret) == EINVAL);
	CHECK(sslc_tls13_update_secret(CRYC_HASH_SHA256, NULL) == EINVAL);
	CHECK(memcmp(secret, base_key, sizeof(secret)) == 0);
}

/*
 * The TLS 1.2 functions.
 */

/* Checks the PRF against the formula over sizes, labels and both hashes. */
static void
test_prf(void)
{
	static const size_t secret_sizes[] = { 0, 1, 20, 48, 64, 65, 127, 128, 129, 200 };
	static const size_t seed_sizes[] = { 0, 1, 32, 64, 100, 500 };
	static const size_t out_sizes[] = { 0, 1, 12, 31, 32, 33, 47, 48, 49, 63, 64, 65, 88, 100, 1000, 1024 };
	static const char *labels[] = { "master secret", "key expansion", "client finished", "extended master secret", "x" };
	static uint8_t secret[256];
	static uint8_t seed[600];
	static uint8_t got[1100];
	static uint8_t expected[1100];
	static uint8_t longer[1100];
	size_t which_secret;
	size_t which_seed;
	size_t which_out;
	size_t which_label;
	int pick;
	int error;

	/* Inputs. */
	random_fill(secret, sizeof(secret));
	random_fill(seed, sizeof(seed));

	/* Each combination (the secret and seed sizes and labels are swept, the output sizes too). */
	for (pick = 0; pick < 2; pick++) {
		for (which_secret = 0; which_secret < sizeof(secret_sizes) / sizeof(secret_sizes[0]); which_secret++) {
			for (which_seed = 0; which_seed < sizeof(seed_sizes) / sizeof(seed_sizes[0]); which_seed++) {
				for (which_out = 0; which_out < sizeof(out_sizes) / sizeof(out_sizes[0]); which_out++) {
					which_label = (which_secret + which_seed + which_out) % (sizeof(labels) / sizeof(labels[0]));
					ref_prf(hash_ids[pick], secret, secret_sizes[which_secret], labels[which_label], seed, seed_sizes[which_seed], expected, out_sizes[which_out]);
					memset(got, 0xa5, sizeof(got));
					error = sslc_tls12_prf(hash_ids[pick], secret, secret_sizes[which_secret], labels[which_label], seed, seed_sizes[which_seed], got, out_sizes[which_out]);
					CHECK(error == 0);
					CHECK_BYTES(got, expected, out_sizes[which_out]);
					CHECK(got[out_sizes[which_out]] == 0xa5);
				}
			}
		}
	}

	/* A shorter output is the start of a longer one. */
	for (pick = 0; pick < 2; pick++) {
		error = sslc_tls12_prf(hash_ids[pick], secret, 48, "key expansion", seed, 64, longer, 200);
		CHECK(error == 0);
		error = sslc_tls12_prf(hash_ids[pick], secret, 48, "key expansion", seed, 64, got, 77);
		CHECK(error == 0);
		CHECK_BYTES(got, longer, 77);
	}

	/* NULL secret and seed with size 0 are allowed. */
	error = sslc_tls12_prf(CRYC_HASH_SHA256, NULL, 0, "x", NULL, 0, got, 10);
	CHECK(error == 0);
	ref_prf(CRYC_HASH_SHA256, secret, 0, "x", seed, 0, expected, 10);
	CHECK_BYTES(got, expected, 10);
}

/* Checks the refusals of the PRF, and that the output is then untouched. */
static void
test_prf_errors(void)
{
	uint8_t secret[48];
	uint8_t seed[64];
	static uint8_t out[1100];

	/* Inputs. */
	random_fill(secret, sizeof(secret));
	random_fill(seed, sizeof(seed));
	memset(out, 0xa5, sizeof(out));

	/* Each refusal. */
	CHECK(sslc_tls12_prf(CRYC_HASH_NONE, secret, 48, "x", seed, 64, out, 32) == EINVAL);
	CHECK(sslc_tls12_prf(CRYC_HASH_SHA512, secret, 48, "x", seed, 64, out, 32) == EINVAL);
	CHECK(sslc_tls12_prf((enum cryc_hash_id)200, secret, 48, "x", seed, 64, out, 32) == EINVAL);
	CHECK(sslc_tls12_prf(CRYC_HASH_SHA256, secret, 48, NULL, seed, 64, out, 32) == EINVAL);
	CHECK(sslc_tls12_prf(CRYC_HASH_SHA256, NULL, 1, "x", seed, 64, out, 32) == EINVAL);
	CHECK(sslc_tls12_prf(CRYC_HASH_SHA256, secret, 48, "x", NULL, 1, out, 32) == EINVAL);
	CHECK(sslc_tls12_prf(CRYC_HASH_SHA256, secret, 48, "x", seed, 64, NULL, 32) == EINVAL);
	CHECK(sslc_tls12_prf(CRYC_HASH_SHA256, secret, 48, "x", seed, 64, out, 1025) == EINVAL);
	CHECK(sslc_tls12_prf(CRYC_HASH_SHA384, secret, 48, "x", seed, 64, out, 100000) == EINVAL);
	CHECK(out[0] == 0xa5 && out[1024] == 0xa5);

	/* A zero-length output with no buffer is fine. */
	CHECK(sslc_tls12_prf(CRYC_HASH_SHA256, secret, 48, "x", seed, 64, NULL, 0) == 0);
}

/* Checks the master secret, both forms, against the PRF formula, and its refusals. */
static void
test_master(void)
{
	static const size_t pre_sizes[] = { 1, 32, 48, 66 };
	uint8_t pre_master[66];
	uint8_t context[64];
	uint8_t master[SSLC_TLS12_MASTER + 1];
	uint8_t expected[SSLC_TLS12_MASTER];
	size_t which;
	size_t size;
	const char *label;
	int pick;
	int extended;
	int error;

	/* Inputs. */
	random_fill(pre_master, sizeof(pre_master));
	random_fill(context, sizeof(context));

	/* Each hash, form and pre-master size. */
	for (pick = 0; pick < 2; pick++) {
		for (extended = 0; extended < 2; extended++) {
			size = 64U;
			label = "master secret";
			if (extended) {
				size = hash_sizes[pick];
				label = "extended master secret";
			}

			/* Each size of pre-master secret. */
			for (which = 0; which < sizeof(pre_sizes) / sizeof(pre_sizes[0]); which++) {
				/* The PRF of the pre-master over the label and the session hash or the randoms. */
				ref_prf(hash_ids[pick], pre_master, pre_sizes[which], label, context, size, expected, 48);
				memset(master, 0xa5, sizeof(master));
				error = sslc_tls12_master(hash_ids[pick], extended, pre_master, pre_sizes[which], context, size, master);
				CHECK(error == 0);
				CHECK_BYTES(master, expected, 48);
				CHECK(master[48] == 0xa5);
			}
		}
	}

	/* The two forms differ for the same inputs (the label is part of the PRF). */
	error = sslc_tls12_master(CRYC_HASH_SHA256, 0, pre_master, 32, context, 64, expected);
	CHECK(error == 0);
	error = sslc_tls12_master(CRYC_HASH_SHA256, 1, pre_master, 32, context, 32, master);
	CHECK(error == 0);
	CHECK(memcmp(expected, master, 48) != 0);

	/* The refusals. */
	memset(master, 0xa5, sizeof(master));
	CHECK(sslc_tls12_master(CRYC_HASH_NONE, 0, pre_master, 32, context, 64, master) == EINVAL);
	CHECK(sslc_tls12_master(CRYC_HASH_SHA512, 0, pre_master, 32, context, 64, master) == EINVAL);
	CHECK(sslc_tls12_master(CRYC_HASH_SHA256, 2, pre_master, 32, context, 64, master) == EINVAL);
	CHECK(sslc_tls12_master(CRYC_HASH_SHA256, -1, pre_master, 32, context, 64, master) == EINVAL);
	CHECK(sslc_tls12_master(CRYC_HASH_SHA256, 0, NULL, 32, context, 64, master) == EINVAL);
	CHECK(sslc_tls12_master(CRYC_HASH_SHA256, 0, pre_master, 0, context, 64, master) == EINVAL);
	CHECK(sslc_tls12_master(CRYC_HASH_SHA256, 0, pre_master, 32, NULL, 64, master) == EINVAL);
	CHECK(sslc_tls12_master(CRYC_HASH_SHA256, 0, pre_master, 32, context, 63, master) == EINVAL);
	CHECK(sslc_tls12_master(CRYC_HASH_SHA256, 0, pre_master, 32, context, 65, master) == EINVAL);
	CHECK(sslc_tls12_master(CRYC_HASH_SHA256, 0, pre_master, 32, context, 32, master) == EINVAL);
	CHECK(sslc_tls12_master(CRYC_HASH_SHA256, 1, pre_master, 32, context, 64, master) == EINVAL);
	CHECK(sslc_tls12_master(CRYC_HASH_SHA384, 1, pre_master, 32, context, 32, master) == EINVAL);
	CHECK(sslc_tls12_master(CRYC_HASH_SHA384, 1, pre_master, 32, context, 48, NULL) == EINVAL);
	CHECK(master[0] == 0xa5 && master[47] == 0xa5);
}

/* Checks the key block of every TLS 1.2 suite against the formula, with the order of the halves, and the refusals. */
static void
test_key_block(void)
{
	static const uint16_t suite_ids[6] = {
		SSLC_SUITE_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
		SSLC_SUITE_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
		SSLC_SUITE_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
		SSLC_SUITE_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
		SSLC_SUITE_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
		SSLC_SUITE_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256
	};
	struct sslc_traffic_keys client;
	struct sslc_traffic_keys server;
	struct sslc_traffic_keys before;
	const struct sslc_suite *suite;
	const struct sslc_suite *suite13;
	uint8_t master[SSLC_TLS12_MASTER];
	uint8_t client_random[32];
	uint8_t server_random[32];
	uint8_t seed[64];
	uint8_t block[88];
	size_t which;
	size_t key_size;
	size_t iv_size;
	unsigned round;
	int error;

	/* Each TLS 1.2 suite over random inputs. */
	for (which = 0; which < 6U; which++) {
		suite = sslc_tls_suite(suite_ids[which]);
		CHECK(suite != NULL);
		if (suite == NULL)
			continue;
		key_size = suite->key_size;
		iv_size = suite->fixed_iv_size;
		for (round = 0; round < 10U; round++) {
			random_fill(master, sizeof(master));
			random_fill(client_random, sizeof(client_random));
			random_fill(server_random, sizeof(server_random));

			/* The block: PRF over the server's random then the client's; split key, key, IV, IV. */
			memcpy(seed, server_random, 32);
			memcpy(seed + 32, client_random, 32);
			ref_prf(suite->hash, master, 48, "key expansion", seed, 64, block, 2U * key_size + 2U * iv_size);
			memset(&client, 0xa5, sizeof(client));
			memset(&server, 0xa5, sizeof(server));
			error = sslc_tls12_key_block(suite, master, client_random, server_random, &client, &server);
			CHECK(error == 0);
			CHECK(client.suite == suite && server.suite == suite);
			CHECK(client.key_size == key_size && server.key_size == key_size);
			CHECK(client.iv_size == iv_size && server.iv_size == iv_size);
			CHECK_BYTES(client.key, block, key_size);
			CHECK_BYTES(server.key, block + key_size, key_size);
			CHECK_BYTES(client.iv, block + 2U * key_size, iv_size);
			CHECK_BYTES(server.iv, block + 2U * key_size + iv_size, iv_size);
			CHECK(is_zero(client.key + key_size, sizeof(client.key) - key_size));
			CHECK(is_zero(server.key + key_size, sizeof(server.key) - key_size));
			CHECK(is_zero(client.iv + iv_size, sizeof(client.iv) - iv_size));
			CHECK(is_zero(server.iv + iv_size, sizeof(server.iv) - iv_size));
		}
	}

	/* The refusals leave the outputs alone. */
	suite = sslc_tls_suite(SSLC_SUITE_ECDHE_RSA_WITH_AES_128_GCM_SHA256);
	suite13 = sslc_tls_suite(SSLC_SUITE_AES_128_GCM_SHA256);
	CHECK(suite != NULL && suite13 != NULL);
	if (suite == NULL || suite13 == NULL)
		return;
	memset(&client, 0xa5, sizeof(client));
	memset(&server, 0xa5, sizeof(server));
	before = client;
	CHECK(sslc_tls12_key_block(suite13, master, client_random, server_random, &client, &server) == EINVAL);
	CHECK(sslc_tls12_key_block(NULL, master, client_random, server_random, &client, &server) == EINVAL);
	CHECK(sslc_tls12_key_block(suite, NULL, client_random, server_random, &client, &server) == EINVAL);
	CHECK(sslc_tls12_key_block(suite, master, NULL, server_random, &client, &server) == EINVAL);
	CHECK(sslc_tls12_key_block(suite, master, client_random, NULL, &client, &server) == EINVAL);
	CHECK(sslc_tls12_key_block(suite, master, client_random, server_random, NULL, &server) == EINVAL);
	CHECK(sslc_tls12_key_block(suite, master, client_random, server_random, &client, NULL) == EINVAL);
	CHECK(sslc_tls12_key_block(suite, master, client_random, server_random, &client, &client) == EINVAL);
	CHECK(memcmp(&client, &before, sizeof(before)) == 0);
	CHECK(memcmp(&server, &before, sizeof(before)) == 0);
}

/* Checks the TLS 1.2 Finished against the PRF formula, both senders and hashes, and its refusals. */
static void
test_tls12_finished(void)
{
	uint8_t master[SSLC_TLS12_MASTER];
	uint8_t transcript_hash[CRYC_HASH_MAX_SIZE];
	uint8_t got[SSLC_TLS12_VERIFY_DATA + 1];
	uint8_t expected[SSLC_TLS12_VERIFY_DATA];
	uint8_t other[SSLC_TLS12_VERIFY_DATA];
	const char *label;
	unsigned round;
	int pick;
	int from_client;
	int error;

	/* Each hash and sender. */
	for (pick = 0; pick < 2; pick++) {
		for (round = 0; round < 10U; round++) {
			random_fill(master, sizeof(master));
			random_fill(transcript_hash, sizeof(transcript_hash));
			for (from_client = 0; from_client < 2; from_client++) {
				label = "server finished";
				if (from_client)
					label = "client finished";
				ref_prf(hash_ids[pick], master, 48, label, transcript_hash, hash_sizes[pick], expected, 12);
				memset(got, 0xa5, sizeof(got));
				error = sslc_tls12_finished(hash_ids[pick], master, from_client, transcript_hash, got);
				CHECK(error == 0);
				CHECK_BYTES(got, expected, 12);
				CHECK(got[12] == 0xa5);
			}

			/* The two senders' values differ. */
			error = sslc_tls12_finished(hash_ids[pick], master, 1, transcript_hash, other);
			CHECK(error == 0);
			error = sslc_tls12_finished(hash_ids[pick], master, 0, transcript_hash, got);
			CHECK(error == 0);
			CHECK(memcmp(other, got, 12) != 0);
		}
	}

	/* Any non-zero from_client is the client's. */
	error = sslc_tls12_finished(CRYC_HASH_SHA256, master, 7, transcript_hash, other);
	CHECK(error == 0);
	error = sslc_tls12_finished(CRYC_HASH_SHA256, master, 1, transcript_hash, got);
	CHECK(error == 0);
	CHECK_BYTES(other, got, 12);

	/* The refusals. */
	memset(got, 0xa5, sizeof(got));
	CHECK(sslc_tls12_finished(CRYC_HASH_NONE, master, 1, transcript_hash, got) == EINVAL);
	CHECK(sslc_tls12_finished(CRYC_HASH_SHA512, master, 1, transcript_hash, got) == EINVAL);
	CHECK(sslc_tls12_finished(CRYC_HASH_SHA256, NULL, 1, transcript_hash, got) == EINVAL);
	CHECK(sslc_tls12_finished(CRYC_HASH_SHA256, master, 1, NULL, got) == EINVAL);
	CHECK(sslc_tls12_finished(CRYC_HASH_SHA256, master, 1, transcript_hash, NULL) == EINVAL);
	CHECK(got[0] == 0xa5);
}

/*
 * Random calls: every function with random hashes, sizes and NULLs, on
 * exact-size heap blocks, under ASan and UBSan.  Each call must answer 0
 * or EINVAL.
 */

/* Allocates size bytes (at least 1) filled with random bytes. */
static uint8_t *
random_block(
	size_t size)
{
	uint8_t *block;
	size_t room;

	/* A heap block, so that a stray read or write past it is seen. */
	room = size;
	if (size == 0)
		room = 1U;
	block = malloc(room);
	if (block == NULL) {
		printf("FAIL out of memory\n");
		exit(EXIT_FAILURE);
	}

	/* Fills the block with random bytes. */
	random_fill(block, size);
	return block;
}

/* Takes a pointer as it is, or in one draw in eight as NULL. */
static const void *
maybe_missing(
	const void *pointer)
{
	unsigned draw;

	/* One draw in eight takes the pointer away. */
	draw = random_next() % 8U;
	if (draw == 0U)
		return NULL;

	/* The pointer stays. */
	return pointer;
}

/* Takes an output pointer as it is, or in one draw in eight as NULL. */
static void *
maybe_missing_output(
	void *pointer)
{
	unsigned draw;

	/* One draw in eight takes the pointer away. */
	draw = random_next() % 8U;
	if (draw == 0U)
		return NULL;

	/* The pointer stays. */
	return pointer;
}

/* Returns a random hash id, often a valid one. */
static enum cryc_hash_id
random_hash(void)
{
	static const enum cryc_hash_id ids[6] = { CRYC_HASH_SHA256, CRYC_HASH_SHA384, CRYC_HASH_SHA256, CRYC_HASH_NONE, CRYC_HASH_SHA512, (enum cryc_hash_id)42 };

	/* One of the six. */
	return ids[random_next() % 6U];
}

/* Runs random calls of every function. */
static void
test_random_calls(void)
{
	static const char *labels[3] = { "", "key", "a label of some length" };
	struct sslc_tls13_schedule schedule;
	struct sslc_traffic_keys keys;
	struct sslc_traffic_keys keys_two;
	const struct sslc_suite *suite;
	enum cryc_hash_id hash;
	uint8_t *first;
	uint8_t *second;
	uint8_t *third;
	uint8_t *out;
	size_t hash_size;
	size_t size_one;
	size_t size_two;
	size_t size_three;
	char *label;
	const uint8_t *first_in;
	const uint8_t *second_in;
	const uint8_t *third_in;
	const char *label_in;
	const struct sslc_suite *suite_in;
	uint8_t *out_arg;
	uint8_t *secret_arg;
	struct sslc_traffic_keys *keys_arg;
	unsigned round;
	unsigned draw;
	int error;
	int pick;
	int mode;

	/* Many rounds of each. */
	for (round = 0; round < 3000U; round++) {
		hash = random_hash();
		hash_size = cryc_hash_size(hash);
		if (hash_size == 0)
			hash_size = 32;
		size_one = random_next() % 300U;
		size_two = random_next() % 300U;

		/* expand_label with a label of random length, sometimes missing the pointers. */
		first = random_block(hash_size);
		second = random_block(size_one);
		out = random_block(size_two);
		label = malloc(300U);
		memset(label, 'q', 299);
		label[random_next() % 300U] = '\0';
		first_in = maybe_missing(first);
		label_in = maybe_missing(label);
		second_in = maybe_missing(second);
		out_arg = maybe_missing_output(out);
		error = sslc_tls13_expand_label(hash, first_in, label_in, second_in, size_one, out_arg, size_two);
		CHECK(error == 0 || error == EINVAL);
		free(label);
		free(out);
		free(first);
		free(second);

		/* The schedule's steps. */
		first = random_block(hash_size);
		second = random_block(size_one);
		memset(&schedule, 0, sizeof(schedule));
		draw = random_next() % 4U;
		if (draw != 0U)
			error = sslc_tls13_start(&schedule, hash);
		else
			error = EINVAL;
		CHECK(error == 0 || error == EINVAL);
		second_in = maybe_missing(second);
		first_in = maybe_missing(first);
		error = sslc_tls13_handshake(&schedule, second_in, size_one, first_in);
		CHECK(error == 0 || error == EINVAL);
		first_in = maybe_missing(first);
		error = sslc_tls13_master(&schedule, first_in);
		CHECK(error == 0 || error == EINVAL);
		sslc_tls13_clear(&schedule);
		free(first);
		free(second);

		/* Keys, Finished and update. */
		first = random_block(hash_size);
		second = random_block(hash_size);
		out = random_block(hash_size);
		draw = random_next() % 2U;
		if (draw != 0U)
			suite = sslc_tls_suite(SSLC_SUITE_AES_128_GCM_SHA256);
		else
			suite = sslc_tls_suite(SSLC_SUITE_AES_256_GCM_SHA384);
		suite_in = maybe_missing(suite);
		first_in = maybe_missing(first);
		error = sslc_tls13_traffic_keys(hash, suite_in, first_in, &keys);
		CHECK(error == 0 || error == EINVAL);
		first_in = maybe_missing(first);
		second_in = maybe_missing(second);
		out_arg = maybe_missing_output(out);
		error = sslc_tls13_finished(hash, first_in, second_in, out_arg);
		CHECK(error == 0 || error == EINVAL);
		secret_arg = maybe_missing_output(first);
		error = sslc_tls13_update_secret(hash, secret_arg);
		CHECK(error == 0 || error == EINVAL);
		free(first);
		free(second);
		free(out);

		/* The PRF with sizes up to 300 beyond or within the bound. */
		first = random_block(size_one);
		second = random_block(size_two);
		size_three = random_next() % 1100U;
		out = random_block(size_three);
		pick = (int)(random_next() % 3U);
		first_in = maybe_missing(first);
		second_in = maybe_missing(second);
		error = sslc_tls12_prf(hash, first_in, size_one, labels[pick], second_in, size_two, out, size_three);
		CHECK(error == 0 || error == EINVAL);
		free(first);
		free(second);
		free(out);

		/* The master secret and the TLS 1.2 Finished. */
		first = random_block(size_one);
		draw = random_next() % 2U;
		size_three = hash_size;
		if (draw != 0U)
			size_three = 64U;
		second = random_block(size_three);
		out = random_block(48);
		mode = (int)(random_next() % 4U) - 1;
		first_in = maybe_missing(first);
		second_in = maybe_missing(second);
		error = sslc_tls12_master(hash, mode, first_in, size_one, second_in, size_three, out);
		CHECK(error == 0 || error == EINVAL);
		free(first);
		free(second);
		free(out);
		first = random_block(48);
		second = random_block(hash_size);
		out = random_block(12);
		first_in = maybe_missing(first);
		mode = (int)(random_next() % 3U);
		second_in = maybe_missing(second);
		out_arg = maybe_missing_output(out);
		error = sslc_tls12_finished(hash, first_in, mode, second_in, out_arg);
		CHECK(error == 0 || error == EINVAL);
		free(first);
		free(second);
		free(out);

		/* The key block. */
		first = random_block(48);
		second = random_block(32);
		third = random_block(32);
		draw = random_next() % 2U;
		if (draw != 0U)
			suite = sslc_tls_suite(SSLC_SUITE_ECDHE_RSA_WITH_AES_128_GCM_SHA256);
		else
			suite = sslc_tls_suite(SSLC_SUITE_AES_128_GCM_SHA256);
		suite_in = maybe_missing(suite);
		first_in = maybe_missing(first);
		second_in = maybe_missing(second);
		third_in = maybe_missing(third);
		draw = random_next() % 8U;
		keys_arg = &keys_two;
		if (draw == 0U)
			keys_arg = &keys;
		error = sslc_tls12_key_block(suite_in, first_in, second_in, third_in, &keys, keys_arg);
		CHECK(error == 0 || error == EINVAL);
		free(first);
		free(second);
		free(third);
	}
}

/* Runs the checks. */
int
main(void)
{
	/* The RFC 8448 traces and every expansion in them. */
	test_rfc8448();

	/* The transcript. */
	test_transcript_hashing();
	test_transcript_select();
	test_transcript_hrr();

	/* TLS 1.3. */
	test_expand_label();
	test_expand_label_errors();
	test_schedule();
	test_schedule_errors();
	test_traffic_keys();
	test_finished_and_update();

	/* TLS 1.2. */
	test_prf();
	test_prf_errors();
	test_master();
	test_key_block();
	test_tls12_finished();

	/* Hostile and random arguments. */
	test_random_calls();

	/* The last line. */
	return check_finish("test-keysched");
}
