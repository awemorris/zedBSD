/*
 * PCI NVMe terminal-shutdown best-effort transaction
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * This private helper deliberately contains no hardware calls.  A shutdown
 * must attempt every later safety boundary even when admission drain or the
 * controller's normal shutdown notification fails.
 */
#ifndef ZEDBSD_DRIVERS_PCI_NVME_SHUTDOWN_LIFECYCLE_H
#define ZEDBSD_DRIVERS_PCI_NVME_SHUTDOWN_LIFECYCLE_H

#include <errno.h>
#include <string.h>

struct drv_nvme_shutdown_lifecycle {
	unsigned running;
	unsigned completed;
	unsigned admission_attempted;
	unsigned admission_stopped;
	unsigned shutdown_attempted;
	unsigned shutdown_completed;
	unsigned disable_attempted;
	unsigned controller_disabled;
	unsigned master_attempted;
	unsigned master_disabled;
	unsigned hardware_dma_safe;
	unsigned failure_count;
	int admission_error;
	int shutdown_error;
	int disable_error;
	int master_error;
	int first_error;
};

struct drv_nvme_shutdown_ops {
	int (*stop_admission)(void *context);
	int (*shutdown_normal)(void *context);
	int (*controller_disable)(void *context);
	int (*bus_master_disable)(void *context);
};

static inline void
drv_nvme_shutdown_lifecycle_init(
	struct drv_nvme_shutdown_lifecycle *lifecycle)
{
	if (lifecycle != NULL)
		memset(lifecycle, 0, sizeof(*lifecycle));
}

static inline void
drv_nvme_shutdown_lifecycle_record_error(
	struct drv_nvme_shutdown_lifecycle *lifecycle, int error)
{
	if (error == 0)
		return;
	if (lifecycle->first_error == 0)
		lifecycle->first_error = error;
	lifecycle->failure_count++;
}

static inline int
drv_nvme_shutdown_lifecycle_run(
	struct drv_nvme_shutdown_lifecycle *lifecycle,
	const struct drv_nvme_shutdown_ops *ops, void *context)
{
	int error;

	if (lifecycle == NULL || ops == NULL || ops->stop_admission == NULL ||
	    ops->shutdown_normal == NULL || ops->controller_disable == NULL ||
	    ops->bus_master_disable == NULL)
		return EINVAL;
	if (lifecycle->completed)
		return lifecycle->first_error;
	if (lifecycle->running)
		return EBUSY;
	lifecycle->running = 1;

	lifecycle->admission_attempted = 1;
	error = ops->stop_admission(context);
	lifecycle->admission_error = error;
	if (error == 0)
		lifecycle->admission_stopped = 1;
	drv_nvme_shutdown_lifecycle_record_error(lifecycle, error);

	/* SHN/SHST remains worth attempting after an admission timeout, and the
	 * two hard quiescence boundaries remain mandatory after any SHN failure. */
	lifecycle->shutdown_attempted = 1;
	error = ops->shutdown_normal(context);
	lifecycle->shutdown_error = error;
	if (error == 0)
		lifecycle->shutdown_completed = 1;
	drv_nvme_shutdown_lifecycle_record_error(lifecycle, error);

	lifecycle->disable_attempted = 1;
	error = ops->controller_disable(context);
	lifecycle->disable_error = error;
	if (error == 0)
		lifecycle->controller_disabled = 1;
	drv_nvme_shutdown_lifecycle_record_error(lifecycle, error);

	/* Bus-master disable is independent of CC.EN/CSTS.RDY and must always run. */
	lifecycle->master_attempted = 1;
	error = ops->bus_master_disable(context);
	lifecycle->master_error = error;
	if (error == 0)
		lifecycle->master_disabled = 1;
	drv_nvme_shutdown_lifecycle_record_error(lifecycle, error);

	/* Either independently verified boundary prevents further controller DMA. */
	lifecycle->hardware_dma_safe = lifecycle->controller_disabled ||
	    lifecycle->master_disabled;
	lifecycle->running = 0;
	lifecycle->completed = 1;
	return lifecycle->first_error;
}

#endif
