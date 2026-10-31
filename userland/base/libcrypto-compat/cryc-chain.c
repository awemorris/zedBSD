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

/*
 * Makes an empty store.
 */
int
cryc_store_init(
	struct cryc_store *store)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(store);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Frees every entry and the lock; the store must be made again before
 * reuse.
 */
void
cryc_store_free(
	struct cryc_store *store)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(store);
}

/*
 * Adds one certificate from its DER (copied).
 */
int
cryc_store_add_der(
	struct cryc_store *store,
	const uint8_t *der,
	size_t size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(store);
	UNUSED_PARAMETER(der);
	UNUSED_PARAMETER(size);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Adds every CERTIFICATE block of a PEM text (cryc_pem_next_certificate).
 */
int
cryc_store_add_pem(
	struct cryc_store *store,
	const char *text,
	size_t size,
	size_t *added)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(store);
	UNUSED_PARAMETER(text);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(added);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(store);
	UNUSED_PARAMETER(path);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Adds the system's roots (cryc_os_system_roots, with this store's add_der
 * and add_file).
 */
int
cryc_store_add_system(
	struct cryc_store *store)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(store);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(store);
	UNUSED_PARAMETER(peer);
	UNUSED_PARAMETER(peer_count);
	UNUSED_PARAMETER(params);
	UNUSED_PARAMETER(result);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}
