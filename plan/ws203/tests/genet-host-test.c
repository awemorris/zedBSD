/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A GENET register/PHY model exercising the unmodified production driver.
 * It checks payload ownership, finite waits, ring counters, and lifecycle;
 * it does not emulate analogue PHY timing or replace real hardware testing.
 */

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drivers/platform/rpi4/rpi4-ethernet.h"
#include <drivers/generic/dma.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/lock.h>
#include <kern/net/net-device.h>
#include <kern/net/packet-buf.h>
#include <kern/pmem.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <uapi/errno.h>

/* The opaque allocator only records whether it still owns DMA runs. */
struct drv_dma_device {
	unsigned live;
};

/* The host tracks a scheduled thread without running its infinite service loop. */
struct thread {
	unsigned started;
};

/* All model state belongs to this single-threaded host test. */
struct genet_model {
	uint8_t *tree;
	size_t tree_size;
	uint32_t registers[0x10000U / 4U];
	uint16_t phy[32];
	struct drv_dma_device dma;
	struct net_device net;
	struct thread thread;
	void *rx;
	void *tx;
	kern_irq_handler_t handler;
	void *handler_argument;
	uint64_t counter;
	uint64_t ticks;
	unsigned allocations;
	unsigned freed_packets;
	unsigned received;
	unsigned scheduled;
	unsigned eoi;
	unsigned lock_depth;
	unsigned irq_masked;
	unsigned irq_unregistered;
	unsigned mapped;
	unsigned carrier;
	unsigned mdio_stuck;
	unsigned counter_stuck;
	unsigned counter_missing;
	unsigned pool_empty;
	unsigned fail_allocation;
	unsigned fail_irq;
	uint8_t frame[2048];
	size_t frame_length;
};

/* The production accessors interact only with this model's mapping and DMA. */
static struct genet_model model;

static struct packet_buf *make_packet(unsigned length);
static void reset_model(void);
static void test_description(void);
static void test_failures(void);
static void test_packets(void);
static void inject_receive(unsigned slot, unsigned length, uint32_t flags);

/*
 * Runs bounded discovery, failure, traffic, and lifecycle checks.
 */
int
main(
	int argc,
	char **argv)
{
	FILE *file;
	long size;
	size_t bytes;
	int error;

	/* Requires the firmware's actual Pi 4 device tree as the test input. */
	assert(argc == 2);
	file = fopen(argv[1], "rb");
	assert(file != NULL);
	error = fseek(file, 0, SEEK_END);
	assert(error == 0);
	size = ftell(file);
	assert(size > 0);
	error = fseek(file, 0, SEEK_SET);
	assert(error == 0);
	model.tree = malloc((size_t)size);
	assert(model.tree != NULL);
	model.tree_size = (size_t)size;
	bytes = fread(model.tree, 1, model.tree_size, file);
	assert(bytes == model.tree_size);
	error = fclose(file);
	assert(error == 0);

	/* Exercises firmware interpretation before the driver mutates hardware. */
	test_description();
	test_failures();
	test_packets();

	/* Gives the host allocations back after production close stopped both rings. */
	free(model.rx);
	free(model.tx);
	free(model.tree);
	puts(
		"ws203: FDT / attach failures / PHY timeout-recovery / TX " "wrap-backpressure / RX drops-budget / IRQ / close-reopen PASS");

	/* Succeeded: every model assertion held on the production driver path. */
	return 0;
}

/*
 * Supplies the read-only FDT handoff mapping requested by production attach.
 */
void *
kern_pmem_to_kernel(
	hal_physaddr_t physical)
{
	/* Requires the exact handoff address used by the test. */
	assert(physical == 0x100000U);

	/* Reports the firmware blob owned by the host fixture. */
	return model.tree;
}

/*
 * Maps the GENET range described by the real firmware tree.
 */
int
kern_device_map(
	uint64_t physical,
	size_t size,
	unsigned attributes,
	void **mapped)
{
	/* Requires the CPU address after SCB translation, not the bus alias. */
	assert(physical == 0xfd580000U);
	assert(size == sizeof(model.registers));
	assert(attributes == KERN_DEVICE_UNCACHED);
	assert(model.mapped == 0);
	model.mapped = 1;
	*mapped = model.registers;

	/* Succeeded: accesses now target the register model. */
	return 0;
}

/*
 * Unmaps a failed attachment after its IRQ and DMA ownership retire.
 */
int
kern_device_unmap(
	void *mapped,
	size_t size)
{
	/* Detects leaked DMA or handler ownership on initialization failure. */
	assert(mapped == model.registers);
	assert(size == sizeof(model.registers));
	assert(model.allocations == 0);
	assert(model.handler == NULL);
	model.mapped = 0;

	/* Succeeded: the model can be attached again. */
	return 0;
}

/*
 * Reads a register from the mapped controller model.
 */
uint32_t
kern_mmio_read32(
	const volatile void *address)
{
	size_t offset;

	/* Bounds every production MMIO read by the controller's actual mapping. */
	offset = (const volatile uint8_t *)address -
		 (const volatile uint8_t *)model.registers;
	assert(offset < sizeof(model.registers));
	assert((offset & 3U) == 0);

	/* Reports the modelled hardware register. */
	return model.registers[offset / 4U];
}

/*
 * Executes ordered controller writes, including clause-22 MDIO transactions.
 */
void
kern_mmio_write32(
	volatile void *address,
	uint32_t bits)
{
	size_t offset;
	unsigned reg;

	/* Bounds every production MMIO write before applying side effects. */
	offset =
	    (volatile uint8_t *)address - (volatile uint8_t *)model.registers;
	assert(offset < sizeof(model.registers));
	assert((offset & 3U) == 0);
	model.registers[offset / 4U] = bits;

	/* Emulates the primary interrupt bank's write-one commands. */
	if (offset == 0x208U)
		model.registers[0x200U / 4U] &= ~bits;

	/* Tracks interrupt masks to detect an incorrectly armed close. */
	if (offset == 0x210U)
		model.registers[0x20cU / 4U] |= bits;

	/* Clears only the enabled sources selected by the driver. */
	if (offset == 0x214U)
		model.registers[0x20cU / 4U] &= ~bits;

	/* Executes only the model's clause-22 command register. */
	if (offset != 0xe14U || (bits & (1U << 29)) == 0)
		return;

	/* Leaves BUSY set to exercise the original production timeout path. */
	if (model.mdio_stuck != 0)
		return;

	/* Requires the external PHY address from the real Pi 4 firmware node. */
	assert(((bits >> 21) & 31U) == 1U);
	reg = (bits >> 16) & 31U;

	/* Completes reads with the modelled PHY register contents. */
	if ((bits & (1U << 27)) != 0) {
		model.registers[offset / 4U] =
		    (bits & ~((1U << 29) | 0xffffU)) | model.phy[reg];
	} else {
		/* Completes writes and models a self-clearing PHY reset. */
		model.phy[reg] = (uint16_t)bits;
		if (reg == 0 && (bits & 0x8000U) != 0)
			model.phy[reg] = 0;
		model.registers[offset / 4U] = bits & ~(1U << 29);
	}
}

/*
 * Supplies a monotonic microsecond counter for production reset deadlines.
 */
bool
kern_rtc_read_counter(
	uint64_t *counter,
	uint64_t *frequency)
{
	/* Exercises an unavailable time base without adding a production switch. */
	if (model.counter_missing != 0)
		return false;

	/* Allows the independent iteration bound to catch a frozen clock. */
	if (model.counter_stuck == 0)
		model.counter += 10U;
	*counter = model.counter;
	*frequency = 1000000U;

	/* Succeeded: the caller sees one stable counter frequency. */
	return true;
}

/*
 * Supplies scheduler ticks for PHY service cadence.
 */
uint64_t
clock_ticks(
	void)
{
	/* Reports the test-controlled periodic service time. */
	return model.ticks;
}

/*
 * Supplies scheduler ticks for the permanent service thread.
 */
uint64_t
sched_ticks(
	void)
{
	/* Reports the same clock used by the network worker. */
	return model.ticks;
}

/*
 * Rejects execution of the service thread's infinite loop in a host unit test.
 */
void
sched_sleep(
	uint64_t deadline)
{
	(void)deadline;

	/* A host test records thread setup but does not run the scheduler. */
	abort();
}

/*
 * Records creation of the periodic service thread without starting a host
 * thread.
 */
int
kthread_create(
	void (*entry)(void *),
	void *argument,
	int priority,
	struct thread **thread)
{
	/* Requires a genuine entry and owner before publishing an opaque thread. */
	assert(entry != NULL);
	assert(argument != NULL);
	assert(priority == SCHED_PRIORITY_DEFAULT);
	*thread = &model.thread;

	/* Succeeded: thread_start can publish service readiness. */
	return 0;
}

/*
 * Records that the periodic service was made runnable.
 */
void
thread_start(
	struct thread *thread)
{
	/* Requires the thread allocated by the model. */
	assert(thread == &model.thread);
	thread->started++;
}

/*
 * Creates the uncached physical DMA owner required by the production NIC.
 */
int
drv_dma_device_create(
	const struct drv_dma_constraints *constraints,
	struct drv_dma_device **device)
{
	/* Verifies the no-snooping and address constraints independently of MMIO. */
	assert(constraints->coherent == 0);
	assert(constraints->address_bits == 32U);
	model.dma.live = 1;
	*device = &model.dma;

	/* Succeeded: subsequent allocations belong to this model owner. */
	return 0;
}

/*
 * Retires an allocator only after production has returned all DMA allocations.
 */
int
drv_dma_device_destroy(
	struct drv_dma_device *device)
{
	/* Checks that initialization unwind preserved allocator ownership. */
	assert(device == &model.dma);
	assert(model.allocations == 0);
	model.dma.live = 0;

	/* Succeeded: there are no outstanding DMA runs. */
	return 0;
}

/*
 * Allocates a host payload with a distinct low physical DMA address.
 */
int
drv_dma_alloc_coherent(
	struct drv_dma_device *device,
	size_t size,
	size_t alignment,
	struct drv_dma_buffer *buffer)
{
	/* Models failure of either allocation before the NIC is published. */
	assert(device == &model.dma);
	assert(model.dma.live != 0);
	assert(alignment == 64U);
	if (model.fail_allocation == model.allocations + 1U)
		return ENOMEM;

	/* Allocates independent backing instead of aliasing the controller SRAM. */
	buffer->address = calloc(1, size);
	assert(buffer->address != NULL);
	buffer->size = size;
	buffer->device_address = 0x200000U + model.allocations * size;

	/* Captures the receive and transmit payloads for external packet injection. */
	if (model.allocations == 0) {
		model.rx = buffer->address;
	} else {
		model.tx = buffer->address;
	}

	/* Accounts the run until a failed attachment or final host teardown returns it. */
	model.allocations++;

	/* Succeeded: the model supplies both CPU and DMA addresses. */
	return 0;
}

/*
 * Returns a coherent run during production attachment unwind.
 */
void
drv_dma_free_coherent(
	struct drv_dma_device *device,
	struct drv_dma_buffer *buffer)
{
	/* Rejects double release and frees only the retained buffer's allocation. */
	assert(device == &model.dma);
	assert(buffer->address != NULL);
	assert(model.allocations != 0);
	free(buffer->address);
	memset(buffer, 0, sizeof(*buffer));
	model.allocations--;
}

/*
 * Registers the primary GENET completion handler while the line is masked.
 */
int
kern_irq_register(
	int irq,
	kern_irq_handler_t handler,
	void *argument)
{
	/* Exercises a failed registration before interface publication. */
	assert(irq == 189);
	assert(model.irq_masked != 0);
	if (model.fail_irq != 0)
		return EBUSY;
	model.handler = handler;
	model.handler_argument = argument;

	/* Succeeded: tests can deliver DMA interrupts to the production handler. */
	return 0;
}

/*
 * Removes only the IRQ registration owned by the failed attachment.
 */
int
kern_irq_unregister(
	int irq,
	kern_irq_handler_t handler,
	void *argument)
{
	/* Verifies exact registration ownership before retiring the handler. */
	assert(irq == 189);
	assert(handler == model.handler);
	assert(argument == model.handler_argument);
	model.handler = NULL;
	model.irq_unregistered++;

	/* Succeeded: no handler can reach a subsequently unmapped register bank. */
	return 0;
}

/*
 * Checks that the driver configures the real GIC SPI's trigger mode.
 */
int
kern_irq_set_mode(
	int irq,
	unsigned trigger,
	unsigned polarity)
{
	/* Requires the level-high mode described by the real Pi 4 FDT. */
	assert(irq == 189);
	assert(trigger == KERN_IRQ_TRIGGER_LEVEL);
	assert(polarity == KERN_IRQ_POLARITY_HIGH);

	/* Succeeded: the simulated interrupt controller accepts this SPI. */
	return 0;
}

/*
 * Masks the GIC line before shutdown or initial registration.
 */
void
kern_irq_mask(
	int irq)
{
	/* Records GIC masking independently of the device's interrupt-source mask. */
	assert(irq == 189);
	model.irq_masked = 1;
}

/*
 * Arms the GIC line after interface open completes.
 */
void
kern_irq_unmask(
	int irq)
{
	/* Requires the handler to exist before any completion is admitted. */
	assert(irq == 189);
	assert(model.handler != NULL);
	model.irq_masked = 0;
}

/*
 * Retires the opaque GIC acknowledgement token received by the handler.
 */
void
kern_irq_send_eoi(
	kern_irq_ack_t acknowledge)
{
	/* Verifies that the handler passes the actual token through unchanged. */
	assert(acknowledge == 0x1234U);
	model.eoi++;
}

/*
 * Allocates the static network slot used by the model.
 */
struct net_device *
net_device_alloc(
	void)
{
	/* Publishes an empty slot so production must initialize every public field. */
	memset(&model.net, 0, sizeof(model.net));

	/* Reports the single network registry slot. */
	return &model.net;
}

/*
 * Verifies the interface description before making it observable to the stack.
 */
int
net_device_create(
	struct net_device *net)
{
	int comparison;

	/* Requires a working normal Ethernet interface, without a fabricated carrier. */
	comparison = strcmp(net->name, "en0");
	assert(comparison == 0);
	assert(net->mtu == 1500U);
	assert(net->hwaddr_len == 6U);
	assert((net->flags & NET_DEVICE_RUNNING) == 0);
	assert(net->ops != NULL);
	assert(net->ops->open != NULL);
	assert(net->ops->close != NULL);
	assert(net->ops->poll_receive != NULL);

	/* Succeeded: tests can drive the public network callback interface. */
	return 0;
}

/*
 * Retires an unpublished network slot on attachment failure.
 */
void
net_device_destroy(
	struct net_device *net)
{
	/* Requires the same registry slot that production allocated. */
	assert(net == &model.net);
}

/*
 * Records a lifetime reference retained by the permanent poll service.
 */
void
net_device_ref(
	struct net_device *net)
{
	/* Requires references to be acquired outside the higher-ranked device lock. */
	assert(model.lock_depth == 0);
	assert(net == &model.net);
}

/*
 * Records carrier notifications delivered outside the hardware lock.
 */
int
net_device_set_carrier(
	struct net_device *net,
	int carrier)
{
	/* Checks lock ordering on every open, poll, and close carrier update. */
	assert(model.lock_depth == 0);
	assert(net == &model.net);
	model.carrier = (unsigned)carrier;

	/* Succeeded: the simulated route/network listener can observe carrier. */
	return 0;
}

/*
 * Captures a received stack packet and consumes its ownership.
 */
void
net_device_receive(
	struct net_device *net,
	struct packet_buf *packet)
{
	/* Requires complete Ethernet payloads to arrive outside the device lock. */
	assert(model.lock_depth == 0);
	assert(net == &model.net);
	assert(packet->length <= sizeof(model.frame));
	memcpy(model.frame, packet->data, packet->length);
	model.frame_length = packet->length;
	model.received++;
	packet_buf_free(packet);
}

/*
 * Records an IRQ-driven poll request without recursively invoking the worker.
 */
void
net_device_schedule_poll(
	struct net_device *net)
{
	/* Checks that the handler releases its device lock before entering the stack. */
	assert(model.lock_depth == 0);
	assert(net == &model.net);
	model.scheduled++;
}

/*
 * Allocates a stack packet while permitting a deliberate pool-exhaustion case.
 */
struct packet_buf *
packet_buf_alloc(
	size_t headroom)
{
	struct packet_buf *packet;

	/* Rejects allocation under the device lock, which would invert kernel ranks. */
	assert(model.lock_depth == 0);
	if (model.pool_empty != 0)
		return NULL;

	/* Creates an independent network packet rather than handing DMA memory to users. */
	packet = calloc(1, sizeof(*packet));
	assert(packet != NULL);
	packet->storage = malloc(2048U);
	assert(packet->storage != NULL);
	packet->data = packet->storage + headroom;
	packet->capacity = 2048U - headroom;

	/* Reports a packet whose ownership is consumed by transmit or receive. */
	return packet;
}

/*
 * Reserves a bounded packet payload for the production receive copy.
 */
void *
packet_buf_append(
	struct packet_buf *packet,
	size_t length)
{
	void *payload;

	/* Rejects a payload outside the network pool's own storage. */
	if (length > packet->capacity - packet->length)
		return NULL;
	payload = packet->data + packet->length;
	packet->length += length;

	/* Reports the reserved packet bytes. */
	return payload;
}

/*
 * Consumes network packet ownership on success and all drop paths.
 */
void
packet_buf_free(
	struct packet_buf *packet)
{
	/* Supports the real packet pool's harmless NULL release behavior. */
	if (packet == NULL)
		return;

	/* Detects frees under the device lock and double frees through ASan. */
	assert(model.lock_depth == 0);
	free(packet->storage);
	free(packet);
	model.freed_packets++;
}

/*
 * Initializes the production lock's metadata for the single-threaded model.
 */
void
spin_init(
	struct spinlock *lock,
	enum lock_rank rank,
	const char *name)
{
	/* Preserves the actual rank used by production hardware operations. */
	memset(lock, 0, sizeof(*lock));
	lock->rank = rank;
	lock->name = name;
}

/*
 * Models exclusive device access and records the lock-order boundary.
 */
unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	/* Rejects recursive locks in the production driver. */
	assert(lock->rank == LOCK_RANK_DEVICE);
	assert(model.lock_depth == 0);
	model.lock_depth++;

	/* Reports the model's originally enabled interrupt state. */
	return 1U;
}

/*
 * Ends an exclusive device operation and restores the model's interrupt state.
 */
void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	/* Detects unbalanced locking on all callback and initialization paths. */
	assert(lock->rank == LOCK_RANK_DEVICE);
	assert(enabled == 1U);
	assert(model.lock_depth == 1U);
	model.lock_depth--;
}

/*
 * Preserves the CPU-to-device ordering call in the production transmit path.
 */
void
kern_io_write_barrier(
	void)
{
	/* The single-threaded model preserves access order without host MMIO. */
	__asm__ volatile("" : : : "memory");
}

/*
 * Preserves the device-to-CPU ordering call in the production receive path.
 */
void
kern_io_read_barrier(
	void)
{
	/* The model never exposes a payload before its descriptor completion. */
	__asm__ volatile("" : : : "memory");
}

/*
 * Preserves ownership ordering before returning a receive slot to DMA.
 */
void
kern_io_barrier(
	void)
{
	/* The model respects the explicit completion-before-consumer sequence. */
	__asm__ volatile("" : : : "memory");
}

/*
 * Accepts production diagnostic messages without disclosing packet contents.
 */
void
kern_logf(
	const char *format,
	...)
{
	(void)format;
}

/* Allocates a reproducible outgoing Ethernet packet for transmit tests. */
static struct packet_buf *
make_packet(
	unsigned length)
{
	struct packet_buf *packet;
	void *payload;

	/* Builds a frame independently of the driver's uncached transmit storage. */
	packet = packet_buf_alloc(64U);
	assert(packet != NULL);
	payload = packet_buf_append(packet, length);
	assert(payload != NULL);
	memset(payload, 0x5a, length);

	/* Reports a stack-owned frame for the production callback to consume. */
	return packet;
}

/* Restores hardware state between failed attachment attempts. */
static void
reset_model(
	void)
{
	uint8_t *tree;
	size_t tree_size;

	/* Keeps the immutable firmware input while clearing hardware fault injection. */
	tree = model.tree;
	tree_size = model.tree_size;
	memset(&model, 0, sizeof(model));
	model.tree = tree;
	model.tree_size = tree_size;
	model.registers[0x000U / 4U] = 0x06000000U;
	model.registers[0x80cU / 4U] = 0x02010203U;
	model.registers[0x810U / 4U] = 0x0405U;
	model.phy[2] = 0x600dU;
	model.phy[3] = 0x84a2U;
}

/* Checks the real DTB and rejects disabled hardware or malformed SPI bindings. */
static void
test_description(
	void)
{
	struct drv_fdt fdt;
	struct rpi4_ethernet_config config;
	const uint8_t *property;
	uint8_t saved;
	uint32_t node;
	uint32_t length;
	int error;

	/* Describes the actual firmware node rather than a hand-written test-only tree. */
	error = drv_fdt_open(&fdt, model.tree, model.tree_size);
	assert(error == 0);
	error =
	    drv_fdt_find_compatible(&fdt, "brcm,bcm2711-genet-v5", 0, &node);
	assert(error == 0);
	error = drv_rpi4_ethernet_describe(&fdt, node, &config);
	assert(error == 0);
	assert(config.physical == 0xfd580000U);
	assert(config.irq == 189U);
	assert(config.phy_address == 1U);
	assert(config.rx_delay == 1U);
	assert(config.tx_delay == 0U);
	assert(config.dma_limit == UINT32_MAX);

	/* Verifies that a firmware-disabled GENET never becomes a register probe. */
	error = drv_fdt_property(&fdt, node, "status", &property, &length);
	assert(error == 0);
	assert(length == 5U);
	memcpy((void *)property, "fail", 5U);
	error = drv_rpi4_ethernet_describe(&fdt, node, &config);
	assert(error == ENODEV);
	memcpy((void *)property, "okay", 5U);

	/* Refuses an edge trigger instead of accidentally configuring the wrong GIC mode. */
	error = drv_fdt_property(&fdt, node, "interrupts", &property, &length);
	assert(error == 0);
	assert(length >= 12U);
	saved = property[11];
	((uint8_t *)property)[11] = 1U;
	error = drv_rpi4_ethernet_describe(&fdt, node, &config);
	assert(error == ENOTSUP);
	((uint8_t *)property)[11] = saved;
}

/* Ensures hardware absence and initialization failures unwind without boot stalls. */
static void
test_failures(
	void)
{
	unsigned allocation;
	int error;

	/* Fails both allocation stages separately and checks for retained ownership. */
	for (allocation = 1; allocation <= 2U; allocation++) {
		/* Restarts the controller model and chooses one allocation failure. */
		reset_model();
		model.fail_allocation = allocation;
		error = drv_rpi4_ethernet_init(0x100000U);
		assert(error == ENOMEM);
		assert(model.allocations == 0);
		assert(model.mapped == 0);
	}

	/* Rejects a PHY whose register layout differs before changing vendor shadows. */
	reset_model();
	model.phy[3] = 0x1234U;
	error = drv_rpi4_ethernet_init(0x100000U);
	assert(error == ENOTSUP);
	assert(model.mapped == 0);

	/* Proves that an MDIO engine stuck BUSY reaches a finite timeout. */
	reset_model();
	model.mdio_stuck = 1;
	error = drv_rpi4_ethernet_init(0x100000U);
	assert(error == ETIMEDOUT);
	assert(model.mapped == 0);

	/* Proves that a frozen monotonic counter cannot make reset wait forever. */
	reset_model();
	model.counter_stuck = 1;
	error = drv_rpi4_ethernet_init(0x100000U);
	assert(error == ETIMEDOUT);
	assert(model.mapped == 0);

	/* Rejects an unavailable counter with no DMA allocation or registry publication. */
	reset_model();
	model.counter_missing = 1;
	error = drv_rpi4_ethernet_init(0x100000U);
	assert(error == EIO);
	assert(model.mapped == 0);

	/* Returns both coherent allocations after IRQ registration refuses ownership. */
	reset_model();
	model.fail_irq = 1;
	error = drv_rpi4_ethernet_init(0x100000U);
	assert(error == EBUSY);
	assert(model.mapped == 0);
	assert(model.allocations == 0);
}

/* Injects one completed hardware receive descriptor and its aligned payload. */
static void
inject_receive(
	unsigned slot,
	unsigned length,
	uint32_t flags)
{
	uint8_t *payload;

	/* Gives the frame distinguishable padding that must not reach the network stack. */
	payload = (uint8_t *)model.rx + slot * 2048U;
	memset(payload, 0x33, 2048U);
	payload[0] = 0xcc;
	payload[1] = 0xdd;
	model.registers[(0x2000U + slot * 12U) / 4U] = (length << 16) | flags;
}

/* Checks payload copies, ring wrap, malformed RX release, interrupts, and reopen. */
static void
test_packets(
	void)
{
	struct net_device *net;
	struct packet_buf *packet;
	unsigned i;
	unsigned count;
	unsigned freed;
	int error;

	/* Attaches and opens through the production public callbacks with no cable. */
	reset_model();
	error = drv_rpi4_ethernet_init(0x100000U);
	assert(error == 0);
	net = &model.net;
	error = net->ops->open(net);
	assert(error == 0);
	assert(model.carrier == 0);
	assert(model.phy[4] == 0x0141U);
	assert(model.phy[9] == 0x0200U);
	assert((model.phy[0x18] & 0x200U) != 0);
	assert((model.phy[0x1c] & 0x200U) == 0);
	drv_rpi4_ethernet_refresh();
	assert(model.thread.started == 1U);

	/* Completes autonegotiation and requires a carrier event with matching MAC speed. */
	model.phy[1] = 0x24U;
	model.phy[0x19] = 0x0700U;
	model.ticks += KERN_MS_TO_TICKS(300U);
	count = net->ops->poll_receive(net, 16U);
	assert(count == 0);
	assert(model.carrier == 1U);
	assert(net->link_mbps == 1000U);
	assert((model.registers[0x808U / 4U] & 15U) == 11U);

	/* Requires a consumed short frame to become 60 bytes with zero padding and hardware CRC. */
	packet = make_packet(14U);
	freed = model.freed_packets;
	error = net->ops->transmit(net, packet);
	assert(error == 0);
	assert(model.freed_packets == freed + 1U);
	assert(model.registers[0x500cU / 4U] == 1U);
	assert((model.registers[0x4000U / 4U] >> 16) == 60U);
	assert((model.registers[0x4000U / 4U] & 0x6040U) == 0x6040U);
	assert(((uint8_t *)model.tx)[13] == 0x5aU);
	assert(((uint8_t *)model.tx)[14] == 0);
	assert(((uint8_t *)model.tx)[59] == 0);

	/* Fills the entire ring without pretending hardware already consumed its slots. */
	for (i = 1; i < 256U; i++) {
		/* Produces a real packet for every occupied descriptor. */
		packet = make_packet(1514U);
		error = net->ops->transmit(net, packet);
		assert(error == 0);
	}

	/* Refuses a 257th outstanding frame and still releases its stack ownership. */
	packet = make_packet(60U);
	error = net->ops->transmit(net, packet);
	assert(error == ENOBUFS);
	assert(model.registers[0x500cU / 4U] == 256U);

	/* Exercises the full 16-bit sequence boundary with immediate hardware completions. */
	for (i = 256U; i < 65538U; i++) {
		/* Completes the prior producer before supplying one more packet. */
		model.registers[0x5008U / 4U] = model.registers[0x500cU / 4U];
		packet = make_packet(60U);
		error = net->ops->transmit(net, packet);
		assert(error == 0);
	}

	/* Requires sequence wrap rather than a 32-bit producer the hardware cannot represent. */
	assert(model.registers[0x500cU / 4U] == 2U);

	/* Receives a valid frame and rejects CRC/malformed frames while advancing every slot. */
	inject_receive(0, 62U, 0x6000U);
	inject_receive(1, 62U, 0x6002U);
	inject_receive(2, 2048U, 0x6000U);
	model.registers[0x3008U / 4U] = 3U;
	count = net->ops->poll_receive(net, 2U);
	assert(count == 2U);
	assert(model.registers[0x300cU / 4U] == 2U);
	assert(model.received == 1U);
	assert(model.frame_length == 60U);
	assert(model.frame[0] == 0x33U);
	count = net->ops->poll_receive(net, 2U);
	assert(count == 1U);
	assert(net->rx_errors == 2U);
	assert(model.registers[0x300cU / 4U] == 3U);

	/* Returns a valid RX slot even if the network pool cannot allocate its copy. */
	inject_receive(3, 62U, 0x6000U);
	model.registers[0x3008U / 4U] = 4U;
	model.pool_empty = 1;
	count = net->ops->poll_receive(net, 16U);
	assert(count == 1U);
	assert(model.registers[0x300cU / 4U] == 4U);
	model.pool_empty = 0;

	/* Verifies level acknowledgement, RX masking, scheduling, and thread-context rearm. */
	model.registers[0x200U / 4U] = (1U << 13) | (1U << 16);
	model.handler(189, 0x1234U, model.handler_argument);
	assert(model.eoi == 1U);
	assert(model.scheduled == 1U);
	assert((model.registers[0x20cU / 4U] & (1U << 13)) != 0);
	count = net->ops->poll_receive(net, 16U);
	assert(count == 0);
	assert((model.registers[0x20cU / 4U] & (1U << 13)) == 0);

	/* Stops carrier on a later MDIO stall and permits recovery on a future poll. */
	model.mdio_stuck = 1;
	model.ticks += KERN_MS_TO_TICKS(300U);
	count = net->ops->poll_receive(net, 16U);
	assert(count == 0);
	assert(model.carrier == 0);
	model.mdio_stuck = 0;
	model.registers[0xe14U / 4U] = 0;
	model.ticks += KERN_MS_TO_TICKS(300U);
	count = net->ops->poll_receive(net, 16U);
	assert(count == 0);
	assert(model.carrier == 1U);

	/* Requires synchronous close to mask interrupts and stop both DMA directions. */
	net->ops->close(net);
	assert(model.carrier == 0);
	assert(model.irq_masked == 1U);
	assert(model.registers[0x3044U / 4U] == 0);
	assert(model.registers[0x5044U / 4U] == 0);
	packet = make_packet(60U);
	error = net->ops->transmit(net, packet);
	assert(error == ENETDOWN);

	/* Requires reopen to reset stale ring state while retaining the permanent payloads. */
	error = net->ops->open(net);
	assert(error == 0);
	assert(model.carrier == 1U);
	assert(model.registers[0x300cU / 4U] == 0);
	assert(model.registers[0x500cU / 4U] == 0);
	assert(model.allocations == 2U);
	net->ops->close(net);
}
