/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/* Begin consolidated pci-nvme.c. */
/*
 * PCI NVMe controller driver
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <drivers/pci-nvme.h>
#include <drivers/pci-nvme-protocol.h>
#include <drivers/pci.h>

/* Begin consolidated pci-nvme-lifecycle.h. */
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

/* Supports the drv nvme lifecycle init operation. */
/* Supports the drv nvme lifecycle init operation. */
static __inline void
drv_nvme_lifecycle_init(
	struct drv_nvme_lifecycle *lifecycle)
{
	/* Handles the lifecycle availability. */
	if (lifecycle != NULL)
		memset(lifecycle, 0, sizeof(*lifecycle));
}

/* Record each attach acquisition immediately after it succeeds.  The strict prefix ordering makes an omitted ownership transition visible in both the fixture and production review. */
/* Supports the drv nvme lifecycle record operation. */
static __inline int
drv_nvme_lifecycle_record(
	struct drv_nvme_lifecycle *lifecycle,
	enum drv_nvme_lifecycle_event event)
{
	/* Handles the lifecycle availability. */
	if (lifecycle == NULL || lifecycle->cleanup_started ||
	    lifecycle->completed)

		/* Returns the computed result. */
		return EINVAL;
	/* Dispatch the selected operation case. */
	switch (event) {
	case DRV_NVME_LIFECYCLE_BAR_CLAIMED:
		/* Handles the lifecycle condition. */
		if (lifecycle->bar_claimed)
			return EALREADY;
		lifecycle->bar_claimed = 1;

		/* Reports successful completion. */
		return 0;
	case DRV_NVME_LIFECYCLE_BAR_SNAPSHOTTED:
		/* Handles the lifecycle condition. */
		if (!lifecycle->bar_claimed || lifecycle->bar_snapshotted)
			return EINVAL;
		lifecycle->bar_snapshotted = 1;

		/* Reports successful completion. */
		return 0;
	case DRV_NVME_LIFECYCLE_PCI_STATE_SAVED:
		/* Handles the lifecycle condition. */
		if (!lifecycle->bar_snapshotted || lifecycle->pci_state_saved)
			return EINVAL;
		lifecycle->pci_state_saved = 1;

		/* Reports successful completion. */
		return 0;
	case DRV_NVME_LIFECYCLE_BAR_MAPPED:
		/* Handles the lifecycle condition. */
		if (!lifecycle->pci_command_changed || lifecycle->bar_mapped)
			return EINVAL;
		lifecycle->bar_mapped = 1;

		/* Reports successful completion. */
		return 0;
	case DRV_NVME_LIFECYCLE_PCI_COMMAND_CHANGED:
		/* Handles the lifecycle condition. */
		if (!lifecycle->pci_state_saved || lifecycle->bar_mapped ||
		    lifecycle->pci_command_changed)

			/* Returns the computed result. */
			return EINVAL;
		lifecycle->pci_command_changed = 1;

		/* Reports successful completion. */
		return 0;
	case DRV_NVME_LIFECYCLE_CONTROLLER_CLAIMED:
		/* Handles the lifecycle condition. */
		if (!lifecycle->pci_command_changed ||
		    lifecycle->controller_claimed)

			/* Returns the computed result. */
			return EINVAL;
		lifecycle->controller_claimed = 1;
		lifecycle->master_disable_required = 1;

		/* Reports successful completion. */
		return 0;
	case DRV_NVME_LIFECYCLE_DMA_ALLOCATED:
		/* Handles the lifecycle condition. */
		if (!lifecycle->controller_claimed || lifecycle->dma_allocated)
			return EINVAL;
		lifecycle->dma_allocated = 1;

		/* Reports successful completion. */
		return 0;
	case DRV_NVME_LIFECYCLE_IRQ_ALLOCATED:
		/* Handles the lifecycle condition. */
		if (!lifecycle->dma_allocated || lifecycle->irq_allocated)
			return EINVAL;
		lifecycle->irq_allocated = 1;

		/* Reports successful completion. */
		return 0;
	case DRV_NVME_LIFECYCLE_IRQ_ESTABLISHED:
		/* Handles the lifecycle condition. */
		if (!lifecycle->irq_allocated || lifecycle->irq_established)
			return EINVAL;
		lifecycle->irq_established = 1;
		lifecycle->irq_may_be_busy = 1;

		/* Reports successful completion. */
		return 0;
	case DRV_NVME_LIFECYCLE_CONTROLLER_ENABLED:
		/* Handles the lifecycle condition. */
		if (!lifecycle->irq_established ||
		    lifecycle->controller_enabled)

			/* Returns the computed result. */
			return EINVAL;
		lifecycle->controller_enabled = 1;

		/* Reports successful completion. */
		return 0;
	}

	/* Returns the computed result. */
	return EINVAL;
}

/* Supports the drv nvme lifecycle fail operation. */
/* Supports the drv nvme lifecycle fail operation. */
static __inline int
drv_nvme_lifecycle_fail(
	struct drv_nvme_lifecycle *lifecycle,
	int error)
{
	/* Checks the operation status. */
	if (error == 0)
		error = EIO;
	lifecycle->last_error = error;
	lifecycle->failure_count++;
	lifecycle->quarantined = 1;

	/* Returns the computed result. */
	return error;
}

/* Run cleanup in the only DMA-safe order: controller -> bus master -> handler -> in-flight IRQs -> IRQ allocation -> DMA -> BAR mapping/address -> saved PCI command -> BAR claim. A fallible action is committed to the ledger only after it succeeds.  The caller may retain a quarantined object or retry; either choice cannot free a successfully released resource twice. */
/* Supports the drv nvme lifecycle cleanup operation. */
static __inline int
drv_nvme_lifecycle_cleanup(
	struct drv_nvme_lifecycle *lifecycle,
	const struct drv_nvme_lifecycle_ops *ops,
	void *context)
{
	int function_result;
	int error;

	/* Handles the lifecycle availability. */
	if (lifecycle == NULL || ops == NULL)
		return EINVAL;

	/* Handles the lifecycle condition. */
	if (lifecycle->completed)
		return 0;
	lifecycle->cleanup_started = 1;

	/* Handles the lifecycle condition. */
	if (lifecycle->controller_claimed && !lifecycle->controller_disabled) {
		/* Handles the controller disable availability. */
		if (ops->controller_disable == NULL) {
			/* Obtains the drv nvme lifecycle fail result. */
			function_result =
				drv_nvme_lifecycle_fail(lifecycle, EINVAL);

			/* Returns the computed result. */
			return function_result;
		}
		error = ops->controller_disable(context);

		/* Checks the operation status. */
		if (error != 0) {
			/* Obtains the drv nvme lifecycle fail result. */
			function_result =
				drv_nvme_lifecycle_fail(lifecycle, error);

			/* Returns the computed result. */
			return function_result;
		}
		lifecycle->controller_disabled = 1;
		lifecycle->controller_enabled = 0;
	}

	/* Handles the lifecycle condition. */
	if (lifecycle->master_disable_required && !lifecycle->master_disabled) {
		/* Handles the bus master disable availability. */
		if (ops->bus_master_disable == NULL) {
			/* Obtains the drv nvme lifecycle fail result. */
			function_result =
				drv_nvme_lifecycle_fail(lifecycle, EINVAL);

			/* Returns the computed result. */
			return function_result;
		}
		error = ops->bus_master_disable(context);

		/* Checks the operation status. */
		if (error != 0) {
			/* Obtains the drv nvme lifecycle fail result. */
			function_result =
				drv_nvme_lifecycle_fail(lifecycle, error);

			/* Returns the computed result. */
			return function_result;
		}
		lifecycle->master_disabled = 1;
	}

	/* Handles the lifecycle condition. */
	if (lifecycle->irq_established) {
		/* Handles the irq disestablish availability. */
		if (ops->irq_disestablish == NULL) {
			/* Obtains the drv nvme lifecycle fail result. */
			function_result =
				drv_nvme_lifecycle_fail(lifecycle, EINVAL);

			/* Returns the computed result. */
			return function_result;
		}
		error = ops->irq_disestablish(context);

		/* Checks the operation status. */
		if (error != 0) {
			/* Obtains the drv nvme lifecycle fail result. */
			function_result =
				drv_nvme_lifecycle_fail(lifecycle, error);

			/* Returns the computed result. */
			return function_result;
		}
		lifecycle->irq_established = 0;
	}

	/* Handles the lifecycle condition. */
	if (lifecycle->irq_may_be_busy) {
		/* Handles the irq drain availability. */
		if (ops->irq_drain == NULL) {
			/* Obtains the drv nvme lifecycle fail result. */
			function_result =
				drv_nvme_lifecycle_fail(lifecycle, EINVAL);

			/* Returns the computed result. */
			return function_result;
		}
		error = ops->irq_drain(context);

		/* Checks the operation status. */
		if (error != 0) {
			/* Obtains the drv nvme lifecycle fail result. */
			function_result =
				drv_nvme_lifecycle_fail(lifecycle, error);

			/* Returns the computed result. */
			return function_result;
		}
		lifecycle->irq_may_be_busy = 0;
	}

	/* Handles the lifecycle condition. */
	if (lifecycle->irq_allocated) {
		/* Handles the irq free availability. */
		if (ops->irq_free == NULL) {
			/* Obtains the drv nvme lifecycle fail result. */
			function_result =
				drv_nvme_lifecycle_fail(lifecycle, EINVAL);

			/* Returns the computed result. */
			return function_result;
		}
		ops->irq_free(context);
		lifecycle->irq_allocated = 0;
	}

	/* Handles the lifecycle condition. */
	if (lifecycle->dma_allocated) {
		/* Handles the dma free availability. */
		if (ops->dma_free == NULL) {
			/* Obtains the drv nvme lifecycle fail result. */
			function_result =
				drv_nvme_lifecycle_fail(lifecycle, EINVAL);

			/* Returns the computed result. */
			return function_result;
		}
		ops->dma_free(context);
		lifecycle->dma_allocated = 0;
	}
	lifecycle->controller_claimed = 0;

	/* Handles the lifecycle condition. */
	if (lifecycle->bar_mapped) {
		/* Handles the bar unmap availability. */
		if (ops->bar_unmap == NULL) {
			/* Obtains the drv nvme lifecycle fail result. */
			function_result =
				drv_nvme_lifecycle_fail(lifecycle, EINVAL);

			/* Returns the computed result. */
			return function_result;
		}
		ops->bar_unmap(context);
		lifecycle->bar_mapped = 0;
	}

	/* Handles the lifecycle condition. */
	if (lifecycle->bar_snapshotted) {
		/* Handles the bar restore availability. */
		if (ops->bar_restore == NULL) {
			/* Obtains the drv nvme lifecycle fail result. */
			function_result =
				drv_nvme_lifecycle_fail(lifecycle, EINVAL);

			/* Returns the computed result. */
			return function_result;
		}
		error = ops->bar_restore(context);

		/* Checks the operation status. */
		if (error != 0) {
			/* Obtains the drv nvme lifecycle fail result. */
			function_result =
				drv_nvme_lifecycle_fail(lifecycle, error);

			/* Returns the computed result. */
			return function_result;
		}
		lifecycle->bar_snapshotted = 0;
	}

	/* Handles the lifecycle condition. */
	if (lifecycle->pci_state_saved && lifecycle->pci_command_changed) {
		/* Handles the pci state restore availability. */
		if (ops->pci_state_restore == NULL) {
			/* Obtains the drv nvme lifecycle fail result. */
			function_result =
				drv_nvme_lifecycle_fail(lifecycle, EINVAL);

			/* Returns the computed result. */
			return function_result;
		}
		error = ops->pci_state_restore(context);

		/* Checks the operation status. */
		if (error != 0) {
			/* Obtains the drv nvme lifecycle fail result. */
			function_result =
				drv_nvme_lifecycle_fail(lifecycle, error);

			/* Returns the computed result. */
			return function_result;
		}
		lifecycle->pci_command_changed = 0;
	}
	lifecycle->pci_state_saved = 0;

	/* Handles the lifecycle condition. */
	if (lifecycle->bar_claimed) {
		/* Handles the bar release availability. */
		if (ops->bar_release == NULL) {
			/* Obtains the drv nvme lifecycle fail result. */
			function_result =
				drv_nvme_lifecycle_fail(lifecycle, EINVAL);

			/* Returns the computed result. */
			return function_result;
		}
		ops->bar_release(context);
		lifecycle->bar_claimed = 0;
	}
	lifecycle->last_error = 0;
	lifecycle->quarantined = 0;
	lifecycle->completed = 1;

	/* Reports successful completion. */
	return 0;
}

#endif
/* End consolidated pci-nvme-lifecycle.h. */

/* Begin consolidated pci-nvme-io-lifecycle.h. */
/*
 * PCI NVMe serialized I/O ownership ledger
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * This private helper contains no hardware calls.  Production records the
 * transitions around those calls, while the host fixture injects completion,
 * timeout, reset, quarantine, shutdown, and detach orderings directly.
 */
#ifndef ZEDBSD_DRIVERS_PCI_NVME_IO_LIFECYCLE_H
#define ZEDBSD_DRIVERS_PCI_NVME_IO_LIFECYCLE_H

#include <errno.h>
#include <stdint.h>
#include <string.h>

struct drv_nvme_io_lifecycle {
	unsigned queues_online;
	unsigned accepting;
	unsigned bio_owned;
	unsigned command_owned;
	unsigned payload_dma_active;
	unsigned faulted;
	unsigned controller_quiesced;
	unsigned quarantined;
	unsigned released;
	unsigned bio_completion_count;
	unsigned command_completion_count;
	uint16_t command_id;
};

/*
 * Once disk_gone_if_idle() has made a namespace unreachable, its final
 * controller-cache FLUSH is a detach transaction obligation.  A failed
 * command must not be forgotten merely because the next detach observes an
 * already-gone disk.  This controller-wide ledger is deliberately separate
 * from the per-command lifecycle above.
 */
struct drv_nvme_detach_flush_lifecycle {
	unsigned required;
	unsigned attempt_active;
	unsigned attempts;
	unsigned completed;
	unsigned unavailable;
};

/* Supports the drv nvme detach flush init operation. */
/* Supports the drv nvme detach flush init operation. */
static __inline void
drv_nvme_detach_flush_init(
	struct drv_nvme_detach_flush_lifecycle *lifecycle)
{
	/* Handles the lifecycle availability. */
	if (lifecycle != NULL)
		memset(lifecycle, 0, sizeof(*lifecycle));
}

/* Supports the drv nvme detach flush require operation. */
/* Supports the drv nvme detach flush require operation. */
static __inline int
drv_nvme_detach_flush_require(
	struct drv_nvme_detach_flush_lifecycle *lifecycle)
{
	/* Handles the lifecycle availability. */
	if (lifecycle == NULL || lifecycle->attempt_active)
		return EINVAL;

	/* Handles the lifecycle condition. */
	if (lifecycle->required)
		return EALREADY;
	lifecycle->required = 1;
	lifecycle->completed = 0;
	lifecycle->unavailable = 0;

	/* Reports successful completion. */
	return 0;
}

/* Supports the drv nvme detach flush begin operation. */
/* Supports the drv nvme detach flush begin operation. */
static __inline int
drv_nvme_detach_flush_begin(
	struct drv_nvme_detach_flush_lifecycle *lifecycle,
	int queue_available)
{
	/* Handles the lifecycle availability. */
	if (lifecycle == NULL || (queue_available != 0 && queue_available != 1))
		return EINVAL;

	/* Handles the lifecycle condition. */
	if (!lifecycle->required)
		return EALREADY;

	/* Handles the lifecycle condition. */
	if (lifecycle->attempt_active)
		return EBUSY;

	/* Handles the queue available condition. */
	if (!queue_available) {
		lifecycle->unavailable = 1;

		/* Returns the computed result. */
		return ENXIO;
	}
	lifecycle->attempt_active = 1;
	lifecycle->attempts++;
	lifecycle->unavailable = 0;

	/* Reports successful completion. */
	return 0;
}

/* Supports the drv nvme detach flush finish operation. */
/* Supports the drv nvme detach flush finish operation. */
static __inline int
drv_nvme_detach_flush_finish(
	struct drv_nvme_detach_flush_lifecycle *lifecycle,
	int error)
{
	/* Handles the lifecycle availability. */
	if (lifecycle == NULL || !lifecycle->attempt_active)
		return EINVAL;
	lifecycle->attempt_active = 0;

	/* Checks the operation status. */
	if (error != 0)
		return error;
	lifecycle->required = 0;
	lifecycle->completed = 1;
	lifecycle->unavailable = 0;

	/* Reports successful completion. */
	return 0;
}

/* Supports the drv nvme io lifecycle init operation. */
/* Supports the drv nvme io lifecycle init operation. */
static __inline void
drv_nvme_io_lifecycle_init(
	struct drv_nvme_io_lifecycle *lifecycle)
{
	/* Handles the lifecycle availability. */
	if (lifecycle != NULL)
		memset(lifecycle, 0, sizeof(*lifecycle));
}

/* Supports the drv nvme io lifecycle online operation. */
/* Supports the drv nvme io lifecycle online operation. */
static __inline int
drv_nvme_io_lifecycle_online(
	struct drv_nvme_io_lifecycle *lifecycle)
{
	/* Handles the lifecycle availability. */
	if (lifecycle == NULL || lifecycle->released ||
	    lifecycle->quarantined || lifecycle->queues_online ||
	    lifecycle->bio_owned || lifecycle->command_owned ||
	    lifecycle->payload_dma_active)

		/* Returns the computed result. */
		return EINVAL;
	lifecycle->queues_online = 1;
	lifecycle->accepting = 1;
	lifecycle->faulted = 0;
	lifecycle->controller_quiesced = 0;

	/* Reports successful completion. */
	return 0;
}

/* Supports the drv nvme io lifecycle begin bio operation. */
/* Supports the drv nvme io lifecycle begin bio operation. */
static __inline int
drv_nvme_io_lifecycle_begin_bio(
	struct drv_nvme_io_lifecycle *lifecycle)
{
	/* Handles the lifecycle availability. */
	if (lifecycle == NULL || !lifecycle->queues_online ||
	    !lifecycle->accepting)

		/* Returns the computed result. */
		return ENXIO;

	/* Handles the lifecycle condition. */
	if (lifecycle->bio_owned || lifecycle->command_owned)
		return EBUSY;
	lifecycle->bio_owned = 1;

	/* Reports successful completion. */
	return 0;
}

/* Supports the drv nvme io lifecycle submit operation. */
/* Supports the drv nvme io lifecycle submit operation. */
static __inline int
drv_nvme_io_lifecycle_submit(
	struct drv_nvme_io_lifecycle *lifecycle,
	uint16_t command_id,
	int uses_payload_dma)
{
	/* Handles the lifecycle availability. */
	if (lifecycle == NULL ||
	    (uses_payload_dma != 0 && uses_payload_dma != 1))

		/* Returns the computed result. */
		return EINVAL;

	/* Handles the lifecycle condition. */
	if (!lifecycle->queues_online || !lifecycle->accepting)
		return ENXIO;

	/* Handles the lifecycle condition. */
	if (!lifecycle->bio_owned)
		return EINVAL;

	/* Handles the lifecycle condition. */
	if (lifecycle->command_owned || lifecycle->payload_dma_active)
		return EBUSY;
	lifecycle->command_id = command_id;
	lifecycle->command_owned = 1;
	lifecycle->payload_dma_active = (unsigned)uses_payload_dma;

	/* Reports successful completion. */
	return 0;
}

/* Supports the drv nvme io lifecycle complete command operation. */
/* Supports the drv nvme io lifecycle complete command operation. */
static __inline int
drv_nvme_io_lifecycle_complete_command(
	struct drv_nvme_io_lifecycle *lifecycle,
	uint16_t command_id)
{
	/* Handles the lifecycle availability. */
	if (lifecycle == NULL)
		return EINVAL;

	/* Handles the lifecycle condition. */
	if (!lifecycle->command_owned)
		return ENOENT;

	/* Handles the lifecycle condition. */
	if (lifecycle->command_id != command_id) {
		lifecycle->accepting = 0;
		lifecycle->faulted = 1;

		/* Returns the computed result. */
		return EIO;
	}
	lifecycle->command_owned = 0;
	lifecycle->payload_dma_active = 0;
	lifecycle->command_completion_count++;

	/* Reports successful completion. */
	return 0;
}

/* Supports the drv nvme io lifecycle complete bio operation. */
/* Supports the drv nvme io lifecycle complete bio operation. */
static __inline int
drv_nvme_io_lifecycle_complete_bio(
	struct drv_nvme_io_lifecycle *lifecycle)
{
	/* Handles the lifecycle availability. */
	if (lifecycle == NULL)
		return EINVAL;

	/* Handles the lifecycle condition. */
	if (!lifecycle->bio_owned)
		return EALREADY;

	/*
 * Normal completion waits for the command.  A fault may publish a BIO
	 * error early because hardware DMA is isolated to the bounce buffer. */
	if (lifecycle->command_owned && !lifecycle->faulted)
		return EBUSY;
	lifecycle->bio_owned = 0;
	lifecycle->bio_completion_count++;

	/* Reports successful completion. */
	return 0;
}

/* Supports the drv nvme io lifecycle stop operation. */
/* Supports the drv nvme io lifecycle stop operation. */
static __inline int
drv_nvme_io_lifecycle_stop(
	struct drv_nvme_io_lifecycle *lifecycle)
{
	/* Handles the lifecycle availability. */
	if (lifecycle == NULL || lifecycle->released)
		return EINVAL;
	lifecycle->accepting = 0;

	/* Returns the computed result. */
	return lifecycle->bio_owned || lifecycle->command_owned ? EBUSY : 0;
}

/* Supports the drv nvme io lifecycle fault operation. */
/* Supports the drv nvme io lifecycle fault operation. */
static __inline int
drv_nvme_io_lifecycle_fault(
	struct drv_nvme_io_lifecycle *lifecycle)
{
	/* Handles the lifecycle availability. */
	if (lifecycle == NULL || lifecycle->released)
		return EINVAL;
	lifecycle->accepting = 0;
	lifecycle->faulted = 1;

	/* Reports successful completion. */
	return 0;
}

/* Record this only after CC.EN/CSTS.RDY and bus mastering prove that the controller can no longer access queue or payload DMA. */
/* Supports the drv nvme io lifecycle quiesced operation. */
static __inline int
drv_nvme_io_lifecycle_quiesced(
	struct drv_nvme_io_lifecycle *lifecycle)
{
	/* Handles the lifecycle availability. */
	if (lifecycle == NULL || lifecycle->released || lifecycle->accepting)
		return EINVAL;
	lifecycle->queues_online = 0;
	lifecycle->command_owned = 0;
	lifecycle->payload_dma_active = 0;
	lifecycle->controller_quiesced = 1;

	/* Reports successful completion. */
	return 0;
}

/* Supports the drv nvme io lifecycle quarantine operation. */
/* Supports the drv nvme io lifecycle quarantine operation. */
static __inline int
drv_nvme_io_lifecycle_quarantine(
	struct drv_nvme_io_lifecycle *lifecycle)
{
	/* Handles the lifecycle availability. */
	if (lifecycle == NULL || lifecycle->released)
		return EINVAL;
	lifecycle->accepting = 0;
	lifecycle->faulted = 1;
	lifecycle->quarantined = 1;

	/* Reports successful completion. */
	return 0;
}

/* Quarantine records that an earlier teardown could not prove DMA safety; it must not become an irrevocable resource leak.  A later cleanup may resolve it only after independently proving both controller/bus-master quiescence and IRQ quiescence.  Software BIO ownership must already have retired. */
/* Supports the drv nvme io lifecycle resolve quarantine operation. */
static __inline int
drv_nvme_io_lifecycle_resolve_quarantine(
	struct drv_nvme_io_lifecycle *lifecycle,
	int hardware_quiesced,
	int irq_quiesced)
{
	/* Handles the lifecycle availability. */
	if (lifecycle == NULL ||
	    (hardware_quiesced != 0 && hardware_quiesced != 1) ||
	    (irq_quiesced != 0 && irq_quiesced != 1))

		/* Returns the computed result. */
		return EINVAL;

	/* Handles the lifecycle condition. */
	if (lifecycle->released)
		return EALREADY;

	/* Handles the lifecycle condition. */
	if (!lifecycle->quarantined)
		return lifecycle->controller_quiesced ? EALREADY : EINVAL;

	/* Handles the hardware quiesced condition. */
	if (!hardware_quiesced || !irq_quiesced || lifecycle->accepting ||
	    lifecycle->bio_owned)

		/* Returns the computed result. */
		return EBUSY;
	lifecycle->queues_online = 0;
	lifecycle->command_owned = 0;
	lifecycle->payload_dma_active = 0;
	lifecycle->controller_quiesced = 1;
	lifecycle->quarantined = 0;

	/* Reports successful completion. */
	return 0;
}

/* Supports the drv nvme io lifecycle release operation. */
/* Supports the drv nvme io lifecycle release operation. */
static __inline int
drv_nvme_io_lifecycle_release(
	struct drv_nvme_io_lifecycle *lifecycle)
{
	/* Handles the lifecycle availability. */
	if (lifecycle == NULL)
		return EINVAL;

	/* Handles the lifecycle condition. */
	if (lifecycle->released)
		return EALREADY;

	/* Handles the lifecycle condition. */
	if (!lifecycle->controller_quiesced || lifecycle->queues_online ||
	    lifecycle->accepting || lifecycle->bio_owned ||
	    lifecycle->command_owned || lifecycle->payload_dma_active ||
	    lifecycle->quarantined)

		/* Returns the computed result. */
		return EBUSY;
	lifecycle->released = 1;

	/* Reports successful completion. */
	return 0;
}

#endif
/* End consolidated pci-nvme-io-lifecycle.h. */

/* Begin consolidated pci-nvme-shutdown-lifecycle.h. */
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

/* Supports the drv nvme shutdown lifecycle init operation. */
static __inline void
drv_nvme_shutdown_lifecycle_init(
	struct drv_nvme_shutdown_lifecycle *lifecycle)
{
	/* Handles the lifecycle availability. */
	if (lifecycle != NULL)
		memset(lifecycle, 0, sizeof(*lifecycle));
}

/* Supports the drv nvme shutdown lifecycle record error operation. */
static __inline void
drv_nvme_shutdown_lifecycle_record_error(
	struct drv_nvme_shutdown_lifecycle *lifecycle,
	int error)
{
	/* Checks the operation status. */
	if (error == 0)
		return;

	/* Checks the operation status. */
	if (lifecycle->first_error == 0)
		lifecycle->first_error = error;
	lifecycle->failure_count++;
}

/* Supports the drv nvme shutdown lifecycle run operation. */
static __inline int
drv_nvme_shutdown_lifecycle_run(
	struct drv_nvme_shutdown_lifecycle *lifecycle,
	const struct drv_nvme_shutdown_ops *ops,
	void *context)
{
	int error;

	/* Handles the lifecycle availability. */
	if (lifecycle == NULL || ops == NULL || ops->stop_admission == NULL ||
	    ops->shutdown_normal == NULL || ops->controller_disable == NULL ||
	    ops->bus_master_disable == NULL)

		/* Returns the computed result. */
		return EINVAL;

	/* Handles the lifecycle condition. */
	if (lifecycle->completed)
		return lifecycle->first_error;

	/* Handles the lifecycle condition. */
	if (lifecycle->running)
		return EBUSY;
	lifecycle->running = 1;

	lifecycle->admission_attempted = 1;
	error = ops->stop_admission(context);
	lifecycle->admission_error = error;

	/* Checks the operation status. */
	if (error == 0)
		lifecycle->admission_stopped = 1;
	drv_nvme_shutdown_lifecycle_record_error(lifecycle, error);

	/*
 * SHN/SHST remains worth attempting after an admission timeout, and the
	 * two hard quiescence boundaries remain mandatory after any SHN
	 * failure. */
	lifecycle->shutdown_attempted = 1;
	error = ops->shutdown_normal(context);
	lifecycle->shutdown_error = error;

	/* Checks the operation status. */
	if (error == 0)
		lifecycle->shutdown_completed = 1;
	drv_nvme_shutdown_lifecycle_record_error(lifecycle, error);

	lifecycle->disable_attempted = 1;
	error = ops->controller_disable(context);
	lifecycle->disable_error = error;

	/* Checks the operation status. */
	if (error == 0)
		lifecycle->controller_disabled = 1;
	drv_nvme_shutdown_lifecycle_record_error(lifecycle, error);

	/*
 * Bus-master disable is independent of CC.EN/CSTS.RDY and must always
	 * run. */
	lifecycle->master_attempted = 1;
	error = ops->bus_master_disable(context);
	lifecycle->master_error = error;

	/* Checks the operation status. */
	if (error == 0)
		lifecycle->master_disabled = 1;
	drv_nvme_shutdown_lifecycle_record_error(lifecycle, error);

	/*
 * Either independently verified boundary prevents further controller
	 * DMA. */
	lifecycle->hardware_dma_safe =
		lifecycle->controller_disabled || lifecycle->master_disabled;
	lifecycle->running = 0;
	lifecycle->completed = 1;

	/* Returns the computed result. */
	return lifecycle->first_error;
}

#endif
/* End consolidated pci-nvme-shutdown-lifecycle.h. */

#include <errno.h>
#include <hal/hal.h>
#include <kern/atomic.h>
#include <kern/clock.h>
#include <kern/disk.h>
#include <kern/lock.h>
#include <kern/page.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <kern/waitq.h>
#include <limits.h>
#include <string.h>

#define NVME_ADMIN_QUEUE_REQUESTED_DEPTH 64U
#define NVME_IO_QUEUE_REQUESTED_DEPTH 64U
#define NVME_IO_QUEUE_ID 1U
#define NVME_IO_BOUNCE_SIZE 4096U
#define NVME_IO_MAX_SLOTS (NVME_IO_QUEUE_REQUESTED_DEPTH - 1U)
#define NVME_PCI_COMMAND 0x04U
#define NVME_PCI_COMMAND_ENABLE 0x0007U
#define NVME_PCI_COMMAND_MEMORY 0x0002U
#define NVME_PCI_COMMAND_MASTER 0x0004U
#define NVME_PCI_MSI_CAPABILITY 0x05U
#define NVME_PCI_MSIX_CAPABILITY 0x11U
#define NVME_PCI_MESSAGE_CONTROL 0x02U
#define NVME_PCI_MSI_ENABLE 0x0001U
#define NVME_PCI_MSIX_ENABLE 0x8000U
#define NVME_PCI_MSIX_FUNCTION_MASK 0x4000U

/*
 * Controller enable happens during platform discovery, before the platform
 * enables interrupts.  The clock therefore cannot be the sole timeout
 * source for CSTS.RDY.  This secondary, policy-bounded MMIO-read budget keeps
 * an absent controller from hanging boot while the tick deadline remains the
 * authoritative limit once ticks advance.
 */
#define NVME_READY_SPINS_PER_MS 2000U
#define NVME_READY_SPIN_LIMIT 100000000U
#define NVME_IRQ_DRAIN_SPINS 1000000U
#define NVME_TIMEOUT_POLICY_MAX_MS 10000U
#define NVME_CC_SHN_NORMAL 0x00004000U
#define NVME_CSTS_SHST_MASK 0x0000000cU
#define NVME_CSTS_SHST_COMPLETE 0x00000008U

enum nvme_io_slot_state {
	NVME_IO_SLOT_FREE,
	NVME_IO_SLOT_ACTIVE,
	NVME_IO_SLOT_CQ_DONE,
	NVME_IO_SLOT_FAULTED,
	NVME_IO_SLOT_COPYING
};

struct nvme_io_slot {
	struct drv_nvme_io_lifecycle lifecycle;
	struct drv_dma_buffer bounce_dma;
	struct wait_queue waitq;
	uint16_t command_id;
	uint8_t state;
	unsigned posted;
	uint32_t epoch;
	int error;
};

struct nvme_controller {
	struct drv_nvme_lifecycle lifecycle;
	struct drv_nvme_detach_flush_lifecycle detach_flush;
	struct drv_nvme_shutdown_lifecycle shutdown_lifecycle;
	struct drv_pci_device *pci;
	struct drv_pci_mapping mapping;
	struct drv_pci_bar original_bar;
	struct drv_pci_enable_state pci_enable_state;
	unsigned inherited_msi_capability;
	unsigned inherited_msix_capability;
	uint16_t inherited_msi_control;
	uint16_t inherited_msix_control;
	unsigned inherited_msi_saved;
	unsigned inherited_msix_saved;
	volatile uint8_t *registers;
	uint64_t capability;
	uint32_t version;
	uint32_t page_size;
	unsigned queue_depth;
	unsigned timeout_ms;
	uint64_t timeout_ticks;
	size_t submission_doorbell;
	size_t completion_doorbell;

	struct drv_dma_buffer admin_submission_dma;
	struct drv_dma_buffer admin_completion_dma;
	struct drv_dma_buffer identify_dma;
	volatile struct drv_nvme_command *admin_submission;
	volatile struct drv_nvme_completion *admin_completion;
	unsigned submission_tail;
	struct drv_nvme_completion_cursor completion_cursor;
	uint16_t next_command_id;

	struct drv_dma_buffer io_submission_dma;
	struct drv_dma_buffer io_completion_dma;
	volatile struct drv_nvme_command *io_submission;
	volatile struct drv_nvme_completion *io_completion;
	struct nvme_io_slot io_slots[NVME_IO_MAX_SLOTS];
	unsigned io_slot_count;
	unsigned io_queue_depth;
	unsigned io_submission_tail;
	struct drv_nvme_completion_cursor io_completion_cursor;
	uint16_t next_io_command_id;
	uint32_t io_epoch;
	size_t io_submission_doorbell;
	size_t io_completion_doorbell;
	uint64_t namespace_blocks;
	uint32_t namespace_block_size;
	size_t maximum_transfer_bytes;

	struct spinlock command_lock;
	struct wait_queue io_state_waitq;
	unsigned command_pending;
	unsigned command_completed;
	uint16_t expected_command_id;
	int command_error;
	uint32_t command_result;
	int admin_fault;
	unsigned io_pending;
	unsigned io_owned;
	unsigned io_calls;
	unsigned io_data_active;
	unsigned io_flush_waiting;
	unsigned io_flush_active;
	unsigned io_queue_ready;
	unsigned io_recovery_needed;
	unsigned io_recovery_busy;
	int io_fault;

	struct drv_pci_irq irq;
	void *irq_cookie;
	volatile unsigned irq_busy;
	struct disk *namespace_disk;
	uint32_t namespace_id;

	unsigned bar_claimed;
	unsigned bar_mapped;
	unsigned original_bar_valid;
	unsigned pci_state_saved;
	unsigned restore_allowed;
	unsigned dma_allocated;
	unsigned irq_allocated;
	unsigned controller_owned;
	unsigned controller_enabled;
	unsigned probe_started;
	unsigned probe_busy;
	unsigned detach_busy;
	unsigned stopping;
	unsigned quarantined;
};

static int nvme_controller_enable(struct nvme_controller *controller);
static int nvme_io_dma_allocate(struct nvme_controller *controller);
static int nvme_io_queue_create(struct nvme_controller *controller, int recovery_command);
static int nvme_io_recover(struct nvme_controller *controller);
static int nvme_io_lifecycles_quiesce(struct nvme_controller *controller);
static void nvme_io_fail_all_locked(struct nvme_controller *controller, int error);

static struct nvme_controller *nvme_primary;
static struct spinlock nvme_registry_lock;

static uint32_t nvme_read32(const struct nvme_controller *controller, size_t offset);

/* Supports the nvme read32 operation. */
static uint32_t
nvme_read32(
	const struct nvme_controller *controller,
	size_t offset)
{
	/* Returns the computed result. */
	return *(volatile uint32_t *)(controller->registers + offset);
}

static uint64_t nvme_read64(const struct nvme_controller *controller, size_t offset);

/* Supports the nvme read64 operation. */
static uint64_t
nvme_read64(
	const struct nvme_controller *controller,
	size_t offset)
{
	uint32_t low = nvme_read32(controller, offset);
	uint32_t high = nvme_read32(controller, offset + sizeof(uint32_t));

	/* Returns the computed result. */
	return (uint64_t)low | ((uint64_t)high << 32);
}

static void nvme_write32(struct nvme_controller *controller, size_t offset, uint32_t value);

/* Supports the nvme write32 operation. */
static void
nvme_write32(
	struct nvme_controller *controller,
	size_t offset,
	uint32_t value)
{
	*(volatile uint32_t *)(controller->registers + offset) = value;

	hal_io_mb();
}

static void nvme_write64(struct nvme_controller *controller, size_t offset, uint64_t value);

/* Supports the nvme write64 operation. */
static void
nvme_write64(
	struct nvme_controller *controller,
	size_t offset,
	uint64_t value)
{
	/* Queue address registers are programmed only while CC.EN is clear. */
	*(volatile uint32_t *)(controller->registers + offset) =
		(uint32_t)value;
	*(volatile uint32_t *)(controller->registers + offset + 4U) =
		(uint32_t)(value >> 32);

	hal_io_mb();
}

static uint64_t nvme_timeout_ticks(unsigned milliseconds);

/* Supports the nvme timeout ticks operation. */
static uint64_t
nvme_timeout_ticks(
	unsigned milliseconds)
{
	uint64_t ticks =
		((uint64_t)milliseconds * KERN_CLOCK_HZ + 999U) / 1000U;

	/* Returns the computed result. */
	return ticks == 0U ? 1U : ticks;
}

static int nvme_wait_ready(struct nvme_controller *controller, int expected_ready);

/* Supports the nvme wait ready operation. */
static int
nvme_wait_ready(
	struct nvme_controller *controller,
	int expected_ready)
{
	uint32_t status;
	enum drv_nvme_ready_state state;
	uint64_t now;
	uint64_t started = clock_ticks();
	uint64_t deadline_ticks = controller->timeout_ticks;
	uint64_t spin_budget;
	uint64_t spin = 0;
	int clock_running = 0;

	/* Handles the controller condition. */
	if (controller->timeout_ms >
	    NVME_READY_SPIN_LIMIT / NVME_READY_SPINS_PER_MS)
		spin_budget = NVME_READY_SPIN_LIMIT;
	else
		spin_budget = (uint64_t)controller->timeout_ms *
			      NVME_READY_SPINS_PER_MS;

	/* Handles the spin budget condition. */
	if (spin_budget == 0U)
		spin_budget = 1U;
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		status = nvme_read32(controller, DRV_NVME_REG_CSTS);
		state = expected_ready
				? drv_nvme_controller_ready_state(status, 1)
				: drv_nvme_controller_disable_state(status);

		/* Handles the state condition. */
		if (state == DRV_NVME_READY_MATCH)
			return 0;

		/* Handles the state condition. */
		if (state == DRV_NVME_READY_FATAL ||
		    state == DRV_NVME_READY_UNREACHABLE)

			/* Returns the computed result. */
			return EIO;
		now = clock_ticks();

		/* Handles the now condition. */
		if (now != started)
			clock_running = 1;

		/* Handles the clock running condition. */
		if (clock_running) {
			/* Handles the now condition. */
			if (now - started >= deadline_ticks)
				return ETIMEDOUT;
		} else if (++spin >= spin_budget) {
			/* Early attach can precede the first timer tick. */
			return ETIMEDOUT;
		}
		hal_compiler_barrier();
	}
}

static int nvme_wait_shutdown_complete(struct nvme_controller *controller);

/* Supports the nvme wait shutdown complete operation. */
static int
nvme_wait_shutdown_complete(
	struct nvme_controller *controller)
{
	uint32_t status;
	uint64_t now;
	uint64_t started = clock_ticks();
	uint64_t spin_budget;
	uint64_t spin = 0;
	int clock_running = 0;

	/* Handles the controller condition. */
	if (controller->timeout_ms >
	    NVME_READY_SPIN_LIMIT / NVME_READY_SPINS_PER_MS)
		spin_budget = NVME_READY_SPIN_LIMIT;
	else
		spin_budget = (uint64_t)controller->timeout_ms *
			      NVME_READY_SPINS_PER_MS;

	/* Handles the spin budget condition. */
	if (spin_budget == 0U)
		spin_budget = 1U;
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		status = nvme_read32(controller, DRV_NVME_REG_CSTS);

		/* Checks the operation status. */
		if (status == UINT32_MAX ||
		    (status & DRV_NVME_CSTS_FATAL) != 0U)

			/* Returns the computed result. */
			return EIO;

		/* Checks the operation status. */
		if ((status & NVME_CSTS_SHST_MASK) == NVME_CSTS_SHST_COMPLETE)
			return 0;
		now = clock_ticks();

		/* Handles the now condition. */
		if (now != started)
			clock_running = 1;

		/* Handles the clock running condition. */
		if (clock_running) {
			/* Handles the now condition. */
			if (now - started >= controller->timeout_ticks)
				return ETIMEDOUT;
		} else if (++spin >= spin_budget) {
			/* Returns the computed result. */
			return ETIMEDOUT;
		}
		hal_compiler_barrier();
	}
}

static int nvme_bus_master_disable(struct nvme_controller *controller);

/* Supports the nvme bus master disable operation. */
static int
nvme_bus_master_disable(
	struct nvme_controller *controller)
{
	uint16_t command;
	int error;

	error = drv_pci_device_set_bus_master(controller->pci, false);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = drv_pci_device_config_read16(controller->pci, NVME_PCI_COMMAND,
					     &command);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Returns the computed result. */
	return (command & NVME_PCI_COMMAND_MASTER) == 0U ? 0 : EIO;
}

static int nvme_pci_quiesce(struct nvme_controller *controller);

/* Supports the nvme pci quiesce operation. */
static int
nvme_pci_quiesce(
	struct nvme_controller *controller)
{
	uint16_t command;
	int error;

	error = drv_pci_device_config_read16(controller->pci, NVME_PCI_COMMAND,
					     &command);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = drv_pci_device_config_write16(
		controller->pci, NVME_PCI_COMMAND,
		(uint16_t)(command & (uint16_t)~NVME_PCI_COMMAND_ENABLE));

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = drv_pci_device_config_read16(controller->pci, NVME_PCI_COMMAND,
					     &command);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Returns the computed result. */
	return (command & NVME_PCI_COMMAND_ENABLE) == 0U ? 0 : EIO;
}

static int nvme_message_irq_mask(struct nvme_controller *controller);

/* Supports the nvme message irq mask operation. */
static int
nvme_message_irq_mask(
	struct nvme_controller *controller)
{
	unsigned capability;
	uint16_t control, readback;
	int error;

	error = drv_pci_device_find_capability(
		controller->pci, NVME_PCI_MSI_CAPABILITY, &capability);

	/* Checks the operation status. */
	if (error == 0) {
		error = drv_pci_device_config_read16(
			controller->pci, capability + NVME_PCI_MESSAGE_CONTROL,
			&control);

		/* Checks the operation status. */
		if (error != 0)
			return error;
		control &= (uint16_t)~NVME_PCI_MSI_ENABLE;
		error = drv_pci_device_config_write16(
			controller->pci, capability + NVME_PCI_MESSAGE_CONTROL,
			control);

		/* Checks the operation status. */
		if (error != 0 ||
		    drv_pci_device_config_read16(
			    controller->pci,
			    capability + NVME_PCI_MESSAGE_CONTROL,
			    &readback) != 0 ||
		    (readback & NVME_PCI_MSI_ENABLE) != 0U)

			/* Returns the computed result. */
			return error != 0 ? error : EIO;
	} else if (error != ENOENT) {
		/* Returns the computed result. */
		return error;
	}
	error = drv_pci_device_find_capability(
		controller->pci, NVME_PCI_MSIX_CAPABILITY, &capability);

	/* Checks the operation status. */
	if (error == 0) {
		error = drv_pci_device_config_read16(
			controller->pci, capability + NVME_PCI_MESSAGE_CONTROL,
			&control);

		/* Checks the operation status. */
		if (error != 0)
			return error;
		control =
			(uint16_t)((control & (uint16_t)~NVME_PCI_MSIX_ENABLE) |
				   NVME_PCI_MSIX_FUNCTION_MASK);
		error = drv_pci_device_config_write16(
			controller->pci, capability + NVME_PCI_MESSAGE_CONTROL,
			control);

		/* Checks the operation status. */
		if (error != 0 ||
		    drv_pci_device_config_read16(
			    controller->pci,
			    capability + NVME_PCI_MESSAGE_CONTROL,
			    &readback) != 0 ||
		    (readback &
		     (NVME_PCI_MSIX_ENABLE | NVME_PCI_MSIX_FUNCTION_MASK)) !=
			    NVME_PCI_MSIX_FUNCTION_MASK)

			/* Returns the computed result. */
			return error != 0 ? error : EIO;
	} else if (error != ENOENT) {
		/* Returns the computed result. */
		return error;
	}

	/* Reports successful completion. */
	return 0;
}

static int nvme_message_irq_save_and_mask(struct nvme_controller *controller);

/* Supports the nvme message irq save and mask operation. */
static int
nvme_message_irq_save_and_mask(
	struct nvme_controller *controller)
{
	int function_result;
	unsigned capability;
	int error;

	error = drv_pci_device_find_capability(
		controller->pci, NVME_PCI_MSI_CAPABILITY, &capability);

	/* Checks the operation status. */
	if (error == 0) {
		error = drv_pci_device_config_read16(
			controller->pci, capability + NVME_PCI_MESSAGE_CONTROL,
			&controller->inherited_msi_control);

		/* Checks the operation status. */
		if (error != 0)
			return error;
		controller->inherited_msi_capability = capability;
		controller->inherited_msi_saved = 1;
	} else if (error != ENOENT) {
		/* Returns the computed result. */
		return error;
	}
	error = drv_pci_device_find_capability(
		controller->pci, NVME_PCI_MSIX_CAPABILITY, &capability);

	/* Checks the operation status. */
	if (error == 0) {
		error = drv_pci_device_config_read16(
			controller->pci, capability + NVME_PCI_MESSAGE_CONTROL,
			&controller->inherited_msix_control);

		/* Checks the operation status. */
		if (error != 0)
			return error;
		controller->inherited_msix_capability = capability;
		controller->inherited_msix_saved = 1;
	} else if (error != ENOENT) {
		/* Returns the computed result. */
		return error;
	}

	/* Obtains the nvme message irq mask result. */
	function_result = nvme_message_irq_mask(controller);

	/* Returns the computed result. */
	return function_result;
}

static int nvme_message_irq_restore(struct nvme_controller *controller);

/* Supports the nvme message irq restore operation. */
static int
nvme_message_irq_restore(
	struct nvme_controller *controller)
{
	uint16_t readback;
	int error;

	/* Handles the controller condition. */
	if (controller->inherited_msi_saved) {
		error = drv_pci_device_config_write16(
			controller->pci,
			controller->inherited_msi_capability +
				NVME_PCI_MESSAGE_CONTROL,
			controller->inherited_msi_control);

		/* Checks the operation status. */
		if (error != 0 ||
		    drv_pci_device_config_read16(
			    controller->pci,
			    controller->inherited_msi_capability +
				    NVME_PCI_MESSAGE_CONTROL,
			    &readback) != 0 ||
		    (readback & NVME_PCI_MSI_ENABLE) !=
			    (controller->inherited_msi_control &
			     NVME_PCI_MSI_ENABLE))

			/* Returns the computed result. */
			return error != 0 ? error : EIO;
	}

	/* Handles the controller condition. */
	if (controller->inherited_msix_saved) {
		error = drv_pci_device_config_write16(
			controller->pci,
			controller->inherited_msix_capability +
				NVME_PCI_MESSAGE_CONTROL,
			controller->inherited_msix_control);

		/* Checks the operation status. */
		if (error != 0 ||
		    drv_pci_device_config_read16(
			    controller->pci,
			    controller->inherited_msix_capability +
				    NVME_PCI_MESSAGE_CONTROL,
			    &readback) != 0 ||
		    (readback &
		     (NVME_PCI_MSIX_ENABLE | NVME_PCI_MSIX_FUNCTION_MASK)) !=
			    (controller->inherited_msix_control &
			     (NVME_PCI_MSIX_ENABLE |
			      NVME_PCI_MSIX_FUNCTION_MASK)))

			/* Returns the computed result. */
			return error != 0 ? error : EIO;
	}

	/* Reports successful completion. */
	return 0;
}

static int nvme_runtime_irq_mask(struct nvme_controller *controller, unsigned *capability_out, uint16_t *control_out);

/* Supports the nvme runtime irq mask operation. */
static int
nvme_runtime_irq_mask(
	struct nvme_controller *controller,
	unsigned *capability_out,
	uint16_t *control_out)
{
	unsigned capability;
	uint16_t control, masked, readback;
	unsigned capability_id;
	int error;

	/* Handles the capability out availability. */
	if (capability_out == NULL || control_out == NULL)
		return EINVAL;
	capability_id = controller->irq.type == DRV_PCI_IRQ_MSIX
				? NVME_PCI_MSIX_CAPABILITY
				: NVME_PCI_MSI_CAPABILITY;
	error = drv_pci_device_find_capability(controller->pci, capability_id,
					       &capability);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = drv_pci_device_config_read16(
		controller->pci, capability + NVME_PCI_MESSAGE_CONTROL,
		&control);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the controller condition. */
	if (controller->irq.type == DRV_PCI_IRQ_MSIX)
		masked = control | NVME_PCI_MSIX_FUNCTION_MASK;
	else if (controller->irq.type == DRV_PCI_IRQ_MSI)
		masked = control & (uint16_t)~NVME_PCI_MSI_ENABLE;
	else

		/* Returns the computed result. */
		return EOPNOTSUPP;
	error = drv_pci_device_config_write16(
		controller->pci, capability + NVME_PCI_MESSAGE_CONTROL, masked);

	/* Checks the operation status. */
	if (error != 0 ||
	    drv_pci_device_config_read16(controller->pci,
					 capability + NVME_PCI_MESSAGE_CONTROL,
					 &readback) != 0)

		/* Returns the computed result. */
		return error != 0 ? error : EIO;

	/* Handles the controller condition. */
	if (controller->irq.type == DRV_PCI_IRQ_MSIX) {
		/* Handles the readback condition. */
		if ((readback & NVME_PCI_MSIX_FUNCTION_MASK) == 0U)
			return EIO;
	} else if ((readback & NVME_PCI_MSI_ENABLE) != 0U) {
		/* Returns the computed result. */
		return EIO;
	}
	*capability_out = capability;
	*control_out = control;
	/* Reports successful completion. */
	return 0;
}

static int nvme_runtime_irq_restore(struct nvme_controller *controller, unsigned capability, uint16_t control);

/* Supports the nvme runtime irq restore operation. */
static int
nvme_runtime_irq_restore(
	struct nvme_controller *controller,
	unsigned capability,
	uint16_t control)
{
	uint16_t readback;
	int error;

	error = drv_pci_device_config_write16(
		controller->pci, capability + NVME_PCI_MESSAGE_CONTROL,
		control);

	/* Checks the operation status. */
	if (error != 0 ||
	    drv_pci_device_config_read16(controller->pci,
					 capability + NVME_PCI_MESSAGE_CONTROL,
					 &readback) != 0)

		/* Returns the computed result. */
		return error != 0 ? error : EIO;

	/* Handles the controller condition. */
	if (controller->irq.type == DRV_PCI_IRQ_MSIX) {
		return (readback &
			(NVME_PCI_MSIX_ENABLE | NVME_PCI_MSIX_FUNCTION_MASK)) ==
				       (control & (NVME_PCI_MSIX_ENABLE |
						   NVME_PCI_MSIX_FUNCTION_MASK))
			       ? 0
			       : EIO;
	}

	/* Returns the computed result. */
	return (readback & NVME_PCI_MSI_ENABLE) ==
			       (control & NVME_PCI_MSI_ENABLE)
		       ? 0
		       : EIO;
}

static int nvme_controller_disable(struct nvme_controller *controller);

/* Supports the nvme controller disable operation. */
static int
nvme_controller_disable(
	struct nvme_controller *controller)
{
	uint32_t configuration;
	int error;

	/*
	 * INTMS/INTMC do not mask MSI-X vectors.  Once MSI-X is enabled the
	 * controller specification makes writes to those registers undefined;
	 * the PCI core masks the MSI-X table entry during teardown instead.
	 */
	if (!controller->irq_allocated ||
	    controller->irq.type != DRV_PCI_IRQ_MSIX)
		nvme_write32(controller, DRV_NVME_REG_INTMS, UINT32_MAX);
	configuration = nvme_read32(controller, DRV_NVME_REG_CC);

	/* Handles the configuration condition. */
	if (configuration == UINT32_MAX)
		return EIO;

	/* Handles the configuration condition. */
	if ((configuration & DRV_NVME_CC_ENABLE) != 0U) {
		configuration &= ~(DRV_NVME_CC_ENABLE | DRV_NVME_CC_SHN_MASK);
		nvme_write32(controller, DRV_NVME_REG_CC, configuration);
		error = nvme_wait_ready(controller, 0);

		/* Checks the operation status. */
		if (error != 0)
			return error;
	} else {
		error = nvme_wait_ready(controller, 0);

		/* Checks the operation status. */
		if (error != 0)
			return error;
	}
	controller->controller_enabled = 0;

	/* Reports successful completion. */
	return 0;
}

static int nvme_controller_shutdown_normal(struct nvme_controller *controller);

/* Supports the nvme controller shutdown normal operation. */
static int
nvme_controller_shutdown_normal(
	struct nvme_controller *controller)
{
	int function_result;
	uint32_t configuration;

	/* Handles the controller condition. */
	if (!controller->controller_enabled)
		return 0;
	configuration = nvme_read32(controller, DRV_NVME_REG_CC);

	/* Handles the configuration condition. */
	if (configuration == UINT32_MAX ||
	    (configuration & DRV_NVME_CC_ENABLE) == 0U)

		/* Returns the computed result. */
		return EIO;
	configuration &= ~DRV_NVME_CC_SHN_MASK;
	configuration |= NVME_CC_SHN_NORMAL;
	nvme_write32(controller, DRV_NVME_REG_CC, configuration);

	/* Obtains the nvme wait shutdown complete result. */
	function_result = nvme_wait_shutdown_complete(controller);

	/* Returns the computed result. */
	return function_result;
}

static int nvme_stop_admission(struct nvme_controller *controller);

/* Supports the nvme stop admission operation. */
static int
nvme_stop_admission(
	struct nvme_controller *controller)
{
	unsigned slot;
	uint64_t deadline = clock_ticks() + (controller->timeout_ticks != 0U
						     ? controller->timeout_ticks
						     : 1U);
	unsigned long irq;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		irq = spin_lock_irqsave(&controller->command_lock);
		controller->stopping = 1;
		waitq_wake_all(&controller->io_state_waitq);
		/* Process each remaining element. */
		for (slot = 0; slot < controller->io_slot_count; slot++) {
			(void)drv_nvme_io_lifecycle_stop(
				&controller->io_slots[slot].lifecycle);
			waitq_wake_all(&controller->io_slots[slot].waitq);
		}

		/* Handles the controller condition. */
		if (!controller->command_pending && !controller->probe_busy &&
		    !controller->io_calls && !controller->io_pending &&
		    !controller->io_owned && !controller->io_recovery_busy) {
			spin_unlock_irqrestore(&controller->command_lock, irq);

			/* Reports successful completion. */
			return 0;
		}
		spin_unlock_irqrestore(&controller->command_lock, irq);

		/* Checks the clock ticks result. */
		if (clock_ticks() >= deadline)
			return EBUSY;
		sched_yield();
	}
}

static void nvme_detach_release(struct nvme_controller *controller, int resume);

/* Supports the nvme detach release operation. */
static void
nvme_detach_release(
	struct nvme_controller *controller,
	int resume)
{
	unsigned long irq = spin_lock_irqsave(&controller->command_lock);

	controller->detach_busy = 0;

	/* Handles the resume condition. */
	if (resume && !controller->quarantined &&
	    controller->controller_enabled)
		controller->stopping = 0;
	waitq_wake_all(&controller->io_state_waitq);
	spin_unlock_irqrestore(&controller->command_lock, irq);
}

static int nvme_detach_claim(struct drv_pci_device *device, struct nvme_controller **result);

/* Claim the only detach transaction before waiting for an admitted namespace probe.  The probe failure path can claim the same transaction while it still owns probe_busy, so no second caller can free the controller between clearing probe_busy and starting teardown. */
static int
nvme_detach_claim(
	struct drv_pci_device *device,
	struct nvme_controller **result)
{
	struct nvme_controller *controller;
	uint64_t deadline;
	unsigned long irq, registry_irq;

	/* Handles the device availability. */
	if (device == NULL || result == NULL)
		return EINVAL;
	registry_irq = spin_lock_irqsave(&nvme_registry_lock);
	controller = nvme_primary;

	/* Handles the controller availability. */
	if (controller == NULL || controller->pci != device) {
		spin_unlock_irqrestore(&nvme_registry_lock, registry_irq);

		/* Returns the computed result. */
		return EBUSY;
	}
	irq = spin_lock_irqsave(&controller->command_lock);

	/* Handles the controller condition. */
	if (controller->detach_busy) {
		spin_unlock_irqrestore(&controller->command_lock, irq);
		spin_unlock_irqrestore(&nvme_registry_lock, registry_irq);

		/* Returns the computed result. */
		return EBUSY;
	}
	controller->detach_busy = 1;
	spin_unlock_irqrestore(&controller->command_lock, irq);
	spin_unlock_irqrestore(&nvme_registry_lock, registry_irq);
	deadline = clock_ticks() + (controller->timeout_ticks != 0U
					    ? controller->timeout_ticks
					    : 1U);

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		irq = spin_lock_irqsave(&controller->command_lock);

		/* Handles the controller condition. */
		if (!controller->command_pending && !controller->probe_busy) {
			spin_unlock_irqrestore(&controller->command_lock, irq);
			*result = controller;
			/* Reports successful completion. */
			return 0;
		}
		spin_unlock_irqrestore(&controller->command_lock, irq);

		/* Checks the clock ticks result. */
		if (clock_ticks() >= deadline) {
			nvme_detach_release(controller, 1);

			/* Returns the computed result. */
			return EBUSY;
		}
		sched_yield();
	}
}

static int nvme_shutdown_claim(struct drv_pci_device *device, struct nvme_controller **result);

/* Terminal shutdown needs the same lifetime exclusion as detach, but it must not wait for an admin command or namespace probe before attempting the two independent hardware DMA-stop boundaries.  A lost completion is precisely the case in which that wait would otherwise bypass controller and PCI quiescence.  The shutdown lifecycle records the bounded admission failure; this claim only prevents another owner from freeing the controller. */
static int
nvme_shutdown_claim(
	struct drv_pci_device *device,
	struct nvme_controller **result)
{
	struct nvme_controller *controller;
	unsigned long irq, registry_irq;

	/* Handles the device availability. */
	if (device == NULL || result == NULL)
		return EINVAL;
	registry_irq = spin_lock_irqsave(&nvme_registry_lock);
	controller = nvme_primary;

	/* Handles the controller availability. */
	if (controller == NULL || controller->pci != device) {
		spin_unlock_irqrestore(&nvme_registry_lock, registry_irq);

		/* Returns the computed result. */
		return EBUSY;
	}
	irq = spin_lock_irqsave(&controller->command_lock);

	/* Handles the controller condition. */
	if (controller->detach_busy) {
		spin_unlock_irqrestore(&controller->command_lock, irq);
		spin_unlock_irqrestore(&nvme_registry_lock, registry_irq);

		/* Returns the computed result. */
		return EBUSY;
	}
	controller->detach_busy = 1;
	controller->stopping = 1;
	waitq_wake_all(&controller->io_state_waitq);
	spin_unlock_irqrestore(&controller->command_lock, irq);
	spin_unlock_irqrestore(&nvme_registry_lock, registry_irq);
	*result = controller;
	/* Reports successful completion. */
	return 0;
}

static int nvme_shutdown_stop_admission(void *context);

/* Supports the nvme shutdown stop admission operation. */
static int
nvme_shutdown_stop_admission(
	void *context)
{
	unsigned long irq;
	struct nvme_controller *controller = context;
	int error = nvme_stop_admission(controller);

	/* Checks the operation status. */
	if (error != 0) {
		irq = spin_lock_irqsave(&controller->command_lock);

		/*
 * Terminal shutdown cannot wait forever for a lost completion.
		 * Fail the software owners, then continue to the two
		 * independent hardware DMA-stop boundaries below. */
		nvme_io_fail_all_locked(controller, error);
		spin_unlock_irqrestore(&controller->command_lock, irq);
	}

	/* Returns the computed result. */
	return error;
}

static int nvme_shutdown_normal(void *context);

/* Supports the nvme shutdown normal operation. */
static int
nvme_shutdown_normal(
	void *context)
{
	int function_result;
	struct nvme_controller *controller = context;

	/* Handles the controller condition. */
	if (!controller->lifecycle.controller_claimed ||
	    controller->lifecycle.controller_disabled)

		/* Reports successful completion. */
		return 0;

	/* Handles the registers availability. */
	if (!controller->lifecycle.bar_mapped || !controller->bar_mapped ||
	    controller->registers == NULL)

		/* Returns the computed result. */
		return EIO;

	/* Obtains the nvme controller shutdown normal result. */
	function_result = nvme_controller_shutdown_normal(controller);

	/* Returns the computed result. */
	return function_result;
}

static int nvme_shutdown_controller_disable(void *context);

/* Supports the nvme shutdown controller disable operation. */
static int
nvme_shutdown_controller_disable(
	void *context)
{
	struct nvme_controller *controller = context;
	int error;

	/* Handles the controller condition. */
	if (!controller->lifecycle.controller_claimed ||
	    controller->lifecycle.controller_disabled)

		/* Reports successful completion. */
		return 0;

	/* Handles the registers availability. */
	if (!controller->lifecycle.bar_mapped || !controller->bar_mapped ||
	    controller->registers == NULL)

		/* Returns the computed result. */
		return EIO;
	error = nvme_controller_disable(controller);

	/* Checks the operation status. */
	if (error == 0) {
		controller->lifecycle.controller_disabled = 1;
		controller->lifecycle.controller_enabled = 0;
	}

	/* Returns the computed result. */
	return error;
}

static int nvme_shutdown_bus_master_disable(void *context);

/* Supports the nvme shutdown bus master disable operation. */
static int
nvme_shutdown_bus_master_disable(
	void *context)
{
	struct nvme_controller *controller = context;
	int error;

	/* Handles the controller condition. */
	if (!controller->lifecycle.master_disable_required ||
	    controller->lifecycle.master_disabled)

		/* Reports successful completion. */
		return 0;
	error = nvme_bus_master_disable(controller);

	/* Checks the operation status. */
	if (error == 0)
		controller->lifecycle.master_disabled = 1;

	/* Returns the computed result. */
	return error;
}

static const struct drv_nvme_shutdown_ops nvme_shutdown_operations = {
	.stop_admission = nvme_shutdown_stop_admission,
	.shutdown_normal = nvme_shutdown_normal,
	.controller_disable = nvme_shutdown_controller_disable,
	.bus_master_disable = nvme_shutdown_bus_master_disable,
};

static int nvme_irq_remove(struct nvme_controller *controller);

/* Supports the nvme irq remove operation. */
static int
nvme_irq_remove(
	struct nvme_controller *controller)
{
	unsigned attempt;
	int error = 0;

	/* Handles the irq cookie availability. */
	if (controller->irq_cookie != NULL) {
		/* Process each element required by the operation. */
		for (attempt = 0; attempt < NVME_IRQ_DRAIN_SPINS; attempt++) {
			error = drv_pci_device_disestablish_irq_checked(
				controller->pci, controller->irq_cookie);

			/* Checks the operation status. */
			if (error != EBUSY)
				break;
			hal_compiler_barrier();
		}

		/* Checks the operation status. */
		if (error != 0) {
			hal_printf("nvme: IRQ removal failed (%d); retaining "
				   "controller resources\n",
				   error);

			/* Returns the computed result. */
			return error;
		}
		controller->irq_cookie = NULL;
	}

	/* Reports successful completion. */
	return 0;
}

static int nvme_irq_drain(struct nvme_controller *controller);

/* Supports the nvme irq drain operation. */
static int
nvme_irq_drain(
	struct nvme_controller *controller)
{
	unsigned attempt;

	/* Process each element required by the operation. */
	for (attempt = 0; attempt < NVME_IRQ_DRAIN_SPINS; attempt++) {
		/* Checks the atomic raw load acquire result. */
		if (atomic_raw_load_acquire(&controller->irq_busy) == 0U)
			return 0;
		hal_compiler_barrier();
	}
	hal_printf(
		"nvme: IRQ drain timed out; retaining controller resources\n");

	/* Returns the computed result. */
	return EBUSY;
}

static void nvme_dma_free(struct nvme_controller *controller);

/* Supports the nvme dma free operation. */
static void
nvme_dma_free(
	struct nvme_controller *controller)
{
	struct drv_dma_device *dma = drv_pci_device_dma(controller->pci);
	unsigned slot;

	/* Process each element required by the operation. */
	for (slot = 0; slot < NVME_IO_MAX_SLOTS; slot++) {
		/* Handles the slot condition. */
		if (slot < controller->io_slot_count) {
			/* Checks the drv nvme io lifecycle release result. */
			if (!controller->io_slots[slot]
				     .lifecycle.controller_quiesced ||
			    controller->io_slots[slot].lifecycle.quarantined ||
			    drv_nvme_io_lifecycle_release(
				    &controller->io_slots[slot].lifecycle) != 0)
				__builtin_trap();
		}

		/* Handles the address availability. */
		if (controller->io_slots[slot].bounce_dma.address != NULL) {
			drv_dma_free_coherent(
				dma, &controller->io_slots[slot].bounce_dma);
		}
	}

	/* Handles the address availability. */
	if (controller->io_completion_dma.address != NULL)
		drv_dma_free_coherent(dma, &controller->io_completion_dma);

	/* Handles the address availability. */
	if (controller->io_submission_dma.address != NULL)
		drv_dma_free_coherent(dma, &controller->io_submission_dma);

	/* Handles the address availability. */
	if (controller->identify_dma.address != NULL)
		drv_dma_free_coherent(dma, &controller->identify_dma);

	/* Handles the address availability. */
	if (controller->admin_completion_dma.address != NULL)
		drv_dma_free_coherent(dma, &controller->admin_completion_dma);

	/* Handles the address availability. */
	if (controller->admin_submission_dma.address != NULL)
		drv_dma_free_coherent(dma, &controller->admin_submission_dma);
	controller->admin_submission = NULL;
	controller->admin_completion = NULL;
	controller->io_submission = NULL;
	controller->io_completion = NULL;
	controller->io_slot_count = 0;
	controller->io_queue_ready = 0;
	controller->dma_allocated = 0;
}

static int nvme_restore_bar(struct nvme_controller *controller);

/* Supports the nvme restore bar operation. */
static int
nvme_restore_bar(
	struct nvme_controller *controller)
{
	int function_result;
	struct drv_pci_bar current;
	int error;

	/* Handles the controller condition. */
	if (!controller->original_bar_valid)
		return 0;
	error = drv_pci_device_bar(controller->pci, 0, &current);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the current condition. */
	if (current.bus_address == controller->original_bar.bus_address)
		return 0;

	/* Obtains the drv pci device assign bar result. */
	function_result = drv_pci_device_assign_bar(
		controller->pci, 0, controller->original_bar.bus_address);

	/* Returns the computed result. */
	return function_result;
}

static int nvme_lifecycle_controller_disable(void *context);

/* Supports the nvme lifecycle controller disable operation. */
static int
nvme_lifecycle_controller_disable(
	void *context)
{
	int function_result;

	/* Obtains the nvme controller disable result. */
	function_result = nvme_controller_disable(context);

	/* Returns the computed result. */
	return function_result;
}

static int nvme_lifecycle_master_disable(void *context);

/* Supports the nvme lifecycle master disable operation. */
static int
nvme_lifecycle_master_disable(
	void *context)
{
	int function_result;

	/* Obtains the nvme bus master disable result. */
	function_result = nvme_bus_master_disable(context);

	/* Returns the computed result. */
	return function_result;
}

static int nvme_lifecycle_irq_remove(void *context);

/* Supports the nvme lifecycle irq remove operation. */
static int
nvme_lifecycle_irq_remove(
	void *context)
{
	int function_result;

	/* Obtains the nvme irq remove result. */
	function_result = nvme_irq_remove(context);

	/* Returns the computed result. */
	return function_result;
}

static int nvme_lifecycle_irq_drain(void *context);

/* Supports the nvme lifecycle irq drain operation. */
static int
nvme_lifecycle_irq_drain(
	void *context)
{
	struct nvme_controller *controller = context;
	int error = nvme_irq_drain(controller);

	/* Checks the operation status. */
	if (error == 0 && controller->io_slot_count != 0U)
		error = nvme_io_lifecycles_quiesce(controller);

	/* Returns the computed result. */
	return error;
}

static void nvme_lifecycle_irq_free(void *context);

/* Supports the nvme lifecycle irq free operation. */
static void
nvme_lifecycle_irq_free(
	void *context)
{
	struct nvme_controller *controller = context;

	drv_pci_device_free_irqs(controller->pci, &controller->irq, 1);
	controller->irq_allocated = 0;
}

static void nvme_lifecycle_dma_free(void *context);

/* Supports the nvme lifecycle dma free operation. */
static void
nvme_lifecycle_dma_free(
	void *context)
{
	nvme_dma_free(context);
}

static void nvme_lifecycle_bar_unmap(void *context);

/* Supports the nvme lifecycle bar unmap operation. */
static void
nvme_lifecycle_bar_unmap(
	void *context)
{
	struct nvme_controller *controller = context;

	drv_pci_device_unmap_bar(controller->pci, &controller->mapping);
	controller->bar_mapped = 0;
	controller->registers = NULL;
}

static int nvme_lifecycle_bar_restore(void *context);

/* Supports the nvme lifecycle bar restore operation. */
static int
nvme_lifecycle_bar_restore(
	void *context)
{
	struct nvme_controller *controller = context;
	int error = nvme_restore_bar(controller);
	int quiesce_error;

	/* Checks the operation status. */
	if (error == 0) {
		controller->original_bar_valid = 0;

		/* Reports successful completion. */
		return 0;
	}
	quiesce_error = nvme_pci_quiesce(controller);

	/* Checks the operation status. */
	if (quiesce_error != 0) {
		hal_printf("nvme: PCI quiesce after BAR restore failure failed "
			   "(%d)\n",
			   quiesce_error);
	}

	/* Returns the computed result. */
	return error;
}

static int nvme_lifecycle_pci_restore(void *context);

/* Supports the nvme lifecycle pci restore operation. */
static int
nvme_lifecycle_pci_restore(
	void *context)
{
	struct nvme_controller *controller = context;
	int error, message_error, quiesce_error;

	error = nvme_message_irq_restore(controller);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;
	error = drv_pci_device_restore_enable_state(
		controller->pci, &controller->pci_enable_state);

	/* Checks the operation status. */
	if (error == 0) {
		controller->inherited_msi_saved = 0;
		controller->inherited_msix_saved = 0;
		controller->pci_state_saved = 0;

		/* Reports successful completion. */
		return 0;
	}

fail:
	message_error = nvme_message_irq_mask(controller);

	/* Checks the operation status. */
	if (message_error != 0) {
		hal_printf("nvme: message interrupt mask after PCI restore "
			   "failure failed (%d)\n",
			   message_error);
	}
	quiesce_error = nvme_pci_quiesce(controller);

	/* Checks the operation status. */
	if (quiesce_error != 0) {
		hal_printf("nvme: PCI quiesce after command restore failure "
			   "failed (%d)\n",
			   quiesce_error);
	}

	/* Returns the computed result. */
	return error;
}

static void nvme_lifecycle_bar_release(void *context);

/* Supports the nvme lifecycle bar release operation. */
static void
nvme_lifecycle_bar_release(
	void *context)
{
	struct nvme_controller *controller = context;

	drv_pci_device_release_bar(controller->pci, 0);
	controller->bar_claimed = 0;
}

static const struct drv_nvme_lifecycle_ops nvme_lifecycle_operations = {
	.controller_disable = nvme_lifecycle_controller_disable,
	.bus_master_disable = nvme_lifecycle_master_disable,
	.irq_disestablish = nvme_lifecycle_irq_remove,
	.irq_drain = nvme_lifecycle_irq_drain,
	.irq_free = nvme_lifecycle_irq_free,
	.dma_free = nvme_lifecycle_dma_free,
	.bar_unmap = nvme_lifecycle_bar_unmap,
	.bar_restore = nvme_lifecycle_bar_restore,
	.pci_state_restore = nvme_lifecycle_pci_restore,
	.bar_release = nvme_lifecycle_bar_release,
};

static int nvme_cleanup(struct nvme_controller *controller);

/* Supports the nvme cleanup operation. */
static int
nvme_cleanup(
	struct nvme_controller *controller)
{
	int error;

	error = nvme_stop_admission(controller);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = drv_nvme_lifecycle_cleanup(
		&controller->lifecycle, &nvme_lifecycle_operations, controller);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	controller->controller_owned = 0;
	controller->controller_enabled = 0;
	controller->dma_allocated = 0;
	controller->irq_allocated = 0;

	/* Reports successful completion. */
	return 0;
}

static void nvme_publish_controller(struct nvme_controller *controller);

/* Supports the nvme publish controller operation. */
static void
nvme_publish_controller(
	struct nvme_controller *controller)
{
	unsigned long irq;

	(void)drv_pci_device_set_driver_data(controller->pci, controller);
	irq = spin_lock_irqsave(&nvme_registry_lock);
	nvme_primary = controller;
	spin_unlock_irqrestore(&nvme_registry_lock, irq);
}

static void nvme_unpublish_controller(struct nvme_controller *controller);

/* Supports the nvme unpublish controller operation. */
static void
nvme_unpublish_controller(
	struct nvme_controller *controller)
{
	unsigned long irq = spin_lock_irqsave(&nvme_registry_lock);

	/* Handles the nvme primary condition. */
	if (nvme_primary == controller)
		nvme_primary = NULL;
	spin_unlock_irqrestore(&nvme_registry_lock, irq);
}

/* Supports the nvme io fail all locked operation. */
static void
nvme_io_fail_all_locked(
	struct nvme_controller *controller,
	int error)
{
	struct nvme_io_slot *slot;
	unsigned slot_index;

	/* Checks the operation status. */
	if (error == 0)
		error = EIO;
	controller->io_fault = error;
	controller->io_recovery_needed = 1;
	controller->stopping = 1;
	/* Process each remaining element. */
	for (slot_index = 0; slot_index < controller->io_slot_count;
	     slot_index++) {
		slot = &controller->io_slots[slot_index];

		/* Handles the slot condition. */
		if (slot->state != NVME_IO_SLOT_ACTIVE)
			continue;
		(void)drv_nvme_io_lifecycle_fault(&slot->lifecycle);
		slot->state = NVME_IO_SLOT_FAULTED;
		slot->error = error;

		/* Handles the slot condition. */
		if (slot->posted) {
			/* Handles the controller condition. */
			if (controller->io_pending != 0U)
				controller->io_pending--;
			else
				controller->io_fault = EIO;
			slot->posted = 0;
		}
		waitq_wake_all(&slot->waitq);
	}
	waitq_wake_all(&controller->io_state_waitq);
}

static unsigned nvme_irq_admin_locked(struct nvme_controller *controller);

/* Supports the nvme irq admin locked operation. */
static unsigned
nvme_irq_admin_locked(
	struct nvme_controller *controller)
{
	volatile struct drv_nvme_completion *entry;
	struct drv_nvme_completion completion;
	unsigned consumed = 0;

	/* Continue while the operation condition remains true. */
	while (consumed < controller->queue_depth) {
		entry = &controller->admin_completion
				 [controller->completion_cursor.head];

		/* Handles the entry condition. */
		if ((entry->status & 1U) !=
		    (controller->completion_cursor.phase & 1U))
			break;
		hal_io_rmb();
		completion.result = entry->result;
		completion.reserved = entry->reserved;
		completion.submission_head = entry->submission_head;
		completion.submission_id = entry->submission_id;
		completion.command_id = entry->command_id;
		completion.status = entry->status;

		/* Checks the drv nvme completion matches result. */
		if (!controller->command_pending ||
		    !drv_nvme_completion_matches(
			    &completion, controller->completion_cursor.phase,
			    0U, (uint16_t)controller->submission_tail,
			    controller->expected_command_id)) {
			controller->admin_fault = EIO;
			controller->command_error = EIO;
		} else {
			controller->command_result = completion.result;
			controller->command_error =
				drv_nvme_completion_error(&completion);
		}
		controller->command_pending = 0;
		controller->command_completed = 1;
		drv_nvme_completion_cursor_advance(
			&controller->completion_cursor);
		consumed++;
	}

	/* Handles the consumed condition. */
	if (consumed != 0U) {
		nvme_write32(controller, controller->completion_doorbell,
			     controller->completion_cursor.head);
	}

	/* Returns the computed result. */
	return consumed;
}

static struct nvme_io_slot * nvme_io_completion_owner_locked(struct nvme_controller *controller, uint16_t command_id);

/* Supports the nvme io completion owner locked operation. */
static struct nvme_io_slot *
nvme_io_completion_owner_locked(
	struct nvme_controller *controller,
	uint16_t command_id)
{
	struct nvme_io_slot *slot;
	unsigned index;

	/* Process each remaining element. */
	for (index = 0; index < controller->io_slot_count; index++) {
		slot = &controller->io_slots[index];

		/* Handles the slot condition. */
		if (slot->state == NVME_IO_SLOT_ACTIVE && slot->posted &&
		    slot->epoch == controller->io_epoch &&
		    slot->command_id == command_id)

			/* Returns the computed result. */
			return slot;
	}

	/* Reports that no result is available. */
	return NULL;
}

static unsigned nvme_irq_io_locked(struct nvme_controller *controller);

/* Supports the nvme irq io locked operation. */
static unsigned
nvme_irq_io_locked(
	struct nvme_controller *controller)
{
	volatile struct drv_nvme_completion *entry;
	struct drv_nvme_completion completion;
	struct nvme_io_slot *slot;
	unsigned consumed = 0;

	/* Handles the io completion availability. */
	if (!controller->io_queue_ready || controller->io_completion == NULL)
		return 0;
	/* Continue while the operation condition remains true. */
	while (consumed < controller->io_queue_depth) {
		entry = &controller->io_completion
				 [controller->io_completion_cursor.head];

		/* Handles the entry condition. */
		if ((entry->status & 1U) !=
		    (controller->io_completion_cursor.phase & 1U))
			break;
		hal_io_rmb();
		completion.result = entry->result;
		completion.reserved = entry->reserved;
		completion.submission_head = entry->submission_head;
		completion.submission_id = entry->submission_id;
		completion.command_id = entry->command_id;
		completion.status = entry->status;
		slot = nvme_io_completion_owner_locked(controller,
						       completion.command_id);

		/* Handles the slot availability. */
		if (completion.submission_id != NVME_IO_QUEUE_ID ||
		    completion.submission_head >= controller->io_queue_depth ||
		    slot == NULL) {
			drv_nvme_completion_cursor_advance(
				&controller->io_completion_cursor);
			consumed++;
			nvme_io_fail_all_locked(controller, EIO);
			break;
		}

		/* Checks the drv nvme io lifecycle complete command result. */
		if (drv_nvme_io_lifecycle_complete_command(
			    &slot->lifecycle, completion.command_id) != 0 ||
		    controller->io_pending == 0U) {
			(void)drv_nvme_io_lifecycle_fault(&slot->lifecycle);
			slot->error = EIO;
			slot->state = NVME_IO_SLOT_FAULTED;

			/* Handles the slot condition. */
			if (slot->posted && controller->io_pending != 0U)
				controller->io_pending--;
			slot->posted = 0;
			waitq_wake_all(&slot->waitq);
			nvme_io_fail_all_locked(controller, EIO);
		} else {
			slot->error = drv_nvme_completion_error(&completion);
			slot->state = NVME_IO_SLOT_CQ_DONE;
			slot->posted = 0;
			controller->io_pending--;
			waitq_wake_all(&slot->waitq);
			waitq_wake_all(&controller->io_state_waitq);
		}
		drv_nvme_completion_cursor_advance(
			&controller->io_completion_cursor);
		consumed++;
	}

	/* Handles the consumed condition. */
	if (consumed != 0U) {
		nvme_write32(controller, controller->io_completion_doorbell,
			     controller->io_completion_cursor.head);
	}

	/* Returns the computed result. */
	return consumed;
}

static int nvme_irq(void *argument);

/* Supports the nvme irq operation. */
static int
nvme_irq(
	void *argument)
{
	struct nvme_controller *controller = argument;
	unsigned consumed;
	unsigned long irq;
	uint32_t status;

	/* Drains both completion queues and samples the status. */
	(void)atomic_raw_fetch_add_relaxed(&controller->irq_busy, 1U);
	irq = spin_lock_irqsave(&controller->command_lock);
	consumed = nvme_irq_admin_locked(controller);
	consumed += nvme_irq_io_locked(controller);
	status = controller->controller_enabled
			 ? nvme_read32(controller, DRV_NVME_REG_CSTS)
			 : 0U;

	/* Checks the operation status. */
	if (status == UINT32_MAX || (status & DRV_NVME_CSTS_FATAL) != 0U) {
		/* Handles the controller condition. */
		if (controller->command_pending) {
			controller->command_pending = 0;
			controller->command_completed = 1;
			controller->command_error = EIO;
			controller->admin_fault = EIO;
		}

		/* Handles the controller condition. */
		if (controller->io_queue_ready)
			nvme_io_fail_all_locked(controller, EIO);
		consumed++;
	}
	spin_unlock_irqrestore(&controller->command_lock, irq);

	/* Checks the atomic raw fetch add release result. */
	if (atomic_raw_fetch_add_release(&controller->irq_busy, (unsigned)-1) ==
	    0U)
		__builtin_trap();

	/* Returns the computed result. */
	return consumed != 0U;
}

static int nvme_admin_execute_mode(struct nvme_controller *controller, struct drv_nvme_command *command, uint32_t *result, int recovery_command);

/* Supports the nvme admin execute mode operation. */
static int
nvme_admin_execute_mode(
	struct nvme_controller *controller,
	struct drv_nvme_command *command,
	uint32_t *result,
	int recovery_command)
{
	volatile struct drv_nvme_command *slot;
	uint64_t deadline;
	unsigned next;
	unsigned long irq;
	int error = 0;

	/* Handles the controller availability. */
	if (controller == NULL || command == NULL)
		return EINVAL;
	deadline = clock_ticks() + controller->timeout_ticks;
	irq = spin_lock_irqsave(&controller->command_lock);

	/* Handles the controller condition. */
	if ((controller->stopping &&
	     !(recovery_command && controller->io_recovery_busy)) ||
	    controller->quarantined || !controller->controller_enabled) {
		error = ENXIO;
		goto out;
	}

	/* Handles the controller condition. */
	if (controller->admin_fault != 0) {
		error = controller->admin_fault;
		goto out;
	}

	/* Handles the controller condition. */
	if (controller->command_pending) {
		error = EBUSY;
		goto out;
	}
	controller->next_command_id++;

	/* Handles the controller condition. */
	if (controller->next_command_id == 0U)
		controller->next_command_id++;
	command->cdw0 &= 0x0000ffffU;
	command->cdw0 |= (uint32_t)controller->next_command_id << 16;
	controller->expected_command_id = controller->next_command_id;
	controller->command_error = 0;
	controller->command_result = 0;
	controller->command_completed = 0;
	controller->command_pending = 1;
	slot = &controller->admin_submission[controller->submission_tail];
	*slot = *command;
	next = drv_nvme_queue_index_advance(controller->submission_tail,
					    controller->queue_depth);

	/* Handles the next condition. */
	if (next == UINT32_MAX) {
		controller->command_pending = 0;
		error = EIO;
		goto out;
	}
	controller->submission_tail = next;
	hal_io_wmb();
	nvme_write32(controller, controller->submission_doorbell,
		     controller->submission_tail);
	/* Continue while the operation condition remains true. */
	while (!controller->command_completed) {
		/*
		 * Completion remains strictly interrupt-driven.  Do not sleep
		 * with the irqsave state held: the initial boot thread can
		 * otherwise prevent the CPU0-targeted MSI from leaving the
		 * local APIC IRR.
		 */
		spin_unlock_irqrestore(&controller->command_lock, irq);
		sched_yield();
		irq = spin_lock_irqsave(&controller->command_lock);

		/* Checks the clock ticks result. */
		if (!controller->command_completed &&
		    clock_ticks() >= deadline) {
			error = ETIMEDOUT;
			break;
		}
	}

	/* Checks the operation status. */
	if (error != 0) {
		controller->command_pending = 0;
		controller->admin_fault = error;
	} else {
		error = controller->command_error;

		/* Handles the result availability. */
		if (result != NULL)
			*result = controller->command_result;
	}
out:
	spin_unlock_irqrestore(&controller->command_lock, irq);

	/* Returns the computed result. */
	return error;
}

static int nvme_admin_execute(struct nvme_controller *controller, struct drv_nvme_command *command, uint32_t *result);

/* Supports the nvme admin execute operation. */
static int
nvme_admin_execute(
	struct nvme_controller *controller,
	struct drv_nvme_command *command,
	uint32_t *result)
{
	int function_result;

	/* Obtains the nvme admin execute mode result. */
	function_result =
		nvme_admin_execute_mode(controller, command, result, 0);

	/* Returns the computed result. */
	return function_result;
}

static int nvme_identify(struct nvme_controller *controller, uint32_t namespace_id, uint8_t selector);

/* Supports the nvme identify operation. */
static int
nvme_identify(
	struct nvme_controller *controller,
	uint32_t namespace_id,
	uint8_t selector)
{
	int function_result;
	struct drv_nvme_command command;

	memset(controller->identify_dma.address, 0,
	       controller->identify_dma.size);

	/* Checks the drv nvme identify command result. */
	if (!drv_nvme_identify_command(&command, 0, namespace_id, selector,
				       controller->identify_dma.device_address))

		/* Returns the computed result. */
		return EINVAL;
	hal_io_wmb();

	/* Obtains the nvme admin execute result. */
	function_result = nvme_admin_execute(controller, &command, NULL);

	/* Returns the computed result. */
	return function_result;
}

static int nvme_io_wait_locked(struct nvme_controller *controller, struct wait_queue *waitq, uint64_t deadline, unsigned long *irq);

/* Supports the nvme io wait locked operation. */
static int
nvme_io_wait_locked(
	struct nvme_controller *controller,
	struct wait_queue *waitq,
	uint64_t deadline,
	unsigned long *irq)
{
	int function_result;
	uint64_t sequence;
	struct thread *thread = thread_current();
	int error;

	/*
	 * Early VFS discovery runs in CPU0's idle thread.  An idle thread
	 * cannot enter the ordinary wait-queue sleep path: when no other thread
	 * is runnable the scheduler immediately selects the same idle context
	 * while the irqsave state remains disabled.  A CPU0-targeted MSI (and
	 * the timer which advances the deadline) would then remain pending
	 * forever.  Poll like the boot-time admin path in that context,
	 * restoring interrupts on every pass.  Once regular threads exist, use
	 * the wait queue normally.
	 */
	if (thread != NULL && (thread->flags & THREAD_FLAG_IDLE) == 0U) {
		sequence = waitq_sequence(waitq);

		error = waitq_sleep(waitq, &controller->command_lock, sequence,
				    deadline, 0);

		/* Returns the computed result. */
		return error == EAGAIN ? 0 : error;
	}
	spin_unlock_irqrestore(&controller->command_lock, *irq);
	sched_yield();
	*irq = spin_lock_irqsave(&controller->command_lock);
	/* Computes the function result. */
	function_result = sched_ticks() >= deadline ? ETIMEDOUT : 0;

	/* Returns the computed result. */
	return function_result;
}

static int nvme_io_ensure_online(struct nvme_controller *controller);

/* Supports the nvme io ensure online operation. */
static int
nvme_io_ensure_online(
	struct nvme_controller *controller)
{
	int function_result;
	uint64_t deadline = sched_ticks() + controller->timeout_ticks;
	unsigned long irq;
	int error;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		irq = spin_lock_irqsave(&controller->command_lock);

		/* Handles the controller condition. */
		if (controller->io_queue_ready && !controller->stopping &&
		    !controller->quarantined &&
		    !controller->io_recovery_needed) {
			spin_unlock_irqrestore(&controller->command_lock, irq);

			/* Reports successful completion. */
			return 0;
		}

		/* Handles the controller condition. */
		if (controller->io_recovery_needed &&
		    !controller->io_recovery_busy && !controller->detach_busy &&
		    !controller->quarantined) {
			controller->io_recovery_busy = 1;
			spin_unlock_irqrestore(&controller->command_lock, irq);

			/* Obtains the nvme io recover result. */
			function_result = nvme_io_recover(controller);

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles the controller condition. */
		if (!controller->io_recovery_busy) {
			spin_unlock_irqrestore(&controller->command_lock, irq);

			/* Returns the computed result. */
			return ENXIO;
		}
		error = nvme_io_wait_locked(controller,
					    &controller->io_state_waitq,
					    deadline, &irq);
		spin_unlock_irqrestore(&controller->command_lock, irq);

		/* Checks the operation status. */
		if (error != 0)
			return error;
	}
}

static int nvme_io_begin_bio(struct nvme_controller *controller, enum bio_op operation, int *owned);

/* Supports the nvme io begin bio operation. */
static int
nvme_io_begin_bio(
	struct nvme_controller *controller,
	enum bio_op operation,
	int *owned)
{
	uint64_t deadline = sched_ticks() + controller->timeout_ticks;
	unsigned long irq;
	int error = 0;

	*owned = 0;
	error = nvme_io_ensure_online(controller);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	irq = spin_lock_irqsave(&controller->command_lock);

	/* Handles the controller condition. */
	if (controller->stopping || controller->quarantined ||
	    !controller->io_queue_ready) {
		error = ENXIO;
		goto out;
	}
	controller->io_calls++;
	*owned = 1;
	/* Validates the selected operation. */
	if (operation == BIO_FLUSH) {
		controller->io_flush_waiting++;
		/* Continue while the operation condition remains true. */
		while (controller->io_data_active != 0U ||
		       controller->io_flush_active) {
			/* Handles the controller condition. */
			if (controller->stopping) {
				error = ENXIO;
				break;
			}
			error = nvme_io_wait_locked(controller,
						    &controller->io_state_waitq,
						    deadline, &irq);

			/* Checks the operation status. */
			if (error != 0)
				break;
		}
		controller->io_flush_waiting--;

		/* Checks the operation status. */
		if (error == 0)
			controller->io_flush_active = 1;
	} else {
		/* Continue while the operation condition remains true. */
		while (controller->io_flush_waiting != 0U ||
		       controller->io_flush_active) {
			/* Handles the controller condition. */
			if (controller->stopping) {
				error = ENXIO;
				break;
			}
			error = nvme_io_wait_locked(controller,
						    &controller->io_state_waitq,
						    deadline, &irq);

			/* Checks the operation status. */
			if (error != 0)
				break;
		}

		/* Checks the operation status. */
		if (error == 0)
			controller->io_data_active++;
	}

	/* Checks the operation status. */
	if (error != 0) {
		controller->io_calls--;
		*owned = 0;
		waitq_wake_all(&controller->io_state_waitq);
	}
out:
	spin_unlock_irqrestore(&controller->command_lock, irq);

	/* Returns the computed result. */
	return error;
}

static void nvme_io_end_bio(struct nvme_controller *controller, enum bio_op operation);

/* Supports the nvme io end bio operation. */
static void
nvme_io_end_bio(
	struct nvme_controller *controller,
	enum bio_op operation)
{
	unsigned long irq = spin_lock_irqsave(&controller->command_lock);

	/* Validates the selected operation. */
	if (operation == BIO_FLUSH) {
		/* Handles the controller condition. */
		if (!controller->io_flush_active)
			__builtin_trap();
		controller->io_flush_active = 0;
	} else {
		/* Handles the controller condition. */
		if (controller->io_data_active == 0U)
			__builtin_trap();
		controller->io_data_active--;
	}

	/* Handles the controller condition. */
	if (controller->io_calls == 0U)
		__builtin_trap();
	controller->io_calls--;
	waitq_wake_all(&controller->io_state_waitq);
	spin_unlock_irqrestore(&controller->command_lock, irq);
}

static int nvme_io_command_id_in_use_locked(struct nvme_controller *controller, uint16_t command_id);

/* Supports the nvme io command id in use locked operation. */
static int
nvme_io_command_id_in_use_locked(
	struct nvme_controller *controller,
	uint16_t command_id)
{
	unsigned index;

	/* Process each remaining element. */
	for (index = 0; index < controller->io_slot_count; index++) {
		/* Handles the controller condition. */
		if (controller->io_slots[index].state != NVME_IO_SLOT_FREE &&
		    controller->io_slots[index].command_id == command_id)

			/* Reports operation failure. */
			return 1;
	}

	/* Reports successful completion. */
	return 0;
}

static uint16_t nvme_io_next_command_id_locked(struct nvme_controller *controller);

/* Supports the nvme io next command id locked operation. */
static uint16_t
nvme_io_next_command_id_locked(
	struct nvme_controller *controller)
{
	unsigned attempts;

	/* Process each element required by the operation. */
	for (attempts = 0; attempts < UINT16_MAX; attempts++) {
		controller->next_io_command_id++;

		/* Handles the controller condition. */
		if (controller->next_io_command_id == 0U)
			controller->next_io_command_id++;

		/* Checks the nvme io command id in use locked result. */
		if (!nvme_io_command_id_in_use_locked(
			    controller, controller->next_io_command_id))

			/* Returns the computed result. */
			return controller->next_io_command_id;
	}

	/* Reports successful completion. */
	return 0;
}

static int nvme_io_slot_acquire(struct nvme_controller *controller, struct nvme_io_slot **result);

/* Supports the nvme io slot acquire operation. */
static int
nvme_io_slot_acquire(
	struct nvme_controller *controller,
	struct nvme_io_slot **result)
{
	struct nvme_io_slot *slot;
	uint16_t command_id;
	unsigned index;
	uint64_t deadline = sched_ticks() + controller->timeout_ticks;
	unsigned long irq;
	int error = 0;

	irq = spin_lock_irqsave(&controller->command_lock);
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles the controller condition. */
		if (controller->stopping || controller->quarantined ||
		    !controller->io_queue_ready || controller->io_fault != 0) {
			error = ENXIO;
			break;
		}
		/* Process each remaining element. */
		for (index = 0; index < controller->io_slot_count; index++) {
			slot = &controller->io_slots[index];

			/* Handles the slot condition. */
			if (slot->state != NVME_IO_SLOT_FREE)
				continue;
			command_id = nvme_io_next_command_id_locked(controller);

			/* Handles the command id condition. */
			if (command_id == 0U) {
				error = EBUSY;
				break;
			}

			/* Checks the drv nvme io lifecycle begin bio result. */
			if (drv_nvme_io_lifecycle_begin_bio(&slot->lifecycle) !=
			    0) {
				error = EIO;
				break;
			}
			slot->command_id = command_id;
			slot->error = 0;
			slot->state = NVME_IO_SLOT_ACTIVE;
			slot->posted = 0;
			slot->epoch = controller->io_epoch;
			controller->io_owned++;
			*result = slot;
			spin_unlock_irqrestore(&controller->command_lock, irq);

			/* Reports successful completion. */
			return 0;
		}

		/* Checks the operation status. */
		if (error != 0)
			break;
		error = nvme_io_wait_locked(controller,
					    &controller->io_state_waitq,
					    deadline, &irq);

		/* Checks the operation status. */
		if (error != 0)
			break;
	}
	spin_unlock_irqrestore(&controller->command_lock, irq);

	/* Returns the computed result. */
	return error;
}

static int nvme_io_post(struct nvme_controller *controller, struct nvme_io_slot *slot, const struct drv_nvme_command *command, int uses_payload);

/* Supports the nvme io post operation. */
static int
nvme_io_post(
	struct nvme_controller *controller,
	struct nvme_io_slot *slot,
	const struct drv_nvme_command *command,
	int uses_payload)
{
	volatile struct drv_nvme_command *submission;
	unsigned next;
	unsigned long irq;
	int error;

	irq = spin_lock_irqsave(&controller->command_lock);

	/* Handles the slot condition. */
	if (slot->state != NVME_IO_SLOT_ACTIVE || controller->stopping ||
	    controller->quarantined || !controller->io_queue_ready ||
	    controller->io_fault != 0) {
		error = ENXIO;
		goto fail;
	}

	/* Handles the controller condition. */
	if (controller->io_pending >= controller->io_queue_depth - 1U) {
		error = EBUSY;
		goto fail;
	}
	error = drv_nvme_io_lifecycle_submit(&slot->lifecycle, slot->command_id,
					     uses_payload);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;
	submission = &controller->io_submission[controller->io_submission_tail];
	*submission = *command;
	next = drv_nvme_queue_index_advance(controller->io_submission_tail,
					    controller->io_queue_depth);

	/* Handles the next condition. */
	if (next == UINT32_MAX) {
		(void)drv_nvme_io_lifecycle_fault(&slot->lifecycle);
		error = EIO;
		goto fail;
	}
	controller->io_submission_tail = next;
	slot->posted = 1;
	controller->io_pending++;
	hal_io_wmb();
	nvme_write32(controller, controller->io_submission_doorbell,
		     controller->io_submission_tail);
	spin_unlock_irqrestore(&controller->command_lock, irq);

	/* Reports successful completion. */
	return 0;

fail:
	nvme_io_fail_all_locked(controller, error);
	spin_unlock_irqrestore(&controller->command_lock, irq);

	/* Returns the computed result. */
	return error;
}

static int nvme_io_wait_completion(struct nvme_controller *controller, struct nvme_io_slot *slot, int *recovery_owner);

/* Supports the nvme io wait completion operation. */
static int
nvme_io_wait_completion(
	struct nvme_controller *controller,
	struct nvme_io_slot *slot,
	int *recovery_owner)
{
	uint64_t deadline = sched_ticks() + controller->timeout_ticks;
	unsigned long irq;
	int error = 0;

	*recovery_owner = 0;
	irq = spin_lock_irqsave(&controller->command_lock);
	/* Continue while the operation condition remains true. */
	while (slot->state == NVME_IO_SLOT_ACTIVE) {
		error = nvme_io_wait_locked(controller, &slot->waitq, deadline,
					    &irq);

		/* Checks the operation status. */
		if (error != 0 && slot->state == NVME_IO_SLOT_ACTIVE) {
			nvme_io_fail_all_locked(controller, error == ETIMEDOUT
								    ? ETIMEDOUT
								    : EIO);
			break;
		}
	}

	/* Handles the slot condition. */
	if (slot->state == NVME_IO_SLOT_CQ_DONE ||
	    slot->state == NVME_IO_SLOT_FAULTED) {
		error = slot->error;
		slot->state = NVME_IO_SLOT_COPYING;
	} else if (error == 0) {
		error = EIO;
	}

	/* Handles the controller condition. */
	if (controller->io_recovery_needed && !controller->io_recovery_busy &&
	    !controller->detach_busy) {
		controller->io_recovery_busy = 1;
		*recovery_owner = 1;
	}
	spin_unlock_irqrestore(&controller->command_lock, irq);

	/* Returns the computed result. */
	return error;
}

static int nvme_io_claim_recovery_locked(struct nvme_controller *controller);

/* Supports the nvme io claim recovery locked operation. */
static int
nvme_io_claim_recovery_locked(
	struct nvme_controller *controller)
{
	/* Handles the controller condition. */
	if (!controller->io_recovery_needed || controller->io_recovery_busy ||
	    controller->detach_busy)

		/* Reports successful completion. */
		return 0;
	controller->io_recovery_busy = 1;

	/* Reports operation failure. */
	return 1;
}

static void nvme_io_slot_release(struct nvme_controller *controller, struct nvme_io_slot *slot);

/* Supports the nvme io slot release operation. */
static void
nvme_io_slot_release(
	struct nvme_controller *controller,
	struct nvme_io_slot *slot)
{
	unsigned long irq = spin_lock_irqsave(&controller->command_lock);

	/* Handles the slot condition. */
	if (slot->state != NVME_IO_SLOT_COPYING &&
	    slot->state != NVME_IO_SLOT_FAULTED)
		__builtin_trap();

	/* Checks the drv nvme io lifecycle complete bio result. */
	if (drv_nvme_io_lifecycle_complete_bio(&slot->lifecycle) != 0)
		__builtin_trap();
	slot->state = NVME_IO_SLOT_FREE;
	slot->posted = 0;
	slot->command_id = 0;
	slot->error = 0;

	/* Handles the controller condition. */
	if (controller->io_owned == 0U)
		__builtin_trap();
	controller->io_owned--;
	waitq_wake_all(&controller->io_state_waitq);
	spin_unlock_irqrestore(&controller->command_lock, irq);
}

static int nvme_io_execute(struct nvme_controller *controller, uint8_t opcode, uint64_t first_block, uint32_t block_count, void *bytes);

/* Supports the nvme io execute operation. */
static int
nvme_io_execute(
	struct nvme_controller *controller,
	uint8_t opcode,
	uint64_t first_block,
	uint32_t block_count,
	void *bytes)
{
	unsigned long irq_local;
	unsigned long irq_local1;
	struct drv_nvme_command command;
	struct nvme_io_slot *slot;
	int recovery_owner;
	int error;

	error = nvme_io_slot_acquire(controller, &slot);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the opcode condition. */
	if (opcode == DRV_NVME_NVM_WRITE) {
		memcpy(slot->bounce_dma.address, bytes,
		       (size_t)block_count * controller->namespace_block_size);
	}

	/* Checks the drv nvme io command result. */
	if (!drv_nvme_io_command(&command, slot->command_id, opcode,
				 controller->namespace_id, first_block,
				 block_count,
				 opcode == DRV_NVME_NVM_FLUSH
					 ? 0U
					 : slot->bounce_dma.device_address) ||
	    (opcode != DRV_NVME_NVM_FLUSH &&
	     !drv_nvme_single_prp_transfer_valid(
		     slot->bounce_dma.device_address,
		     controller->namespace_block_size, block_count,
		     controller->page_size))) {
		irq_local = spin_lock_irqsave(&controller->command_lock);

		nvme_io_fail_all_locked(controller, EINVAL);
		recovery_owner = nvme_io_claim_recovery_locked(controller);
		spin_unlock_irqrestore(&controller->command_lock, irq_local);
		nvme_io_slot_release(controller, slot);

		/* Handles the recovery owner condition. */
		if (recovery_owner)
			(void)nvme_io_recover(controller);

		/* Returns the computed result. */
		return EINVAL;
	}
	error = nvme_io_post(controller, slot, &command,
			     opcode != DRV_NVME_NVM_FLUSH);

	/* Checks the operation status. */
	if (error == 0) {
		error = nvme_io_wait_completion(controller, slot,
						&recovery_owner);
	} else {
		irq_local1 = spin_lock_irqsave(&controller->command_lock);

		recovery_owner = nvme_io_claim_recovery_locked(controller);
		spin_unlock_irqrestore(&controller->command_lock, irq_local1);
	}

	/* Checks the operation status. */
	if (error == 0 && opcode == DRV_NVME_NVM_READ) {
		hal_io_rmb();
		memcpy(bytes, slot->bounce_dma.address,
		       (size_t)block_count * controller->namespace_block_size);
	}
	nvme_io_slot_release(controller, slot);

	/* Handles the recovery owner condition. */
	if (recovery_owner)
		(void)nvme_io_recover(controller);

	/* Returns the computed result. */
	return error;
}

static int nvme_disk_submit(struct disk *disk, struct bio *bio);

/* Supports the nvme disk submit operation. */
static int
nvme_disk_submit(
	struct disk *disk,
	struct bio *bio)
{
	uint32_t chunk;
	size_t chunk_bytes;
	struct nvme_controller *controller;
	uint64_t block;
	uint32_t remaining;
	uint8_t *bytes;
	size_t transferred = 0;
	int owned;
	int error;

	/* Handles the disk availability. */
	if (disk == NULL || bio == NULL || disk->d_data == NULL)
		return EINVAL;

	/* Handles the bio condition. */
	if (bio->b_op != BIO_READ && bio->b_op != BIO_WRITE &&
	    bio->b_op != BIO_FLUSH)

		/* Returns the computed result. */
		return EOPNOTSUPP;
	controller = disk->d_data;

	/* Checks the drv nvme io range valid result. */
	if (bio->b_op != BIO_FLUSH &&
	    (!drv_nvme_io_range_valid(bio->b_mapped_block, bio->b_block_count,
				      controller->namespace_blocks) ||
	     controller->namespace_block_size == 0U ||
	     (size_t)bio->b_block_count >
		     SIZE_MAX / controller->namespace_block_size))

		/* Returns the computed result. */
		return EOVERFLOW;
	error = nvme_io_begin_bio(controller, bio->b_op, &owned);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the owned condition. */
	if (!owned)
		return EIO;

	/* Handles the bio condition. */
	if (bio->b_op == BIO_FLUSH) {
		error = nvme_io_execute(controller, DRV_NVME_NVM_FLUSH, 0U, 0U,
					NULL);
	} else {
		block = bio->b_mapped_block;
		remaining = bio->b_block_count;
		bytes = bio->b_data;
		/* Continue while the operation condition remains true. */
		while (remaining != 0U) {
			chunk = drv_nvme_io_chunk_blocks(
				remaining, controller->namespace_block_size,
				NVME_IO_BOUNCE_SIZE,
				controller->maximum_transfer_bytes);

			/* Handles the chunk condition. */
			if (chunk == 0U) {
				error = EINVAL;
				break;
			}
			chunk_bytes = (size_t)chunk *
				      controller->namespace_block_size;
			error = nvme_io_execute(controller,
						bio->b_op == BIO_READ
							? DRV_NVME_NVM_READ
							: DRV_NVME_NVM_WRITE,
						block, chunk, bytes);

			/* Checks the operation status. */
			if (error != 0)
				break;
			block += chunk;
			remaining -= chunk;
			bytes += chunk_bytes;
			transferred += chunk_bytes;
		}
	}
	nvme_io_end_bio(controller, bio->b_op);
	bio_complete(bio, error, transferred);

	/* Reports successful completion. */
	return 0;
}

static const struct disk_ops nvme_disk_ops = {
	.submit = nvme_disk_submit,
};

static int nvme_io_flush_internal(struct nvme_controller *controller);

/* Supports the nvme io flush internal operation. */
static int
nvme_io_flush_internal(
	struct nvme_controller *controller)
{
	int owned;
	int error;

	error = nvme_io_begin_bio(controller, BIO_FLUSH, &owned);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the owned condition. */
	if (!owned)
		return EIO;
	error = nvme_io_execute(controller, DRV_NVME_NVM_FLUSH, 0U, 0U, NULL);
	nvme_io_end_bio(controller, BIO_FLUSH);

	/* Returns the computed result. */
	return error;
}

static int nvme_probe_permitted(struct nvme_controller *controller);

/* Supports the nvme probe permitted operation. */
static int
nvme_probe_permitted(
	struct nvme_controller *controller)
{
	unsigned long irq = spin_lock_irqsave(&controller->command_lock);
	int permitted = !controller->stopping && !controller->detach_busy &&
			!controller->quarantined;

	spin_unlock_irqrestore(&controller->command_lock, irq);

	/* Returns the computed result. */
	return permitted;
}

static int nvme_probe_namespace(struct nvme_controller *controller);

/* Supports the nvme probe namespace operation. */
static int
nvme_probe_namespace(
	struct nvme_controller *controller)
{
	struct drv_nvme_controller_profile controller_profile;
	struct drv_nvme_namespace_profile namespace_profile;
	struct disk *disk;
	uint32_t namespace_id;
	uint32_t reasons;
	int error;

	error = nvme_identify(controller, 0, DRV_NVME_IDENTIFY_CONTROLLER);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	reasons = drv_nvme_identify_controller_validate(
		controller->identify_dma.address, controller->identify_dma.size,
		&controller_profile);

	/* Handles the reasons condition. */
	if (reasons != 0U) {
		hal_printf("nvme: unsupported Identify Controller (%08x)\n",
			   reasons);

		/* Returns the computed result. */
		return EOPNOTSUPP;
	}
	error = nvme_identify(controller, 0, DRV_NVME_IDENTIFY_ACTIVE_LIST);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	reasons = drv_nvme_active_namespace_validate(
		controller->identify_dma.address, controller->identify_dma.size,
		controller_profile.namespace_count, &namespace_id);

	/* Handles the reasons condition. */
	if (reasons != 0U) {
		hal_printf("nvme: unsupported active namespace set (%08x)\n",
			   reasons);

		/* Returns the computed result. */
		return EOPNOTSUPP;
	}
	error = nvme_identify(controller, namespace_id,
			      DRV_NVME_IDENTIFY_NAMESPACE);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	reasons = drv_nvme_identify_namespace_validate(
		controller->identify_dma.address, controller->identify_dma.size,
		&namespace_profile);

	/* Handles the reasons condition. */
	if (reasons != 0U) {
		hal_printf("nvme: unsupported namespace %u (%08x)\n",
			   namespace_id, reasons);

		/* Returns the computed result. */
		return EOPNOTSUPP;
	}
	controller->namespace_id = namespace_id;
	controller->namespace_blocks = namespace_profile.block_count;
	controller->namespace_block_size =
		UINT32_C(1) << namespace_profile.block_size_shift;
	controller->maximum_transfer_bytes = drv_nvme_max_transfer_bytes(
		controller_profile.maximum_transfer_shift,
		controller->page_size, NVME_IO_BOUNCE_SIZE);

	/* Handles the controller condition. */
	if (controller->maximum_transfer_bytes <
	    controller->namespace_block_size)

		/* Returns the computed result. */
		return EOPNOTSUPP;
	error = nvme_io_dma_allocate(controller);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = nvme_io_queue_create(controller, 0);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	disk = disk_alloc();

	/* Handles the disk availability. */
	if (disk == NULL)
		return ENOMEM;
	error = disk_alloc_nvme_name(disk, 0, namespace_id);

	/* Checks the operation status. */
	if (error != 0)
		goto fail_disk;
	disk->d_flags = 0;
	disk->d_block_size = controller->namespace_block_size;
	disk->d_block_count = namespace_profile.block_count;
	disk->d_max_transfer_blocks =
		(uint32_t)(controller->maximum_transfer_bytes /
			   disk->d_block_size);
	disk->d_ops = &nvme_disk_ops;
	disk->d_data = controller;

	/* Checks the nvme probe permitted result. */
	if (!nvme_probe_permitted(controller)) {
		error = ENXIO;
		goto fail_disk;
	}
	error = disk_create(disk);

	/* Checks the operation status. */
	if (error != 0)
		goto fail_disk;
	controller->namespace_disk = disk;
	hal_printf("nvme: /dev/%s namespace=%u blocks=%08x:%08x block-size=%u "
		   "writable max-transfer=%u\n",
		   disk->d_name, namespace_id,
		   (uint32_t)(namespace_profile.block_count >> 32),
		   (uint32_t)namespace_profile.block_count, disk->d_block_size,
		   disk->d_max_transfer_blocks);

	/* Reports successful completion. */
	return 0;

fail_disk:
	(void)disk_destroy(disk);

	/* Returns the computed result. */
	return error;
}

static int nvme_dma_allocate(struct nvme_controller *controller);

/* Supports the nvme dma allocate operation. */
static int
nvme_dma_allocate(
	struct nvme_controller *controller)
{
	struct drv_dma_device *dma = drv_pci_device_dma(controller->pci);
	size_t submission_bytes, completion_bytes;
	int error;

	/* Checks the drv nvme queue bytes result. */
	if (dma == NULL ||
	    !drv_nvme_queue_bytes(controller->queue_depth,
				  sizeof(struct drv_nvme_command),
				  &submission_bytes) ||
	    !drv_nvme_queue_bytes(controller->queue_depth,
				  sizeof(struct drv_nvme_completion),
				  &completion_bytes))

		/* Returns the computed result. */
		return EINVAL;
	error = drv_dma_alloc_coherent(dma, submission_bytes,
				       controller->page_size,
				       &controller->admin_submission_dma);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = drv_dma_alloc_coherent(dma, completion_bytes,
				       controller->page_size,
				       &controller->admin_completion_dma);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;
	error = drv_dma_alloc_coherent(dma, DRV_NVME_IDENTIFY_SIZE,
				       controller->page_size,
				       &controller->identify_dma);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;

	/* Handles the controller condition. */
	if ((controller->admin_submission_dma.device_address &
	     (controller->page_size - 1U)) != 0U ||
	    (controller->admin_completion_dma.device_address &
	     (controller->page_size - 1U)) != 0U ||
	    (controller->identify_dma.device_address &
	     (controller->page_size - 1U)) != 0U) {
		error = EIO;
		goto fail;
	}
	memset(controller->admin_submission_dma.address, 0,
	       controller->admin_submission_dma.size);
	memset(controller->admin_completion_dma.address, 0,
	       controller->admin_completion_dma.size);
	memset(controller->identify_dma.address, 0,
	       controller->identify_dma.size);
	controller->admin_submission = controller->admin_submission_dma.address;
	controller->admin_completion = controller->admin_completion_dma.address;
	controller->dma_allocated = 1;

	/* Reports successful completion. */
	return 0;

fail:
	nvme_dma_free(controller);

	/* Returns the computed result. */
	return error;
}

static void nvme_io_dma_free_unpublished(struct nvme_controller *controller);

/* Supports the nvme io dma free unpublished operation. */
static void
nvme_io_dma_free_unpublished(
	struct nvme_controller *controller)
{
	struct drv_dma_device *dma = drv_pci_device_dma(controller->pci);
	unsigned index;

	/* Process each remaining element. */
	for (index = 0; index < NVME_IO_MAX_SLOTS; index++) {
		/* Handles the address availability. */
		if (controller->io_slots[index].bounce_dma.address != NULL) {
			drv_dma_free_coherent(
				dma, &controller->io_slots[index].bounce_dma);
		}
	}

	/* Handles the address availability. */
	if (controller->io_completion_dma.address != NULL)
		drv_dma_free_coherent(dma, &controller->io_completion_dma);

	/* Handles the address availability. */
	if (controller->io_submission_dma.address != NULL)
		drv_dma_free_coherent(dma, &controller->io_submission_dma);
	controller->io_submission = NULL;
	controller->io_completion = NULL;
	controller->io_slot_count = 0;
	controller->io_queue_depth = 0;
}

/* Supports the nvme io dma allocate operation. */
static int
nvme_io_dma_allocate(
	struct nvme_controller *controller)
{
	struct nvme_io_slot *slot;
	struct drv_dma_device *dma = drv_pci_device_dma(controller->pci);
	size_t submission_bytes, completion_bytes;
	unsigned index;
	int error;

	controller->io_queue_depth = drv_nvme_selected_queue_depth(
		controller->capability, NVME_IO_QUEUE_REQUESTED_DEPTH);

	/* Checks the drv nvme queue bytes result. */
	if (dma == NULL || controller->io_queue_depth < 2U ||
	    controller->io_queue_depth - 1U > NVME_IO_MAX_SLOTS ||
	    !drv_nvme_queue_bytes(controller->io_queue_depth,
				  sizeof(struct drv_nvme_command),
				  &submission_bytes) ||
	    !drv_nvme_queue_bytes(controller->io_queue_depth,
				  sizeof(struct drv_nvme_completion),
				  &completion_bytes))

		/* Returns the computed result. */
		return EINVAL;
	controller->io_slot_count = controller->io_queue_depth - 1U;
	error = drv_dma_alloc_coherent(dma, submission_bytes,
				       controller->page_size,
				       &controller->io_submission_dma);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;
	error = drv_dma_alloc_coherent(dma, completion_bytes,
				       controller->page_size,
				       &controller->io_completion_dma);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;
	/* Process each remaining element. */
	for (index = 0; index < controller->io_slot_count; index++) {
		error = drv_dma_alloc_coherent(
			dma, NVME_IO_BOUNCE_SIZE, controller->page_size,
			&controller->io_slots[index].bounce_dma);

		/* Checks the operation status. */
		if (error != 0)
			goto fail;

		/* Handles the controller condition. */
		if ((controller->io_slots[index].bounce_dma.device_address &
		     (controller->page_size - 1U)) != 0U ||
		    controller->io_slots[index].bounce_dma.size <
			    NVME_IO_BOUNCE_SIZE) {
			error = EIO;
			goto fail;
		}
	}

	/* Handles the controller condition. */
	if ((controller->io_submission_dma.device_address &
	     (controller->page_size - 1U)) != 0U ||
	    (controller->io_completion_dma.device_address &
	     (controller->page_size - 1U)) != 0U) {
		error = EIO;
		goto fail;
	}
	memset(controller->io_submission_dma.address, 0,
	       controller->io_submission_dma.size);
	memset(controller->io_completion_dma.address, 0,
	       controller->io_completion_dma.size);
	controller->io_submission = controller->io_submission_dma.address;
	controller->io_completion = controller->io_completion_dma.address;
	controller->io_submission_tail = 0;
	controller->io_epoch = 1U;

	/* Checks the drv nvme completion cursor init result. */
	if (!drv_nvme_completion_cursor_init(&controller->io_completion_cursor,
					     controller->io_queue_depth)) {
		error = EIO;
		goto fail;
	}
	/* Process each remaining element. */
	for (index = 0; index < controller->io_slot_count; index++) {
		slot = &controller->io_slots[index];

		/* Puts every slot in the free state of the current epoch. */
		drv_nvme_io_lifecycle_init(&slot->lifecycle);
		waitq_init(&slot->waitq, "NVMe I/O completion");
		slot->state = NVME_IO_SLOT_FREE;
		slot->posted = 0;
		slot->epoch = controller->io_epoch;
	}

	/* Reports successful completion. */
	return 0;

fail:
	nvme_io_dma_free_unpublished(controller);

	/* Returns the computed result. */
	return error;
}

static int nvme_io_queue_memory_reset(struct nvme_controller *controller);

/* Supports the nvme io queue memory reset operation. */
static int
nvme_io_queue_memory_reset(
	struct nvme_controller *controller)
{
	struct nvme_io_slot *slot;
	unsigned long irq;
	unsigned index;

	/* Handles the io submission availability. */
	if (controller->io_submission == NULL ||
	    controller->io_completion == NULL)

		/* Returns the computed result. */
		return EINVAL;
	irq = spin_lock_irqsave(&controller->command_lock);

	/* Handles the controller condition. */
	if (controller->io_owned != 0U || controller->io_pending != 0U) {
		spin_unlock_irqrestore(&controller->command_lock, irq);

		/* Returns the computed result. */
		return EBUSY;
	}
	controller->io_queue_ready = 0;
	spin_unlock_irqrestore(&controller->command_lock, irq);
	memset(controller->io_submission_dma.address, 0,
	       controller->io_submission_dma.size);
	memset(controller->io_completion_dma.address, 0,
	       controller->io_completion_dma.size);
	irq = spin_lock_irqsave(&controller->command_lock);
	controller->io_submission_tail = 0;
	controller->io_epoch++;

	/* Handles the controller condition. */
	if (controller->io_epoch == 0U)
		controller->io_epoch++;

	/* Checks the drv nvme completion cursor init result. */
	if (!drv_nvme_completion_cursor_init(&controller->io_completion_cursor,
					     controller->io_queue_depth)) {
		spin_unlock_irqrestore(&controller->command_lock, irq);

		/* Returns the computed result. */
		return EIO;
	}
	/* Process each remaining element. */
	for (index = 0; index < controller->io_slot_count; index++) {
		slot = &controller->io_slots[index];

		/* Returns every slot to the free state of the current epoch. */
		slot->state = NVME_IO_SLOT_FREE;
		slot->posted = 0;
		slot->command_id = 0;
		slot->error = 0;
		slot->epoch = controller->io_epoch;
	}
	spin_unlock_irqrestore(&controller->command_lock, irq);

	/* Reports successful completion. */
	return 0;
}

/* Supports the nvme io queue create operation. */
static int
nvme_io_queue_create(
	struct nvme_controller *controller,
	int recovery_command)
{
	struct drv_nvme_command command;
	uint32_t result;
	unsigned submission_count, completion_count;
	unsigned long irq;
	unsigned index;
	int error;

	/* Checks the drv nvme set queue count command result. */
	if (!drv_nvme_set_queue_count_command(&command, 0, 1U, 1U))
		return EINVAL;
	error = nvme_admin_execute_mode(controller, &command, &result,
					recovery_command);

	/* Checks the operation status. */
	if (error != 0 || !drv_nvme_queue_count_result(
				  result, &submission_count, &completion_count))

		/* Returns the computed result. */
		return error != 0 ? error : EIO;

	/* Handles the submission count condition. */
	if (submission_count < 1U || completion_count < 1U)
		return ENOSPC;

	/* Checks the drv nvme create io cq command result. */
	if (!drv_nvme_create_io_cq_command(
		    &command, 0, NVME_IO_QUEUE_ID, controller->io_queue_depth,
		    controller->io_completion_dma.device_address, 0U, 1))

		/* Returns the computed result. */
		return EINVAL;
	error = nvme_admin_execute_mode(controller, &command, NULL,
					recovery_command);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Checks the drv nvme create io sq command result. */
	if (!drv_nvme_create_io_sq_command(
		    &command, 0, NVME_IO_QUEUE_ID, NVME_IO_QUEUE_ID,
		    controller->io_queue_depth,
		    controller->io_submission_dma.device_address))

		/* Returns the computed result. */
		return EINVAL;
	error = nvme_admin_execute_mode(controller, &command, NULL,
					recovery_command);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	irq = spin_lock_irqsave(&controller->command_lock);
	/* Process each remaining element. */
	for (index = 0; index < controller->io_slot_count; index++) {
		error = drv_nvme_io_lifecycle_online(
			&controller->io_slots[index].lifecycle);

		/* Checks the operation status. */
		if (error != 0)
			break;
	}

	/* Checks the operation status. */
	if (error == 0) {
		controller->io_queue_ready = 1;
		controller->io_fault = 0;
		controller->io_recovery_needed = 0;
	}
	waitq_wake_all(&controller->io_state_waitq);
	spin_unlock_irqrestore(&controller->command_lock, irq);

	/* Returns the computed result. */
	return error;
}

/* Supports the nvme controller enable operation. */
static int
nvme_controller_enable(
	struct nvme_controller *controller)
{
	uint32_t configuration;
	int error;

	/* Points the controller at the admin queues. */
	nvme_write32(controller, DRV_NVME_REG_AQA,
		     drv_nvme_admin_queue_attributes(controller->queue_depth));
	nvme_write64(controller, DRV_NVME_REG_ASQ,
		     controller->admin_submission_dma.device_address);
	nvme_write64(controller, DRV_NVME_REG_ACQ,
		     controller->admin_completion_dma.device_address);
	configuration =
		drv_nvme_controller_configuration(controller->page_size, 1);

	/* Handles the configuration condition. */
	if (configuration == 0U)
		return EINVAL;
	error = drv_pci_device_set_bus_master(controller->pci, true);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	controller->controller_owned = 1;
	nvme_write32(controller, DRV_NVME_REG_CC, configuration);
	controller->controller_enabled = 1;
	error = nvme_wait_ready(controller, 1);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the controller condition. */
	if (controller->irq.type != DRV_PCI_IRQ_MSIX)
		nvme_write32(controller, DRV_NVME_REG_INTMC, 1U);

	/* Reports successful completion. */
	return 0;
}

static int nvme_admin_queue_memory_reset(struct nvme_controller *controller);

/* Supports the nvme admin queue memory reset operation. */
static int
nvme_admin_queue_memory_reset(
	struct nvme_controller *controller)
{
	unsigned long irq;

	irq = spin_lock_irqsave(&controller->command_lock);

	/* Handles the controller condition. */
	if (controller->command_pending) {
		spin_unlock_irqrestore(&controller->command_lock, irq);

		/* Returns the computed result. */
		return EBUSY;
	}
	spin_unlock_irqrestore(&controller->command_lock, irq);
	memset(controller->admin_submission_dma.address, 0,
	       controller->admin_submission_dma.size);
	memset(controller->admin_completion_dma.address, 0,
	       controller->admin_completion_dma.size);
	irq = spin_lock_irqsave(&controller->command_lock);
	controller->submission_tail = 0;
	controller->command_pending = 0;
	controller->command_completed = 0;
	controller->command_error = 0;
	controller->command_result = 0;
	controller->admin_fault = 0;

	/* Checks the drv nvme completion cursor init result. */
	if (!drv_nvme_completion_cursor_init(&controller->completion_cursor,
					     controller->queue_depth)) {
		spin_unlock_irqrestore(&controller->command_lock, irq);

		/* Returns the computed result. */
		return EIO;
	}
	spin_unlock_irqrestore(&controller->command_lock, irq);

	/* Reports successful completion. */
	return 0;
}

/* Supports the nvme io lifecycles quiesce operation. */
static int
nvme_io_lifecycles_quiesce(
	struct nvme_controller *controller)
{
	struct nvme_io_slot *slot;
	int lifecycle_error;
	unsigned long irq = spin_lock_irqsave(&controller->command_lock);
	unsigned index;
	int error = 0;

	controller->io_queue_ready = 0;
	/* Process each remaining element. */
	for (index = 0; index < controller->io_slot_count; index++) {
		slot = &controller->io_slots[index];

		(void)drv_nvme_io_lifecycle_stop(&slot->lifecycle);

		/*
 * Every caller reaches this point only after a fresh controller
		 * or bus-master stop and an IRQ drain.  That fresh proof may
		 * resolve an older DMA-unsafe quarantine; the historical flag
		 * itself must not make safely owned DMA impossible to release
		 * forever. */
		if (slot->lifecycle.quarantined) {
			lifecycle_error =
				drv_nvme_io_lifecycle_resolve_quarantine(
					&slot->lifecycle, 1, 1);
		} else {
			lifecycle_error = drv_nvme_io_lifecycle_quiesced(
				&slot->lifecycle);
		}

		/* Checks the operation status. */
		if (lifecycle_error != 0)
			error = EIO;
		slot->posted = 0;
	}
	spin_unlock_irqrestore(&controller->command_lock, irq);

	/* Returns the computed result. */
	return error;
}

static void nvme_io_quarantine(struct nvme_controller *controller, int error, int dma_unsafe);

/* Supports the nvme io quarantine operation. */
static void
nvme_io_quarantine(
	struct nvme_controller *controller,
	int error,
	int dma_unsafe)
{
	unsigned long irq = spin_lock_irqsave(&controller->command_lock);
	unsigned index;

	/* Marks the I/O side faulted and closed to new work. */
	controller->io_queue_ready = 0;
	controller->io_fault = error != 0 ? error : EIO;
	controller->io_recovery_needed = 1;
	controller->io_recovery_busy = 0;
	controller->stopping = 1;
	controller->quarantined = 1;

	/* Handles the dma unsafe condition. */
	if (dma_unsafe) {
		/* Process each remaining element. */
		for (index = 0; index < controller->io_slot_count; index++) {
			(void)drv_nvme_io_lifecycle_quarantine(
				&controller->io_slots[index].lifecycle);
		}
	}
	waitq_wake_all(&controller->io_state_waitq);
	spin_unlock_irqrestore(&controller->command_lock, irq);
}

/* Supports the nvme io recover operation. */
static int
nvme_io_recover(
	struct nvme_controller *controller)
{
	int disable_error;
	int master_error;
	int drain_error;
	unsigned index_for;
	uint64_t deadline = sched_ticks() + controller->timeout_ticks;
	unsigned irq_capability = 0;
	uint16_t irq_control = 0;
	unsigned long irq;
	int irq_masked = 0;
	int quiesced = 0;
	int error;

	irq = spin_lock_irqsave(&controller->command_lock);

	/* Handles the controller condition. */
	if (!controller->io_recovery_busy || !controller->io_recovery_needed ||
	    controller->detach_busy) {
		controller->io_recovery_busy = 0;
		spin_unlock_irqrestore(&controller->command_lock, irq);

		/* Returns the computed result. */
		return controller->detach_busy ? ENXIO : EINVAL;
	}
	controller->stopping = 1;
	/* Process each remaining element. */
	for (index_for = 0; index_for < controller->io_slot_count;
	     index_for++) {
		(void)drv_nvme_io_lifecycle_stop(
			&controller->io_slots[index_for].lifecycle);
	}
	/* Continue while the operation condition remains true. */
	while (controller->io_owned != 0U) {
		error = nvme_io_wait_locked(controller,
					    &controller->io_state_waitq,
					    deadline, &irq);

		/* Checks the operation status. */
		if (error != 0) {
			spin_unlock_irqrestore(&controller->command_lock, irq);
			nvme_io_quarantine(controller, error, 1);

			/* Returns the computed result. */
			return error;
		}
	}
	spin_unlock_irqrestore(&controller->command_lock, irq);

	error = nvme_runtime_irq_mask(controller, &irq_capability,
				      &irq_control);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;
	irq_masked = 1;
	error = nvme_controller_disable(controller);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;
	error = nvme_bus_master_disable(controller);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;
	error = nvme_irq_drain(controller);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;
	quiesced = 1;
	error = nvme_io_lifecycles_quiesce(controller);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;
	error = nvme_admin_queue_memory_reset(controller);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;
	error = nvme_io_queue_memory_reset(controller);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;

	/* Bus mastering below invalidates the earlier quiescence proof. */
	quiesced = 0;
	error = nvme_controller_enable(controller);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;
	error = nvme_runtime_irq_restore(controller, irq_capability,
					 irq_control);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;
	irq_masked = 0;
	error = nvme_io_queue_create(controller, 1);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;
	irq = spin_lock_irqsave(&controller->command_lock);
	controller->io_recovery_busy = 0;
	controller->io_recovery_needed = 0;
	controller->io_fault = 0;

	/* Handles the controller condition. */
	if (!controller->detach_busy)
		controller->stopping = 0;
	waitq_wake_all(&controller->io_state_waitq);
	spin_unlock_irqrestore(&controller->command_lock, irq);
	hal_printf("nvme: I/O queue recovered epoch=%u\n",
		   controller->io_epoch);

	/* Reports successful completion. */
	return 0;

fail:

	/* Checks the nvme runtime irq mask result. */
	if (!irq_masked && controller->controller_enabled &&
	    nvme_runtime_irq_mask(controller, &irq_capability, &irq_control) ==
		    0)
		irq_masked = 1;

	/* Handles the controller condition. */
	if (controller->controller_owned) {
		disable_error = nvme_controller_disable(controller);
		master_error = nvme_bus_master_disable(controller);
		drain_error = nvme_irq_drain(controller);

		/* Checks the operation status. */
		if (disable_error == 0 && master_error == 0 && drain_error == 0)
			quiesced = 1;
		else
			quiesced = 0;
	}

	/* Handles the quiesced condition. */
	if (quiesced)
		(void)nvme_io_lifecycles_quiesce(controller);
	nvme_io_quarantine(controller, error, !quiesced);
	hal_printf("nvme: I/O recovery failed (%d); disk unavailable, DMA "
		   "retained\n",
		   error);

	/* Returns the computed result. */
	return error;
}

static int nvme_attach(struct drv_pci_device *device, const struct drv_pci_id *id);

/* Supports the nvme attach operation. */
static int
nvme_attach(
	struct drv_pci_device *device,
	const struct drv_pci_id *id)
{
	struct drv_nvme_capability_snapshot snapshot;
	struct nvme_controller *controller;
	struct drv_pci_address address;
	struct drv_pci_bar mapped_bar;
	const char *stage = "allocation";
	const char *irq_name;
	uint32_t reasons;
	uint16_t command;
	unsigned irq_count = 0;
	unsigned long registry_irq;
	int cleanup_error;
	int error;

	(void)id;
	registry_irq = spin_lock_irqsave(&nvme_registry_lock);

	/* Handles the nvme primary availability. */
	if (nvme_primary != NULL) {
		spin_unlock_irqrestore(&nvme_registry_lock, registry_irq);
		hal_printf("nvme: additional controller rejected by initial "
			   "profile\n");

		/* Returns the computed result. */
		return EBUSY;
	}
	spin_unlock_irqrestore(&nvme_registry_lock, registry_irq);
	controller = hal_malloc(sizeof(*controller));

	/* Handles the controller availability. */
	if (controller == NULL)
		return ENOMEM;
	memset(controller, 0, sizeof(*controller));
	drv_nvme_lifecycle_init(&controller->lifecycle);
	drv_nvme_detach_flush_init(&controller->detach_flush);
	drv_nvme_shutdown_lifecycle_init(&controller->shutdown_lifecycle);
	controller->pci = device;
	controller->page_size = ZEDBSD_PAGE_SIZE;
	spin_init(&controller->command_lock, LOCK_RANK_DEVICE,
		  "NVMe command state");
	waitq_init(&controller->io_state_waitq, "NVMe I/O state");
	drv_pci_device_address(device, &address);

	stage = "BAR0 claim";
	error = drv_pci_device_claim_bar(device, 0);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;
	controller->bar_claimed = 1;

	/* Checks the drv nvme lifecycle record result. */
	if (drv_nvme_lifecycle_record(&controller->lifecycle,
				      DRV_NVME_LIFECYCLE_BAR_CLAIMED) != 0) {
		error = EIO;
		goto fail;
	}
	stage = "BAR0 inspection";
	error = drv_pci_device_bar(device, 0, &controller->original_bar);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;
	controller->original_bar_valid = 1;

	/* Checks the drv nvme lifecycle record result. */
	if (drv_nvme_lifecycle_record(&controller->lifecycle,
				      DRV_NVME_LIFECYCLE_BAR_SNAPSHOTTED) !=
	    0) {
		error = EIO;
		goto fail;
	}

	/* Handles the controller condition. */
	if (controller->original_bar.type != DRV_PCI_BAR_MEMORY32 &&
	    controller->original_bar.type != DRV_PCI_BAR_MEMORY64) {
		error = ENODEV;
		goto fail;
	}
	stage = "PCI command save";
	error = drv_pci_device_save_enable_state(device,
						 &controller->pci_enable_state);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;
	controller->pci_state_saved = 1;
	controller->restore_allowed = 1;

	/* Checks the drv nvme lifecycle record result. */
	if (drv_nvme_lifecycle_record(&controller->lifecycle,
				      DRV_NVME_LIFECYCLE_PCI_STATE_SAVED) !=
	    0) {
		error = EIO;
		goto fail;
	}
	stage = "PCI quiesce";

	/* Checks the drv nvme lifecycle record result. */
	if (drv_nvme_lifecycle_record(&controller->lifecycle,
				      DRV_NVME_LIFECYCLE_PCI_COMMAND_CHANGED) !=
	    0) {
		error = EIO;
		goto fail;
	}
	stage = "message interrupt quiesce";
	error = nvme_message_irq_save_and_mask(controller);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;
	stage = "PCI quiesce";
	error = nvme_pci_quiesce(controller);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;
	stage = "BAR0 map";
	error = drv_pci_device_map_bar(device, 0,
				       DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE |
					       DRV_PCI_MAP_NOCACHE,
				       &controller->mapping);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;
	controller->bar_mapped = 1;

	/* Checks the drv nvme lifecycle record result. */
	if (drv_nvme_lifecycle_record(&controller->lifecycle,
				      DRV_NVME_LIFECYCLE_BAR_MAPPED) != 0) {
		error = EIO;
		goto fail;
	}
	stage = "BAR0 readback";
	error = drv_pci_device_bar(device, 0, &mapped_bar);

	/* Checks the operation status. */
	if (error != 0 ||
	    (mapped_bar.type != DRV_PCI_BAR_MEMORY32 &&
	     mapped_bar.type != DRV_PCI_BAR_MEMORY64) ||
	    controller->mapping.address == NULL ||
	    controller->mapping.size > mapped_bar.size) {
		error = EIO;
		goto fail;
	}

	/* Handles the controller condition. */
	if (controller->mapping.size < DRV_NVME_REG_VS + sizeof(uint32_t)) {
		error = ENODEV;
		goto fail;
	}
	stage = "PCI memory enable";
	error = drv_pci_device_enable_memory(device);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;
	error = drv_pci_device_config_read16(device, NVME_PCI_COMMAND,
					     &command);

	/* Checks the operation status. */
	if (error != 0 || (command & NVME_PCI_COMMAND_MEMORY) == 0U ||
	    (command & NVME_PCI_COMMAND_MASTER) != 0U) {
		error = EIO;
		goto fail;
	}
	controller->registers = controller->mapping.address;
	stage = "capabilities";
	controller->capability = nvme_read64(controller, DRV_NVME_REG_CAP);
	controller->version = nvme_read32(controller, DRV_NVME_REG_VS);
	memset(&snapshot, 0, sizeof(snapshot));
	snapshot.mapping_size = controller->mapping.size;
	snapshot.capability = controller->capability;
	snapshot.version = controller->version;
	snapshot.page_size = controller->page_size;
	snapshot.requested_queue_entries = NVME_ADMIN_QUEUE_REQUESTED_DEPTH;
	snapshot.maximum_queue_id = NVME_IO_QUEUE_ID;
	reasons = drv_nvme_capability_validate(&snapshot);

	/* Handles the reasons condition. */
	if (reasons != 0U) {
		hal_printf("nvme: pci %04x:%02x:%02x.%u capability rejected "
			   "%08x:%s\n",
			   address.segment, address.bus, address.device,
			   address.function, reasons,
			   drv_nvme_capability_reason_name(reasons));
		error = ENODEV;
		goto fail;
	}
	controller->queue_depth = drv_nvme_selected_queue_depth(
		controller->capability, NVME_ADMIN_QUEUE_REQUESTED_DEPTH);
	controller->timeout_ms =
		drv_nvme_cap_timeout_ms(controller->capability);

	/* Handles the controller condition. */
	if (controller->timeout_ms > NVME_TIMEOUT_POLICY_MAX_MS)
		controller->timeout_ms = NVME_TIMEOUT_POLICY_MAX_MS;
	controller->timeout_ticks = nvme_timeout_ticks(controller->timeout_ms);

	/* Checks the drv nvme doorbell offset result. */
	if (!drv_nvme_doorbell_offset(controller->capability, 0, 0,
				      controller->mapping.size,
				      &controller->submission_doorbell) ||
	    !drv_nvme_doorbell_offset(controller->capability, 0, 1,
				      controller->mapping.size,
				      &controller->completion_doorbell) ||
	    !drv_nvme_doorbell_offset(controller->capability, NVME_IO_QUEUE_ID,
				      0, controller->mapping.size,
				      &controller->io_submission_doorbell) ||
	    !drv_nvme_doorbell_offset(controller->capability, NVME_IO_QUEUE_ID,
				      1, controller->mapping.size,
				      &controller->io_completion_doorbell) ||
	    !drv_nvme_completion_cursor_init(&controller->completion_cursor,
					     controller->queue_depth)) {
		error = EIO;
		goto fail;
	}
	stage = "controller reset";

	/*
 * From this point cleanup must prove CSTS.RDY clear before releasing
	 * BAR or DMA ownership.  A failed reset is quarantined, not
	 * half-detached. */
	controller->controller_owned = 1;

	/* Checks the drv nvme lifecycle record result. */
	if (drv_nvme_lifecycle_record(&controller->lifecycle,
				      DRV_NVME_LIFECYCLE_CONTROLLER_CLAIMED) !=
	    0) {
		error = EIO;
		goto fail;
	}
	nvme_write32(controller, DRV_NVME_REG_INTMS, UINT32_MAX);
	error = nvme_controller_disable(controller);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;
	stage = "admin DMA";
	error = nvme_dma_allocate(controller);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;

	/* Checks the drv nvme lifecycle record result. */
	if (drv_nvme_lifecycle_record(&controller->lifecycle,
				      DRV_NVME_LIFECYCLE_DMA_ALLOCATED) != 0) {
		error = EIO;
		goto fail;
	}
	stage = "IRQ allocation";
	error = drv_pci_device_allocate_irqs(
		device, DRV_PCI_IRQ_ALLOW_MSIX | DRV_PCI_IRQ_ALLOW_MSI, 1, 1,
		&controller->irq, &irq_count);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;

	/* Handles the irq count condition. */
	if (irq_count != 1U) {
		/* Handles the irq count condition. */
		if (irq_count != 0U) {
			controller->irq_allocated = 1;
			(void)drv_nvme_lifecycle_record(
				&controller->lifecycle,
				DRV_NVME_LIFECYCLE_IRQ_ALLOCATED);
		}
		error = EIO;
		goto fail;
	}
	controller->irq_allocated = 1;

	/* Checks the drv nvme lifecycle record result. */
	if (drv_nvme_lifecycle_record(&controller->lifecycle,
				      DRV_NVME_LIFECYCLE_IRQ_ALLOCATED) != 0) {
		error = EIO;
		goto fail;
	}

	/*
 * The initial reset masked all legacy/MSI sources.  Clear that mask
	 * before enabling MSI-X; INTMC must not be touched afterwards. */
	if (controller->irq.type == DRV_PCI_IRQ_MSIX)
		nvme_write32(controller, DRV_NVME_REG_INTMC, UINT32_MAX);
	stage = "IRQ establishment";
	error = drv_pci_device_establish_irq(device, &controller->irq, nvme_irq,
					     controller, "nvme",
					     &controller->irq_cookie);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;

	/* Checks the drv nvme lifecycle record result. */
	if (drv_nvme_lifecycle_record(&controller->lifecycle,
				      DRV_NVME_LIFECYCLE_IRQ_ESTABLISHED) !=
	    0) {
		error = EIO;
		goto fail;
	}
	stage = "controller enable";
	error = nvme_controller_enable(controller);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;

	/* Checks the drv nvme lifecycle record result. */
	if (drv_nvme_lifecycle_record(&controller->lifecycle,
				      DRV_NVME_LIFECYCLE_CONTROLLER_ENABLED) !=
	    0) {
		error = EIO;
		goto fail;
	}
	controller->stopping = 0;
	nvme_publish_controller(controller);
	irq_name = controller->irq.type == DRV_PCI_IRQ_MSIX ? "MSI-X" : "MSI";
	hal_printf("nvme: PCI controller %04x:%02x:%02x.%u version=%x queue=%u "
		   "timeout=%ums %s\n",
		   address.segment, address.bus, address.device,
		   address.function, controller->version,
		   controller->queue_depth, controller->timeout_ms, irq_name);

	/* Reports successful completion. */
	return 0;

fail:
	cleanup_error = nvme_cleanup(controller);

	/* Checks the operation status. */
	if (cleanup_error != 0) {
		controller->quarantined = 1;
		nvme_publish_controller(controller);
		hal_printf("nvme: attach failed at %s (%d), cleanup failed "
			   "(%d); quarantined\n",
			   stage, error, cleanup_error);

		/* Reports successful completion. */
		return 0;
	}
	hal_printf("nvme: attach failed at %s (%d)\n", stage, error);
	hal_free(controller);

	/* Returns the computed result. */
	return error;
}

static int nvme_detach_owned(struct nvme_controller *controller);

/* Supports the nvme detach owned operation. */
static int
nvme_detach_owned(
	struct nvme_controller *controller)
{
	struct drv_pci_device *device = controller->pci;
	unsigned long irq;
	int can_flush;
	int newly_gone;
	int flush_error;
	int error;

	/* Handles the namespace disk availability. */
	if (controller->namespace_disk != NULL) {
		error = disk_gone_if_idle(controller->namespace_disk);
		newly_gone = error == 0;

		/* Checks the operation status. */
		if (error != 0 && error != ENXIO) {
			nvme_detach_release(controller, 1);

			/* Returns the computed result. */
			return error;
		}

		/* Checks the drv nvme detach flush require result. */
		if (newly_gone && drv_nvme_detach_flush_require(
					  &controller->detach_flush) != 0) {
			nvme_detach_release(controller, 0);

			/* Returns the computed result. */
			return EIO;
		}

		/*
 * disk_gone_if_idle writes dirty buffers but does not issue the
		 * device-cache FLUSH.  Once required, that durability
		 * obligation survives a failed detach and an already-gone
		 * result on retry. */
		irq = spin_lock_irqsave(&controller->command_lock);
		can_flush = controller->io_queue_ready &&
			    !controller->quarantined &&
			    !controller->io_recovery_needed;

		/* Handles the controller condition. */
		if (controller->detach_flush.required && can_flush)

			/*
 * The namespace is GONE and detach owns the
			 * transaction, so this admits only the driver's
			 * internal retry. */
			controller->stopping = 0;
		flush_error = drv_nvme_detach_flush_begin(
			&controller->detach_flush, can_flush);
		spin_unlock_irqrestore(&controller->command_lock, irq);

		/* Checks the operation status. */
		if (flush_error == 0) {
			error = nvme_io_flush_internal(controller);
			irq = spin_lock_irqsave(&controller->command_lock);
			flush_error = drv_nvme_detach_flush_finish(
				&controller->detach_flush, error);

			/* Checks the operation status. */
			if (flush_error != 0)
				controller->stopping = 1;
			waitq_wake_all(&controller->io_state_waitq);
			spin_unlock_irqrestore(&controller->command_lock, irq);
		}

		/* Checks the operation status. */
		if (flush_error != 0 && flush_error != EALREADY) {
			irq = spin_lock_irqsave(&controller->command_lock);
			controller->stopping = 1;

			/* Checks the operation status. */
			if (flush_error == ENXIO && controller->io_fault != 0)
				flush_error = controller->io_fault;
			waitq_wake_all(&controller->io_state_waitq);
			spin_unlock_irqrestore(&controller->command_lock, irq);
			nvme_detach_release(controller, 0);
			hal_printf("nvme: final detach FLUSH incomplete (%d); "
				   "disk gone, resources retained\n",
				   flush_error);

			/* Returns the computed result. */
			return flush_error;
		}
	}
	error = nvme_cleanup(controller);

	/* Checks the operation status. */
	if (error != 0) {
		controller->quarantined = 1;
		nvme_detach_release(controller, 0);

		/* Returns the computed result. */
		return error;
	}

	/* Handles the namespace disk availability. */
	if (controller->namespace_disk != NULL) {
		error = disk_destroy(controller->namespace_disk);

		/* Checks the operation status. */
		if (error != 0) {
			controller->quarantined = 1;
			nvme_detach_release(controller, 0);

			/* Returns the computed result. */
			return error;
		}
		controller->namespace_disk = NULL;
	}
	nvme_unpublish_controller(controller);
	(void)drv_pci_device_set_driver_data(device, NULL);
	hal_free(controller);

	/* Reports successful completion. */
	return 0;
}

static int nvme_detach(struct drv_pci_device *device, unsigned flags);

/* Supports the nvme detach operation. */
static int
nvme_detach(
	struct drv_pci_device *device,
	unsigned flags)
{
	int function_result;
	struct nvme_controller *controller;
	int error;

	(void)flags;
	error = nvme_detach_claim(device, &controller);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Obtains the nvme detach owned result. */
	function_result = nvme_detach_owned(controller);

	/* Returns the computed result. */
	return function_result;
}

static void nvme_shutdown(struct drv_pci_device *device);

/* Supports the nvme shutdown operation. */
static void
nvme_shutdown(
	struct drv_pci_device *device)
{
	struct nvme_controller *controller;
	unsigned irq_capability = 0;
	uint16_t irq_control = 0;
	int drain_error = 0;
	int lifecycle_error;
	int mask_error = 0;
	int quiesce_error = 0;
	int error;

	error = nvme_shutdown_claim(device, &controller);

	/* Checks the operation status. */
	if (error != 0)
		return;

	/*
 * A historical quarantine is not a reason to skip terminal DMA safety.
	 * Mask delivery when the allocation still exists, but continue even if
	 * PCI capability access itself is damaged. */
	if (controller->irq_allocated) {
		mask_error = nvme_runtime_irq_mask(controller, &irq_capability,
						   &irq_control);
	}
	lifecycle_error = drv_nvme_shutdown_lifecycle_run(
		&controller->shutdown_lifecycle, &nvme_shutdown_operations,
		controller);

	/* Handles the irq cookie availability. */
	if (controller->irq_cookie != NULL ||
	    controller->lifecycle.irq_may_be_busy)
		drain_error = nvme_irq_drain(controller);

	/* Checks the operation status. */
	if (controller->shutdown_lifecycle.hardware_dma_safe &&
	    drain_error == 0 && controller->io_slot_count != 0U)
		quiesce_error = nvme_io_lifecycles_quiesce(controller);

	/* Checks the operation status. */
	if (mask_error != 0 || lifecycle_error != 0 || drain_error != 0 ||
	    quiesce_error != 0) {
		controller->quarantined = 1;
		hal_printf("nvme: shutdown retained resources mask=%d "
			   "lifecycle=%d drain=%d quiesce=%d dma-safe=%u\n",
			   mask_error, lifecycle_error, drain_error,
			   quiesce_error,
			   controller->shutdown_lifecycle.hardware_dma_safe);
	}
	nvme_detach_release(controller, 0);
}

static const struct drv_pci_id nvme_ids[] = {
	{
		.vendor = DRV_PCI_ANY_ID,
		.device = DRV_PCI_ANY_ID,
		.subvendor = DRV_PCI_ANY_ID,
		.subdevice = DRV_PCI_ANY_ID,
		.class_code = DRV_NVME_PCI_CLASS,
		.class_mask = 0xffffffU,
	},
};

static struct drv_pci_driver nvme_driver = {
	.name = "nvme",
	.ids = nvme_ids,
	.id_count = sizeof(nvme_ids) / sizeof(nvme_ids[0]),
	.attach = nvme_attach,
	.detach = nvme_detach,
	.shutdown = nvme_shutdown,
};

/*
 * Implements the drv pci nvme driver register operation.
 */
int
drv_pci_nvme_driver_register(
	void)
{
	int function_result;

	spin_init(&nvme_registry_lock, LOCK_RANK_DEVICE,
		  "NVMe controller registry");

	/* Obtains the drv pci driver register result. */
	function_result = drv_pci_driver_register(&nvme_driver);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the drv pci nvme probe namespaces operation.
 */
void
drv_pci_nvme_probe_namespaces(
	void)
{
	struct nvme_controller *controller;
	unsigned long irq, registry_irq;
	int owns_detach = 0;
	int error;

	registry_irq = spin_lock_irqsave(&nvme_registry_lock);
	controller = nvme_primary;

	/* Handles the controller availability. */
	if (controller == NULL) {
		spin_unlock_irqrestore(&nvme_registry_lock, registry_irq);

		/* Returns the computed result. */
		return;
	}
	irq = spin_lock_irqsave(&controller->command_lock);

	/* Handles the controller condition. */
	if (controller->probe_started || controller->detach_busy ||
	    controller->stopping || controller->quarantined) {
		spin_unlock_irqrestore(&controller->command_lock, irq);
		spin_unlock_irqrestore(&nvme_registry_lock, registry_irq);

		/* Returns the computed result. */
		return;
	}
	controller->probe_started = 1;
	controller->probe_busy = 1;
	spin_unlock_irqrestore(&controller->command_lock, irq);
	spin_unlock_irqrestore(&nvme_registry_lock, registry_irq);
	error = nvme_probe_namespace(controller);

	/* Checks the operation status. */
	if (error != 0)
		hal_printf("nvme: namespace probe failed (%d)\n", error);
	irq = spin_lock_irqsave(&controller->command_lock);

	/* Checks the operation status. */
	if (error != 0 && !controller->detach_busy) {
		controller->detach_busy = 1;
		controller->stopping = 1;
		owns_detach = 1;
	}
	controller->probe_busy = 0;
	spin_unlock_irqrestore(&controller->command_lock, irq);

	/* Checks the operation status. */
	if (error == 0)
		return;

	/*
 * A concurrent PCI detach already owns teardown and may free controller
	 * as soon as probe_busy clears.  Do not dereference it in that case. */
	if (!owns_detach)
		return;

	/*
 * A failed Identify probe releases every safely quiesced resource but
	 * retains the bound controller object as a terminal quarantine.  This
	 * keeps the PCI core's driver binding coherent; a later ordinary PCI
	 * detach can retry any retained release and free the object. */
	error = nvme_cleanup(controller);
	controller->quarantined = 1;
	nvme_detach_release(controller, 0);

	/* Checks the operation status. */
	if (error != 0) {
		hal_printf(
			"nvme: failed probe teardown retained resources (%d)\n",
			error);
	} else {
		hal_printf(
			"nvme: failed probe resources released; quarantined\n");
	}
}
/* End consolidated pci-nvme.c. */
