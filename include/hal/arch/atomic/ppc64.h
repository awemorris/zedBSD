/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 64-bit PowerPC lock word and spin hint of <hal/atomic.h>.
 *
 * <hal/atomic.h> reads this file inside its own include guard.  The file
 * has no guard of its own, so that a reader which undefines the atomic
 * macros together with that guard gets them again from a later read.
 */
#if !defined(HAL_HAL_ATOMIC_H)
# error "<hal/arch/atomic/ppc64.h> is read through <hal/atomic.h>"
#endif

/*
 * Takes a lock word: stores one with a reservation and reports whether
 * the word was zero.
 */
#define hal_atomic_uint_try_acquire(value)				\
	({								\
		volatile unsigned *hal_atomic_word_ = (value);		\
		unsigned hal_atomic_previous_;				\
									\
		__asm__ volatile("1:\tlwarx %0,0,%1\n"			\
				 "\tstwcx. %2,0,%1\n"			\
				 "\tbne- 1b\n"				\
				 "\tisync"				\
				 : "=&r"(hal_atomic_previous_)		\
				 : "r"(hal_atomic_word_), "r"(1U)	\
				 : "cr0", "memory");			\
		hal_atomic_previous_ == 0U;				\
	})

/*
 * Lets the processor rest in a spin.
 */
#define hal_atomic_relax()						\
	__asm__ volatile("" ::: "memory")
