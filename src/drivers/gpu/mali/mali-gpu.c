/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GPU control block of a Mali Valhall GPU: its identity, the soft
 * reset, the L2 cache's power, the cache flushes and the three interrupt
 * lines.
 *
 * The host powers only the L2; with a command stream front end the
 * firmware powers the shader and tiler cores itself, from the core mask
 * the global interface gives it.  Every wait here polls a raw status
 * register for a bounded time, sleeping a millisecond between reads, so it does
 * not depend on an interrupt that a wrong line number would never bring.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/klog.h>
#include <kern/sched.h>
#include <uapi/errno.h>

#include "drivers/gpu/mali/mali-private.h"
#include "drivers/gpu/mali/mali-regs.h"

/* How long a register poll sleeps before the next read, in milliseconds (a tick at least). */
#define MALI_GPU_POLL_MS 1U

/*
 * How long the waits last: the soft reset 100 ms, a power change 20 ms,
 * a cache flush 100 ms.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_gpu.c (the timeouts of
 * panthor_gpu_soft_reset, panthor_gpu_l2_power_on and
 * panthor_gpu_flush_caches).
 */
#define RESET_WAIT_MS		100U
#define POWER_WAIT_MS		20U
#define FLUSH_WAIT_MS		100U

/* The GPU block's interrupts the handler takes: the two faults. */
#define GPU_IRQ_TAKEN		(MALI_GPU_IRQ_FAULT | MALI_GPU_IRQ_PROTECTED_FAULT)

/* Every address space's fault bit of the MMU block (the low 16 bits). */
#define MMU_IRQ_FAULTS		0x0000ffffU

static int wait_ready(struct mali_device *device, uint32_t ready, uint32_t transition, uint64_t mask, unsigned milliseconds);
static void gpu_interrupt(int irq, kern_irq_ack_t acknowledge, void *argument);
static void mmu_interrupt(int irq, kern_irq_ack_t acknowledge, void *argument);
static void job_interrupt(int irq, kern_irq_ack_t acknowledge, void *argument);
static int take_line(int irq, kern_irq_handler_t handler, struct mali_device *device);

/*
 * Reads a 32-bit register of the GPU.
 */
uint32_t
mali_read32(
	const struct mali_device *device,
	uint32_t offset)
{
	uint32_t value;

	/* One read of the register window. */
	value = kern_mmio_read32((const volatile void *)(device->registers + offset));

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Reads a 64-bit register pair of the GPU, low word first.
 */
uint64_t
mali_read64(
	const struct mali_device *device,
	uint32_t offset)
{
	uint64_t low;
	uint64_t high;

	/* The GPU's 64-bit registers are two 32-bit words, read low then high. */
	low = kern_mmio_read32((const volatile void *)(device->registers + offset));
	high = kern_mmio_read32((const volatile void *)(device->registers + offset + 4U));

	/* Succeeded: the register pair's value. */
	return low | (high << 32);
}

/*
 * Writes a 32-bit register of the GPU.
 */
void
mali_write32(
	struct mali_device *device,
	uint32_t offset,
	uint32_t value)
{
	/* One write of the register window. */
	kern_mmio_write32((volatile void *)(device->registers + offset), value);
}

/*
 * Writes a 64-bit register pair of the GPU, low word first.
 */
void
mali_write64(
	struct mali_device *device,
	uint32_t offset,
	uint64_t value)
{
	/* The low word, then the high word. */
	kern_mmio_write32((volatile void *)(device->registers + offset), (uint32_t)value);
	kern_mmio_write32((volatile void *)(device->registers + offset + 4U), (uint32_t)(value >> 32));
}

/*
 * Reads the GPU's identity, features and present cores, and splits the
 * identity into its fields.
 */
void
mali_gpu_read_identity(
	struct mali_device *device)
{
	uint32_t id;

	/* The identity register and the feature registers. */
	id = mali_read32(device, MALI_REG_GPU_ID);
	device->gpu_id = id;
	device->revision = mali_read32(device, MALI_REG_REVID);
	device->l2_features = mali_read32(device, MALI_REG_L2_FEATURES);
	device->core_features = mali_read32(device, MALI_REG_CORE_FEATURES);
	device->tiler_features = mali_read32(device, MALI_REG_TILER_FEATURES);
	device->memory_features = mali_read32(device, MALI_REG_MEM_FEATURES);
	device->mmu_features = mali_read32(device, MALI_REG_MMU_FEATURES);
	device->address_spaces = mali_read32(device, MALI_REG_AS_PRESENT);
	device->csf_id = mali_read32(device, MALI_REG_CSF_ID);
	device->coherency_features = mali_read32(device, MALI_REG_COHERENCY_FEATURES);

	/* The cores of each kind that the GPU has. */
	device->shader_present = mali_read64(device, MALI_REG_SHADER_PRESENT);
	device->tiler_present = mali_read64(device, MALI_REG_TILER_PRESENT);
	device->l2_present = mali_read64(device, MALI_REG_L2_PRESENT);

	/* The identity's fields. */
	device->arch_major = (id >> MALI_ID_ARCH_MAJOR_SHIFT) & MALI_ID_NIBBLE_MASK;
	device->arch_minor = (id >> MALI_ID_ARCH_MINOR_SHIFT) & MALI_ID_NIBBLE_MASK;
	device->arch_revision = (id >> MALI_ID_ARCH_REV_SHIFT) & MALI_ID_NIBBLE_MASK;
	device->product = (id >> MALI_ID_PRODUCT_MAJOR_SHIFT) & MALI_ID_NIBBLE_MASK;
	device->version_major = (id >> MALI_ID_VERSION_MAJOR_SHIFT) & MALI_ID_NIBBLE_MASK;
	device->version_minor = (id >> MALI_ID_VERSION_MINOR_SHIFT) & MALI_ID_BYTE_MASK;
	device->version_status = id & MALI_ID_NIBBLE_MASK;

	/* The widths of the GPU's virtual and physical addresses. */
	device->virtual_bits = device->mmu_features & MALI_MMU_VA_BITS_MASK;
	device->physical_bits = (device->mmu_features >> MALI_MMU_PA_BITS_SHIFT) & MALI_MMU_VA_BITS_MASK;
}

/*
 * Names the GPU's model from its architecture and product.
 */
const char *
mali_gpu_model(
	const struct mali_device *device)
{
	/* Only the products of the v10 architecture are named. */
	if (device->arch_major != MALI_ARCH_VALHALL_CSF)
		return "unknown Mali";

	/* The model of each known product. */
	switch (device->product) {
	case MALI_PRODUCT_G710:
		return "Mali-G710";
	case MALI_PRODUCT_G510:
		return "Mali-G510";
	case MALI_PRODUCT_G310:
		return "Mali-G310";
	case MALI_PRODUCT_G610:
		return "Mali-G610";
	default:
		break;
	}

	/* Succeeded: an architecture the driver knows, a product it does not name. */
	return "Mali Valhall";
}

/*
 * Resets the GPU's state machines and waits for the reset to complete.
 */
int
mali_gpu_soft_reset(
	struct mali_device *device)
{
	int error;

	/* Clears an old completion, then asks for the reset. */
	mali_write32(device, MALI_GPU_INT_BASE + MALI_INT_CLEAR, MALI_GPU_IRQ_RESET_DONE);
	mali_write32(device, MALI_REG_COMMAND, MALI_COMMAND_SOFT_RESET);

	/* Waits for the GPU to report the reset done. */
	error = mali_gpu_wait_raw(device, MALI_GPU_INT_BASE, MALI_GPU_IRQ_RESET_DONE, RESET_WAIT_MS);
	if (error != 0) {
		kern_logf("mali: soft reset did not complete (status %08x)\n",
			  (unsigned)mali_read32(device, MALI_REG_STATUS));
		return error;
	}

	/* Clears the completion it raised. */
	mali_write32(device, MALI_GPU_INT_BASE + MALI_INT_CLEAR, MALI_GPU_IRQ_RESET_DONE);

	/* Succeeded: the GPU is reset. */
	return 0;
}

/*
 * Chooses no coherency and powers the L2 cache on.
 *
 * When the GPU has several L2 slices only the first is asked for; the
 * hardware brings the others with it.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_gpu.c
 * (panthor_gpu_l2_power_on, panthor_gpu_block_power_on).
 */
int
mali_gpu_l2_power_on(
	struct mali_device *device)
{
	uint64_t mask;
	int error;

	/* The GPU does not snoop the processor's caches on this SoC. */
	mali_write32(device, MALI_REG_COHERENCY_PROTOCOL, MALI_COHERENCY_NONE);

	/* The first slice of the L2. */
	mask = 1U;
	if (device->l2_present == 0U)
		return ENODEV;

	/* Lets a change under way finish, then asks for the slice. */
	error = wait_ready(device, 0U, MALI_REG_L2_TRANSITION, mask, POWER_WAIT_MS);
	if (error != 0)
		return error;
	mali_write64(device, MALI_REG_L2_POWER_ON, mask);

	/* Waits for the slice to be ready. */
	error = wait_ready(device, MALI_REG_L2_READY, MALI_REG_L2_TRANSITION, mask, POWER_WAIT_MS);
	if (error != 0) {
		kern_logf("mali: L2 did not power on (ready %llx, transition %llx)\n",
			  (unsigned long long)mali_read64(device, MALI_REG_L2_READY),
			  (unsigned long long)mali_read64(device, MALI_REG_L2_TRANSITION));
		return error;
	}

	/* Succeeded: the L2 is on. */
	return 0;
}

/*
 * Powers every L2 slice off, after any change under way.
 */
void
mali_gpu_l2_power_off(
	struct mali_device *device)
{
	/* Lets a change under way finish, then asks for every slice off. */
	(void)wait_ready(device, 0U, MALI_REG_L2_TRANSITION, device->l2_present, POWER_WAIT_MS);
	mali_write64(device, MALI_REG_L2_POWER_OFF, device->l2_present);

	/* Waits, without a verdict, for the change to end. */
	(void)wait_ready(device, 0U, MALI_REG_L2_TRANSITION, device->l2_present, POWER_WAIT_MS);
}

/*
 * Cleans or invalidates the GPU's caches: the L2, the load-store caches
 * and the others, each by MALI_CACHE_CLEAN and MALI_CACHE_INVALIDATE.
 */
int
mali_gpu_flush_caches(
	struct mali_device *device,
	unsigned l2,
	unsigned load_store,
	unsigned other)
{
	uint32_t command;
	int error;

	/* The command and its payload. */
	command = MALI_COMMAND_FLUSH_CACHES;
	command |= (uint32_t)l2 << MALI_FLUSH_L2_SHIFT;
	command |= (uint32_t)load_store << MALI_FLUSH_LSC_SHIFT;
	command |= (uint32_t)other << MALI_FLUSH_OTHER_SHIFT;

	/* Clears an old completion, then asks for the flush. */
	mali_write32(device, MALI_GPU_INT_BASE + MALI_INT_CLEAR, MALI_GPU_IRQ_CACHES_CLEAN);
	mali_write32(device, MALI_REG_COMMAND, command);

	/* Waits for the GPU to report the flush done. */
	error = mali_gpu_wait_raw(device, MALI_GPU_INT_BASE, MALI_GPU_IRQ_CACHES_CLEAN, FLUSH_WAIT_MS);
	if (error != 0)
		return error;

	/* Clears the completion it raised. */
	mali_write32(device, MALI_GPU_INT_BASE + MALI_INT_CLEAR, MALI_GPU_IRQ_CACHES_CLEAN);

	/* Succeeded: the caches are flushed. */
	return 0;
}

/*
 * Takes the three interrupt lines and opens the GPU block's fault
 * interrupts and the MMU's address space faults.
 *
 * The job block stays masked until the firmware is running; its boot is
 * observed by polling.  The lines are level triggered, active high.
 */
int
mali_gpu_irq_start(
	struct mali_device *device)
{
	int error;

	/* Masks and clears everything first, so no old event raises a line. */
	mali_write32(device, MALI_GPU_INT_BASE + MALI_INT_MASK, 0U);
	mali_write32(device, MALI_JOB_INT_BASE + MALI_INT_MASK, 0U);
	mali_write32(device, MALI_MMU_INT_BASE + MALI_INT_MASK, 0U);
	mali_write32(device, MALI_GPU_INT_BASE + MALI_INT_CLEAR, 0xffffffffU);
	mali_write32(device, MALI_JOB_INT_BASE + MALI_INT_CLEAR, 0xffffffffU);
	mali_write32(device, MALI_MMU_INT_BASE + MALI_INT_CLEAR, 0xffffffffU);

	/* Takes each line. */
	error = take_line(device->irq_gpu, gpu_interrupt, device);
	if (error != 0)
		return error;

	error = take_line(device->irq_mmu, mmu_interrupt, device);
	if (error != 0)
		return error;

	error = take_line(device->irq_job, job_interrupt, device);
	if (error != 0)
		return error;

	/* Opens the faults of the GPU block and of every address space. */
	mali_write32(device, MALI_GPU_INT_BASE + MALI_INT_MASK, GPU_IRQ_TAKEN);
	mali_write32(device, MALI_MMU_INT_BASE + MALI_INT_MASK, MMU_IRQ_FAULTS);

	/* Succeeded: the lines are taken. */
	return 0;
}

/*
 * Writes, from the later stage's thread, what the interrupt handlers
 * recorded since the last report, and forgets it.
 */
void
mali_gpu_report_events(
	struct mali_device *device)
{
	uint32_t gpu;
	uint32_t faults;
	uint32_t status;
	uint64_t address;

	/* Takes the recorded words, leaving zero for the handlers. */
	gpu = __atomic_exchange_n(&device->gpu_events, 0U, __ATOMIC_ACQ_REL);
	faults = __atomic_exchange_n(&device->mmu_faults, 0U, __ATOMIC_ACQ_REL);
	status = __atomic_load_n(&device->fault_status, __ATOMIC_ACQUIRE);
	address = __atomic_load_n(&device->fault_address, __ATOMIC_ACQUIRE);

	/* A GPU fault: its status and address. */
	if ((gpu & MALI_GPU_IRQ_FAULT) != 0U) {
		kern_logf("mali: GPU fault %08x at %016llx\n",
			  (unsigned)mali_read32(device, MALI_REG_FAULT_STATUS),
			  (unsigned long long)mali_read64(device, MALI_REG_FAULT_ADDRESS));
	}

	/* A protected-mode fault, which the driver never asked for. */
	if ((gpu & MALI_GPU_IRQ_PROTECTED_FAULT) != 0U)
		kern_logf("mali: GPU fault in protected mode\n");

	/* Address space faults: the spaces, and the last one's status and address. */
	if (faults != 0U) {
		kern_logf("mali: MMU fault on spaces %04x, status %08x at %016llx\n",
			  (unsigned)faults,
			  (unsigned)status,
			  (unsigned long long)address);
	}
}

/*
 * Polls one interrupt block's raw status until every bit of a mask is
 * set; ETIMEDOUT when the time runs out first.
 */
int
mali_gpu_wait_raw(
	struct mali_device *device,
	uint32_t block,
	uint32_t mask,
	unsigned milliseconds)
{
	uint64_t deadline;
	uint64_t now;
	uint32_t raw;

	/* Reads, then sleeps a tick, until the bits are set or the time is spent. */
	deadline = sched_ticks() + kern_ms_to_ticks(milliseconds) + 1U;
	for (;;) {
		/* The bits are set: the event happened. */
		raw = mali_read32(device, block + MALI_INT_RAWSTAT);
		if ((raw & mask) == mask)
			return 0;

		/* The time is spent. */
		now = sched_ticks();
		if (now >= deadline)
			break;

		/* A millisecond before the next read. */
		sched_sleep(now + kern_ms_to_ticks(MALI_GPU_POLL_MS));
	}

	/* One last read, in case the event came during the last sleep. */
	raw = mali_read32(device, block + MALI_INT_RAWSTAT);
	if ((raw & mask) == mask)
		return 0;

	/* Succeeded in waiting, but the event did not come. */
	return ETIMEDOUT;
}

/*
 * Polls one kind of core's power until no core of a mask is changing and,
 * when ready names a register, every core of the mask is ready.
 */
static int
wait_ready(
	struct mali_device *device,
	uint32_t ready,
	uint32_t transition,
	uint64_t mask,
	unsigned milliseconds)
{
	uint64_t deadline;
	uint64_t now;
	uint64_t changing;
	uint64_t on;

	/* Reads, then sleeps a tick, until the cores settle or the time is spent. */
	deadline = sched_ticks() + kern_ms_to_ticks(milliseconds) + 1U;
	for (;;) {
		/* The cores of the mask that are changing, and those that are ready. */
		changing = mali_read64(device, transition) & mask;
		on = mask;
		if (ready != 0U)
			on = mali_read64(device, ready) & mask;

		/* Settled, and ready when asked. */
		if (changing == 0U && on == mask)
			return 0;

		/* The time is spent. */
		now = sched_ticks();
		if (now >= deadline)
			break;

		/* A millisecond before the next read. */
		sched_sleep(now + kern_ms_to_ticks(MALI_GPU_POLL_MS));
	}

	/* Succeeded in waiting, but the cores did not settle. */
	return ETIMEDOUT;
}

/* Records and clears the GPU block's faults. */
static void
gpu_interrupt(
	int irq,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	struct mali_device *device;
	uint32_t status;

	(void)irq;
	device = argument;

	/* What is pending and unmasked, cleared at the GPU so the level line drops. */
	status = mali_read32(device, MALI_GPU_INT_BASE + MALI_INT_STAT);
	mali_write32(device, MALI_GPU_INT_BASE + MALI_INT_CLEAR, status);
	__atomic_fetch_or(&device->gpu_events, status, __ATOMIC_ACQ_REL);

	kern_irq_send_eoi(acknowledge);
}

/* Records and clears the MMU's address space faults, with the lowest space's status and address. */
static void
mmu_interrupt(
	int irq,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	struct mali_device *device;
	uint32_t status;
	uint32_t base;
	unsigned space;

	(void)irq;
	device = argument;

	/* What is pending and unmasked. */
	status = mali_read32(device, MALI_MMU_INT_BASE + MALI_INT_STAT) & MMU_IRQ_FAULTS;

	/* The lowest faulting space's status and address, for the report. */
	for (space = 0U; space < 16U; space++) {
		if ((status & (1U << space)) != 0U)
			break;
	}

	/* A space faulted: keeps its status and address before clearing. */
	if (space < 16U) {
		base = MALI_AS_BASE + space * MALI_AS_STRIDE;
		__atomic_store_n(&device->fault_status, mali_read32(device, base + MALI_AS_FAULT_STATUS), __ATOMIC_RELEASE);
		__atomic_store_n(&device->fault_address, mali_read64(device, base + MALI_AS_FAULT_ADDRESS), __ATOMIC_RELEASE);
	}

	/*
	 * Clears the faults so the level line drops.  A faulting space stays
	 * stalled until its tables are fixed; the driver only reports it.
	 */
	mali_write32(device, MALI_MMU_INT_BASE + MALI_INT_CLEAR, status);
	__atomic_fetch_or(&device->mmu_faults, status, __ATOMIC_ACQ_REL);

	kern_irq_send_eoi(acknowledge);
}

/* Records and clears the job block's firmware events. */
static void
job_interrupt(
	int irq,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	struct mali_device *device;
	uint32_t status;

	(void)irq;
	device = argument;

	/* What is pending and unmasked, cleared so the level line drops. */
	status = mali_read32(device, MALI_JOB_INT_BASE + MALI_INT_STAT);
	mali_write32(device, MALI_JOB_INT_BASE + MALI_INT_CLEAR, status);
	__atomic_fetch_or(&device->job_events, status, __ATOMIC_ACQ_REL);

	kern_irq_send_eoi(acknowledge);
}

/* Sets one line level triggered, active high, and installs its handler. */
static int
take_line(
	int irq,
	kern_irq_handler_t handler,
	struct mali_device *device)
{
	int error;

	/* The device tree says level, active high, for all three lines. */
	error = kern_irq_set_mode(irq, KERN_IRQ_TRIGGER_LEVEL, KERN_IRQ_POLARITY_HIGH);
	if (error != 0 && error != ENOTSUP) {
		kern_logf("mali: interrupt %d mode not set (%d)\n", irq, error);
		return error;
	}

	/* Installs the handler. */
	error = kern_irq_register(irq, handler, device);
	if (error != 0) {
		kern_logf("mali: interrupt %d not taken (%d)\n", irq, error);
		return error;
	}

	/* Succeeded: the line is the driver's. */
	return 0;
}
