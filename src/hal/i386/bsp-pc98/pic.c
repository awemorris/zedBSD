#include <hal/hal.h>
#include "../asm.h"
#include "../pic.h"
#include "../int.h"

#define PIC_MASTER_PORT1	0x0000
#define PIC_MASTER_PORT2	0x0002
#define PIC_SLAVE_PORT1		0x0008
#define PIC_SLAVE_PORT2		0x000a

#define SLAVE_IRQ		7

static uint8_t master_mask;
static uint8_t slave_mask;

static void
pic_update_cascade_mask(void)
{
	if (slave_mask == 0xffU)
		master_mask |= (uint8_t)(1U << SLAVE_IRQ);
	else
		master_mask &= (uint8_t)~(1U << SLAVE_IRQ);
	asm_outb(PIC_MASTER_PORT2, master_mask);
}

/*
 * Initialize the PIC.
 */
void pic_init(void)
{
	/* Initialize the 8259A master. */
	asm_outb(PIC_MASTER_PORT1, 0x11);		/* Start init, edge-triggered / cascaded */
	asm_outb(PIC_MASTER_PORT2, INT_IRQ_BASE);	/* INT E0h-EFh */
	asm_outb(PIC_MASTER_PORT2, 1 << SLAVE_IRQ);	/* Connect to the slave */
	asm_outb(PIC_MASTER_PORT2, 0x01);		/* 80x86 mode */

	/* Initialize the 8259A slave. */
	asm_outb(PIC_SLAVE_PORT1, 0x11);		/* Start init, edge-triggered / cascaded */
	asm_outb(PIC_SLAVE_PORT2, INT_IRQ_BASE + 8);	/* INT E8h-EFh */
	asm_outb(PIC_SLAVE_PORT2, SLAVE_IRQ);		/* Connect to the master */
	asm_outb(PIC_SLAVE_PORT2, 0x01);		/* 80x86 mode */

	/* Mask all IRQs. */
	master_mask = 0xffU;
	slave_mask = 0xffU;
	asm_outb(PIC_MASTER_PORT2, master_mask);
	asm_outb(PIC_SLAVE_PORT2, slave_mask);
}
/*
 * Set the IRQ mask.
 */
void pic_set_irq_mask(
	int	irq_num,	/* IRQ number */
	int	mask)		/* 0: allow, 1: disallow */
{
	unsigned bit;

	if (irq_num < 0 || irq_num > 15)
		return;
	if (irq_num < 8) {
		/* IRQ7 is reserved for the slave and is derived from its mask. */
		if (irq_num != SLAVE_IRQ) {
			if (mask)
				master_mask |= (uint8_t)(1U << irq_num);
			else
				master_mask &= (uint8_t)~(1U << irq_num);
		}
		pic_update_cascade_mask();
		return;
	}

	bit = (unsigned)irq_num - 8U;
	if (mask)
		slave_mask |= (uint8_t)(1U << bit);
	else
		slave_mask &= (uint8_t)~(1U << bit);

	/* Close the cascade only after the final slave source is masked. */
	if (mask) {
		asm_outb(PIC_SLAVE_PORT2, slave_mask);
		pic_update_cascade_mask();
	} else {
		pic_update_cascade_mask();
		asm_outb(PIC_SLAVE_PORT2, slave_mask);
	}
}

/*
 * Get the in-service IRQ number.
 */
int pic_get_irq_in_service(void)
{
	uint8_t in_service;
	int irq_num;

	/* Read ISR register to know in-service IRQ number. */
	asm_outb(PIC_MASTER_PORT1, 0x0B);
	in_service = asm_inb(PIC_MASTER_PORT1);

	/* Search the bit to get the IRQ number. */
	irq_num = -1;
	while(in_service != 0) {
		irq_num++;
		in_service >>= 1;
	}

	/* If slave IRQ. */
	if (irq_num == 7) {
		asm_outb(PIC_SLAVE_PORT1, 0x0B);
		in_service = asm_inb(PIC_SLAVE_PORT1);

		irq_num = 7;
		while(in_service != 0) {
			irq_num++;
			in_service >>= 1;
		}
	}

	return irq_num;
}

/*
 * Send EOI.
 */
void pic_send_eoi(int irq_num)
{
	/* Sent EOI. */
	if(irq_num <= 7) {
		/* Send EOI to master. */
		asm_outb(PIC_MASTER_PORT1, 0x20);
	} else {
		/* Send EOI to slave. */
		asm_outb(PIC_SLAVE_PORT1, 0x20);

		/* Read slave ISR. */
		asm_outb(PIC_SLAVE_PORT1, 0x0B);

		/* If there is no remaining ISR: */
		if(asm_inb(PIC_SLAVE_PORT1) == 0) {
			/* Also send EOI to master. */
			asm_outb(PIC_MASTER_PORT1, 0x20);
		}
	}
}

/*
 * PC-98 device windows that must never reach the allocator: text and
 * graphics VRAM with the BIOS/ROM window above them, and the 15-16MB
 * C-bus hole.  The Cirrus linear aperture lives at 0xf0000000, beyond
 * the allocator's reach.
 */
/*
 * Total RAM from the BIOS work area: byte 0x401 counts extended memory
 * below 16MB in 128KB units, word 0x594 counts memory above 16MB in MB.
 * The low megabyte is always present.
 */
uint32_t
bsp_mem_probe(void)
{
	uint32_t low_ext = (uint32_t)(*(volatile uint8_t *)(SYS_START + 0x401))
		<< 17;
	uint32_t high = (uint32_t)(*(volatile uint16_t *)(SYS_START + 0x594))
		<< 20;

	return 0x100000 + low_ext + high;
}

void
hal_pc98_memory_segments(uint32_t *low_extended, uint32_t *high_mib)
{
	if (low_extended != NULL)
		*low_extended =
			(uint32_t)(*(volatile uint8_t *)(SYS_START + 0x401)) << 17;
	if (high_mib != NULL)
		*high_mib = (uint32_t)(*(volatile uint16_t *)(SYS_START + 0x594));
}

void
hal_pc98_enable_high_memory(void)
{
	uint8_t value;

	asm_outb(0x00f2, 0x00);
	asm_outb(0x00f6, 0x02);
	value = asm_inb(0x0439);
	asm_outb(0x0439, (uint8_t)(value & 0xfb));
	asm_outb(0x00f8, 0x00);
	asm_outb(0x043b, 0x04);
}
