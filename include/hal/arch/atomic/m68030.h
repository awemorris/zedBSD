/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MC68030 lock word and spin hint of <hal/atomic.h>.
 *
 * <hal/atomic.h> reads this file inside its own include guard.  The file
 * has no guard of its own, so that a reader which undefines the atomic
 * macros together with that guard gets them again from a later read.
 */
#if !defined(HAL_HAL_ATOMIC_H)
# error "<hal/arch/atomic/m68030.h> is read through <hal/atomic.h>"
#endif

/*
 * The declaration of the port's lock-word function, which the HAL defines
 * out of line with interrupts off.  The macro below declares it inside its
 * own block, so that a reader sees no name of the HAL at file scope; the
 * definition is preceded by the same declaration, so that a definition
 * which drifts from it does not compile.
 */
#define HAL_ATOMIC_TRY_ACQUIRE_DECLARE()				\
	_Bool hal_atomic_uint_try_acquire(volatile unsigned *hal_atomic_word)

/*
 * Takes a lock word: stores one where the word held zero and reports
 * whether the word was zero.
 */
#define hal_atomic_uint_try_acquire(value)				\
	({								\
		HAL_ATOMIC_TRY_ACQUIRE_DECLARE();			\
		hal_atomic_uint_try_acquire(value);			\
	})

/*
 * Lets the processor rest in a spin.  The processor has no spin hint, so
 * the step is a compiler barrier only.
 */
#define hal_atomic_relax()						\
	__asm__ volatile("" ::: "memory")
