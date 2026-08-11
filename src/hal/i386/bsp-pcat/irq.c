/*
 * BSP: PC/AT IRQ (8259A)
 */

#include <sys/kern/hal.h>
#include <sys/kern/crt.h>
#include <kern/sched.h>

/*
 * 8259A ports.
 */
#define PIC_MASTER_PORT1	0x0020
#define PIC_MASTER_PORT2	0x0021
#define PIC_SLAVE_PORT1		0x00a0
#define PIC_SLAVE_PORT2		0x00a1

/*
 * PC/AT 8259 slave IRQ number.
 */
#define SLAVE_IRQ		2

/*
 * IRQ numbers.
 */
#define IRQ_TIMER		(0)
#define IRQ_KEYBOARD		(1)

/*
 * IRQ count.
 */
#define IRQ_MAX			(15)

/*
 * ISR.
 */
static void (*isr_func)(void)[IRQ_MAX+1];
static void (*isr_arg)(void)[IRQ_MAX+1];

/*
 * Initialize IRQ.
 */
void
bsp_irq_init(void)
{
	int i;

	for (i = 0; i <= IRQ_MAX; i++) {
		isr_func[i] = NULL;
		isr_arg[i] = NULL;
	}

	/*
	 * Initialize PIC.
	 * All IRQ will be masked.
	 */

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
	asm_outb(PIC_MASTER_PORT2, 0xff);
	asm_outb(PIC_SLAVE_PORT2, 0xff);
}

/*
 * Set IRQ CPU affinity.
 */
void
bsp_irq_set_affinity(
	int irq,
	struct hal_cpu_mask cpu_mask)
{
	/* Not supported. */
	return 0;
}

/*
 * Disable IRQ by lock counter.
 */
bool
bsp_irq_disable(void)
{
	int status = asm_get_eflags() & EFLAGS_IF;

	asm_cli();

	/* Return false if IRQ is already disabled. */
	return status ? true : false;
}

/*
 * Enable IRQ.
 */
void
bsp_irq_enable(void)
{
	asm_sti();
}

/*
 * Mask an IRQ.
 */
void
bsp_irq_mask_irq(
	int irq_num)
{
	if(irq_num < 8) {
		uint8 imr = asm_inb(PIC_MASTER_PORT2);
		imr |=  (1 << irq_num);
		asm_outb(PIC_MASTER_PORT2, imr);
	} else {
		uint8 imr = asm_inb(PIC_SLAVE_PORT2);
		imr |=  (1 << (irq_num&7));
		asm_outb(PIC_SLAVE_PORT2, imr);
	}
}

/*
 * Unmask an IRQ.
 */
void bsp_irq_unmask_irq(int irq_num)
{
	if(irq_num < 8) {
		uint8 imr = asm_inb(PIC_MASTER_PORT2);
		imr &= ~(1 << irq_num);
		asm_outb(PIC_MASTER_PORT2, imr);
	} else {
		uint8 imr = asm_inb(PIC_SLAVE_PORT2);
		imr &= ~(1 << (irq_num&7));
		asm_outb(PIC_SLAVE_PORT2, imr);
	}
}

/*
 * Send EOI to the IRQ controller.
 */
void
bsp_irq_send_eoi(int irq)
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
 * Set an IRQ handler.
 */
void
bsp_irq_set_handler(
	int irq_num,
	void (*func)(void *p),
	void *arg)
{
	rt_isr[irq_num] = func;
	rt_isr_arg[irq_num] = arg;
}

/*
 * Get the in-service IRQ number.
 */
int
bsp_irq_get_in_service(void)
{
	uint8 in_service;
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
 * IRQ interrupt handler.
 *  - Called from the interrupt handler.
 */
void
bsp_irq_handler(
	int irq_num)
{
	task_t t;

	/*
	 * Interrupt is disabled here.
	 */

	/* Call a handler. */
	if (rt_isr[irq_num] != NULL) {
		rt_isr[irq_num](rt_isr_arg[irq_num]);
	}
}
