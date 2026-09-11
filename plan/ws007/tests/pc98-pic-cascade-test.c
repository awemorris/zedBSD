/*
 * WS007 GFX-T02: production PC-98 PIC cascade mask fixture.
 *
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../src/hal/i386/bsp-pc98/pic.c"

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, \
		    __LINE__, #expression); \
		exit(1); \
	} \
} while (0)

struct mask_transition {
	int irq;
	int masked;
	uint8_t master;
	uint8_t slave;
};

static uint8_t ports[UINT16_MAX + 1U];
static unsigned port_writes;

void
asm_outb(uint16_t port, uint8_t value)
{
	ports[port] = value;
	port_writes++;
}

uint8_t
asm_inb(uint16_t port)
{
	return ports[port];
}

static void
expect_masks(uint8_t master, uint8_t slave)
{
	CHECK(ports[PIC_MASTER_PORT2] == master);
	CHECK(ports[PIC_SLAVE_PORT2] == slave);
}

static void
reset_pic(void)
{
	memset(ports, 0, sizeof(ports));
	port_writes = 0;
	pic_init();
	expect_masks(0xffU, 0xffU);
}

static void
check_slave_mask_matrix(void)
{
	static const struct mask_transition transitions[] = {
		{13, 0, 0x7fU, 0xdfU},
		{ 9, 0, 0x7fU, 0xddU},
		{13, 1, 0x7fU, 0xfdU},
		{ 7, 1, 0x7fU, 0xfdU},
		{ 9, 1, 0xffU, 0xffU},
	};
	size_t index;

	reset_pic();
	for (index = 0; index < ARRAY_COUNT(transitions); index++) {
		pic_set_irq_mask(transitions[index].irq,
		    transitions[index].masked);
		expect_masks(transitions[index].master,
		    transitions[index].slave);
	}
}

static void
check_reserved_cascade(void)
{
	reset_pic();
	pic_set_irq_mask(7, 0);
	expect_masks(0xffU, 0xffU);
	pic_set_irq_mask(13, 0);
	expect_masks(0x7fU, 0xdfU);
	pic_set_irq_mask(7, 1);
	expect_masks(0x7fU, 0xdfU);
	pic_set_irq_mask(7, 0);
	expect_masks(0x7fU, 0xdfU);
	pic_set_irq_mask(13, 1);
	expect_masks(0xffU, 0xffU);
}

static void
check_unrelated_master_bits(void)
{
	reset_pic();
	pic_set_irq_mask(1, 0);
	pic_set_irq_mask(4, 0);
	expect_masks(0xedU, 0xffU);
	pic_set_irq_mask(13, 0);
	expect_masks(0x6dU, 0xdfU);
	pic_set_irq_mask(1, 1);
	expect_masks(0x6fU, 0xdfU);
	pic_set_irq_mask(13, 1);
	expect_masks(0xefU, 0xffU);
	pic_set_irq_mask(4, 1);
	expect_masks(0xffU, 0xffU);
}

static void
check_invalid_irqs(void)
{
	static const int invalid_irqs[] = {-1, 16, INT_MIN, INT_MAX};
	unsigned writes;
	size_t index;

	reset_pic();
	writes = port_writes;
	for (index = 0; index < ARRAY_COUNT(invalid_irqs); index++) {
		pic_set_irq_mask(invalid_irqs[index], 0);
		pic_set_irq_mask(invalid_irqs[index], 1);
	}
	CHECK(port_writes == writes);
	expect_masks(0xffU, 0xffU);
}

int
main(void)
{
	check_slave_mask_matrix();
	check_reserved_cascade();
	check_unrelated_master_bits();
	check_invalid_irqs();
	return 0;
}
