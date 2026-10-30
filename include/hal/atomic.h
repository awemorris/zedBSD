/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef HAL_HAL_ATOMIC_H
#define HAL_HAL_ATOMIC_H

#include <stdint.h>

/*
 * Atomic operations
 * -----------------
 * This header is a contract of macros only: it declares nothing at file
 * scope, so a translation unit that reads it and then undefines its macros
 * keeps no name of the HAL.
 *
 * It is expected to be read through <hal/hal.h>, after <hal/arch.h> has
 * given HAL_ATOMIC_STYLE_NATIVE, HAL_ATOMIC_STYLE_ENTER and the port's
 * HAL_ATOMIC_STYLE.  A user that defines those three itself, as the
 * kernel's own atomic header does from the build configuration, may also
 * read it alone.  Such a user undefines the macros together with the
 * include guard HAL_HAL_ATOMIC_H, so that a later <hal/hal.h> in the same
 * translation unit reads the header again.
 *
 * Besides the operations below, which follow the style, every port gives
 * two macros of its own processor, from <hal/arch/atomic/<arch>.h> chosen
 * by the build's HAL_ARCH_* definition:
 *
 *   hal_atomic_uint_try_acquire(value)
 *	Stores one into the unsigned lock word at value where it held zero,
 *	with acquire ordering, and is nonzero when it did.  The port's own
 *	instruction sequence, usable as the guard of an ENTER port's
 *	serialization region.
 *
 *   hal_atomic_relax()
 *	Lets the processor rest for one step of a spin, with a compiler
 *	barrier.
 *
 * A port whose sequence is a function declares it inside the macro's
 * block, as the ENTER macros below declare their region.
 */
#if !defined(HAL_ATOMIC_STYLE) || !defined(HAL_ATOMIC_STYLE_ENTER) || \
    !defined(HAL_ATOMIC_STYLE_NATIVE)
# error "HAL_ATOMIC_STYLE must be defined before <hal/atomic.h>"
#endif

#if HAL_ATOMIC_STYLE == HAL_ATOMIC_STYLE_ENTER

/*
 * Atomic fallback serialization
 * -----------------------------
 * HAL_ATOMIC_STYLE_ENTER ports serialize the short plain-memory sequences
 * used to emulate atomic operations.  The region is non-recursive, cannot
 * sleep, and must exclude every CPU.  A local IRQ mask is sufficient only
 * for a HAL which guarantees uniprocessor execution.
 *
 * Both enter and leave are full compiler barriers.  hal_atomic_enter()
 * returns a cookie which the matching hal_atomic_leave() takes back.
 *
 * The two functions are declared inside the block of each macro below, not
 * at file scope.  The port's definitions are preceded by the same
 * declarations at file scope, from HAL_ATOMIC_ENTER_DECLARE(), so that a
 * definition which drifts from them does not compile.
 */
#define HAL_ATOMIC_ENTER_DECLARE()					\
	uintptr_t hal_atomic_enter(void);				\
	void hal_atomic_leave(uintptr_t cookie)

#define hal_atomic_load_acquire(pointer)					\
	({									\
		HAL_ATOMIC_ENTER_DECLARE();					\
		__auto_type hal_atomic_pointer_ = (pointer);			\
		uintptr_t hal_atomic_cookie_ = hal_atomic_enter();		\
		__auto_type hal_atomic_result_ = *hal_atomic_pointer_;		\
		hal_atomic_leave(hal_atomic_cookie_);				\
		hal_atomic_result_;						\
	})

#define hal_atomic_load_relaxed(pointer)					\
	hal_atomic_load_acquire(pointer)

#define hal_atomic_store_release(pointer, value)				\
	do {									\
		HAL_ATOMIC_ENTER_DECLARE();					\
		__auto_type hal_atomic_pointer_ = (pointer);			\
		__auto_type hal_atomic_value_ = (value);			\
		uintptr_t hal_atomic_cookie_ = hal_atomic_enter();		\
		*hal_atomic_pointer_ = hal_atomic_value_;			\
		hal_atomic_leave(hal_atomic_cookie_);				\
	} while (0)

#define hal_atomic_store_relaxed(pointer, value)				\
	hal_atomic_store_release((pointer), (value))

#define hal_atomic_fetch_add_relaxed(pointer, value)				\
	({									\
		HAL_ATOMIC_ENTER_DECLARE();					\
		__auto_type hal_atomic_pointer_ = (pointer);			\
		__auto_type hal_atomic_value_ = (value);			\
		uintptr_t hal_atomic_cookie_ = hal_atomic_enter();		\
		__auto_type hal_atomic_previous_ = *hal_atomic_pointer_;	\
		*hal_atomic_pointer_ = hal_atomic_previous_ +			\
		    hal_atomic_value_;						\
		hal_atomic_leave(hal_atomic_cookie_);				\
		hal_atomic_previous_;						\
	})

#define hal_atomic_fetch_add_release(pointer, value)				\
	hal_atomic_fetch_add_relaxed((pointer), (value))

#define hal_atomic_fetch_or_release(pointer, value)				\
	({									\
		HAL_ATOMIC_ENTER_DECLARE();					\
		__auto_type hal_atomic_pointer_ = (pointer);			\
		__auto_type hal_atomic_value_ = (value);			\
		uintptr_t hal_atomic_cookie_ = hal_atomic_enter();		\
		__auto_type hal_atomic_previous_ = *hal_atomic_pointer_;	\
		*hal_atomic_pointer_ = hal_atomic_previous_ |			\
		    hal_atomic_value_;						\
		hal_atomic_leave(hal_atomic_cookie_);				\
		hal_atomic_previous_;						\
	})

#define hal_atomic_compare_exchange_serialized(pointer, expected, desired)	\
	({									\
		HAL_ATOMIC_ENTER_DECLARE();					\
		__auto_type hal_atomic_pointer_ = (pointer);			\
		__auto_type hal_atomic_expected_ = (expected);			\
		__auto_type hal_atomic_desired_ = (desired);			\
		uintptr_t hal_atomic_cookie_ = hal_atomic_enter();		\
		__auto_type hal_atomic_current_ = *hal_atomic_pointer_;		\
		int hal_atomic_exchanged_ =					\
		    hal_atomic_current_ == *hal_atomic_expected_;		\
		if (hal_atomic_exchanged_)					\
			*hal_atomic_pointer_ = hal_atomic_desired_;		\
		else								\
			*hal_atomic_expected_ = hal_atomic_current_;		\
		hal_atomic_leave(hal_atomic_cookie_);				\
		hal_atomic_exchanged_;						\
	})

#define hal_atomic_compare_exchange_acq_rel(pointer, expected, desired)	\
	hal_atomic_compare_exchange_serialized((pointer), (expected),	\
		 (desired))

#define hal_atomic_compare_exchange_acquire(pointer, expected, desired)	\
	hal_atomic_compare_exchange_serialized((pointer), (expected),	\
		 (desired))

#define hal_atomic_compare_exchange_release(pointer, expected, desired)	\
	hal_atomic_compare_exchange_serialized((pointer), (expected),	\
		(desired))

#define hal_atomic_fence_acquire()					\
	__asm__ volatile("" ::: "memory")

#else

/*
 * GCC and Clang compiler primitives are an implementation detail of the HAL.
 * Memory-order constants do not cross this interface into the kernel.
 */
#define hal_atomic_load_acquire(pointer)				\
	__atomic_load_n((pointer), __ATOMIC_ACQUIRE)

#define hal_atomic_load_relaxed(pointer)				\
	__atomic_load_n((pointer), __ATOMIC_RELAXED)

#define hal_atomic_store_release(pointer, value)			\
	__atomic_store_n((pointer), (value), __ATOMIC_RELEASE)

#define hal_atomic_store_relaxed(pointer, value)			\
	__atomic_store_n((pointer), (value), __ATOMIC_RELAXED)

#define hal_atomic_fetch_add_relaxed(pointer, value)			\
	__atomic_fetch_add((pointer), (value), __ATOMIC_RELAXED)

#define hal_atomic_fetch_add_release(pointer, value)			\
	__atomic_fetch_add((pointer), (value), __ATOMIC_RELEASE)

#define hal_atomic_fetch_or_release(pointer, value)			\
	__atomic_fetch_or((pointer), (value), __ATOMIC_RELEASE)

#define hal_atomic_compare_exchange_acq_rel(pointer, expected, desired)	\
	__atomic_compare_exchange_n((pointer), (expected), (desired),	\
		0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)

#define hal_atomic_compare_exchange_acquire(pointer, expected, desired) \
	__atomic_compare_exchange_n((pointer), (expected), (desired),	\
		0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)

#define hal_atomic_compare_exchange_release(pointer, expected, desired) \
	__atomic_compare_exchange_n((pointer), (expected), (desired),	\
		 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)

#define hal_atomic_fence_acquire()					\
	__atomic_thread_fence(__ATOMIC_ACQUIRE)

#endif

/*
 * The port's lock word and spin hint.
 */
#if defined(HAL_ARCH_I386)
# include <hal/arch/atomic/i386.h>
#elif defined(HAL_ARCH_AMD64)
# include <hal/arch/atomic/amd64.h>
#elif defined(HAL_ARCH_ARM64)
# include <hal/arch/atomic/aarch64.h>
#elif defined(HAL_ARCH_RISCV64)
# include <hal/arch/atomic/riscv64.h>
#elif defined(HAL_ARCH_RISCV32)
# include <hal/arch/atomic/riscv32.h>
#elif defined(HAL_ARCH_ARMV7)
# include <hal/arch/atomic/armv7.h>
#elif defined(HAL_ARCH_PPC64)
# include <hal/arch/atomic/ppc64.h>
#elif defined(HAL_ARCH_PPC32)
# include <hal/arch/atomic/ppc32.h>
#elif defined(HAL_ARCH_MIPS64)
# include <hal/arch/atomic/mips64.h>
#elif defined(HAL_ARCH_MIPS32)
# include <hal/arch/atomic/mips32.h>
#elif defined(HAL_ARCH_M68K) && defined(HAL_ARCH_M68040)
# include <hal/arch/atomic/m68040.h>
#elif defined(HAL_ARCH_M68K)
# include <hal/arch/atomic/m68030.h>
#elif defined(HAL_ARCH_SPARCV9)
# include <hal/arch/atomic/sparcv9.h>
#else

/*
 * Architecture-neutral host tests take the lock word with the compiler's
 * exchange and spin with a compiler barrier only.
 */
#define hal_atomic_uint_try_acquire(value)				\
	(__atomic_exchange_n((value), 1U, __ATOMIC_ACQUIRE) == 0U)

#define hal_atomic_relax()						\
	__asm__ volatile("" ::: "memory")

#endif

#endif
