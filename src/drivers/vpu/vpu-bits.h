/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The bit writer the parameter set writers share (vpu-h264-ps.c,
 * vpu-h265-ps.c): an RBSP written most significant bit first into a
 * zeroed buffer, with the fixed-length and Exp-Golomb codes, the trailing
 * bits, and the NAL unit made from it (a start code, the header bytes and
 * the RBSP with emulation prevention).  No hardware: a host test runs it.
 */

#ifndef KERN_DRIVERS_VPU_VPU_BITS_H
#define KERN_DRIVERS_VPU_VPU_BITS_H

#include <stddef.h>
#include <stdint.h>

/*
 * An RBSP being written: its buffer, its capacity in bits and the next
 * bit's position.  error is set, and stays set, when a write would run
 * past the capacity; such a write writes nothing.
 */
struct vpu_bits {
	uint8_t *data;
	size_t capacity_bits;
	size_t position;
	int error;
};

/* Starts writing into data of capacity bytes, which are zeroed. */
void vpu_bits_start(struct vpu_bits *bits, uint8_t *data, size_t capacity);

/* Writes the count (at most 32) low bits of value, most significant first. */
void vpu_bits_put(struct vpu_bits *bits, uint32_t value, unsigned count);

/* Writes one bit: whether flags has flag set. */
void vpu_bits_flag(struct vpu_bits *bits, uint32_t flags, uint32_t flag);

/* Writes an unsigned Exp-Golomb code ue(v). */
void vpu_bits_ue(struct vpu_bits *bits, uint32_t value);

/* Writes a signed Exp-Golomb code se(v); INT32_MIN cannot be coded and must not be given. */
void vpu_bits_se(struct vpu_bits *bits, int32_t value);

/* Writes rbsp_trailing_bits: a one, then zeros to the byte boundary. */
void vpu_bits_trailing(struct vpu_bits *bits);

/*
 * Writes a NAL unit: a four-byte start code, the header_bytes (1 or 2)
 * bytes of header (most significant first), then the RBSP's bytes with
 * emulation prevention (a 0x03 after any two zeros that precede a byte of
 * three or less).  ENOSPC when it does not fit in capacity.
 */
int vpu_bits_nal(uint32_t header, unsigned header_bytes, const uint8_t *rbsp, size_t bytes, uint8_t *output, size_t capacity, size_t *written);

#endif
