/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The BIOS loader's boot logo: a binary PPM read one sector at a time and
 * turned into framebuffer writes.
 */

#ifndef KERN_BOOTLOADER_BIOS_LOGO_H
#define KERN_BOOTLOADER_BIOS_LOGO_H

/* The most pixel writes one 512-byte sector can give (a pixel is 3 bytes, one may be split). */
#define ZBL_BIOS_LOGO_WRITES		172

/* Where bootzbsd.S finds the results in struct zbl_bios_logo (checked in logo.c). */
#define ZBL_BIOS_LOGO_FILL_NOW		84
#define ZBL_BIOS_LOGO_BACKGROUND	88
#define ZBL_BIOS_LOGO_COUNT		92
#define ZBL_BIOS_LOGO_OFFSETS		96
#define ZBL_BIOS_LOGO_PIXELS		(ZBL_BIOS_LOGO_OFFSETS + 4 * ZBL_BIOS_LOGO_WRITES)
#define ZBL_BIOS_LOGO_TAIL		(ZBL_BIOS_LOGO_PIXELS + 4 * ZBL_BIOS_LOGO_WRITES)
#define ZBL_BIOS_LOGO_SIZE		(ZBL_BIOS_LOGO_TAIL + 32)

/* The longest header comment the decoder keeps to read (fit=contain). */
#define ZBL_BIOS_LOGO_COMMENT		16

#ifndef __ASSEMBLER__
#include <stdint.h>

/*
 * The decoder's state between sectors, and the writes the last sector gave:
 * byte offsets into the linear framebuffer and the pixel for each.
 * fill_now is set once, when the background colour (the first pixel's, or
 * black for fit=contain) is known: the caller fills the screen with background before the writes.
 */
struct zbl_bios_logo {
	uint32_t stage;
	uint32_t numbers[3];
	uint32_t number_index;
	uint32_t digits;
	uint32_t in_comment;
	uint32_t width;
	uint32_t height;
	uint32_t screen_width;
	uint32_t screen_height;
	uint32_t stride;
	uint32_t format;
	uint32_t origin_x;
	uint32_t origin_y;
	uint32_t skip_x;
	uint32_t skip_y;
	uint32_t x;
	uint32_t y;
	uint32_t channels;
	uint8_t channel[3];
	uint8_t reserved;
	uint32_t fill_now;
	uint32_t background;
	uint32_t count;
	uint32_t offsets[ZBL_BIOS_LOGO_WRITES];
	uint32_t pixels[ZBL_BIOS_LOGO_WRITES];

	/*
	 * A picture over black bars (the header comment fit=contain):
	 * whether it is, the comment being read, and the size the picture is
	 * drawn at (origin_x and origin_y are its corner).
	 * Only the loader's C reads these, after the fields bootzbsd.S
	 * knows.
	 */
	uint32_t contain;
	uint32_t comment_length;
	uint8_t comment[ZBL_BIOS_LOGO_COMMENT];
	uint32_t scaled_width;
	uint32_t scaled_height;
};

void
zbl_bios_logo_begin(
	struct zbl_bios_logo *logo,
	uint32_t screen_width,
	uint32_t screen_height,
	uint32_t stride,
	uint32_t format);

int
zbl_bios_logo_feed(
	struct zbl_bios_logo *logo,
	const uint8_t *bytes,
	uint32_t size);

#endif
#endif
