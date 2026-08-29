/*
 * PCI NVMe controller driver
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <drivers/pci-nvme.h>
#include <drivers/pci-nvme-protocol.h>
#include <drivers/pci.h>

#include "pci-nvme-lifecycle.h"
#include "pci-nvme-io-lifecycle.h"
#include "pci-nvme-shutdown-lifecycle.h"

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
static int nvme_io_queue_create(struct nvme_controller *controller,
	int recovery_command);
static int nvme_io_recover(struct nvme_controller *controller);
static int nvme_io_lifecycles_quiesce(
	struct nvme_controller *controller);
static void nvme_io_fail_all_locked(struct nvme_controller *controller,
	int error);

static struct nvme_controller *nvme_primary;
static struct spinlock nvme_registry_lock;

static uint32_t
nvme_read32(const struct nvme_controller *controller, size_t offset)
{
	return *(volatile uint32_t *)(controller->registers + offset);
}

static uint64_t
nvme_read64(const struct nvme_controller *controller, size_t offset)
{
	uint32_t low = nvme_read32(controller, offset);
	uint32_t high = nvme_read32(controller, offset + sizeof(uint32_t));

	return (uint64_t)low | ((uint64_t)high << 32);
}

static void
nvme_write32(struct nvme_controller *controller, size_t offset,
	uint32_t value)
{
	*(volatile uint32_t *)(controller->registers + offset) = value;
	hal_io_mb();
}

static void
nvme_write64(struct nvme_controller *controller, size_t offset,
	uint64_t value)
{
	/* Queue address registers are programmed only while CC.EN is clear. */
	*(volatile uint32_t *)(controller->registers + offset) =
	    (uint32_t)value;
	*(volatile uint32_t *)(controller->registers + offset + 4U) =
	    (uint32_t)(value >> 32);
	hal_io_mb();
}

static uint64_t
nvme_timeout_ticks(unsigned milliseconds)
{
	uint64_t ticks = ((uint64_t)milliseconds * KERN_CLOCK_HZ + 999U) /
	    1000U;

	return ticks == 0U ? 1U : ticks;
}

static int
nvme_wait_ready(struct nvme_controller *controller, int expected_ready)
{
	uint64_t started = clock_ticks();
	uint64_t deadline_ticks = controller->timeout_ticks;
	uint64_t spin_budget;
	uint64_t spin = 0;
	int clock_running = 0;

	if (controller->timeout_ms >
	    NVME_READY_SPIN_LIMIT / NVME_READY_SPINS_PER_MS)
		spin_budget = NVME_READY_SPIN_LIMIT;
	else
		spin_budget = (uint64_t)controller->timeout_ms *
		    NVME_READY_SPINS_PER_MS;
	if (spin_budget == 0U)
		spin_budget = 1U;
	for (;;) {
		uint32_t status = nvme_read32(controller, DRV_NVME_REG_CSTS);
		enum drv_nvme_ready_state state = expected_ready ?
		    drv_nvme_controller_ready_state(status, 1) :
		    drv_nvme_controller_disable_state(status);
		uint64_t now;

		if (state == DRV_NVME_READY_MATCH)
			return 0;
		if (state == DRV_NVME_READY_FATAL ||
		    state == DRV_NVME_READY_UNREACHABLE)
			return EIO;
		now = clock_ticks();
		if (now != started)
			clock_running = 1;
		if (clock_running) {
			if (now - started >= deadline_ticks)
				return ETIMEDOUT;
		} else if (++spin >= spin_budget) {
			/* Early attach can precede the first timer tick. */
			return ETIMEDOUT;
		}
		hal_compiler_barrier();
	}
}

static int
nvme_wait_shutdown_complete(struct nvme_controller *controller)
{
	uint64_t started = clock_ticks();
	uint64_t spin_budget;
	uint64_t spin = 0;
	int clock_running = 0;

	if (controller->timeout_ms >
	    NVME_READY_SPIN_LIMIT / NVME_READY_SPINS_PER_MS)
		spin_budget = NVME_READY_SPIN_LIMIT;
	else
		spin_budget = (uint64_t)controller->timeout_ms *
		    NVME_READY_SPINS_PER_MS;
	if (spin_budget == 0U)
		spin_budget = 1U;
	for (;;) {
		uint32_t status = nvme_read32(controller, DRV_NVME_REG_CSTS);
		uint64_t now;

		if (status == UINT32_MAX ||
		    (status & DRV_NVME_CSTS_FATAL) != 0U)
			return EIO;
		if ((status & NVME_CSTS_SHST_MASK) == NVME_CSTS_SHST_COMPLETE)
			return 0;
		now = clock_ticks();
		if (now != started)
			clock_running = 1;
		if (clock_running) {
			if (now - started >= controller->timeout_ticks)
				return ETIMEDOUT;
		} else if (++spin >= spin_budget) {
			return ETIMEDOUT;
		}
		hal_compiler_barrier();
	}
}

static int
nvme_bus_master_disable(struct nvme_controller *controller)
{
	uint16_t command;
	int error;

	error = drv_pci_device_set_bus_master(controller->pci, false);
	if (error != 0)
		return error;
	error = drv_pci_device_config_read16(controller->pci,
	    NVME_PCI_COMMAND, &command);
	if (error != 0)
		return error;
	return (command & NVME_PCI_COMMAND_MASTER) == 0U ? 0 : EIO;
}

static int
nvme_pci_quiesce(struct nvme_controller *controller)
{
	uint16_t command;
	int error;

	error = drv_pci_device_config_read16(controller->pci,
	    NVME_PCI_COMMAND, &command);
	if (error != 0)
		return error;
	error = drv_pci_device_config_write16(controller->pci,
	    NVME_PCI_COMMAND,
	    (uint16_t)(command & (uint16_t)~NVME_PCI_COMMAND_ENABLE));
	if (error != 0)
		return error;
	error = drv_pci_device_config_read16(controller->pci,
	    NVME_PCI_COMMAND, &command);
	if (error != 0)
		return error;
	return (command & NVME_PCI_COMMAND_ENABLE) == 0U ? 0 : EIO;
}

static int
nvme_message_irq_mask(struct nvme_controller *controller)
{
	unsigned capability;
	uint16_t control, readback;
	int error;

	error = drv_pci_device_find_capability(controller->pci,
	    NVME_PCI_MSI_CAPABILITY, &capability);
	if (error == 0) {
		error = drv_pci_device_config_read16(controller->pci,
		    capability + NVME_PCI_MESSAGE_CONTROL, &control);
		if (error != 0)
			return error;
		control &= (uint16_t)~NVME_PCI_MSI_ENABLE;
		error = drv_pci_device_config_write16(controller->pci,
		    capability + NVME_PCI_MESSAGE_CONTROL, control);
		if (error != 0 || drv_pci_device_config_read16(controller->pci,
		    capability + NVME_PCI_MESSAGE_CONTROL, &readback) != 0 ||
		    (readback & NVME_PCI_MSI_ENABLE) != 0U)
			return error != 0 ? error : EIO;
	} else if (error != ENOENT) {
		return error;
	}
	error = drv_pci_device_find_capability(controller->pci,
	    NVME_PCI_MSIX_CAPABILITY, &capability);
	if (error == 0) {
		error = drv_pci_device_config_read16(controller->pci,
		    capability + NVME_PCI_MESSAGE_CONTROL, &control);
		if (error != 0)
			return error;
		control = (uint16_t)((control &
		    (uint16_t)~NVME_PCI_MSIX_ENABLE) |
		    NVME_PCI_MSIX_FUNCTION_MASK);
		error = drv_pci_device_config_write16(controller->pci,
		    capability + NVME_PCI_MESSAGE_CONTROL, control);
		if (error != 0 || drv_pci_device_config_read16(controller->pci,
		    capability + NVME_PCI_MESSAGE_CONTROL, &readback) != 0 ||
		    (readback & (NVME_PCI_MSIX_ENABLE |
		    NVME_PCI_MSIX_FUNCTION_MASK)) !=
		    NVME_PCI_MSIX_FUNCTION_MASK)
			return error != 0 ? error : EIO;
	} else if (error != ENOENT) {
		return error;
	}
	return 0;
}

static int
nvme_message_irq_save_and_mask(struct nvme_controller *controller)
{
	unsigned capability;
	int error;

	error = drv_pci_device_find_capability(controller->pci,
	    NVME_PCI_MSI_CAPABILITY, &capability);
	if (error == 0) {
		error = drv_pci_device_config_read16(controller->pci,
		    capability + NVME_PCI_MESSAGE_CONTROL,
		    &controller->inherited_msi_control);
		if (error != 0)
			return error;
		controller->inherited_msi_capability = capability;
		controller->inherited_msi_saved = 1;
	} else if (error != ENOENT) {
		return error;
	}
	error = drv_pci_device_find_capability(controller->pci,
	    NVME_PCI_MSIX_CAPABILITY, &capability);
	if (error == 0) {
		error = drv_pci_device_config_read16(controller->pci,
		    capability + NVME_PCI_MESSAGE_CONTROL,
		    &controller->inherited_msix_control);
		if (error != 0)
			return error;
		controller->inherited_msix_capability = capability;
		controller->inherited_msix_saved = 1;
	} else if (error != ENOENT) {
		return error;
	}
	return nvme_message_irq_mask(controller);
}

static int
nvme_message_irq_restore(struct nvme_controller *controller)
{
	uint16_t readback;
	int error;

	if (controller->inherited_msi_saved) {
		error = drv_pci_device_config_write16(controller->pci,
		    controller->inherited_msi_capability +
		    NVME_PCI_MESSAGE_CONTROL,
		    controller->inherited_msi_control);
		if (error != 0 || drv_pci_device_config_read16(controller->pci,
		    controller->inherited_msi_capability +
		    NVME_PCI_MESSAGE_CONTROL, &readback) != 0 ||
		    (readback & NVME_PCI_MSI_ENABLE) !=
		    (controller->inherited_msi_control & NVME_PCI_MSI_ENABLE))
			return error != 0 ? error : EIO;
	}
	if (controller->inherited_msix_saved) {
		error = drv_pci_device_config_write16(controller->pci,
		    controller->inherited_msix_capability +
		    NVME_PCI_MESSAGE_CONTROL,
		    controller->inherited_msix_control);
		if (error != 0 || drv_pci_device_config_read16(controller->pci,
		    controller->inherited_msix_capability +
		    NVME_PCI_MESSAGE_CONTROL, &readback) != 0 ||
		    (readback & (NVME_PCI_MSIX_ENABLE |
		    NVME_PCI_MSIX_FUNCTION_MASK)) !=
		    (controller->inherited_msix_control &
		    (NVME_PCI_MSIX_ENABLE | NVME_PCI_MSIX_FUNCTION_MASK)))
			return error != 0 ? error : EIO;
	}
	return 0;
}

static int
nvme_runtime_irq_mask(struct nvme_controller *controller,
	unsigned *capability_out, uint16_t *control_out)
{
	unsigned capability;
	uint16_t control, masked, readback;
	unsigned capability_id;
	int error;

	if (capability_out == NULL || control_out == NULL)
		return EINVAL;
	capability_id = controller->irq.type == DRV_PCI_IRQ_MSIX ?
	    NVME_PCI_MSIX_CAPABILITY : NVME_PCI_MSI_CAPABILITY;
	error = drv_pci_device_find_capability(controller->pci,
	    capability_id, &capability);
	if (error != 0)
		return error;
	error = drv_pci_device_config_read16(controller->pci,
	    capability + NVME_PCI_MESSAGE_CONTROL, &control);
	if (error != 0)
		return error;
	if (controller->irq.type == DRV_PCI_IRQ_MSIX)
		masked = control | NVME_PCI_MSIX_FUNCTION_MASK;
	else if (controller->irq.type == DRV_PCI_IRQ_MSI)
		masked = control & (uint16_t)~NVME_PCI_MSI_ENABLE;
	else
		return EOPNOTSUPP;
	error = drv_pci_device_config_write16(controller->pci,
	    capability + NVME_PCI_MESSAGE_CONTROL, masked);
	if (error != 0 || drv_pci_device_config_read16(controller->pci,
	    capability + NVME_PCI_MESSAGE_CONTROL, &readback) != 0)
		return error != 0 ? error : EIO;
	if (controller->irq.type == DRV_PCI_IRQ_MSIX) {
		if ((readback & NVME_PCI_MSIX_FUNCTION_MASK) == 0U)
			return EIO;
	} else if ((readback & NVME_PCI_MSI_ENABLE) != 0U) {
		return EIO;
	}
	*capability_out = capability;
	*control_out = control;
	return 0;
}

static int
nvme_runtime_irq_restore(struct nvme_controller *controller,
	unsigned capability, uint16_t control)
{
	uint16_t readback;
	int error;

	error = drv_pci_device_config_write16(controller->pci,
	    capability + NVME_PCI_MESSAGE_CONTROL, control);
	if (error != 0 || drv_pci_device_config_read16(controller->pci,
	    capability + NVME_PCI_MESSAGE_CONTROL, &readback) != 0)
		return error != 0 ? error : EIO;
	if (controller->irq.type == DRV_PCI_IRQ_MSIX)
		return (readback & (NVME_PCI_MSIX_ENABLE |
		    NVME_PCI_MSIX_FUNCTION_MASK)) ==
		    (control & (NVME_PCI_MSIX_ENABLE |
		    NVME_PCI_MSIX_FUNCTION_MASK)) ? 0 : EIO;
	return (readback & NVME_PCI_MSI_ENABLE) ==
	    (control & NVME_PCI_MSI_ENABLE) ? 0 : EIO;
}

static int
nvme_controller_disable(struct nvme_controller *controller)
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
	if (configuration == UINT32_MAX)
		return EIO;
	if ((configuration & DRV_NVME_CC_ENABLE) != 0U) {
		configuration &= ~(DRV_NVME_CC_ENABLE | DRV_NVME_CC_SHN_MASK);
		nvme_write32(controller, DRV_NVME_REG_CC, configuration);
		error = nvme_wait_ready(controller, 0);
		if (error != 0)
			return error;
	} else {
		error = nvme_wait_ready(controller, 0);
		if (error != 0)
			return error;
	}
	controller->controller_enabled = 0;
	return 0;
}

static int
nvme_controller_shutdown_normal(struct nvme_controller *controller)
{
	uint32_t configuration;

	if (!controller->controller_enabled)
		return 0;
	configuration = nvme_read32(controller, DRV_NVME_REG_CC);
	if (configuration == UINT32_MAX ||
	    (configuration & DRV_NVME_CC_ENABLE) == 0U)
		return EIO;
	configuration &= ~DRV_NVME_CC_SHN_MASK;
	configuration |= NVME_CC_SHN_NORMAL;
	nvme_write32(controller, DRV_NVME_REG_CC, configuration);
	return nvme_wait_shutdown_complete(controller);
}

static int
nvme_stop_admission(struct nvme_controller *controller)
{
	uint64_t deadline = clock_ticks() +
	    (controller->timeout_ticks != 0U ? controller->timeout_ticks : 1U);
	unsigned long irq;

	for (;;) {
		unsigned slot;

		irq = spin_lock_irqsave(&controller->command_lock);
		controller->stopping = 1;
		waitq_wake_all(&controller->io_state_waitq);
		for (slot = 0; slot < controller->io_slot_count; slot++) {
			(void)drv_nvme_io_lifecycle_stop(
			    &controller->io_slots[slot].lifecycle);
			waitq_wake_all(&controller->io_slots[slot].waitq);
		}
		if (!controller->command_pending && !controller->probe_busy &&
		    !controller->io_calls && !controller->io_pending &&
		    !controller->io_owned && !controller->io_recovery_busy) {
			spin_unlock_irqrestore(&controller->command_lock, irq);
			return 0;
		}
		spin_unlock_irqrestore(&controller->command_lock, irq);
		if (clock_ticks() >= deadline)
			return EBUSY;
		sched_yield();
	}
}

static void
nvme_detach_release(struct nvme_controller *controller, int resume)
{
	unsigned long irq = spin_lock_irqsave(&controller->command_lock);

	controller->detach_busy = 0;
	if (resume && !controller->quarantined &&
	    controller->controller_enabled)
		controller->stopping = 0;
	waitq_wake_all(&controller->io_state_waitq);
	spin_unlock_irqrestore(&controller->command_lock, irq);
}

/*
 * Claim the only detach transaction before waiting for an admitted namespace
 * probe.  The probe failure path can claim the same transaction while it
 * still owns probe_busy, so no second caller can free the controller between
 * clearing probe_busy and starting teardown.
 */
static int
nvme_detach_claim(struct drv_pci_device *device,
	struct nvme_controller **result)
{
	struct nvme_controller *controller;
	uint64_t deadline;
	unsigned long irq, registry_irq;

	if (device == NULL || result == NULL)
		return EINVAL;
	registry_irq = spin_lock_irqsave(&nvme_registry_lock);
	controller = nvme_primary;
	if (controller == NULL || controller->pci != device) {
		spin_unlock_irqrestore(&nvme_registry_lock, registry_irq);
		return EBUSY;
	}
	irq = spin_lock_irqsave(&controller->command_lock);
	if (controller->detach_busy) {
		spin_unlock_irqrestore(&controller->command_lock, irq);
		spin_unlock_irqrestore(&nvme_registry_lock, registry_irq);
		return EBUSY;
	}
	controller->detach_busy = 1;
	spin_unlock_irqrestore(&controller->command_lock, irq);
	spin_unlock_irqrestore(&nvme_registry_lock, registry_irq);
	deadline = clock_ticks() +
	    (controller->timeout_ticks != 0U ? controller->timeout_ticks : 1U);

	for (;;) {
		irq = spin_lock_irqsave(&controller->command_lock);
		if (!controller->command_pending && !controller->probe_busy) {
			spin_unlock_irqrestore(&controller->command_lock, irq);
			*result = controller;
			return 0;
		}
		spin_unlock_irqrestore(&controller->command_lock, irq);
		if (clock_ticks() >= deadline) {
			nvme_detach_release(controller, 1);
			return EBUSY;
		}
		sched_yield();
	}
}

/*
 * Terminal shutdown needs the same lifetime exclusion as detach, but it must
 * not wait for an admin command or namespace probe before attempting the two
 * independent hardware DMA-stop boundaries.  A lost completion is precisely
 * the case in which that wait would otherwise bypass controller and PCI
 * quiescence.  The shutdown lifecycle records the bounded admission failure;
 * this claim only prevents another owner from freeing the controller.
 */
static int
nvme_shutdown_claim(struct drv_pci_device *device,
	struct nvme_controller **result)
{
	struct nvme_controller *controller;
	unsigned long irq, registry_irq;

	if (device == NULL || result == NULL)
		return EINVAL;
	registry_irq = spin_lock_irqsave(&nvme_registry_lock);
	controller = nvme_primary;
	if (controller == NULL || controller->pci != device) {
		spin_unlock_irqrestore(&nvme_registry_lock, registry_irq);
		return EBUSY;
	}
	irq = spin_lock_irqsave(&controller->command_lock);
	if (controller->detach_busy) {
		spin_unlock_irqrestore(&controller->command_lock, irq);
		spin_unlock_irqrestore(&nvme_registry_lock, registry_irq);
		return EBUSY;
	}
	controller->detach_busy = 1;
	controller->stopping = 1;
	waitq_wake_all(&controller->io_state_waitq);
	spin_unlock_irqrestore(&controller->command_lock, irq);
	spin_unlock_irqrestore(&nvme_registry_lock, registry_irq);
	*result = controller;
	return 0;
}

static int
nvme_shutdown_stop_admission(void *context)
{
	struct nvme_controller *controller = context;
	int error = nvme_stop_admission(controller);

	if (error != 0) {
		unsigned long irq = spin_lock_irqsave(&controller->command_lock);

		/* Terminal shutdown cannot wait forever for a lost completion.  Fail
		 * the software owners, then continue to the two independent hardware
		 * DMA-stop boundaries below. */
		nvme_io_fail_all_locked(controller, error);
		spin_unlock_irqrestore(&controller->command_lock, irq);
	}
	return error;
}

static int
nvme_shutdown_normal(void *context)
{
	struct nvme_controller *controller = context;

	if (!controller->lifecycle.controller_claimed ||
	    controller->lifecycle.controller_disabled)
		return 0;
	if (!controller->lifecycle.bar_mapped || !controller->bar_mapped ||
	    controller->registers == NULL)
		return EIO;
	return nvme_controller_shutdown_normal(controller);
}

static int
nvme_shutdown_controller_disable(void *context)
{
	struct nvme_controller *controller = context;
	int error;

	if (!controller->lifecycle.controller_claimed ||
	    controller->lifecycle.controller_disabled)
		return 0;
	if (!controller->lifecycle.bar_mapped || !controller->bar_mapped ||
	    controller->registers == NULL)
		return EIO;
	error = nvme_controller_disable(controller);
	if (error == 0) {
		controller->lifecycle.controller_disabled = 1;
		controller->lifecycle.controller_enabled = 0;
	}
	return error;
}

static int
nvme_shutdown_bus_master_disable(void *context)
{
	struct nvme_controller *controller = context;
	int error;

	if (!controller->lifecycle.master_disable_required ||
	    controller->lifecycle.master_disabled)
		return 0;
	error = nvme_bus_master_disable(controller);
	if (error == 0)
		controller->lifecycle.master_disabled = 1;
	return error;
}

static const struct drv_nvme_shutdown_ops nvme_shutdown_operations = {
	.stop_admission = nvme_shutdown_stop_admission,
	.shutdown_normal = nvme_shutdown_normal,
	.controller_disable = nvme_shutdown_controller_disable,
	.bus_master_disable = nvme_shutdown_bus_master_disable,
};

static int
nvme_irq_remove(struct nvme_controller *controller)
{
	unsigned attempt;
	int error = 0;

	if (controller->irq_cookie != NULL) {
		for (attempt = 0; attempt < NVME_IRQ_DRAIN_SPINS; attempt++) {
			error = drv_pci_device_disestablish_irq_checked(
			    controller->pci, controller->irq_cookie);
			if (error != EBUSY)
				break;
			hal_compiler_barrier();
		}
		if (error != 0) {
			hal_printf(
			    "nvme: IRQ removal failed (%d); retaining controller resources\n",
			    error);
			return error;
		}
		controller->irq_cookie = NULL;
	}
	return 0;
}

static int
nvme_irq_drain(struct nvme_controller *controller)
{
	unsigned attempt;

	for (attempt = 0; attempt < NVME_IRQ_DRAIN_SPINS; attempt++) {
		if (atomic_raw_load_acquire(&controller->irq_busy) == 0U)
			return 0;
		hal_compiler_barrier();
	}
	hal_printf("nvme: IRQ drain timed out; retaining controller resources\n");
	return EBUSY;
}

static void
nvme_dma_free(struct nvme_controller *controller)
{
	struct drv_dma_device *dma = drv_pci_device_dma(controller->pci);
	unsigned slot;

	for (slot = 0; slot < NVME_IO_MAX_SLOTS; slot++) {
		if (slot < controller->io_slot_count) {
			if (!controller->io_slots[slot].lifecycle.controller_quiesced ||
			    controller->io_slots[slot].lifecycle.quarantined ||
			    drv_nvme_io_lifecycle_release(
			    &controller->io_slots[slot].lifecycle) != 0)
				__builtin_trap();
		}
		if (controller->io_slots[slot].bounce_dma.address != NULL)
			drv_dma_free_coherent(dma,
			    &controller->io_slots[slot].bounce_dma);
	}
	if (controller->io_completion_dma.address != NULL)
		drv_dma_free_coherent(dma, &controller->io_completion_dma);
	if (controller->io_submission_dma.address != NULL)
		drv_dma_free_coherent(dma, &controller->io_submission_dma);
	if (controller->identify_dma.address != NULL)
		drv_dma_free_coherent(dma, &controller->identify_dma);
	if (controller->admin_completion_dma.address != NULL)
		drv_dma_free_coherent(dma,
		    &controller->admin_completion_dma);
	if (controller->admin_submission_dma.address != NULL)
		drv_dma_free_coherent(dma,
		    &controller->admin_submission_dma);
	controller->admin_submission = NULL;
	controller->admin_completion = NULL;
	controller->io_submission = NULL;
	controller->io_completion = NULL;
	controller->io_slot_count = 0;
	controller->io_queue_ready = 0;
	controller->dma_allocated = 0;
}

static int
nvme_restore_bar(struct nvme_controller *controller)
{
	struct drv_pci_bar current;
	int error;

	if (!controller->original_bar_valid)
		return 0;
	error = drv_pci_device_bar(controller->pci, 0, &current);
	if (error != 0)
		return error;
	if (current.bus_address == controller->original_bar.bus_address)
		return 0;
	return drv_pci_device_assign_bar(controller->pci, 0,
	    controller->original_bar.bus_address);
}

static int
nvme_lifecycle_controller_disable(void *context)
{
	return nvme_controller_disable(context);
}

static int
nvme_lifecycle_master_disable(void *context)
{
	return nvme_bus_master_disable(context);
}

static int
nvme_lifecycle_irq_remove(void *context)
{
	return nvme_irq_remove(context);
}

static int
nvme_lifecycle_irq_drain(void *context)
{
	struct nvme_controller *controller = context;
	int error = nvme_irq_drain(controller);

	if (error == 0 && controller->io_slot_count != 0U)
		error = nvme_io_lifecycles_quiesce(controller);
	return error;
}

static void
nvme_lifecycle_irq_free(void *context)
{
	struct nvme_controller *controller = context;

	drv_pci_device_free_irqs(controller->pci, &controller->irq, 1);
	controller->irq_allocated = 0;
}

static void
nvme_lifecycle_dma_free(void *context)
{
	nvme_dma_free(context);
}

static void
nvme_lifecycle_bar_unmap(void *context)
{
	struct nvme_controller *controller = context;

	drv_pci_device_unmap_bar(controller->pci, &controller->mapping);
	controller->bar_mapped = 0;
	controller->registers = NULL;
}

static int
nvme_lifecycle_bar_restore(void *context)
{
	struct nvme_controller *controller = context;
	int error = nvme_restore_bar(controller);
	int quiesce_error;

	if (error == 0) {
		controller->original_bar_valid = 0;
		return 0;
	}
	quiesce_error = nvme_pci_quiesce(controller);
	if (quiesce_error != 0)
		hal_printf(
		    "nvme: PCI quiesce after BAR restore failure failed (%d)\n",
		    quiesce_error);
	return error;
}

static int
nvme_lifecycle_pci_restore(void *context)
{
	struct nvme_controller *controller = context;
	int error, message_error, quiesce_error;

	error = nvme_message_irq_restore(controller);
	if (error != 0)
		goto fail;
	error = drv_pci_device_restore_enable_state(controller->pci,
	    &controller->pci_enable_state);
	if (error == 0) {
		controller->inherited_msi_saved = 0;
		controller->inherited_msix_saved = 0;
		controller->pci_state_saved = 0;
		return 0;
	}

fail:
	message_error = nvme_message_irq_mask(controller);
	if (message_error != 0)
		hal_printf(
		    "nvme: message interrupt mask after PCI restore failure failed (%d)\n",
		    message_error);
	quiesce_error = nvme_pci_quiesce(controller);
	if (quiesce_error != 0)
		hal_printf(
		    "nvme: PCI quiesce after command restore failure failed (%d)\n",
		    quiesce_error);
	return error;
}

static void
nvme_lifecycle_bar_release(void *context)
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

static int
nvme_cleanup(struct nvme_controller *controller)
{
	int error;

	error = nvme_stop_admission(controller);
	if (error != 0)
		return error;
	error = drv_nvme_lifecycle_cleanup(&controller->lifecycle,
	    &nvme_lifecycle_operations, controller);
	if (error != 0)
		return error;
	controller->controller_owned = 0;
	controller->controller_enabled = 0;
	controller->dma_allocated = 0;
	controller->irq_allocated = 0;
	return 0;
}

static void
nvme_publish_controller(struct nvme_controller *controller)
{
	unsigned long irq;

	(void)drv_pci_device_set_driver_data(controller->pci, controller);
	irq = spin_lock_irqsave(&nvme_registry_lock);
	nvme_primary = controller;
	spin_unlock_irqrestore(&nvme_registry_lock, irq);
}

static void
nvme_unpublish_controller(struct nvme_controller *controller)
{
	unsigned long irq = spin_lock_irqsave(&nvme_registry_lock);

	if (nvme_primary == controller)
		nvme_primary = NULL;
	spin_unlock_irqrestore(&nvme_registry_lock, irq);
}

static void
nvme_io_fail_all_locked(struct nvme_controller *controller, int error)
{
	unsigned slot_index;

	if (error == 0)
		error = EIO;
	controller->io_fault = error;
	controller->io_recovery_needed = 1;
	controller->stopping = 1;
	for (slot_index = 0; slot_index < controller->io_slot_count;
	    slot_index++) {
		struct nvme_io_slot *slot = &controller->io_slots[slot_index];

		if (slot->state != NVME_IO_SLOT_ACTIVE)
			continue;
		(void)drv_nvme_io_lifecycle_fault(&slot->lifecycle);
		slot->state = NVME_IO_SLOT_FAULTED;
		slot->error = error;
		if (slot->posted) {
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

static unsigned
nvme_irq_admin_locked(struct nvme_controller *controller)
{
	unsigned consumed = 0;

	while (consumed < controller->queue_depth) {
		volatile struct drv_nvme_completion *entry =
		    &controller->admin_completion[
		    controller->completion_cursor.head];
		struct drv_nvme_completion completion;

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
		if (!controller->command_pending ||
		    !drv_nvme_completion_matches(&completion,
		    controller->completion_cursor.phase, 0U,
		    (uint16_t)controller->submission_tail,
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
	if (consumed != 0U)
		nvme_write32(controller, controller->completion_doorbell,
		    controller->completion_cursor.head);
	return consumed;
}

static struct nvme_io_slot *
nvme_io_completion_owner_locked(struct nvme_controller *controller,
	uint16_t command_id)
{
	unsigned index;

	for (index = 0; index < controller->io_slot_count; index++) {
		struct nvme_io_slot *slot = &controller->io_slots[index];

		if (slot->state == NVME_IO_SLOT_ACTIVE && slot->posted &&
		    slot->epoch == controller->io_epoch &&
		    slot->command_id == command_id)
			return slot;
	}
	return NULL;
}

static unsigned
nvme_irq_io_locked(struct nvme_controller *controller)
{
	unsigned consumed = 0;

	if (!controller->io_queue_ready || controller->io_completion == NULL)
		return 0;
	while (consumed < controller->io_queue_depth) {
		volatile struct drv_nvme_completion *entry =
		    &controller->io_completion[
		    controller->io_completion_cursor.head];
		struct drv_nvme_completion completion;
		struct nvme_io_slot *slot;

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
		if (completion.submission_id != NVME_IO_QUEUE_ID ||
		    completion.submission_head >= controller->io_queue_depth ||
		    slot == NULL) {
			drv_nvme_completion_cursor_advance(
			    &controller->io_completion_cursor);
			consumed++;
			nvme_io_fail_all_locked(controller, EIO);
			break;
		}
		if (drv_nvme_io_lifecycle_complete_command(&slot->lifecycle,
		    completion.command_id) != 0 || controller->io_pending == 0U) {
			(void)drv_nvme_io_lifecycle_fault(&slot->lifecycle);
			slot->error = EIO;
			slot->state = NVME_IO_SLOT_FAULTED;
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
	if (consumed != 0U)
		nvme_write32(controller, controller->io_completion_doorbell,
		    controller->io_completion_cursor.head);
	return consumed;
}

static int
nvme_irq(void *argument)
{
	struct nvme_controller *controller = argument;
	unsigned consumed;
	unsigned long irq;
	uint32_t status;

	(void)atomic_raw_fetch_add_relaxed(&controller->irq_busy, 1U);
	irq = spin_lock_irqsave(&controller->command_lock);
	consumed = nvme_irq_admin_locked(controller);
	consumed += nvme_irq_io_locked(controller);
	status = controller->controller_enabled ?
	    nvme_read32(controller, DRV_NVME_REG_CSTS) : 0U;
	if (status == UINT32_MAX || (status & DRV_NVME_CSTS_FATAL) != 0U) {
		if (controller->command_pending) {
			controller->command_pending = 0;
			controller->command_completed = 1;
			controller->command_error = EIO;
			controller->admin_fault = EIO;
		}
		if (controller->io_queue_ready)
			nvme_io_fail_all_locked(controller, EIO);
		consumed++;
	}
	spin_unlock_irqrestore(&controller->command_lock, irq);
	if (atomic_raw_fetch_add_release(&controller->irq_busy,
	    (unsigned)-1) == 0U)
		__builtin_trap();
	return consumed != 0U;
}

static int
nvme_admin_execute_mode(struct nvme_controller *controller,
	struct drv_nvme_command *command, uint32_t *result,
	int recovery_command)
{
	volatile struct drv_nvme_command *slot;
	uint64_t deadline;
	unsigned next;
	unsigned long irq;
	int error = 0;

	if (controller == NULL || command == NULL)
		return EINVAL;
	deadline = clock_ticks() + controller->timeout_ticks;
	irq = spin_lock_irqsave(&controller->command_lock);
	if ((controller->stopping &&
	    !(recovery_command && controller->io_recovery_busy)) ||
	    controller->quarantined ||
	    !controller->controller_enabled) {
		error = ENXIO;
		goto out;
	}
	if (controller->admin_fault != 0) {
		error = controller->admin_fault;
		goto out;
	}
	if (controller->command_pending) {
		error = EBUSY;
		goto out;
	}
	controller->next_command_id++;
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
	if (next == UINT32_MAX) {
		controller->command_pending = 0;
		error = EIO;
		goto out;
	}
	controller->submission_tail = next;
	hal_io_wmb();
	nvme_write32(controller, controller->submission_doorbell,
	    controller->submission_tail);
	while (!controller->command_completed) {
		/*
		 * Completion remains strictly interrupt-driven.  Do not sleep with
		 * the irqsave state held: the initial boot thread can otherwise
		 * prevent the CPU0-targeted MSI from leaving the local APIC IRR.
		 */
		spin_unlock_irqrestore(&controller->command_lock, irq);
		sched_yield();
		irq = spin_lock_irqsave(&controller->command_lock);
		if (!controller->command_completed && clock_ticks() >= deadline) {
			error = ETIMEDOUT;
			break;
		}
	}
	if (error != 0) {
		controller->command_pending = 0;
		controller->admin_fault = error;
	} else {
		error = controller->command_error;
		if (result != NULL)
			*result = controller->command_result;
	}
out:
	spin_unlock_irqrestore(&controller->command_lock, irq);
	return error;
}

static int
nvme_admin_execute(struct nvme_controller *controller,
	struct drv_nvme_command *command, uint32_t *result)
{
	return nvme_admin_execute_mode(controller, command, result, 0);
}

static int
nvme_identify(struct nvme_controller *controller, uint32_t namespace_id,
	uint8_t selector)
{
	struct drv_nvme_command command;

	memset(controller->identify_dma.address, 0,
	    controller->identify_dma.size);
	if (!drv_nvme_identify_command(&command, 0, namespace_id, selector,
	    controller->identify_dma.device_address))
		return EINVAL;
	hal_io_wmb();
	return nvme_admin_execute(controller, &command, NULL);
}

static int
nvme_io_wait_locked(struct nvme_controller *controller,
	struct wait_queue *waitq, uint64_t deadline, unsigned long *irq)
{
	struct thread *thread = thread_current();
	int error;

	/*
	 * Early VFS discovery runs in CPU0's idle thread.  An idle thread cannot
	 * enter the ordinary wait-queue sleep path: when no other thread is
	 * runnable the scheduler immediately selects the same idle context while
	 * the irqsave state remains disabled.  A CPU0-targeted MSI (and the timer
	 * which advances the deadline) would then remain pending forever.  Poll
	 * like the boot-time admin path in that context, restoring interrupts on
	 * every pass.  Once regular threads exist, use the wait queue normally.
	 */
	if (thread != NULL && (thread->flags & THREAD_FLAG_IDLE) == 0U) {
		uint64_t sequence = waitq_sequence(waitq);

		error = waitq_sleep(waitq, &controller->command_lock,
		    sequence, deadline, 0);
		return error == EAGAIN ? 0 : error;
	}
	spin_unlock_irqrestore(&controller->command_lock, *irq);
	sched_yield();
	*irq = spin_lock_irqsave(&controller->command_lock);
	return sched_ticks() >= deadline ? ETIMEDOUT : 0;
}

static int
nvme_io_ensure_online(struct nvme_controller *controller)
{
	uint64_t deadline = sched_ticks() + controller->timeout_ticks;
	unsigned long irq;
	int error;

	for (;;) {
		irq = spin_lock_irqsave(&controller->command_lock);
		if (controller->io_queue_ready && !controller->stopping &&
		    !controller->quarantined && !controller->io_recovery_needed) {
			spin_unlock_irqrestore(&controller->command_lock, irq);
			return 0;
		}
		if (controller->io_recovery_needed &&
		    !controller->io_recovery_busy && !controller->detach_busy &&
		    !controller->quarantined) {
			controller->io_recovery_busy = 1;
			spin_unlock_irqrestore(&controller->command_lock, irq);
			return nvme_io_recover(controller);
		}
		if (!controller->io_recovery_busy) {
			spin_unlock_irqrestore(&controller->command_lock, irq);
			return ENXIO;
		}
		error = nvme_io_wait_locked(controller,
		    &controller->io_state_waitq, deadline, &irq);
		spin_unlock_irqrestore(&controller->command_lock, irq);
		if (error != 0)
			return error;
	}
}

static int
nvme_io_begin_bio(struct nvme_controller *controller, enum bio_op operation,
	int *owned)
{
	uint64_t deadline = sched_ticks() + controller->timeout_ticks;
	unsigned long irq;
	int error = 0;

	*owned = 0;
	error = nvme_io_ensure_online(controller);
	if (error != 0)
		return error;
	irq = spin_lock_irqsave(&controller->command_lock);
	if (controller->stopping || controller->quarantined ||
	    !controller->io_queue_ready) {
		error = ENXIO;
		goto out;
	}
	controller->io_calls++;
	*owned = 1;
	if (operation == BIO_FLUSH) {
		controller->io_flush_waiting++;
		while (controller->io_data_active != 0U ||
		    controller->io_flush_active) {
			if (controller->stopping) {
				error = ENXIO;
				break;
			}
			error = nvme_io_wait_locked(controller,
			    &controller->io_state_waitq, deadline, &irq);
			if (error != 0)
				break;
		}
		controller->io_flush_waiting--;
		if (error == 0)
			controller->io_flush_active = 1;
	} else {
		while (controller->io_flush_waiting != 0U ||
		    controller->io_flush_active) {
			if (controller->stopping) {
				error = ENXIO;
				break;
			}
			error = nvme_io_wait_locked(controller,
			    &controller->io_state_waitq, deadline, &irq);
			if (error != 0)
				break;
		}
		if (error == 0)
			controller->io_data_active++;
	}
	if (error != 0) {
		controller->io_calls--;
		*owned = 0;
		waitq_wake_all(&controller->io_state_waitq);
	}
out:
	spin_unlock_irqrestore(&controller->command_lock, irq);
	return error;
}

static void
nvme_io_end_bio(struct nvme_controller *controller, enum bio_op operation)
{
	unsigned long irq = spin_lock_irqsave(&controller->command_lock);

	if (operation == BIO_FLUSH) {
		if (!controller->io_flush_active)
			__builtin_trap();
		controller->io_flush_active = 0;
	} else {
		if (controller->io_data_active == 0U)
			__builtin_trap();
		controller->io_data_active--;
	}
	if (controller->io_calls == 0U)
		__builtin_trap();
	controller->io_calls--;
	waitq_wake_all(&controller->io_state_waitq);
	spin_unlock_irqrestore(&controller->command_lock, irq);
}

static int
nvme_io_command_id_in_use_locked(struct nvme_controller *controller,
	uint16_t command_id)
{
	unsigned index;

	for (index = 0; index < controller->io_slot_count; index++)
		if (controller->io_slots[index].state != NVME_IO_SLOT_FREE &&
		    controller->io_slots[index].command_id == command_id)
			return 1;
	return 0;
}

static uint16_t
nvme_io_next_command_id_locked(struct nvme_controller *controller)
{
	unsigned attempts;

	for (attempts = 0; attempts < UINT16_MAX; attempts++) {
		controller->next_io_command_id++;
		if (controller->next_io_command_id == 0U)
			controller->next_io_command_id++;
		if (!nvme_io_command_id_in_use_locked(controller,
		    controller->next_io_command_id))
			return controller->next_io_command_id;
	}
	return 0;
}

static int
nvme_io_slot_acquire(struct nvme_controller *controller,
	struct nvme_io_slot **result)
{
	uint64_t deadline = sched_ticks() + controller->timeout_ticks;
	unsigned long irq;
	int error = 0;

	irq = spin_lock_irqsave(&controller->command_lock);
	for (;;) {
		unsigned index;

		if (controller->stopping || controller->quarantined ||
		    !controller->io_queue_ready || controller->io_fault != 0) {
			error = ENXIO;
			break;
		}
		for (index = 0; index < controller->io_slot_count; index++) {
			struct nvme_io_slot *slot = &controller->io_slots[index];
			uint16_t command_id;

			if (slot->state != NVME_IO_SLOT_FREE)
				continue;
			command_id = nvme_io_next_command_id_locked(controller);
			if (command_id == 0U) {
				error = EBUSY;
				break;
			}
			if (drv_nvme_io_lifecycle_begin_bio(
			    &slot->lifecycle) != 0) {
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
			return 0;
		}
		if (error != 0)
			break;
		error = nvme_io_wait_locked(controller,
		    &controller->io_state_waitq, deadline, &irq);
		if (error != 0)
			break;
	}
	spin_unlock_irqrestore(&controller->command_lock, irq);
	return error;
}

static int
nvme_io_post(struct nvme_controller *controller, struct nvme_io_slot *slot,
	const struct drv_nvme_command *command, int uses_payload)
{
	volatile struct drv_nvme_command *submission;
	unsigned next;
	unsigned long irq;
	int error;

	irq = spin_lock_irqsave(&controller->command_lock);
	if (slot->state != NVME_IO_SLOT_ACTIVE || controller->stopping ||
	    controller->quarantined || !controller->io_queue_ready ||
	    controller->io_fault != 0) {
		error = ENXIO;
		goto fail;
	}
	if (controller->io_pending >= controller->io_queue_depth - 1U) {
		error = EBUSY;
		goto fail;
	}
	error = drv_nvme_io_lifecycle_submit(&slot->lifecycle,
	    slot->command_id, uses_payload);
	if (error != 0)
		goto fail;
	submission = &controller->io_submission[
	    controller->io_submission_tail];
	*submission = *command;
	next = drv_nvme_queue_index_advance(controller->io_submission_tail,
	    controller->io_queue_depth);
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
	return 0;

fail:
	nvme_io_fail_all_locked(controller, error);
	spin_unlock_irqrestore(&controller->command_lock, irq);
	return error;
}

static int
nvme_io_wait_completion(struct nvme_controller *controller,
	struct nvme_io_slot *slot, int *recovery_owner)
{
	uint64_t deadline = sched_ticks() + controller->timeout_ticks;
	unsigned long irq;
	int error = 0;

	*recovery_owner = 0;
	irq = spin_lock_irqsave(&controller->command_lock);
	while (slot->state == NVME_IO_SLOT_ACTIVE) {
		error = nvme_io_wait_locked(controller, &slot->waitq,
		    deadline, &irq);
		if (error != 0 && slot->state == NVME_IO_SLOT_ACTIVE) {
			nvme_io_fail_all_locked(controller,
			    error == ETIMEDOUT ? ETIMEDOUT : EIO);
			break;
		}
	}
	if (slot->state == NVME_IO_SLOT_CQ_DONE ||
	    slot->state == NVME_IO_SLOT_FAULTED) {
		error = slot->error;
		slot->state = NVME_IO_SLOT_COPYING;
	} else if (error == 0) {
		error = EIO;
	}
	if (controller->io_recovery_needed &&
	    !controller->io_recovery_busy && !controller->detach_busy) {
		controller->io_recovery_busy = 1;
		*recovery_owner = 1;
	}
	spin_unlock_irqrestore(&controller->command_lock, irq);
	return error;
}

static int
nvme_io_claim_recovery_locked(struct nvme_controller *controller)
{
	if (!controller->io_recovery_needed ||
	    controller->io_recovery_busy || controller->detach_busy)
		return 0;
	controller->io_recovery_busy = 1;
	return 1;
}

static void
nvme_io_slot_release(struct nvme_controller *controller,
	struct nvme_io_slot *slot)
{
	unsigned long irq = spin_lock_irqsave(&controller->command_lock);

	if (slot->state != NVME_IO_SLOT_COPYING &&
	    slot->state != NVME_IO_SLOT_FAULTED)
		__builtin_trap();
	if (drv_nvme_io_lifecycle_complete_bio(&slot->lifecycle) != 0)
		__builtin_trap();
	slot->state = NVME_IO_SLOT_FREE;
	slot->posted = 0;
	slot->command_id = 0;
	slot->error = 0;
	if (controller->io_owned == 0U)
		__builtin_trap();
	controller->io_owned--;
	waitq_wake_all(&controller->io_state_waitq);
	spin_unlock_irqrestore(&controller->command_lock, irq);
}

static int
nvme_io_execute(struct nvme_controller *controller, uint8_t opcode,
	uint64_t first_block, uint32_t block_count, void *bytes)
{
	struct drv_nvme_command command;
	struct nvme_io_slot *slot;
	int recovery_owner;
	int error;

	error = nvme_io_slot_acquire(controller, &slot);
	if (error != 0)
		return error;
	if (opcode == DRV_NVME_NVM_WRITE)
		memcpy(slot->bounce_dma.address, bytes,
		    (size_t)block_count * controller->namespace_block_size);
	if (!drv_nvme_io_command(&command, slot->command_id, opcode,
	    controller->namespace_id, first_block, block_count,
	    opcode == DRV_NVME_NVM_FLUSH ? 0U :
	    slot->bounce_dma.device_address) ||
	    (opcode != DRV_NVME_NVM_FLUSH &&
	    !drv_nvme_single_prp_transfer_valid(
	    slot->bounce_dma.device_address, controller->namespace_block_size,
	    block_count, controller->page_size))) {
		unsigned long irq = spin_lock_irqsave(&controller->command_lock);

		nvme_io_fail_all_locked(controller, EINVAL);
		recovery_owner = nvme_io_claim_recovery_locked(controller);
		spin_unlock_irqrestore(&controller->command_lock, irq);
		nvme_io_slot_release(controller, slot);
		if (recovery_owner)
			(void)nvme_io_recover(controller);
		return EINVAL;
	}
	error = nvme_io_post(controller, slot, &command,
	    opcode != DRV_NVME_NVM_FLUSH);
	if (error == 0)
		error = nvme_io_wait_completion(controller, slot,
		    &recovery_owner);
	else {
		unsigned long irq = spin_lock_irqsave(&controller->command_lock);

		recovery_owner = nvme_io_claim_recovery_locked(controller);
		spin_unlock_irqrestore(&controller->command_lock, irq);
	}
	if (error == 0 && opcode == DRV_NVME_NVM_READ) {
		hal_io_rmb();
		memcpy(bytes, slot->bounce_dma.address,
		    (size_t)block_count * controller->namespace_block_size);
	}
	nvme_io_slot_release(controller, slot);
	if (recovery_owner)
		(void)nvme_io_recover(controller);
	return error;
}

static int
nvme_disk_submit(struct disk *disk, struct bio *bio)
{
	struct nvme_controller *controller;
	uint64_t block;
	uint32_t remaining;
	uint8_t *bytes;
	size_t transferred = 0;
	int owned;
	int error;

	if (disk == NULL || bio == NULL || disk->d_data == NULL)
		return EINVAL;
	if (bio->b_op != BIO_READ && bio->b_op != BIO_WRITE &&
	    bio->b_op != BIO_FLUSH)
		return EOPNOTSUPP;
	controller = disk->d_data;
	if (bio->b_op != BIO_FLUSH &&
	    (!drv_nvme_io_range_valid(bio->b_mapped_block,
	    bio->b_block_count, controller->namespace_blocks) ||
	    controller->namespace_block_size == 0U ||
	    (size_t)bio->b_block_count >
	    SIZE_MAX / controller->namespace_block_size))
		return EOVERFLOW;
	error = nvme_io_begin_bio(controller, bio->b_op, &owned);
	if (error != 0)
		return error;
	if (!owned)
		return EIO;
	if (bio->b_op == BIO_FLUSH) {
		error = nvme_io_execute(controller, DRV_NVME_NVM_FLUSH,
		    0U, 0U, NULL);
	} else {
		block = bio->b_mapped_block;
		remaining = bio->b_block_count;
		bytes = bio->b_data;
		while (remaining != 0U) {
			uint32_t chunk = drv_nvme_io_chunk_blocks(remaining,
			    controller->namespace_block_size,
			    NVME_IO_BOUNCE_SIZE,
			    controller->maximum_transfer_bytes);
			size_t chunk_bytes;

			if (chunk == 0U) {
				error = EINVAL;
				break;
			}
			chunk_bytes = (size_t)chunk *
			    controller->namespace_block_size;
			error = nvme_io_execute(controller,
			    bio->b_op == BIO_READ ? DRV_NVME_NVM_READ :
			    DRV_NVME_NVM_WRITE, block, chunk, bytes);
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
	return 0;
}

static const struct disk_ops nvme_disk_ops = {
	.submit = nvme_disk_submit,
};

static int
nvme_io_flush_internal(struct nvme_controller *controller)
{
	int owned;
	int error;

	error = nvme_io_begin_bio(controller, BIO_FLUSH, &owned);
	if (error != 0)
		return error;
	if (!owned)
		return EIO;
	error = nvme_io_execute(controller, DRV_NVME_NVM_FLUSH,
	    0U, 0U, NULL);
	nvme_io_end_bio(controller, BIO_FLUSH);
	return error;
}

static int
nvme_probe_permitted(struct nvme_controller *controller)
{
	unsigned long irq = spin_lock_irqsave(&controller->command_lock);
	int permitted = !controller->stopping && !controller->detach_busy &&
	    !controller->quarantined;

	spin_unlock_irqrestore(&controller->command_lock, irq);
	return permitted;
}

static int
nvme_probe_namespace(struct nvme_controller *controller)
{
	struct drv_nvme_controller_profile controller_profile;
	struct drv_nvme_namespace_profile namespace_profile;
	struct disk *disk;
	uint32_t namespace_id;
	uint32_t reasons;
	int error;

	error = nvme_identify(controller, 0,
	    DRV_NVME_IDENTIFY_CONTROLLER);
	if (error != 0)
		return error;
	reasons = drv_nvme_identify_controller_validate(
	    controller->identify_dma.address, controller->identify_dma.size,
	    &controller_profile);
	if (reasons != 0U) {
		hal_printf("nvme: unsupported Identify Controller (%08x)\n",
		    reasons);
		return EOPNOTSUPP;
	}
	error = nvme_identify(controller, 0,
	    DRV_NVME_IDENTIFY_ACTIVE_LIST);
	if (error != 0)
		return error;
	reasons = drv_nvme_active_namespace_validate(
	    controller->identify_dma.address, controller->identify_dma.size,
	    controller_profile.namespace_count, &namespace_id);
	if (reasons != 0U) {
		hal_printf("nvme: unsupported active namespace set (%08x)\n",
		    reasons);
		return EOPNOTSUPP;
	}
	error = nvme_identify(controller, namespace_id,
	    DRV_NVME_IDENTIFY_NAMESPACE);
	if (error != 0)
		return error;
	reasons = drv_nvme_identify_namespace_validate(
	    controller->identify_dma.address, controller->identify_dma.size,
	    &namespace_profile);
	if (reasons != 0U) {
		hal_printf("nvme: unsupported namespace %u (%08x)\n",
		    namespace_id, reasons);
		return EOPNOTSUPP;
	}
	controller->namespace_id = namespace_id;
	controller->namespace_blocks = namespace_profile.block_count;
	controller->namespace_block_size =
	    UINT32_C(1) << namespace_profile.block_size_shift;
	controller->maximum_transfer_bytes = drv_nvme_max_transfer_bytes(
	    controller_profile.maximum_transfer_shift, controller->page_size,
	    NVME_IO_BOUNCE_SIZE);
	if (controller->maximum_transfer_bytes <
	    controller->namespace_block_size)
		return EOPNOTSUPP;
	error = nvme_io_dma_allocate(controller);
	if (error != 0)
		return error;
	error = nvme_io_queue_create(controller, 0);
	if (error != 0)
		return error;
	disk = disk_alloc();
	if (disk == NULL)
		return ENOMEM;
	error = disk_alloc_nvme_name(disk, 0, namespace_id);
	if (error != 0)
		goto fail_disk;
	disk->d_flags = 0;
	disk->d_block_size = controller->namespace_block_size;
	disk->d_block_count = namespace_profile.block_count;
	disk->d_max_transfer_blocks = (uint32_t)(
	    controller->maximum_transfer_bytes / disk->d_block_size);
	disk->d_ops = &nvme_disk_ops;
	disk->d_data = controller;
	if (!nvme_probe_permitted(controller)) {
		error = ENXIO;
		goto fail_disk;
	}
	error = disk_create(disk);
	if (error != 0)
		goto fail_disk;
	controller->namespace_disk = disk;
	hal_printf(
	    "nvme: /dev/%s namespace=%u blocks=%08x:%08x block-size=%u "
	    "writable max-transfer=%u\n",
	    disk->d_name, namespace_id,
	    (uint32_t)(namespace_profile.block_count >> 32),
	    (uint32_t)namespace_profile.block_count,
	    disk->d_block_size, disk->d_max_transfer_blocks);
	return 0;

fail_disk:
	(void)disk_destroy(disk);
	return error;
}

static int
nvme_dma_allocate(struct nvme_controller *controller)
{
	struct drv_dma_device *dma = drv_pci_device_dma(controller->pci);
	size_t submission_bytes, completion_bytes;
	int error;

	if (dma == NULL ||
	    !drv_nvme_queue_bytes(controller->queue_depth,
	    sizeof(struct drv_nvme_command), &submission_bytes) ||
	    !drv_nvme_queue_bytes(controller->queue_depth,
	    sizeof(struct drv_nvme_completion), &completion_bytes))
		return EINVAL;
	error = drv_dma_alloc_coherent(dma, submission_bytes,
	    controller->page_size, &controller->admin_submission_dma);
	if (error != 0)
		return error;
	error = drv_dma_alloc_coherent(dma, completion_bytes,
	    controller->page_size, &controller->admin_completion_dma);
	if (error != 0)
		goto fail;
	error = drv_dma_alloc_coherent(dma, DRV_NVME_IDENTIFY_SIZE,
	    controller->page_size, &controller->identify_dma);
	if (error != 0)
		goto fail;
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
	controller->admin_submission =
	    controller->admin_submission_dma.address;
	controller->admin_completion =
	    controller->admin_completion_dma.address;
	controller->dma_allocated = 1;
	return 0;

fail:
	nvme_dma_free(controller);
	return error;
}

static void
nvme_io_dma_free_unpublished(struct nvme_controller *controller)
{
	struct drv_dma_device *dma = drv_pci_device_dma(controller->pci);
	unsigned index;

	for (index = 0; index < NVME_IO_MAX_SLOTS; index++)
		if (controller->io_slots[index].bounce_dma.address != NULL)
			drv_dma_free_coherent(dma,
			    &controller->io_slots[index].bounce_dma);
	if (controller->io_completion_dma.address != NULL)
		drv_dma_free_coherent(dma, &controller->io_completion_dma);
	if (controller->io_submission_dma.address != NULL)
		drv_dma_free_coherent(dma, &controller->io_submission_dma);
	controller->io_submission = NULL;
	controller->io_completion = NULL;
	controller->io_slot_count = 0;
	controller->io_queue_depth = 0;
}

static int
nvme_io_dma_allocate(struct nvme_controller *controller)
{
	struct drv_dma_device *dma = drv_pci_device_dma(controller->pci);
	size_t submission_bytes, completion_bytes;
	unsigned index;
	int error;

	controller->io_queue_depth = drv_nvme_selected_queue_depth(
	    controller->capability, NVME_IO_QUEUE_REQUESTED_DEPTH);
	if (dma == NULL || controller->io_queue_depth < 2U ||
	    controller->io_queue_depth - 1U > NVME_IO_MAX_SLOTS ||
	    !drv_nvme_queue_bytes(controller->io_queue_depth,
	    sizeof(struct drv_nvme_command), &submission_bytes) ||
	    !drv_nvme_queue_bytes(controller->io_queue_depth,
	    sizeof(struct drv_nvme_completion), &completion_bytes))
		return EINVAL;
	controller->io_slot_count = controller->io_queue_depth - 1U;
	error = drv_dma_alloc_coherent(dma, submission_bytes,
	    controller->page_size, &controller->io_submission_dma);
	if (error != 0)
		goto fail;
	error = drv_dma_alloc_coherent(dma, completion_bytes,
	    controller->page_size, &controller->io_completion_dma);
	if (error != 0)
		goto fail;
	for (index = 0; index < controller->io_slot_count; index++) {
		error = drv_dma_alloc_coherent(dma, NVME_IO_BOUNCE_SIZE,
		    controller->page_size,
		    &controller->io_slots[index].bounce_dma);
		if (error != 0)
			goto fail;
		if ((controller->io_slots[index].bounce_dma.device_address &
		    (controller->page_size - 1U)) != 0U ||
		    controller->io_slots[index].bounce_dma.size <
		    NVME_IO_BOUNCE_SIZE) {
			error = EIO;
			goto fail;
		}
	}
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
	if (!drv_nvme_completion_cursor_init(
	    &controller->io_completion_cursor,
	    controller->io_queue_depth)) {
		error = EIO;
		goto fail;
	}
	for (index = 0; index < controller->io_slot_count; index++) {
		struct nvme_io_slot *slot = &controller->io_slots[index];

		drv_nvme_io_lifecycle_init(&slot->lifecycle);
		waitq_init(&slot->waitq, "NVMe I/O completion");
		slot->state = NVME_IO_SLOT_FREE;
		slot->posted = 0;
		slot->epoch = controller->io_epoch;
	}
	return 0;

fail:
	nvme_io_dma_free_unpublished(controller);
	return error;
}

static int
nvme_io_queue_memory_reset(struct nvme_controller *controller)
{
	unsigned long irq;
	unsigned index;

	if (controller->io_submission == NULL ||
	    controller->io_completion == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&controller->command_lock);
	if (controller->io_owned != 0U || controller->io_pending != 0U) {
		spin_unlock_irqrestore(&controller->command_lock, irq);
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
	if (controller->io_epoch == 0U)
		controller->io_epoch++;
	if (!drv_nvme_completion_cursor_init(
	    &controller->io_completion_cursor,
	    controller->io_queue_depth)) {
		spin_unlock_irqrestore(&controller->command_lock, irq);
		return EIO;
	}
	for (index = 0; index < controller->io_slot_count; index++) {
		struct nvme_io_slot *slot = &controller->io_slots[index];

		slot->state = NVME_IO_SLOT_FREE;
		slot->posted = 0;
		slot->command_id = 0;
		slot->error = 0;
		slot->epoch = controller->io_epoch;
	}
	spin_unlock_irqrestore(&controller->command_lock, irq);
	return 0;
}

static int
nvme_io_queue_create(struct nvme_controller *controller,
	int recovery_command)
{
	struct drv_nvme_command command;
	uint32_t result;
	unsigned submission_count, completion_count;
	unsigned long irq;
	unsigned index;
	int error;

	if (!drv_nvme_set_queue_count_command(&command, 0, 1U, 1U))
		return EINVAL;
	error = nvme_admin_execute_mode(controller, &command, &result,
	    recovery_command);
	if (error != 0 || !drv_nvme_queue_count_result(result,
	    &submission_count, &completion_count))
		return error != 0 ? error : EIO;
	if (submission_count < 1U || completion_count < 1U)
		return ENOSPC;
	if (!drv_nvme_create_io_cq_command(&command, 0, NVME_IO_QUEUE_ID,
	    controller->io_queue_depth,
	    controller->io_completion_dma.device_address, 0U, 1))
		return EINVAL;
	error = nvme_admin_execute_mode(controller, &command, NULL,
	    recovery_command);
	if (error != 0)
		return error;
	if (!drv_nvme_create_io_sq_command(&command, 0, NVME_IO_QUEUE_ID,
	    NVME_IO_QUEUE_ID, controller->io_queue_depth,
	    controller->io_submission_dma.device_address))
		return EINVAL;
	error = nvme_admin_execute_mode(controller, &command, NULL,
	    recovery_command);
	if (error != 0)
		return error;
	irq = spin_lock_irqsave(&controller->command_lock);
	for (index = 0; index < controller->io_slot_count; index++) {
		error = drv_nvme_io_lifecycle_online(
		    &controller->io_slots[index].lifecycle);
		if (error != 0)
			break;
	}
	if (error == 0) {
		controller->io_queue_ready = 1;
		controller->io_fault = 0;
		controller->io_recovery_needed = 0;
	}
	waitq_wake_all(&controller->io_state_waitq);
	spin_unlock_irqrestore(&controller->command_lock, irq);
	return error;
}

static int
nvme_controller_enable(struct nvme_controller *controller)
{
	uint32_t configuration;
	int error;

	nvme_write32(controller, DRV_NVME_REG_AQA,
	    drv_nvme_admin_queue_attributes(controller->queue_depth));
	nvme_write64(controller, DRV_NVME_REG_ASQ,
	    controller->admin_submission_dma.device_address);
	nvme_write64(controller, DRV_NVME_REG_ACQ,
	    controller->admin_completion_dma.device_address);
	configuration = drv_nvme_controller_configuration(
	    controller->page_size, 1);
	if (configuration == 0U)
		return EINVAL;
	error = drv_pci_device_set_bus_master(controller->pci, true);
	if (error != 0)
		return error;
	controller->controller_owned = 1;
	nvme_write32(controller, DRV_NVME_REG_CC, configuration);
	controller->controller_enabled = 1;
	error = nvme_wait_ready(controller, 1);
	if (error != 0)
		return error;
	if (controller->irq.type != DRV_PCI_IRQ_MSIX)
		nvme_write32(controller, DRV_NVME_REG_INTMC, 1U);
	return 0;
}

static int
nvme_admin_queue_memory_reset(struct nvme_controller *controller)
{
	unsigned long irq;

	irq = spin_lock_irqsave(&controller->command_lock);
	if (controller->command_pending) {
		spin_unlock_irqrestore(&controller->command_lock, irq);
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
	if (!drv_nvme_completion_cursor_init(&controller->completion_cursor,
	    controller->queue_depth)) {
		spin_unlock_irqrestore(&controller->command_lock, irq);
		return EIO;
	}
	spin_unlock_irqrestore(&controller->command_lock, irq);
	return 0;
}

static int
nvme_io_lifecycles_quiesce(struct nvme_controller *controller)
{
	unsigned long irq = spin_lock_irqsave(&controller->command_lock);
	unsigned index;
	int error = 0;

	controller->io_queue_ready = 0;
	for (index = 0; index < controller->io_slot_count; index++) {
		struct nvme_io_slot *slot = &controller->io_slots[index];
		int lifecycle_error;

		(void)drv_nvme_io_lifecycle_stop(&slot->lifecycle);
		/* Every caller reaches this point only after a fresh controller or
		 * bus-master stop and an IRQ drain.  That fresh proof may resolve an
		 * older DMA-unsafe quarantine; the historical flag itself must not
		 * make safely owned DMA impossible to release forever. */
		if (slot->lifecycle.quarantined)
			lifecycle_error =
			    drv_nvme_io_lifecycle_resolve_quarantine(
			    &slot->lifecycle, 1, 1);
		else
			lifecycle_error = drv_nvme_io_lifecycle_quiesced(
			    &slot->lifecycle);
		if (lifecycle_error != 0)
			error = EIO;
		slot->posted = 0;
	}
	spin_unlock_irqrestore(&controller->command_lock, irq);
	return error;
}

static void
nvme_io_quarantine(struct nvme_controller *controller, int error,
	int dma_unsafe)
{
	unsigned long irq = spin_lock_irqsave(&controller->command_lock);
	unsigned index;

	controller->io_queue_ready = 0;
	controller->io_fault = error != 0 ? error : EIO;
	controller->io_recovery_needed = 1;
	controller->io_recovery_busy = 0;
	controller->stopping = 1;
	controller->quarantined = 1;
	if (dma_unsafe)
		for (index = 0; index < controller->io_slot_count; index++)
			(void)drv_nvme_io_lifecycle_quarantine(
			    &controller->io_slots[index].lifecycle);
	waitq_wake_all(&controller->io_state_waitq);
	spin_unlock_irqrestore(&controller->command_lock, irq);
}

static int
nvme_io_recover(struct nvme_controller *controller)
{
	uint64_t deadline = sched_ticks() + controller->timeout_ticks;
	unsigned irq_capability = 0;
	uint16_t irq_control = 0;
	unsigned long irq;
	int irq_masked = 0;
	int quiesced = 0;
	int error;

	irq = spin_lock_irqsave(&controller->command_lock);
	if (!controller->io_recovery_busy ||
	    !controller->io_recovery_needed || controller->detach_busy) {
		controller->io_recovery_busy = 0;
		spin_unlock_irqrestore(&controller->command_lock, irq);
		return controller->detach_busy ? ENXIO : EINVAL;
	}
	controller->stopping = 1;
	for (unsigned index = 0; index < controller->io_slot_count; index++)
		(void)drv_nvme_io_lifecycle_stop(
		    &controller->io_slots[index].lifecycle);
	while (controller->io_owned != 0U) {
		error = nvme_io_wait_locked(controller,
		    &controller->io_state_waitq, deadline, &irq);
		if (error != 0) {
			spin_unlock_irqrestore(&controller->command_lock, irq);
			nvme_io_quarantine(controller, error, 1);
			return error;
		}
	}
	spin_unlock_irqrestore(&controller->command_lock, irq);

	error = nvme_runtime_irq_mask(controller, &irq_capability,
	    &irq_control);
	if (error != 0)
		goto fail;
	irq_masked = 1;
	error = nvme_controller_disable(controller);
	if (error != 0)
		goto fail;
	error = nvme_bus_master_disable(controller);
	if (error != 0)
		goto fail;
	error = nvme_irq_drain(controller);
	if (error != 0)
		goto fail;
	quiesced = 1;
	error = nvme_io_lifecycles_quiesce(controller);
	if (error != 0)
		goto fail;
	error = nvme_admin_queue_memory_reset(controller);
	if (error != 0)
		goto fail;
	error = nvme_io_queue_memory_reset(controller);
	if (error != 0)
		goto fail;
	/* Bus mastering below invalidates the earlier quiescence proof. */
	quiesced = 0;
	error = nvme_controller_enable(controller);
	if (error != 0)
		goto fail;
	error = nvme_runtime_irq_restore(controller, irq_capability,
	    irq_control);
	if (error != 0)
		goto fail;
	irq_masked = 0;
	error = nvme_io_queue_create(controller, 1);
	if (error != 0)
		goto fail;
	irq = spin_lock_irqsave(&controller->command_lock);
	controller->io_recovery_busy = 0;
	controller->io_recovery_needed = 0;
	controller->io_fault = 0;
	if (!controller->detach_busy)
		controller->stopping = 0;
	waitq_wake_all(&controller->io_state_waitq);
	spin_unlock_irqrestore(&controller->command_lock, irq);
	hal_printf("nvme: I/O queue recovered epoch=%u\n",
	    controller->io_epoch);
	return 0;

fail:
	if (!irq_masked && controller->controller_enabled &&
	    nvme_runtime_irq_mask(controller, &irq_capability,
	    &irq_control) == 0)
		irq_masked = 1;
	if (controller->controller_owned) {
		int disable_error = nvme_controller_disable(controller);
		int master_error = nvme_bus_master_disable(controller);
		int drain_error = nvme_irq_drain(controller);

		if (disable_error == 0 && master_error == 0 && drain_error == 0)
			quiesced = 1;
		else
			quiesced = 0;
	}
	if (quiesced)
		(void)nvme_io_lifecycles_quiesce(controller);
	nvme_io_quarantine(controller, error, !quiesced);
	hal_printf("nvme: I/O recovery failed (%d); disk unavailable, DMA retained\n",
	    error);
	return error;
}

static int
nvme_attach(struct drv_pci_device *device, const struct drv_pci_id *id)
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
	if (nvme_primary != NULL) {
		spin_unlock_irqrestore(&nvme_registry_lock, registry_irq);
		hal_printf("nvme: additional controller rejected by initial profile\n");
		return EBUSY;
	}
	spin_unlock_irqrestore(&nvme_registry_lock, registry_irq);
	controller = hal_malloc(sizeof(*controller));
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
	if (error != 0)
		goto fail;
	controller->bar_claimed = 1;
	if (drv_nvme_lifecycle_record(&controller->lifecycle,
	    DRV_NVME_LIFECYCLE_BAR_CLAIMED) != 0) {
		error = EIO;
		goto fail;
	}
	stage = "BAR0 inspection";
	error = drv_pci_device_bar(device, 0, &controller->original_bar);
	if (error != 0)
		goto fail;
	controller->original_bar_valid = 1;
	if (drv_nvme_lifecycle_record(&controller->lifecycle,
	    DRV_NVME_LIFECYCLE_BAR_SNAPSHOTTED) != 0) {
		error = EIO;
		goto fail;
	}
	if (controller->original_bar.type != DRV_PCI_BAR_MEMORY32 &&
	    controller->original_bar.type != DRV_PCI_BAR_MEMORY64) {
		error = ENODEV;
		goto fail;
	}
	stage = "PCI command save";
	error = drv_pci_device_save_enable_state(device,
	    &controller->pci_enable_state);
	if (error != 0)
		goto fail;
	controller->pci_state_saved = 1;
	controller->restore_allowed = 1;
	if (drv_nvme_lifecycle_record(&controller->lifecycle,
	    DRV_NVME_LIFECYCLE_PCI_STATE_SAVED) != 0) {
		error = EIO;
		goto fail;
	}
	stage = "PCI quiesce";
	if (drv_nvme_lifecycle_record(&controller->lifecycle,
	    DRV_NVME_LIFECYCLE_PCI_COMMAND_CHANGED) != 0) {
		error = EIO;
		goto fail;
	}
	stage = "message interrupt quiesce";
	error = nvme_message_irq_save_and_mask(controller);
	if (error != 0)
		goto fail;
	stage = "PCI quiesce";
	error = nvme_pci_quiesce(controller);
	if (error != 0)
		goto fail;
	stage = "BAR0 map";
	error = drv_pci_device_map_bar(device, 0,
	    DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE | DRV_PCI_MAP_NOCACHE,
	    &controller->mapping);
	if (error != 0)
		goto fail;
	controller->bar_mapped = 1;
	if (drv_nvme_lifecycle_record(&controller->lifecycle,
	    DRV_NVME_LIFECYCLE_BAR_MAPPED) != 0) {
		error = EIO;
		goto fail;
	}
	stage = "BAR0 readback";
	error = drv_pci_device_bar(device, 0, &mapped_bar);
	if (error != 0 ||
	    (mapped_bar.type != DRV_PCI_BAR_MEMORY32 &&
	    mapped_bar.type != DRV_PCI_BAR_MEMORY64) ||
	    controller->mapping.address == NULL ||
	    controller->mapping.size > mapped_bar.size) {
		error = EIO;
		goto fail;
	}
	if (controller->mapping.size <
	    DRV_NVME_REG_VS + sizeof(uint32_t)) {
		error = ENODEV;
		goto fail;
	}
	stage = "PCI memory enable";
	error = drv_pci_device_enable_memory(device);
	if (error != 0)
		goto fail;
	error = drv_pci_device_config_read16(device, NVME_PCI_COMMAND,
	    &command);
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
	snapshot.requested_queue_entries =
	    NVME_ADMIN_QUEUE_REQUESTED_DEPTH;
	snapshot.maximum_queue_id = NVME_IO_QUEUE_ID;
	reasons = drv_nvme_capability_validate(&snapshot);
	if (reasons != 0U) {
		hal_printf(
		    "nvme: pci %04x:%02x:%02x.%u capability rejected %08x:%s\n",
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
	if (controller->timeout_ms > NVME_TIMEOUT_POLICY_MAX_MS)
		controller->timeout_ms = NVME_TIMEOUT_POLICY_MAX_MS;
	controller->timeout_ticks =
	    nvme_timeout_ticks(controller->timeout_ms);
	if (!drv_nvme_doorbell_offset(controller->capability, 0, 0,
	    controller->mapping.size, &controller->submission_doorbell) ||
	    !drv_nvme_doorbell_offset(controller->capability, 0, 1,
	    controller->mapping.size, &controller->completion_doorbell) ||
	    !drv_nvme_doorbell_offset(controller->capability,
	    NVME_IO_QUEUE_ID, 0, controller->mapping.size,
	    &controller->io_submission_doorbell) ||
	    !drv_nvme_doorbell_offset(controller->capability,
	    NVME_IO_QUEUE_ID, 1, controller->mapping.size,
	    &controller->io_completion_doorbell) ||
	    !drv_nvme_completion_cursor_init(&controller->completion_cursor,
	    controller->queue_depth)) {
		error = EIO;
		goto fail;
	}
	stage = "controller reset";
	/* From this point cleanup must prove CSTS.RDY clear before releasing BAR
	 * or DMA ownership.  A failed reset is quarantined, not half-detached. */
	controller->controller_owned = 1;
	if (drv_nvme_lifecycle_record(&controller->lifecycle,
	    DRV_NVME_LIFECYCLE_CONTROLLER_CLAIMED) != 0) {
		error = EIO;
		goto fail;
	}
	nvme_write32(controller, DRV_NVME_REG_INTMS, UINT32_MAX);
	error = nvme_controller_disable(controller);
	if (error != 0)
		goto fail;
	stage = "admin DMA";
	error = nvme_dma_allocate(controller);
	if (error != 0)
		goto fail;
	if (drv_nvme_lifecycle_record(&controller->lifecycle,
	    DRV_NVME_LIFECYCLE_DMA_ALLOCATED) != 0) {
		error = EIO;
		goto fail;
	}
	stage = "IRQ allocation";
	error = drv_pci_device_allocate_irqs(device,
	    DRV_PCI_IRQ_ALLOW_MSIX | DRV_PCI_IRQ_ALLOW_MSI,
	    1, 1, &controller->irq, &irq_count);
	if (error != 0)
		goto fail;
	if (irq_count != 1U) {
		if (irq_count != 0U) {
			controller->irq_allocated = 1;
			(void)drv_nvme_lifecycle_record(&controller->lifecycle,
			    DRV_NVME_LIFECYCLE_IRQ_ALLOCATED);
		}
		error = EIO;
		goto fail;
	}
	controller->irq_allocated = 1;
	if (drv_nvme_lifecycle_record(&controller->lifecycle,
	    DRV_NVME_LIFECYCLE_IRQ_ALLOCATED) != 0) {
		error = EIO;
		goto fail;
	}
	/* The initial reset masked all legacy/MSI sources.  Clear that mask
	 * before enabling MSI-X; INTMC must not be touched afterwards. */
	if (controller->irq.type == DRV_PCI_IRQ_MSIX)
		nvme_write32(controller, DRV_NVME_REG_INTMC, UINT32_MAX);
	stage = "IRQ establishment";
	error = drv_pci_device_establish_irq(device, &controller->irq,
	    nvme_irq, controller, "nvme", &controller->irq_cookie);
	if (error != 0)
		goto fail;
	if (drv_nvme_lifecycle_record(&controller->lifecycle,
	    DRV_NVME_LIFECYCLE_IRQ_ESTABLISHED) != 0) {
		error = EIO;
		goto fail;
	}
	stage = "controller enable";
	error = nvme_controller_enable(controller);
	if (error != 0)
		goto fail;
	if (drv_nvme_lifecycle_record(&controller->lifecycle,
	    DRV_NVME_LIFECYCLE_CONTROLLER_ENABLED) != 0) {
		error = EIO;
		goto fail;
	}
	controller->stopping = 0;
	nvme_publish_controller(controller);
	irq_name = controller->irq.type == DRV_PCI_IRQ_MSIX ? "MSI-X" :
	    "MSI";
	hal_printf(
	    "nvme: PCI controller %04x:%02x:%02x.%u version=%x queue=%u timeout=%ums %s\n",
	    address.segment, address.bus, address.device, address.function,
	    controller->version, controller->queue_depth,
	    controller->timeout_ms, irq_name);
	return 0;

fail:
	cleanup_error = nvme_cleanup(controller);
	if (cleanup_error != 0) {
		controller->quarantined = 1;
		nvme_publish_controller(controller);
		hal_printf(
		    "nvme: attach failed at %s (%d), cleanup failed (%d); quarantined\n",
		    stage, error, cleanup_error);
		return 0;
	}
	hal_printf("nvme: attach failed at %s (%d)\n", stage, error);
	hal_free(controller);
	return error;
}

static int
nvme_detach_owned(struct nvme_controller *controller)
{
	struct drv_pci_device *device = controller->pci;
	unsigned long irq;
	int can_flush;
	int newly_gone;
	int flush_error;
	int error;

	if (controller->namespace_disk != NULL) {
		error = disk_gone_if_idle(controller->namespace_disk);
		newly_gone = error == 0;
		if (error != 0 && error != ENXIO) {
			nvme_detach_release(controller, 1);
			return error;
		}
		if (newly_gone && drv_nvme_detach_flush_require(
		    &controller->detach_flush) != 0) {
			nvme_detach_release(controller, 0);
			return EIO;
		}
		/* disk_gone_if_idle writes dirty buffers but does not issue the
		 * device-cache FLUSH.  Once required, that durability obligation
		 * survives a failed detach and an already-gone result on retry. */
		irq = spin_lock_irqsave(&controller->command_lock);
		can_flush = controller->io_queue_ready &&
		    !controller->quarantined &&
		    !controller->io_recovery_needed;
		if (controller->detach_flush.required && can_flush)
			/* The namespace is GONE and detach owns the transaction, so this
			 * admits only the driver's internal retry. */
			controller->stopping = 0;
		flush_error = drv_nvme_detach_flush_begin(
		    &controller->detach_flush, can_flush);
		spin_unlock_irqrestore(&controller->command_lock, irq);
		if (flush_error == 0) {
			error = nvme_io_flush_internal(controller);
			irq = spin_lock_irqsave(&controller->command_lock);
			flush_error = drv_nvme_detach_flush_finish(
			    &controller->detach_flush, error);
			if (flush_error != 0)
				controller->stopping = 1;
			waitq_wake_all(&controller->io_state_waitq);
			spin_unlock_irqrestore(&controller->command_lock, irq);
		}
		if (flush_error != 0 && flush_error != EALREADY) {
			irq = spin_lock_irqsave(&controller->command_lock);
			controller->stopping = 1;
			if (flush_error == ENXIO && controller->io_fault != 0)
				flush_error = controller->io_fault;
			waitq_wake_all(&controller->io_state_waitq);
			spin_unlock_irqrestore(&controller->command_lock, irq);
			nvme_detach_release(controller, 0);
			hal_printf(
			    "nvme: final detach FLUSH incomplete (%d); disk gone, resources retained\n",
			    flush_error);
			return flush_error;
		}
	}
	error = nvme_cleanup(controller);
	if (error != 0) {
		controller->quarantined = 1;
		nvme_detach_release(controller, 0);
		return error;
	}
	if (controller->namespace_disk != NULL) {
		error = disk_destroy(controller->namespace_disk);
		if (error != 0) {
			controller->quarantined = 1;
			nvme_detach_release(controller, 0);
			return error;
		}
		controller->namespace_disk = NULL;
	}
	nvme_unpublish_controller(controller);
	(void)drv_pci_device_set_driver_data(device, NULL);
	hal_free(controller);
	return 0;
}

static int
nvme_detach(struct drv_pci_device *device, unsigned flags)
{
	struct nvme_controller *controller;
	int error;

	(void)flags;
	error = nvme_detach_claim(device, &controller);
	if (error != 0)
		return error;
	return nvme_detach_owned(controller);
}

static void
nvme_shutdown(struct drv_pci_device *device)
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
	if (error != 0)
		return;
	/* A historical quarantine is not a reason to skip terminal DMA safety.
	 * Mask delivery when the allocation still exists, but continue even if
	 * PCI capability access itself is damaged. */
	if (controller->irq_allocated)
		mask_error = nvme_runtime_irq_mask(controller,
		    &irq_capability, &irq_control);
	lifecycle_error = drv_nvme_shutdown_lifecycle_run(
	    &controller->shutdown_lifecycle, &nvme_shutdown_operations,
	    controller);
	if (controller->irq_cookie != NULL ||
	    controller->lifecycle.irq_may_be_busy)
		drain_error = nvme_irq_drain(controller);
	if (controller->shutdown_lifecycle.hardware_dma_safe &&
	    drain_error == 0 && controller->io_slot_count != 0U)
		quiesce_error = nvme_io_lifecycles_quiesce(controller);
	if (mask_error != 0 || lifecycle_error != 0 || drain_error != 0 ||
	    quiesce_error != 0) {
		controller->quarantined = 1;
		hal_printf(
		    "nvme: shutdown retained resources mask=%d lifecycle=%d drain=%d quiesce=%d dma-safe=%u\n",
		    mask_error, lifecycle_error, drain_error, quiesce_error,
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

int
drv_pci_nvme_driver_register(void)
{
	spin_init(&nvme_registry_lock, LOCK_RANK_DEVICE,
	    "NVMe controller registry");
	return drv_pci_driver_register(&nvme_driver);
}

void
drv_pci_nvme_probe_namespaces(void)
{
	struct nvme_controller *controller;
	unsigned long irq, registry_irq;
	int owns_detach = 0;
	int error;

	registry_irq = spin_lock_irqsave(&nvme_registry_lock);
	controller = nvme_primary;
	if (controller == NULL) {
		spin_unlock_irqrestore(&nvme_registry_lock, registry_irq);
		return;
	}
	irq = spin_lock_irqsave(&controller->command_lock);
	if (controller->probe_started || controller->detach_busy ||
	    controller->stopping ||
	    controller->quarantined) {
		spin_unlock_irqrestore(&controller->command_lock, irq);
		spin_unlock_irqrestore(&nvme_registry_lock, registry_irq);
		return;
	}
	controller->probe_started = 1;
	controller->probe_busy = 1;
	spin_unlock_irqrestore(&controller->command_lock, irq);
	spin_unlock_irqrestore(&nvme_registry_lock, registry_irq);
	error = nvme_probe_namespace(controller);
	if (error != 0)
		hal_printf("nvme: namespace probe failed (%d)\n", error);
	irq = spin_lock_irqsave(&controller->command_lock);
	if (error != 0 && !controller->detach_busy) {
		controller->detach_busy = 1;
		controller->stopping = 1;
		owns_detach = 1;
	}
	controller->probe_busy = 0;
	spin_unlock_irqrestore(&controller->command_lock, irq);
	if (error == 0)
		return;
	/* A concurrent PCI detach already owns teardown and may free controller
	 * as soon as probe_busy clears.  Do not dereference it in that case. */
	if (!owns_detach)
		return;
	/* A failed Identify probe releases every safely quiesced resource but
	 * retains the bound controller object as a terminal quarantine.  This
	 * keeps the PCI core's driver binding coherent; a later ordinary PCI
	 * detach can retry any retained release and free the object. */
	error = nvme_cleanup(controller);
	controller->quarantined = 1;
	nvme_detach_release(controller, 0);
	if (error != 0) {
		hal_printf("nvme: failed probe teardown retained resources (%d)\n",
		    error);
	} else {
		hal_printf("nvme: failed probe resources released; quarantined\n");
	}
}
