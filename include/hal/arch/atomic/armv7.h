/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ARMv7-A lock word and spin hint of <hal/atomic.h>.
 *
 * <hal/atomic.h> reads this file inside its own include guard.  The file
 * has no guard of its own, so that a reader which undefines the atomic
 * macros together with that guard gets them again from a later read.
 */
#if !defined(HAL_HAL_ATOMIC_H)
# error "<hal/arch/atomic/armv7.h> is read through <hal/atomic.h>"
#endif

/*
 * Takes a lock word: stores one where the word held zero, with acquire
 * ordering, and reports whether the word was zero.
 *
 * The store is retried while another observer broke the reservation.  A
 * word found held is left with clrex, so that the exclusive monitor is
 * not left open by the attempt.
 */
#define hal_atomic_uint_try_acquire(value)				\
	({								\
		volatile unsigned *hal_atomic_word_ = (value);		\
		unsigned hal_atomic_previous_;				\
		unsigned hal_atomic_failed_;				\
									\
		__asm__ volatile("1:\tldrex %0, [%2]\n\t"		\
				 "cmp %0, #0\n\t"			\
				 "bne 2f\n\t"				\
				 "strex %1, %3, [%2]\n\t"		\
				 "cmp %1, #0\n\t"			\
				 "bne 1b\n\t"				\
				 "dmb ish\n\t"				\
				 "b 3f\n"				\
				 "2:\tclrex\n"				\
				 "3:"					\
				 : "=&r"(hal_atomic_previous_),		\
				   "=&r"(hal_atomic_failed_)		\
				 : "r"(hal_atomic_word_), "r"(1U)	\
				 : "cc", "memory");			\
		hal_atomic_previous_ == 0U;				\
	})

/*
 * Lets the processor rest in a spin.
 *
 * yield is the ARMv7-A spin hint, which the Cortex-A9 runs as a no-op.
 * A spin does not wait with wfe and wake its waiters with sev: an sev
 * would also wake the ZYBO's second processor, which waits in the boot
 * ROM's wfe loop until the HAL starts it.
 */
#define hal_atomic_relax()						\
	__asm__ volatile("yield" ::: "memory")
