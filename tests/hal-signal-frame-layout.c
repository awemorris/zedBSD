/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/types.h>
#include <hal/arch.h>

#ifndef HAL_TASK_SIGNAL_FRAME_ALIGNMENT
#error "signal frame alignment is not defined"
#endif
#ifndef HAL_TASK_SIGNAL_FRAME_TOKEN_OFFSET
#error "signal frame token offset is not defined"
#endif
#ifndef HAL_TASK_SIGNAL_FRAME_CONTEXT_POINTER_OFFSET
#error "signal frame context offset is not defined"
#endif
#ifndef HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET
#error "signal restorer token offset is not defined"
#endif
#ifndef HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET
#error "signal restorer context offset is not defined"
#endif

#if HAL_TASK_SIGNAL_FRAME_HAS_RESTORER
#ifndef HAL_TASK_SIGNAL_FRAME_RESTORER_OFFSET
#error "restorer field has no frame offset"
#endif
#else
#ifdef HAL_TASK_SIGNAL_FRAME_RESTORER_OFFSET
#error "restorer offset is defined for an absent field"
#endif
#endif

#if HAL_TASK_SIGNAL_FRAME_HAS_SIGNO
#ifndef HAL_TASK_SIGNAL_FRAME_SIGNO_OFFSET
#error "signo field has no frame offset"
#endif
#else
#ifdef HAL_TASK_SIGNAL_FRAME_SIGNO_OFFSET
#error "signo offset is defined for an absent field"
#endif
#endif

#if HAL_TASK_SIGNAL_FRAME_HAS_INFO_POINTER
#ifndef HAL_TASK_SIGNAL_FRAME_INFO_POINTER_OFFSET
#error "info_pointer field has no frame offset"
#endif
#else
#ifdef HAL_TASK_SIGNAL_FRAME_INFO_POINTER_OFFSET
#error "info_pointer offset is defined for an absent field"
#endif
#endif

_Static_assert(HAL_TASK_SIGNAL_FRAME_ALIGNMENT > 0,
	"signal frame alignment must be positive");
_Static_assert((HAL_TASK_SIGNAL_FRAME_ALIGNMENT &
	(HAL_TASK_SIGNAL_FRAME_ALIGNMENT - 1)) == 0,
	"signal frame alignment must be a power of two");

int
hal_signal_frame_layout_compile_fixture(void)
{
	return sizeof(struct hal_task_signal_frame_head) == 0;
}
