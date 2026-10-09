/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Host verification of the actual first-scanout command generator. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/display-program.h"

/* The test owns one bounded program and its simulated register observations. */
static struct bcm2711_display_program test_program;

/* Independent register image used to apply write and update commands. */
static uint32_t test_registers[BCM2711_REGION_COUNT][8192];

/* The complete SRAM image used for output-identity checks. */
static uint32_t test_list[BCM2711_LIST_WORDS];

/* Failed observations accumulate until the final result. */
static unsigned test_failures;

static void check(bool condition, const char *what);
static void make_mode(struct bcm2711_display_mode *mode, struct drv_bcm2711_boot_screen *screen);
static void check_program(uint32_t port);
static uint32_t locate(enum bcm2711_display_operation operation, uint32_t region, uint32_t offset, uint32_t value, uint32_t after);
static void apply_words(void);
static void check_refusals(void);
static void check_identity(void);

/*
 * Runs independent field-value, ordering, refusal and framebuffer-identity checks.
 * The simulation proves software preparation, not physical scanout or IRQ delivery.
 */
int
main(
	void)
{
	/* Checks both physical lane mappings and both pixelvalve FIFO thresholds. */
	check_program(0);
	check_program(1);
	check_refusals();
	check_identity();

	/* Reports every failed observation together. */
	if (test_failures != 0) {
		printf("display-program-host-test FAIL %u\n", test_failures);
		return 1;
	}

	/* Succeeded: software values and order match the independent expectations. */
	puts("display-program-host-test PASS");
	return 0;
}

/* Records one observable failure without skipping later independent checks. */
static void
check(
	bool condition,
	const char *what)
{
	/* A failed observation retains its explanation. */
	if (!condition) {
		printf("FAIL %s\n", what);
		test_failures++;
	}
}

/* Supplies a known 1080p60 RGB8 mode and its complete framebuffer. */
static void
make_mode(
	struct bcm2711_display_mode *mode,
	struct drv_bcm2711_boot_screen *screen)
{
	uint32_t checksum;
	uint32_t index;

	/* Initializes the test-owned input before assigning meaningful fields. */
	memset(mode, 0, sizeof(*mode));
	memset(screen, 0, sizeof(*screen));
	mode->width = 1920;
	mode->height = 1080;
	mode->hfront = 88;
	mode->hsync = 44;
	mode->hback = 148;
	mode->vfront = 4;
	mode->vsync = 5;
	mode->vback = 36;
	mode->tmds_hz = 148500000;
	mode->hpositive = true;
	mode->vpositive = true;
	mode->hdmi = true;
	mode->limited = true;
	screen->physical = 0x01000000;
	screen->size = 7680U * 1080U;
	screen->width = 1920;
	screen->height = 1080;
	screen->pitch = 7680;
	screen->format = 0;

	/* Builds a complete CEA AVI for the same RGB mode, with an independent checksum. */
	mode->avi[0] = 0x82;
	mode->avi[1] = 2;
	mode->avi[2] = 13;
	mode->avi[7] = 16;
	checksum = 0;
	for (index = 0; index < 17U; index++) {
		/* The checksum covers header and payload bytes. */
		checksum += mode->avi[index];
	}

	/* Completes the packet so all bytes sum to zero modulo 256. */
	mode->avi[3] = (uint8_t)(0U - checksum);
}

/* Checks concrete register images and dependency order for one output. */
static void
check_program(
	uint32_t port)
{
	struct bcm2711_display_mode mode;
	struct drv_bcm2711_boot_screen screen;
	uint32_t base;
	uint32_t timing;
	uint32_t mux;
	uint32_t list;
	uint32_t vblank;
	uint32_t channel;
	uint32_t phy;
	uint32_t pv;
	uint32_t pixels;
	uint32_t video;
	uint32_t adoption;
	uint32_t core_restore;
	uint32_t capture;
	uint32_t delay;
	uint32_t index;
	uint32_t writes;
	int error;

	/* Calls the production generator with the independent known-mode inputs. */
	make_mode(&mode, &screen);
	error = bcm2711_display_program_prepare(&test_program, &mode, &screen, port, 2, 600000000);
	check(error == 0, "known mode prepares");
	if (error != 0)
		return;
	check(test_program.count < BCM2711_DISPLAY_COMMANDS, "program capacity");
	check(test_program.commands[0].operation == BCM2711_DISPLAY_NOTIFY, "firmware notification precedes HVS writes");
	base = BCM2711_REGION_HDMI0 + port * BCM2711_ENCODER_REGIONS;
	timing = BCM2711_REGION_PV0 + port;

	/* Compares literal hardware values, independent of the generator's formulas. */
	apply_words();
	/* Independent rational reconstruction samples retain every packed field and symmetric SRAM word. */
	check(test_registers[BCM2711_REGION_HVS][0x4080 / 4] == 0x07effc00, "independent filter word 0");
	check(test_registers[BCM2711_REGION_HVS][0x4084 / 4] == 0x07e3eff8, "independent filter word 1");
	check(test_registers[BCM2711_REGION_HVS][0x4088 / 4] == 0x00600ffd, "independent filter word 2");
	check(test_registers[BCM2711_REGION_HVS][0x408c / 4] == 0x01dca632, "independent filter word 3");
	check(test_registers[BCM2711_REGION_HVS][0x4090 / 4] == 0x034d749a, "independent filter word 4");
	check(test_registers[BCM2711_REGION_HVS][0x4094 / 4] == 0x0001c2e1, "independent filter word 5");
	check(test_registers[BCM2711_REGION_HVS][0x4098 / 4] == 0x034d749a, "independent filter word 6");
	check(test_registers[BCM2711_REGION_HVS][0x409c / 4] == 0x01dca632, "independent filter word 7");
	check(test_registers[BCM2711_REGION_HVS][0x40a0 / 4] == 0x00600ffd, "independent filter word 8");
	check(test_registers[BCM2711_REGION_HVS][0x40a4 / 4] == 0x07e3eff8, "independent filter word 9");
	check(test_registers[BCM2711_REGION_HVS][0x40a8 / 4] == 0x07effc00, "independent filter word 10");

	/* The scanout still uses the exact unscaled primary layout. */
	check(test_registers[BCM2711_REGION_HVS][0x40 / 4] == 0x87800438, "HVS active dimensions and enable");
	check(test_registers[BCM2711_REGION_HVS][0x4c / 4] == 0xad806020, "channel0 COB partition");
	check(test_registers[BCM2711_REGION_HVS][0x5c / 4] == 0x60103010, "channel1 COB partition");
	check(test_registers[BCM2711_REGION_HVS][0x6c / 4] == 0x30000000, "channel2 COB partition");
	check(test_registers[BCM2711_REGION_HVS][0x40ac / 4] == 0x4800d807, "new primary control");
	check(test_registers[BCM2711_REGION_HVS][0x40b4 / 4] == 0x4000fff0, "opaque fixed alpha");
	check(test_registers[BCM2711_REGION_HVS][0x40b8 / 4] == 0x04380780, "primary size");
	check(test_registers[BCM2711_REGION_HVS][0x40c0 / 4] == 0xc1000000, "primary address alias");
	check(test_registers[BCM2711_REGION_HVS][0x40c8 / 4] == 0x1e00, "primary pitch");
	check(test_registers[BCM2711_REGION_HVS][0x40cc / 4] == 0x80000000, "primary end marker");
	check(test_registers[base + BCM2711_ENCODER_RATE][0x18 / 4] == 0x8a500000, "PLL ratio includes doubled reference factor");
	check(test_registers[base + BCM2711_ENCODER_PHY][0x28 / 4] == 0x300, "PLL divider");
	check(test_registers[base + BCM2711_ENCODER_CORE][0xe4 / 4] == 0x0058c780, "HDMI full-pixel timing");
	check(test_registers[timing][0x10 / 4] == 0x002c03c0, "PV half-pixel-clock timing");
	check(test_registers[timing][0x0c / 4] == 0x004a0016, "PV horizontal back porch and sync");
	check(test_registers[timing][0x14 / 4] == 0x00240005, "PV vertical back porch and sync");
	check(test_registers[timing][0x18 / 4] == 0x00040438, "PV active lines and front porch");
	if (port == 0) {
		/* HDMI0 uses the uncrossed lane map and its larger PV FIFO. */
		check(test_registers[base + BCM2711_ENCODER_PHY][0x08 / 4] == 0x0909090c, "HDMI0 lane drive");
		check(test_registers[base + BCM2711_ENCODER_PHY][0x10 / 4] == 0x00050003, "HDMI0 lane termination");
		check(test_registers[base + BCM2711_ENCODER_PHY][0x4c / 4] == 0x3210, "HDMI0 lane map");
		check(test_registers[timing][0] == 0x06177003, "HDMI0 PV threshold");
	} else {
		/* HDMI1 crosses data/clock lanes and requires a fixed threshold of 32. */
		check(test_registers[base + BCM2711_ENCODER_PHY][0x08 / 4] == 0x0c090909, "HDMI1 lane drive");
		check(test_registers[base + BCM2711_ENCODER_PHY][0x10 / 4] == 0x00053000, "HDMI1 lane termination");
		check(test_registers[base + BCM2711_ENCODER_PHY][0x4c / 4] == 0x2301, "HDMI1 lane map");
		check(test_registers[timing][0] == 0x00107003, "HDMI1 PV threshold");
	}

	/* Verifies the ordering of the actual operations that make scanout possible. */
	mux = 0x18;
	if (port == 1)
		mux = 0x14;
	mux = locate(BCM2711_DISPLAY_UPDATE, BCM2711_REGION_HVS, mux, 0, 0);
	list = locate(BCM2711_DISPLAY_WRITE, BCM2711_REGION_HVS, 0x40ac, 0x4800d807, 0);
	vblank = locate(BCM2711_DISPLAY_WRITE, timing, 0x24, 0x80, 0);
	channel = locate(BCM2711_DISPLAY_WRITE, BCM2711_REGION_HVS, 0x40, 0x87800438, 0);
	phy = locate(BCM2711_DISPLAY_WRITE, base + BCM2711_ENCODER_PHY, 0, 0x0f, 0);
	pv = locate(BCM2711_DISPLAY_WRITE, timing, 0x10, 0x002c03c0, 0);
	pixels = locate(BCM2711_DISPLAY_UPDATE, timing, 0x04, 1, pv);
	video = locate(BCM2711_DISPLAY_UPDATE, base + BCM2711_ENCODER_SHARED, 0x44 + port * 4, 0xe0810000, 0);
	adoption = locate(BCM2711_DISPLAY_WAIT, BCM2711_REGION_HVS, 0x30, 43, 0);
	core_restore = locate(BCM2711_DISPLAY_CLOCK_RATE, 4, 0, 137600000, adoption);
	if (port == 1)
		core_restore = locate(BCM2711_DISPLAY_CLOCK_RATE, 4, 0, 148500000, adoption);
	check(mux < list && list < vblank && vblank < channel, "mux/list/vblank/channel ordering");
	check(channel < phy && phy < pv && pv < pixels && pixels < video, "HVS/PHY/PV/pixel/video ordering");
	check(video < adoption && adoption < core_restore && core_restore < test_program.count, "adoption precedes core-rate reduction");
	capture = locate(BCM2711_DISPLAY_CAPTURE, base + BCM2711_ENCODER_CORE, 0x74, 0, 0);
	delay = locate(BCM2711_DISPLAY_DELAY, 0, 0, 0, capture);
	check(capture < delay && delay < adoption, "recenter gap precedes adoption");
	writes = 0;
	for (index = 0; index < test_program.count; index++) {
		/* The other physical output must never acquire video enable. */
		if (test_program.commands[index].operation == BCM2711_DISPLAY_UPDATE &&
		    (test_program.commands[index].region == BCM2711_REGION_HDMI0 + BCM2711_ENCODER_SHARED ||
			 test_program.commands[index].region == BCM2711_REGION_HDMI1 + BCM2711_ENCODER_SHARED) &&
		    (test_program.commands[index].offset == 0x44 || test_program.commands[index].offset == 0x48) &&
		    (test_program.commands[index].value & 0x80000000U) != 0)
			writes++;
	}

	/* No additional video-enable action may target the other output. */
	check(writes == 1, "exactly one selected video enable");
}

/* Finds an expected operation after a dependency, or returns the invalid count. */
static uint32_t
locate(
	enum bcm2711_display_operation operation,
	uint32_t region,
	uint32_t offset,
	uint32_t value,
	uint32_t after)
{
	uint32_t index;

	/* Searches the prepared operation stream rather than a duplicate implementation. */
	for (index = after; index < test_program.count; index++) {
		/* All fields identify the hardware action being checked. */
		if (test_program.commands[index].operation == operation &&
		    test_program.commands[index].region == region &&
		    test_program.commands[index].offset == offset &&
		    test_program.commands[index].value == value)
			return index;
	}

	/* Absence is an invalid index that ordering checks cannot mistake for success. */
	return test_program.count;
}

/* Applies only writes and field updates to an independent, zeroed register image. */
static void
apply_words(
	void)
{
	const struct bcm2711_display_command *command;
	uint32_t index;
	uint32_t *word;

	/* No live firmware register value is needed to check these explicit fields. */
	memset(test_registers, 0, sizeof(test_registers));
	for (index = 0; index < test_program.count; index++) {
		/* Non-register commands are covered by the dependency-order observations. */
		command = &test_program.commands[index];
		if (command->operation != BCM2711_DISPLAY_WRITE && command->operation != BCM2711_DISPLAY_UPDATE)
			continue;
		word = &test_registers[command->region][command->offset / 4U];
		if (command->operation == BCM2711_DISPLAY_WRITE) {
			/* Exact writes replace the complete register. */
			*word = command->value;
		} else {
			/* Field updates retain unrelated simulated register bits. */
			*word = (*word & ~command->mask) | command->value;
		}
	}
}

/* Verifies that unsupported inputs leave no destructive program to execute. */
static void
check_refusals(
	void)
{
	struct bcm2711_display_mode mode;
	struct drv_bcm2711_boot_screen screen;
	int error;

	/* Odd timing cannot fit the two-pixel clock of these PV blocks. */
	make_mode(&mode, &screen);
	mode.hfront = 89;
	error = bcm2711_display_program_prepare(&test_program, &mode, &screen, 0, 2, 600000000);
	check(error == ENOTSUP && test_program.count == 0, "odd timing refusal");

	/* A high TMDS rate requires a separate SCDC/DDC implementation. */
	make_mode(&mode, &screen);
	mode.tmds_hz = 340000000;
	error = bcm2711_display_program_prepare(&test_program, &mode, &screen, 0, 2, 600000000);
	check(error == ENOTSUP && test_program.count == 0, "SCDC rate refusal");

	/* A corrupt AVI must not survive a same-mode restart. */
	make_mode(&mode, &screen);
	mode.avi[3]++;
	error = bcm2711_display_program_prepare(&test_program, &mode, &screen, 0, 2, 600000000);
	check(error == EINVAL && test_program.count == 0, "AVI checksum refusal");

	/* A framebuffer outside the compositor aperture cannot be aliased into it. */
	make_mode(&mode, &screen);
	screen.physical = 0x40000000ULL;
	error = bcm2711_display_program_prepare(&test_program, &mode, &screen, 0, 2, 600000000);
	check(error == EINVAL && test_program.count == 0, "unreachable framebuffer refusal");

	/* Truncated RAM cannot hold even one complete frame. */
	make_mode(&mode, &screen);
	screen.size--;
	error = bcm2711_display_program_prepare(&test_program, &mode, &screen, 1, 3, 600000000);
	check(error == EINVAL && test_program.count == 0, "truncated framebuffer refusal");
}

/* Proves output identity without assuming the framebuffer occupies plane zero. */
static void
check_identity(
	void)
{
	struct bcm2711_display_mode mode;
	struct drv_bcm2711_boot_screen screen;
	struct bcm2711_list list;
	bool matches;

	/* A different first plane precedes the actual console plane. */
	make_mode(&mode, &screen);
	memset(test_list, 0, sizeof(test_list));
	test_list[0] = 0x4800d807;
	test_list[3] = 0x04380780;
	test_list[5] = 0xc2000000;
	test_list[7] = 7680;
	test_list[8] = 0x4800d807;
	test_list[11] = 0x04380780;
	test_list[13] = 0xc1000000;
	test_list[15] = 7680;
	test_list[16] = 0x80000000;
	bcm2711_list_decode(test_list, 0, &list);
	matches = bcm2711_list_screen_matches(&list, &screen);
	check(matches, "console plane after an unrelated plane");

	/* Flipped addressing cannot be treated as an unflipped framebuffer match. */
	test_list[9] = 0x80000000;
	bcm2711_list_decode(test_list, 0, &list);
	matches = bcm2711_list_screen_matches(&list, &screen);
	check(!matches, "flipped console plane refusal");

	/* A valid list on a different buffer cannot select the boot output. */
	test_list[9] = 0;
	test_list[13] = 0xc3000000;
	bcm2711_list_decode(test_list, 0, &list);
	matches = bcm2711_list_screen_matches(&list, &screen);
	check(!matches, "different output framebuffer refusal");
}
