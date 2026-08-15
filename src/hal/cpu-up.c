/* Generic uniprocessor implementation of the public CPU lifecycle API. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>

hal_cpu_id_t
hal_cpu_current(void)
{
	return 0;
}

unsigned
hal_cpu_count(void)
{
	return 1;
}

void
hal_cpu_ready_mask(struct hal_cpu_mask *result)
{
	if (result == NULL)
		return;
	hal_cpu_mask_zero(result);
	hal_cpu_mask_set(result, 0);
}

int
hal_cpu_start_others(void)
{
	return HAL_OK;
}

int
hal_cpu_notify(hal_cpu_id_t cpu)
{
	return cpu == 0 ? HAL_ERR_UNSUPPORTED : HAL_ERR_INVALID;
}

int
hal_cpu_notify_mask(const struct hal_cpu_mask *targets)
{
	hal_cpu_id_t cpu;

	if (targets == NULL || !hal_cpu_mask_test(targets, 0))
		return HAL_ERR_INVALID;
	for (cpu = 1; cpu < HAL_CPU_MAX; cpu++)
		if (hal_cpu_mask_test(targets, cpu))
			return HAL_ERR_INVALID;
	return HAL_ERR_UNSUPPORTED;
}

_Noreturn void
hal_cpu_park(void)
{
	(void)hal_irq_disable();
	for (;;)
		hal_halt();
}

_Noreturn void
hal_cpu_panic_all(void)
{
	hal_cpu_park();
}
