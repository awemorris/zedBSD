/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host's OpenSSL 3.5 in place of the system's libcrypto-compat
 * <compat/crypto/crypto.h>, for the host unit tests of ssh: libcrypto-compat
 * has OpenSSL 3.5's names, arguments and return conventions, so the same
 * source builds against either.
 */

#ifndef SSH_TEST_HOST_CRYPTO_CRYPTO_H
#define SSH_TEST_HOST_CRYPTO_CRYPTO_H

#include <openssl/crypto.h>

#endif
