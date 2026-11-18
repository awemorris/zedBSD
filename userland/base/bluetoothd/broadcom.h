/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of bluetoothd's Broadcom firmware load without system calls
 * (ws215-p005, plan/ws215/phase001/phase.md section 4): whether the
 * controller is Broadcom's, the patch file a chip's name is loaded from,
 * the records of a .hcd file, the speed's parameters and whether the
 * board's address replaces the controller's.  The host tests build them.
 *
 * The vendor commands' opcodes and parameters were checked against
 * Zephyr's Infineon UART driver and Broadcom's brcm_patchram_plus (both
 * Apache-2.0) and NetBSD's btattach (BSD-2-Clause, which alone sets the
 * controller's UART clock before a speed of 3,000,000 baud); no code is
 * taken from them.
 */

#ifndef BLUETOOTHD_BROADCOM_H
#define BLUETOOTHD_BROADCOM_H

#include <stddef.h>
#include <stdint.h>

/* The Company Identifiers of Broadcom and of Cypress, which makes the same chips (Bluetooth SIG Assigned Numbers). */
#define BTD_BROADCOM_MANUFACTURER	15U
#define BTD_CYPRESS_MANUFACTURER	305U

/* Where Broadcom's patch files are. */
#define BTD_BROADCOM_FOLDER		"/lib/firmware/brcm"

/* The vendor commands of the load. */
#define BTD_BROADCOM_WRITE_ADDRESS	0xfc01U
#define BTD_BROADCOM_UPDATE_BAUD	0xfc18U
#define BTD_BROADCOM_UART_CLOCK		0xfc45U
#define BTD_BROADCOM_MINIDRIVER		0xfc2eU
#define BTD_BROADCOM_WRITE_RAM		0xfc4cU
#define BTD_BROADCOM_LAUNCH_RAM		0xfc4eU

/* The line's speed the controller's ROM starts at and comes back to after Launch RAM. */
#define BTD_BROADCOM_BOOT_BAUD		115200U

/*
 * The controller's UART clock (Set UART Clock's parameter, NetBSD's
 * btattach): 48 MHz for 3,000,000 baud and faster, else 24 MHz.
 */
#define BTD_BROADCOM_CLOCK_48		0x01U
#define BTD_BROADCOM_CLOCK_24		0x02U
#define BTD_BROADCOM_CLOCK_48_BAUD	3000000U

/* The bytes of Update UART Baud Rate's parameters: two zero bytes, then the speed. */
#define BTD_BROADCOM_BAUD_BYTES		6U

/* A .hcd record's header: the opcode (least significant byte first) and the parameters' length. */
#define BTD_BROADCOM_RECORD_HEADER	3U

/* The room of a chip's name as the log and the reason show it, the NUL included. */
#define BTD_BROADCOM_CHIP_MAX		32U

/* One record of a .hcd file: a vendor command and its parameters, inside the file. */
struct btd_broadcom_record {
	uint16_t opcode;
	const uint8_t *parameters;
	size_t length;
};

/*
 * What the start does with the controller's address: keeps it (an address
 * of its own, or the board's already), writes the board's (the controller
 * has a chip's default one), or keeps a default one because the board has
 * none.
 */
enum btd_broadcom_address {
	BTD_BROADCOM_ADDRESS_KEEP = 0,
	BTD_BROADCOM_ADDRESS_WRITE,
	BTD_BROADCOM_ADDRESS_DEFAULT
};

int btd_broadcom_manufacturer(uint16_t manufacturer);
int btd_broadcom_file_name(const uint8_t *name, size_t length, const char *folder, char *path, size_t size, char *chip, size_t chip_size);
int btd_broadcom_hcd_check(const uint8_t *file, size_t length, size_t *records, const char **reason);
int btd_broadcom_hcd_next(const uint8_t *file, size_t length, size_t *offset, struct btd_broadcom_record *record);
void btd_broadcom_baud(uint32_t baud, uint8_t *parameters);
enum btd_broadcom_address btd_broadcom_address_choice(const uint8_t *controller, const uint8_t *board, int have_board);

#endif
