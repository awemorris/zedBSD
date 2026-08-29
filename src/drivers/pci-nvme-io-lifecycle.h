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

static inline void
drv_nvme_detach_flush_init(
	struct drv_nvme_detach_flush_lifecycle *lifecycle)
{
	if (lifecycle != NULL)
		memset(lifecycle, 0, sizeof(*lifecycle));
}

static inline int
drv_nvme_detach_flush_require(
	struct drv_nvme_detach_flush_lifecycle *lifecycle)
{
	if (lifecycle == NULL || lifecycle->attempt_active)
		return EINVAL;
	if (lifecycle->required)
		return EALREADY;
	lifecycle->required = 1;
	lifecycle->completed = 0;
	lifecycle->unavailable = 0;
	return 0;
}

static inline int
drv_nvme_detach_flush_begin(
	struct drv_nvme_detach_flush_lifecycle *lifecycle,
	int queue_available)
{
	if (lifecycle == NULL || (queue_available != 0 &&
	    queue_available != 1))
		return EINVAL;
	if (!lifecycle->required)
		return EALREADY;
	if (lifecycle->attempt_active)
		return EBUSY;
	if (!queue_available) {
		lifecycle->unavailable = 1;
		return ENXIO;
	}
	lifecycle->attempt_active = 1;
	lifecycle->attempts++;
	lifecycle->unavailable = 0;
	return 0;
}

static inline int
drv_nvme_detach_flush_finish(
	struct drv_nvme_detach_flush_lifecycle *lifecycle, int error)
{
	if (lifecycle == NULL || !lifecycle->attempt_active)
		return EINVAL;
	lifecycle->attempt_active = 0;
	if (error != 0)
		return error;
	lifecycle->required = 0;
	lifecycle->completed = 1;
	lifecycle->unavailable = 0;
	return 0;
}

static inline void
drv_nvme_io_lifecycle_init(struct drv_nvme_io_lifecycle *lifecycle)
{
	if (lifecycle != NULL)
		memset(lifecycle, 0, sizeof(*lifecycle));
}

static inline int
drv_nvme_io_lifecycle_online(struct drv_nvme_io_lifecycle *lifecycle)
{
	if (lifecycle == NULL || lifecycle->released || lifecycle->quarantined ||
	    lifecycle->queues_online || lifecycle->bio_owned ||
	    lifecycle->command_owned || lifecycle->payload_dma_active)
		return EINVAL;
	lifecycle->queues_online = 1;
	lifecycle->accepting = 1;
	lifecycle->faulted = 0;
	lifecycle->controller_quiesced = 0;
	return 0;
}

static inline int
drv_nvme_io_lifecycle_begin_bio(struct drv_nvme_io_lifecycle *lifecycle)
{
	if (lifecycle == NULL || !lifecycle->queues_online ||
	    !lifecycle->accepting)
		return ENXIO;
	if (lifecycle->bio_owned || lifecycle->command_owned)
		return EBUSY;
	lifecycle->bio_owned = 1;
	return 0;
}

static inline int
drv_nvme_io_lifecycle_submit(struct drv_nvme_io_lifecycle *lifecycle,
	uint16_t command_id, int uses_payload_dma)
{
	if (lifecycle == NULL || (uses_payload_dma != 0 &&
	    uses_payload_dma != 1))
		return EINVAL;
	if (!lifecycle->queues_online || !lifecycle->accepting)
		return ENXIO;
	if (!lifecycle->bio_owned)
		return EINVAL;
	if (lifecycle->command_owned || lifecycle->payload_dma_active)
		return EBUSY;
	lifecycle->command_id = command_id;
	lifecycle->command_owned = 1;
	lifecycle->payload_dma_active = (unsigned)uses_payload_dma;
	return 0;
}

static inline int
drv_nvme_io_lifecycle_complete_command(
	struct drv_nvme_io_lifecycle *lifecycle, uint16_t command_id)
{
	if (lifecycle == NULL)
		return EINVAL;
	if (!lifecycle->command_owned)
		return ENOENT;
	if (lifecycle->command_id != command_id) {
		lifecycle->accepting = 0;
		lifecycle->faulted = 1;
		return EIO;
	}
	lifecycle->command_owned = 0;
	lifecycle->payload_dma_active = 0;
	lifecycle->command_completion_count++;
	return 0;
}

static inline int
drv_nvme_io_lifecycle_complete_bio(struct drv_nvme_io_lifecycle *lifecycle)
{
	if (lifecycle == NULL)
		return EINVAL;
	if (!lifecycle->bio_owned)
		return EALREADY;
	/* Normal completion waits for the command.  A fault may publish a BIO
	 * error early because hardware DMA is isolated to the bounce buffer. */
	if (lifecycle->command_owned && !lifecycle->faulted)
		return EBUSY;
	lifecycle->bio_owned = 0;
	lifecycle->bio_completion_count++;
	return 0;
}

static inline int
drv_nvme_io_lifecycle_stop(struct drv_nvme_io_lifecycle *lifecycle)
{
	if (lifecycle == NULL || lifecycle->released)
		return EINVAL;
	lifecycle->accepting = 0;
	return lifecycle->bio_owned || lifecycle->command_owned ? EBUSY : 0;
}

static inline int
drv_nvme_io_lifecycle_fault(struct drv_nvme_io_lifecycle *lifecycle)
{
	if (lifecycle == NULL || lifecycle->released)
		return EINVAL;
	lifecycle->accepting = 0;
	lifecycle->faulted = 1;
	return 0;
}

/* Record this only after CC.EN/CSTS.RDY and bus mastering prove that the
 * controller can no longer access queue or payload DMA. */
static inline int
drv_nvme_io_lifecycle_quiesced(struct drv_nvme_io_lifecycle *lifecycle)
{
	if (lifecycle == NULL || lifecycle->released || lifecycle->accepting)
		return EINVAL;
	lifecycle->queues_online = 0;
	lifecycle->command_owned = 0;
	lifecycle->payload_dma_active = 0;
	lifecycle->controller_quiesced = 1;
	return 0;
}

static inline int
drv_nvme_io_lifecycle_quarantine(struct drv_nvme_io_lifecycle *lifecycle)
{
	if (lifecycle == NULL || lifecycle->released)
		return EINVAL;
	lifecycle->accepting = 0;
	lifecycle->faulted = 1;
	lifecycle->quarantined = 1;
	return 0;
}

/*
 * Quarantine records that an earlier teardown could not prove DMA safety; it
 * must not become an irrevocable resource leak.  A later cleanup may resolve
 * it only after independently proving both controller/bus-master quiescence
 * and IRQ quiescence.  Software BIO ownership must already have retired.
 */
static inline int
drv_nvme_io_lifecycle_resolve_quarantine(
	struct drv_nvme_io_lifecycle *lifecycle,
	int hardware_quiesced, int irq_quiesced)
{
	if (lifecycle == NULL || (hardware_quiesced != 0 &&
	    hardware_quiesced != 1) || (irq_quiesced != 0 && irq_quiesced != 1))
		return EINVAL;
	if (lifecycle->released)
		return EALREADY;
	if (!lifecycle->quarantined)
		return lifecycle->controller_quiesced ? EALREADY : EINVAL;
	if (!hardware_quiesced || !irq_quiesced || lifecycle->accepting ||
	    lifecycle->bio_owned)
		return EBUSY;
	lifecycle->queues_online = 0;
	lifecycle->command_owned = 0;
	lifecycle->payload_dma_active = 0;
	lifecycle->controller_quiesced = 1;
	lifecycle->quarantined = 0;
	return 0;
}

static inline int
drv_nvme_io_lifecycle_release(struct drv_nvme_io_lifecycle *lifecycle)
{
	if (lifecycle == NULL)
		return EINVAL;
	if (lifecycle->released)
		return EALREADY;
	if (!lifecycle->controller_quiesced || lifecycle->queues_online ||
	    lifecycle->accepting || lifecycle->bio_owned ||
	    lifecycle->command_owned || lifecycle->payload_dma_active ||
	    lifecycle->quarantined)
		return EBUSY;
	lifecycle->released = 1;
	return 0;
}

#endif
