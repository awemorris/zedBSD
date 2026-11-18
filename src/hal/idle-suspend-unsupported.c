/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Suspend-to-idle on a platform without it (ws052-p006, HAL v2 H1v2 and
 * H4v2): i386, arm64, sparcv9 and m68k have no state like S0ix, so the
 * kernel is told before it touches a device, and the IRQ suspension that
 * only a system sleep uses is refused the same way.
 */

#include <hal/hal.h>

/*
 * Reports that this platform cannot suspend to idle.
 */
int
hal_cpu_idle_suspend_supported(
	void)
{
	/* No suspend-safe idle state. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Returns at once: there is no suspend-to-idle state to enter.
 */
int
hal_cpu_idle_suspend(
	void)
{
	/* The same answer as the probe, without waiting. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Refuses a wake source: no system sleep holds the other IRQs here.
 */
int
hal_irq_set_wake(
	int irq,
	bool enable)
{
	/* Nothing to arm. */
	(void)irq;
	(void)enable;
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Refuses to hold the system-wide sources: no system sleep here.
 */
int
hal_irq_suspend(
	void)
{
	/* Nothing changed. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Reports that no suspension is held (none can be).
 */
int
hal_irq_resume(
	void)
{
	/* Never suspended. */
	return HAL_ERR_STATE;
}
