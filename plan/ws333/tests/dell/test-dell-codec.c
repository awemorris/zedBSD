/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host unit test of the Dell provider's pure parts (acpi-dell-codec.c):
 * the BIOS thermal mode each strategy picks, the mode names, the decoding
 * of the DDV fan information, and the buffers of the SMBIOS thermal mode
 * call and of the SMM fan calls with the checks of their answers.  The fan
 * information of the first case is what the Latitude 5330's DDV gave under
 * Debian (one fan at 3897 rpm).
 *
 * The last line is "test-dell-codec: PASS (N checks)" or
 * "test-dell-codec: FAIL (M of N checks)".
 */

#include <stdio.h>
#include <string.h>

#include <drivers/acpi/dell.h>
#include <uapi/errno.h>

/* The number of checks run and failed. */
static unsigned checks;
static unsigned failures;

static void check(int passed, const char *what);
static unsigned mode_of(unsigned value, unsigned speed, unsigned noise);
static void test_modes(void);
static void test_fans(void);
static void test_mode_call(void);
static void test_smm_call(void);

/*
 * Runs every check and prints the verdict.
 */
int
main(void)
{
	/* The four parts. */
	test_modes();
	test_fans();
	test_mode_call();
	test_smm_call();

	/* The verdict. */
	if (failures != 0) {
		printf("test-dell-codec: FAIL (%u of %u checks)\n", failures, checks);
		return 1;
	}
	printf("test-dell-codec: PASS (%u checks)\n", checks);
	return 0;
}

/* Counts one check and reports it when it fails. */
static void
check(
	int passed,
	const char *what)
{
	checks++;
	if (passed)
		return;
	failures++;
	printf("  FAIL: %s\n", what);
}

/* The mode a strategy with the axes picks. */
static unsigned
mode_of(
	unsigned value,
	unsigned speed,
	unsigned noise)
{
	struct kern_thermal_strategy strategy;

	memset(&strategy, 0, sizeof(strategy));
	strategy.value = value;
	strategy.speed = (unsigned char)speed;
	strategy.noise = (unsigned char)noise;
	strategy.gating = 50;
	return drv_acpi_dell_mode_for(&strategy);
}

/* The strategies' modes and the names. */
static void
test_modes(void)
{
	const char *name;
	unsigned mode;

	check(mode_of(KERN_THERMAL_STRATEGY_PERFORMANCE, 0, 0) == DRV_ACPI_DELL_MODE_PERFORMANCE, "performance -> Performance");
	check(mode_of(KERN_THERMAL_STRATEGY_BALANCED, 0, 0) == DRV_ACPI_DELL_MODE_OPTIMIZED, "balanced -> Optimized");
	check(mode_of(KERN_THERMAL_STRATEGY_POWERSAVE, 0, 0) == DRV_ACPI_DELL_MODE_COOL, "powersave -> Cool");
	check(mode_of(KERN_THERMAL_STRATEGY_FIRMWARE, 0, 0) == DRV_ACPI_DELL_MODE_NONE, "firmware -> none");
	check(mode_of(9, 0, 0) == DRV_ACPI_DELL_MODE_NONE, "unknown strategy -> none");

	/* The manual thresholds: noise 33 and below is quiet, 67 and up with speed 67 and up is performance. */
	check(mode_of(KERN_THERMAL_STRATEGY_MANUAL, 100, 0) == DRV_ACPI_DELL_MODE_QUIET, "manual noise 0 speed 100 -> Quiet");
	check(mode_of(KERN_THERMAL_STRATEGY_MANUAL, 50, 33) == DRV_ACPI_DELL_MODE_QUIET, "manual noise 33 -> Quiet");
	check(mode_of(KERN_THERMAL_STRATEGY_MANUAL, 50, 34) == DRV_ACPI_DELL_MODE_OPTIMIZED, "manual noise 34 -> Optimized");
	check(mode_of(KERN_THERMAL_STRATEGY_MANUAL, 67, 67) == DRV_ACPI_DELL_MODE_PERFORMANCE, "manual 67/67 -> Performance");
	check(mode_of(KERN_THERMAL_STRATEGY_MANUAL, 66, 67) == DRV_ACPI_DELL_MODE_OPTIMIZED, "manual speed 66 noise 67 -> Optimized");
	check(mode_of(KERN_THERMAL_STRATEGY_MANUAL, 67, 66) == DRV_ACPI_DELL_MODE_OPTIMIZED, "manual speed 67 noise 66 -> Optimized");
	check(mode_of(KERN_THERMAL_STRATEGY_MANUAL, 100, 100) == DRV_ACPI_DELL_MODE_PERFORMANCE, "manual 100/100 -> Performance");
	check(mode_of(KERN_THERMAL_STRATEGY_MANUAL, 0, 100) == DRV_ACPI_DELL_MODE_OPTIMIZED, "manual speed 0 noise 100 -> Optimized");

	/* Every mode has a name that fits the core's 15 characters; others have none. */
	for (mode = 0; mode < DRV_ACPI_DELL_MODE_COUNT; mode++) {
		name = drv_acpi_dell_mode_name(mode);
		check(name != NULL && strlen(name) > 0 && strlen(name) <= 15, "mode name fits");
	}
	name = drv_acpi_dell_mode_name(DRV_ACPI_DELL_MODE_OPTIMIZED);
	check(name != NULL && strcmp(name, "Optimized") == 0, "name Optimized");
	name = drv_acpi_dell_mode_name(DRV_ACPI_DELL_MODE_PERFORMANCE);
	check(name != NULL && strcmp(name, "Performance") == 0, "name Performance");
	check(drv_acpi_dell_mode_name(DRV_ACPI_DELL_MODE_COUNT) == NULL, "no name past the modes");
	check(drv_acpi_dell_mode_name(DRV_ACPI_DELL_MODE_NONE) == NULL, "no name for none");
}

/* The DDV fan information. */
static void
test_fans(void)
{
	static const unsigned char machine[4] = { 0x00, 0x39, 0x0f, 0xff };
	static const unsigned char none[1] = { 0x00 };
	static const unsigned char two[7] = { 0x00, 0x10, 0x0e, 0x01, 0x00, 0x00, 0xff };
	static const unsigned char unclosed[4] = { 0x00, 0x39, 0x0f, 0x00 };
	unsigned char five[16];
	struct drv_acpi_dell_fan fans[DRV_ACPI_DELL_FANS_MAX];
	unsigned count;
	unsigned index;
	int error;

	/* The 5330's one fan at 3897 rpm. */
	error = drv_acpi_dell_fans_decode(4, machine, sizeof(machine), fans, DRV_ACPI_DELL_FANS_MAX, &count);
	check(error == 0 && count == 1, "5330: one fan");
	check(fans[0].type == 0 && fans[0].rpm == 3897, "5330: kind 0 at 3897 rpm");

	/* A machine whose controller reports no fan: length 0 and one byte that is not a fan. */
	count = 9;
	error = drv_acpi_dell_fans_decode(0, none, sizeof(none), fans, DRV_ACPI_DELL_FANS_MAX, &count);
	check(error == 0 && count == 0, "length 0: no fan");

	/* Only the end mark. */
	count = 9;
	error = drv_acpi_dell_fans_decode(1, &machine[3], 1, fans, DRV_ACPI_DELL_FANS_MAX, &count);
	check(error == 0 && count == 0, "end mark alone: no fan");

	/* Two fans, the second stopped. */
	error = drv_acpi_dell_fans_decode(7, two, sizeof(two), fans, DRV_ACPI_DELL_FANS_MAX, &count);
	check(error == 0 && count == 2, "two fans");
	check(fans[0].rpm == 3600 && fans[1].type == 1 && fans[1].rpm == 0, "two fans: kinds and speeds");

	/* Refusals: not whole fans, past the buffer, no end mark. */
	error = drv_acpi_dell_fans_decode(5, two, sizeof(two), fans, DRV_ACPI_DELL_FANS_MAX, &count);
	check(error == EINVAL && count == 0, "length 5 refused");
	error = drv_acpi_dell_fans_decode(7, machine, sizeof(machine), fans, DRV_ACPI_DELL_FANS_MAX, &count);
	check(error == EINVAL, "length past the buffer refused");
	error = drv_acpi_dell_fans_decode(4, unclosed, sizeof(unclosed), fans, DRV_ACPI_DELL_FANS_MAX, &count);
	check(error == EINVAL, "no end mark refused");

	/* Five fans: the first four are taken. */
	for (index = 0; index < 5; index++) {
		five[index * 3] = (unsigned char)index;
		five[index * 3 + 1] = (unsigned char)(index + 1);
		five[index * 3 + 2] = 0;
	}
	five[15] = 0xff;
	error = drv_acpi_dell_fans_decode(16, five, sizeof(five), fans, DRV_ACPI_DELL_FANS_MAX, &count);
	check(error == E2BIG && count == DRV_ACPI_DELL_FANS_MAX, "five fans: four taken, E2BIG");
	check(fans[3].type == 3 && fans[3].rpm == 4, "five fans: the fourth");
}

/* The SMBIOS thermal mode call. */
static void
test_mode_call(void)
{
	static const unsigned char expected_cool[DRV_ACPI_DELL_SMBIOS_SIZE] = {
		0x11, 0x00, 0x13, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	unsigned char request[DRV_ACPI_DELL_SMBIOS_SIZE];
	unsigned char answer[DRV_ACPI_DELL_SMBIOS_SIZE];
	unsigned mode;
	uint32_t status;
	int error;

	/* The whole buffer of the cool mode, and the input bit of each mode. */
	drv_acpi_dell_mode_request(DRV_ACPI_DELL_MODE_COOL, request);
	check(memcmp(request, expected_cool, sizeof(request)) == 0, "cool request bytes");
	for (mode = 0; mode < DRV_ACPI_DELL_MODE_COUNT; mode++) {
		drv_acpi_dell_mode_request(mode, request);
		check(request[8] == (1U << mode) && request[9] == 0 && request[4] == 1, "mode input bit");
	}

	/* An answer the firmware never touched. */
	drv_acpi_dell_mode_request(DRV_ACPI_DELL_MODE_QUIET, request);
	memcpy(answer, request, sizeof(answer));
	error = drv_acpi_dell_mode_answer(answer, sizeof(answer), &status);
	check(error == ENXIO, "untouched outputs: ENXIO");

	/* A completed call. */
	memset(&answer[0x14], 0, 4);
	error = drv_acpi_dell_mode_answer(answer, sizeof(answer), &status);
	check(error == 0 && status == 0, "status 0: completed");

	/* A call that failed (-2), and one that wrote another output while output0 stayed all ones. */
	answer[0x14] = 0xfe;
	answer[0x15] = 0xff;
	answer[0x16] = 0xff;
	answer[0x17] = 0xff;
	error = drv_acpi_dell_mode_answer(answer, sizeof(answer), &status);
	check(error == EIO && status == 0xfffffffeU, "status -2: EIO");
	memcpy(answer, request, sizeof(answer));
	answer[0x18] = 0;
	error = drv_acpi_dell_mode_answer(answer, sizeof(answer), &status);
	check(error == EIO && status == 0xffffffffU, "output1 written, output0 -1: EIO");

	/* A buffer too short for the outputs. */
	error = drv_acpi_dell_mode_answer(answer, 0x23, &status);
	check(error == EINVAL, "short answer: EINVAL");
}

/* The SMM fan calls. */
static void
test_smm_call(void)
{
	static const unsigned char expected_get[DRV_ACPI_DELL_SMM_SIZE] = {
		0x04, 0x00, 0x00, 0x00, 0xa3, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
		0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	static const unsigned char expected_set[DRV_ACPI_DELL_SMM_SIZE] = {
		0x04, 0x00, 0x00, 0x00, 0xa3, 0x01, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
		0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	unsigned char request[DRV_ACPI_DELL_SMM_SIZE];
	unsigned char answer[DRV_ACPI_DELL_SMM_SIZE];
	uint32_t eax;
	int error;

	/* The get of fan 1 and the set of fan 0 to the high state. */
	drv_acpi_dell_fan_get_request(1, request);
	check(memcmp(request, expected_get, sizeof(request)) == 0, "get fan 1 bytes");
	drv_acpi_dell_fan_set_request(0, DRV_ACPI_DELL_FAN_STATE_HIGH, request);
	check(memcmp(request, expected_set, sizeof(request)) == 0, "set fan 0 high bytes");

	/* An answer that left EAX as the command. */
	memcpy(answer, request, sizeof(answer));
	error = drv_acpi_dell_smm_answer(answer, sizeof(answer), request, &eax);
	check(error == ENXIO && eax == 0x1a3, "EAX unchanged: ENXIO");

	/* The SMM's refusals: the low 16 bits all ones. */
	answer[4] = 0xff;
	answer[5] = 0xff;
	error = drv_acpi_dell_smm_answer(answer, sizeof(answer), request, &eax);
	check(error == EIO, "EAX 0xffff: EIO");
	answer[6] = 0x01;
	error = drv_acpi_dell_smm_answer(answer, sizeof(answer), request, &eax);
	check(error == EIO && eax == 0x1ffff, "EAX 0x1ffff: EIO");

	/* An answer: the state 2 in the low byte. */
	answer[4] = 0x02;
	answer[5] = 0x00;
	answer[6] = 0x00;
	error = drv_acpi_dell_smm_answer(answer, sizeof(answer), request, &eax);
	check(error == 0 && eax == 2, "EAX 2: answered");

	/* A buffer too short for the registers. */
	error = drv_acpi_dell_smm_answer(answer, 31, request, &eax);
	check(error == EINVAL, "short answer: EINVAL");
}
