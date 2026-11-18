/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The bit helpers the register headers of this directory are written
 * with: a bit, a field mask from its high and low bit, and the page size.
 */

#ifndef DRIVERS_GPU_I915_INTEL_BITS_H
#define DRIVERS_GPU_I915_INTEL_BITS_H

/* A 32-bit and a 64-bit bit, a 32-bit and a 64-bit field mask, the page size. */
#define I915_INC_BIT(n)			(1U << (n))
#define I915_INC_BIT64(n)		(1ULL << (n))
#define I915_INC_GENMASK(h, l)		(((0xffffffffU) >> (31U - (h))) & ((0xffffffffU) << (l)))
#define I915_INC_GENMASK64(h, l)	(((0xffffffffffffffffULL) >> (63U - (h))) & ((0xffffffffffffffffULL) << (l)))
#define I915_INC_PAGE_SIZE		4096U

#endif /* DRIVERS_GPU_I915_INTEL_BITS_H */
