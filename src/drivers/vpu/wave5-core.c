/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Wave5 decoder core: its boot, its commands and its interrupts.
 *
 * Booting stops the core's bus traffic and resets it by software, copies
 * the firmware into a code area the core reaches through two 1 MiB remap
 * windows, gives it a temporary area, and starts its processor with the
 * initialization command; the identity query then tells what runs.  A
 * command is written to the command block with the instance it is for,
 * the host interrupt tells the firmware to look, and the busy flag falls
 * once the firmware took it.  The commands that queue work (a sequence, a
 * picture) end later with an interrupt whose reason names the work and
 * whose completion masks name the instance; a waiter that does not see
 * the interrupt polls the same registers.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/sched.h>
#include <uapi/errno.h>

#include "drivers/vpu/wave5-private.h"

/*
 * The product number of the Wave511 decoder.
 *
 * Value referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-vpuconfig.h
 * (WAVE511_CODE 0x5110).
 */
#define WAVE511_PRODUCT		0x5110U

/*
 * The arguments of the initialization command: the code area's base, size
 * and bus parameters, the temporary area's base and size, the hardware
 * options, and the secondary bus parameters.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-regdefine.h
 * (W5_ADDR_CODE_BASE 0x110 to W5_HW_OPTION 0x12c, W5_SEC_AXI_PARAM 0x180).
 */
#define BOOT_CODE_BASE		0x0110U
#define BOOT_CODE_SIZE		0x0114U
#define BOOT_CODE_PARAMETERS	0x0118U
#define BOOT_TEMP_BASE		0x011cU
#define BOOT_TEMP_SIZE		0x0120U
#define BOOT_HARDWARE_OPTIONS	0x012cU
#define BOOT_SECONDARY_BUS	0x0180U

/*
 * The identity query's answers: the firmware's version, the product's
 * name and version, the standards it implements, its features and its
 * customer.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-regdefine.h
 * (W5_RET_FW_VERSION 0x118 to W5_RET_CONF_FEATURE 0x12c,
 * W5_RET_CUSTOMER_ID 0x140).
 */
#define IDENTITY_FIRMWARE	0x0118U
#define IDENTITY_NAME		0x011cU
#define IDENTITY_VERSION	0x0120U
#define IDENTITY_STANDARD0	0x0124U
#define IDENTITY_STANDARD1	0x0128U
#define IDENTITY_FEATURES	0x012cU
#define IDENTITY_CUSTOMER	0x0140U

/*
 * The code area and the temporary area, and the remap windows the
 * processor sees the code through: each 1 MiB, enabled, in bypass mode,
 * with its index in bits 12 to 15 and its size in 4 KiB pages in bits 0
 * to 8.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-vpuconfig.h
 * (WAVE521_MAX_CODE_BUF_SIZE 2 MiB, WAVE5_TEMPBUF_SIZE 1 MiB,
 * W5_REMAP_MAX_SIZE 1 MiB), wave5-hw.c (REMAP_CTRL_REGISTER_VALUE: bits
 * 31 and 11) and wave5-regdefine.h (the REMAP_CTRL field layout).
 */
#define CODE_SIZE		0x200000U
#define TEMP_SIZE		0x100000U
#define REMAP_WINDOW		0x100000U
#define REMAP_ENABLE		0x80000000U
#define REMAP_BYPASS		0x00000800U
#define REMAP_INDEX_SHIFT	12U
#define REMAP_PAGES		(REMAP_WINDOW >> 12)

/*
 * The fast I/O window: the low 16 bits name a register of the bus fabric,
 * bit 16 makes the access a write, bit 31 reports it done.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-hw.c (FIO_CTRL_READY,
 * FIO_CTRL_WRITE, FASTIO_ADDRESS_MASK).
 */
#define FAST_IO_ADDRESS		0x0000ffffU
#define FAST_IO_WRITE		0x00010000U
#define FAST_IO_READY		0x80000000U

/*
 * The bus fabric's registers behind the fast I/O window: the plain bus
 * interface's request control and status, and the backbone's (the
 * processor's and the codec core's, or the combined one), its external
 * address, bus parameters and AXI identifiers.  The status that means
 * "idle" is 0x3f for a Wave511's plain bus interface and 0 for the
 * processor's backbone.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-regdefine.h
 * (W5_GDI_BUS_CTRL 0x88f0, W5_GDI_BUS_STATUS 0x88f4,
 * W5_BACKBONE_BASE_VCPU 0xfe00 and its offsets 0x00c, 0x010, 0x014,
 * 0x0c0, 0x0e0, W5_BACKBONE_BASE_VCORE0 0x8e00) and wave5-hw.c
 * (wave5_wait_bus_busy: 0x3f; the bus control values 0x100, 0xff, 0x7).
 */
#define BUS_PLAIN_CONTROL		0x88f0U
#define BUS_PLAIN_STATUS		0x88f4U
#define BUS_PROCESSOR_AXI_ID		0xfe0cU
#define BUS_PROCESSOR_CONTROL		0xfe10U
#define BUS_PROCESSOR_STATUS		0xfe14U
#define BUS_PROCESSOR_EXTERNAL		0xfec0U
#define BUS_PROCESSOR_PARAMETERS	0xfee0U
#define BUS_CORE_CONTROL		0x8e10U
#define BUS_CORE_STATUS			0x8e14U
#define BUS_COMBINED_CONTROL		0xfe10U
#define BUS_COMBINED_STATUS		0xfe14U
#define BUS_PLAIN_IDLE			0x3fU
#define BUS_PLAIN_STOP			0x100U
#define BUS_PROCESSOR_STOP		0xffU
#define BUS_CORE_STOP			0x7U

/*
 * The configuration bits that say which bus fabric the core has: a
 * backbone, one for the codec core, one for the processor.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-hw.c
 * (W521_FEATURE_BACKBONE bit 16, W521_FEATURE_VCORE_BACKBONE bit 22,
 * W521_FEATURE_VCPU_BACKBONE bit 28).
 */
#define CONFIG_BACKBONE			(1U << 16)
#define CONFIG_CORE_BACKBONE		(1U << 22)
#define CONFIG_PROCESSOR_BACKBONE	(1U << 28)

/*
 * The software reset of every block (the codec cores' three clocks and
 * the processor).
 *
 * Value referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-regdefine.h
 * (W5_RST_BLOCK_ALL 0x3fffffff).
 */
#define RESET_EVERY_BLOCK	0x3fffffffU

/* How long a fast I/O access and a bus stop are waited for, in milliseconds. */
#define FAST_IO_MS		100U

static int stop_bus(struct drv_wave5 *core, uint32_t configuration, uint32_t *control);
static void free_bus(struct drv_wave5 *core, uint32_t control);
static int software_reset(struct drv_wave5 *core);
static int boot_firmware(struct drv_wave5 *core, const uint8_t *firmware, size_t size);
static int read_identity(struct drv_wave5 *core);
static int fast_io_read(struct drv_wave5 *core, uint32_t address, uint32_t *value);
static int fast_io_write(struct drv_wave5 *core, uint32_t address, uint32_t value);
static int wait_register(struct drv_wave5 *core, uint32_t offset, uint32_t mask, uint32_t value, unsigned milliseconds);
static int service_interrupt(struct drv_wave5 *core, int from_handler);
static void interrupt_handler(int irq, kern_irq_ack_t acknowledge, void *argument);

/*
 * The uncached view of RAM.  Only architectures with devices that do not
 * snoop the CPU caches provide it, so it is declared weak and a core
 * cannot be attached without it.
 */
extern int kern_pmem_map_uncached(const struct kern_pmem *run, void **mapped) __attribute__((weak));
extern int kern_pmem_unmap_uncached(void *mapped, size_t size) __attribute__((weak));

/*
 * Boots the firmware on a powered core and starts taking its interrupts.
 */
int
drv_wave5_attach(
	const struct drv_wave5_place *place,
	const uint8_t *firmware,
	size_t size,
	struct drv_wave5 **core)
{
	struct drv_wave5 *booted;
	uint32_t product;
	void *mapped;
	int error;

	/* Refuses a missing place, firmware or result. */
	if (place == NULL || firmware == NULL || size == 0 || core == NULL)
		return EINVAL;

	/* Refuses an architecture without uncached views. */
	if (kern_pmem_map_uncached == NULL || kern_pmem_unmap_uncached == NULL)
		return ENOTSUP;

	/* Refuses firmware the code area cannot hold twice over, as the core needs. */
	if (size > CODE_SIZE / 2U)
		return EFBIG;

	/* Allocates the core's state. */
	booted = kern_calloc(1, sizeof(*booted));
	if (booted == NULL)
		return ENOMEM;

	/* Initializes the locks, the wait queue and the place. */
	booted->place = *place;
	(void)mutex_init(&booted->command_lock, LOCK_RANK_DEVICE, "wave5 command");
	spin_init(&booted->irq_lock, LOCK_RANK_DEVICE, "wave5 irq");
	waitq_init(&booted->irq_wait, "wave5");

	/* Maps the host registers. */
	error = kern_device_map(place->registers, (size_t)place->register_size, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0) {
		kern_free(booted);
		return error;
	}
	booted->registers = mapped;

	/* Refuses a core that is not the Wave511 decoder. */
	product = drv_wave5_core_read(booted, VPU_PRODUCT_NUMBER);
	booted->identity.product_code = product;
	if (product != WAVE511_PRODUCT) {
		(void)kern_device_unmap(mapped, (size_t)place->register_size);
		kern_free(booted);
		return ENODEV;
	}

	/* Takes the code and temporary areas. */
	error = drv_wave5_core_take(booted, CODE_SIZE + TEMP_SIZE, &booted->common);
	if (error != 0) {
		(void)kern_device_unmap(mapped, (size_t)place->register_size);
		kern_free(booted);
		return error;
	}

	/* Resets the core, boots the firmware and asks what runs. */
	mutex_lock(&booted->command_lock);

	error = software_reset(booted);
	if (error == 0)
		error = boot_firmware(booted, firmware, size);
	if (error == 0)
		error = read_identity(booted);

	mutex_unlock(&booted->command_lock);

	/* Leaves nothing behind when the boot failed. */
	if (error != 0) {
		drv_wave5_core_give(&booted->common);
		(void)kern_device_unmap(mapped, (size_t)place->register_size);
		kern_free(booted);
		return error;
	}

	/* Enables the decoder's interrupts at the core, then takes them. */
	drv_wave5_core_write(booted, VPU_IRQ_ENABLE, VPU_IRQ_SEQUENCE | VPU_IRQ_PICTURE | VPU_IRQ_STREAM_EMPTY);
	error = kern_irq_register((int)place->interrupt, interrupt_handler, booted);
	if (error != 0) {
		drv_wave5_core_write(booted, VPU_IRQ_ENABLE, 0);
		drv_wave5_core_give(&booted->common);
		(void)kern_device_unmap(mapped, (size_t)place->register_size);
		kern_free(booted);
		return error;
	}
	booted->irq_installed = 1;
	kern_irq_unmask((int)place->interrupt);

	/* Succeeded: the firmware runs and takes commands. */
	*core = booted;
	return 0;
}

/*
 * Stops using a core.
 */
void
drv_wave5_stop(
	struct drv_wave5 *core)
{
	/* Refuses every later command, waiting out one being issued. */
	mutex_lock(&core->command_lock);

	core->stopped = 1;

	mutex_unlock(&core->command_lock);

	/* Stops the core's interrupts, then removes the handler. */
	if (core->irq_installed) {
		drv_wave5_core_write(core, VPU_IRQ_ENABLE, 0);
		kern_irq_mask((int)core->place.interrupt);
		(void)kern_irq_unregister((int)core->place.interrupt, interrupt_handler, core);
		core->irq_installed = 0;
	}
}

/*
 * Frees a stopped core and every instance still open on it.
 */
void
drv_wave5_detach(
	struct drv_wave5 *core)
{
	struct drv_wave5_stream *orphan;

	/* Frees the instances the firmware never let go. */
	while (core->orphans != NULL) {
		orphan = core->orphans;
		core->orphans = orphan->next_orphan;
		drv_wave5_stream_free(orphan);
	}

	/* Frees the code area, the register mapping and the state. */
	drv_wave5_core_give(&core->common);
	(void)kern_device_unmap((void *)core->registers, (size_t)core->place.register_size);
	kern_free(core);
}

/*
 * Reports what the booted core said about itself.
 */
void
drv_wave5_identity(
	const struct drv_wave5 *core,
	struct drv_wave5_identity *identity)
{
	/* The identity read at boot. */
	*identity = core->identity;
}

/*
 * Reports how the core's completions were taken and their reasons.
 */
void
drv_wave5_interrupts(
	struct drv_wave5 *core,
	unsigned *taken,
	unsigned *polled,
	uint32_t *reasons)
{
	unsigned long irq;

	/* Takes the counts and the reasons, starting the reasons afresh. */
	irq = spin_lock_irqsave(&core->irq_lock);

	*taken = core->interrupts;
	*polled = core->polled;
	*reasons = core->reasons;
	core->reasons = 0;

	spin_unlock_irqrestore(&core->irq_lock, irq);
}

/*
 * Reports the reason the firmware gave for the last command it refused.
 */
uint32_t
drv_wave5_last_reason(
	struct drv_wave5 *core)
{
	/* The reason the command path kept. */
	return core->last_reason;
}

/*
 * Reads one host register.
 */
uint32_t
drv_wave5_core_read(
	struct drv_wave5 *core,
	uint32_t offset)
{
	uint32_t value;

	/* The registers are 32 bits wide at their byte offsets. */
	value = kern_mmio_read32((const volatile uint32_t *)(core->registers + offset));

	/* Succeeded: the register's contents. */
	return value;
}

/*
 * Writes one host register.
 */
void
drv_wave5_core_write(
	struct drv_wave5 *core,
	uint32_t offset,
	uint32_t value)
{
	/* The registers are 32 bits wide at their byte offsets. */
	kern_mmio_write32((volatile uint32_t *)(core->registers + offset), value);
}

/*
 * Issues one command and waits until the firmware took it.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-hw.c
 * (wave5_bit_issue_command, send_firmware_command,
 * wave5_vpu_firmware_command_queue_error_check: the instance and codec in
 * the instance register, the busy flag set, the command, the host
 * interrupt; the busy flag's fall; the success register and the reason).
 */
int
drv_wave5_core_command(
	struct drv_wave5 *core,
	const struct drv_wave5_stream *stream,
	uint32_t command,
	int check,
	uint32_t *reason)
{
	uint32_t taken;
	uint32_t why;
	int error;

	/* Refuses a command to a stopped core. */
	if (core->stopped)
		return EIO;

	/*
	 * Names the instance and its codec, raising the busy flag the
	 * firmware lowers once it took the command.  The core's own commands
	 * raise it themselves.
	 */
	if (stream != NULL) {
		drv_wave5_core_write(core, VPU_INSTANCE, (stream->standard << 16) | (stream->id & 0xffffU));
		drv_wave5_core_write(core, VPU_BUSY, 1);
	}

	/* Writes the command and tells the firmware to look, after every argument. */
	kern_io_write_barrier();
	drv_wave5_core_write(core, VPU_COMMAND, command);
	drv_wave5_core_write(core, VPU_HOST_IRQ, 1);

	/* Waits until the firmware took the command. */
	error = wait_register(core, VPU_BUSY, 0xffffffffU, 0, VPU_COMMAND_MS);
	if (error != 0)
		return error;

	/* Some commands are checked only after a series of them. */
	if (!check)
		return 0;

	/* Reports the firmware's refusal and its reason. */
	taken = drv_wave5_core_read(core, VPU_SUCCESS);
	if (taken == 0) {
		why = drv_wave5_core_read(core, VPU_FAIL_REASON);
		core->last_reason = why;
		if (reason != NULL)
			*reason = why;
		if (why == VPU_REASON_STILL_RUNNING)
			return EBUSY;
		return EIO;
	}

	/* Succeeded: the firmware took the command. */
	return 0;
}

/*
 * Issues one query.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-hw.c (wave5_send_query:
 * the option, the busy flag, the query command).
 */
int
drv_wave5_core_query(
	struct drv_wave5 *core,
	const struct drv_wave5_stream *stream,
	uint32_t option)
{
	int error;

	/* Names the query and raises the busy flag, then issues it. */
	drv_wave5_core_write(core, VPU_OPTION, option);
	drv_wave5_core_write(core, VPU_BUSY, 1);
	error = drv_wave5_core_command(core, stream, VPU_RUN_QUERY, 1, NULL);
	if (error != 0)
		return error;

	/* Succeeded: the query's answers are in the argument registers. */
	return 0;
}

/*
 * Forgets an instance's completions from before the next command.
 */
void
drv_wave5_core_forget(
	struct drv_wave5 *core,
	const struct drv_wave5_stream *stream)
{
	unsigned long irq;
	uint32_t bit;

	/* Clears the instance's bits in both masks. */
	bit = 1U << stream->id;
	irq = spin_lock_irqsave(&core->irq_lock);

	core->sequence_done &= ~bit;
	core->picture_done &= ~bit;

	spin_unlock_irqrestore(&core->irq_lock, irq);
}

/*
 * Waits for an instance's sequence or picture completion.
 */
int
drv_wave5_core_wait(
	struct drv_wave5 *core,
	const struct drv_wave5_stream *stream,
	int sequence,
	unsigned milliseconds)
{
	uint64_t observed;
	uint64_t deadline;
	uint64_t step;
	uint64_t now;
	unsigned long irq;
	uint32_t bit;
	int done;

	/* Sleeps until the instance's bit shows, polling the core each millisecond. */
	bit = 1U << stream->id;
	deadline = sched_ticks() + kern_ms_to_ticks(milliseconds);
	for (;;) {
		/* Takes the bit when the completion has come. */
		irq = spin_lock_irqsave(&core->irq_lock);

		observed = waitq_sequence(&core->irq_wait);
		done = 0;
		if (sequence && (core->sequence_done & bit) != 0) {
			core->sequence_done &= ~bit;
			done = 1;
		} else if (!sequence && (core->picture_done & bit) != 0) {
			core->picture_done &= ~bit;
			done = 1;
		}

		/* Sleeps one step unless it came or the bound is spent. */
		now = sched_ticks();
		if (!done && now < deadline) {
			step = now + kern_ms_to_ticks(1);
			if (step > deadline)
				step = deadline;
			(void)waitq_sleep(&core->irq_wait, &core->irq_lock, observed, step, 0);
		}

		spin_unlock_irqrestore(&core->irq_lock, irq);

		/* Reports the completion. */
		if (done)
			return 0;

		/* Gives up once the bound is spent. */
		now = sched_ticks();
		if (now >= deadline)
			return ETIMEDOUT;

		/* Polls the core, for an interrupt that did not reach the host. */
		(void)service_interrupt(core, 0);
	}
}

/*
 * Takes zeroed contiguous memory the core reaches, with its uncached view.
 */
int
drv_wave5_core_take(
	struct drv_wave5 *core,
	size_t size,
	struct wave5_memory *memory)
{
	size_t rounded;
	int error;

	/* Rounds to whole pages, as the uncached view maps them. */
	rounded = (size + 0xfffU) & ~(size_t)0xfffU;

	/* Takes the run below the core's reach. */
	memory->view = NULL;
	error = kern_pmem_alloc_limited(rounded, 0x1000U, core->place.dma_limit, 0, &memory->run);
	if (error != 0)
		return error;

	/* Maps it uncached, so the core and the host see the same bytes. */
	error = kern_pmem_map_uncached(&memory->run, &memory->view);
	if (error != 0) {
		(void)kern_pmem_free(&memory->run);
		memory->view = NULL;
		return error;
	}

	/* Clears it, so the core reads no stale bytes. */
	kern_memset(memory->view, 0, rounded);

	/* Succeeded: the memory is the caller's. */
	return 0;
}

/*
 * Gives contiguous memory back.
 */
void
drv_wave5_core_give(
	struct wave5_memory *memory)
{
	/* An empty memory has nothing to give. */
	if (memory->view == NULL)
		return;

	/* Removes the view, then frees the run. */
	(void)kern_pmem_unmap_uncached(memory->view, memory->run.size);
	(void)kern_pmem_free(&memory->run);
	memory->view = NULL;
}

/*
 * Reports a memory's bus address.
 */
uint32_t
drv_wave5_core_address(
	const struct wave5_memory *memory)
{
	/* The run lies below the core's reach, which is 32 bits. */
	return (uint32_t)memory->run.paddr;
}

/*
 * Stops the core's bus traffic before a software reset, reporting which
 * control register was used.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-hw.c (wave5_vpu_reset:
 * steps 1 and 2 for the backbone and the plain bus interface).
 */
static int
stop_bus(
	struct drv_wave5 *core,
	uint32_t configuration,
	uint32_t *control)
{
	uint32_t status;
	int error;

	/* Without a backbone the plain bus interface is stopped. */
	if ((configuration & CONFIG_BACKBONE) == 0) {
		*control = BUS_PLAIN_CONTROL;
		error = fast_io_write(core, BUS_PLAIN_CONTROL, BUS_PLAIN_STOP);
		if (error != 0)
			return error;
		error = fast_io_read(core, BUS_PLAIN_STATUS, &status);
		if (error != 0 || status != BUS_PLAIN_IDLE) {
			(void)fast_io_write(core, BUS_PLAIN_CONTROL, 0);
			return ETIMEDOUT;
		}
		return 0;
	}

	/* A combined backbone is stopped whole. */
	if ((configuration & CONFIG_CORE_BACKBONE) == 0) {
		*control = BUS_COMBINED_CONTROL;
		error = fast_io_write(core, BUS_COMBINED_CONTROL, BUS_CORE_STOP);
		if (error != 0)
			return error;
		error = fast_io_read(core, BUS_COMBINED_STATUS, &status);
		if (error != 0 || status != BUS_PLAIN_IDLE) {
			(void)fast_io_write(core, BUS_COMBINED_CONTROL, 0);
			return ETIMEDOUT;
		}
		return 0;
	}

	/* The processor's backbone is stopped first, when there is one. */
	*control = BUS_CORE_CONTROL;
	if ((configuration & CONFIG_PROCESSOR_BACKBONE) != 0) {
		error = fast_io_write(core, BUS_PROCESSOR_CONTROL, BUS_PROCESSOR_STOP);
		if (error != 0)
			return error;
		error = fast_io_read(core, BUS_PROCESSOR_STATUS, &status);
		if (error != 0 || status != 0) {
			(void)fast_io_write(core, BUS_PROCESSOR_CONTROL, 0);
			return ETIMEDOUT;
		}
	}

	/* Then the codec core's. */
	error = fast_io_write(core, BUS_CORE_CONTROL, BUS_CORE_STOP);
	if (error != 0)
		return error;
	error = fast_io_read(core, BUS_CORE_STATUS, &status);
	if (error != 0 || status != BUS_PLAIN_IDLE) {
		(void)fast_io_write(core, BUS_CORE_CONTROL, 0);
		return ETIMEDOUT;
	}

	/* Succeeded: no bus transaction is in flight. */
	return 0;
}

/* Lets the bus traffic run again after a software reset. */
static void
free_bus(
	struct drv_wave5 *core,
	uint32_t control)
{
	/* The processor's backbone was stopped along with the codec core's. */
	if (control == BUS_CORE_CONTROL)
		(void)fast_io_write(core, BUS_PROCESSOR_CONTROL, 0);

	/* The control register that stopped the traffic. */
	(void)fast_io_write(core, control, 0);
}

/*
 * Resets every block of the core by software, its bus traffic stopped
 * around it.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-hw.c (wave5_vpu_reset,
 * SW_RESET_ON_BOOT) and wave5-vdi.c (wave5_vdi_init: the command block
 * cleared while the processor is not running).
 */
static int
software_reset(
	struct drv_wave5 *core)
{
	uint32_t configuration;
	uint32_t control;
	uint32_t offset;
	uint32_t counter;
	int error;

	/* Clears the command block of a processor that never ran. */
	counter = drv_wave5_core_read(core, VPU_PROGRAM_COUNTER);
	if (counter == 0) {
		for (offset = VPU_COMMAND_FIRST; offset < VPU_COMMAND_END; offset += 4U)
			drv_wave5_core_write(core, offset, 0);
	}

	/* Lowers the busy flag: the reset itself answers nothing. */
	drv_wave5_core_write(core, VPU_BUSY, 0);

	/* Stops the bus traffic of the fabric the core has. */
	configuration = drv_wave5_core_read(core, VPU_CONFIG0);
	core->identity.configuration[0] = configuration;
	core->identity.configuration[1] = drv_wave5_core_read(core, VPU_CONFIG1);
	error = stop_bus(core, configuration, &control);
	if (error != 0)
		return error;

	/* Resets every block and waits for the reset to end. */
	drv_wave5_core_write(core, VPU_RESET_REQUEST, RESET_EVERY_BLOCK);
	error = wait_register(core, VPU_RESET_STATUS, 0xffffffffU, 0, VPU_COMMAND_MS);
	drv_wave5_core_write(core, VPU_RESET_REQUEST, 0);

	/* Lets the traffic run again either way. */
	free_bus(core, control);
	if (error != 0)
		return error;

	/* Succeeded: the core is reset. */
	return 0;
}

/*
 * Copies the firmware into the code area and starts the processor on it.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-hw.c (wave5_vpu_init,
 * remap_page: the power-on configuration, the command block cleared, the
 * two remap windows, the code and temporary areas, the options and bus
 * parameters cleared, the AXI identifiers of a backbone, the
 * initialization command and the remapped start).
 */
static int
boot_firmware(
	struct drv_wave5 *core,
	const uint8_t *firmware,
	size_t size)
{
	uint32_t code;
	uint32_t offset;
	uint32_t index;
	uint32_t taken;
	int error;

	/* Copies the firmware to the start of the code area. */
	code = drv_wave5_core_address(&core->common);
	kern_memcpy(core->common.view, firmware, size);
	kern_io_write_barrier();

	/* Starts without the power-on debug mode and with a clear command block. */
	drv_wave5_core_write(core, VPU_POWER_ON_CONFIG, 0);
	for (offset = VPU_COMMAND_FIRST; offset < VPU_COMMAND_END; offset += 4U)
		drv_wave5_core_write(core, offset, 0);

	/* Lets the processor see the code area through two 1 MiB windows. */
	for (index = 0; index < 2U; index++) {
		drv_wave5_core_write(core, VPU_REMAP_CONTROL, REMAP_ENABLE | (index << REMAP_INDEX_SHIFT) | REMAP_BYPASS | REMAP_PAGES);
		drv_wave5_core_write(core, VPU_REMAP_VIRTUAL, index * REMAP_WINDOW);
		drv_wave5_core_write(core, VPU_REMAP_PHYSICAL, code + index * REMAP_WINDOW);
	}

	/* Names the code and temporary areas and clears the options. */
	drv_wave5_core_write(core, BOOT_CODE_BASE, code);
	drv_wave5_core_write(core, BOOT_CODE_SIZE, CODE_SIZE);
	drv_wave5_core_write(core, BOOT_CODE_PARAMETERS, 0);
	drv_wave5_core_write(core, BOOT_TEMP_BASE, code + CODE_SIZE);
	drv_wave5_core_write(core, BOOT_TEMP_SIZE, TEMP_SIZE);
	drv_wave5_core_write(core, BOOT_HARDWARE_OPTIONS, 0);

	/* Clears the bus fabric's external address and parameters. */
	error = fast_io_write(core, BUS_PROCESSOR_EXTERNAL, 0);
	if (error != 0)
		return error;
	error = fast_io_write(core, BUS_PROCESSOR_PARAMETERS, 0);
	if (error != 0)
		return error;
	drv_wave5_core_write(core, BOOT_SECONDARY_BUS, 0);

	/* Gives every bus master of a backbone the AXI identifier 0. */
	if ((core->identity.configuration[0] & CONFIG_BACKBONE) != 0) {
		error = fast_io_write(core, BUS_PROCESSOR_AXI_ID, 0);
		if (error != 0)
			return error;
	}

	/* Issues the initialization and starts the processor at the remapped code. */
	drv_wave5_core_write(core, VPU_BUSY, 1);
	drv_wave5_core_write(core, VPU_COMMAND, VPU_RUN_INIT);
	drv_wave5_core_write(core, VPU_REMAP_START, 1);
	error = wait_register(core, VPU_BUSY, 0xffffffffU, 0, VPU_BOOT_MS);
	if (error != 0)
		return error;

	/* Refuses firmware that did not take the initialization. */
	taken = drv_wave5_core_read(core, VPU_SUCCESS);
	if (taken == 0)
		return EIO;

	/* Succeeded: the firmware runs. */
	return 0;
}

/*
 * Asks the firmware what runs.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-hw.c
 * (setup_wave5_properties: the identity query and its answers, the
 * product name most significant byte first).
 */
static int
read_identity(
	struct drv_wave5 *core)
{
	uint32_t name;
	int error;

	/* Asks the identity query. */
	error = drv_wave5_core_query(core, NULL, VPU_ASK_IDENTITY);
	if (error != 0)
		return error;

	/* Takes the name, most significant byte first. */
	name = drv_wave5_core_read(core, IDENTITY_NAME);
	core->identity.product_name[0] = (char)(name >> 24);
	core->identity.product_name[1] = (char)(name >> 16);
	core->identity.product_name[2] = (char)(name >> 8);
	core->identity.product_name[3] = (char)name;
	core->identity.product_name[4] = '\0';

	/* Takes the versions, the standards, the features and the customer. */
	core->identity.firmware_version = drv_wave5_core_read(core, IDENTITY_FIRMWARE);
	core->identity.product_version = drv_wave5_core_read(core, IDENTITY_VERSION);
	core->identity.standards[0] = drv_wave5_core_read(core, IDENTITY_STANDARD0);
	core->identity.standards[1] = drv_wave5_core_read(core, IDENTITY_STANDARD1);
	core->identity.features = drv_wave5_core_read(core, IDENTITY_FEATURES);
	core->identity.customer = drv_wave5_core_read(core, IDENTITY_CUSTOMER);

	/* Succeeded: the identity is known. */
	return 0;
}

/*
 * Reads a bus fabric register through the fast I/O window.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-hw.c
 * (wave5_wait_fio_readl).
 */
static int
fast_io_read(
	struct drv_wave5 *core,
	uint32_t address,
	uint32_t *value)
{
	int error;

	/* Names the register and waits for the access to end. */
	drv_wave5_core_write(core, VPU_FAST_IO_CONTROL, address & FAST_IO_ADDRESS);
	error = wait_register(core, VPU_FAST_IO_CONTROL, FAST_IO_READY, FAST_IO_READY, FAST_IO_MS);
	if (error != 0)
		return error;

	/* Succeeded: the register's value. */
	*value = drv_wave5_core_read(core, VPU_FAST_IO_DATA);
	return 0;
}

/*
 * Writes a bus fabric register through the fast I/O window.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-hw.c (wave5_fio_writel).
 */
static int
fast_io_write(
	struct drv_wave5 *core,
	uint32_t address,
	uint32_t value)
{
	int error;

	/* Puts the value, names the register as a write and waits for it to end. */
	drv_wave5_core_write(core, VPU_FAST_IO_DATA, value);
	drv_wave5_core_write(core, VPU_FAST_IO_CONTROL, (address & FAST_IO_ADDRESS) | FAST_IO_WRITE);
	error = wait_register(core, VPU_FAST_IO_CONTROL, FAST_IO_READY, FAST_IO_READY, FAST_IO_MS);
	if (error != 0)
		return error;

	/* Succeeded: the register is written. */
	return 0;
}

/* Waits until a register's masked bits read a value, ETIMEDOUT after the bound. */
static int
wait_register(
	struct drv_wave5 *core,
	uint32_t offset,
	uint32_t mask,
	uint32_t value,
	unsigned milliseconds)
{
	uint64_t deadline;
	uint64_t now;
	uint32_t contents;

	/* Reads the register until it shows the value, resting a little between reads. */
	deadline = sched_ticks() + kern_ms_to_ticks(milliseconds);
	for (;;) {
		/* Stops once the bits show the value. */
		contents = drv_wave5_core_read(core, offset);
		if ((contents & mask) == value)
			return 0;

		/* Gives up once the bound is spent. */
		now = sched_ticks();
		if (now >= deadline)
			return ETIMEDOUT;

		/* Rests before the next read. */
		kern_usleep_range(5, 20);
	}
}

/*
 * Takes one interrupt of the core, from the handler or a polling waiter:
 * clears it at the core, gathers the instances whose sequence or picture
 * completed, and wakes the waiters.  Reports whether one was pending.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-vpu.c
 * (wave5_vpu_irq_thread, wave5_vpu_handle_irq: the status, the reason and
 * its clear, the interrupt clear, the per-instance completion masks) and
 * wave5-hw.c (wave5_vpu_clear_interrupt: the reason the host sees).
 */
static int
service_interrupt(
	struct drv_wave5 *core,
	int from_handler)
{
	unsigned long irq;
	uint32_t status;
	uint32_t reason;
	uint32_t sequences;
	uint32_t pictures;
	uint32_t host;

	/* Takes the interrupt at the core and gathers its completions. */
	irq = spin_lock_irqsave(&core->irq_lock);

	status = drv_wave5_core_read(core, VPU_IRQ_STATUS);
	if (status == 0) {
		spin_unlock_irqrestore(&core->irq_lock, irq);
		return 0;
	}
	reason = drv_wave5_core_read(core, VPU_IRQ_REASON);
	drv_wave5_core_write(core, VPU_IRQ_REASON_CLEAR, reason);
	drv_wave5_core_write(core, VPU_IRQ_CLEAR, 1);
	sequences = drv_wave5_core_read(core, VPU_SEQUENCE_DONE);
	pictures = drv_wave5_core_read(core, VPU_PICTURE_DONE);
	if (sequences != 0)
		drv_wave5_core_write(core, VPU_SEQUENCE_DONE, 0);
	if (pictures != 0)
		drv_wave5_core_write(core, VPU_PICTURE_DONE, 0);
	host = drv_wave5_core_read(core, VPU_IRQ_REASON_HOST);
	drv_wave5_core_write(core, VPU_IRQ_REASON_HOST, host & ~reason);

	/*
	 * The masks name the instances whose sequence or picture is done; a
	 * waiter takes its own bit.  The counts tell the interrupts the host
	 * took from those a waiter found by polling.
	 */
	core->sequence_done |= sequences;
	core->picture_done |= pictures;
	core->reasons |= reason;
	if (from_handler) {
		core->interrupts++;
	} else {
		core->polled++;
	}
	waitq_wake_all(&core->irq_wait);

	spin_unlock_irqrestore(&core->irq_lock, irq);

	/* Succeeded: an interrupt was taken. */
	return 1;
}

/* Takes the core's interrupt line. */
static void
interrupt_handler(
	int irq,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	struct drv_wave5 *core;

	/* The core the handler was installed for. */
	(void)irq;
	core = argument;

	/* Takes the interrupt; a stopped core is not read. */
	if (!core->stopped)
		(void)service_interrupt(core, 1);

	/* Retires the acknowledgement before returning, as every handler must. */
	kern_irq_send_eoi(acknowledge);
}
