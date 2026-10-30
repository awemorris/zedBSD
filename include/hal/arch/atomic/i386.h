/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The i386 lock word and spin hint of <hal/atomic.h>.
 *
 * <hal/atomic.h> reads this file inside its own include guard.  The file
 * has no guard of its own, so that a reader which undefines the atomic
 * macros together with that guard gets them again from a later read.
 */
#if !defined(HAL_HAL_ATOMIC_H)
# error "<hal/arch/atomic/i386.h> is read through <hal/atomic.h>"
#endif

/*
 * Takes a lock word: exchanges one into it, which the processor locks
 * implicitly, and reports whether the word was zero.
 */
#define hal_atomic_uint_try_acquire(value)				\
	({								\
		volatile unsigned *hal_atomic_word_ = (value);		\
		unsigned hal_atomic_previous_ = 1U;			\
									\
		__asm__ volatile("xchgl %0, %1"				\
				 : "+r"(hal_atomic_previous_),		\
				   "+m"(*hal_atomic_word_)		\
				 :					\
				 : "memory");				\
		hal_atomic_previous_ == 0U;				\
	})

/*
 * Lets the processor rest in a spin.
 */
#define hal_atomic_relax()						\
	__asm__ volatile("pause" ::: "memory")
