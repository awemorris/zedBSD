/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of the NVMe power state and Autonomous Power State Transition
 * helpers of include/drivers/pci/pci-nvme-protocol.h: the Identify
 * Controller's power state descriptors, the table of transitions and the
 * Set Features command.
 *
 *   cc -std=c99 -Wall -Wextra -Werror -I include \
 *       -o build/tmp/nvme-apst-test plan/ws300/tests/host/nvme-apst-test.c
 *
 * The drive of the cases named BG5 is the Kioxia BG5 of a Latitude 5330
 * (plan/ws300/survey-5330.md): PS0 to PS2 operational (4.0, 2.5 and 1.3 W,
 * 1 us each way), PS3 0.05 W (800 us in, 1000 us out), PS4 0.003 W
 * (3000 us in, 44900 us out).
 */

#include <drivers/pci/pci-nvme-protocol.h>

#include <stdio.h>
#include <string.h>

/* How many checks failed; the test passes only when it stays zero. */
static unsigned failures;

/* An Identify Controller page the cases fill in. */
static uint8_t identify[4096];

/* A table of transitions, larger than needed so that a write past it shows. */
static uint8_t table[DRV_NVME_APST_TABLE_SIZE + 16U];

static void
check(int condition, const char *what)
{
	if (!condition) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

/* Writes one power state descriptor: the power in MP units, the scale, operational or not, the latencies. */
static void
put_state(unsigned index, uint16_t power, int fine_scale, int non_operational, uint32_t entry_us, uint32_t exit_us)
{
	uint8_t *descriptor;
	unsigned byte;

	descriptor = identify + 2048U + index * 32U;
	memset(descriptor, 0, 32U);
	descriptor[0] = (uint8_t)power;
	descriptor[1] = (uint8_t)(power >> 8);
	descriptor[3] = (uint8_t)((fine_scale ? 1U : 0U) | (non_operational ? 2U : 0U));
	for (byte = 0; byte < 4U; byte++) {
		descriptor[4U + byte] = (uint8_t)(entry_us >> (8U * byte));
		descriptor[8U + byte] = (uint8_t)(exit_us >> (8U * byte));
	}
}

/* Fills the page with the BG5's five states, APSTA set. */
static void
put_bg5(void)
{
	memset(identify, 0, sizeof(identify));
	identify[263] = 4;
	identify[265] = 1;
	put_state(0, 400, 0, 0, 1, 1);
	put_state(1, 250, 0, 0, 1, 1);
	put_state(2, 130, 0, 0, 1, 1);
	put_state(3, 500, 1, 1, 800, 1000);
	put_state(4, 30, 1, 1, 3000, 44900);
}

/* Reads entry index of the table: its target state and idle time, and whether its other bits are clear. */
static void
read_entry(unsigned index, unsigned *target, uint32_t *idle_ms, int *clean)
{
	const uint8_t *entry;
	uint32_t low;

	entry = table + index * DRV_NVME_APST_ENTRY_SIZE;
	low = (uint32_t)entry[0] | ((uint32_t)entry[1] << 8) | ((uint32_t)entry[2] << 16) | ((uint32_t)entry[3] << 24);
	*target = (low >> 3) & 31U;
	*idle_ms = low >> 8;
	*clean = (low & 7U) == 0U && entry[4] == 0 && entry[5] == 0 && entry[6] == 0 && entry[7] == 0;
}

/* The BG5: parse, then the table Debian also gives it. */
static void
test_bg5(void)
{
	struct drv_nvme_power_profile profile;
	struct drv_nvme_apst_summary summary;
	unsigned target;
	uint32_t idle_ms;
	unsigned index;
	int clean;
	int error;

	put_bg5();
	check(drv_nvme_power_parse(identify, sizeof(identify), &profile), "bg5 parse");
	check(profile.count == 5U && profile.apst_supported == 1U, "bg5 count and APSTA");
	check(profile.states[0].max_power_uw == 4000000U && !profile.states[0].non_operational, "bg5 ps0");
	check(profile.states[2].max_power_uw == 1300000U, "bg5 ps2 power");
	check(profile.states[3].max_power_uw == 50000U && profile.states[3].non_operational, "bg5 ps3 power (MXPS)");
	check(profile.states[3].entry_latency_us == 800U && profile.states[3].exit_latency_us == 1000U, "bg5 ps3 latency");
	check(profile.states[4].max_power_uw == 3000U && profile.states[4].exit_latency_us == 44900U, "bg5 ps4");
	check(profile.states[5].max_power_uw == 0U && profile.states[31].entry_latency_us == 0U, "bg5 undescribed states zero");

	memset(table, 0xa5, sizeof(table));
	error = drv_nvme_apst_build(&profile, DRV_NVME_APST_LATENCY_LIMIT_US, table, sizeof(table), &summary);
	check(error == 0, "bg5 build");
	check(summary.transitions == 4U, "bg5 four transitions");
	for (index = 0; index < 3U; index++) {
		read_entry(index, &target, &idle_ms, &clean);
		check(target == 3U && idle_ms == 100U && clean, "bg5 ps0-ps2 to ps3 after 100 ms");
		check(summary.target[index] == 3U && summary.idle_ms[index] == 100U, "bg5 summary ps0-ps2");
	}
	read_entry(3, &target, &idle_ms, &clean);
	check(target == 4U && idle_ms == 2000U && clean, "bg5 ps3 to ps4 after 2000 ms");
	read_entry(4, &target, &idle_ms, &clean);
	check(target == 0U && idle_ms == 0U && clean, "bg5 ps4 stays");
	check(summary.target[4] == 4U && summary.idle_ms[4] == 0U, "bg5 summary ps4");
	for (index = 5; index < DRV_NVME_POWER_STATES_MAX; index++) {
		read_entry(index, &target, &idle_ms, &clean);
		check(target == 0U && idle_ms == 0U && clean, "bg5 unused entries zero");
	}
	check(table[DRV_NVME_APST_TABLE_SIZE] == 0xa5 && table[DRV_NVME_APST_TABLE_SIZE + 15U] == 0xa5, "bg5 nothing written past the table");

	/* A tighter limit leaves out PS4 (47.9 ms), so PS3 stays. */
	error = drv_nvme_apst_build(&profile, 47899U, table, sizeof(table), &summary);
	check(error == 0 && summary.transitions == 3U && summary.idle_ms[3] == 0U, "bg5 limit below ps4");
	error = drv_nvme_apst_build(&profile, 47900U, table, sizeof(table), &summary);
	check(error == 0 && summary.transitions == 4U && summary.target[3] == 4U, "bg5 limit at ps4");

	/* A limit below PS3 (1.8 ms) sends PS0 to PS2 straight to PS4 only when PS4 fits; here neither fits. */
	error = drv_nvme_apst_build(&profile, 1799U, table, sizeof(table), &summary);
	check(error == 0 && summary.transitions == 0U, "bg5 limit below ps3");
}

/* The two idle times: 15000 us in and out is quick, 15001 us is not. */
static void
test_idle_boundary(void)
{
	struct drv_nvme_power_profile profile;
	struct drv_nvme_apst_summary summary;
	int error;

	memset(identify, 0, sizeof(identify));
	identify[263] = 2;
	identify[265] = 1;
	put_state(0, 300, 0, 0, 5, 5);
	put_state(1, 10, 0, 1, 7000, 8000);
	put_state(2, 1, 0, 1, 7000, 8001);
	check(drv_nvme_power_parse(identify, sizeof(identify), &profile), "boundary parse");
	error = drv_nvme_apst_build(&profile, DRV_NVME_APST_LATENCY_LIMIT_US, table, sizeof(table), &summary);
	check(error == 0, "boundary build");
	check(summary.target[0] == 1U && summary.idle_ms[0] == 100U, "15000 us is quick");
	check(summary.target[1] == 2U && summary.idle_ms[1] == 2000U, "15001 us is slow");
}

/* States that are never targets: operational, unreported latencies, more power, too slow; and the sums that do not overflow. */
static void
test_excluded(void)
{
	struct drv_nvme_power_profile profile;
	struct drv_nvme_apst_summary summary;
	int error;

	memset(identify, 0, sizeof(identify));
	identify[263] = 5;
	identify[265] = 1;
	put_state(0, 100, 0, 0, 1, 1);
	put_state(1, 50, 0, 0, 1, 1);
	put_state(2, 40, 0, 1, 0, 100);
	put_state(3, 200, 0, 1, 100, 100);
	put_state(4, 30, 0, 1, 0xffffffffU, 0xffffffffU);
	put_state(5, 20, 0, 1, 100, 200);
	check(drv_nvme_power_parse(identify, sizeof(identify), &profile), "excluded parse");
	error = drv_nvme_apst_build(&profile, DRV_NVME_APST_LATENCY_LIMIT_US, table, sizeof(table), &summary);
	check(error == 0, "excluded build");
	check(summary.target[0] == 5U, "ps0 skips operational, unreported, hungrier and slow states");
	check(summary.target[1] == 5U && summary.target[2] == 5U && summary.target[3] == 5U && summary.target[4] == 5U, "ps1-ps4 to ps5");
	check(summary.idle_ms[5] == 0U && summary.transitions == 5U, "ps5 stays");

	/* The power compared is the state's own: a hungrier state is not a target from a thriftier one. */
	put_state(5, 60, 0, 1, 100, 200);
	check(drv_nvme_power_parse(identify, sizeof(identify), &profile), "excluded parse 2");
	error = drv_nvme_apst_build(&profile, DRV_NVME_APST_LATENCY_LIMIT_US, table, sizeof(table), &summary);
	check(error == 0 && summary.target[0] == 5U && summary.idle_ms[1] == 0U && summary.idle_ms[4] == 0U, "power compared per state");
}

/* Refusals and the edges of the page. */
static void
test_refusals(void)
{
	struct drv_nvme_power_profile profile;
	struct drv_nvme_apst_summary summary;
	int error;

	put_bg5();
	check(!drv_nvme_power_parse(identify, 2048U + 32U * 32U - 1U, &profile), "short page refused");
	check(drv_nvme_power_parse(identify, 2048U + 32U * 32U, &profile), "page just long enough");
	check(!drv_nvme_power_parse(NULL, sizeof(identify), &profile), "no page refused");
	error = drv_nvme_apst_build(&profile, DRV_NVME_APST_LATENCY_LIMIT_US, table, DRV_NVME_APST_TABLE_SIZE - 1U, &summary);
	check(error == EINVAL, "short table refused");

	/* All 32 states described. */
	identify[263] = 31;
	check(drv_nvme_power_parse(identify, sizeof(identify), &profile) && profile.count == 32U, "32 states");
	error = drv_nvme_apst_build(&profile, DRV_NVME_APST_LATENCY_LIMIT_US, table, DRV_NVME_APST_TABLE_SIZE, &summary);
	check(error == 0, "32 states build");

	/* APSTA clear is reported, and the table is still built (the driver decides). */
	identify[263] = 4;
	identify[265] = 0;
	check(drv_nvme_power_parse(identify, sizeof(identify), &profile) && profile.apst_supported == 0U, "APSTA clear");
}

/* The Set Features command. */
static void
test_command(void)
{
	struct drv_nvme_command command;

	check(drv_nvme_apst_command(&command, 9, 1, UINT64_C(0x12345000)), "command built");
	check((command.cdw0 & 0xffU) == DRV_NVME_ADMIN_SET_FEATURES && (command.cdw0 >> 16) == 9U, "opcode and id");
	check(command.cdw10 == 0x0cU && command.cdw11 == 1U, "feature 0Ch, APSTE");
	check(command.prp1 == UINT64_C(0x12345000) && command.prp2 == 0U && command.namespace_id == 0U, "data pointer");
	check(drv_nvme_apst_command(&command, 9, 0, UINT64_C(0x12345f00)) && command.cdw11 == 0U, "disable, table at the end of a page");
	check(!drv_nvme_apst_command(&command, 9, 1, UINT64_C(0x12345f04)), "table across a page refused");
	check(!drv_nvme_apst_command(&command, 9, 1, UINT64_C(0x12345002)), "unaligned table refused");
	check(!drv_nvme_apst_command(&command, 9, 1, 0), "no table refused");
}

int
main(void)
{
	test_bg5();
	test_idle_boundary();
	test_excluded();
	test_refusals();
	test_command();
	if (failures != 0U) {
		printf("nvme-apst-test: FAIL (%u)\n", failures);
		return 1;
	}
	printf("nvme-apst-test: PASS\n");
	return 0;
}
