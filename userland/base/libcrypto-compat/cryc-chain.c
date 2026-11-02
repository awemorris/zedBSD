/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The trust store and certificate path validation.
 */

#include "cryc-chain.h"

#include "cryc-hash.h"
#include "cryc-pem.h"
#include "cryc-rsa.h"

#include <stdlib.h>
#include <string.h>

/* The most nodes of the search a validation expands (a node is one certificate whose issuers are looked for); the signature bound alone does not stop a walk over paths whose signatures are already known. */
#define CHAIN_MAX_NODES		1024U

/* The first number of entries or marks a store's arrays are made for. */
#define CHAIN_FIRST_CAPACITY	16U

/*
 * A snapshot of a path: the certificates of its first
 * CRYC_CHAIN_MAX_DEPTH positions.  It lives inside one validation.
 */
struct chain_snapshot {
	const struct cryc_x509 *chain[CRYC_CHAIN_MAX_DEPTH];
	size_t count;
};

/*
 * One certificate of the path being searched: the certificate (a view into
 * the peer's table or the store's entry) and whether the SHA-256 of its DER
 * is marked distrusted.
 */
struct chain_node {
	const struct cryc_x509 *cert;
	int distrusted;
};

/*
 * The remembered outcome of verifying one certificate's signature by one
 * issuer's key, so that the same pair is never verified (and counted) twice.
 */
struct chain_edge {
	const struct cryc_x509 *child;
	const struct cryc_x509 *issuer;
	enum cryc_verify_code code;
};

/*
 * The state of one validation: the inputs, the path being walked, the
 * signature verifications already made, and what the walk has found so
 * far.  It lives for one call of cryc_chain_verify, which holds the
 * store's lock all that time.
 */
struct chain_search {
	struct cryc_store *store;
	const struct cryc_x509 *peer;
	size_t peer_count;
	const struct cryc_verify_params *params;
	int64_t now;
	size_t max_depth;
	uint8_t peer_digest[CRYC_CHAIN_MAX_PEER][CRYC_SHA256_SIZE];
	int peer_skip[CRYC_CHAIN_MAX_PEER];
	struct chain_node path[CRYC_CHAIN_MAX_DEPTH + 1U];
	size_t count;
	struct chain_edge edges[CRYC_CHAIN_MAX_WORK];
	size_t edge_count;
	size_t work;
	size_t nodes;
	int exhausted;
	size_t exhausted_depth;
	int done;
	enum cryc_verify_code done_code;
	struct chain_snapshot done_chain;
	int has_first;
	enum cryc_verify_code first_code;
	size_t first_depth;
	struct chain_snapshot first_chain;
	struct chain_snapshot longest;
	int dead_end_self_issued;
	size_t dead_end_depth;
};

/* Forward declarations of the static functions. */
static int store_find_entry(const struct cryc_store *store, const uint8_t *digest, size_t der_size, const uint8_t *der);
static int store_make_entry(const uint8_t *der, size_t size, struct cryc_store_entry **entry);
static void store_free_entry(struct cryc_store_entry *entry);
static int store_insert(struct cryc_store *store, struct cryc_store_entry *entry, int *added);
static int store_add_entry(struct cryc_store *store, const uint8_t *der, size_t size, int *added);
static int store_system_distrusted(void *context, const uint8_t *der, size_t size);
static int store_system_der(void *context, const uint8_t *der, size_t size);
static int store_system_file(void *context, const char *path);
static void chain_snapshot_take(struct chain_snapshot *snapshot, const struct chain_search *search);
static int chain_is_distrusted(const struct cryc_store *store, const uint8_t *digest, const struct cryc_x509 *cert);
static int chain_same_der(const struct cryc_x509 *left, const struct cryc_x509 *right);
static int chain_self_issued(const struct cryc_x509 *cert);
static int chain_issued_by(const struct cryc_x509 *cert, const struct cryc_x509 *issuer);
static size_t chain_rsa_bits(const struct cryc_pubkey *key);
static int chain_path_rejected(const struct chain_search *search);
static enum cryc_verify_code chain_edge_code(struct chain_search *search, const struct cryc_x509 *child, const struct cryc_x509 *issuer);
static enum cryc_verify_code chain_check_time(const struct cryc_x509 *cert, int64_t now);
static enum cryc_verify_code chain_check_names(const struct chain_search *search, size_t index);
static int chain_allows_server_auth(const struct cryc_x509 *cert);
static enum cryc_verify_code chain_check_intermediate(const struct chain_search *search, size_t index);
static enum cryc_verify_code chain_check_leaf(const struct cryc_x509 *cert);
static enum cryc_verify_code chain_validate_certificate(struct chain_search *search, size_t index);
static enum cryc_verify_code chain_validate_path(struct chain_search *search, size_t *depth);
static int chain_peer_usable(const struct chain_search *search, size_t peer_index);
static void chain_complete(struct chain_search *search);
static void chain_extend(struct chain_search *search);
static int chain_search_prepare(struct chain_search *search);
static void chain_copy_snapshot(const struct chain_snapshot *snapshot, struct cryc_verify_result *result);
static void chain_fill_result(const struct chain_search *search, struct cryc_verify_result *result);

/*
 * Makes an empty store.
 */
int
cryc_store_init(
	struct cryc_store *store)
{
	int error;

	/* Refuses a missing store. */
	if (store == NULL)
		return EINVAL;

	/* Starts with no entries and no marks. */
	memset(store, 0, sizeof(*store));

	/* Makes the lock that guards the arrays. */
	error = cryc_os_mutex_init(&store->lock);
	if (error != 0)
		return error;

	/* Succeeded: an empty store. */
	return 0;
}

/*
 * Frees every entry, the distrusted array and the lock; the store must be
 * made again before reuse.
 */
void
cryc_store_free(
	struct cryc_store *store)
{
	size_t index;

	/* Refuses a missing store. */
	if (store == NULL)
		return;

	/* Frees each entry with its copy of the DER. */
	for (index = 0; index < store->count; index++)
		store_free_entry(store->entries[index]);

	/* Frees the two arrays and the lock, and forgets them. */
	free(store->entries);
	free(store->distrusted);
	cryc_os_mutex_destroy(&store->lock);
	memset(store, 0, sizeof(*store));
}

/*
 * Adds one certificate from its DER (copied).
 *
 * A certificate already in the store (same SHA-256 of the DER) is not
 * added again and is not an error.
 */
int
cryc_store_add_der(
	struct cryc_store *store,
	const uint8_t *der,
	size_t size)
{
	int added;
	int error;

	/* Adds it; whether it was new is not reported here. */
	added = 0;
	error = store_add_entry(store, der, size, &added);
	if (error != 0)
		return error;

	/* Succeeded: the certificate is in the store. */
	return 0;
}

/*
 * Marks one certificate as distrusted, from its DER: the mark is the
 * SHA-256 of its tbsCertificate when the DER parses (what its issuer
 * signed: a certificate whose signature is encoded otherwise, as an ECDSA
 * (r, n - s) is, keeps the mark), else of the whole DER.
 */
int
cryc_store_add_distrusted(
	struct cryc_store *store,
	const uint8_t *der,
	size_t size)
{
	uint8_t digest[CRYC_SHA256_SIZE];
	uint8_t (*grown)[CRYC_SHA256_SIZE];
	struct cryc_x509 parsed;
	size_t index;
	size_t capacity;
	int error;

	/* Refuses a missing store and an empty certificate. */
	if (store == NULL || der == NULL || size == 0U)
		return EINVAL;

	/* The mark: the SHA-256 of the tbsCertificate, or of the whole DER when it does not parse. */
	if (cryc_x509_parse(&parsed, der, size) == 0)
		error = cryc_hash_digest(CRYC_HASH_SHA256, parsed.tbs, parsed.tbs_size, digest);
	else
		error = cryc_hash_digest(CRYC_HASH_SHA256, der, size, digest);
	if (error != 0)
		return error;

	/* Looks for the mark and adds it when it is new, under the store's lock. */
	cryc_os_mutex_lock(&store->lock);

	/* A value already marked is not marked again, and that is not an error. */
	for (index = 0; index < store->distrusted_count; index++) {
		if (memcmp(store->distrusted[index], digest, CRYC_SHA256_SIZE) == 0) {
			cryc_os_mutex_unlock(&store->lock);
			return 0;
		}
	}

	/* Refuses a mark beyond the store's limit. */
	if (store->distrusted_count >= CRYC_STORE_MAX) {
		cryc_os_mutex_unlock(&store->lock);
		return ENOBUFS;
	}

	/* Makes the array larger when it is full. */
	if (store->distrusted_count >= store->distrusted_capacity) {
		capacity = store->distrusted_capacity * 2U;
		if (capacity < CHAIN_FIRST_CAPACITY)
			capacity = CHAIN_FIRST_CAPACITY;
		if (capacity > CRYC_STORE_MAX)
			capacity = CRYC_STORE_MAX;
		grown = realloc(store->distrusted, capacity * CRYC_SHA256_SIZE);
		if (grown == NULL) {
			cryc_os_mutex_unlock(&store->lock);
			return ENOMEM;
		}
		store->distrusted = grown;
		store->distrusted_capacity = capacity;
	}

	/* Records the new mark. */
	memcpy(store->distrusted[store->distrusted_count], digest, CRYC_SHA256_SIZE);
	store->distrusted_count++;

	cryc_os_mutex_unlock(&store->lock);

	/* Succeeded: the certificate is distrusted. */
	return 0;
}

/*
 * Adds every CERTIFICATE block of a PEM text (cryc_pem_next_certificate).
 *
 * A block that does not decode or whose certificate does not parse is
 * skipped and remembered; the good ones are kept.
 */
int
cryc_store_add_pem(
	struct cryc_store *store,
	const char *text,
	size_t size,
	size_t *added)
{
	uint8_t *der;
	size_t der_size;
	size_t position;
	size_t previous;
	size_t total;
	size_t blocks;
	int bad;
	int was_added;
	int error;

	/* Nothing is added yet. */
	total = 0;
	if (added != NULL)
		*added = 0;

	/* Refuses a missing store, and text that is missing though it has a size. */
	if (store == NULL)
		return EINVAL;
	if (text == NULL && size != 0U)
		return EINVAL;

	/* Reads each block in turn; a block that does not decode is counted and skipped. */
	bad = 0;
	blocks = 0;
	position = 0;
	while (position < size) {
		previous = position;
		der = NULL;
		der_size = 0;

		/* Finds and decodes the next CERTIFICATE block. */
		error = cryc_pem_next_certificate(text, size, &position, &der, &der_size);
		if (error == ENOENT)
			break;
		if (error == ENOMEM)
			return ENOMEM;

		/* A block that does not decode is remembered; the text goes on after its BEGIN line. */
		if (error != 0) {
			blocks++;
			bad = 1;
			if (position <= previous)
				break;
			continue;
		}

		/* Puts the certificate into the store. */
		blocks++;
		was_added = 0;
		error = store_add_entry(store, der, der_size, &was_added);
		free(der);
		if (error == EINVAL) {
			bad = 1;
		} else if (error != 0) {
			if (added != NULL)
				*added = total;
			return error;
		}
		if (was_added != 0)
			total++;

		/* Ends a text whose position did not move, which cannot happen after a block was read. */
		if (position <= previous)
			break;
	}

	/* Reports how many certificates are new. */
	if (added != NULL)
		*added = total;

	/* Reports a text with no block at all. */
	if (blocks == 0U)
		return ENOENT;

	/* Reports that some block was bad. */
	if (bad != 0)
		return EINVAL;

	/* Succeeded: every block was good. */
	return 0;
}

/*
 * Adds the CERTIFICATE blocks of a PEM file (read with cryc_os_read_file,
 * at most CRYC_OS_FILE_MAX bytes).
 */
int
cryc_store_add_file(
	struct cryc_store *store,
	const char *path)
{
	uint8_t *data;
	size_t size;
	size_t added;
	int error;

	/* Refuses a missing store or path. */
	if (store == NULL || path == NULL)
		return EINVAL;

	/* Reads the whole file before the lock is taken. */
	data = NULL;
	size = 0;
	error = cryc_os_read_file(path, CRYC_OS_FILE_MAX, &data, &size);
	if (error != 0)
		return error;

	/* Adds the file's blocks. */
	added = 0;
	error = cryc_store_add_pem(store, (const char *)data, size, &added);
	free(data);
	if (error != 0)
		return error;

	/* Succeeded: every block of the file was good. */
	return 0;
}

/*
 * Adds the system's roots and distrusted certificates
 * (cryc_os_system_roots, with this store's add_distrusted, add_der and
 * add_file).
 */
int
cryc_store_add_system(
	struct cryc_store *store)
{
	int error;

	/* Refuses a missing store. */
	if (store == NULL)
		return EINVAL;

	/* Hands the operating system's trust decisions to the store; no lock is held here, each callback takes it for its own change. */
	error = cryc_os_system_roots(store_system_distrusted, store_system_der, store_system_file, store);
	if (error == ENOENT)
		return 0;
	if (error != 0)
		return error;

	/* Succeeded: the system's roots are in the store. */
	return 0;
}

/*
 * Validates a server's chain.
 */
int
cryc_chain_verify(
	struct cryc_store *store,
	const struct cryc_x509 *peer,
	size_t peer_count,
	const struct cryc_verify_params *params,
	struct cryc_verify_result *result)
{
	struct chain_search search;
	size_t max_depth;
	int error;

	/* Refuses a missing result, which is the one thing the call must write. */
	if (result == NULL)
		return EINVAL;

	/* The outcome is written whatever happens. */
	memset(result, 0, sizeof(*result));

	/* Refuses a missing store, parameter block or peer table. */
	if (store == NULL || params == NULL) {
		result->code = CRYC_VERIFY_MALFORMED;
		return EINVAL;
	}
	if (peer == NULL && peer_count != 0U) {
		result->code = CRYC_VERIFY_MALFORMED;
		return EINVAL;
	}

	/* A server that sent nothing has no chain. */
	if (peer_count == 0U) {
		result->code = CRYC_VERIFY_NO_PEER_CERTIFICATE;
		return 0;
	}

	/* A server that sent more certificates than the limit is refused. */
	if (peer_count > CRYC_CHAIN_MAX_PEER) {
		result->code = CRYC_VERIFY_MALFORMED;
		return 0;
	}

	/* Takes the path depth limit: 0 means the largest, and a value outside 2 to the largest is refused. */
	max_depth = params->max_depth;
	if (max_depth == 0U)
		max_depth = CRYC_CHAIN_MAX_DEPTH;
	if (max_depth < 2U || max_depth > CRYC_CHAIN_MAX_DEPTH) {
		result->code = CRYC_VERIFY_MALFORMED;
		return 0;
	}

	/* A name and an address together are refused: at most one is matched. */
	if (params->has_host != 0 && params->ip_size != 0U) {
		result->code = CRYC_VERIFY_MALFORMED;
		return 0;
	}

	/* Sets up the search over the peer's certificates. */
	memset(&search, 0, sizeof(search));
	search.store = store;
	search.peer = peer;
	search.peer_count = peer_count;
	search.params = params;
	search.max_depth = max_depth;
	if (params->has_time != 0)
		search.now = params->time;
	else
		search.now = cryc_os_time();

	/* Walks the paths with the store locked, so that the entries cannot change under the walk. */
	cryc_os_mutex_lock(&store->lock);

	error = chain_search_prepare(&search);
	if (error == 0 && search.done == 0)
		chain_extend(&search);
	if (error == 0)
		chain_fill_result(&search, result);

	cryc_os_mutex_unlock(&store->lock);

	/* Reports a certificate that could not be hashed, which a bounded hash does not do. */
	if (error != 0) {
		result->code = CRYC_VERIFY_MALFORMED;
		return 0;
	}

	/* Succeeded: the outcome is in the result. */
	return 0;
}

/*
 * Reports whether the store holds a certificate with this SHA-256 and
 * these bytes.
 */
static int
store_find_entry(
	const struct cryc_store *store,
	const uint8_t *digest,
	size_t der_size,
	const uint8_t *der)
{
	const struct cryc_store_entry *entry;
	size_t index;

	/* Compares the digest first and the bytes of a certificate with the same digest. */
	for (index = 0; index < store->count; index++) {
		entry = store->entries[index];
		if (memcmp(entry->digest, digest, CRYC_SHA256_SIZE) != 0)
			continue;
		if (entry->der_size != der_size)
			continue;
		if (memcmp(entry->der, der, der_size) != 0)
			continue;
		return 1;
	}

	/* No such certificate. */
	return 0;
}

/*
 * Makes a store entry: a copy of the DER, its parse and its SHA-256.
 */
static int
store_make_entry(
	const uint8_t *der,
	size_t size,
	struct cryc_store_entry **entry)
{
	struct cryc_store_entry *made;
	int error;

	/* Allocates the entry. */
	made = malloc(sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	memset(made, 0, sizeof(*made));

	/* Allocates the copy of the DER, which the parsed views point into. */
	made->der = malloc(size);
	if (made->der == NULL) {
		free(made);
		return ENOMEM;
	}
	memcpy(made->der, der, size);
	made->der_size = size;

	/* Parses the copy; a certificate that does not parse is not stored. */
	error = cryc_x509_parse(&made->cert, made->der, made->der_size);
	if (error != 0) {
		store_free_entry(made);
		return EINVAL;
	}

	/* Takes the SHA-256 that tells a duplicate. */
	error = cryc_hash_digest(CRYC_HASH_SHA256, made->der, made->der_size, made->digest);
	if (error != 0) {
		store_free_entry(made);
		return error;
	}

	/* Succeeded: the caller owns the entry. */
	*entry = made;
	return 0;
}

/*
 * Frees an entry and its copy of the DER.
 */
static void
store_free_entry(
	struct cryc_store_entry *entry)
{
	/* Frees the copy, then the entry. */
	free(entry->der);
	free(entry);
}

/*
 * Puts an entry into the store, or frees it when the store already has it
 * or is full.
 */
static int
store_insert(
	struct cryc_store *store,
	struct cryc_store_entry *entry,
	int *added)
{
	struct cryc_store_entry **grown;
	size_t capacity;
	int found;

	/* Takes the lock for the whole change. */
	cryc_os_mutex_lock(&store->lock);

	/* A certificate the store has is not added again, and that is not an error. */
	found = store_find_entry(store, entry->digest, entry->der_size, entry->der);
	if (found != 0) {
		cryc_os_mutex_unlock(&store->lock);
		store_free_entry(entry);
		return 0;
	}

	/* Refuses a certificate beyond the store's limit. */
	if (store->count >= CRYC_STORE_MAX) {
		cryc_os_mutex_unlock(&store->lock);
		store_free_entry(entry);
		return ENOBUFS;
	}

	/* Makes the array larger when it is full; the entries themselves never move. */
	if (store->count >= store->capacity) {
		capacity = store->capacity * 2U;
		if (capacity < CHAIN_FIRST_CAPACITY)
			capacity = CHAIN_FIRST_CAPACITY;
		if (capacity > CRYC_STORE_MAX)
			capacity = CRYC_STORE_MAX;
		grown = realloc(store->entries, capacity * sizeof(*grown));
		if (grown == NULL) {
			cryc_os_mutex_unlock(&store->lock);
			store_free_entry(entry);
			return ENOMEM;
		}
		store->entries = grown;
		store->capacity = capacity;
	}

	/* Appends the entry in the order of adding. */
	store->entries[store->count] = entry;
	store->count++;

	cryc_os_mutex_unlock(&store->lock);

	/* Succeeded: the entry is new in the store. */
	*added = 1;
	return 0;
}

/*
 * Parses a DER and adds it to the store, telling whether it was new.
 */
static int
store_add_entry(
	struct cryc_store *store,
	const uint8_t *der,
	size_t size,
	int *added)
{
	struct cryc_store_entry *entry;
	int error;

	/* Refuses a missing store and an empty certificate. */
	if (store == NULL || der == NULL || size == 0U)
		return EINVAL;

	/* Makes the entry before the lock is taken. */
	entry = NULL;
	error = store_make_entry(der, size, &entry);
	if (error != 0)
		return error;

	/* Hands the entry to the store, which keeps or frees it. */
	error = store_insert(store, entry, added);
	if (error != 0)
		return error;

	/* Succeeded: the store has the certificate. */
	return 0;
}

/*
 * The system trust source's callback for a distrusted certificate: marks
 * it.  An empty certificate cannot be marked and is skipped.
 */
static int
store_system_distrusted(
	void *context,
	const uint8_t *der,
	size_t size)
{
	int error;

	/* Marks the certificate. */
	error = cryc_store_add_distrusted((struct cryc_store *)context, der, size);
	if (error == EINVAL)
		return 0;
	if (error != 0)
		return error;

	/* Succeeded: the certificate is distrusted. */
	return 0;
}

/*
 * The system trust source's callback for a trusted certificate: adds it.
 * A certificate that does not parse cannot be an anchor and is skipped, so
 * one odd certificate of the system's store does not stop the others.
 */
static int
store_system_der(
	void *context,
	const uint8_t *der,
	size_t size)
{
	int error;

	/* Adds the certificate. */
	error = cryc_store_add_der((struct cryc_store *)context, der, size);
	if (error == EINVAL)
		return 0;
	if (error != 0)
		return error;

	/* Succeeded: the certificate is in the store. */
	return 0;
}

/*
 * The system trust source's callback for a bundle file: adds its blocks.
 * A missing file is not an error, and a bundle with a bad block keeps its
 * good ones.
 */
static int
store_system_file(
	void *context,
	const char *path)
{
	int error;

	/* Adds the file's certificates. */
	error = cryc_store_add_file((struct cryc_store *)context, path);
	if (error == ENOENT)
		return 0;
	if (error == EINVAL)
		return 0;
	if (error != 0)
		return error;

	/* Succeeded: the file's certificates are in the store. */
	return 0;
}

/*
 * Copies the first positions of the path being walked into a snapshot.
 */
static void
chain_snapshot_take(
	struct chain_snapshot *snapshot,
	const struct chain_search *search)
{
	size_t index;

	/* Copies at most as many certificates as a result can hold. */
	snapshot->count = search->count;
	if (snapshot->count > CRYC_CHAIN_MAX_DEPTH)
		snapshot->count = CRYC_CHAIN_MAX_DEPTH;
	for (index = 0; index < snapshot->count; index++)
		snapshot->chain[index] = search->path[index].cert;
}

/*
 * Reports whether a certificate is marked distrusted: the SHA-256 of its
 * tbsCertificate, or digest (that of its whole DER), is one of the
 * store's marks.
 */
static int
chain_is_distrusted(
	const struct cryc_store *store,
	const uint8_t *digest,
	const struct cryc_x509 *cert)
{
	uint8_t tbs_digest[CRYC_SHA256_SIZE];
	size_t index;
	int have_tbs;

	/* Nothing to compare with. */
	if (store->distrusted_count == 0U)
		return 0;
	have_tbs = cryc_hash_digest(CRYC_HASH_SHA256, cert->tbs, cert->tbs_size, tbs_digest) == 0;

	/* Looks both values up among the marks. */
	for (index = 0; index < store->distrusted_count; index++) {
		if (memcmp(store->distrusted[index], digest, CRYC_SHA256_SIZE) == 0)
			return 1;
		if (have_tbs && memcmp(store->distrusted[index], tbs_digest, CRYC_SHA256_SIZE) == 0)
			return 1;
	}

	/* Not marked. */
	return 0;
}

/*
 * Reports whether two parsed certificates are the same bytes.
 */
static int
chain_same_der(
	const struct cryc_x509 *left,
	const struct cryc_x509 *right)
{
	/* The same object is the same certificate. */
	if (left == right)
		return 1;

	/* Different lengths are different certificates. */
	if (left->der_size != right->der_size)
		return 0;

	/* Equal lengths are compared byte for byte. */
	if (memcmp(left->der, right->der, left->der_size) != 0)
		return 0;

	/* The same bytes. */
	return 1;
}

/*
 * Reports whether a certificate is self-issued: its issuer and subject are
 * the same name (RFC 5280 6.1, the comparison of 7.1).
 */
static int
chain_self_issued(
	const struct cryc_x509 *cert)
{
	int equal;

	/* Names that are the same bytes are the same name, as the parser noted. */
	if ((cert->flags & CRYC_X509_SELF_ISSUED) != 0U)
		return 1;

	/* Other spellings of one name are compared as the path rules ask. */
	equal = cryc_name_equal(cert->issuer, cert->issuer_size, cert->subject, cert->subject_size);
	if (equal != 0)
		return 1;

	/* Different names. */
	return 0;
}

/*
 * Reports whether a certificate could have issued another: its subject is
 * the other's issuer and, when both carry one, its subjectKeyIdentifier is
 * the other's authorityKeyIdentifier (RFC 4158 3.5.12 and RFC 5280
 * 4.2.1.1).
 */
static int
chain_issued_by(
	const struct cryc_x509 *cert,
	const struct cryc_x509 *issuer)
{
	int equal;

	/* The issuer's subject must be the certificate's issuer name. */
	equal = cryc_name_equal(cert->issuer, cert->issuer_size, issuer->subject, issuer->subject_size);
	if (equal == 0)
		return 0;

	/* A certificate that names no key identifier, or an issuer without one, is not told apart by it. */
	if (cert->aki == NULL)
		return 1;
	if (issuer->ski == NULL)
		return 1;

	/* When both carry one they must agree. */
	if (cert->aki_size != issuer->ski_size)
		return 0;
	if (memcmp(cert->aki, issuer->ski, cert->aki_size) != 0)
		return 0;

	/* The names and the key identifiers agree. */
	return 1;
}

/*
 * Returns the length of an RSA modulus in bits, not counting leading zero
 * bits (0 for a modulus without bytes).
 */
static size_t
chain_rsa_bits(
	const struct cryc_pubkey *key)
{
	size_t bits;
	size_t index;
	unsigned first;

	/* An absent modulus has no bits. */
	if (key->rsa.n == NULL || key->rsa.n_size == 0U)
		return 0;

	/* Skips leading zero bytes, in case the view carries any. */
	index = 0;
	while (index < key->rsa.n_size && key->rsa.n[index] == 0U)
		index++;
	if (index == key->rsa.n_size)
		return 0;

	/* Counts the bytes that remain, then takes the unused high bits of the first off. */
	bits = (key->rsa.n_size - index) * 8U;
	first = key->rsa.n[index];
	while ((first & 0x80U) == 0U) {
		first = first << 1;
		bits--;
	}

	/* The length of the modulus in bits. */
	return bits;
}

/*
 * Reports whether the path walked so far holds a distrusted certificate.
 */
static int
chain_path_rejected(
	const struct chain_search *search)
{
	size_t index;

	/* Looks at each certificate's mark. */
	for (index = 0; index < search->count; index++) {
		if (search->path[index].distrusted != 0)
			return 1;
	}

	/* No certificate of the path is distrusted. */
	return 0;
}

/*
 * Verifies a certificate's signature by an issuer's key and remembers the
 * outcome.  A verification not yet made counts as work; when the bound is
 * reached the search is stopped and CRYC_VERIFY_WORK_EXCEEDED is returned.
 */
static enum cryc_verify_code
chain_edge_code(
	struct chain_search *search,
	const struct cryc_x509 *child,
	const struct cryc_x509 *issuer)
{
	enum cryc_verify_code code;
	size_t index;
	size_t bits;
	int error;

	/* Reuses an outcome already known for this pair. */
	for (index = 0; index < search->edge_count; index++) {
		if (search->edges[index].child == child && search->edges[index].issuer == issuer)
			return search->edges[index].code;
	}

	/* A signature algorithm or a key outside the range cannot be verified, which costs nothing. */
	if (child->signature_alg.alg == CRYC_SIG_UNSUPPORTED || child->signature_alg.alg == CRYC_SIG_NONE)
		return CRYC_VERIFY_UNSUPPORTED_ALGORITHM;
	if (issuer->key.type == CRYC_KEY_UNSUPPORTED || issuer->key.type == CRYC_KEY_NONE)
		return CRYC_VERIFY_UNSUPPORTED_ALGORITHM;

	/* Stops the search when the bound on verifications is reached. */
	if (search->work >= CRYC_CHAIN_MAX_WORK) {
		search->exhausted = 1;
		search->exhausted_depth = 0;
		if (search->count > 0U)
			search->exhausted_depth = search->count - 1U;
		return CRYC_VERIFY_WORK_EXCEEDED;
	}

	/* Verifies the signature over the certificate's tbsCertificate. */
	search->work++;
	error = cryc_signature_verify(&issuer->key, &child->signature_alg, child->tbs, child->tbs_size, child->signature, child->signature_size);

	/* Names the outcome: a short RSA key and an unusable algorithm both come back as ENOTSUP. */
	code = CRYC_VERIFY_SIGNATURE_FAILURE;
	if (error == 0) {
		code = CRYC_VERIFY_OK;
	} else if (error == ENOTSUP) {
		code = CRYC_VERIFY_UNSUPPORTED_ALGORITHM;
		if (issuer->key.type == CRYC_KEY_RSA) {
			bits = chain_rsa_bits(&issuer->key);
			if (bits < CRYC_RSA_MIN_BITS)
				code = CRYC_VERIFY_KEY_TOO_SMALL;
		}
	}

	/* Remembers the pair; the table has room for every verification the bound allows. */
	search->edges[search->edge_count].child = child;
	search->edges[search->edge_count].issuer = issuer;
	search->edges[search->edge_count].code = code;
	search->edge_count++;

	/* The outcome of verifying this pair. */
	return code;
}

/*
 * Checks a certificate's validity period against the time of validation
 * (RFC 5280 6.1.3 (a)(2): from notBefore to notAfter, both inclusive).
 */
static enum cryc_verify_code
chain_check_time(
	const struct cryc_x509 *cert,
	int64_t now)
{
	/* A certificate not yet in force. */
	if (now < cert->not_before)
		return CRYC_VERIFY_NOT_YET_VALID;

	/* A certificate past its end. */
	if (now > cert->not_after)
		return CRYC_VERIFY_EXPIRED;

	/* Within the period. */
	return CRYC_VERIFY_OK;
}

/*
 * Applies the name constraints of every certificate above one of the path
 * to it (RFC 5280 6.1.3 (b) and (c), 6.1.4 (g)); a self-issued certificate
 * that is not the leaf is not checked.
 */
static enum cryc_verify_code
chain_check_names(
	const struct chain_search *search,
	size_t index)
{
	const struct cryc_x509 *cert;
	const struct cryc_x509 *constraining;
	size_t above;
	int self_issued;
	int is_leaf;
	int error;

	/* A self-issued certificate in the middle of the path is not constrained: it is the same entity with a new key. */
	cert = search->path[index].cert;
	is_leaf = 0;
	if (index == 0U)
		is_leaf = 1;
	if (is_leaf == 0) {
		self_issued = chain_self_issued(cert);
		if (self_issued != 0)
			return CRYC_VERIFY_OK;
	}

	/* Applies each constraining certificate above, the trust anchor included. */
	for (above = index + 1U; above < search->count; above++) {
		constraining = search->path[above].cert;
		if ((constraining->flags & CRYC_X509_HAS_NAME_CONSTRAINTS) == 0U)
			continue;

		/* Checks the certificate's names against the constraints. */
		error = cryc_name_constraints_check(constraining, cert, is_leaf);
		if (error == 0)
			continue;
		if (error == EACCES)
			return CRYC_VERIFY_NAME_CONSTRAINT_VIOLATION;
		if (error == ENOTSUP)
			return CRYC_VERIFY_UNSUPPORTED_NAME_CONSTRAINT;

		/* Malformed constraints or names. */
		return CRYC_VERIFY_MALFORMED;
	}

	/* Every name is allowed. */
	return CRYC_VERIFY_OK;
}

/*
 * Reports whether a certificate's extended key usage, when it has one,
 * allows TLS server authentication (serverAuth or anyExtendedKeyUsage).
 */
static int
chain_allows_server_auth(
	const struct cryc_x509 *cert)
{
	/* A certificate without the extension is not restricted. */
	if ((cert->flags & CRYC_X509_HAS_EXT_KEY_USAGE) == 0U)
		return 1;

	/* The extension must name serverAuth or any purpose. */
	if ((cert->ext_key_usage & (CRYC_EKU_SERVER_AUTH | CRYC_EKU_ANY)) != 0U)
		return 1;

	/* The extension does not allow server authentication. */
	return 0;
}

/*
 * Checks what an intermediate certificate must be: a version 3 CA that may
 * sign certificates, whose path length allows the intermediates below it
 * and whose purposes include TLS server authentication.
 */
static enum cryc_verify_code
chain_check_intermediate(
	const struct chain_search *search,
	size_t index)
{
	const struct cryc_x509 *cert;
	const struct cryc_x509 *below;
	size_t position;
	size_t below_count;
	int self_issued;
	int allowed;

	/* A CA certificate is version 3 and states in basicConstraints that it is a CA (RFC 5280 6.1.4 (k)). */
	cert = search->path[index].cert;
	if (cert->version != 3U)
		return CRYC_VERIFY_INVALID_CA;
	if ((cert->flags & CRYC_X509_HAS_BASIC_CONSTRAINTS) == 0U)
		return CRYC_VERIFY_INVALID_CA;
	if ((cert->flags & CRYC_X509_CA) == 0U)
		return CRYC_VERIFY_INVALID_CA;

	/* A key usage extension must allow certificate signing (6.1.4 (n)). */
	if ((cert->flags & CRYC_X509_HAS_KEY_USAGE) != 0U) {
		if ((cert->key_usage & CRYC_KU_KEY_CERT_SIGN) == 0U)
			return CRYC_VERIFY_KEY_USAGE;
	}

	/* The path length counts the non-self-issued intermediates below this one (6.1.4 (l) and (m)). */
	if ((cert->flags & CRYC_X509_HAS_PATH_LEN) != 0U) {
		below_count = 0;
		for (position = 1; position < index; position++) {
			below = search->path[position].cert;
			self_issued = chain_self_issued(below);
			if (self_issued == 0)
				below_count++;
		}
		if ((size_t)cert->path_len < below_count)
			return CRYC_VERIFY_PATH_LENGTH_EXCEEDED;
	}

	/* The purposes must include TLS server authentication. */
	allowed = chain_allows_server_auth(cert);
	if (allowed == 0)
		return CRYC_VERIFY_INVALID_PURPOSE;

	/* A certificate that may sign server certificates. */
	return CRYC_VERIFY_OK;
}

/*
 * Checks what the end-entity certificate must be for a TLS server: its
 * purposes include server authentication, its key may sign the handshake
 * and is of a kind and size the library can use.
 */
static enum cryc_verify_code
chain_check_leaf(
	const struct cryc_x509 *cert)
{
	size_t bits;
	int allowed;

	/* The purposes must include TLS server authentication. */
	allowed = chain_allows_server_auth(cert);
	if (allowed == 0)
		return CRYC_VERIFY_INVALID_PURPOSE;

	/* A key usage extension must allow digital signatures: TLS 1.3 signs the handshake with the key, and so do the ECDHE suites of TLS 1.2. */
	if ((cert->flags & CRYC_X509_HAS_KEY_USAGE) != 0U) {
		if ((cert->key_usage & CRYC_KU_DIGITAL_SIGNATURE) == 0U)
			return CRYC_VERIFY_KEY_USAGE;
	}

	/* A key outside the library's range cannot sign a handshake. */
	if (cert->key.type == CRYC_KEY_UNSUPPORTED || cert->key.type == CRYC_KEY_NONE)
		return CRYC_VERIFY_UNSUPPORTED_ALGORITHM;

	/* An RSA key below the floor is too small. */
	if (cert->key.type == CRYC_KEY_RSA) {
		bits = chain_rsa_bits(&cert->key);
		if (bits < CRYC_RSA_MIN_BITS)
			return CRYC_VERIFY_KEY_TOO_SMALL;
	}

	/* A certificate whose key a server may use. */
	return CRYC_VERIFY_OK;
}

/*
 * Validates one certificate of a complete path in the order of the header:
 * the signature by the issuer above it, the validity period, the name
 * constraints, the critical extensions, and what its role asks of it.  The
 * trust anchor of a longer path gets only its validity period; the only
 * certificate of a one-certificate path gets the leaf's checks too.
 */
static enum cryc_verify_code
chain_validate_certificate(
	struct chain_search *search,
	size_t index)
{
	const struct cryc_x509 *cert;
	enum cryc_verify_code code;
	size_t anchor;

	/* Finds the certificate and the position of the trust anchor. */
	cert = search->path[index].cert;
	anchor = search->count - 1U;

	/* Every certificate but the anchor was signed by the one above it. */
	if (index != anchor) {
		code = chain_edge_code(search, cert, search->path[index + 1U].cert);
		if (code != CRYC_VERIFY_OK)
			return code;
	}

	/* The validity period contains the time of validation, for the anchor too. */
	code = chain_check_time(cert, search->now);
	if (code != CRYC_VERIFY_OK)
		return code;

	/* The anchor of a longer path is taken as trusted as it is: nothing else of it is checked. */
	if (index == anchor && index != 0U)
		return CRYC_VERIFY_OK;

	/* The name constraints of the certificates above apply to this one. */
	code = chain_check_names(search, index);
	if (code != CRYC_VERIFY_OK)
		return code;

	/* An extension this library does not know that the issuer marked critical cannot be ignored (6.1.4 (o)). */
	if ((cert->flags & CRYC_X509_UNKNOWN_CRITICAL) != 0U)
		return CRYC_VERIFY_UNHANDLED_CRITICAL_EXTENSION;

	/* The leaf must suit a TLS server. */
	if (index == 0U) {
		code = chain_check_leaf(cert);
		if (code != CRYC_VERIFY_OK)
			return code;

		/* The leaf passed everything asked of it. */
		return CRYC_VERIFY_OK;
	}

	/* An intermediate must be a CA that may sign server certificates. */
	code = chain_check_intermediate(search, index);
	if (code != CRYC_VERIFY_OK)
		return code;

	/* An intermediate that passed everything asked of it. */
	return CRYC_VERIFY_OK;
}

/*
 * Validates the complete path in search->path (leaf first, trust anchor
 * last) from the anchor down to the leaf; *depth receives the index the
 * code is about.
 */
static enum cryc_verify_code
chain_validate_path(
	struct chain_search *search,
	size_t *depth)
{
	enum cryc_verify_code code;
	size_t index;

	/* A path longer than the limit is refused before any signature is looked at. */
	if (search->count > search->max_depth) {
		*depth = search->max_depth;
		return CRYC_VERIFY_CHAIN_TOO_LONG;
	}

	/* Validates each certificate from the trust anchor down to the leaf, and stops at the first failure. */
	for (index = search->count; index > 0U; index--) {
		code = chain_validate_certificate(search, index - 1U);
		if (code != CRYC_VERIFY_OK) {
			*depth = index - 1U;
			return code;
		}
	}

	/* The whole path is valid. */
	*depth = 0;
	return CRYC_VERIFY_OK;
}

/*
 * Reports whether a certificate the peer sent may be tried as an issuer:
 * it is not a repeat or the twin of a store entry (the store's copy is
 * tried instead) and is not already on the path.
 */
static int
chain_peer_usable(
	const struct chain_search *search,
	size_t peer_index)
{
	size_t index;

	/* A repeat of an earlier certificate or the twin of a store entry is left to the one it repeats. */
	if (search->peer_skip[peer_index] != 0)
		return 0;

	/* A certificate already on the path would make a cycle. */
	for (index = 0; index < search->count; index++) {
		if (search->path[index].cert == &search->peer[peer_index])
			return 0;
	}

	/* Free to be tried. */
	return 1;
}

/*
 * Handles a complete path (its last certificate is the trust anchor):
 * looks at its distrusted marks, validates it and records the outcome.
 * The first valid path ends the search; otherwise the first complete
 * path's code is kept.
 */
static void
chain_complete(
	struct chain_search *search)
{
	enum cryc_verify_code code;
	size_t depth;
	size_t index;
	int matched;
	int found_mark;

	/* A distrusted certificate anywhere on the path rejects it at the lowest such depth, with no signature looked at. */
	found_mark = 0;
	depth = 0;
	for (index = 0; index < search->count; index++) {
		if (search->path[index].distrusted != 0) {
			found_mark = 1;
			depth = index;
			break;
		}
	}

	/* Validates a path that has no distrusted certificate. */
	if (found_mark != 0)
		code = CRYC_VERIFY_REJECTED;
	else
		code = chain_validate_path(search, &depth);

	/* A path whose validation was cut off by the work bound has no outcome. */
	if (search->exhausted != 0)
		return;

	/* A valid path is matched against the name or address that was asked for. */
	if (code == CRYC_VERIFY_OK) {
		if (search->params->has_host != 0) {
			matched = cryc_name_match_host(search->path[0].cert, search->params->host);
			if (matched == 0)
				code = CRYC_VERIFY_HOSTNAME_MISMATCH;
		} else if (search->params->ip_size != 0U) {
			matched = cryc_name_match_ip(search->path[0].cert, search->params->ip, search->params->ip_size);
			if (matched == 0)
				code = CRYC_VERIFY_IP_ADDRESS_MISMATCH;
		}

		/* The first valid path ends the search, whether or not the name matched: the leaf is the same on every path. */
		search->done = 1;
		search->done_code = code;
		chain_snapshot_take(&search->done_chain, search);
		return;
	}

	/* The first complete path that is not valid gives the code if no path turns out valid. */
	if (search->has_first == 0) {
		search->has_first = 1;
		search->first_code = code;
		search->first_depth = depth;
		chain_snapshot_take(&search->first_chain, search);
	}
}

/*
 * Extends the path by every issuer candidate of its last certificate, depth
 * first: the store's entries (a path ends at one), then the peer's other
 * certificates.  Returns when the candidates are tried or the search has
 * ended.
 */
static void
chain_extend(
	struct chain_search *search)
{
	const struct cryc_x509 *top;
	struct cryc_store_entry *entry;
	enum cryc_verify_code edge;
	size_t entry_index;
	size_t peer_index;
	int issued;
	int usable;
	int found_any;
	int rejected;
	int candidate_distrusted;
	int self_issued;

	/* The walk is bounded in the number of certificates it expands. */
	search->nodes++;
	if (search->nodes > CHAIN_MAX_NODES) {
		search->exhausted = 1;
		search->exhausted_depth = search->count - 1U;
		return;
	}

	/* Keeps the longest path found, which is what a failure reports when no path was completed. */
	if (search->count > search->longest.count)
		chain_snapshot_take(&search->longest, search);

	/* The certificate whose issuer is looked for. */
	top = search->path[search->count - 1U].cert;
	found_any = 0;

	/* A path may end at a store entry; one more certificate than the limit is allowed so that a too-long path is reported as such. */
	if (search->count <= search->max_depth) {
		/* Tries each store entry that could have issued the certificate. */
		for (entry_index = 0; entry_index < search->store->count; entry_index++) {
			entry = search->store->entries[entry_index];
			issued = chain_issued_by(top, &entry->cert);
			if (issued == 0)
				continue;
			found_any = 1;

			/* A path with a distrusted certificate needs no signature; any other has its signature verified now, which bounds the work of a hostile chain. */
			rejected = chain_path_rejected(search);
			candidate_distrusted = chain_is_distrusted(search->store, entry->digest, &entry->cert);
			if (rejected == 0 && candidate_distrusted == 0) {
				edge = chain_edge_code(search, top, &entry->cert);
				UNUSED_PARAMETER(edge);
				if (search->exhausted != 0)
					return;
			}

			/* Puts the entry on the path as its trust anchor and handles the complete path. */
			search->path[search->count].cert = &entry->cert;
			search->path[search->count].distrusted = candidate_distrusted;
			search->count++;
			chain_complete(search);
			search->count--;
			if (search->exhausted != 0 || search->done != 0)
				return;
		}
	}

	/* Another certificate of the peer may stand between this one and an anchor when the path still has room for the anchor. */
	if (search->count < search->max_depth) {
		/* Tries each certificate the peer sent that could have issued the certificate. */
		for (peer_index = 1; peer_index < search->peer_count; peer_index++) {
			usable = chain_peer_usable(search, peer_index);
			if (usable == 0)
				continue;
			issued = chain_issued_by(top, &search->peer[peer_index]);
			if (issued == 0)
				continue;
			found_any = 1;

			/* Verifies the signature now, unless the path is already rejected. */
			rejected = chain_path_rejected(search);
			candidate_distrusted = chain_is_distrusted(search->store, search->peer_digest[peer_index], &search->peer[peer_index]);
			if (rejected == 0 && candidate_distrusted == 0) {
				edge = chain_edge_code(search, top, &search->peer[peer_index]);
				UNUSED_PARAMETER(edge);
				if (search->exhausted != 0)
					return;
			}

			/* Walks on from the certificate. */
			search->path[search->count].cert = &search->peer[peer_index];
			search->path[search->count].distrusted = candidate_distrusted;
			search->count++;
			chain_extend(search);
			search->count--;
			if (search->exhausted != 0 || search->done != 0)
				return;
		}
	}

	/* A peer certificate with no issuer candidate at all is a dead end; a self-issued one makes the failure a self-signed one. */
	if (found_any == 0 && search->count >= 2U) {
		self_issued = chain_self_issued(top);
		if (self_issued != 0) {
			search->dead_end_self_issued = 1;
			search->dead_end_depth = search->count - 1U;
		}
	}
}

/*
 * Prepares the walk: the SHA-256 of each peer certificate, the ones the
 * walk skips (repeats and twins of store entries), the path of a leaf that
 * the store itself holds, and the leaf as the first certificate of the
 * path the walk extends.
 */
static int
chain_search_prepare(
	struct chain_search *search)
{
	const struct cryc_store_entry *entry;
	size_t index;
	size_t other;
	size_t entry_index;
	int same;
	int error;

	/* Takes the SHA-256 of each peer certificate. */
	for (index = 0; index < search->peer_count; index++) {
		error = cryc_hash_digest(CRYC_HASH_SHA256, search->peer[index].der, search->peer[index].der_size, search->peer_digest[index]);
		if (error != 0)
			return error;
	}

	/* Marks a certificate that repeats an earlier one, or is the twin of a store entry, so the walk skips it. */
	for (index = 1; index < search->peer_count; index++) {
		for (other = 0; other < index; other++) {
			if (memcmp(search->peer_digest[index], search->peer_digest[other], CRYC_SHA256_SIZE) != 0)
				continue;
			same = chain_same_der(&search->peer[index], &search->peer[other]);
			if (same != 0)
				search->peer_skip[index] = 1;
		}
		for (entry_index = 0; entry_index < search->store->count; entry_index++) {
			entry = search->store->entries[entry_index];
			if (memcmp(search->peer_digest[index], entry->digest, CRYC_SHA256_SIZE) != 0)
				continue;
			same = chain_same_der(&search->peer[index], &entry->cert);
			if (same != 0)
				search->peer_skip[index] = 1;
		}
	}

	/* A leaf that is a store entry byte for byte is its own trust anchor: that one-certificate path is handled first. */
	for (entry_index = 0; entry_index < search->store->count; entry_index++) {
		entry = search->store->entries[entry_index];
		if (memcmp(search->peer_digest[0], entry->digest, CRYC_SHA256_SIZE) != 0)
			continue;
		same = chain_same_der(&search->peer[0], &entry->cert);
		if (same == 0)
			continue;

		/* Puts the store's copy on the path as its only certificate. */
		search->path[0].cert = &entry->cert;
		search->path[0].distrusted = chain_is_distrusted(search->store, entry->digest, &entry->cert);
		search->count = 1;
		chain_complete(search);
		break;
	}

	/* The leaf is the first certificate of the path the walk extends. */
	search->path[0].cert = &search->peer[0];
	search->path[0].distrusted = chain_is_distrusted(search->store, search->peer_digest[0], &search->peer[0]);
	search->count = 1;

	/* Succeeded: the walk can start. */
	return 0;
}

/*
 * Copies a snapshot into a result.
 */
static void
chain_copy_snapshot(
	const struct chain_snapshot *snapshot,
	struct cryc_verify_result *result)
{
	size_t index;

	/* Copies each certificate of the snapshot. */
	for (index = 0; index < snapshot->count; index++)
		result->chain[index] = snapshot->chain[index];
	result->chain_count = snapshot->count;
}

/*
 * Turns what the walk found into the result: the first valid path, else
 * the first complete path's code, else the work bound, else the kind of
 * dead end.
 */
static void
chain_fill_result(
	const struct chain_search *search,
	struct cryc_verify_result *result)
{
	int self_issued;

	/* A valid path, with the outcome of the name match, is the result. */
	if (search->done != 0) {
		result->code = search->done_code;
		result->depth = 0;
		chain_copy_snapshot(&search->done_chain, result);
		return;
	}

	/* The first complete path's code is next: it says what is wrong with the path closest to a trust anchor. */
	if (search->has_first != 0) {
		result->code = search->first_code;
		result->depth = search->first_depth;
		chain_copy_snapshot(&search->first_chain, result);
		return;
	}

	/* The work bound stopped the search before any path was completed. */
	if (search->exhausted != 0) {
		result->code = CRYC_VERIFY_WORK_EXCEEDED;
		result->depth = search->exhausted_depth;
		chain_copy_snapshot(&search->longest, result);
		return;
	}

	/* No path reached a trust anchor: the longest partial path is reported with the kind of dead end. */
	chain_copy_snapshot(&search->longest, result);
	self_issued = chain_self_issued(&search->peer[0]);
	if (self_issued != 0) {
		result->code = CRYC_VERIFY_DEPTH_ZERO_SELF_SIGNED;
		result->depth = 0;
	} else if (search->dead_end_self_issued != 0) {
		result->code = CRYC_VERIFY_SELF_SIGNED_IN_CHAIN;
		result->depth = search->dead_end_depth;
	} else {
		result->code = CRYC_VERIFY_UNABLE_TO_GET_ISSUER;
		result->depth = 0;
		if (result->chain_count > 0U)
			result->depth = result->chain_count - 1U;
	}
}
