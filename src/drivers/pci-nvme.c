/*
 * PCI NVMe controller driver
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <drivers/pci-nvme.h>
#include <drivers/pci-nvme-protocol.h>
#include <drivers/pci.h>

#include "pci-nvme-lifecycle.h"

#include <errno.h>
#include <hal/hal.h>
#include <kern/atomic.h>
#include <kern/clock.h>
#include <kern/disk.h>
#include <kern/lock.h>
#include <kern/page.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <limits.h>
#include <string.h>

#define NVME_ADMIN_QUEUE_REQUESTED_DEPTH 64U
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

struct nvme_controller {
	struct drv_nvme_lifecycle lifecycle;
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

	struct spinlock command_lock;
	unsigned command_pending;
	unsigned command_completed;
	uint16_t expected_command_id;
	int command_error;
	uint32_t command_result;
	int admin_fault;

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
		enum drv_nvme_ready_state state =
		    drv_nvme_controller_ready_state(
		    nvme_read32(controller, DRV_NVME_REG_CSTS),
		    expected_ready);
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
nvme_stop_admission(struct nvme_controller *controller)
{
	uint64_t deadline = clock_ticks() +
	    (controller->timeout_ticks != 0U ? controller->timeout_ticks : 1U);
	unsigned long irq;

	for (;;) {
		irq = spin_lock_irqsave(&controller->command_lock);
		controller->stopping = 1;
		if (!controller->command_pending && !controller->probe_busy) {
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
	controller->stopping = 1;
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

static int
nvme_controller_stop(struct nvme_controller *controller)
{
	int disable_error = 0;
	int master_error;
	int error;

	error = nvme_stop_admission(controller);
	if (error != 0)
		return error;
	if (controller->controller_owned)
		disable_error = nvme_controller_disable(controller);
	master_error = nvme_bus_master_disable(controller);
	if (disable_error != 0) {
		hal_printf("nvme: controller disable failed (%d); retaining DMA\n",
		    disable_error);
		return disable_error;
	}
	if (master_error != 0) {
		hal_printf("nvme: bus-master disable failed (%d); retaining DMA\n",
		    master_error);
		return master_error;
	}
	return 0;
}

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
	return nvme_irq_drain(context);
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

static int
nvme_irq(void *argument)
{
	struct nvme_controller *controller = argument;
	unsigned consumed = 0;
	unsigned long irq;

	(void)atomic_raw_fetch_add_relaxed(&controller->irq_busy, 1U);
	irq = spin_lock_irqsave(&controller->command_lock);
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
			    drv_nvme_completion_success(&completion) ? 0 : EIO;
		}
		controller->command_pending = 0;
		controller->command_completed = 1;
		drv_nvme_completion_cursor_advance(
		    &controller->completion_cursor);
		consumed++;
	}
	if (consumed != 0U) {
		nvme_write32(controller, controller->completion_doorbell,
		    controller->completion_cursor.head);
	}
	spin_unlock_irqrestore(&controller->command_lock, irq);
	if (atomic_raw_fetch_add_release(&controller->irq_busy,
	    (unsigned)-1) == 0U)
		__builtin_trap();
	return consumed != 0U;
}

static int
nvme_admin_execute(struct nvme_controller *controller,
	struct drv_nvme_command *command, uint32_t *result)
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
	if (controller->stopping || controller->quarantined ||
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
nvme_disk_submit(struct disk *disk, struct bio *bio)
{
	(void)disk;
	(void)bio;
	return EOPNOTSUPP;
}

static const struct disk_ops nvme_read_only_disk_ops = {
	.submit = nvme_disk_submit,
};

static int
nvme_probe_permitted(struct nvme_controller *controller)
{
	unsigned long irq = spin_lock_irqsave(&controller->command_lock);
	int permitted = !controller->stopping && !controller->quarantined;

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
	disk = disk_alloc();
	if (disk == NULL)
		return ENOMEM;
	error = disk_alloc_nvme_name(disk, 0, namespace_id);
	if (error != 0)
		goto fail_disk;
	disk->d_flags = DISK_READ_ONLY;
	disk->d_block_size = UINT32_C(1) << namespace_profile.block_size_shift;
	disk->d_block_count = namespace_profile.block_count;
	disk->d_max_transfer_blocks = 1U;
	disk->d_ops = &nvme_read_only_disk_ops;
	disk->d_data = controller;
	if (!nvme_probe_permitted(controller)) {
		error = ENXIO;
		goto fail_disk;
	}
	error = disk_create(disk);
	if (error != 0)
		goto fail_disk;
	controller->namespace_disk = disk;
	controller->namespace_id = namespace_id;
	hal_printf(
	    "nvme: /dev/%s namespace=%u blocks=%08x:%08x block-size=%u "
	    "read-only\n",
	    disk->d_name, namespace_id,
	    (uint32_t)(namespace_profile.block_count >> 32),
	    (uint32_t)namespace_profile.block_count,
	    disk->d_block_size);
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
	controller->pci = device;
	controller->page_size = ZEDBSD_PAGE_SIZE;
	spin_init(&controller->command_lock, LOCK_RANK_DEVICE,
	    "NVMe admin command");
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
	snapshot.maximum_queue_id = 0;
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
	int error;

	if (controller->namespace_disk != NULL) {
		error = disk_gone_if_idle(controller->namespace_disk);
		if (error != 0 && error != ENXIO) {
			nvme_detach_release(controller, 1);
			return error;
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
	int error;

	error = nvme_detach_claim(device, &controller);
	if (error != 0)
		return;
	if (controller->quarantined) {
		nvme_detach_release(controller, 0);
		return;
	}
	error = nvme_controller_stop(controller);
	if (error != 0)
		controller->quarantined = 1;
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
	if (controller->probe_started || controller->stopping ||
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
