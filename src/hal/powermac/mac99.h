/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerPC Mac's shared parts, the same for the 32-bit and the 64-bit
 * processor: what the firmware reported, the tick, the interrupt
 * controller and the power manager, which each processor's board start
 * and trap handling call.
 */

#ifndef KERN_HAL_POWERMAC_MAC99_H
#define KERN_HAL_POWERMAC_MAC99_H

#include <hal/hal.h>

/*
 * The ids of the boot device in the kernel's handoff: the initial
 * ramdisk, or the disk the firmware read the kernel from.
 */
#define MAC99_BOOT_ID_RAMDISK	0x80U
#define MAC99_BOOT_ID_DISK	0x81U

/* The most device windows the firmware's tree is read for. */
#define MAC99_WINDOWS_MAX	8U

/* A device window: the 256 MiB block of the address space a device sits in. */
#define MAC99_WINDOW_MASK	0xf0000000U

/*
 * What the firmware reported, read once before the HAL takes the machine:
 * the RAM (the run of banks from the first one's start, and all of the
 * banks), the time base's rate, the display, the interrupt controller,
 * the device windows the kernel needs (the blocks of the interrupt
 * controller and the I/O controller, of the PCI host's registers, of the
 * display and of the PCI host's memory ranges, in that order) and the
 * power manager (PMU): its VIA's registers and interrupt, and the GPIO
 * line it raises when it has something to report (0 and the GPIO 0
 * when the board has none).
 */
struct mac99_board_info {
	uint32_t memory_base;
	uint32_t memory_size;
	uint64_t memory_total;
	uint32_t ramdisk_base;
	uint32_t ramdisk_size;
	uint32_t timebase_frequency;
	uint32_t framebuffer_address;
	uint32_t framebuffer_width;
	uint32_t framebuffer_height;
	uint32_t framebuffer_linebytes;
	uint32_t framebuffer_depth;
	uint32_t openpic_base;
	uint32_t window_count;
	uint32_t windows[MAC99_WINDOWS_MAX];
	uint32_t pmu_base;
	uint32_t pmu_interrupt;
	uint32_t pmu_gpio;
	uint32_t pmu_gpio_interrupt;
	uint32_t pmu_gpio_sense;
};

/*
 * One line of a PCI device as the firmware's device tree routes it to
 * the interrupt controller: the device's configuration address (bus in
 * bits 16 to 23, device in 11 to 15, function in 8 to 10), the pin it
 * raises (1 to 4, or 0 for whichever), the controller's source and
 * Apple's sense cell for it.
 */
struct mac99_interrupt_route {
	uint32_t address;
	uint32_t pin;
	uint32_t source;
	uint32_t sense;
};

const struct mac99_board_info *mac99_board(void);
const struct mac99_interrupt_route *mac99_interrupt_routes(unsigned *count);
void mac99_firmware_read(uint32_t initrd_address, uint32_t initrd_size);
const struct kern_boot_handoff *mac99_kernel_handoff(void);
bool mac99_boot_option(const char *word);

void mac99_clock_init(void);
void mac99_clock_sample(uint32_t address);
const uint32_t *mac99_clock_samples(void);
void mac99_clock_interrupt(void);
bool mac99_clock_ticked(void);

void mac99_openpic_init(uint32_t base, const struct mac99_interrupt_route *routes, unsigned route_count);
void mac99_openpic_sense(uint32_t source, uint32_t sense);
void mac99_take_external(void);
const uint32_t *mac99_irq_watch(void);

void mac99_pmu_init(void);
void mac99_pmu_poweroff(void);
void mac99_pmu_restart(void);

#endif
