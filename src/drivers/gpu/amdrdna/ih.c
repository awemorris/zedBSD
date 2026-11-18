/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GPU's interrupt ring.
 *
 * Every block of the GPU reports its events to the interrupt controller,
 * which writes each one as a 32-byte entry into a ring in system memory,
 * copies its write pointer into a word of system memory, and signals the
 * CPU with a message.  The message handler only notes the signal and wakes
 * the interrupt thread; the thread reads the entries up to the copied
 * write pointer, hands each to the block it belongs to, and rings the
 * controller's doorbell with how far it read.  Stage A2 prepares the ring
 * and the thread with the ring off; stage A3 turns it on.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/generic/dma.h>
#include <drivers/pci/pci.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <kern/waitq.h>
#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-private.h"
#include "drivers/gpu/amdrdna/amdrdna-regs.h"

/* The alignment of the ring and of the pointer's page. */
#define INTERRUPT_ALIGNMENT		4096U

/* The bytes of the page that holds the copy of the write pointer. */
#define INTERRUPT_POINTER_BYTES		4096U

/* How many times the thread reads the ring again when entries keep coming. */
#define INTERRUPT_ROUNDS		8U

/* How many unknown events the log names before it stays quiet. */
#define INTERRUPT_NAMED_UNKNOWN		8U

/* How long the stop waits for the thread to end, and its step. */
#define INTERRUPT_STOP_WAIT_US		200000U
#define INTERRUPT_STOP_STEP_US		10000U

/* How long the quiet before a reset gives a read under way to end. */
#define INTERRUPT_QUIET_US		20000U

static int ih_allocate(struct amdrdna_device *device);
static int ih_signal_setup(struct amdrdna_device *device);
static void ih_registers(struct amdrdna_device *device, uint32_t ring_word);
static int ih_signal(void *argument);
static void ih_thread(void *argument);
static void ih_drain(struct amdrdna_device *device);
static uint32_t ih_write_offset(struct amdrdna_device *device);
static void ih_dispatch(struct amdrdna_device *device, const struct amdrdna_interrupt *interrupt);
static uint32_t ih_read(struct amdrdna_device *device, uint32_t offset);
static void ih_write(struct amdrdna_device *device, uint32_t offset, uint32_t value);

/*
 * Prepares the interrupt ring with the ring off: the ring and the write
 * pointer's copy in system memory mapped for the GPU, the message
 * interrupt and its handler, the controller's and the host bridge's
 * registers, the doorbell, and the interrupt thread.
 *
 * Returns 0, or the error of the step that failed.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/navi10_ih.c (navi10_ih_irq_init without its
 * last step, which turns the ring on).
 */
int
amdrdna_ih_prepare(
	struct amdrdna_device *device)
{
	struct amdrdna_interrupts *interrupts;
	struct thread *thread;
	const char *kind;
	uint32_t ring_word;
	uint32_t word;
	int error;

	/* The ring's state. */
	interrupts = &device->interrupts;
	interrupts->ring_bytes = AMDRDNA_INTERRUPT_RING_BYTES;
	interrupts->read_offset = 0U;

	/* Allocates the ring and the pointer's page and maps them for the GPU. */
	error = ih_allocate(device);
	if (error != 0)
		return error;

	/* Sets up the message interrupt and its handler. */
	error = ih_signal_setup(device);
	if (error != 0)
		return error;

	/* The ring's control word for this ring and this kind of interrupt. */
	word = ih_read(device, AMDRDNA_IH_REG_RING_CONTROL);
	error = amdrdna_interrupt_ring_word(word, interrupts->ring_bytes, interrupts->message, &ring_word);
	if (error != 0)
		return error;

	/* Writes the controller's and the host bridge's registers, the ring left off. */
	ih_registers(device, ring_word);

	/* Lets the GPU write to system memory and signal the CPU. */
	error = drv_pci_device_set_bus_master(device->pci, true);
	if (error != 0) {
		amdrdna_mark("A2 fail: bus mastering not enabled (%d)", error);
		return error;
	}

	/* Creates the interrupt thread. */
	error = kthread_create(ih_thread, device, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0)
		return error;

	/*
	 * thread_running tells the stop that the thread may still read the
	 * ring; the thread clears it last.  Nobody joins the thread.
	 */
	interrupts->thread_running = true;
	thread->detached = 1;
	thread_start(thread);
	interrupts->prepared = true;

	/* Names the kind of message interrupt for the mark. */
	kind = "msi";
	if (interrupts->irq.type == DRV_PCI_IRQ_MSIX)
		kind = "msix";

	/* Marks the ring. */
	amdrdna_mark("A2 ih ring=0x%llx bus=0x%llx pointer=0x%llx %uKiB irq=%s vector=%u doorbell=0x%x control=0x%08x",
	    (unsigned long long)interrupts->ring.gpu_address,
	    (unsigned long long)interrupts->ring.dma.device_address,
	    (unsigned long long)interrupts->pointer.gpu_address,
	    (unsigned)(interrupts->ring_bytes >> 10),
	    kind,
	    interrupts->irq.vector,
	    (unsigned)AMDRDNA_DOORBELL_INTERRUPTS,
	    (unsigned)ih_read(device, AMDRDNA_IH_REG_RING_CONTROL));

	/* Succeeded: the ring is ready to be turned on. */
	return 0;
}

/*
 * Turns the interrupt ring and its message interrupt on (stage A3), and
 * lets the thread read whatever the controller writes from now on.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/navi10_ih.c (navi10_ih_toggle_interrupts
 * with true).
 */
void
amdrdna_ih_enable(
	struct amdrdna_device *device)
{
	struct amdrdna_interrupts *interrupts;
	unsigned long irq;
	uint32_t word;

	/* A ring that was never prepared stays off. */
	interrupts = &device->interrupts;
	if (!interrupts->prepared)
		return;

	/* The thread reads the ring from the moment the controller may write it. */
	interrupts->enabled = true;

	/* Turns the ring and its interrupt on. */
	word = ih_read(device, AMDRDNA_IH_REG_RING_CONTROL);
	word = amdrdna_interrupt_switch_word(word, true);
	ih_write(device, AMDRDNA_IH_REG_RING_CONTROL, word);

	/* Wakes the thread once, for anything written before the first message. */
	irq = spin_lock_irqsave(&interrupts->lock);

	interrupts->pending = true;
	waitq_wake_all(&interrupts->waitq);

	spin_unlock_irqrestore(&interrupts->lock, irq);
}

/*
 * Stops the interrupt ring and the interrupt thread for the shutdown of
 * the machine: the ring off and its pointers at zero, then the thread
 * told to end and waited for, for a bounded time.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/navi10_ih.c (navi10_ih_toggle_interrupts
 * with false).
 */
void
amdrdna_ih_stop(
	struct amdrdna_device *device)
{
	struct amdrdna_interrupts *interrupts;
	unsigned long irq;
	unsigned waited;
	uint32_t word;
	bool running;

	/* A ring that was never prepared has nothing to stop. */
	interrupts = &device->interrupts;
	if (!interrupts->prepared)
		return;

	/* Turns the ring and its interrupt off and zeroes its pointers. */
	word = ih_read(device, AMDRDNA_IH_REG_RING_CONTROL);
	word = amdrdna_interrupt_switch_word(word, false);
	ih_write(device, AMDRDNA_IH_REG_RING_CONTROL, word);
	ih_write(device, AMDRDNA_IH_REG_READ_POINTER, 0U);
	ih_write(device, AMDRDNA_IH_REG_WRITE_POINTER, 0U);
	interrupts->enabled = false;

	/* Tells the thread to end. */
	irq = spin_lock_irqsave(&interrupts->lock);

	interrupts->stopping = true;
	waitq_wake_all(&interrupts->waitq);

	spin_unlock_irqrestore(&interrupts->lock, irq);

	/* Waits for the thread to end, for a bounded time. */
	for (waited = 0U; waited < INTERRUPT_STOP_WAIT_US; waited += INTERRUPT_STOP_STEP_US) {
		/* Reads whether the thread still runs. */
		irq = spin_lock_irqsave(&interrupts->lock);

		running = interrupts->thread_running;

		spin_unlock_irqrestore(&interrupts->lock, irq);

		/* Stops waiting once it has ended. */
		if (!running)
			return;

		/* Gives the thread a step to finish. */
		kern_usleep_range(INTERRUPT_STOP_STEP_US, INTERRUPT_STOP_STEP_US * 2U);
	}

	/* The thread is still reading; the reset follows anyway. */
	amdrdna_mark("ih: the interrupt thread still runs; left to the reset");
}

/*
 * Turns the interrupt ring off before a reset of the GPU, leaving its
 * thread and its buffers: the ring and its message off, its pointers at
 * zero, and the thread told to read nothing more.  A read already under
 * way is given a moment to end.
 */
void
amdrdna_ih_quiet(
	struct amdrdna_device *device)
{
	struct amdrdna_interrupts *interrupts;
	uint32_t word;

	/* A ring that was never prepared has nothing to turn off. */
	interrupts = &device->interrupts;
	if (!interrupts->prepared)
		return;

	/* The thread reads nothing more; then the ring and its interrupt off, its pointers at zero. */
	interrupts->enabled = false;
	word = ih_read(device, AMDRDNA_IH_REG_RING_CONTROL);
	word = amdrdna_interrupt_switch_word(word, false);
	ih_write(device, AMDRDNA_IH_REG_RING_CONTROL, word);
	ih_write(device, AMDRDNA_IH_REG_READ_POINTER, 0U);
	ih_write(device, AMDRDNA_IH_REG_WRITE_POINTER, 0U);

	/* A read the thread had begun ends within its bounded rounds. */
	kern_usleep_range(INTERRUPT_QUIET_US, INTERRUPT_QUIET_US + INTERRUPT_QUIET_US / 4U);
}

/*
 * Sets the interrupt ring up again after a reset of the GPU, with the
 * buffers, the message interrupt and the thread stage A2 made: the ring
 * and the pointer's copy zeroed, the controller's and the host bridge's
 * registers written as the stage writes them, the ring left off (stage
 * A3 turns it on), and the GPU's bus mastering on.
 *
 * Returns 0, EBUSY for a ring that was never prepared, or the error of
 * the step that failed.
 */
int
amdrdna_ih_restart(
	struct amdrdna_device *device)
{
	struct amdrdna_interrupts *interrupts;
	uint32_t ring_word;
	uint32_t word;
	int error;

	/* Only a prepared ring is set up again. */
	interrupts = &device->interrupts;
	if (!interrupts->prepared)
		return EBUSY;

	/* Nothing read yet; the buffers zeroed. */
	interrupts->enabled = false;
	interrupts->read_offset = 0U;
	kern_memset(interrupts->ring.dma.address, 0, interrupts->ring_bytes);
	kern_memset(interrupts->pointer.dma.address, 0, INTERRUPT_POINTER_BYTES);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* The ring's control word, as the stage made it. */
	word = ih_read(device, AMDRDNA_IH_REG_RING_CONTROL);
	error = amdrdna_interrupt_ring_word(word, interrupts->ring_bytes, interrupts->message, &ring_word);
	if (error != 0)
		return error;

	/* The registers, the ring left off. */
	ih_registers(device, ring_word);

	/* The GPU may write to system memory and signal the CPU again. */
	error = drv_pci_device_set_bus_master(device->pci, true);
	if (error != 0)
		return error;

	/* Succeeded: the ring is ready to be turned on. */
	return 0;
}

/* Allocates the ring and the write pointer's page in system memory and maps both for the GPU. */
static int
ih_allocate(
	struct amdrdna_device *device)
{
	struct amdrdna_interrupts *interrupts;
	int error;

	/* Allocates the ring. */
	interrupts = &device->interrupts;
	error = drv_dma_alloc_coherent(device->dma, interrupts->ring_bytes, INTERRUPT_ALIGNMENT, &interrupts->ring.dma);
	if (error != 0) {
		amdrdna_mark("A2 fail: interrupt ring not allocated (%d)", error);
		return error;
	}

	/* Clears it; the driver keeps it for the GPU's lifetime. */
	interrupts->ring.allocated = true;
	kern_memset(interrupts->ring.dma.address, 0, interrupts->ring_bytes);

	/* Allocates the page the controller copies its write pointer to. */
	error = drv_dma_alloc_coherent(device->dma, INTERRUPT_POINTER_BYTES, INTERRUPT_ALIGNMENT, &interrupts->pointer.dma);
	if (error != 0) {
		amdrdna_mark("A2 fail: interrupt pointer page not allocated (%d)", error);
		return error;
	}

	/* Clears it; the driver keeps it for the GPU's lifetime. */
	interrupts->pointer.allocated = true;
	kern_memset(interrupts->pointer.dma.address, 0, INTERRUPT_POINTER_BYTES);

	/* Maps the ring for the GPU. */
	error = amdrdna_gart_map(device, &interrupts->ring, INTERRUPT_ALIGNMENT);
	if (error != 0) {
		amdrdna_mark("A2 fail: interrupt ring not mapped (%d)", error);
		return error;
	}

	/* Maps the pointer's page for the GPU. */
	error = amdrdna_gart_map(device, &interrupts->pointer, INTERRUPT_ALIGNMENT);
	if (error != 0) {
		amdrdna_mark("A2 fail: interrupt pointer page not mapped (%d)", error);
		return error;
	}

	/* Succeeded: the GPU reaches the ring and the pointer's page. */
	return 0;
}

/* Takes one message interrupt and puts the handler on it. */
static int
ih_signal_setup(
	struct amdrdna_device *device)
{
	struct amdrdna_interrupts *interrupts;
	int error;

	/* Takes one message interrupt, extended or plain. */
	interrupts = &device->interrupts;
	error = drv_pci_device_allocate_irqs(device->pci,
	    DRV_PCI_IRQ_ALLOW_MSIX | DRV_PCI_IRQ_ALLOW_MSI,
	    1U,
	    1U,
	    &interrupts->irq,
	    &interrupts->irq_count);
	if (error != 0 || interrupts->irq_count != 1U) {
		amdrdna_mark("A2 fail: no message interrupt (%d, %u)", error, interrupts->irq_count);
		return ENODEV;
	}

	/* A message interrupt is re-armed by the read-pointer write. */
	interrupts->message = true;

	/* Puts the handler on it. */
	error = drv_pci_device_establish_irq(device->pci, &interrupts->irq, ih_signal, device, "amdrdna", &interrupts->irq_cookie);
	if (error != 0) {
		amdrdna_mark("A2 fail: interrupt handler not set (%d)", error);
		return error;
	}

	/* Succeeded: the handler wakes the thread on each message. */
	return 0;
}

/*
 * Writes the controller's and the host bridge's registers for the ring:
 * the ring off, the host bridge's dummy page and interrupt control, the
 * ring's base and control word, the address of the pointer's copy, both
 * pointers at zero, the doorbell and its range.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/navi10_ih.c (navi10_ih_irq_init,
 * navi10_ih_enable_ring) and drivers/gpu/drm/amd/amdgpu/nbio_v2_3.c
 * (nbio_v2_3_ih_control, nbio_v2_3_ih_doorbell_range).
 */
static void
ih_registers(
	struct amdrdna_device *device,
	uint32_t ring_word)
{
	struct amdrdna_interrupts *interrupts;
	uint64_t ring;
	uint64_t pointer;
	uint32_t word;

	/* Turns the ring and its interrupt off and zeroes its pointers. */
	interrupts = &device->interrupts;
	word = ih_read(device, AMDRDNA_IH_REG_RING_CONTROL);
	word = amdrdna_interrupt_switch_word(word, false);
	ih_write(device, AMDRDNA_IH_REG_RING_CONTROL, word);
	ih_write(device, AMDRDNA_IH_REG_READ_POINTER, 0U);
	ih_write(device, AMDRDNA_IH_REG_WRITE_POINTER, 0U);

	/* The host bridge's dummy page and its interrupt control for a message interrupt. */
	word = (uint32_t)(device->dummy_page.dma.device_address >> AMDRDNA_INTERRUPT_DUMMY_SHIFT);
	amdrdna_block_write(device, AMDRDNA_BLOCK_NBIO, AMDRDNA_NBIO_WINDOW_HOST, AMDRDNA_NBIO_REG_INTERRUPT_DUMMY, word);
	word = amdrdna_block_read(device, AMDRDNA_BLOCK_NBIO, AMDRDNA_NBIO_WINDOW_HOST, AMDRDNA_NBIO_REG_INTERRUPT_CONTROL);
	word = amdrdna_interrupt_control_word(word);
	amdrdna_block_write(device, AMDRDNA_BLOCK_NBIO, AMDRDNA_NBIO_WINDOW_HOST, AMDRDNA_NBIO_REG_INTERRUPT_CONTROL, word);

	/* The ring's base, address bits 39:8 and 47:40. */
	ring = interrupts->ring.gpu_address;
	ih_write(device, AMDRDNA_IH_REG_RING_BASE, (uint32_t)(ring >> AMDRDNA_IH_RING_BASE_SHIFT));
	ih_write(device, AMDRDNA_IH_REG_RING_BASE_HIGH, (uint32_t)(ring >> AMDRDNA_IH_RING_BASE_HIGH_SHIFT) & AMDRDNA_IH_RING_BASE_HIGH_MASK);

	/* The ring's control word, the ring still off. */
	ih_write(device, AMDRDNA_IH_REG_RING_CONTROL, ring_word);

	/* Where the controller copies its write pointer. */
	pointer = interrupts->pointer.gpu_address;
	ih_write(device, AMDRDNA_IH_REG_POINTER_COPY_LOW, (uint32_t)pointer);
	ih_write(device, AMDRDNA_IH_REG_POINTER_COPY_HIGH, (uint32_t)(pointer >> 32) & AMDRDNA_IH_POINTER_COPY_HIGH_MASK);

	/* Both pointers at zero. */
	ih_write(device, AMDRDNA_IH_REG_WRITE_POINTER, 0U);
	ih_write(device, AMDRDNA_IH_REG_READ_POINTER, 0U);

	/* The read-pointer doorbell, and the host bridge's route for it. */
	word = amdrdna_interrupt_doorbell_word(AMDRDNA_DOORBELL_INTERRUPTS);
	ih_write(device, AMDRDNA_IH_REG_DOORBELL, word);
	word = amdrdna_block_read(device, AMDRDNA_BLOCK_NBIO, AMDRDNA_NBIO_WINDOW_HOST, AMDRDNA_NBIO_REG_INTERRUPT_DOORBELLS);
	word = amdrdna_interrupt_doorbell_range_word(word, AMDRDNA_DOORBELL_INTERRUPTS);
	amdrdna_block_write(device, AMDRDNA_BLOCK_NBIO, AMDRDNA_NBIO_WINDOW_HOST, AMDRDNA_NBIO_REG_INTERRUPT_DOORBELLS, word);
}

/*
 * Notes a message from the interrupt controller and wakes the interrupt
 * thread.  The message belongs to this GPU alone, so it is always taken.
 */
static int
ih_signal(
	void *argument)
{
	struct amdrdna_device *device;
	struct amdrdna_interrupts *interrupts;
	unsigned long irq;

	/* The GPU the message comes from. */
	device = argument;
	interrupts = &device->interrupts;

	/* Notes the message and wakes the thread. */
	irq = spin_lock_irqsave(&interrupts->lock);

	interrupts->pending = true;
	interrupts->signals++;
	waitq_wake_all(&interrupts->waitq);

	spin_unlock_irqrestore(&interrupts->lock, irq);

	/* Succeeded: the message is taken. */
	return 1;
}

/* Reads the ring whenever a message came, until told to stop. */
static void
ih_thread(
	void *argument)
{
	struct amdrdna_device *device;
	struct amdrdna_interrupts *interrupts;
	unsigned long irq;
	uint64_t sequence;

	/* The GPU whose ring the thread reads. */
	device = argument;
	interrupts = &device->interrupts;

	/* Reads the ring once per message. */
	for (;;) {
		/* Sleeps until a message came or the ring stops. */
		irq = spin_lock_irqsave(&interrupts->lock);

		while (!interrupts->pending && !interrupts->stopping) {
			sequence = waitq_sequence(&interrupts->waitq);
			(void)waitq_sleep(&interrupts->waitq, &interrupts->lock, sequence, 0U, 0U);
		}

		/* Leaves the loop, with the lock let go, when the ring stops. */
		if (interrupts->stopping) {
			spin_unlock_irqrestore(&interrupts->lock, irq);
			break;
		}

		/* Takes the message. */
		interrupts->pending = false;

		spin_unlock_irqrestore(&interrupts->lock, irq);

		/* Reads the entries that came. */
		ih_drain(device);
	}

	/* Tells the stop that the thread no longer reads the ring. */
	irq = spin_lock_irqsave(&interrupts->lock);

	interrupts->thread_running = false;

	spin_unlock_irqrestore(&interrupts->lock, irq);
}

/*
 * Reads the ring's entries up to the controller's write pointer, hands each
 * to its block, and rings the doorbell with how far it read; reads again
 * while entries keep coming, a bounded number of times (the next message
 * brings the thread back).
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_ih.c (amdgpu_ih_process) and
 * navi10_ih.c (navi10_ih_set_rptr).
 */
static void
ih_drain(
	struct amdrdna_device *device)
{
	struct amdrdna_interrupts *interrupts;
	struct amdrdna_interrupt interrupt;
	const volatile uint32_t *ring;
	uint32_t words[AMDRDNA_INTERRUPT_ENTRY_BYTES / 4U];
	uint32_t write_offset;
	unsigned round;
	unsigned word;
	uint8_t *doorbells;

	/* A ring that is off has nothing to read. */
	interrupts = &device->interrupts;
	if (!interrupts->enabled)
		return;

	/* Reads up to the write pointer, again while it moves. */
	ring = interrupts->ring.dma.address;
	doorbells = device->doorbells.address;
	for (round = 0U; round < INTERRUPT_ROUNDS; round++) {
		/* Stops once the thread has read everything. */
		write_offset = ih_write_offset(device);
		if (write_offset == interrupts->read_offset)
			break;

		/* Orders the pointer's read before the entries' reads. */
		__atomic_thread_fence(__ATOMIC_ACQUIRE);

		/* Hands every entry up to the pointer to its block. */
		while (interrupts->read_offset != write_offset) {
			/* Copies the entry out of the ring and decodes it. */
			for (word = 0U; word < AMDRDNA_INTERRUPT_ENTRY_BYTES / 4U; word++)
				words[word] = ring[interrupts->read_offset / 4U + word];
			amdrdna_interrupt_decode(words, &interrupt);
			interrupts->entries++;

			/* Hands it on, and steps past it. */
			ih_dispatch(device, &interrupt);
			interrupts->read_offset = (interrupts->read_offset + AMDRDNA_INTERRUPT_ENTRY_BYTES) & (interrupts->ring_bytes - 1U);
		}

		/* Tells the controller how far the thread read; this also re-arms the message. */
		kern_mmio_write32(doorbells + AMDRDNA_DOORBELL_INTERRUPTS * 4U, interrupts->read_offset);
	}
}

/*
 * Reads the controller's write pointer from its copy in memory.  After an
 * overflow the pointer is read from the register instead, the reading
 * restarts at the oldest entry not overwritten, and the overflow is
 * acknowledged.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/navi10_ih.c (navi10_ih_get_wptr).
 */
static uint32_t
ih_write_offset(
	struct amdrdna_device *device)
{
	struct amdrdna_interrupts *interrupts;
	const volatile uint32_t *copy;
	uint32_t pointer;
	uint32_t offset;
	uint32_t word;
	uint32_t acknowledge;
	bool overflowed;

	/* The copy in memory. */
	interrupts = &device->interrupts;
	copy = interrupts->pointer.dma.address;
	pointer = *copy;
	offset = amdrdna_interrupt_offset(pointer, interrupts->ring_bytes, &overflowed);
	if (!overflowed)
		return offset;

	/* The register says whether the overflow still stands. */
	pointer = ih_read(device, AMDRDNA_IH_REG_WRITE_POINTER);
	offset = amdrdna_interrupt_offset(pointer, interrupts->ring_bytes, &overflowed);
	if (!overflowed)
		return offset;

	/* Restarts past the entry the controller overwrote last. */
	interrupts->read_offset = (offset + AMDRDNA_INTERRUPT_ENTRY_BYTES) & (interrupts->ring_bytes - 1U);
	interrupts->overflows++;
	amdrdna_mark("ih: ring overflow at 0x%x, reading from 0x%x", (unsigned)offset, (unsigned)interrupts->read_offset);

	/* Acknowledges the overflow, then clears the acknowledgement so the next one is seen. */
	word = ih_read(device, AMDRDNA_IH_REG_RING_CONTROL);
	acknowledge = amdrdna_interrupt_overflow_word(word, true);
	ih_write(device, AMDRDNA_IH_REG_RING_CONTROL, acknowledge);
	acknowledge = amdrdna_interrupt_overflow_word(word, false);
	ih_write(device, AMDRDNA_IH_REG_RING_CONTROL, acknowledge);

	/* Succeeded: the offset of the next entry the controller writes. */
	return offset;
}

/* Hands one entry to the block it belongs to. */
static void
ih_dispatch(
	struct amdrdna_device *device,
	const struct amdrdna_interrupt *interrupt)
{
	struct amdrdna_interrupts *interrupts;

	/* A page fault of either memory hub. */
	interrupts = &device->interrupts;
	if (interrupt->source == AMDRDNA_SOURCE_PAGE_FAULT) {
		if (interrupt->client == AMDRDNA_CLIENT_MEDIA_HUB || interrupt->client == AMDRDNA_CLIENT_GRAPHICS_HUB) {
			amdrdna_vm_fault(device, interrupt);
			return;
		}
	}

	/* An event of the command processor or of the first copy engine. */
	if (interrupt->client == AMDRDNA_CLIENT_COMMAND || interrupt->client == AMDRDNA_CLIENT_COPY0) {
		amdrdna_engines_interrupt(device, interrupt);
		return;
	}

	/* A fence's interrupt of the video engine. */
	if (interrupt->client == AMDRDNA_CLIENT_VIDEO) {
		amdrdna_vcn_interrupt(device, interrupt);
		return;
	}

	/* A frame's start, a finished flip or a hot-plug line of the display engine. */
	if (interrupt->client == AMDRDNA_CLIENT_DISPLAY) {
		amdrdna_display_interrupt(device, interrupt);
		return;
	}

	/* A thermal alert, the critical-temperature trip, or the power unit's software interrupt. */
	if (interrupt->client == AMDRDNA_CLIENT_THERMAL ||
	    interrupt->client == AMDRDNA_CLIENT_SMU_IO ||
	    interrupt->client == AMDRDNA_CLIENT_SMU) {
		amdrdna_thermal_interrupt(device, interrupt);
		return;
	}

	/* Counts an event no block of this stage takes, and names the first few. */
	interrupts->unknown++;
	if (interrupts->unknown > INTERRUPT_NAMED_UNKNOWN)
		return;
	amdrdna_mark("ih: unhandled client=0x%x source=0x%x ring=%u ctx=%u data=0x%08x 0x%08x 0x%08x 0x%08x",
	    (unsigned)interrupt->client,
	    (unsigned)interrupt->source,
	    (unsigned)interrupt->ring,
	    (unsigned)interrupt->context,
	    (unsigned)interrupt->data[0],
	    (unsigned)interrupt->data[1],
	    (unsigned)interrupt->data[2],
	    (unsigned)interrupt->data[3]);
}

/* Reads one of the interrupt controller's registers. */
static uint32_t
ih_read(
	struct amdrdna_device *device,
	uint32_t offset)
{
	uint32_t value;

	/* The controller's registers are in its block's first window. */
	value = amdrdna_block_read(device, AMDRDNA_BLOCK_IH, AMDRDNA_IH_WINDOW_MAIN, offset);

	/* Succeeded: the register's value. */
	return value;
}

/* Writes one of the interrupt controller's registers. */
static void
ih_write(
	struct amdrdna_device *device,
	uint32_t offset,
	uint32_t value)
{
	/* The controller's registers are in its block's first window. */
	amdrdna_block_write(device, AMDRDNA_BLOCK_IH, AMDRDNA_IH_WINDOW_MAIN, offset, value);
}
