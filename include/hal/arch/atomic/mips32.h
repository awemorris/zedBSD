/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS32 lock word and spin hint of <hal/atomic.h>.
 *
 * <hal/atomic.h> reads this file inside its own include guard.  The file
 * has no guard of its own, so that a reader which undefines the atomic
 * macros together with that guard gets them again from a later read.
 */
#if !defined(HAL_HAL_ATOMIC_H)
# error "<hal/arch/atomic/mips32.h> is read through <hal/atomic.h>"
#endif

/*
 * Takes a lock word: swaps in one with acquire ordering and reports
 * whether the word was zero.  The swap is retried until the
 * store-conditional holds.
 */
#define hal_atomic_uint_try_acquire(value)				\
	({								\
		volatile unsigned *hal_atomic_word_ = (value);		\
		unsigned hal_atomic_previous_;				\
		unsigned hal_atomic_one_;				\
									\
		__asm__ volatile(".set push\n\t"			\
				 ".set noreorder\n"			\
				 "1:\tll %0, 0(%2)\n\t"			\
				 "li %1, 1\n\t"				\
				 "sc %1, 0(%2)\n\t"			\
				 "beqz %1, 1b\n\t"			\
				 "nop\n\t"				\
				 "sync\n\t"				\
				 ".set pop"				\
				 : "=&r"(hal_atomic_previous_),		\
				   "=&r"(hal_atomic_one_)		\
				 : "r"(hal_atomic_word_)		\
				 : "memory");				\
		hal_atomic_previous_ == 0U;				\
	})

/*
 * Lets the processor rest in a spin.
 *
 * MIPS32 release 2 has no spin hint outside the multithreading
 * extension: its pause waits for the link bit to clear, which is not the
 * meaning of a spin, so the wait slot is an ordinary no-op.
 */
#define hal_atomic_relax()						\
	__asm__ volatile("nop" ::: "memory")
