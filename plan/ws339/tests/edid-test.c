/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the UEFI loader's monitor-size selection (video.c): the
 * EDID decoder on base blocks built by hand from the EDID 1.4 byte layout,
 * the search for the EDID protocols on a firmware made of fakes, and the
 * choice of the GOP mode among a fake GOP's modes.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -I. plan/ws339/tests/edid-test.c \
 *      bootloader/uefi/video.c -o edid-test && ./edid-test
 *
 * The last line is "edid-test: PASS" when every check holds.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bootloader/uefi/video.h"

/* The most modes a fake GOP has. */
#define FAKE_MODES_MAX		8U

/* The most handles the fake firmware has. */
#define FAKE_HANDLES_MAX	4U

/*
 * A GOP the test owns: the protocol first, so the loader's pointer to the
 * protocol is a pointer to the whole fake, then the modes it advertises.
 */
struct fake_gop {
	EFI_GRAPHICS_OUTPUT_PROTOCOL protocol;
	EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mode;
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION modes[FAKE_MODES_MAX];
	unsigned set_calls;
};

/*
 * One handle of the fake firmware: the GOP and the two EDID protocols it
 * carries (null for none).
 */
struct fake_handle {
	EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
	EFI_EDID_PROTOCOL *active;
	EFI_EDID_PROTOCOL *discovered;
};

/*
 * The fake firmware's handles, filled by each check before it calls the
 * loader and read by the fake boot services.
 */
static struct fake_handle fake_handles[FAKE_HANDLES_MAX];
static unsigned fake_handle_count;

/*
 * The pool buffers the fake firmware has handed out and not had back; a
 * check ends with none outstanding.
 */
static int fake_pool_outstanding;

/* The number of failed checks. */
static unsigned failures;

static void check(int condition, const char *what);
static void edid_base(uint8_t *edid, UINT32 width, UINT32 height);
static void edid_seal(uint8_t *edid);
static void test_decoder(void);
static void fake_gop_init(struct fake_gop *gop, const UINT32 (*sizes)[3], unsigned count);
static void fake_boot_init(EFI_BOOT_SERVICES *boot);
static UINT32 fake_gop_width(const struct fake_gop *gop);
static UINT32 fake_gop_height(const struct fake_gop *gop);
static void test_search(void);
static void test_select(void);
static EFI_STATUS EFIAPI fake_query_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *protocol, UINT32 index, UINTN *size, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **info);
static EFI_STATUS EFIAPI fake_set_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *protocol, UINT32 index);
static EFI_STATUS EFIAPI fake_free_pool(void *buffer);
static EFI_STATUS EFIAPI fake_locate_handle_buffer(int type, const EFI_GUID *guid, void *key, UINTN *count, EFI_HANDLE **handles);
static EFI_STATUS EFIAPI fake_handle_protocol(EFI_HANDLE handle, const EFI_GUID *guid, void **interface);

/* Runs every check and reports the verdict. */
int
main(void)
{
	/* Runs the three groups of checks. */
	test_decoder();
	test_search();
	test_select();

	/* Reports a failure. */
	if (failures != 0U) {
		printf("edid-test: FAIL (%u)\n", failures);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("edid-test: PASS\n");
	return 0;
}

/* Counts and names one failed check. */
static void
check(
	int condition,
	const char *what)
{
	/* A check that held is silent. */
	if (condition)
		return;

	/* Names the check that did not hold. */
	printf("edid-test: not ok: %s\n", what);
	failures++;
}

/*
 * Builds an EDID 1.4 base block whose first descriptor is a detailed timing
 * of a size, with the blanking of 1920x1080 at 60 Hz (CTA-861 VIC 16:
 * 148.5 MHz, 280 by 45 lines of blanking), and seals its checksum.
 *
 * The bytes are written from the EDID 1.4 layout: the header 00 FF FF FF FF
 * FF FF 00, the version 1 and revision 4 at 0x12 and 0x13, the descriptor at
 * 0x36 (the pixel clock in units of 10 kHz, little-endian; the low 8 bits
 * of the horizontal active and blanking sizes; their bits 11..8 as the upper
 * and lower nibbles of the next byte; the same three bytes for the vertical
 * sizes; the flags at byte 17), and the checksum at 0x7F.
 */
static void
edid_base(
	uint8_t *edid,
	UINT32 width,
	UINT32 height)
{
	uint8_t *timing;

	/* Starts from an empty block with the header. */
	memset(edid, 0, 128);
	edid[0] = 0x00;
	memset(edid + 1, 0xff, 6);
	edid[7] = 0x00;

	/* Version 1, revision 4. */
	edid[0x12] = 1;
	edid[0x13] = 4;

	/* The preferred timing: a pixel clock of 148.5 MHz is 14850 = 0x3A02. */
	timing = edid + 0x36;
	timing[0] = 0x02;
	timing[1] = 0x3a;

	/* The horizontal active size and the 280 (0x118) pixels of blanking. */
	timing[2] = (uint8_t)(width & 0xffU);
	timing[3] = 0x18;
	timing[4] = (uint8_t)(((width >> 8) & 0x0fU) << 4 | 0x01U);

	/* The vertical active size and the 45 (0x2D) lines of blanking. */
	timing[5] = (uint8_t)(height & 0xffU);
	timing[6] = 0x2d;
	timing[7] = (uint8_t)(((height >> 8) & 0x0fU) << 4 | 0x00U);

	/* Progressive, digital separate sync with both polarities positive. */
	timing[17] = 0x1e;

	/* Makes the 128 bytes sum to 0 modulo 256. */
	edid_seal(edid);
}

/* Rewrites a base block's checksum byte so its 128 bytes sum to 0 modulo 256. */
static void
edid_seal(
	uint8_t *edid)
{
	unsigned sum;
	unsigned index;

	/* Sums every byte but the checksum. */
	sum = 0;
	for (index = 0; index < 127U; index++)
		sum += edid[index];

	/* The checksum completes the sum to a multiple of 256. */
	edid[127] = (uint8_t)((256U - (sum & 0xffU)) & 0xffU);
}

/* Checks the decoder on good and bad base blocks. */
static void
test_decoder(void)
{
	static const uint8_t vic16[128] = {
		/*
		 * 1920x1080 at 60 Hz written out byte by byte, computed by hand:
		 * the descriptor at 0x36 is 02 3A 80 18 71 38 2D 40, eight bytes of
		 * 0 and 1E (H active 0x780, V active 0x438).  The bytes before the
		 * checksum sum to 0xFF * 6 (1530) + 1 + 4 + the descriptor's 520
		 * = 2055 = 8 * 256 + 7, so the checksum is 256 - 7 = 249 = 0xF9.
		 */
		0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0x01, 0x04,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0x1e,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0,
		0xf9
	};
	uint8_t edid[256];
	UINT32 width;
	UINT32 height;
	int found;

	/* The hand-computed block decodes to 1920x1080. */
	width = 0;
	height = 0;
	found = zbl_edid_preferred_size(vic16, sizeof(vic16), &width, &height);
	check(found == 1 && width == 1920U && height == 1080U, "hand-computed 1920x1080 block");

	/* The builder makes the same block, checksum included. */
	edid_base(edid, 1920U, 1080U);
	check(memcmp(edid, vic16, 128) == 0, "builder matches the hand-computed block");

	/* Sizes whose low bytes are 0 come from the upper nibbles alone: 2560x1440 is 0xA00 by 0x5A0. */
	edid_base(edid, 2560U, 1440U);
	found = zbl_edid_preferred_size(edid, 128, &width, &height);
	check(found == 1 && width == 2560U && height == 1440U, "2560x1440 from the upper nibbles");

	/* The largest 12-bit sizes. */
	edid_base(edid, 4095U, 4095U);
	found = zbl_edid_preferred_size(edid, 128, &width, &height);
	check(found == 1 && width == 4095U && height == 4095U, "4095x4095");

	/* A block with an extension block after it is still read. */
	edid_base(edid, 1366U, 768U);
	memset(edid + 128, 0x5a, 128);
	edid[126] = 1;
	edid_seal(edid);
	found = zbl_edid_preferred_size(edid, 256, &width, &height);
	check(found == 1 && width == 1366U && height == 768U, "base block with an extension");

	/* A block one byte short is refused. */
	edid_base(edid, 1920U, 1080U);
	found = zbl_edid_preferred_size(edid, 127, &width, &height);
	check(found == 0, "short block refused");

	/* A null block is refused. */
	found = zbl_edid_preferred_size(0, 128, &width, &height);
	check(found == 0, "null block refused");

	/* A checksum off by one is refused. */
	edid_base(edid, 1920U, 1080U);
	edid[127]++;
	found = zbl_edid_preferred_size(edid, 128, &width, &height);
	check(found == 0, "bad checksum refused");

	/* A broken header is refused even with a good checksum. */
	edid_base(edid, 1920U, 1080U);
	edid[3] = 0xfe;
	edid_seal(edid);
	found = zbl_edid_preferred_size(edid, 128, &width, &height);
	check(found == 0, "bad header refused");

	/* An EDID structure of version 2 is refused. */
	edid_base(edid, 1920U, 1080U);
	edid[0x12] = 2;
	edid_seal(edid);
	found = zbl_edid_preferred_size(edid, 128, &width, &height);
	check(found == 0, "version 2 refused");

	/* A first descriptor with a pixel clock of 0 is a display descriptor. */
	edid_base(edid, 1920U, 1080U);
	edid[0x36] = 0;
	edid[0x37] = 0;
	edid_seal(edid);
	found = zbl_edid_preferred_size(edid, 128, &width, &height);
	check(found == 0, "display descriptor refused");

	/* An interlaced timing (flags bit 7) is refused. */
	edid_base(edid, 1920U, 540U);
	edid[0x36 + 17] |= 0x80;
	edid_seal(edid);
	found = zbl_edid_preferred_size(edid, 128, &width, &height);
	check(found == 0, "interlaced timing refused");

	/* A timing of no width is refused. */
	edid_base(edid, 0U, 1080U);
	found = zbl_edid_preferred_size(edid, 128, &width, &height);
	check(found == 0, "empty width refused");
}

/* Makes a fake GOP of modes given as width, height and pixel format, in mode 0. */
static void
fake_gop_init(
	struct fake_gop *gop,
	const UINT32 (*sizes)[3],
	unsigned count)
{
	unsigned index;

	/* Starts with no modes and the services. */
	memset(gop, 0, sizeof(*gop));
	gop->protocol.QueryMode = fake_query_mode;
	gop->protocol.SetMode = fake_set_mode;
	gop->protocol.Mode = &gop->mode;

	/* Describes each mode with rows exactly its width wide. */
	for (index = 0; index < count; index++) {
		gop->modes[index].HorizontalResolution = sizes[index][0];
		gop->modes[index].VerticalResolution = sizes[index][1];
		gop->modes[index].PixelFormat = sizes[index][2];
		gop->modes[index].PixelsPerScanLine = sizes[index][0];
	}

	/* Mode 0 is active. */
	gop->mode.MaxMode = count;
	gop->mode.Mode = 0;
	gop->mode.Info = &gop->modes[0];
	gop->mode.SizeOfInfo = sizeof(gop->modes[0]);
}

/* Fills the boot services the loader's selection calls. */
static void
fake_boot_init(
	EFI_BOOT_SERVICES *boot)
{
	/* Only the services the selection uses are there. */
	memset(boot, 0, sizeof(*boot));
	boot->FreePool = fake_free_pool;
	boot->LocateHandleBuffer = fake_locate_handle_buffer;
	boot->HandleProtocol = fake_handle_protocol;
}

/* Reports the width of a fake GOP's active mode. */
static UINT32
fake_gop_width(
	const struct fake_gop *gop)
{
	/* The width of the active mode. */
	return gop->mode.Info->HorizontalResolution;
}

/* Reports the height of a fake GOP's active mode. */
static UINT32
fake_gop_height(
	const struct fake_gop *gop)
{
	/* The height of the active mode. */
	return gop->mode.Info->VerticalResolution;
}

/* Checks the search for the EDID protocols on the fake firmware. */
static void
test_search(void)
{
	static const UINT32 sizes[1][3] = {{800, 600, PixelBlueGreenRedReserved8BitPerColor}};
	uint8_t big[128];
	uint8_t small[128];
	EFI_EDID_PROTOCOL big_edid;
	EFI_EDID_PROTOCOL small_edid;
	EFI_EDID_PROTOCOL empty_edid;
	EFI_BOOT_SERVICES boot;
	struct fake_gop gop;
	struct fake_gop other;
	UINT32 width;
	UINT32 height;
	int found;

	/* Two monitors' EDIDs, and an empty one. */
	edid_base(big, 2560U, 1440U);
	edid_base(small, 1280U, 1024U);
	big_edid.SizeOfEdid = 128;
	big_edid.Edid = big;
	small_edid.SizeOfEdid = 128;
	small_edid.Edid = small;
	empty_edid.SizeOfEdid = 0;
	empty_edid.Edid = 0;
	fake_gop_init(&gop, sizes, 1);
	fake_gop_init(&other, sizes, 1);
	fake_boot_init(&boot);

	/* One handle with the active EDID, on a GOP other than the loader's (a console splitter's). */
	memset(fake_handles, 0, sizeof(fake_handles));
	fake_handles[0].gop = &other.protocol;
	fake_handles[0].active = &big_edid;
	fake_handle_count = 1;
	width = 0;
	height = 0;
	found = zbl_uefi_video_monitor_size(&boot, &gop.protocol, &width, &height);
	check(found == 1 && width == 2560U && height == 1440U, "the only active EDID");
	check(fake_pool_outstanding == 0, "handle list released (one active)");

	/* The active EDID wins over the discovered one. */
	fake_handles[0].discovered = &small_edid;
	found = zbl_uefi_video_monitor_size(&boot, &gop.protocol, &width, &height);
	check(found == 1 && width == 2560U && height == 1440U, "active before discovered");

	/* An empty active EDID falls back to the discovered one. */
	fake_handles[0].active = &empty_edid;
	found = zbl_uefi_video_monitor_size(&boot, &gop.protocol, &width, &height);
	check(found == 1 && width == 1280U && height == 1024U, "discovered after an empty active");

	/* Only a discovered EDID. */
	fake_handles[0].active = 0;
	found = zbl_uefi_video_monitor_size(&boot, &gop.protocol, &width, &height);
	check(found == 1 && width == 1280U && height == 1024U, "discovered alone");
	check(fake_pool_outstanding == 0, "handle list released (discovered)");

	/* Two monitors: the EDID on the loader's own GOP's handle is taken. */
	memset(fake_handles, 0, sizeof(fake_handles));
	fake_handles[0].gop = &other.protocol;
	fake_handles[0].active = &small_edid;
	fake_handles[1].gop = &gop.protocol;
	fake_handles[1].active = &big_edid;
	fake_handle_count = 2;
	found = zbl_uefi_video_monitor_size(&boot, &gop.protocol, &width, &height);
	check(found == 1 && width == 2560U && height == 1440U, "the EDID of the loader's GOP");

	/* Two monitors, neither on the loader's GOP: the monitor is unknown. */
	fake_handles[1].gop = &other.protocol;
	found = zbl_uefi_video_monitor_size(&boot, &gop.protocol, &width, &height);
	check(found == 0, "two foreign EDIDs give no size");
	check(fake_pool_outstanding == 0, "handle list released (two)");

	/* No EDID at all. */
	memset(fake_handles, 0, sizeof(fake_handles));
	fake_handle_count = 0;
	found = zbl_uefi_video_monitor_size(&boot, &gop.protocol, &width, &height);
	check(found == 0, "no EDID gives no size");

	/* A firmware without LocateHandleBuffer. */
	boot.LocateHandleBuffer = 0;
	found = zbl_uefi_video_monitor_size(&boot, &gop.protocol, &width, &height);
	check(found == 0, "no LocateHandleBuffer gives no size");
}

/* Checks the choice of the GOP mode. */
static void
test_select(void)
{
	static const UINT32 sizes[6][3] = {
		{640, 480, PixelBlueGreenRedReserved8BitPerColor},
		{800, 600, PixelBlueGreenRedReserved8BitPerColor},
		{1024, 768, PixelBlueGreenRedReserved8BitPerColor},
		{1280, 800, PixelBlueGreenRedReserved8BitPerColor},
		{1920, 1080, PixelRedGreenBlueReserved8BitPerColor},
		{2560, 1440, PixelBltOnly}
	};
	static const UINT32 ties[3][3] = {
		{640, 480, PixelBlueGreenRedReserved8BitPerColor},
		{1280, 750, PixelBlueGreenRedReserved8BitPerColor},
		{1200, 800, PixelBlueGreenRedReserved8BitPerColor}
	};
	static const char none[] = "kernel=vmunix";
	static const char video[] = "kernel=vmunix video=800x600";
	static const char missing[] = "kernel=vmunix video=1600x900";
	EFI_BOOT_SERVICES boot;
	struct fake_gop gop;
	EFI_STATUS status;

	/* The monitor's own size is set. */
	fake_boot_init(&boot);
	fake_gop_init(&gop, sizes, 6);
	status = zbl_uefi_video_select(&boot, &gop.protocol, none, strlen(none), 1920U, 1080U, 0U, 0U);
	check(status == EFI_SUCCESS && fake_gop_width(&gop) == 1920U && fake_gop_height(&gop) == 1080U,
	      "monitor 1920x1080 sets 1920x1080");
	check(fake_pool_outstanding == 0, "mode descriptions released");

	/* A monitor size the firmware lacks takes the largest mode inside it. */
	fake_gop_init(&gop, sizes, 6);
	status = zbl_uefi_video_select(&boot, &gop.protocol, none, strlen(none), 1680U, 1050U, 0U, 0U);
	check(status == EFI_SUCCESS && fake_gop_width(&gop) == 1280U && fake_gop_height(&gop) == 800U,
	      "monitor 1680x1050 sets 1280x800");

	/* A mode of the monitor's size in a layout the kernel cannot take is passed over. */
	fake_gop_init(&gop, sizes, 6);
	status = zbl_uefi_video_select(&boot, &gop.protocol, none, strlen(none), 2560U, 1440U, 0U, 0U);
	check(status == EFI_SUCCESS && fake_gop_width(&gop) == 1920U && fake_gop_height(&gop) == 1080U,
	      "monitor 2560x1440 (Blt only) sets 1920x1080");

	/* Between modes of equal area, the one of the monitor's proportions wins. */
	fake_gop_init(&gop, ties, 3);
	status = zbl_uefi_video_select(&boot, &gop.protocol, none, strlen(none), 1920U, 1280U, 0U, 0U);
	check(status == EFI_SUCCESS && fake_gop_width(&gop) == 1200U && fake_gop_height(&gop) == 800U,
	      "monitor 1920x1280 sets 1200x800 (3:2) over 1280x750");

	/* A monitor smaller than every mode falls back to the preferred mode. */
	fake_gop_init(&gop, sizes, 6);
	status = zbl_uefi_video_select(&boot, &gop.protocol, none, strlen(none), 600U, 400U, 1920U, 1080U);
	check(status == EFI_SUCCESS && fake_gop_width(&gop) == 1920U && fake_gop_height(&gop) == 1080U,
	      "monitor 600x400 falls back to the preferred 1920x1080");

	/* With neither, the current mode stays and is not set again. */
	fake_gop_init(&gop, sizes, 6);
	gop.mode.Mode = 1;
	gop.mode.Info = &gop.modes[1];
	status = zbl_uefi_video_select(&boot, &gop.protocol, none, strlen(none), 0U, 0U, 0U, 0U);
	check(status == EFI_SUCCESS && fake_gop_width(&gop) == 800U && gop.set_calls == 0U,
	      "no monitor and no wish keeps the current mode");

	/* Without a monitor size, the preferred mode is set as before. */
	fake_gop_init(&gop, sizes, 6);
	status = zbl_uefi_video_select(&boot, &gop.protocol, none, strlen(none), 0U, 0U, 1920U, 1080U);
	check(status == EFI_SUCCESS && fake_gop_width(&gop) == 1920U, "no monitor sets the preferred mode");

	/* The monitor's mode already active is not set again. */
	fake_gop_init(&gop, sizes, 6);
	gop.mode.Mode = 4;
	gop.mode.Info = &gop.modes[4];
	status = zbl_uefi_video_select(&boot, &gop.protocol, none, strlen(none), 1920U, 1080U, 0U, 0U);
	check(status == EFI_SUCCESS && fake_gop_width(&gop) == 1920U && gop.set_calls == 0U,
	      "active monitor mode is not set again");

	/* video= wins over the monitor. */
	fake_gop_init(&gop, sizes, 6);
	status = zbl_uefi_video_select(&boot, &gop.protocol, video, strlen(video), 1920U, 1080U, 0U, 0U);
	check(status == EFI_SUCCESS && fake_gop_width(&gop) == 800U && fake_gop_height(&gop) == 600U,
	      "video=800x600 wins over the monitor");

	/* A video= the firmware lacks still fails, monitor or not. */
	fake_gop_init(&gop, sizes, 6);
	status = zbl_uefi_video_select(&boot, &gop.protocol, missing, strlen(missing), 1920U, 1080U, 0U, 0U);
	check(status == EFI_UNSUPPORTED, "missing video= fails");
	check(fake_pool_outstanding == 0, "mode descriptions released at the end");
}

/* Describes one fake mode in a pool buffer the caller frees. */
static EFI_STATUS EFIAPI
fake_query_mode(
	EFI_GRAPHICS_OUTPUT_PROTOCOL *protocol,
	UINT32 index,
	UINTN *size,
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **info)
{
	struct fake_gop *gop;
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *copy;

	/* Refuses a mode the GOP does not have. */
	gop = (struct fake_gop *)protocol;
	if (index >= gop->mode.MaxMode)
		return EFI_INVALID_PARAMETER;

	/* Copies the description into a new buffer. */
	copy = malloc(sizeof(*copy));
	if (copy == 0)
		return EFI_OUT_OF_RESOURCES;

	*copy = gop->modes[index];
	fake_pool_outstanding++;

	/* Succeeded: the caller owns the description. */
	*size = sizeof(*copy);
	*info = copy;
	return EFI_SUCCESS;
}

/* Makes one fake mode active. */
static EFI_STATUS EFIAPI
fake_set_mode(
	EFI_GRAPHICS_OUTPUT_PROTOCOL *protocol,
	UINT32 index)
{
	struct fake_gop *gop;

	/* Refuses a mode the GOP does not have. */
	gop = (struct fake_gop *)protocol;
	if (index >= gop->mode.MaxMode)
		return EFI_UNSUPPORTED;

	/* Succeeded: the mode is active. */
	gop->mode.Mode = index;
	gop->mode.Info = &gop->modes[index];
	gop->set_calls++;
	return EFI_SUCCESS;
}

/* Takes back a pool buffer the fake firmware handed out. */
static EFI_STATUS EFIAPI
fake_free_pool(
	void *buffer)
{
	/* Succeeded: one buffer fewer is outstanding. */
	free(buffer);
	fake_pool_outstanding--;
	return EFI_SUCCESS;
}

/* Lists the fake handles that carry a protocol, in a pool buffer the caller frees. */
static EFI_STATUS EFIAPI
fake_locate_handle_buffer(
	int type,
	const EFI_GUID *guid,
	void *key,
	UINTN *count,
	EFI_HANDLE **handles)
{
	EFI_HANDLE *list;
	unsigned index;
	UINTN found;
	int active;

	/* Only a search by protocol is faked. */
	if (type != ByProtocol || key != 0)
		return EFI_INVALID_PARAMETER;

	/* Only the EDID protocols are faked. */
	active = memcmp(guid, &EFI_EDID_ACTIVE_PROTOCOL_GUID, sizeof(*guid)) == 0;
	if (!active && memcmp(guid, &EFI_EDID_DISCOVERED_PROTOCOL_GUID, sizeof(*guid)) != 0)
		return EFI_NOT_FOUND;

	/* Allocates the list. */
	list = malloc(sizeof(*list) * FAKE_HANDLES_MAX);
	if (list == 0)
		return EFI_OUT_OF_RESOURCES;

	/* Lists the handles that carry the protocol. */
	found = 0;
	for (index = 0; index < fake_handle_count; index++) {
		if (active && fake_handles[index].active != 0)
			list[found++] = &fake_handles[index];
		else if (!active && fake_handles[index].discovered != 0)
			list[found++] = &fake_handles[index];
	}

	/* None carries it. */
	if (found == 0) {
		free(list);
		return EFI_NOT_FOUND;
	}

	/* Succeeded: the caller owns the list. */
	fake_pool_outstanding++;
	*count = found;
	*handles = list;
	return EFI_SUCCESS;
}

/* Gives one of a fake handle's protocols. */
static EFI_STATUS EFIAPI
fake_handle_protocol(
	EFI_HANDLE handle,
	const EFI_GUID *guid,
	void **interface)
{
	struct fake_handle *fake;
	void *protocol;

	/* Picks the protocol the GUID names. */
	fake = handle;
	protocol = 0;
	if (memcmp(guid, &EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID, sizeof(*guid)) == 0)
		protocol = fake->gop;
	else if (memcmp(guid, &EFI_EDID_ACTIVE_PROTOCOL_GUID, sizeof(*guid)) == 0)
		protocol = fake->active;
	else if (memcmp(guid, &EFI_EDID_DISCOVERED_PROTOCOL_GUID, sizeof(*guid)) == 0)
		protocol = fake->discovered;

	/* The handle does not carry it. */
	if (protocol == 0)
		return EFI_UNSUPPORTED;

	/* Succeeded: the protocol's interface. */
	*interface = protocol;
	return EFI_SUCCESS;
}
