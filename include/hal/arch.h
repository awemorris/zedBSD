/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Historical Architecture Library
 */

#ifndef HAL_HAL_ARCH_H
#define HAL_HAL_ARCH_H

#if !defined(__ASSEMBLER__) && !defined(_ASM_SRC_)
# include <hal/types.h>
#endif

/*
 * Signal-frame contract
 * ---------------------
 * Each architecture defines struct hal_task_signal_frame_head with the
 * common token and context_pointer members.  A port also defines:
 *
 *   HAL_TASK_SIGNAL_FRAME_ALIGNMENT
 *   HAL_TASK_SIGNAL_FRAME_{TOKEN,CONTEXT_POINTER}_OFFSET
 *   HAL_TASK_SIGNAL_RESTORER_{TOKEN,CONTEXT}_OFFSET
 *
 * FRAME offsets are measured from the beginning of the head.  RESTORER
 * offsets are measured from the user stack pointer on entry to
 * __signal_restorer, after any handler return-address pop or stack bias.
 *
 * restorer, signo, and info_pointer are optional head members.  Their
 * HAS_* macro is always 0 or 1; the corresponding FRAME_*_OFFSET macro is
 * defined only when the member exists.  Assembly must use RESTORER offsets,
 * while generic C code selects optional members with the HAS_* macros.
 */

#if defined(HAL_ARCH_I386)
# include <hal/arch/i386.h>
#elif defined(HAL_ARCH_AMD64)
# include <hal/arch/amd64.h>
#elif defined(HAL_ARCH_ARM64)
# include <hal/arch/aarch64.h>
#elif defined(HAL_ARCH_M68K)
# include <hal/arch/m68030.h>
#elif defined(HAL_ARCH_SPARCV9)
# include <hal/arch/sparcv9.h>
#endif

#endif
