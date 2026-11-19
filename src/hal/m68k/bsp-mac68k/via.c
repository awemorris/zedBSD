/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Macintosh's interrupts: the two VIAs (6522) behind processor levels
 * 6 and 2, the serial controller behind level 4, and the kernel's
 * interrupt interface on top of them.
 *
 * The interrupt wiring is switched to the one A/UX uses: VIA1 raises level
 * 6, VIA2 level 2 and the serial controller level 4.  The other levels
 * have no source the HAL serves.
 *
 * An interrupt number is 1 + 8 * VIA + flag bit for the seven flags of
 * each VIA (VIA1's are 1 to 7, VIA2's 9 to 15), and 16 for the serial
 * controller.  The tick is VIA1's timer 1 and is served by the clock
 * without the kernel's table.
 *
 * Values referenced: Linux master arch/m68k/include/asm/mac_via.h (the
 * register offsets, 0x200 apart; the interrupt flag bits; VIA2 port B bit
 * 2, which turns the power off where the VIA controls it);
 * arch/m68k/mac/via.c (VIA2's peripheral control value 0x22).  QEMU
 * v10.0.0 hw/m68k/q800-glue.c (the two wirings, chosen by VIA1 port B
 * bit 6: low for the A/UX wiring) and hw/misc/mac_via.c (the power bit).
 * Procedure referenced: Linux master arch/m68k/mac/via.c (via_init: all
 * interrupts disabled and cleared, then port B bit 6 made an output and
 * driven low).
 */

#include <hal/hal.h>
#include "../m68040/asm.h"
#include "../m68040/percpu.h"
#include "../m68040/spin.h"
#include "bsp.h"

/* The VIA registers, 0x200 apart. */
#define VIA_ORB			0x0000U
#define VIA_DDRB		0x0400U
#define VIA_ACR			0x1600U
#define VIA_PCR			0x1800U
#define VIA_IFR			0x1a00U
#define VIA_IER			0x1c00U

/* The flag and enable registers: the set bit, and the seven flags. */
#define VIA_IER_SET		0x80U
#define VIA_FLAGS		0x7fU
#define VIA_FLAG_COUNT		7U

/* VIA1's timer 1 flag, which the tick raises. */
#define VIA_FLAG_TIMER1		0x40U

/* The auxiliary control bits of the shift register, which other code may use. */
#define VIA_ACR_SHIFT_MASK	0x1cU

/* VIA1 port B bit 6: low selects the A/UX interrupt wiring. */
#define VIA1_PORTB_AUX_WIRING	0x40U

/* VIA2's peripheral control: CA2 and CB2 independent inputs on the falling edge. */
#define VIA2_PCR_VALUE		0x22U

/* VIA2 port B bit 2: driven low, it turns the power off. */
#define VIA2_PORTB_POWER	0x04U

/* The processor levels of the A/UX wiring. */
#define LEVEL_VIA2		2U
#define LEVEL_SCC		4U
#define LEVEL_VIA1		6U
#define LEVEL_NMI		7U

/* The interrupt numbers: each VIA's first, the serial controller's, and the count. */
#define IRQ_VIA1_FIRST		1U
#define IRQ_VIA2_FIRST		9U
#define IRQ_SCC			16U
#define IRQ_MAX			17U

/* The tick's interrupt number: VIA1's timer 1 flag. */
#define IRQ_CLOCK		(IRQ_VIA1_FIRST + 6U)

/*
 * A source's registered handler and argument.  Kept under slots_lock; a
 * NULL handler means none.
 */
struct irq_slot {
	hal_irq_handler_t handler;
	void *argument;
};

/* Each source's handler, by interrupt number; slot 0 is unused. */
static struct irq_slot slots[IRQ_MAX];

/* Guards the slots against a change while an interrupt reads them; zero when free. */
static volatile unsigned slots_lock;

/*
 * How many interrupts came on a level no source is served on, by level.
 * Only counted, to be seen in a debugger.
 */
static unsigned long stray_interrupts[8];

static int valid_source(int irq);
static volatile uint8_t *via_register(uint32_t base, uint32_t offset);
static void serve_via(uint32_t base, unsigned first_irq);
static void dispatch(unsigned irq);
static void set_enabled(unsigned irq, bool enabled);

/*
 * Masks every VIA interrupt and selects the A/UX wiring.
 */
void
mac68k_via_init(
	void)
{
	uint8_t value;

	/* Nothing raises an interrupt until a source is enabled; old flags are dropped. */
	*via_register(MAC68K_VIA1_BASE, VIA_IER) = VIA_FLAGS;
	*via_register(MAC68K_VIA1_BASE, VIA_IFR) = VIA_FLAGS;
	*via_register(MAC68K_VIA2_BASE, VIA_IER) = VIA_FLAGS;
	*via_register(MAC68K_VIA2_BASE, VIA_IFR) = VIA_FLAGS;

	/* VIA1's timers stop interrupting; the shift register's mode stays the ROM's. */
	value = *via_register(MAC68K_VIA1_BASE, VIA_ACR);
	*via_register(MAC68K_VIA1_BASE, VIA_ACR) = value & VIA_ACR_SHIFT_MASK;

	/* VIA2's timers stop, and its CA2 and CB2 take the SCSI controller's lines. */
	*via_register(MAC68K_VIA2_BASE, VIA_ACR) = 0;
	*via_register(MAC68K_VIA2_BASE, VIA_PCR) = VIA2_PCR_VALUE;

	/*
	 * Drives port B bit 6 low as an output, for the A/UX wiring.  It is
	 * driven high first, so that the change is seen even when the bit
	 * already read low.
	 */
	value = *via_register(MAC68K_VIA1_BASE, VIA_ORB);
	*via_register(MAC68K_VIA1_BASE, VIA_ORB) = value | VIA1_PORTB_AUX_WIRING;
	value = *via_register(MAC68K_VIA1_BASE, VIA_DDRB);
	*via_register(MAC68K_VIA1_BASE, VIA_DDRB) = value | VIA1_PORTB_AUX_WIRING;
	value = *via_register(MAC68K_VIA1_BASE, VIA_ORB);
	*via_register(MAC68K_VIA1_BASE, VIA_ORB) = value & (uint8_t)~VIA1_PORTB_AUX_WIRING;
	hal_puts("MAC68K VIA READY\n");
}

/*
 * Lets the tick's flag raise its interrupt.
 */
void
mac68k_via_enable_clock(
	void)
{
	/* The flag is cleared first so that a stale one does not count. */
	*via_register(MAC68K_VIA1_BASE, VIA_IFR) = VIA_FLAG_TIMER1;
	*via_register(MAC68K_VIA1_BASE, VIA_IER) = VIA_IER_SET | VIA_FLAG_TIMER1;
}

/*
 * Reports whether the tick's flag is raised.
 */
bool
mac68k_via_clock_pending(
	void)
{
	uint8_t flags;

	/* The flag register shows the flag whether or not it is let through. */
	flags = *via_register(MAC68K_VIA1_BASE, VIA_IFR);
	if ((flags & VIA_FLAG_TIMER1) != 0)
		return true;

	/* The flag is down. */
	return false;
}

/*
 * Turns the power off through VIA2, on the machines whose power it
 * controls; on the others it does nothing and the caller stops.
 */
void
mac68k_via_power_off(
	void)
{
	const struct mac68k_board_info *board;
	uint8_t value;

	/* Only the Quadra 800's power is VIA2's in the machines the HAL knows. */
	board = mac68k_board();
	if (board->model != MAC68K_MODEL_QUADRA_800)
		return;

	/* Drives the power bit low as an output. */
	value = *via_register(MAC68K_VIA2_BASE, VIA_DDRB);
	*via_register(MAC68K_VIA2_BASE, VIA_DDRB) = value | VIA2_PORTB_POWER;
	value = *via_register(MAC68K_VIA2_BASE, VIA_ORB);
	*via_register(MAC68K_VIA2_BASE, VIA_ORB) = value & (uint8_t)~VIA2_PORTB_POWER;
}

/*
 * Serves the sources behind one processor level.
 */
void
mac68k_via_take_interrupt(
	unsigned level)
{
	/* Chooses the controller by the level. */
	switch (level) {
	case LEVEL_VIA1:
		serve_via(MAC68K_VIA1_BASE, IRQ_VIA1_FIRST);
		break;
	case LEVEL_VIA2:
		serve_via(MAC68K_VIA2_BASE, IRQ_VIA2_FIRST);
		break;
	case LEVEL_SCC:
		dispatch(IRQ_SCC);
		break;
	case LEVEL_NMI:
		hal_puts("MAC68K NMI (interrupt switch)\n");
		break;
	default:
		/* Counted only: the other levels have no source the HAL serves. */
		if (level < 8U)
			stray_interrupts[level]++;
		break;
	}
}

/*
 * Serves the board's interrupt sources behind a processor level.
 */
void
m68040_board_interrupt(
	unsigned level)
{
	/* The VIAs and the serial controller. */
	mac68k_via_take_interrupt(level);
}

/*
 * Closes the interrupt mask and reports whether it was open.
 */
bool
hal_irq_disable(
	void)
{
	bool enabled;

	/* The status register's mask. */
	enabled = m68040_irq_save();

	/* Succeeded: whether the mask was open. */
	return enabled;
}

/*
 * Opens the interrupt mask.
 */
void
hal_irq_enable(
	void)
{
	/* Every level is let in. */
	m68040_irq_restore(true);
}

/*
 * Keeps a source from raising its interrupt.
 */
void
hal_irq_mask(
	int irq)
{
	int valid;

	/* Refuses a source the board does not have. */
	valid = valid_source(irq);
	if (!valid)
		HAL_FATAL("bad mac68k IRQ mask");

	/* The VIA's enable bit; the serial controller's own driver masks it. */
	set_enabled((unsigned)irq, false);
}

/*
 * Lets a source raise its interrupt again.
 */
void
hal_irq_unmask(
	int irq)
{
	int valid;

	/* Refuses a source the board does not have. */
	valid = valid_source(irq);
	if (!valid)
		HAL_FATAL("bad mac68k IRQ unmask");

	/* The VIA's enable bit; the serial controller's own driver unmasks it. */
	set_enabled((unsigned)irq, true);
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

	/* Requires a handler and a source that is not the tick's. */
	if (handler == NULL)
		return HAL_ERR_INVALID;
	valid = valid_source(irq);
	if (!valid)
		return HAL_ERR_INVALID;
	if ((unsigned)irq == IRQ_CLOCK)
		return HAL_ERR_INVALID;

	/* Records the handler. */
	enabled = m68040_spin_lock(&slots_lock);

	slots[irq].handler = handler;
	slots[irq].argument = argument;

	m68040_spin_unlock(&slots_lock, enabled);

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

	/* Masks the source and forgets the handler, when it is the one registered. */
	enabled = m68040_spin_lock(&slots_lock);

	if (slots[irq].handler != handler || slots[irq].argument != argument) {
		m68040_spin_unlock(&slots_lock, enabled);
		return HAL_ERR_INVALID;
	}
	set_enabled((unsigned)irq, false);
	slots[irq].handler = NULL;
	slots[irq].argument = NULL;

	m68040_spin_unlock(&slots_lock, enabled);

	/* Succeeded: the source has no handler. */
	return HAL_OK;
}

/*
 * Ends the interrupt a handler took.  The flag was cleared before the
 * handler ran, so there is nothing to tell the VIA.
 */
void
hal_irq_send_eoi(
	hal_irq_ack_t acknowledge)
{
	struct m68040_percpu *cpu;

	/* Requires the acknowledgement of the interrupt being handled. */
	cpu = m68040_percpu_current();
	if (acknowledge == HAL_IRQ_ACK_NONE || acknowledge != cpu->active_ack)
		HAL_FATAL("invalid mac68k IRQ acknowledgement");
	cpu->active_ack = HAL_IRQ_ACK_NONE;
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

	/* Requires a source the board has. */
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
 * Accepts the trigger the VIA flags have: an edge, which the flag holds
 * until it is cleared.
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
	if (trigger != HAL_IRQ_TRIGGER_EDGE)
		return HAL_ERR_UNSUPPORTED;

	/* Succeeded: the flags latch edges. */
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

/* Reports whether an interrupt number names a source of the board. */
static int
valid_source(
	int irq)
{
	/* VIA1's flags, VIA2's flags, and the serial controller. */
	if (irq >= (int)IRQ_VIA1_FIRST && irq < (int)(IRQ_VIA1_FIRST + VIA_FLAG_COUNT))
		return 1;
	if (irq >= (int)IRQ_VIA2_FIRST && irq < (int)(IRQ_VIA2_FIRST + VIA_FLAG_COUNT))
		return 1;
	if (irq == (int)IRQ_SCC)
		return 1;

	/* Anything else is no source. */
	return 0;
}

/* Reports one register of a VIA. */
static volatile uint8_t *
via_register(
	uint32_t base,
	uint32_t offset)
{
	/* The registers are bytes 0x200 apart. */
	return (volatile uint8_t *)(uintptr_t)(base + offset);
}

/*
 * Serves every raised and enabled flag of one VIA, lowest first, until
 * none is left.  Each flag is cleared before its source is served.
 */
static void
serve_via(
	uint32_t base,
	unsigned first_irq)
{
	uint8_t flags;
	uint8_t enables;
	uint8_t pending;
	unsigned bit;

	/* Takes the raised flags that are let through, until none is. */
	for (;;) {
		flags = *via_register(base, VIA_IFR);
		enables = *via_register(base, VIA_IER);
		pending = flags & enables & VIA_FLAGS;
		if (pending == 0)
			return;

		/* The lowest raised flag first. */
		for (bit = 0; bit < VIA_FLAG_COUNT; bit++) {
			if ((pending & (1U << bit)) != 0)
				break;
		}

		/* Clears the flag, then hands its source on. */
		*via_register(base, VIA_IFR) = (uint8_t)(1U << bit);
		dispatch(first_irq + bit);
	}
}

/*
 * Hands one source to the clock or to its handler.  A source with no
 * handler is masked, so that it does not raise its interrupt forever.
 */
static void
dispatch(
	unsigned irq)
{
	struct m68040_percpu *cpu;
	hal_irq_handler_t handler;
	hal_irq_ack_t acknowledge;
	void *argument;

	/* The tick goes to the clock. */
	if (irq == IRQ_CLOCK) {
		mac68k_clock_interrupt();
		return;
	}

	/* Refuses an interrupt inside another one. */
	cpu = m68040_percpu_current();
	if (cpu->active_ack != HAL_IRQ_ACK_NONE)
		HAL_FATAL("nested mac68k IRQ acknowledgement");

	/* The handler ends the interrupt with hal_irq_send_eoi(). */
	acknowledge = (hal_irq_ack_t)irq;
	handler = slots[irq].handler;
	argument = slots[irq].argument;
	if (handler != NULL) {
		cpu->active_ack = acknowledge;
		handler((int)irq, acknowledge, argument);
		return;
	}

	/* No handler: the source is masked and reported. */
	set_enabled(irq, false);
	hal_printf("unexpected mac68k IRQ %u\n", irq);
}

/*
 * Sets or clears a source's enable bit at its VIA.  The serial
 * controller has no bit at a VIA; its driver enables it at the
 * controller.
 */
static void
set_enabled(
	unsigned irq,
	bool enabled)
{
	uint32_t base;
	uint8_t bit;

	/* Finds the VIA and the flag of the source. */
	if (irq >= IRQ_VIA1_FIRST && irq < IRQ_VIA1_FIRST + VIA_FLAG_COUNT) {
		base = MAC68K_VIA1_BASE;
		bit = (uint8_t)(1U << (irq - IRQ_VIA1_FIRST));
	} else if (irq >= IRQ_VIA2_FIRST && irq < IRQ_VIA2_FIRST + VIA_FLAG_COUNT) {
		base = MAC68K_VIA2_BASE;
		bit = (uint8_t)(1U << (irq - IRQ_VIA2_FIRST));
	} else {
		return;
	}

	/* Writing a bit with the set bit enables it, without it disables it. */
	if (enabled) {
		*via_register(base, VIA_IER) = (uint8_t)(VIA_IER_SET | bit);
	} else {
		*via_register(base, VIA_IER) = bit;
	}
}
