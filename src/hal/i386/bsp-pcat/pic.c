/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "../asm.h"
#include "../int.h"
#include "../pic.h"

#define MASTER_COMMAND 0x20U
#define MASTER_DATA 0x21U
#define SLAVE_COMMAND 0xa0U
#define SLAVE_DATA 0xa1U

static uint8_t master_mask, slave_mask;

void
pic_init(void)
{
	asm_outb(MASTER_COMMAND, 0x11U);
	asm_outb(MASTER_DATA, INT_IRQ_BASE);
	asm_outb(MASTER_DATA, 1U << 2);
	asm_outb(MASTER_DATA, 0x01U);
	asm_outb(SLAVE_COMMAND, 0x11U);
	asm_outb(SLAVE_DATA, INT_IRQ_BASE + 8U);
	asm_outb(SLAVE_DATA, 2U);
	asm_outb(SLAVE_DATA, 0x01U);
	master_mask = slave_mask = 0xffU;
	asm_outb(MASTER_DATA, master_mask);
	asm_outb(SLAVE_DATA, slave_mask);
}

void
pic_set_irq_mask(int irq, int masked)
{
	if (irq < 0 || irq > 15) return;
	if (irq < 8) {
		if (masked) master_mask |= (uint8_t)(1U << irq);
		else master_mask &= (uint8_t)~(1U << irq);
		asm_outb(MASTER_DATA, master_mask);
	} else {
		unsigned bit = (unsigned)irq - 8U;
		if (masked) slave_mask |= (uint8_t)(1U << bit);
		else {
			slave_mask &= (uint8_t)~(1U << bit);
			master_mask &= (uint8_t)~(1U << 2);
			asm_outb(MASTER_DATA, master_mask);
		}
		asm_outb(SLAVE_DATA, slave_mask);
	}
}

static int
highest(uint8_t value)
{
	int result = -1;
	while (value != 0) { result++; value >>= 1; }
	return result;
}

int
pic_get_irq_in_service(void)
{
	int master, slave;
	asm_outb(MASTER_COMMAND, 0x0bU);
	master = highest(asm_inb(MASTER_COMMAND));
	if (master != 2) return master;
	asm_outb(SLAVE_COMMAND, 0x0bU);
	slave = highest(asm_inb(SLAVE_COMMAND));
	return slave < 0 ? 2 : slave + 8;
}

void
pic_send_eoi(int irq)
{
	if (irq >= 8) asm_outb(SLAVE_COMMAND, 0x20U);
	asm_outb(MASTER_COMMAND, 0x20U);
}
