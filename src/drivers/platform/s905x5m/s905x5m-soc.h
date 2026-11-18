/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The S905X5M's shared blocks that its peripheral drivers program: the
 * clock controller's gates and source selectors, the reset controller's
 * pulse and level registers, the pin multiplexer and the GPIO banks, and
 * the device tree the bootloader passed.
 */

#ifndef KERN_DRIVERS_PLATFORM_S905X5M_SOC_H
#define KERN_DRIVERS_PLATFORM_S905X5M_SOC_H

#include <stdint.h>

#include <drivers/generic/fdt.h>

/*
 * The clock controller's registers the drivers name: the two words of
 * peripheral gates, and the SD/eMMC clock selector.
 *
 * Values referenced: Hardkernel common_drivers odroids7d-6.12.y @34daba9
 * drivers/clk/meson/s7d.h (CLKCTRL_SYS_CLK_EN0_REG0, CLKCTRL_SYS_CLK_EN0_REG1,
 * CLKCTRL_ETH_CLK_CTRL, CLKCTRL_SD_EMMC_CLK_CTRL).
 */
#define DRV_S905X5M_CLOCK_GATES0	0x044U
#define DRV_S905X5M_CLOCK_GATES1	0x048U
#define DRV_S905X5M_CLOCK_ETHERNET	0x164U
#define DRV_S905X5M_CLOCK_SD_EMMC	0x16cU

/*
 * The GPIO numbers of the device tree, by bank: the tree names a pin by
 * the bank's first number plus the pin's index in the bank.
 *
 * Values referenced: Hardkernel common_drivers odroids7d-6.12.y @34daba9
 * include/dt-bindings/gpio/meson-s7d-gpio.h (GPIOE_0 .. GPIO_CC2).
 */
#define DRV_S905X5M_GPIO_E		0U
#define DRV_S905X5M_GPIO_B		2U
#define DRV_S905X5M_GPIO_C		16U
#define DRV_S905X5M_GPIO_D		24U
#define DRV_S905X5M_GPIO_DV		29U
#define DRV_S905X5M_GPIO_H		36U
#define DRV_S905X5M_GPIO_X		48U
#define DRV_S905X5M_GPIO_Z		68U
#define DRV_S905X5M_GPIO_TEST_N		81U
#define DRV_S905X5M_GPIO_COUNT		82U

/* The pull a pin's input stage applies. */
enum drv_s905x5m_pull {
	DRV_S905X5M_PULL_NONE = 0,
	DRV_S905X5M_PULL_UP,
	DRV_S905X5M_PULL_DOWN
};

/*
 * One interrupt of a device tree node, as the GIC numbers it.
 *
 * irq is the GIC's interrupt number (an SPI's number plus 32), and edge
 * is nonzero for a line the tree says is edge-triggered.
 */
struct drv_s905x5m_interrupt {
	unsigned irq;
	int edge;
};

/*
 * One GPIO a device tree property names: the pin's number and the
 * property's flags (bit 0 active low, bit 1 single ended, bit 2 open
 * drain).
 */
struct drv_s905x5m_gpio {
	unsigned pin;
	uint32_t flags;
};

/* The flags of a GPIO property the drivers act on. */
#define DRV_S905X5M_GPIO_ACTIVE_LOW	0x1U
#define DRV_S905X5M_GPIO_OPEN_DRAIN	0x4U

int drv_s905x5m_soc_init(uint64_t fdt_phys);
const struct drv_fdt *drv_s905x5m_fdt(void);

uint32_t drv_s905x5m_clock_read(uint32_t offset);
void drv_s905x5m_clock_update(uint32_t offset, uint32_t mask, uint32_t value);

void drv_s905x5m_reset_pulse(unsigned bit);
void drv_s905x5m_reset_hold(unsigned bit);
void drv_s905x5m_reset_release(unsigned bit);

int drv_s905x5m_pin_function(unsigned pin, unsigned function);
int drv_s905x5m_pin_pull(unsigned pin, enum drv_s905x5m_pull pull);
int drv_s905x5m_pin_drive(unsigned pin, uint32_t microamps);
int drv_s905x5m_gpio_output(unsigned pin, int high);
int drv_s905x5m_gpio_input(unsigned pin);
int drv_s905x5m_gpio_read(unsigned pin, int *high);

int drv_s905x5m_find_node(const char *compatible, uint64_t address, uint32_t *node);
int drv_s905x5m_node_interrupt(uint32_t node, unsigned index, struct drv_s905x5m_interrupt *interrupt);
int drv_s905x5m_node_gpio(uint32_t node, const char *name, struct drv_s905x5m_gpio *gpio);
uint32_t drv_s905x5m_node_number(uint32_t node, const char *name, uint32_t fallback);
int drv_s905x5m_node_raw_reg(uint32_t node, unsigned index, uint64_t *address, uint64_t *size);

void drv_s905x5m_delay_us(uint64_t microseconds);

#endif
