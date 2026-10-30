/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Raspberry Pi 4 SDIO card layer.
 *
 * Brings the onboard Wi-Fi chip's SDIO interface from power-up to a
 * selected card on a four-line 25 MHz bus, reads its common registers
 * (CCCR) and its card information structure (CIS), enables its functions
 * and sets their block sizes, and moves bytes with CMD52 and CMD53.  It
 * also finds the slot in the device tree.  The values follow the SD
 * Association's SDIO Simplified Specification.
 */

#ifndef KERN_DRIVERS_RPI4_SDIO_H
#define KERN_DRIVERS_RPI4_SDIO_H

#include <stddef.h>
#include <stdint.h>

#include <drivers/generic/fdt.h>
#include "drivers/platform/rpi4/rpi4-gpio.h"
#include "drivers/platform/rpi4/rpi4-sdio-host.h"

/* The most functions an SDIO card has besides function 0. */
#define DRV_RPI4_SDIO_FUNCTION_MAX	7U

/* The most pins the slot's device tree pin group may name. */
#define DRV_RPI4_SDIO_SLOT_PIN_MAX	6U

/* The largest block a function may use. */
#define DRV_RPI4_SDIO_BLOCK_SIZE_MAX	2048U

/*
 * The answer to CMD5 (R4).
 *
 * ready is whether the card has finished its power-up, function_count how
 * many I/O functions it has besides function 0, memory_present whether it
 * is a combined card, and voltages its I/O OCR: one bit per 100 mV window
 * it can run at.
 */
struct drv_rpi4_sdio_ocr {
	unsigned ready;
	unsigned function_count;
	unsigned memory_present;
	uint32_t voltages;
};

/*
 * What the card information structure says.
 *
 * Each part is filled only when its tuple was found: the manufacturer
 * and card codes from CISTPL_MANFID, function 0's largest block and its
 * fastest bus rate code from CISTPL_FUNCE.
 */
struct drv_rpi4_sdio_cis {
	unsigned has_manufacturer;
	uint16_t manufacturer;
	uint16_t card;
	unsigned has_function0;
	uint16_t function0_block_max;
	uint8_t speed_code;
};

/*
 * A way for the card layer to wait.
 *
 * sleep waits the given milliseconds without busy waiting and returns 0,
 * or returns an errno that stops what the card layer is doing, such as
 * ECANCELED when the driver is being stopped.
 */
struct drv_rpi4_sdio_waiter {
	int (*sleep)(void *context, unsigned milliseconds);
	void *context;
};

/*
 * One enumerated SDIO card.
 *
 * Filled by drv_rpi4_sdio_enumerate(); block_size[n] is function n's
 * block size, zero until set.  The host must outlive it.
 */
struct drv_rpi4_sdio_card {
	struct drv_rpi4_sdio_host *host;
	struct drv_rpi4_sdio_ocr ocr;
	uint16_t rca;
	unsigned cccr_revision;
	unsigned sdio_revision;
	uint8_t capability;
	uint32_t cis_pointer;
	struct drv_rpi4_sdio_cis cis;
	uint16_t block_size[DRV_RPI4_SDIO_FUNCTION_MAX + 1U];
};

/*
 * Where the Wi-Fi chip's SDIO slot is and how it is wired.
 *
 * registers is the controller's physical base, irq the kernel's number of
 * its interrupt, and pins, function and pulls the pin group that routes
 * the bus to the chip.
 */
struct drv_rpi4_sdio_slot {
	uint64_t registers;
	int irq;
	unsigned pin_count;
	unsigned pins[DRV_RPI4_SDIO_SLOT_PIN_MAX];
	enum drv_rpi4_gpio_function function;
	enum drv_rpi4_gpio_pull pulls[DRV_RPI4_SDIO_SLOT_PIN_MAX];
};

/*
 * Finds the Wi-Fi chip's SDIO slot in the device tree.
 *
 * The slot is the parent of the brcm,bcm4329-fmac node.  sd_base is the
 * controller the SD card driver bound to.  Touches no register.  Returns
 * ENODEV for a tree without the chip, with the slot disabled, or from
 * QEMU's raspi4b; EBUSY when the SD card driver uses the same controller;
 * EINVAL for a slot the tree describes in a way this layer cannot use.
 */
int drv_rpi4_sdio_describe_slot(const struct drv_fdt *fdt, uintptr_t sd_base, struct drv_rpi4_sdio_slot *slot);

/*
 * Decodes the answer to CMD5.
 */
int drv_rpi4_sdio_decode_ocr(uint32_t reply, struct drv_rpi4_sdio_ocr *ocr);

/*
 * Picks the voltage windows the 3.3 V bus offers out of a card's I/O OCR.
 *
 * Returns ENOTSUP when the card cannot run at 3.3 V.
 */
int drv_rpi4_sdio_voltage_window(uint32_t voltages, uint32_t *window);

/*
 * Decodes the answer to CMD52 (R5): EIO for an error flag, else the data
 * byte.
 */
int drv_rpi4_sdio_decode_r5(uint32_t reply, uint8_t *data);

/*
 * Walks a card information structure's tuple chain.
 *
 * Returns EINVAL when the chain runs past length before its end tuple.
 */
int drv_rpi4_sdio_parse_cis(const uint8_t *bytes, size_t length, struct drv_rpi4_sdio_cis *cis);

/*
 * Brings a freshly powered card to the selected state on a four-line bus
 * at 25 MHz and reads its common registers and its CIS.
 *
 * Waits up to a second for the card to answer and another for its
 * power-up.  Returns ENODEV for a card without I/O functions.
 */
int drv_rpi4_sdio_enumerate(struct drv_rpi4_sdio_card *card, struct drv_rpi4_sdio_host *host, const struct drv_rpi4_sdio_waiter *waiter);

/*
 * Reads one byte of a function's register space with CMD52.
 */
int drv_rpi4_sdio_read_byte(struct drv_rpi4_sdio_card *card, unsigned function, uint32_t address, uint8_t *value);

/*
 * Writes one byte of a function's register space with CMD52.
 */
int drv_rpi4_sdio_write_byte(struct drv_rpi4_sdio_card *card, unsigned function, uint32_t address, uint8_t value);

/*
 * Moves bytes of a function's space with CMD53.
 *
 * increment says whether the card's address moves with the bytes or stays
 * on one port.  A length up to the function's block size moves in byte
 * mode; a longer one must be a whole number of blocks.
 */
int drv_rpi4_sdio_transfer(struct drv_rpi4_sdio_card *card, enum drv_rpi4_sdio_data direction, unsigned function, uint32_t address, int increment, uint8_t *buffer, size_t length);

/*
 * Enables a function and waits up to timeout_ms for it to report ready.
 */
int drv_rpi4_sdio_enable_function(struct drv_rpi4_sdio_card *card, unsigned function, const struct drv_rpi4_sdio_waiter *waiter, unsigned timeout_ms);

/*
 * Disables a function.
 */
int drv_rpi4_sdio_disable_function(struct drv_rpi4_sdio_card *card, unsigned function);

/*
 * Sets a function's block size and reads it back.
 */
int drv_rpi4_sdio_set_block_size(struct drv_rpi4_sdio_card *card, unsigned function, unsigned size);

#endif
