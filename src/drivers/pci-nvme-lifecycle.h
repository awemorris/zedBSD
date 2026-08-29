/*
 * PCI NVMe attach/cleanup transaction ledger
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * This is a private driver helper.  It deliberately contains no PCI, DMA,
 * interrupt, or allocator calls: production supplies those operations, while
 * the host fixture drives the exact same ordering and failure decisions.
 */
#ifndef ZEDBSD_DRIVERS_PCI_NVME_LIFECYCLE_H
#define ZEDBSD_DRIVERS_PCI_NVME_LIFECYCLE_H

#include <errno.h>
#include <stddef.h>
#include <string.h>

enum drv_nvme_lifecycle_event {
	DRV_NVME_LIFECYCLE_BAR_CLAIMED,
	DRV_NVME_LIFECYCLE_BAR_SNAPSHOTTED,
	DRV_NVME_LIFECYCLE_PCI_STATE_SAVED,
	DRV_NVME_LIFECYCLE_BAR_MAPPED,
	DRV_NVME_LIFECYCLE_PCI_COMMAND_CHANGED,
	DRV_NVME_LIFECYCLE_CONTROLLER_CLAIMED,
	DRV_NVME_LIFECYCLE_DMA_ALLOCATED,
	DRV_NVME_LIFECYCLE_IRQ_ALLOCATED,
	DRV_NVME_LIFECYCLE_IRQ_ESTABLISHED,
	DRV_NVME_LIFECYCLE_CONTROLLER_ENABLED
};

struct drv_nvme_lifecycle {
	unsigned bar_claimed;
	unsigned bar_snapshotted;
	unsigned pci_state_saved;
	unsigned bar_mapped;
	unsigned pci_command_changed;
	unsigned controller_claimed;
	unsigned controller_enabled;
	unsigned controller_disabled;
	unsigned master_disable_required;
	unsigned master_disabled;
	unsigned dma_allocated;
	unsigned irq_allocated;
	unsigned irq_established;
	unsigned irq_may_be_busy;
	unsigned cleanup_started;
	unsigned quarantined;
	unsigned completed;
	unsigned failure_count;
	int last_error;
};

struct drv_nvme_lifecycle_ops {
	int (*controller_disable)(void *context);
	int (*bus_master_disable)(void *context);
	int (*irq_disestablish)(void *context);
	int (*irq_drain)(void *context);
	void (*irq_free)(void *context);
	void (*dma_free)(void *context);
	void (*bar_unmap)(void *context);
	int (*bar_restore)(void *context);
	int (*pci_state_restore)(void *context);
	void (*bar_release)(void *context);
};

static inline void
drv_nvme_lifecycle_init(struct drv_nvme_lifecycle *lifecycle)
{
	if (lifecycle != NULL)
		memset(lifecycle, 0, sizeof(*lifecycle));
}

/*
 * Record each attach acquisition immediately after it succeeds.  The strict
 * prefix ordering makes an omitted ownership transition visible in both the
 * fixture and production review.
 */
static inline int
drv_nvme_lifecycle_record(struct drv_nvme_lifecycle *lifecycle,
	enum drv_nvme_lifecycle_event event)
{
	if (lifecycle == NULL || lifecycle->cleanup_started ||
	    lifecycle->completed)
		return EINVAL;
	switch (event) {
	case DRV_NVME_LIFECYCLE_BAR_CLAIMED:
		if (lifecycle->bar_claimed)
			return EALREADY;
		lifecycle->bar_claimed = 1;
		return 0;
	case DRV_NVME_LIFECYCLE_BAR_SNAPSHOTTED:
		if (!lifecycle->bar_claimed || lifecycle->bar_snapshotted)
			return EINVAL;
		lifecycle->bar_snapshotted = 1;
		return 0;
	case DRV_NVME_LIFECYCLE_PCI_STATE_SAVED:
		if (!lifecycle->bar_snapshotted || lifecycle->pci_state_saved)
			return EINVAL;
		lifecycle->pci_state_saved = 1;
		return 0;
	case DRV_NVME_LIFECYCLE_BAR_MAPPED:
		if (!lifecycle->pci_command_changed || lifecycle->bar_mapped)
			return EINVAL;
		lifecycle->bar_mapped = 1;
		return 0;
	case DRV_NVME_LIFECYCLE_PCI_COMMAND_CHANGED:
		if (!lifecycle->pci_state_saved || lifecycle->bar_mapped ||
		    lifecycle->pci_command_changed)
			return EINVAL;
		lifecycle->pci_command_changed = 1;
		return 0;
	case DRV_NVME_LIFECYCLE_CONTROLLER_CLAIMED:
		if (!lifecycle->pci_command_changed ||
		    lifecycle->controller_claimed)
			return EINVAL;
		lifecycle->controller_claimed = 1;
		lifecycle->master_disable_required = 1;
		return 0;
	case DRV_NVME_LIFECYCLE_DMA_ALLOCATED:
		if (!lifecycle->controller_claimed || lifecycle->dma_allocated)
			return EINVAL;
		lifecycle->dma_allocated = 1;
		return 0;
	case DRV_NVME_LIFECYCLE_IRQ_ALLOCATED:
		if (!lifecycle->dma_allocated || lifecycle->irq_allocated)
			return EINVAL;
		lifecycle->irq_allocated = 1;
		return 0;
	case DRV_NVME_LIFECYCLE_IRQ_ESTABLISHED:
		if (!lifecycle->irq_allocated || lifecycle->irq_established)
			return EINVAL;
		lifecycle->irq_established = 1;
		lifecycle->irq_may_be_busy = 1;
		return 0;
	case DRV_NVME_LIFECYCLE_CONTROLLER_ENABLED:
		if (!lifecycle->irq_established ||
		    lifecycle->controller_enabled)
			return EINVAL;
		lifecycle->controller_enabled = 1;
		return 0;
	}
	return EINVAL;
}

static inline int
drv_nvme_lifecycle_fail(struct drv_nvme_lifecycle *lifecycle, int error)
{
	if (error == 0)
		error = EIO;
	lifecycle->last_error = error;
	lifecycle->failure_count++;
	lifecycle->quarantined = 1;
	return error;
}

/*
 * Run cleanup in the only DMA-safe order:
 *
 * controller -> bus master -> handler -> in-flight IRQs -> IRQ allocation ->
 * DMA -> BAR mapping/address -> saved PCI command -> BAR claim.
 *
 * A fallible action is committed to the ledger only after it succeeds.  The
 * caller may retain a quarantined object or retry; either choice cannot free a
 * successfully released resource twice.
 */
static inline int
drv_nvme_lifecycle_cleanup(struct drv_nvme_lifecycle *lifecycle,
	const struct drv_nvme_lifecycle_ops *ops, void *context)
{
	int error;

	if (lifecycle == NULL || ops == NULL)
		return EINVAL;
	if (lifecycle->completed)
		return 0;
	lifecycle->cleanup_started = 1;

	if (lifecycle->controller_claimed &&
	    !lifecycle->controller_disabled) {
		if (ops->controller_disable == NULL)
			return drv_nvme_lifecycle_fail(lifecycle, EINVAL);
		error = ops->controller_disable(context);
		if (error != 0)
			return drv_nvme_lifecycle_fail(lifecycle, error);
		lifecycle->controller_disabled = 1;
		lifecycle->controller_enabled = 0;
	}
	if (lifecycle->master_disable_required &&
	    !lifecycle->master_disabled) {
		if (ops->bus_master_disable == NULL)
			return drv_nvme_lifecycle_fail(lifecycle, EINVAL);
		error = ops->bus_master_disable(context);
		if (error != 0)
			return drv_nvme_lifecycle_fail(lifecycle, error);
		lifecycle->master_disabled = 1;
	}
	if (lifecycle->irq_established) {
		if (ops->irq_disestablish == NULL)
			return drv_nvme_lifecycle_fail(lifecycle, EINVAL);
		error = ops->irq_disestablish(context);
		if (error != 0)
			return drv_nvme_lifecycle_fail(lifecycle, error);
		lifecycle->irq_established = 0;
	}
	if (lifecycle->irq_may_be_busy) {
		if (ops->irq_drain == NULL)
			return drv_nvme_lifecycle_fail(lifecycle, EINVAL);
		error = ops->irq_drain(context);
		if (error != 0)
			return drv_nvme_lifecycle_fail(lifecycle, error);
		lifecycle->irq_may_be_busy = 0;
	}
	if (lifecycle->irq_allocated) {
		if (ops->irq_free == NULL)
			return drv_nvme_lifecycle_fail(lifecycle, EINVAL);
		ops->irq_free(context);
		lifecycle->irq_allocated = 0;
	}
	if (lifecycle->dma_allocated) {
		if (ops->dma_free == NULL)
			return drv_nvme_lifecycle_fail(lifecycle, EINVAL);
		ops->dma_free(context);
		lifecycle->dma_allocated = 0;
	}
	lifecycle->controller_claimed = 0;

	if (lifecycle->bar_mapped) {
		if (ops->bar_unmap == NULL)
			return drv_nvme_lifecycle_fail(lifecycle, EINVAL);
		ops->bar_unmap(context);
		lifecycle->bar_mapped = 0;
	}
	if (lifecycle->bar_snapshotted) {
		if (ops->bar_restore == NULL)
			return drv_nvme_lifecycle_fail(lifecycle, EINVAL);
		error = ops->bar_restore(context);
		if (error != 0)
			return drv_nvme_lifecycle_fail(lifecycle, error);
		lifecycle->bar_snapshotted = 0;
	}
	if (lifecycle->pci_state_saved &&
	    lifecycle->pci_command_changed) {
		if (ops->pci_state_restore == NULL)
			return drv_nvme_lifecycle_fail(lifecycle, EINVAL);
		error = ops->pci_state_restore(context);
		if (error != 0)
			return drv_nvme_lifecycle_fail(lifecycle, error);
		lifecycle->pci_command_changed = 0;
	}
	lifecycle->pci_state_saved = 0;
	if (lifecycle->bar_claimed) {
		if (ops->bar_release == NULL)
			return drv_nvme_lifecycle_fail(lifecycle, EINVAL);
		ops->bar_release(context);
		lifecycle->bar_claimed = 0;
	}
	lifecycle->last_error = 0;
	lifecycle->quarantined = 0;
	lifecycle->completed = 1;
	return 0;
}

#endif
