/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's cryptography (ws143-p004, design section 6.6, D5 b1): AES-128
 * encryption (FIPS-197), AES-CMAC (RFC 4493), and the Security Manager's
 * functions of the Bluetooth Core 5.4 (Vol 3 Part H §2.2): e, ah, c1, s1,
 * f4, f5, f6 and g2.  The elliptic curve work (P-256) is the controller's
 * (LE Read Local P-256 Public Key, LE Generate DHKey).
 *
 * Every value here is most significant byte first, as the Core writes its
 * functions; the Security Manager's PDUs carry them least significant
 * byte first, and smp.c turns them round.
 */

#ifndef BLUETOOTHD_CRYPTO_H
#define BLUETOOTHD_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

/* An AES block and key, and an address with its type for f5 and f6 (type first, then the address). */
#define BTD_BLOCK		16U
#define BTD_SMP_ADDRESS		7U

void btd_aes_encrypt(const uint8_t *key, const uint8_t *input, uint8_t *output);
void btd_cmac(const uint8_t *key, const uint8_t *message, size_t length, uint8_t *mac);
void btd_smp_ah(const uint8_t *key, const uint8_t *r, uint8_t *hash);
void btd_smp_c1(const uint8_t *key, const uint8_t *r, const uint8_t *pres, const uint8_t *preq, uint8_t rat, uint8_t iat, const uint8_t *ia, const uint8_t *ra, uint8_t *confirm);
void btd_smp_s1(const uint8_t *key, const uint8_t *r1, const uint8_t *r2, uint8_t *stk);
void btd_smp_f4(const uint8_t *u, const uint8_t *v, const uint8_t *x, uint8_t z, uint8_t *confirm);
void btd_smp_f5(const uint8_t *w, const uint8_t *n1, const uint8_t *n2, const uint8_t *a1, const uint8_t *a2, uint8_t *mac_key, uint8_t *ltk);
void btd_smp_f6(const uint8_t *w, const uint8_t *n1, const uint8_t *n2, const uint8_t *r, const uint8_t *io_capability, const uint8_t *a1, const uint8_t *a2, uint8_t *check);
uint32_t btd_smp_g2(const uint8_t *u, const uint8_t *v, const uint8_t *x, const uint8_t *y);

#endif
