/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The trust store (the trusted root certificates) and the validation of
 * a server's certificate chain against it: path building (RFC 4158,
 * informative), basic path validation (RFC 5280 6.1) and the server
 * identity check (RFC 9525, through cryc-name.h).
 *
 * The profile is that of a web PKI client: the purpose is always TLS
 * server authentication; no policy processing (cryc-x509.h); no
 * revocation checking (no CRL, no OCSP); the trust anchors are whole
 * self-signed or other certificates taken as trusted as they are (RFC
 * 5280 6.1.1 (d): their name, key and name constraints are used; their own
 * signature, basicConstraints and keyUsage are not checked; their validity
 * period is checked, as OpenSSL does).
 *
 * All inputs are public.  The peer's certificates are untrusted; the work
 * of path building is bounded (CRYC_CHAIN_MAX_WORK) so that a hostile
 * chain of many look-alike certificates cannot make a client spend
 * unbounded time.
 */

#ifndef LIBCRYPTO_COMPAT_CRYC_CHAIN_H
#define LIBCRYPTO_COMPAT_CRYC_CHAIN_H

#include "cryc-name.h"
#include "cryc-os.h"
#include "cryc-sigver.h"

/* The most certificates a store holds (the ca-certificates package has about 150; Windows' ROOT store about 400). */
#define CRYC_STORE_MAX		4096U

/* The most certificates a server may send (OpenSSL's default depth is far larger; real chains have 2 to 4). */
#define CRYC_CHAIN_MAX_PEER	16U

/* The most certificates in a path, the leaf and the trust anchor included (RFC 5280 6.1 sets no limit). */
#define CRYC_CHAIN_MAX_DEPTH	10U

/* The most signature verifications one validation may perform while building paths. */
#define CRYC_CHAIN_MAX_WORK	64U

/*
 * The outcome of a validation.  The public interface maps each to an
 * X509_V_ code (api-x509.c); the order here is not significant.
 */
enum cryc_verify_code {
	CRYC_VERIFY_OK = 0,
	CRYC_VERIFY_NO_PEER_CERTIFICATE,
	CRYC_VERIFY_MALFORMED,
	CRYC_VERIFY_UNABLE_TO_GET_ISSUER,
	CRYC_VERIFY_DEPTH_ZERO_SELF_SIGNED,
	CRYC_VERIFY_SELF_SIGNED_IN_CHAIN,
	CRYC_VERIFY_CHAIN_TOO_LONG,
	CRYC_VERIFY_SIGNATURE_FAILURE,
	CRYC_VERIFY_UNSUPPORTED_ALGORITHM,
	CRYC_VERIFY_KEY_TOO_SMALL,
	CRYC_VERIFY_NOT_YET_VALID,
	CRYC_VERIFY_EXPIRED,
	CRYC_VERIFY_INVALID_CA,
	CRYC_VERIFY_PATH_LENGTH_EXCEEDED,
	CRYC_VERIFY_KEY_USAGE,
	CRYC_VERIFY_INVALID_PURPOSE,
	CRYC_VERIFY_UNHANDLED_CRITICAL_EXTENSION,
	CRYC_VERIFY_NAME_CONSTRAINT_VIOLATION,
	CRYC_VERIFY_UNSUPPORTED_NAME_CONSTRAINT,
	CRYC_VERIFY_HOSTNAME_MISMATCH,
	CRYC_VERIFY_IP_ADDRESS_MISMATCH,
	CRYC_VERIFY_WORK_EXCEEDED
};

/*
 * One certificate of a store: an owned copy of its DER, its parsed form
 * (views into der) and the SHA-256 of der (to refuse duplicates).
 */
struct cryc_store_entry {
	uint8_t *der;
	size_t der_size;
	struct cryc_x509 cert;
	uint8_t digest[CRYC_SHA256_SIZE];
};

/*
 * A set of trust anchors.
 *
 * entries is a malloc'd array of capacity pointers of which count are
 * used, in the order they were added; each points to its own malloc'd
 * entry, which never moves and is never freed before cryc_store_free, so
 * a validation result's pointers into the store stay valid while the
 * store lives even if other threads add certificates.  lock is held while
 * entries change and while a validation reads them (the public
 * X509_STORE allows adding certificates while connections validate on
 * other threads).  The store owns the entries and their DER.
 */
struct cryc_store {
	struct cryc_os_mutex lock;
	struct cryc_store_entry **entries;
	size_t count;
	size_t capacity;
};

/*
 * What a validation checks besides the chain.
 *
 *   time         The time of validation, seconds since 1970, when
 *                has_time is 1; otherwise cryc_os_time() at the call.
 *   host         A normalized host name (cryc_name_host_normalize) to
 *                match, when has_host is 1.
 *   ip, ip_size  An address to match (4 or 16 bytes), when ip_size is
 *                not 0.  At most one of host and ip is set.
 *   max_depth    The most certificates of a path (2 to
 *                CRYC_CHAIN_MAX_DEPTH); 0 means CRYC_CHAIN_MAX_DEPTH.
 */
struct cryc_verify_params {
	int has_time;
	int64_t time;
	int has_host;
	char host[CRYC_NAME_HOST_MAX + 1U];
	uint8_t ip[16];
	size_t ip_size;
	size_t max_depth;
};

/*
 * A validation's outcome: code; depth, the index in the path of the
 * certificate the code is about (0 is the leaf; for a path that was not
 * completed, the index where the search stopped); chain and chain_count,
 * the path validated (leaf first, trust anchor last) when code is
 * CRYC_VERIFY_OK, CRYC_VERIFY_HOSTNAME_MISMATCH or
 * CRYC_VERIFY_IP_ADDRESS_MISMATCH, else the longest partial path found.
 * The pointers point into the peer's parsed certificates and the store's
 * entries.
 */
struct cryc_verify_result {
	enum cryc_verify_code code;
	size_t depth;
	const struct cryc_x509 *chain[CRYC_CHAIN_MAX_DEPTH];
	size_t chain_count;
};

/* Makes an empty store.  Returns 0 or ENOMEM. */
int cryc_store_init(struct cryc_store *store);

/* Frees every entry and the lock; the store must be made again before reuse. */
void cryc_store_free(struct cryc_store *store);

/*
 * Adds one certificate from its DER (copied).
 *
 * A certificate already in the store (same SHA-256 of the DER) is not
 * added again and is not an error.  Returns 0; EINVAL when it does not
 * parse (cryc_x509_parse); ENOBUFS when the store holds CRYC_STORE_MAX;
 * ENOMEM.
 */
int cryc_store_add_der(struct cryc_store *store, const uint8_t *der, size_t size);

/*
 * Adds every CERTIFICATE block of a PEM text (cryc_pem_next_certificate).
 *
 * *added receives the number of certificates newly added.  A block that
 * does not decode or whose certificate does not parse is skipped and
 * remembered; the call goes on with the next block.  Returns 0 when every
 * block was good; EINVAL when at least one was bad (the good ones are
 * kept); ENOENT when the text has no CERTIFICATE block; ENOBUFS or ENOMEM
 * (the ones added before stay).
 */
int cryc_store_add_pem(struct cryc_store *store, const char *text, size_t size, size_t *added);

/*
 * Adds the CERTIFICATE blocks of a PEM file (read with
 * cryc_os_read_file, at most CRYC_OS_FILE_MAX bytes).
 *
 * Returns what cryc_os_read_file or cryc_store_add_pem returns (ENOENT
 * for a missing file).
 */
int cryc_store_add_file(struct cryc_store *store, const char *path);

/*
 * Adds the system's roots (cryc_os_system_roots, with this store's
 * add_der and add_file).  Returns 0 or the first error other than ENOENT
 * (a missing default file leaves the store as it was and is not an error,
 * as OpenSSL's SSL_CTX_set_default_verify_paths is not).
 */
int cryc_store_add_system(struct cryc_store *store);

/*
 * Validates a server's chain.
 *
 * peer is peer_count parsed certificates as the server sent them (leaf
 * first; the others in any order, as RFC 8446 4.4.2 allows; duplicates
 * and unrelated certificates are tolerated), peer_count 0 to
 * CRYC_CHAIN_MAX_PEER (0 gives CRYC_VERIFY_NO_PEER_CERTIFICATE).
 *
 * Path building: from the leaf upward, depth first; at each certificate
 * the candidates for its issuer are first the store's entries, then the
 * peer's other certificates not yet on the path, whose subject equals the
 * certificate's issuer (cryc_name_equal) and, when both are present,
 * whose subjectKeyIdentifier equals the certificate's
 * authorityKeyIdentifier.  A store entry byte-identical to the certificate
 * itself ends the path with that certificate as the trust anchor (a
 * trusted self-signed server).  A path ends at the first store entry
 * reached.  Each complete path is validated (below); the first valid path
 * is the result; otherwise the search backtracks to the next candidate.
 * The search stops after CRYC_CHAIN_MAX_WORK signature verifications
 * (CRYC_VERIFY_WORK_EXCEEDED if no path was valid by then).
 *
 * Validation of a path, from the certificate below the trust anchor down
 * to the leaf (RFC 5280 6.1.3, 6.1.4, 6.1.5):
 *   - the signature by the issuer's key under the certificate's
 *     signatureAlgorithm (cryc_signature_verify; EBADMSG gives
 *     SIGNATURE_FAILURE, ENOTSUP UNSUPPORTED_ALGORITHM or, for a short
 *     RSA key, KEY_TOO_SMALL);
 *   - the validity period contains the time (NOT_YET_VALID, EXPIRED;
 *     also checked for the trust anchor);
 *   - the name constraints of every certificate above it on the path,
 *     the trust anchor included, unless it is a self-issued
 *     intermediate (6.1.4 (g); cryc_name_constraints_check: EACCES gives
 *     NAME_CONSTRAINT_VIOLATION, ENOTSUP UNSUPPORTED_NAME_CONSTRAINT);
 *   - no unknown critical extension (UNHANDLED_CRITICAL_EXTENSION);
 *   - for an intermediate: version 3 with basicConstraints cA TRUE
 *     (INVALID_CA); keyUsage, when present, with keyCertSign (KEY_USAGE);
 *     pathLenConstraint not less than the number of non-self-issued
 *     intermediates below it (PATH_LENGTH_EXCEEDED); extKeyUsage, when
 *     present, with serverAuth or anyExtendedKeyUsage (INVALID_PURPOSE);
 *   - for the leaf: extKeyUsage, when present, with serverAuth or
 *     anyExtendedKeyUsage (INVALID_PURPOSE); keyUsage, when present,
 *     with digitalSignature (KEY_USAGE: TLS 1.3 4.4.2.2 and the ECDHE
 *     suites of TLS 1.2 sign with it); a key that is
 *     CRYC_KEY_UNSUPPORTED (UNSUPPORTED_ALGORITHM) or an RSA key below
 *     CRYC_RSA_MIN_BITS (KEY_TOO_SMALL);
 *   - a path longer than params->max_depth: CHAIN_TOO_LONG.
 * After a valid path, the host or the address of params is matched
 * against the leaf (HOSTNAME_MISMATCH, IP_ADDRESS_MISMATCH).
 *
 * When no path was valid: the code of the first path that reached a
 * trust anchor; when none reached one, DEPTH_ZERO_SELF_SIGNED if the leaf
 * is self-issued, SELF_SIGNED_IN_CHAIN if the search ended at a
 * self-issued peer certificate, and UNABLE_TO_GET_ISSUER otherwise.
 *
 * The store's lock is held for the whole call.  Returns 0 (the outcome is
 * in *result, whatever it is) or ENOMEM; *result is always written.
 */
int cryc_chain_verify(struct cryc_store *store, const struct cryc_x509 *peer, size_t peer_count, const struct cryc_verify_params *params, struct cryc_verify_result *result);

#endif
