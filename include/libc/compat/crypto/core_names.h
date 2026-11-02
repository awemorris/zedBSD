/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The names of the parameters libcrypto-compat takes or gives, with
 * OpenSSL's macro names and values: an EC key's group and public point
 * (EVP_PKEY_fromdata, EVP_PKEY_get_octet_string_param), and HKDF's digest,
 * key, salt and info (EVP_KDF_derive).
 * Values referenced: OpenSSL 3.5.8 util/perl/OpenSSL/paramnames.pm, from
 * which include/openssl/core_names.h is made (ALG_PARAM_DIGEST,
 * KDF_PARAM_KEY, KDF_PARAM_SALT, KDF_PARAM_INFO, PKEY_PARAM_GROUP_NAME,
 * PKEY_PARAM_PUB_KEY, PKEY_PARAM_ENCODED_PUBLIC_KEY).
 */

#ifndef KERN_COMPAT_CRYPTO_CORE_NAMES_H
#define KERN_COMPAT_CRYPTO_CORE_NAMES_H

/* The parameter type the names are used with. */
#include <compat/crypto/types.h>

/* An algorithm's digest, by name (a UTF-8 string, for HKDF "SHA256"). */
#define OSSL_ALG_PARAM_DIGEST			"digest"

/* HKDF's digest, input keying material, salt and info. */
#define OSSL_KDF_PARAM_DIGEST			OSSL_ALG_PARAM_DIGEST
#define OSSL_KDF_PARAM_KEY			"key"
#define OSSL_KDF_PARAM_SALT			"salt"
#define OSSL_KDF_PARAM_INFO			"info"

/* An EC key's group name (a UTF-8 string, "prime256v1") and its public point (an octet string, uncompressed). */
#define OSSL_PKEY_PARAM_GROUP_NAME		"group"
#define OSSL_PKEY_PARAM_PUB_KEY			"pub"
#define OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY	"encoded-pub-key"

#endif
