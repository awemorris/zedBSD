/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The RV32 lock word and spin hint of <hal/atomic.h>.
 *
 * <hal/atomic.h> reads this file inside its own include guard.  The file
 * has no guard of its own, so that a reader which undefines the atomic
 * macros together with that guard gets them again from a later read.
 */
#if !defined(HAL_HAL_ATOMIC_H)
# error "<hal/arch/atomic/riscv32.h> is read through <hal/atomic.h>"
#endif

/*
 * Takes a lock word: swaps in one with acquire ordering and reports
 * whether the word was zero.
 */
#define hal_atomic_uint_try_acquire(value)				\
	({								\
		volatile unsigned *hal_atomic_word_ = (value);		\
		unsigned hal_atomic_previous_;				\
									\
		__asm__ volatile("amoswap.w.aq %0, %2, (%1)"		\
				 : "=&r"(hal_atomic_previous_)		\
				 : "r"(hal_atomic_word_), "r"(1U)	\
				 : "memory");				\
		hal_atomic_previous_ == 0U;				\
	})

/*
 * Lets the hart rest in a spin.  The pause hint is encoded as a fence
 * whose predecessor set is writes only and whose successor set is empty,
 * so a hart without the hint extension executes it as that ordinary
 * fence.
 */
#define hal_atomic_relax()						\
	__asm__ volatile("pause" ::: "memory")
