/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The AArch64 lock word and spin hint of <hal/atomic.h>.
 *
 * <hal/atomic.h> reads this file inside its own include guard.  The file
 * has no guard of its own, so that a reader which undefines the atomic
 * macros together with that guard gets them again from a later read.
 */
#if !defined(HAL_HAL_ATOMIC_H)
# error "<hal/arch/atomic/aarch64.h> is read through <hal/atomic.h>"
#endif

/*
 * Takes a lock word: stores one where the word held zero, with acquire
 * ordering, and reports whether the word was zero.  A word found held is
 * left with clrex, so that the exclusive monitor is not left open.
 */
#define hal_atomic_uint_try_acquire(value)				\
	({								\
		volatile unsigned *hal_atomic_word_ = (value);		\
		unsigned hal_atomic_previous_;				\
		unsigned hal_atomic_status_;				\
									\
		__asm__ volatile("1: ldaxr %w0, [%2]\n"			\
				 "   cbnz %w0, 2f\n"			\
				 "   stxr %w1, %w3, [%2]\n"		\
				 "   cbnz %w1, 1b\n"			\
				 "   b 3f\n"				\
				 "2: clrex\n"				\
				 "3:\n"					\
				 : "=&r"(hal_atomic_previous_),		\
				   "=&r"(hal_atomic_status_)		\
				 : "r"(hal_atomic_word_), "r"(1U)	\
				 : "memory");				\
		hal_atomic_previous_ == 0U;				\
	})

/*
 * Lets the processor rest in a spin.
 */
#define hal_atomic_relax()						\
	__asm__ volatile("yield" ::: "memory")
