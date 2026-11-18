/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * BCM43455: the three files the chip is given, and what is done with them.
 *
 * The package bcm43455-firmware installs the firmware, the regulatory
 * table (CLM) and the board's NVRAM settings under /lib/firmware with
 * their upstream names.  The driver reads them whole, checks their size
 * and form, turns the NVRAM text into the chip's form, and sends the CLM
 * to the running firmware in pieces.  It never changes the files.  Only
 * bcm43455_files_load() touches the file system; everything else here is
 * a pure function.
 */

#ifndef KERN_DRIVERS_BCM43455_FIRMWARE_H
#define KERN_DRIVERS_BCM43455_FIRMWARE_H

#include <stddef.h>
#include <stdint.h>

/* Where the package puts the files (userland/firmware/bcm43455). */
#define BCM43455_FIRMWARE_PATH		"/lib/firmware/cypress/cyfmac43455-sdio-minimal.bin"
#define BCM43455_CLM_PATH		"/lib/firmware/cypress/cyfmac43455-sdio.clm_blob"
#define BCM43455_NVRAM_PATH		"/lib/firmware/brcm/brcmfmac43455-sdio.txt"

/*
 * The files' sizes, as the package's manifest fixes them.  The package's
 * build checks their SHA-256; a file of another size is another version
 * the driver was not checked with.
 */
#define BCM43455_FIRMWARE_SIZE		548728U
#define BCM43455_CLM_SIZE		2676U
#define BCM43455_NVRAM_SIZE		2074U

/* The most bytes the NVRAM settings take in the chip's form. */
#define BCM43455_NVRAM_BLOB_MAX		4096U

/*
 * The most CLM bytes one clmload message carries.
 *
 * Value referenced: Infineon wifi-host-driver bd99da7b whd_clm.c (lines 54-58).
 */
#define BCM43455_CLM_CHUNK_MAX		1400U

/* The clmload message's header before its CLM bytes. */
#define BCM43455_CLM_HEADER_LENGTH	12U

/* The room the firmware's version text is copied into. */
#define BCM43455_VERSION_TEXT_MAX	192U

/*
 * The three files, read whole.
 *
 * Each buffer is the driver's own allocation of exactly the file's size.
 * The owner releases them with bcm43455_files_release(), which also clears
 * the pointers.
 */
struct bcm43455_files {
	uint8_t *firmware;
	size_t firmware_length;
	uint8_t *clm;
	size_t clm_length;
	uint8_t *nvram;
	size_t nvram_length;
};

/*
 * Reads the three files and checks them.
 *
 * The caller holds no lock: the file system takes its own.  Returns
 * ENOENT when the package is not installed, EINVAL for a file of the
 * wrong size or form, and on any failure releases what it read.  Thread
 * context only.
 */
int bcm43455_files_load(struct bcm43455_files *files);

/* Releases the files' buffers. */
void bcm43455_files_release(struct bcm43455_files *files);

/*
 * Reports whether the root file system is mounted, so the files can be
 * looked for.  Thread context only.
 */
int bcm43455_files_root_ready(void);

/*
 * Checks the firmware image and copies its version text.
 *
 * The image carries the text "43455c0-roml/..." that names its build;
 * version receives it, cut at version_capacity - 1 bytes and terminated.
 * Returns EINVAL for an image of the wrong size or without the text.
 */
int bcm43455_firmware_check(const uint8_t *firmware, size_t length, char *version, size_t version_capacity);

/*
 * Checks the CLM file: its size and the "BLOB" its container starts with.
 */
int bcm43455_clm_check(const uint8_t *clm, size_t length);

/*
 * Checks the NVRAM file: its size, and that it is text.
 */
int bcm43455_nvram_check(const uint8_t *text, size_t length);

/*
 * Turns the NVRAM text into the form the chip reads at the top of its RAM.
 *
 * Every "key=value" line becomes "key=value" and a NUL; comments, blank
 * lines and the spaces around a line are dropped.  A NUL ends the list,
 * zeros pad it to four bytes, and a length word follows: the padded
 * length in words in its low half and the complement in its high half.
 * blob_length receives the whole length, word included.  Returns ENOSPC
 * when the result does not fit capacity and EINVAL when no setting is
 * left.
 */
int bcm43455_nvram_pack(const uint8_t *text, size_t length, uint8_t *blob, size_t capacity, size_t *blob_length);

/*
 * Reads the NVRAM text's macaddr setting.
 *
 * The board's NVRAM carries one placeholder address shared by every
 * board; a chip that reports it has no address of its own.  Returns ENOENT
 * when the setting is absent and EINVAL when it is not an address.
 */
int bcm43455_nvram_mac(const uint8_t *text, size_t length, uint8_t mac[6]);

/*
 * Finds where the converted NVRAM goes: directly below the top of RAM, so
 * its length word is RAM's last four bytes.
 *
 * Returns EINVAL when the blob is not whole words or does not fit above
 * the firmware.
 */
int bcm43455_nvram_place(uint32_t ram_base, uint32_t ram_size, size_t firmware_length, size_t blob_length, uint32_t *address);

/*
 * Builds the clmload message that carries the CLM bytes from offset on.
 *
 * The first message is marked as the beginning and the last as the end.
 * message_length receives the message's length and taken the CLM bytes it
 * carries.  Returns EINVAL for an offset at or past the end and ENOSPC for
 * a buffer too small.
 */
int bcm43455_clm_message(uint8_t *message, size_t capacity, const uint8_t *clm, size_t clm_length, size_t offset, size_t *message_length, size_t *taken);

#endif
