/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The board's interrupts: the OpenPIC (MPIC) in the I/O controller, which
 * gathers the devices' lines and raises the processor's external
 * interrupt; and the kernel's interrupt interface on top of it.
 *
 * An interrupt number is the controller's source number, the number the
 * firmware's device tree gives a device's line.  Each source is delivered
 * to processor 0 at one priority with its own number as its vector, so
 * the acknowledge register names the source.  A source starts masked;
 * a driver unmasks it once its handler is registered.  The tick is the
 * processor's decrementer and does not pass through the controller.
 *
 * The controller's registers are little-endian words.  The sources the
 * firmware described are set to the trigger it named for them before
 * any is unmasked: Apple's sense cell 0 is a rising edge, 1 a low level
 * (the PCI lines).
 *
 * Values referenced: the OpenPIC Register Interface Specification,
 * Revision 1.2 (AMD and Cyrix, 1995): the feature reporting register at
 * 0x1000 (the number of sources less one in bits 16 to 26), the global
 * configuration register at 0x1020, the spurious vector register at
 * 0x10e0, each source's vector/priority register at 0x10000 plus 0x20
 * per source (mask bit 31, polarity bit 23, sense bit 22, priority in
 * bits 16 to 19, vector in the low byte) and its destination register
 * 0x10 after it, and processor 0's current task priority (0x20080),
 * interrupt acknowledge (0x200a0) and end of interrupt (0x200b0)
 * registers.  The Linux kernel's arch/powerpc/sysdev/mpic.c sets the
 * pass-through disable bit (bit 29) of the global configuration register
 * and reads Apple's sense cell with only its bit 0 meaningful (0 rising
 * edge, 1 low level).
 *
 * Every take of a source is counted, and the counts are handed over as
 * "mac99.irq-watch" for the kernel's boot watch.  A source taken more
 * than IRQ_STORM_TAKES times within a tenth of a second is a level line
 * its handler does not quiet: it would take the processor from every
 * thread for good, so it is masked and reported, and its driver is left
 * to the polling it does anyway.  A USB host controller interrupts at most
 * once a frame (a millisecond), so the limit is far above any device that
 * works.
 */

#include <hal/hal.h>
#include "../asm.h"
#include "../percpu.h"
#include "../spin.h"
#include "bsp.h"

/* The controller's global registers. */
#define OPENPIC_FEATURE			0x01000U
#define OPENPIC_GLOBAL_CONFIG		0x01020U
#define OPENPIC_SPURIOUS		0x010e0U

/* Where each source's registers are, and its destination after its vector/priority. */
#define OPENPIC_SOURCE_FIRST		0x10000U
#define OPENPIC_SOURCE_STRIDE		0x20U
#define OPENPIC_SOURCE_DESTINATION	0x10U

/* Processor 0's registers. */
#define OPENPIC_CPU0_TASK_PRIORITY	0x20080U
#define OPENPIC_CPU0_ACKNOWLEDGE	0x200a0U
#define OPENPIC_CPU0_END		0x200b0U

/* The global configuration's pass-through disable: the controller, not an 8259, interrupts. */
#define OPENPIC_CONFIG_NO_PASSTHROUGH	0x20000000U

/* A source's vector/priority register. */
#define OPENPIC_SOURCE_MASK		0x80000000U
#define OPENPIC_SOURCE_POLARITY_HIGH	0x00800000U
#define OPENPIC_SOURCE_SENSE_LEVEL	0x00400000U
#define OPENPIC_SOURCE_PRIORITY_SHIFT	16U
#define OPENPIC_SOURCE_VECTOR_MASK	0x000000ffU

/* The priority every source has, and the vector the controller reports when nothing is pending. */
#define OPENPIC_PRIORITY		8U
#define OPENPIC_SPURIOUS_VECTOR		0xffU

/* The most sources the HAL serves; the vector is the source and the spurious vector stays apart. */
#define OPENPIC_SOURCES_MAX		128U

/* Apple's sense cell: bit 0 set is a low level, clear a rising edge. */
#define APPLE_SENSE_LEVEL_LOW		0x1U

/* The takes of one source within a tenth of a second that make a storm, and that window as a divisor of the time base's rate. */
#define IRQ_STORM_TAKES			20000U
#define IRQ_STORM_WINDOW_DIVISOR	10U

/* The watch table's words: the count of sources, a count each, then the storm bitmap. */
#define IRQ_WATCH_COUNTS		1U
#define IRQ_WATCH_STORMS		(IRQ_WATCH_COUNTS + OPENPIC_SOURCES_MAX)
#define IRQ_WATCH_WORDS			(IRQ_WATCH_STORMS + OPENPIC_SOURCES_MAX / 32U)

/*
 * A source's registered handler and argument.  Kept with interrupts off;
 * a NULL handler means none.
 */
struct irq_slot {
	hal_irq_handler_t handler;
	void *argument;
};

/*
 * The controller's registers, reached at their own address in a device
 * window; NULL when the board has no controller the HAL can reach, and
 * then no source can be registered.  Set once by mac99_openpic_init().
 */
static volatile uint8_t *openpic;

/* How many sources the controller has, at most OPENPIC_SOURCES_MAX; set with openpic. */
static unsigned source_count;

/* Each source's handler, by source number. */
static struct irq_slot slots[OPENPIC_SOURCES_MAX];

/* Guards the slots against a change while an interrupt reads them; zero when free. */
static volatile unsigned slots_lock;

/*
 * How often each source was taken and which were masked for a storm,
 * handed over as "mac99.irq-watch": word 0 the number of count slots
 * (OPENPIC_SOURCES_MAX, whatever the controller has), then a count each
 * (wrapping), then one bit a source in the storm bitmap.  Written only by
 * the external interrupt, which never nests; the kernel reads it whenever
 * it likes and may see a count one take old.
 */
static uint32_t irq_watch[IRQ_WATCH_WORDS] = {OPENPIC_SOURCES_MAX};

/*
 * Each source's storm window: the time base when its window started and
 * the takes in it.  Kept by the external interrupt alone.
 */
static uint64_t storm_start[OPENPIC_SOURCES_MAX];
static uint32_t storm_takes[OPENPIC_SOURCES_MAX];

static uint32_t openpic_read(uint32_t offset);
static void openpic_write(uint32_t offset, uint32_t value);
static uint32_t source_register(unsigned source);
static int valid_source(int irq);
static void set_trigger(unsigned source, bool level, bool high);
static bool storm_check(unsigned source);

/*
 * Starts the controller at a physical address: every source masked,
 * routed to processor 0 with its own vector, the triggers the firmware
 * described, and the processor's priority open.  A controller the HAL
 * cannot reach is left alone, and no source can then be used.
 */
void
mac99_openpic_init(
	uint32_t base,
	const struct mac99_interrupt_route *routes,
	unsigned route_count)
{
	void *window;
	uint32_t feature;
	uint32_t value;
	unsigned source;
	unsigned index;
	int error;

	/* Finds the controller in a device window. */
	if (base == 0) {
		hal_puts("PPC32 OPENPIC NONE\n");
		return;
	}
	error = hal_space_map_device(base, OPENPIC_CPU0_END + 4U, HAL_SPACE_READ | HAL_SPACE_WRITE, &window);
	if (error != HAL_OK) {
		hal_printf("PPC32 OPENPIC %08x UNMAPPED\n", base);
		return;
	}
	openpic = window;

	/* The number of sources, bounded by what the HAL serves. */
	feature = openpic_read(OPENPIC_FEATURE);
	source_count = ((feature >> 16) & 0x7ffU) + 1U;
	if (source_count > OPENPIC_SOURCES_MAX)
		source_count = OPENPIC_SOURCES_MAX;

	/* The controller drives the processor itself, and reports nothing pending with its own vector. */
	value = openpic_read(OPENPIC_GLOBAL_CONFIG);
	openpic_write(OPENPIC_GLOBAL_CONFIG, value | OPENPIC_CONFIG_NO_PASSTHROUGH);
	openpic_write(OPENPIC_SPURIOUS, OPENPIC_SPURIOUS_VECTOR);

	/* Every source masked, its own vector, one priority, processor 0; the trigger as it was. */
	for (source = 0; source < source_count; source++) {
		value = openpic_read(source_register(source));
		value &= OPENPIC_SOURCE_POLARITY_HIGH | OPENPIC_SOURCE_SENSE_LEVEL;
		value |= OPENPIC_SOURCE_MASK;
		value |= OPENPIC_PRIORITY << OPENPIC_SOURCE_PRIORITY_SHIFT;
		value |= source;
		openpic_write(source_register(source), value);
		openpic_write(source_register(source) + OPENPIC_SOURCE_DESTINATION, 1U);
	}

	/* The triggers the firmware named for the devices' lines. */
	for (index = 0; index < route_count; index++) {
		if (routes[index].source >= source_count)
			continue;
		if ((routes[index].sense & APPLE_SENSE_LEVEL_LOW) != 0) {
			set_trigger(routes[index].source, true, false);
		} else {
			set_trigger(routes[index].source, false, true);
		}
	}

	/* Takes every priority; whatever was in service is ended. */
	openpic_write(OPENPIC_CPU0_TASK_PRIORITY, 0);
	for (index = 0; index < source_count; index++)
		openpic_write(OPENPIC_CPU0_END, 0);

	/* Says the controller is ready. */
	hal_printf("PPC32 OPENPIC %08x SOURCES %u\n", base, source_count);
}

/*
 * Reports the interrupt counts and the sources masked for a storm, as the
 * table "mac99.irq-watch" hands them over.
 */
const uint32_t *
mac99_irq_watch(
	void)
{
	/* Succeeded: the live table. */
	return irq_watch;
}

/*
 * Sets the trigger of a source the firmware described outside the PCI
 * routes, from Apple's sense cell: 1 a low level, 0 a rising edge.  The
 * source must be masked.
 */
void
mac99_openpic_sense(
	uint32_t source,
	uint32_t sense)
{
	/* A source the controller does not have is left alone. */
	if (openpic == NULL || source >= source_count)
		return;
	if ((sense & APPLE_SENSE_LEVEL_LOW) != 0) {
		set_trigger(source, true, false);
	} else {
		set_trigger(source, false, true);
	}
}

/*
 * Takes the processor's external interrupt: acknowledges the source the
 * controller names and hands it to its handler, which ends it with
 * hal_irq_send_eoi().  A source with no handler is masked and ended, so
 * that it does not raise its interrupt forever.
 */
void
mac99_take_external(
	void)
{
	struct ppc32_percpu *cpu;
	hal_irq_handler_t handler;
	hal_irq_ack_t acknowledge;
	void *argument;
	uint32_t vector;

	/* Without a controller nothing can interrupt. */
	if (openpic == NULL) {
		hal_puts("unexpected ppc32 external interrupt\n");
		return;
	}

	/* The source, or the spurious vector when it went away. */
	vector = openpic_read(OPENPIC_CPU0_ACKNOWLEDGE) & OPENPIC_SOURCE_VECTOR_MASK;
	if (vector == OPENPIC_SPURIOUS_VECTOR)
		return;

	/* A vector past the sources is ended and reported. */
	if (vector >= source_count) {
		openpic_write(OPENPIC_CPU0_END, 0);
		hal_printf("unexpected ppc32 OpenPIC vector %u\n", vector);
		return;
	}

	/* Refuses an interrupt inside another one. */
	cpu = ppc32_percpu_current();
	if (cpu->active_ack != HAL_IRQ_ACK_NONE)
		HAL_FATAL("nested ppc32 IRQ acknowledgement");

	/* Counts the take; a source that storms is masked and ended without its handler. */
	irq_watch[IRQ_WATCH_COUNTS + vector]++;
	if (storm_check(vector)) {
		hal_irq_mask((int)vector);
		openpic_write(OPENPIC_CPU0_END, 0);
		irq_watch[IRQ_WATCH_STORMS + vector / 32U] |= 1U << (vector % 32U);
		hal_printf("PPC32 IRQ %u STORM: MASKED, ITS DRIVER POLLS\n", vector);
		return;
	}

	/* The handler ends the interrupt; the acknowledgement is the source plus one, never none. */
	acknowledge = (hal_irq_ack_t)(vector + 1U);
	handler = slots[vector].handler;
	argument = slots[vector].argument;
	if (handler != NULL) {
		cpu->active_ack = acknowledge;
		handler((int)vector, acknowledge, argument);
		return;
	}

	/* No handler: the source is masked, ended and reported. */
	hal_irq_mask((int)vector);
	openpic_write(OPENPIC_CPU0_END, 0);
	hal_printf("unexpected ppc32 IRQ %u\n", vector);
}

/*
 * Turns external interrupts off and reports whether they were on.
 */
bool
hal_irq_disable(
	void)
{
	bool enabled;

	/* The machine state's enable bit. */
	enabled = ppc32_irq_save();

	/* Succeeded: whether interrupts were on. */
	return enabled;
}

/*
 * Turns external interrupts on.
 */
void
hal_irq_enable(
	void)
{
	/* The machine state's enable bit. */
	ppc32_irq_restore(true);
}

/*
 * Keeps a source from raising its interrupt.
 */
void
hal_irq_mask(
	int irq)
{
	uint32_t value;
	int valid;

	/* Refuses a source the controller does not have. */
	valid = valid_source(irq);
	if (!valid)
		HAL_FATAL("bad ppc32 IRQ mask");

	/* Sets the source's mask bit. */
	value = openpic_read(source_register((unsigned)irq));
	openpic_write(source_register((unsigned)irq), value | OPENPIC_SOURCE_MASK);
}

/*
 * Lets a source raise its interrupt again.
 */
void
hal_irq_unmask(
	int irq)
{
	uint32_t value;
	int valid;

	/* Refuses a source the controller does not have. */
	valid = valid_source(irq);
	if (!valid)
		HAL_FATAL("bad ppc32 IRQ unmask");

	/* Clears the source's mask bit. */
	value = openpic_read(source_register((unsigned)irq));
	openpic_write(source_register((unsigned)irq), value & ~OPENPIC_SOURCE_MASK);
}

/*
 * Registers a handler for a source; the source stays masked until the
 * driver unmasks it.
 */
int
hal_irq_register(
	int irq,
	hal_irq_handler_t handler,
	void *argument)
{
	bool enabled;
	int valid;

	/* Requires a handler and a source the controller has. */
	if (handler == NULL)
		return HAL_ERR_INVALID;
	valid = valid_source(irq);
	if (!valid)
		return HAL_ERR_INVALID;

	/* Refuses a source that already has a handler. */
	enabled = ppc32_spin_lock(&slots_lock);
	if (slots[irq].handler != NULL) {
		ppc32_spin_unlock(&slots_lock, enabled);
		return HAL_ERR_BUSY;
	}

	/* Records the handler. */
	slots[irq].handler = handler;
	slots[irq].argument = argument;
	ppc32_spin_unlock(&slots_lock, enabled);

	/* Succeeded: the source is the handler's. */
	return HAL_OK;
}

/*
 * Removes a source's handler and masks the source.
 */
int
hal_irq_unregister(
	int irq,
	hal_irq_handler_t handler,
	void *argument)
{
	bool enabled;
	int valid;

	/* Requires the registered handler of a source. */
	if (handler == NULL)
		return HAL_ERR_INVALID;
	valid = valid_source(irq);
	if (!valid)
		return HAL_ERR_INVALID;
	enabled = ppc32_spin_lock(&slots_lock);
	if (slots[irq].handler != handler || slots[irq].argument != argument) {
		ppc32_spin_unlock(&slots_lock, enabled);
		return HAL_ERR_INVALID;
	}

	/* Masked only once the removal is known to succeed. */
	hal_irq_mask(irq);

	/* Forgets the handler. */
	slots[irq].handler = NULL;
	slots[irq].argument = NULL;
	ppc32_spin_unlock(&slots_lock, enabled);

	/* Succeeded: the source has no handler. */
	return HAL_OK;
}

/*
 * Ends the interrupt being handled: the tick needs nothing, a source is
 * ended at the controller.
 */
void
hal_irq_send_eoi(
	hal_irq_ack_t acknowledge)
{
	struct ppc32_percpu *cpu;

	/* Requires the acknowledgement of the interrupt being handled. */
	cpu = ppc32_percpu_current();
	if (acknowledge == HAL_IRQ_ACK_NONE || acknowledge != cpu->active_ack)
		HAL_FATAL("invalid ppc32 IRQ acknowledgement");
	cpu->active_ack = HAL_IRQ_ACK_NONE;

	/* A source's acknowledgement ends it at the controller. */
	if (acknowledge != PPC32_ACK_LOCAL)
		openpic_write(OPENPIC_CPU0_END, 0);
}

/*
 * Accepts an affinity that includes the one processor.
 */
int
hal_irq_set_affinity(
	int irq,
	const struct hal_cpu_mask *requested)
{
	int valid;
	bool member;

	/* Every source is taken by processor 0. */
	valid = valid_source(irq);
	if (!valid || requested == NULL)
		return HAL_ERR_INVALID;
	member = hal_cpu_mask_test(requested, 0);
	if (!member)
		return HAL_ERR_INVALID;

	/* Succeeded: the source stays on processor 0. */
	return HAL_OK;
}

/*
 * Reports a source's affinity: processor 0.
 */
int
hal_irq_get_affinity(
	int irq,
	struct hal_cpu_mask *requested,
	struct hal_cpu_mask *effective)
{
	int valid;

	/* Requires a source the controller has. */
	valid = valid_source(irq);
	if (!valid)
		return HAL_ERR_INVALID;
	if (requested != NULL) {
		hal_cpu_mask_zero(requested);
		hal_cpu_mask_set(requested, 0);
	}

	/* The effective affinity is the same. */
	if (effective != NULL) {
		hal_cpu_mask_zero(effective);
		hal_cpu_mask_set(effective, 0);
	}

	/* Succeeded: the affinity is processor 0. */
	return HAL_OK;
}

/*
 * Sets a source's trigger and polarity; the source must be masked.
 */
int
hal_irq_set_mode(
	int irq,
	int trigger,
	int polarity)
{
	int valid;

	/* Requires a source and a known trigger and polarity. */
	valid = valid_source(irq);
	if (!valid)
		return HAL_ERR_INVALID;
	if (trigger != HAL_IRQ_TRIGGER_EDGE && trigger != HAL_IRQ_TRIGGER_LEVEL)
		return HAL_ERR_INVALID;
	if (polarity != HAL_IRQ_POLARITY_HIGH && polarity != HAL_IRQ_POLARITY_LOW)
		return HAL_ERR_INVALID;

	/* Writes the source's sense and polarity bits. */
	set_trigger((unsigned)irq, trigger == HAL_IRQ_TRIGGER_LEVEL, polarity == HAL_IRQ_POLARITY_HIGH);

	/* Succeeded: the source triggers as asked. */
	return HAL_OK;
}

/* The board has no message-signalled interrupts. */
int
hal_irq_register_msi(
	const char *source,
	hal_irq_handler_t handler,
	void *handler_arg,
	int *mapped_irq,
	hal_physaddr_t *mapped_addr,
	uint32_t *mapped_event)
{
	UNUSED_PARAMETER(source);
	UNUSED_PARAMETER(handler);
	UNUSED_PARAMETER(handler_arg);
	UNUSED_PARAMETER(mapped_irq);
	UNUSED_PARAMETER(mapped_addr);
	UNUSED_PARAMETER(mapped_event);
	return HAL_ERR_UNSUPPORTED;
}

/* The board has no message-signalled interrupts. */
int
hal_irq_unregister_msi(
	int mapped_irq)
{
	UNUSED_PARAMETER(mapped_irq);
	return HAL_ERR_UNSUPPORTED;
}

/* The board has no message-signalled interrupts. */
int
hal_irq_alloc_msi(
	const char *source,
	int *mapped_irq,
	paddr_t *mapped_addr,
	uint32_t *mapped_event)
{
	UNUSED_PARAMETER(source);
	UNUSED_PARAMETER(mapped_irq);
	UNUSED_PARAMETER(mapped_addr);
	UNUSED_PARAMETER(mapped_event);
	return HAL_ERR_UNSUPPORTED;
}

/* The board has no message-signalled interrupts. */
int
hal_irq_attach_msi(
	int mapped_irq,
	hal_irq_handler_t handler,
	void *handler_arg)
{
	UNUSED_PARAMETER(mapped_irq);
	UNUSED_PARAMETER(handler);
	UNUSED_PARAMETER(handler_arg);
	return HAL_ERR_UNSUPPORTED;
}

/* The board has no message-signalled interrupts. */
int
hal_irq_detach_msi_sync(
	int mapped_irq,
	hal_irq_handler_t handler,
	void *handler_arg)
{
	UNUSED_PARAMETER(mapped_irq);
	UNUSED_PARAMETER(handler);
	UNUSED_PARAMETER(handler_arg);
	return HAL_ERR_UNSUPPORTED;
}

/* The board has no message-signalled interrupts. */
int
hal_irq_free_msi(
	int mapped_irq)
{
	UNUSED_PARAMETER(mapped_irq);
	return HAL_ERR_UNSUPPORTED;
}

/* Reads a controller register, little-endian. */
static uint32_t
openpic_read(
	uint32_t offset)
{
	uint32_t value;

	/* The load reverses the bytes; sync keeps it in order with the device. */
	__asm__ volatile("lwbrx %0,0,%1\n\tsync" : "=r"(value) : "r"(openpic + offset) : "memory");

	/* Succeeded: the register's value. */
	return value;
}

/* Writes a controller register, little-endian. */
static void
openpic_write(
	uint32_t offset,
	uint32_t value)
{
	/* The store reverses the bytes; sync makes it reach the device before what follows. */
	__asm__ volatile("stwbrx %0,0,%1\n\tsync" : : "r"(value), "r"(openpic + offset) : "memory");
}

/* Reports where a source's vector/priority register is. */
static uint32_t
source_register(
	unsigned source)
{
	/* The sources follow each other 0x20 bytes apart. */
	return OPENPIC_SOURCE_FIRST + source * OPENPIC_SOURCE_STRIDE;
}

/* Reports whether an interrupt number names a source of the controller. */
static int
valid_source(
	int irq)
{
	/* Without a controller there are no sources. */
	if (openpic == NULL)
		return 0;

	/* Zero to the last source. */
	if (irq < 0 || (unsigned)irq >= source_count)
		return 0;
	return 1;
}

/* Writes a source's sense and polarity, keeping the rest. */
static void
set_trigger(
	unsigned source,
	bool level,
	bool high)
{
	uint32_t value;

	/* Replaces the two bits. */
	value = openpic_read(source_register(source));
	value &= ~(OPENPIC_SOURCE_SENSE_LEVEL | OPENPIC_SOURCE_POLARITY_HIGH);
	if (level)
		value |= OPENPIC_SOURCE_SENSE_LEVEL;
	if (high)
		value |= OPENPIC_SOURCE_POLARITY_HIGH;
	openpic_write(source_register(source), value);
}

/*
 * Counts one take of a source in its storm window and reports whether
 * the source has been taken too often in it.
 */
static bool
storm_check(
	unsigned source)
{
	uint64_t now;
	uint64_t window;

	/* The window is a tenth of a second of the time base; without a rate there is no window. */
	window = mac99_board()->timebase_frequency / IRQ_STORM_WINDOW_DIVISOR;
	if (window == 0)
		return false;

	/* A window that has passed starts again with this take. */
	now = ppc32_read_timebase();
	if (now - storm_start[source] >= window) {
		storm_start[source] = now;
		storm_takes[source] = 0;
	}

	/* Counts the take. */
	storm_takes[source]++;

	/* Succeeded: whether the source passed the limit. */
	return storm_takes[source] > IRQ_STORM_TAKES;
}
