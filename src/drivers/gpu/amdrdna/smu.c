/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Stage A4: the system-management unit's mailbox, the fan and the thermal
 * control, the temperatures, and the driver's watch over them.
 *
 * Stage A3 had the security processor start the system-management unit's
 * firmware (PMFW).  This stage checks that it runs and asks its versions,
 * and first drives the fan controller of the thermal block at a static
 * duty (50 % by default), so the fan turns whatever happens next.  It
 * then puts the board's power table together from the video BIOS, hands
 * it to the firmware and asks the firmware to run three of its features:
 * its own critical-temperature stop, the fan control and the thermal
 * control (the full tier, mode F).  When the firmware will not, the
 * features are switched off again and the static duty stays (the static
 * tier, mode S).  In both tiers the thermal block's alerts are sent to the
 * interrupt ring (high at the board's shutdown temperature), and a thread
 * reads the temperatures and the fan's speed once a second: from the
 * firmware's readings in the full tier, and from the thermal block always,
 * so the two can be compared.  Past the limit (the board's edge limit, or
 * 95 degrees Celsius without one), on a high alert, on the
 * critical-temperature trip, or when the firmware stops answering, the
 * driver faults the GPU: it marks the fault, drives the fan at full duty
 * itself and fails the device, so the GPU takes no more
 * work; the system keeps running.  The firmware's own trip protects the
 * GPU at the hardware limit.
 *
 * The stage also checks that the interrupt ring delivers: it raises the
 * low alert's threshold above the temperature for a moment, which makes
 * the thermal block send one low alert.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <kern/waitq.h>
#include <uapi/amdrdna.h>
#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-private.h"
#include "drivers/gpu/amdrdna/amdrdna-regs.h"

/*
 * The messages the stage sends: the unit's firmware version, the
 * driver-interface version it implements, the features it may run (low
 * and high words), all of them on and off, some of them on (low and
 * high), the features that run (low and high), where the table page is
 * (high and low words), a table copied to the page and from it, the boot
 * calibration, how many displays there are, and the preparation for its
 * unload.  Then the interface version this driver was written against, and
 * the mailbox's answers (OK, failed, unknown message, a prerequisite
 * missing, busy).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu_v11_0_ppsmc.h
 * (PPSMC_MSG_GetSmuVersion, PPSMC_MSG_GetDriverIfVersion,
 * PPSMC_MSG_SetAllowedFeaturesMaskLow, PPSMC_MSG_SetAllowedFeaturesMaskHigh,
 * PPSMC_MSG_EnableAllSmuFeatures, PPSMC_MSG_DisableAllSmuFeatures,
 * PPSMC_MSG_EnableSmuFeaturesLow, PPSMC_MSG_EnableSmuFeaturesHigh,
 * PPSMC_MSG_GetEnabledSmuFeaturesLow, PPSMC_MSG_GetEnabledSmuFeaturesHigh,
 * PPSMC_MSG_SetDriverDramAddrHigh, PPSMC_MSG_SetDriverDramAddrLow,
 * PPSMC_MSG_TransferTableSmu2Dram, PPSMC_MSG_TransferTableDram2Smu,
 * PPSMC_MSG_RunBtc, PPSMC_MSG_NumOfDisplays,
 * PPSMC_MSG_PrepareMp1ForUnload, PPSMC_Result_OK, PPSMC_Result_Failed,
 * PPSMC_Result_UnknownCmd, PPSMC_Result_CmdRejectedPrereq,
 * PPSMC_Result_CmdRejectedBusy) and
 * drivers/gpu/drm/amd/pm/swsmu/inc/smu_v11_0.h
 * (SMU11_DRIVER_IF_VERSION_NV10).
 */
#define SMU_MESSAGE_VERSION		0x02U
#define SMU_MESSAGE_INTERFACE		0x03U
#define SMU_MESSAGE_PERMIT_LOW		0x04U
#define SMU_MESSAGE_PERMIT_HIGH		0x05U
#define SMU_MESSAGE_ALL_ON		0x06U
#define SMU_MESSAGE_ALL_OFF		0x07U
#define SMU_MESSAGE_SOME_ON_LOW		0x08U
#define SMU_MESSAGE_SOME_ON_HIGH	0x09U
#define SMU_MESSAGE_RUNNING_LOW		0x0cU
#define SMU_MESSAGE_RUNNING_HIGH	0x0dU
#define SMU_MESSAGE_PAGE_HIGH		0x0eU
#define SMU_MESSAGE_PAGE_LOW		0x0fU
#define SMU_MESSAGE_TABLE_TO_PAGE	0x12U
#define SMU_MESSAGE_TABLE_FROM_PAGE	0x13U
#define SMU_MESSAGE_CALIBRATE		0x3aU
#define SMU_MESSAGE_DISPLAYS		0x40U
#define SMU_MESSAGE_UNLOAD		0x32U
#define SMU_INTERFACE_EXPECTED		0x37U
#define SMU_ANSWER_OK			0x01U
#define SMU_ANSWER_FAILED		0xffU
#define SMU_ANSWER_UNKNOWN		0xfeU
#define SMU_ANSWER_PREREQUISITE		0xfdU
#define SMU_ANSWER_BUSY			0xfcU

/*
 * The tables the page carries, by the ID a copy message names in its low
 * 16 bits: the board's power table, and the firmware's readings.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu11_driver_if_navi10.h
 * (TABLE_PPTABLE, TABLE_SMU_METRICS) and
 * drivers/gpu/drm/amd/pm/swsmu/smu_cmn.c (smu_cmn_update_table: the
 * argument is the table's ID with a parameter in the high 16 bits).
 */
#define SMU_TABLE_POWER			0U
#define SMU_TABLE_READINGS		6U

/*
 * The messages that move the graphics clock once its levels run: the
 * frequency of one of a clock's levels (the index 0xff asks how many
 * levels it has), its floor and its ceiling (in MHz in the low 16 bits,
 * the clock in the high 16 bits; floor 0 and ceiling 0xffff leave the
 * firmware free), and the workloads the firmware tunes its levels for (a
 * mask of the default and the 3D workloads).  Bit 31 of a level's answer
 * is not part of the frequency.  Then the table of the averaging settings,
 * and the graphics clock's number.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu_v11_0_ppsmc.h
 * (PPSMC_MSG_GetDpmFreqByIndex, PPSMC_MSG_SetSoftMinByFreq,
 * PPSMC_MSG_SetSoftMaxByFreq, PPSMC_MSG_SetHardMinByFreq,
 * PPSMC_MSG_SetHardMaxByFreq, PPSMC_MSG_SetWorkloadMask),
 * drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu11_driver_if_navi10.h
 * (WORKLOAD_PPLIB_DEFAULT_BIT, WORKLOAD_PPLIB_FULL_SCREEN_3D_BIT,
 * TABLE_DRIVER_SMU_CONFIG, PPCLK_GFXCLK, PPCLK_UCLK) and
 * drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c
 * (smu_v11_0_get_dpm_level_count, smu_v11_0_get_dpm_freq_by_index,
 * smu_v11_0_set_soft_freq_limited_range,
 * smu_v11_0_set_hard_freq_limited_range: the arguments and the answer).
 */
#define SMU_MESSAGE_FLOOR		0x1aU
#define SMU_MESSAGE_CEILING		0x1bU
#define SMU_MESSAGE_HARD_FLOOR		0x1cU
#define SMU_MESSAGE_HARD_CEILING	0x1dU
#define SMU_MESSAGE_LEVEL		0x20U
#define SMU_MESSAGE_WORKLOADS		0x24U
#define SMU_LEVEL_COUNT			0xffU
#define SMU_LEVEL_FREQUENCY_MASK	0x7fffffffU
#define SMU_FLOOR_FREE			0x0000U
#define SMU_CEILING_FREE		0xffffU
#define SMU_WORKLOAD_DEFAULT		0x01U
#define SMU_WORKLOAD_3D			0x02U
#define SMU_TABLE_SETTINGS		7U
#define SMU_CLOCK_GFX			0U
#define SMU_CLOCK_MEMORY		2U

/*
 * The question to the firmware about the memory controller's firmware
 * (its answer: bit 0, the controller's firmware is later than version
 * 136; bit 1, its hybrid clock-and-data recovery is off), which decides
 * the workaround the memory clock's levels need on this GPU.
 *
 * Values referenced: Linux v6.12
 * drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu_v11_0_ppsmc.h
 * (PPSMC_MSG_GetUMCFWWA) and
 * drivers/gpu/drm/amd/pm/swsmu/smu11/navi10_ppt.c
 * (navi10_run_umc_cdr_workaround: the answer's bits).
 */
#define SMU_MESSAGE_MEMORY_WORKAROUND	0x50U

/* The samples of the hub's memory state handshake a hang's mark takes, and the time between them. */
#define SMU_HANG_HANDSHAKES		4U
#define SMU_HANG_HANDSHAKE_US		5000U

/*
 * The table page: one page of VRAM, as large as the largest table and
 * aligned to it.
 */
#define SMU_TABLE_PAGE_BYTES		4096U

/* The largest firmware file of the unit the stage reads for a power table of its own. */
#define SMU_FILE_LIMIT			(1024U * 1024U)

/*
 * The waits: an answer polled each millisecond for 2 s (Linux polls each
 * microsecond for 20 times its 100 ms timeout), the firmware's running bit
 * for 1 s, the fan's first turns for 5 s, the low alert of the interrupt
 * check for 500 ms, and the watch's period of 1 s.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu_cmn.c (__smu_cmn_poll_stat).
 */
#define SMU_ANSWER_POLLS		2000U
#define SMU_POLL_US			1000U
#define SMU_FIRMWARE_POLLS		1000U
#define THERMAL_SPIN_POLLS		50U
#define THERMAL_SPIN_POLL_US		100000U
#define THERMAL_CHECK_POLLS		50U
#define THERMAL_CHECK_POLL_US		10000U
#define THERMAL_WATCH_TICKS		kern_ms_to_ticks(1000U)

/*
 * The defaults: the static duty (50 %), the temperature limit (95 degrees
 * Celsius), how far above the temperature the interrupt check puts the
 * low threshold (20 degrees), and the high alert when the video BIOS
 * gives no shutdown temperature (10 degrees above the limit).
 */
#define THERMAL_DEFAULT_DUTY		50U
#define THERMAL_DEFAULT_LIMIT		95U
#define THERMAL_CHECK_ABOVE		20U
#define THERMAL_HIGH_ABOVE_LIMIT	10U

/*
 * The watch's reading that is marked in the log with both sources side
 * by side (about 10 s after the stage), for the comparison of the
 * firmware's temperatures with the thermal block's.
 */
#define THERMAL_COMPARE_READING		10U

/*
 * How the driver raises the graphics clock for work (amdrdna.smu.boost=):
 * not at all (0), with the firmware's 3D workload profile while the GPU
 * works (1, the default), or with the profile and the clock's floor at
 * its highest level (2).  Linux switches to the 3D profile while a
 * graphics ring is in use; the floor is the driver's own, for the short
 * bursts of a desktop.  The firmware raises the clock to its highest
 * level under a steady load by itself, and the floor made a measured
 * draw no faster there, so it is not the default.
 */
#define THERMAL_BOOST_NONE		0U
#define THERMAL_BOOST_PROFILE		1U
#define THERMAL_BOOST_FLOOR		2U

/*
 * The boost's times: it ends a second after the last run of commands, and
 * the watch reads the firmware a tenth of a second after it raised the
 * clock, so the raised clock shows in the readings.
 *
 * Value referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_gfx.h (GFX_PROFILE_IDLE_TIMEOUT).
 */
#define THERMAL_BOOST_IDLE_TICKS	kern_ms_to_ticks(1000U)
#define THERMAL_BOOST_READ_TICKS	kern_ms_to_ticks(100U)

/* How long the stop waits for the watch thread, and its step. */
#define THERMAL_STOP_WAIT_US		2000000U
#define THERMAL_STOP_STEP_US		10000U

static int power_firmware_running(struct amdrdna_device *device);
static int power_versions(struct amdrdna_device *device);
static int power_fan(struct amdrdna_device *device);
static int power_fan_duty(struct amdrdna_device *device, unsigned percent);
static int power_table_prepare(struct amdrdna_device *device, uint8_t *table);
static int power_table_from_firmware(struct amdrdna_device *device, const uint8_t *board, size_t board_bytes, uint8_t *table);
static int power_features(struct amdrdna_device *device, const uint8_t *table);
static int power_features_ask(struct amdrdna_device *device);
static int power_features_running(struct amdrdna_device *device, uint32_t *low, uint32_t *high);
static void power_features_off(struct amdrdna_device *device);
static int power_table_send(struct amdrdna_device *device, uint32_t id, const uint8_t *table, size_t bytes);
static void power_clock_setup(struct amdrdna_device *device, unsigned wanted);
static int power_clock_levels(struct amdrdna_device *device, uint32_t clock, uint32_t *count, uint32_t *lowest, uint32_t *highest);
static void power_memory_clock(struct amdrdna_device *device);
static int power_boost(struct amdrdna_device *device, bool raising);
static void power_boost_end(struct amdrdna_device *device, unsigned mode, bool raising, int error);
static int power_readings_fetch(struct amdrdna_device *device, struct amdrdna_power_readings *readings);
static void power_alerts(struct amdrdna_device *device);
static bool power_interrupt_check(struct amdrdna_device *device);
static void power_spin_up(struct amdrdna_device *device);
static void power_read(struct amdrdna_device *device);
static void power_fault(struct amdrdna_device *device, const char *why);
static int power_watch_start(struct amdrdna_device *device);
static void power_watch(void *argument);
static int smu_message(struct amdrdna_device *device, uint32_t message, uint32_t argument, uint32_t *answer);
static void smu_hang_mark(struct amdrdna_device *device, uint32_t message);
static int smu_message_held(struct amdrdna_device *device, uint32_t message, uint32_t argument, uint32_t *answer);
static uint32_t smu_wait_answer(struct amdrdna_device *device);
static uint32_t thermal_read(struct amdrdna_device *device, uint32_t offset);
static void thermal_write(struct amdrdna_device *device, uint32_t offset, uint32_t value);

/*
 * Runs stage A4: checks that the unit's firmware runs and asks its
 * versions, drives the fan at the static duty, then puts the board's power
 * table together and asks the firmware to run its fan and thermal
 * control and move the graphics clock (amdrdna.smu.features=0 leaves
 * that out; amdrdna.smu.gfxclk=0 keeps the graphics clock at the video
 * BIOS's boot clock), falling back to the static duty
 * when it will not; when the graphics clock moves, hands the firmware
 * its averaging settings and workload, reads the clock's levels and sets
 * up how the clock is raised for work (amdrdna.smu.boost=); sends the
 * thermal alerts to the interrupt ring and checks that one arrives, reads
 * the temperatures and the fan, and starts the watch.
 *
 * Returns 0, ENODEV when the block table lacks the unit or the thermal
 * block, ETIMEDOUT when the unit's firmware does not run or does not
 * answer its versions, EINVAL when the fan controller cannot be driven, or
 * ENOMEM.  A firmware that will not run its features is not a failure:
 * the stage ends in the static tier.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/amdgpu_smu.c (smu_hw_init,
 * smu_smc_hw_setup: the order of the steps).
 */
int
amdrdna_a4_power(
	struct amdrdna_device *device)
{
	struct amdrdna_thermal *thermal;
	uint8_t *table;
	const char *path;
	const char *tier;
	uint32_t index;
	unsigned wanted;
	unsigned raise_mode;
	bool delivered;
	int error;

	/* Says the stage starts. */
	amdrdna_mark("A4 begin");
	thermal = &device->thermal;

	/* The unit's mailbox and the thermal block must both be listed. */
	error = amdrdna_block_index(device, AMDRDNA_BLOCK_SMU, AMDRDNA_SMU_WINDOW_MAILBOX, AMDRDNA_SMU_REG_MESSAGE, &index);
	if (error == 0)
		error = amdrdna_block_index(device, AMDRDNA_BLOCK_THM, AMDRDNA_THM_WINDOW_MAIN, AMDRDNA_THM_REG_TEMPERATURE, &index);
	if (error != 0) {
		amdrdna_mark("A4 fail: the block table lists no smu or no thermal block");
		return ENODEV;
	}

	/* The unit's firmware must run. */
	error = power_firmware_running(device);
	if (error != 0)
		return error;

	/* Asks its versions. */
	error = power_versions(device);
	if (error != 0)
		return error;

	/* Drives the fan at the static duty, so it turns whatever the firmware does next. */
	error = power_fan(device);
	if (error != 0)
		return error;

	/* Holds the board's power table while it is put together and handed over. */
	table = kern_malloc(AMDRDNA_SMU_POWER_TABLE_BYTES);
	if (table == NULL) {
		amdrdna_mark("A4 fail: no memory for the power table");
		return ENOMEM;
	}

	/* Puts the table together; its limits serve the static tier too. */
	error = power_table_prepare(device, table);

	/*
	 * The firmware moves the graphics clock too, unless the boot line
	 * keeps it at the video BIOS's boot clock (amdrdna.smu.gfxclk=0).
	 * clock_levels tells the features to permit its levels.
	 */
	thermal->clock_levels = false;
	if (amdrdna_parameter_number("amdrdna.smu.gfxclk", 1U) != 0U)
		thermal->clock_levels = true;

	/*
	 * The memory side's levels with them only when the boot line asks:
	 * held at the highest (amdrdna.smu.memory=1), or moved by the firmware
	 * with the load once the display's step has run (2).  Held at the
	 * highest without the display's step the firmware stopped answering at
	 * idle after loads (twice within 20 minutes of load and idle in turn,
	 * none in 40 minutes without them), so they stay off by default.
	 */
	thermal->memory_mode = amdrdna_parameter_number("amdrdna.smu.memory", 0U);
	if (thermal->memory_mode > 2U)
		thermal->memory_mode = 2U;
	thermal->memory_levels = false;
	if (thermal->memory_mode != 0U)
		thermal->memory_levels = true;
	thermal->memory_level_count = 0U;
	thermal->memory_display = false;
	thermal->memory_follows = false;
	thermal->memory_underflows = 0U;
	thermal->memory_fifo_errors = 0U;
	thermal->display_watch = amdrdna_parameter_number("amdrdna.smu.display_watch", 0U);
	thermal->memory_plan_set = false;

	/* How the clock is raised for work once it moves; nothing is raised until then. */
	raise_mode = amdrdna_parameter_number("amdrdna.smu.boost", THERMAL_BOOST_PROFILE);
	thermal->boost_mode = THERMAL_BOOST_NONE;

	/* Asks the firmware for its features when the table is ready and the boot line allows it. */
	wanted = amdrdna_parameter_number("amdrdna.smu.features", 1U);
	if (error != 0) {
		/* The firmware is not asked without the board's table. */
		amdrdna_mark("A4 note: no power table (%d), the fan stays at its static duty", error);
	} else if (wanted == 0U) {
		/* The boot line keeps the static tier. */
		amdrdna_mark("A4 note: amdrdna.smu.features=0, the fan stays at its static duty");
	} else {
		/* Asks; on a refusal the features go off and the static duty comes back. */
		error = power_features(device, table);
		if (error != 0) {
			power_features_off(device);
			(void)power_fan(device);
		} else if ((thermal->features_low & AMDRDNA_SMU_FEATURE_GFX_LEVELS) != 0U) {
			/* The graphics clock moves: sets up its averages, workload, levels and boost. */
			power_clock_setup(device, raise_mode);
		}
	}

	/* The table has been handed over or given up. */
	kern_free(table);

	/* A firmware that stopped answering cannot protect the GPU. */
	if (thermal->hung)
		power_fault(device, "the smu firmware stopped answering");

	/* Sends the alerts to the interrupt ring, and checks that one arrives. */
	power_alerts(device);
	delivered = power_interrupt_check(device);

	/* Waits for the static duty to turn the fan; the firmware may keep it still while cool. */
	if (!thermal->features_on)
		power_spin_up(device);

	/* Reads the temperatures and the fan. */
	power_read(device);

	/* Marks the clocks the firmware reports, when it gave readings. */
	if (thermal->clocked) {
		amdrdna_mark("A4 clocks gfx=%uMHz average=%uMHz soc=%uMHz mem=%uMHz dcef=%uMHz disp=%uMHz busy=%u%% levels=%s",
		    thermal->gfx_clock,
		    thermal->gfx_average,
		    thermal->system_clock,
		    thermal->memory_clock,
		    thermal->return_clock,
		    thermal->display_engine_clock,
		    thermal->gfx_activity,
		    thermal->clock_levels ? "asked" : "off");
	}

	/* Starts the watch. */
	error = power_watch_start(device);
	if (error != 0) {
		amdrdna_mark("A4 fail: the watch thread not started (%d)", error);
		return error;
	}

	/* Names whether the interrupt check saw its alert. */
	path = "none";
	if (delivered)
		path = "thm";

	/* Names the tier: F when the firmware runs the fan and the thermal control. */
	tier = "S";
	if (thermal->features_on)
		tier = "F";

	/* Marks the result, and the readings on a line of their own (one mark line holds 160 columns). */
	amdrdna_mark("A4 ok if=0x%x smu=%u.%u.%u mode=%s features=0x%08x%08x limit=%uC alert=%uC irq=%s",
	    (unsigned)thermal->interface_version,
	    (unsigned)((thermal->smu_version >> 16) & 0xffU),
	    (unsigned)((thermal->smu_version >> 8) & 0xffU),
	    (unsigned)(thermal->smu_version & 0xffU),
	    tier,
	    (unsigned)thermal->features_high,
	    (unsigned)thermal->features_low,
	    thermal->limit,
	    thermal->high_alert,
	    path);
	amdrdna_mark("A4 readings duty=%u%% fan=%urpm tach=%urpm driven=%u%% temp=%uC hotspot=%uC mem=%uC thm=%uC max=%uC",
	    thermal->duty_percent,
	    (unsigned)thermal->fan_speed,
	    (unsigned)thermal->tachometer_speed,
	    thermal->fan_driven,
	    thermal->temperature,
	    thermal->hotspot,
	    thermal->memory_temperature,
	    thermal->sensor,
	    thermal->highest);

	/* Succeeded: the fan turns under the firmware or at its duty, and the driver watches the temperature. */
	return 0;
}

/*
 * Undoes stage A4 for the shutdown: stops the watch thread, masks the
 * thermal alerts and the unit's software interrupt, switches the
 * firmware's features off when they run (the fan goes back to the static
 * duty), and tells the unit's firmware it is about to be unloaded.  The
 * fan keeps its static duty; the next post of the video BIOS sets it
 * again.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c (smu_v11_0_set_irq_state,
 * the disable case), drivers/gpu/drm/amd/pm/swsmu/amdgpu_smu.c
 * (smu_disable_dpms: all features off) and navi10_ppt.c
 * (PrepareMp1ForUnload).
 */
void
amdrdna_power_stop(
	struct amdrdna_device *device)
{
	struct amdrdna_thermal *thermal;
	unsigned long irq;
	unsigned waited;
	uint32_t word;
	bool running;
	int error;

	/* Nothing to undo before the stage ran. */
	thermal = &device->thermal;
	if (thermal->mode == AMDRDNA_FAN_NONE)
		return;

	/* Tells the watch thread to stop. */
	irq = spin_lock_irqsave(&thermal->lock);

	thermal->stopping = true;
	waitq_wake_all(&thermal->waitq);

	spin_unlock_irqrestore(&thermal->lock, irq);

	/* Waits for it to end, for a bounded time. */
	for (waited = 0U; waited < THERMAL_STOP_WAIT_US; waited += THERMAL_STOP_STEP_US) {
		/* Reads whether the thread still runs. */
		irq = spin_lock_irqsave(&thermal->lock);

		running = thermal->thread_running;

		spin_unlock_irqrestore(&thermal->lock, irq);

		/* Stops waiting once it has ended. */
		if (!running)
			break;

		/* Gives the thread a step to finish. */
		kern_usleep_range(THERMAL_STOP_STEP_US, THERMAL_STOP_STEP_US * 2U);
	}

	/* Masks the thermal alerts and the unit's software interrupt. */
	if (thermal->alerts_on) {
		word = thermal_read(device, AMDRDNA_THM_REG_ALERT_CONTROL);
		thermal_write(device, AMDRDNA_THM_REG_ALERT_CONTROL, amdrdna_alert_off_word(word));
		thermal_write(device, AMDRDNA_THM_REG_ALERT_ENABLE, 0U);
		word = amdrdna_block_read(device, AMDRDNA_BLOCK_SMU, AMDRDNA_SMU_WINDOW_MAILBOX, AMDRDNA_SMU_REG_SOFT_INTERRUPT_CONTROL);
		word |= AMDRDNA_SMU_SOFT_INTERRUPT_MASKED;
		amdrdna_block_write(device, AMDRDNA_BLOCK_SMU, AMDRDNA_SMU_WINDOW_MAILBOX, AMDRDNA_SMU_REG_SOFT_INTERRUPT_CONTROL, word);
		thermal->alerts_on = false;
	}

	/* Switches the firmware's features off, and drives the fan at the static duty until the next post. */
	if (thermal->features_on) {
		power_features_off(device);
		(void)power_fan(device);
	}

	/* Tells the unit's firmware it is about to be unloaded. */
	error = smu_message(device, SMU_MESSAGE_UNLOAD, 0U, NULL);

	/* Marks the undo, for the log, with the unload's answer. */
	amdrdna_mark("shutdown: A4 undone, watch stopped, alerts masked, smu unload answered %d", error);
}

/*
 * Makes stage A4 ready to run again after a reset of the GPU, which
 * restarts the unit's firmware: the first message waits for no earlier
 * answer, a unit marked hung is asked again, the features and alerts are
 * off, and the watch may be started again.  The table page and the
 * limits stay; a thermal fault stays recorded.
 *
 * Returns 0, or EBUSY when the watch thread of the stop has not ended.
 */
int
amdrdna_power_rearm(
	struct amdrdna_device *device)
{
	struct amdrdna_thermal *thermal;
	unsigned long irq;
	bool running;

	/* The stop's watch must have ended, or two would run. */
	thermal = &device->thermal;
	irq = spin_lock_irqsave(&thermal->lock);

	running = thermal->thread_running;
	if (!running)
		thermal->stopping = false;

	spin_unlock_irqrestore(&thermal->lock, irq);

	/* Reports a watch still running. */
	if (running)
		return EBUSY;

	/* The mailbox and the features as before the stage's first run. */
	mutex_lock(&thermal->mailbox);

	thermal->first_sent = false;
	thermal->hung = false;

	mutex_unlock(&thermal->mailbox);

	thermal->features_on = false;
	thermal->alerts_on = false;

	/* Succeeded: the stage may run again. */
	return 0;
}

/*
 * Handles an interrupt of the thermal side: a high alert (the temperature
 * rose past the high threshold) faults the GPU, a low alert is counted
 * (the interrupt check raises one), the critical-temperature trip faults
 * the GPU, and the unit's software interrupt is acknowledged and counted.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c (smu_v11_0_irq_process;
 * the driver faults the GPU instead of shutting the system down).
 */
void
amdrdna_thermal_interrupt(
	struct amdrdna_device *device,
	const struct amdrdna_interrupt *interrupt)
{
	struct amdrdna_thermal *thermal;
	unsigned long irq;
	uint32_t word;

	/* The power side's state. */
	thermal = &device->thermal;

	/* Each source of the three clients. */
	if (interrupt->client == AMDRDNA_CLIENT_THERMAL && interrupt->source == AMDRDNA_SOURCE_THERMAL_HIGH) {
		/* Counts the high alert. */
		irq = spin_lock_irqsave(&thermal->lock);

		thermal->high_alerts++;

		spin_unlock_irqrestore(&thermal->lock, irq);

		/* Faults the GPU. */
		power_fault(device, "the thermal block's high alert");
	} else if (interrupt->client == AMDRDNA_CLIENT_THERMAL && interrupt->source == AMDRDNA_SOURCE_THERMAL_LOW) {
		/* Counts the low alert. */
		irq = spin_lock_irqsave(&thermal->lock);

		thermal->low_alerts++;

		spin_unlock_irqrestore(&thermal->lock, irq);
	} else if (interrupt->client == AMDRDNA_CLIENT_SMU_IO && interrupt->source == AMDRDNA_SOURCE_CRITICAL) {
		/* Counts the trip. */
		irq = spin_lock_irqsave(&thermal->lock);

		thermal->critical_alerts++;

		spin_unlock_irqrestore(&thermal->lock, irq);

		/* Faults the GPU. */
		power_fault(device, "the critical-temperature trip");
	} else if (interrupt->client == AMDRDNA_CLIENT_SMU && interrupt->source == AMDRDNA_SOURCE_SMU_SOFT) {
		/* Acknowledges the software interrupt so the unit can raise the next. */
		word = amdrdna_block_read(device, AMDRDNA_BLOCK_SMU, AMDRDNA_SMU_WINDOW_MAILBOX, AMDRDNA_SMU_REG_SOFT_INTERRUPT_CONTROL);
		word |= AMDRDNA_SMU_SOFT_INTERRUPT_ACKNOWLEDGE;
		amdrdna_block_write(device, AMDRDNA_BLOCK_SMU, AMDRDNA_SMU_WINDOW_MAILBOX, AMDRDNA_SMU_REG_SOFT_INTERRUPT_CONTROL, word);

		/* Counts it. */
		irq = spin_lock_irqsave(&thermal->lock);

		thermal->smu_interrupts++;

		spin_unlock_irqrestore(&thermal->lock, irq);

		/* Names its context in the log. */
		amdrdna_mark("smu: software interrupt context 0x%x", (unsigned)interrupt->data[0]);
	} else {
		/* A source of these clients the stage does not know. */
		amdrdna_mark("thermal: unhandled client=0x%x source=0x%x", (unsigned)interrupt->client, (unsigned)interrupt->source);
	}
}

/*
 * Gives the clocks and the graphics engine's activity the watch last read
 * from the firmware (the clock fields of report->now; the others are left
 * as they are), the highest graphics clock and activity it read since the
 * previous call, which start over, and the boost's count and state.
 * Returns false when the firmware gave no readings: its features do not
 * run, or the watch has not read yet.
 */
bool
amdrdna_power_clocks(
	struct amdrdna_device *device,
	struct amdrdna_power_clock_report *report)
{
	struct amdrdna_thermal *thermal;
	unsigned long irq;
	bool clocked;

	/* What the watch published, under its lock. */
	thermal = &device->thermal;
	irq = spin_lock_irqsave(&thermal->lock);

	clocked = thermal->clocked;
	report->now.gfx_clock = thermal->gfx_clock;
	report->now.system_clock = thermal->system_clock;
	report->now.memory_clock = thermal->memory_clock;
	report->now.return_clock = thermal->return_clock;
	report->now.display_engine_clock = thermal->display_engine_clock;
	report->now.gfx_average = thermal->gfx_average;
	report->now.gfx_activity = thermal->gfx_activity;
	report->gfx_clock_top = thermal->gfx_clock_top;
	report->gfx_activity_top = thermal->gfx_activity_top;
	report->boosts = thermal->boosts;
	report->boosted = thermal->boosted;

	/* The highest readings start over for the next report. */
	thermal->gfx_clock_top = 0U;
	thermal->gfx_activity_top = 0U;

	spin_unlock_irqrestore(&thermal->lock, irq);

	/* Succeeded when they were read. */
	return clocked;
}

/*
 * Notes that the GPU runs commands now: the boost (when the graphics
 * clock is raised for work at all) ends a second after the last such
 * note, and a note while the clock is not raised has the watch raise it.
 * Called before each run of commands; it only takes the power side's lock.
 */
void
amdrdna_power_busy(
	struct amdrdna_device *device)
{
	struct amdrdna_thermal *thermal;
	unsigned long irq;

	/* The power side's state. */
	thermal = &device->thermal;

	/* Notes the time, and asks the watch for the boost once when the clock is not raised. */
	irq = spin_lock_irqsave(&thermal->lock);

	thermal->busy_ticks = sched_ticks();
	if (thermal->boost_mode != THERMAL_BOOST_NONE &&
	    !thermal->boosted &&
	    !thermal->boost_wanted) {
		/*
		 * boost_wanted tells the watch to raise the clock; it stays set
		 * until the watch has taken it, so the runs before that do not
		 * wake the watch again.
		 */
		thermal->boost_wanted = true;
		waitq_wake_all(&thermal->waitq);
	}

	spin_unlock_irqrestore(&thermal->lock, irq);
}

/*
 * Sends one message to the unit's firmware for another part of the driver
 * (the display's step) and waits for its answer; answer, when not NULL,
 * gets the output argument.
 *
 * Returns what smu_message returns.
 */
int
amdrdna_smu_message(
	struct amdrdna_device *device,
	uint32_t message,
	uint32_t argument,
	uint32_t *answer)
{
	int error;

	/* The message, under the mailbox. */
	error = smu_message(device, message, argument, answer);
	if (error != 0)
		return error;

	/* Succeeded: the unit carried the message out. */
	return 0;
}

/*
 * Hands a table (bytes, a multiple of 4, at most a page) to the unit's
 * firmware by its ID through the table page, for another part of the
 * driver.  Returns what power_table_send returns, or EINVAL for a table
 * larger than the page.
 */
int
amdrdna_smu_table_send(
	struct amdrdna_device *device,
	uint32_t id,
	const uint8_t *table,
	size_t bytes)
{
	int error;

	/* The table must fit the page, in whole words. */
	if (bytes > SMU_TABLE_PAGE_BYTES || (bytes % 4U) != 0U)
		return EINVAL;

	/* The page must have been taken by stage A4. */
	if (!device->thermal.table_taken)
		return ENXIO;

	/* The table, through the page. */
	error = power_table_send(device, id, table, bytes);
	if (error != 0)
		return error;

	/* Succeeded: the firmware has the table. */
	return 0;
}

/*
 * Reads the frequency of one level of a clock (the firmware's clock
 * number), in MHz; the level 0xff asks how many levels the clock has.
 * Returns 0 or the message's error.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c
 * (smu_v11_0_get_dpm_freq_by_index).
 */
int
amdrdna_smu_clock_level(
	struct amdrdna_device *device,
	uint32_t clock,
	uint32_t level,
	uint32_t *mhz)
{
	uint32_t answer;
	int error;

	/* The level's answer, without its top bit. */
	answer = 0U;
	error = smu_message(device, SMU_MESSAGE_LEVEL, (clock << 16) | (level & SMU_LEVEL_COUNT), &answer);
	if (error != 0)
		return error;
	*mhz = answer & SMU_LEVEL_FREQUENCY_MASK;

	/* Succeeded: the frequency (or the count) is read. */
	return 0;
}

/*
 * Waits for the unit's firmware to say it runs: bit 0 of its state word,
 * read through the indirect register window.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c
 * (smu_v11_0_check_fw_status).
 */
static int
power_firmware_running(
	struct amdrdna_device *device)
{
	uint32_t word;
	unsigned poll;

	/* Looks each millisecond until the bit is set. */
	word = 0U;
	for (poll = 0U; poll < SMU_FIRMWARE_POLLS; poll++) {
		/* Stops once the firmware runs. */
		word = amdrdna_far_read(device, AMDRDNA_SMU_FAR_FIRMWARE_STATE);
		if ((word & AMDRDNA_SMU_FIRMWARE_RUNNING) != 0U)
			break;

		/* Sleeps one step. */
		kern_usleep_range(SMU_POLL_US, SMU_POLL_US + SMU_POLL_US / 4U);
	}

	/* Reports a firmware that does not run. */
	if ((word & AMDRDNA_SMU_FIRMWARE_RUNNING) == 0U) {
		amdrdna_mark("A4 fail: the smu firmware does not run (state 0x%08x)", (unsigned)word);
		return ETIMEDOUT;
	}

	/* Records the state word. */
	device->smu_state = word;

	/* Succeeded: the firmware runs. */
	return 0;
}

/*
 * Asks the unit's firmware for the driver-interface version it implements
 * and its own version.  A different interface version is only logged:
 * the firmware keeps the older interfaces.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c
 * (smu_v11_0_check_fw_version) and smu_cmn.c (smu_cmn_get_smc_version).
 */
static int
power_versions(
	struct amdrdna_device *device)
{
	struct amdrdna_thermal *thermal;
	int error;

	/* The power side's state. */
	thermal = &device->thermal;

	/* The interface version. */
	error = smu_message(device, SMU_MESSAGE_INTERFACE, 0U, &thermal->interface_version);
	if (error != 0) {
		amdrdna_mark("A4 fail: the smu does not answer the interface version (%d)", error);
		return error;
	}

	/* The firmware version. */
	error = smu_message(device, SMU_MESSAGE_VERSION, 0U, &thermal->smu_version);
	if (error != 0) {
		amdrdna_mark("A4 fail: the smu does not answer its version (%d)", error);
		return error;
	}

	/* Logs an interface version other than the one the driver was written against. */
	if (thermal->interface_version != SMU_INTERFACE_EXPECTED) {
		amdrdna_mark("A4 note: smu interface 0x%x, the driver expects 0x%x",
		    (unsigned)thermal->interface_version,
		    (unsigned)SMU_INTERFACE_EXPECTED);
	}

	/* Succeeded: both versions are read. */
	return 0;
}

/*
 * Drives the fan at the static duty (amdrdna.fan.duty=, 50 % by default):
 * the duty word first, then the mode word that makes the controller use
 * it.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c
 * (smu_v11_0_set_fan_speed_pwm, smu_v11_0_set_fan_static_mode).
 */
static int
power_fan(
	struct amdrdna_device *device)
{
	unsigned percent;
	int error;

	/* The duty asked for, at most 100 %. */
	percent = amdrdna_parameter_number("amdrdna.fan.duty", THERMAL_DEFAULT_DUTY);
	if (percent > 100U)
		percent = 100U;

	/* The controller at that duty. */
	error = power_fan_duty(device, percent);
	if (error != 0)
		return error;

	/* Succeeded: the controller drives the static duty. */
	return 0;
}

/*
 * Drives the fan at percent of the controller's full duty, the
 * driver's from then on (the firmware's fan control left out): the duty
 * word first, then the mode word that makes the controller use it.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c
 * (smu_v11_0_set_fan_speed_pwm, smu_v11_0_set_fan_static_mode).
 */
static int
power_fan_duty(
	struct amdrdna_device *device,
	unsigned percent)
{
	struct amdrdna_thermal *thermal;
	uint32_t limits;
	uint32_t old;
	uint32_t word;
	int error;

	/* The power side's state. */
	thermal = &device->thermal;

	/* The duty word for that percentage of the controller's full duty. */
	limits = thermal_read(device, AMDRDNA_THM_REG_FAN_LIMITS);
	old = thermal_read(device, AMDRDNA_THM_REG_FAN_DUTY);
	error = amdrdna_fan_duty_word(old, limits, percent, &word);
	if (error != 0) {
		amdrdna_mark("A4 fail: the fan controller has no full duty (limits 0x%08x)", (unsigned)limits);
		return EINVAL;
	}

	/* Writes the duty, then the static mode. */
	thermal_write(device, AMDRDNA_THM_REG_FAN_DUTY, word);
	old = thermal_read(device, AMDRDNA_THM_REG_FAN_MODE);
	thermal_write(device, AMDRDNA_THM_REG_FAN_MODE, amdrdna_fan_mode_word(old));

	/* The fan is the driver's from now on. */
	thermal->duty_percent = percent;
	thermal->mode = AMDRDNA_FAN_STATIC;
	amdrdna_mark("A4 fan duty %u%% (limits 0x%08x, duty 0x%08x, mode 0x%08x)",
	    percent,
	    (unsigned)limits,
	    (unsigned)thermal_read(device, AMDRDNA_THM_REG_FAN_DUTY),
	    (unsigned)thermal_read(device, AMDRDNA_THM_REG_FAN_MODE));

	/* Succeeded: the controller drives the static duty. */
	return 0;
}

/*
 * Puts the board's power table together for the unit's firmware: the
 * table the firmware file carries when the video BIOS names one (its power
 * table ID is not 0), otherwise the video BIOS's own; then the video
 * BIOS's board table over its board part.  Records the limits it carries.
 *
 * Returns 0 with the table (AMDRDNA_SMU_POWER_TABLE_BYTES bytes), ENOENT
 * when the video BIOS has no power table, or the error of reading the
 * firmware file or of putting the table together.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c (smu_v11_0_setup_pptable:
 * the firmware file's table when the ID is not 0) and navi10_ppt.c
 * (navi10_setup_pptable).
 */
static int
power_table_prepare(
	struct amdrdna_device *device,
	uint8_t *table)
{
	struct amdrdna_thermal *thermal;
	const struct amdrdna_vbios_info *info;
	const uint8_t *board;
	const uint8_t *power;
	size_t board_bytes;
	size_t power_bytes;
	const char *source;
	int error;

	/* The power side's state and what the video BIOS image says. */
	thermal = &device->thermal;
	info = &device->vbios_info;

	/* Without the image there is no table to start from. */
	if (device->vbios == NULL)
		return ENOENT;

	/* The board table, when the image has one; stage A1v checked that it lies inside the image. */
	board = NULL;
	board_bytes = 0U;
	if (info->board_table != 0U) {
		board = device->vbios + info->board_table;
		board_bytes = amdrdna_le16(device->vbios, info->board_table);
	}

	/* Takes the firmware file's table when the image names one, else the image's own. */
	if (info->power_table_id != 0U) {
		/* The firmware file of the unit carries the table with the ID. */
		source = "firmware";
		error = power_table_from_firmware(device, board, board_bytes, table);
	} else if (info->power_table != 0U) {
		/* The image's power table, which stage A1v checked lies inside the image. */
		source = "vbios";
		power = device->vbios + info->power_table;
		power_bytes = amdrdna_le16(device->vbios, info->power_table);
		error = amdrdna_power_table_build(
			power,
			power_bytes,
			board,
			board_bytes,
			info->board_format,
			info->board_content,
			table,
			&thermal->limits);
	} else {
		/* Neither names a table. */
		return ENOENT;
	}

	/* Reports a table that could not be put together. */
	if (error != 0) {
		amdrdna_mark("A4 note: the power table from the %s not built (%d; board table %u.%u, %uB)",
		    source,
		    error,
		    (unsigned)info->board_format,
		    (unsigned)info->board_content,
		    (unsigned)board_bytes);
		return error;
	}

	/* The limits are valid from now on. */
	thermal->table_built = true;
	amdrdna_mark("A4 power table from the %s id=%u: edge %uC hotspot %uC mem %uC shutdown %uC controller %u features 0x%08x%08x",
	    source,
	    (unsigned)info->power_table_id,
	    thermal->limits.edge,
	    thermal->limits.hotspot,
	    thermal->limits.memory,
	    thermal->limits.shutdown,
	    thermal->limits.controller,
	    (unsigned)thermal->limits.features_high,
	    (unsigned)thermal->limits.features_low);

	/* Succeeded: the table is ready for the firmware. */
	return 0;
}

/*
 * Puts the board's power table together from the one the firmware file
 * of the unit carries under the video BIOS's power table ID.
 *
 * Returns 0 with the table, or the error of reading the file, of finding
 * the ID in it, or of putting the table together.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c
 * (smu_v11_0_set_pptable_v2_1).
 */
static int
power_table_from_firmware(
	struct amdrdna_device *device,
	const uint8_t *board,
	size_t board_bytes,
	uint8_t *table)
{
	const struct amdrdna_vbios_info *info;
	struct amdrdna_blob blob;
	char name[32];
	size_t offset;
	size_t bytes;
	int error;

	/* What the video BIOS image says. */
	info = &device->vbios_info;

	/* Reads the unit's firmware file whole. */
	(void)kern_snprintf(name, sizeof(name), "%s_smc.bin", device->chip);
	error = amdrdna_firmware_read(name, SMU_FILE_LIMIT, &blob);
	if (error != 0) {
		amdrdna_mark("A4 note: firmware %s not read (%d)", name, error);
		return error;
	}

	/* Finds the table with the ID. */
	error = amdrdna_power_table_in_firmware(blob.data, blob.size, info->power_table_id, &offset, &bytes);
	if (error != 0) {
		amdrdna_firmware_free(&blob);
		amdrdna_mark("A4 note: %s carries no power table %u (%d)", name, (unsigned)info->power_table_id, error);
		return error;
	}

	/* Puts the table together from it and the board table. */
	error = amdrdna_power_table_build(
		blob.data + offset,
		bytes,
		board,
		board_bytes,
		info->board_format,
		info->board_content,
		table,
		&device->thermal.limits);
	amdrdna_firmware_free(&blob);

	/* Reports a table that could not be put together. */
	if (error != 0)
		return error;

	/* Succeeded: the table is built from the file's. */
	return 0;
}

/*
 * Asks the unit's firmware to run its fan and thermal control, in the
 * order Linux sets the unit up, without the steps only the clock
 * management needs: no display yet, the table page's place, the board's
 * power table, the boot calibration, the three features permitted, then
 * every permitted feature on and what runs read back.  When the firmware
 * refuses them all at once or leaves one off, the three are asked for one
 * word at a time.
 *
 * Returns 0 with the features running (the fan is then the firmware's),
 * the error of the step that failed, or EIO when the features do not run.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/amdgpu_smu.c (smu_smc_hw_setup),
 * drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c
 * (smu_v11_0_init_display_count, smu_v11_0_set_driver_table_location,
 * smu_v11_0_set_allowed_mask, smu_v11_0_system_features_control),
 * drivers/gpu/drm/amd/pm/swsmu/smu11/navi10_ppt.c (navi10_run_btc) and
 * drivers/gpu/drm/amd/pm/swsmu/smu_cmn.c (smu_cmn_write_pptable,
 * smu_cmn_get_enabled_mask).
 */
static int
power_features(
	struct amdrdna_device *device,
	const uint8_t *table)
{
	struct amdrdna_thermal *thermal;
	uint64_t address;
	uint32_t low;
	uint32_t high;
	uint32_t permitted;
	uint32_t permitted_high;
	const char *how;
	bool on;
	int error;

	/* The power side's state. */
	thermal = &device->thermal;

	/* The three features, and the graphics clock's levels with their limits when the boot line asks. */
	permitted = AMDRDNA_SMU_FEATURES_LOW;
	permitted_high = AMDRDNA_SMU_FEATURES_HIGH;
	if (thermal->clock_levels) {
		permitted |= AMDRDNA_SMU_CLOCK_FEATURES_LOW;
		permitted_high |= AMDRDNA_SMU_CLOCK_FEATURES_HIGH;
		if (!thermal->memory_levels)
			permitted &= ~AMDRDNA_SMU_MEMORY_FEATURES_LOW;
		if (thermal->memory_mode == 2U)
			permitted |= AMDRDNA_SMU_DISPLAY_FEATURES_LOW;
	}

	/* No display is counted yet. */
	error = smu_message(device, SMU_MESSAGE_DISPLAYS, 0U, NULL);
	if (error != 0) {
		amdrdna_mark("A4 features: the display count refused (%d)", error);
		return error;
	}

	/* Takes the table page once, for the record's life. */
	if (!thermal->table_taken) {
		error = amdrdna_vram_take(device, true, SMU_TABLE_PAGE_BYTES, SMU_TABLE_PAGE_BYTES, &thermal->table_vram);
		if (error != 0) {
			amdrdna_mark("A4 features: no VRAM for the table page (%d)", error);
			return error;
		}

		/* The page is the firmware's to read and write from now on. */
		thermal->table_taken = true;
	}

	/* Tells the firmware where the page is, by its GPU address, the high word first. */
	address = device->frame_buffer_base + thermal->table_vram;
	error = smu_message(device, SMU_MESSAGE_PAGE_HIGH, (uint32_t)(address >> 32), NULL);
	if (error == 0)
		error = smu_message(device, SMU_MESSAGE_PAGE_LOW, (uint32_t)address, NULL);
	if (error != 0) {
		amdrdna_mark("A4 features: the table page at 0x%llx refused (%d)", (unsigned long long)address, error);
		return error;
	}

	/* Hands over the board's power table. */
	error = power_table_send(device, SMU_TABLE_POWER, table, AMDRDNA_SMU_POWER_TABLE_BYTES);
	if (error != 0) {
		amdrdna_mark("A4 features: the power table refused (%d)", error);
		return error;
	}

	/* Runs the boot calibration. */
	error = smu_message(device, SMU_MESSAGE_CALIBRATE, 0U, NULL);
	if (error != 0) {
		amdrdna_mark("A4 features: the boot calibration refused (%d)", error);
		return error;
	}

	/* Permits the features, the high word first. */
	error = smu_message(device, SMU_MESSAGE_PERMIT_HIGH, permitted_high, NULL);
	if (error == 0)
		error = smu_message(device, SMU_MESSAGE_PERMIT_LOW, permitted, NULL);
	if (error != 0) {
		amdrdna_mark("A4 features: the permitted features refused (%d)", error);
		return error;
	}

	/* Switches every permitted feature on, and reads back what runs. */
	how = "all";
	low = 0U;
	high = 0U;
	on = false;
	error = smu_message(device, SMU_MESSAGE_ALL_ON, 0U, NULL);
	if (error == 0)
		error = power_features_running(device, &low, &high);
	if (error == 0)
		on = amdrdna_power_features_on(low, high);

	/* Asks for the three one word at a time when that did not do, unless the firmware stopped answering. */
	if (!on && !thermal->hung) {
		amdrdna_mark("A4 features: all on gave 0x%08x%08x (%d), asking one word at a time", (unsigned)high, (unsigned)low, error);
		how = "by word";
		error = power_features_ask(device);
		if (error == 0)
			error = power_features_running(device, &low, &high);
		if (error == 0)
			on = amdrdna_power_features_on(low, high);
	}

	/* Records what the firmware said runs. */
	thermal->features_low = low;
	thermal->features_high = high;

	/* Reports features that do not run. */
	if (!on) {
		amdrdna_mark("A4 features: not running, 0x%08x%08x (%d)", (unsigned)high, (unsigned)low, error);
		return EIO;
	}

	/*
	 * features_on hands the fan to the firmware: the watch takes the
	 * temperatures and the fan from its readings, and the stop switches
	 * the features off again.
	 */
	thermal->features_on = true;
	thermal->mode = AMDRDNA_FAN_FIRMWARE;
	thermal->duty_percent = 0U;
	amdrdna_mark("A4 features %s on: 0x%08x%08x, fan mode word 0x%08x",
	    how,
	    (unsigned)high,
	    (unsigned)low,
	    (unsigned)thermal_read(device, AMDRDNA_THM_REG_FAN_MODE));

	/* Says whether the graphics clock's levels run, when the boot line asked for them. */
	if (thermal->clock_levels) {
		amdrdna_mark("A4 features: graphics clock levels %s (permitted 0x%08x%08x)",
		    (low & AMDRDNA_SMU_FEATURE_GFX_LEVELS) != 0U ? "run" : "do not run",
		    (unsigned)permitted_high,
		    (unsigned)permitted);
	}

	/* Succeeded: the firmware runs the fan and the thermal control. */
	return 0;
}

/*
 * Asks the unit's firmware to switch the three features on, the low word
 * first, then the high word.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu_cmn.c
 * (smu_cmn_feature_update_enable_state).
 */
static int
power_features_ask(
	struct amdrdna_device *device)
{
	int error;

	/* The critical-temperature stop. */
	error = smu_message(device, SMU_MESSAGE_SOME_ON_LOW, AMDRDNA_SMU_FEATURES_LOW, NULL);
	if (error != 0)
		return error;

	/* The fan and the thermal control. */
	error = smu_message(device, SMU_MESSAGE_SOME_ON_HIGH, AMDRDNA_SMU_FEATURES_HIGH, NULL);
	if (error != 0)
		return error;

	/* Succeeded: the firmware took both words. */
	return 0;
}

/*
 * Reads the features the unit's firmware runs, the high word first.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu_cmn.c (smu_cmn_get_enabled_mask).
 */
static int
power_features_running(
	struct amdrdna_device *device,
	uint32_t *low,
	uint32_t *high)
{
	int error;

	/* The high word. */
	error = smu_message(device, SMU_MESSAGE_RUNNING_HIGH, 0U, high);
	if (error != 0)
		return error;

	/* The low word. */
	error = smu_message(device, SMU_MESSAGE_RUNNING_LOW, 0U, low);
	if (error != 0)
		return error;

	/* Succeeded: both words are read. */
	return 0;
}

/*
 * Switches every feature of the unit's firmware off; the fan and the
 * thermal control are no longer the firmware's.  A firmware that stopped
 * answering is not asked.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c
 * (smu_v11_0_system_features_control).
 */
static void
power_features_off(
	struct amdrdna_device *device)
{
	struct amdrdna_thermal *thermal;
	int error;

	/* The power side's state. */
	thermal = &device->thermal;

	/* Asks for all features off. */
	error = smu_message(device, SMU_MESSAGE_ALL_OFF, 0U, NULL);

	/* The features are not counted on from now on, whatever the answer. */
	thermal->features_on = false;
	amdrdna_mark("smu: all features off, answered %d", error);
}

/*
 * Copies a table of bytes bytes (a multiple of 4) into the table page and
 * has the unit's firmware take it as the table id.  The copy and the
 * message happen under the mailbox, which owns the page.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu_cmn.c (smu_cmn_update_table, the copy to
 * the firmware: the table copied, the host path flushed, then the message
 * with the table's ID).
 */
static int
power_table_send(
	struct amdrdna_device *device,
	uint32_t id,
	const uint8_t *table,
	size_t bytes)
{
	struct amdrdna_thermal *thermal;
	volatile uint8_t *page;
	uint32_t word;
	size_t at;
	int error;

	/* The page as the CPU sees it. */
	thermal = &device->thermal;
	page = amdrdna_vram_cpu(device, thermal->table_vram);

	/* One user of the page and the mailbox at a time. */
	mutex_lock(&thermal->mailbox);

	/* Copies the table a word at a time. */
	for (at = 0U; at < bytes; at += 4U) {
		kern_memcpy(&word, table + at, sizeof(word));
		kern_mmio_write32(page + at, word);
	}

	/* Makes it visible to the firmware. */
	amdrdna_host_path_flush(device);

	/* Has the firmware take it. */
	error = smu_message_held(device, SMU_MESSAGE_TABLE_FROM_PAGE, id, NULL);

	mutex_unlock(&thermal->mailbox);

	/* Reports a table the firmware did not take. */
	if (error != 0)
		return error;

	/* Succeeded: the firmware has the table. */
	return 0;
}

/*
 * Sets up the graphics clock once its levels run, as Linux does after it
 * switches the features on: reads the clock's levels, tells the firmware
 * the workload is the default one, hands over the averaging settings, and
 * takes the clock's ceiling off (the automatic level's ceiling).  Then
 * chooses how the clock is raised for work: wanted (amdrdna.smu.boost=),
 * the profile alone when the highest level is unknown, and not at all
 * when the firmware refuses the workload.  A refusal here does not undo
 * the features; the firmware keeps moving the clock on its own.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/amdgpu_smu.c (smu_smc_hw_setup:
 * smu_set_default_dpm_table; smu_late_init: the workload of the
 * initial task, then smu_apply_default_config_table_settings;
 * smu_bump_power_profile_mode),
 * drivers/gpu/drm/amd/pm/swsmu/smu11/navi10_ppt.c
 * (navi10_set_power_profile_mode, navi10_set_config_table) and
 * drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c
 * (smu_v11_0_set_performance_level, the automatic level).
 */
static void
power_clock_setup(
	struct amdrdna_device *device,
	unsigned wanted)
{
	struct amdrdna_thermal *thermal;
	uint8_t settings[AMDRDNA_SMU_SETTINGS_BYTES];
	unsigned long irq;
	uint32_t count;
	uint32_t lowest;
	uint32_t highest;
	const char *how;
	unsigned mode;
	int levels_error;
	int workload_error;
	int settings_error;
	int ceiling_error;

	/* The power side's state. */
	thermal = &device->thermal;

	/* Reads the graphics clock's levels: how many, the lowest and the highest. */
	count = 0U;
	lowest = 0U;
	highest = 0U;
	levels_error = power_clock_levels(device, SMU_CLOCK_GFX, &count, &lowest, &highest);

	/* Tells the firmware the workload is the default one. */
	workload_error = smu_message(device, SMU_MESSAGE_WORKLOADS, SMU_WORKLOAD_DEFAULT, NULL);

	/* Hands over the averaging settings. */
	amdrdna_power_settings_build(settings);
	settings_error = power_table_send(device, SMU_TABLE_SETTINGS, settings, sizeof(settings));

	/* Takes the graphics clock's ceiling off, so the firmware may go up to its highest level. */
	ceiling_error = smu_message(device, SMU_MESSAGE_CEILING, (SMU_CLOCK_GFX << 16) | SMU_CEILING_FREE, NULL);

	/* Chooses the boost: the floor needs the highest level, and any boost needs the workload. */
	mode = wanted;
	if (mode > THERMAL_BOOST_FLOOR)
		mode = THERMAL_BOOST_FLOOR;
	if (mode == THERMAL_BOOST_FLOOR && levels_error != 0)
		mode = THERMAL_BOOST_PROFILE;
	if (workload_error != 0)
		mode = THERMAL_BOOST_NONE;

	/* Names the boost for the log. */
	how = "off";
	if (mode == THERMAL_BOOST_PROFILE)
		how = "3d profile";
	if (mode == THERMAL_BOOST_FLOOR)
		how = "3d profile and floor";

	/* Publishes the levels and the boost, which the watch and the runs read from now on. */
	irq = spin_lock_irqsave(&thermal->lock);

	thermal->gfx_lowest = lowest;
	thermal->gfx_highest = highest;
	thermal->boost_mode = mode;

	spin_unlock_irqrestore(&thermal->lock, irq);

	/* Holds the memory clock at its highest level, when its levels run. */
	power_memory_clock(device);

	/* Marks the setup with each step's answer. */
	amdrdna_mark("A4 graphics clock: %u levels %u-%uMHz (%d), workload %d, averages %d, ceiling %d, boost %s",
	    (unsigned)count,
	    (unsigned)lowest,
	    (unsigned)highest,
	    levels_error,
	    workload_error,
	    settings_error,
	    ceiling_error,
	    how);
}

/*
 * Holds the memory clock at its highest level for the device's life, once
 * the firmware runs its levels: the ceiling, then the floor, at the
 * highest.  The firmware otherwise keeps it at the lowest (100 MHz on the
 * RX 5600 XT, a fifth to a ninth of the bandwidth Linux works with).  The
 * change waits for the display's hub arbiter, which the firmware's picture
 * leaves keeping the memory's self refresh off, so the arbiter allows the
 * change while the two messages run (display.c).
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu11/navi10_ppt.c
 * (navi10_display_disable_memory_clock_switch: the memory clock's hard
 * floor at its highest level while the display cannot take its change) and
 * drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c
 * (smu_v11_0_set_hard_freq_limited_range: the ceiling, then the floor).
 */
static void
power_memory_clock(
	struct amdrdna_device *device)
{
	struct amdrdna_thermal *thermal;
	uint32_t count;
	uint32_t lowest;
	uint32_t highest;
	uint32_t saved;
	uint32_t answer;
	uint32_t allow;
	uint32_t level;
	int ceiling_error;
	int floor_error;
	int error;

	/* Only while the firmware runs the memory clock's levels. */
	thermal = &device->thermal;
	if ((thermal->features_low & AMDRDNA_SMU_FEATURE_MEMORY_LEVELS) == 0U) {
		amdrdna_mark("A4 memory clock: its levels do not run");
		return;
	}

	/* Its levels. */
	count = 0U;
	lowest = 0U;
	highest = 0U;
	error = power_clock_levels(device, SMU_CLOCK_MEMORY, &count, &lowest, &highest);
	if (error != 0) {
		amdrdna_mark("A4 memory clock: no levels (%d)", error);
		return;
	}

	/* Each level's frequency, for the display's step. */
	if (count > AMDRDNA_MEMORY_LEVELS)
		count = AMDRDNA_MEMORY_LEVELS;
	for (level = 0U; level < count; level++) {
		error = amdrdna_smu_clock_level(device, SMU_CLOCK_MEMORY, level, &thermal->memory_levels_mhz[level]);
		if (error != 0) {
			amdrdna_mark("A4 memory clock: level %u unread (%d)", (unsigned)level, error);
			return;
		}
	}
	thermal->memory_level_count = count;

	/* The memory controller's answer about its workaround, marked. */
	answer = 0U;
	error = smu_message(device, SMU_MESSAGE_MEMORY_WORKAROUND, 0U, &answer);
	amdrdna_mark("A4 memory clock: the memory controller's workaround answer 0x%08x (%d)", (unsigned)answer, error);

	/* The ceiling and the floor at the highest level, while the display's arbiter allows the change. */
	saved = amdrdna_display_memory_switch_begin(device);
	ceiling_error = smu_message(device, SMU_MESSAGE_HARD_CEILING, (SMU_CLOCK_MEMORY << 16) | highest, NULL);
	floor_error = EIO;
	if (ceiling_error == 0)
		floor_error = smu_message(device, SMU_MESSAGE_HARD_FLOOR, (SMU_CLOCK_MEMORY << 16) | highest, NULL);
	thermal->memory_saved_state = saved;
	allow = amdrdna_parameter_number("amdrdna.smu.memory_allow", 0U);
	if (thermal->memory_mode == 2U) {
		/* Held with the change allowed until the display's step tells the firmware what the display needs. */
		amdrdna_display_memory_switch_hold(device, saved);
	} else if (allow == 1U) {
		/* amdrdna.smu.memory_allow=1: the arbiter keeps allowing the change. */
		amdrdna_display_memory_switch_hold(device, saved);
	} else if (allow == 2U) {
		/* amdrdna.smu.memory_allow=2: the self refresh and the change as the watermarks allow them, as Linux leaves the arbiter. */
		amdrdna_display_memory_switch_release(device, saved);
	} else {
		amdrdna_display_memory_switch_end(device, saved);
	}

	/* Marks the levels and each answer. */
	amdrdna_mark("A4 memory clock: %u levels %u-%uMHz, held at %uMHz (ceiling %d, floor %d, arbiter 0x%08x)",
	    (unsigned)count,
	    (unsigned)lowest,
	    (unsigned)highest,
	    (unsigned)highest,
	    ceiling_error,
	    floor_error,
	    (unsigned)saved);
}

/*
 * Reads how many levels a clock (SMU_CLOCK_*) has and the frequency of the
 * lowest and the highest, in MHz.
 *
 * Returns 0, EIO when the firmware gives no level, or the error of the
 * message that failed.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c
 * (smu_v11_0_set_single_dpm_table, smu_v11_0_get_dpm_level_count,
 * smu_v11_0_get_dpm_freq_by_index).
 */
static int
power_clock_levels(
	struct amdrdna_device *device,
	uint32_t clock,
	uint32_t *count,
	uint32_t *lowest,
	uint32_t *highest)
{
	uint32_t answer;
	int error;

	/* How many levels. */
	answer = 0U;
	error = smu_message(device, SMU_MESSAGE_LEVEL, (clock << 16) | SMU_LEVEL_COUNT, &answer);
	if (error != 0)
		return error;

	/* A clock without levels cannot be raised to one. */
	*count = answer & SMU_LEVEL_FREQUENCY_MASK;
	if (*count == 0U)
		return EIO;

	/* The lowest level's frequency. */
	error = smu_message(device, SMU_MESSAGE_LEVEL, clock << 16, &answer);
	if (error != 0)
		return error;
	*lowest = answer & SMU_LEVEL_FREQUENCY_MASK;

	/* The highest level's frequency. */
	error = smu_message(device, SMU_MESSAGE_LEVEL, (clock << 16) | (*count - 1U), &answer);
	if (error != 0)
		return error;
	*highest = answer & SMU_LEVEL_FREQUENCY_MASK;

	/* A highest level of 0 MHz is no level either. */
	if (*highest == 0U)
		return EIO;

	/* Succeeded: the levels are read. */
	return 0;
}

/*
 * Raises the graphics clock for work, or lowers it again: the firmware's
 * workloads become the default and the 3D one (raising) or the default one
 * alone, and with the floor boost the clock's floor goes to its highest
 * level (raising) or back to the firmware's choice.  A refusal ends the
 * boost for good.
 *
 * Returns 0, or the error of the message the firmware refused.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_gfx.c
 * (amdgpu_gfx_profile_ring_begin_use, amdgpu_gfx_profile_idle_work_handler:
 * the 3D profile while a graphics ring is in use),
 * drivers/gpu/drm/amd/pm/swsmu/smu11/navi10_ppt.c
 * (navi10_set_power_profile_mode) and
 * drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c
 * (smu_v11_0_set_soft_freq_limited_range).
 */
static int
power_boost(
	struct amdrdna_device *device,
	bool raising)
{
	struct amdrdna_thermal *thermal;
	unsigned long irq;
	uint32_t workloads;
	uint32_t floor_mhz;
	unsigned mode;
	int error;

	/* What the boost is; the setup and the watch alone change it. */
	thermal = &device->thermal;
	irq = spin_lock_irqsave(&thermal->lock);

	mode = thermal->boost_mode;
	floor_mhz = thermal->gfx_highest;

	spin_unlock_irqrestore(&thermal->lock, irq);

	/* A boost that ended does nothing. */
	if (mode == THERMAL_BOOST_NONE)
		return 0;

	/* The workloads and the floor of the raised or the lowered clock. */
	workloads = SMU_WORKLOAD_DEFAULT;
	if (raising) {
		workloads |= SMU_WORKLOAD_3D;
	} else {
		floor_mhz = SMU_FLOOR_FREE;
	}

	/* Tells the firmware the workloads, then moves the floor, and the memory clock's floor with the display's step. */
	error = smu_message(device, SMU_MESSAGE_WORKLOADS, workloads, NULL);
	if (error == 0 && mode == THERMAL_BOOST_FLOOR)
		error = smu_message(device, SMU_MESSAGE_FLOOR, (SMU_CLOCK_GFX << 16) | floor_mhz, NULL);
	if (error == 0)
		amdrdna_memory_display_busy(device, raising);

	/* A refusal ends the boost for good. */
	if (error != 0) {
		power_boost_end(device, mode, raising, error);
		return error;
	}

	/* Succeeded: the firmware has the workloads and the floor. */
	return 0;
}

/*
 * Ends the boost for good after the firmware refused a change of it: the
 * runs no longer ask for it, the default workload and the free floor are
 * asked for again, and the log says which change was refused.
 */
static void
power_boost_end(
	struct amdrdna_device *device,
	unsigned mode,
	bool raising,
	int error)
{
	struct amdrdna_thermal *thermal;
	unsigned long irq;

	/* No boost from now on. */
	thermal = &device->thermal;
	irq = spin_lock_irqsave(&thermal->lock);

	thermal->boost_mode = THERMAL_BOOST_NONE;
	thermal->boost_wanted = false;
	thermal->boosted = false;

	spin_unlock_irqrestore(&thermal->lock, irq);

	/* Puts the default workload and the free floor back, whatever the answers. */
	(void)smu_message(device, SMU_MESSAGE_WORKLOADS, SMU_WORKLOAD_DEFAULT, NULL);
	if (mode == THERMAL_BOOST_FLOOR)
		(void)smu_message(device, SMU_MESSAGE_FLOOR, (SMU_CLOCK_GFX << 16) | SMU_FLOOR_FREE, NULL);

	/* Says which change was refused. */
	amdrdna_mark("smu: the graphics clock's boost refused (%s, %d); no more boosts", raising ? "raise" : "lower", error);
}

/*
 * Has the unit's firmware write its readings into the table page and reads
 * the temperatures and the fan's speed from them.  The message and the
 * copy happen under the mailbox, which owns the page.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu_cmn.c (smu_cmn_update_table, the copy
 * from the firmware: the message with the table's ID, the host path's read
 * cache dropped, then the table copied; smu_cmn_get_metrics_table).
 */
static int
power_readings_fetch(
	struct amdrdna_device *device,
	struct amdrdna_power_readings *readings)
{
	struct amdrdna_thermal *thermal;
	uint8_t bytes[AMDRDNA_SMU_READINGS_BYTES];
	volatile uint8_t *page;
	uint32_t word;
	size_t at;
	int error;

	/* The page as the CPU sees it. */
	thermal = &device->thermal;
	page = amdrdna_vram_cpu(device, thermal->table_vram);

	/* One user of the page and the mailbox at a time. */
	mutex_lock(&thermal->mailbox);

	/* Has the firmware write its readings. */
	error = smu_message_held(device, SMU_MESSAGE_TABLE_TO_PAGE, SMU_TABLE_READINGS, NULL);

	/* Copies them out as the firmware wrote them. */
	if (error == 0) {
		amdrdna_host_path_drop(device);
		for (at = 0U; at < AMDRDNA_SMU_READINGS_BYTES; at += 4U) {
			word = kern_mmio_read32(page + at);
			kern_memcpy(bytes + at, &word, sizeof(word));
		}
	}

	mutex_unlock(&thermal->mailbox);

	/* Reports readings the firmware did not write. */
	if (error != 0)
		return error;

	/* Reads the temperatures and the fan from them. */
	amdrdna_power_readings_read(bytes, readings);

	/* Succeeded: the readings are read. */
	return 0;
}

/*
 * Sets the driver's temperature limit (amdrdna.temp.max=, else the board's
 * edge limit, else 95 degrees Celsius) and sends the thermal alerts to the
 * interrupt ring: high at the board's shutdown temperature from its power
 * table or the video BIOS (or 10 degrees above the driver's limit without
 * one), low at 0; and lets the unit raise its software interrupt.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c (smu_v11_0_set_irq_state,
 * the enable case).
 */
static void
power_alerts(
	struct amdrdna_device *device)
{
	struct amdrdna_thermal *thermal;
	unsigned fallback;
	uint32_t word;

	/* The power side's state. */
	thermal = &device->thermal;

	/* The driver's limit: the board's edge limit when its table was read, else the default. */
	fallback = THERMAL_DEFAULT_LIMIT;
	if (thermal->table_built && thermal->limits.edge != 0U)
		fallback = thermal->limits.edge;
	thermal->limit = amdrdna_parameter_number("amdrdna.temp.max", fallback);

	/* The high alert's threshold: the shutdown temperature of the table, else of the video BIOS. */
	thermal->high_alert = device->vbios_info.shutdown_temperature;
	if (thermal->table_built && thermal->limits.shutdown != 0U)
		thermal->high_alert = thermal->limits.shutdown;
	if (thermal->high_alert == 0U)
		thermal->high_alert = thermal->limit + THERMAL_HIGH_ABOVE_LIMIT;
	if (thermal->high_alert > 255U)
		thermal->high_alert = 255U;

	/* The thresholds and the switch to the ring, then the clears. */
	word = thermal_read(device, AMDRDNA_THM_REG_ALERT_CONTROL);
	thermal_write(device, AMDRDNA_THM_REG_ALERT_CONTROL, amdrdna_alert_word(word, thermal->high_alert, 0U));
	thermal_write(device, AMDRDNA_THM_REG_ALERT_ENABLE, amdrdna_alert_clear_word());

	/* The unit's software interrupt: its ID, not raised, and unmasked. */
	word = amdrdna_block_read(device, AMDRDNA_BLOCK_SMU, AMDRDNA_SMU_WINDOW_MAILBOX, AMDRDNA_SMU_REG_SOFT_INTERRUPT);
	word &= ~(AMDRDNA_SMU_SOFT_INTERRUPT_ID_MASK | AMDRDNA_SMU_SOFT_INTERRUPT_VALID);
	word |= AMDRDNA_SOURCE_SMU_SOFT;
	amdrdna_block_write(device, AMDRDNA_BLOCK_SMU, AMDRDNA_SMU_WINDOW_MAILBOX, AMDRDNA_SMU_REG_SOFT_INTERRUPT, word);
	word = amdrdna_block_read(device, AMDRDNA_BLOCK_SMU, AMDRDNA_SMU_WINDOW_MAILBOX, AMDRDNA_SMU_REG_SOFT_INTERRUPT_CONTROL);
	word &= ~AMDRDNA_SMU_SOFT_INTERRUPT_MASKED;
	amdrdna_block_write(device, AMDRDNA_BLOCK_SMU, AMDRDNA_SMU_WINDOW_MAILBOX, AMDRDNA_SMU_REG_SOFT_INTERRUPT_CONTROL, word);
	thermal->alerts_on = true;

	/* Marks the alerts. */
	amdrdna_mark("A4 alerts high=%uC low=0C control=0x%08x",
	    thermal->high_alert,
	    (unsigned)thermal_read(device, AMDRDNA_THM_REG_ALERT_CONTROL));
}

/*
 * Checks that an interrupt reaches the driver: raises the low alert's
 * threshold 20 degrees above the temperature, which makes the thermal
 * block send a low alert, waits up to 500 ms for the interrupt thread to
 * count it, and puts the threshold back at 0.  Reports whether one came.
 */
static bool
power_interrupt_check(
	struct amdrdna_device *device)
{
	struct amdrdna_thermal *thermal;
	unsigned long irq;
	uint64_t before;
	uint64_t after;
	uint64_t signals;
	unsigned sensor;
	unsigned highest;
	unsigned poll;
	uint32_t word;

	/* The power side's state. */
	thermal = &device->thermal;

	/* The count of low alerts and messages before the check. */
	irq = spin_lock_irqsave(&thermal->lock);

	before = thermal->low_alerts;

	spin_unlock_irqrestore(&thermal->lock, irq);
	signals = device->interrupts.signals;

	/* Raises the low threshold above the temperature. */
	amdrdna_temperature_read(thermal_read(device, AMDRDNA_THM_REG_TEMPERATURE), &sensor, &highest);
	word = thermal_read(device, AMDRDNA_THM_REG_ALERT_CONTROL);
	thermal_write(device, AMDRDNA_THM_REG_ALERT_CONTROL, amdrdna_alert_word(word, thermal->high_alert, sensor + THERMAL_CHECK_ABOVE));

	/* Waits for the alert to be counted. */
	after = before;
	for (poll = 0U; poll < THERMAL_CHECK_POLLS; poll++) {
		/* Sleeps one step, then reads the count. */
		kern_usleep_range(THERMAL_CHECK_POLL_US, THERMAL_CHECK_POLL_US + THERMAL_CHECK_POLL_US / 4U);
		irq = spin_lock_irqsave(&thermal->lock);

		after = thermal->low_alerts;

		spin_unlock_irqrestore(&thermal->lock, irq);

		/* Stops once it came. */
		if (after != before)
			break;
	}

	/* Puts the low threshold back at 0. */
	word = thermal_read(device, AMDRDNA_THM_REG_ALERT_CONTROL);
	thermal_write(device, AMDRDNA_THM_REG_ALERT_CONTROL, amdrdna_alert_word(word, thermal->high_alert, 0U));
	thermal_write(device, AMDRDNA_THM_REG_ALERT_ENABLE, amdrdna_alert_clear_word());

	/* Marks what came: the alerts, the messages and the entries. */
	amdrdna_mark("A4 irq check low=%uC: low alerts %llu -> %llu, messages %llu -> %llu, entries %llu, unknown %llu",
	    sensor + THERMAL_CHECK_ABOVE,
	    (unsigned long long)before,
	    (unsigned long long)after,
	    (unsigned long long)signals,
	    (unsigned long long)device->interrupts.signals,
	    (unsigned long long)device->interrupts.entries,
	    (unsigned long long)device->interrupts.unknown);

	/* Reports a missing alert. */
	if (after == before)
		return false;

	/* Succeeded: the alert came through the ring. */
	return true;
}

/* Waits up to 5 s for the fan's tachometer to count turns. */
static void
power_spin_up(
	struct amdrdna_device *device)
{
	uint32_t speed;
	unsigned poll;

	/* Looks every 100 ms until the fan turns. */
	for (poll = 0U; poll < THERMAL_SPIN_POLLS; poll++) {
		/* Stops once the tachometer counts turns. */
		speed = amdrdna_fan_speed(thermal_read(device, AMDRDNA_THM_REG_FAN_TACHOMETER));
		if (speed != 0U)
			return;

		/* Sleeps one step. */
		kern_usleep_range(THERMAL_SPIN_POLL_US, THERMAL_SPIN_POLL_US + THERMAL_SPIN_POLL_US / 10U);
	}
}

/*
 * Reads the temperatures and the fan and publishes them; faults the GPU
 * past the limit or when the unit's firmware gives no readings.  The
 * thermal block is read always; when the firmware runs its features, its
 * readings are the temperature and the fan the driver goes by, and the
 * thermal block's are kept beside them for the comparison, which the
 * watch's tenth reading marks in the log.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/powerplay/hwmgr/vega10_thermal.c
 * (vega10_thermal_get_temperature),
 * drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c
 * (smu_v11_0_get_fan_speed_rpm, smu_v11_0_get_fan_speed_pwm) and
 * drivers/gpu/drm/amd/pm/swsmu/smu11/navi10_ppt.c
 * (navi10_get_fan_speed_rpm: the firmware's speed while it runs the fan).
 */
static void
power_read(
	struct amdrdna_device *device)
{
	struct amdrdna_thermal *thermal;
	struct amdrdna_power_readings readings;
	unsigned long irq;
	unsigned sensor;
	unsigned highest;
	unsigned driven;
	unsigned temperature;
	unsigned count;
	uint32_t tachometer;
	uint32_t speed;
	uint32_t limits;
	uint32_t word;
	bool fetched;
	int error;

	/* The power side's state. */
	thermal = &device->thermal;

	/* Reads the thermal block: the temperatures, the tachometer's speed and the duty driven. */
	word = thermal_read(device, AMDRDNA_THM_REG_TEMPERATURE);
	amdrdna_temperature_read(word, &sensor, &highest);
	word = thermal_read(device, AMDRDNA_THM_REG_FAN_TACHOMETER);
	tachometer = amdrdna_fan_speed(word);
	limits = thermal_read(device, AMDRDNA_THM_REG_FAN_LIMITS);
	word = thermal_read(device, AMDRDNA_THM_REG_FAN_STATUS);
	driven = amdrdna_fan_driven(word, limits);

	/* Reads the firmware's readings while it runs its features. */
	fetched = false;
	readings.edge = 0U;
	readings.hotspot = 0U;
	readings.memory = 0U;
	readings.fan_speed = 0U;
	readings.gfx_clock = 0U;
	readings.system_clock = 0U;
	readings.memory_clock = 0U;
	readings.return_clock = 0U;
	readings.display_engine_clock = 0U;
	readings.gfx_average = 0U;
	readings.gfx_activity = 0U;
	if (thermal->features_on) {
		/* A firmware that gives no readings no longer watches the GPU either. */
		error = power_readings_fetch(device, &readings);
		if (error == 0) {
			fetched = true;
		} else {
			power_fault(device, "the smu firmware gave no readings");
		}
	}

	/* Goes by the firmware's readings when there are some, else by the thermal block. */
	temperature = sensor;
	speed = tachometer;
	if (fetched) {
		temperature = readings.edge;
		speed = readings.fan_speed;
	}

	/* Publishes the readings, and counts them for the one comparison mark. */
	irq = spin_lock_irqsave(&thermal->lock);

	thermal->temperature = temperature;
	thermal->highest = highest;
	thermal->sensor = sensor;
	thermal->hotspot = readings.hotspot;
	thermal->memory_temperature = readings.memory;
	thermal->fan_speed = speed;
	thermal->tachometer_speed = tachometer;
	thermal->fan_driven = driven;
	thermal->clocked = fetched;
	thermal->gfx_clock = readings.gfx_clock;
	thermal->system_clock = readings.system_clock;
	thermal->memory_clock = readings.memory_clock;
	thermal->return_clock = readings.return_clock;
	thermal->display_engine_clock = readings.display_engine_clock;
	thermal->gfx_average = readings.gfx_average;
	thermal->gfx_activity = readings.gfx_activity;

	/* Keeps the highest graphics clock and activity until the timing marks take them. */
	if (readings.gfx_clock > thermal->gfx_clock_top)
		thermal->gfx_clock_top = readings.gfx_clock;
	if (readings.gfx_activity > thermal->gfx_activity_top)
		thermal->gfx_activity_top = readings.gfx_activity;

	thermal->readings++;
	count = thermal->readings;

	spin_unlock_irqrestore(&thermal->lock, irq);

	/* Marks the two sources side by side once, a while after the stage. */
	if (count == THERMAL_COMPARE_READING) {
		amdrdna_mark("thermal reading %u: edge %uC hotspot %uC mem %uC fan %urpm; thm %uC max %uC tach %urpm driven %u%%",
		    count,
		    readings.edge,
		    readings.hotspot,
		    readings.memory,
		    readings.fan_speed,
		    sensor,
		    highest,
		    (unsigned)tachometer,
		    driven);
	}

	/* Past the limit the GPU is faulted. */
	if (temperature >= thermal->limit)
		power_fault(device, "the temperature reached the limit");
}

/*
 * Faults the GPU for a thermal reason: marks it once and logs why, drives
 * the fan at full duty through the thermal block, and fails the device so
 * the running work stops and the GPU takes no more; the system keeps
 * running.
 */
static void
power_fault(
	struct amdrdna_device *device,
	const char *why)
{
	struct amdrdna_thermal *thermal;
	unsigned long irq;
	bool first;
	int error;

	/* The power side's state. */
	thermal = &device->thermal;

	/* Marks the fault, noting whether it is the first. */
	irq = spin_lock_irqsave(&thermal->lock);

	first = !thermal->faulted;
	thermal->faulted = true;

	spin_unlock_irqrestore(&thermal->lock, irq);

	/* Only the first fault acts. */
	if (!first)
		return;

	/* Logs it with the readings. */
	amdrdna_mark("thermal fault: %s (temp %uC, limit %uC, fan %urpm); the GPU takes no more work",
	    why,
	    thermal->temperature,
	    thermal->limit,
	    (unsigned)thermal->fan_speed);

	/* The work stops first: the running sessions fail and nothing new runs, so the GPU cools down. */
	amdrdna_node_device_failed(device, EIO);

	/*
	 * Then the fan at full duty through the thermal block itself: the
	 * firmware that drove it may have stopped (a hung unit leaves the fan
	 * as it last set it, which may be off).
	 */
	error = power_fan_duty(device, 100U);
	if (error != 0)
		amdrdna_mark("thermal fault: the fan could not be driven (%d)", error);
}

/* Starts the watch thread. */
static int
power_watch_start(
	struct amdrdna_device *device)
{
	struct thread *thread;
	int error;

	/* Creates the thread. */
	error = kthread_create(power_watch, device, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0)
		return error;

	/*
	 * thread_running tells the stop that the thread may still read the
	 * thermal block; the thread clears it last.  Nobody joins the thread.
	 */
	device->thermal.thread_running = true;
	thread->detached = 1;
	thread_start(thread);

	/* Succeeded: the watch runs. */
	return 0;
}

/*
 * Reads the temperature and the fan once a second until told to stop,
 * and raises the graphics clock when work asks for it and lowers it a
 * second after the last work.
 */
static void
power_watch(
	void *argument)
{
	struct amdrdna_device *device;
	struct amdrdna_thermal *thermal;
	unsigned long irq;
	uint64_t sequence;
	uint64_t next_read;
	uint64_t now;
	bool stopping;
	bool raising;
	bool lowering;

	/* The GPU the thread watches, first read a period from now. */
	device = argument;
	thermal = &device->thermal;
	next_read = sched_ticks() + THERMAL_WATCH_TICKS;

	/* One reading or one change of the boost per turn of the loop. */
	for (;;) {
		/* Sleeps until the next reading, unless the stop or a boost is asked for. */
		irq = spin_lock_irqsave(&thermal->lock);

		if (!thermal->stopping && !thermal->boost_wanted) {
			sequence = waitq_sequence(&thermal->waitq);
			(void)waitq_sleep(&thermal->waitq, &thermal->lock, sequence, next_read, 0U);
		}

		/* Notes whether the stop came. */
		stopping = thermal->stopping;
		now = sched_ticks();

		/*
		 * Decides the boost: raised when work asked for it, lowered a
		 * second after the last work.  boosted changes here, before the
		 * messages, so work that comes while the clock is being lowered
		 * asks for the boost again.
		 */
		raising = false;
		lowering = false;
		if (thermal->boost_wanted) {
			thermal->boost_wanted = false;
			thermal->boosted = true;
			thermal->boosts++;
			raising = true;
		} else if (thermal->boosted && now - thermal->busy_ticks >= THERMAL_BOOST_IDLE_TICKS) {
			thermal->boosted = false;
			lowering = true;
		}

		spin_unlock_irqrestore(&thermal->lock, irq);

		/* Leaves the loop when told to stop. */
		if (stopping)
			break;

		/* Raises the clock, and reads soon after so the readings show it; or lowers it. */
		if (raising) {
			(void)power_boost(device, true);
			next_read = now + THERMAL_BOOST_READ_TICKS;
		} else if (lowering) {
			(void)power_boost(device, false);
		}

		/* Reads and checks once the reading is due. */
		if (now >= next_read) {
			power_read(device);
			amdrdna_memory_display_watch(device);
			next_read = now + THERMAL_WATCH_TICKS;
		}
	}

	/* Tells the stop that the thread no longer reads the block. */
	irq = spin_lock_irqsave(&thermal->lock);

	thermal->thread_running = false;

	spin_unlock_irqrestore(&thermal->lock, irq);
}

/*
 * Sends one message to the unit's firmware and waits for its answer,
 * under the mailbox.
 *
 * Returns what smu_message_held returns.
 */
static int
smu_message(
	struct amdrdna_device *device,
	uint32_t message,
	uint32_t argument,
	uint32_t *answer)
{
	struct amdrdna_thermal *thermal;
	int error;

	/* The power side's state. */
	thermal = &device->thermal;

	/* One message at a time. */
	mutex_lock(&thermal->mailbox);

	error = smu_message_held(device, message, argument, answer);

	mutex_unlock(&thermal->mailbox);

	/* Reports a message the unit did not carry out. */
	if (error != 0)
		return error;

	/* Succeeded: the unit carried the message out. */
	return 0;
}

/*
 * Sends one message to the unit's firmware and waits for its answer; the
 * caller holds the mailbox.  All but the first message wait for the
 * previous answer first; the answer word is cleared, the argument and the
 * message written, and the answer waited for.  On OK the output argument
 * is read.  A unit that does not answer is marked hung and sent nothing
 * more.
 *
 * Returns 0, EIO for a failed or hung unit, EINVAL for a message it does
 * not know, EAGAIN for a missing prerequisite, EBUSY, or ETIMEDOUT.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu_cmn.c (smu_cmn_send_smc_msg_with_param,
 * __smu_cmn_send_msg, __smu_cmn_poll_stat, __smu_cmn_reg2errno,
 * smu_cmn_read_arg).
 */
static int
smu_message_held(
	struct amdrdna_device *device,
	uint32_t message,
	uint32_t argument,
	uint32_t *answer)
{
	struct amdrdna_thermal *thermal;
	uint32_t word;
	int error;

	/* The power side's state. */
	thermal = &device->thermal;

	/* A hung unit is sent nothing. */
	if (thermal->hung)
		return EIO;

	/* After the first message, the previous answer must have come. */
	if (thermal->first_sent) {
		word = smu_wait_answer(device);
		if (word == 0U) {
			amdrdna_mark("smu: no answer to the previous message before 0x%x", (unsigned)message);
			smu_hang_mark(device, message);
			return ETIMEDOUT;
		}
	}

	/* Clears the answer, writes the argument, then the message. */
	amdrdna_block_write(device, AMDRDNA_BLOCK_SMU, AMDRDNA_SMU_WINDOW_MAILBOX, AMDRDNA_SMU_REG_RESPONSE, 0U);
	amdrdna_block_write(device, AMDRDNA_BLOCK_SMU, AMDRDNA_SMU_WINDOW_MAILBOX, AMDRDNA_SMU_REG_ARGUMENT, argument);
	amdrdna_block_write(device, AMDRDNA_BLOCK_SMU, AMDRDNA_SMU_WINDOW_MAILBOX, AMDRDNA_SMU_REG_MESSAGE, message);
	thermal->first_sent = true;

	/* Waits for the answer and turns it into an error. */
	word = smu_wait_answer(device);
	switch (word) {
	case SMU_ANSWER_OK:
		error = 0;
		break;
	case SMU_ANSWER_FAILED:
		error = EIO;
		break;
	case SMU_ANSWER_UNKNOWN:
		error = EINVAL;
		break;
	case SMU_ANSWER_PREREQUISITE:
		error = EAGAIN;
		break;
	case SMU_ANSWER_BUSY:
		error = EBUSY;
		break;
	case 0U:
		error = ETIMEDOUT;
		thermal->hung = true;
		break;
	default:
		error = EIO;
		thermal->hung = true;
		break;
	}

	/* Reports a message the unit did not carry out, and what the unit shows of a hang. */
	if (error != 0) {
		amdrdna_mark("smu: message 0x%x answered 0x%x (%d)", (unsigned)message, (unsigned)word, error);
		if (thermal->hung)
			smu_hang_mark(device, message);
		return error;
	}

	/* Reads the output argument of an answered message. */
	if (answer != NULL)
		*answer = amdrdna_block_read(device, AMDRDNA_BLOCK_SMU, AMDRDNA_SMU_WINDOW_MAILBOX, AMDRDNA_SMU_REG_ARGUMENT);

	/* Succeeded: the unit carried the message out. */
	return 0;
}

/*
 * Marks what the unit shows when it stopped answering: its mailbox's
 * message, argument and answer words, its firmware's state word, and the
 * display hub's memory state word (whether the display lets the memory's
 * state change), for the message sent last.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu_cmn.c (__smu_cmn_reg_print_error: the
 * three mailbox words) and smu11/smu_v11_0.c (smu_v11_0_check_fw_status:
 * the firmware's state word).
 */
static void
smu_hang_mark(
	struct amdrdna_device *device,
	uint32_t message)
{
	uint32_t handshake[SMU_HANG_HANDSHAKES];
	unsigned sample;

	/* The mailbox, the firmware's state word and the hub's memory state. */
	amdrdna_mark("smu: hung at 0x%x: mailbox message 0x%x argument 0x%x answer 0x%x, firmware 0x%08x, display memory state 0x%08x",
	    (unsigned)message,
	    (unsigned)amdrdna_block_read(device, AMDRDNA_BLOCK_SMU, AMDRDNA_SMU_WINDOW_MAILBOX, AMDRDNA_SMU_REG_MESSAGE),
	    (unsigned)amdrdna_block_read(device, AMDRDNA_BLOCK_SMU, AMDRDNA_SMU_WINDOW_MAILBOX, AMDRDNA_SMU_REG_ARGUMENT),
	    (unsigned)amdrdna_block_read(device, AMDRDNA_BLOCK_SMU, AMDRDNA_SMU_WINDOW_MAILBOX, AMDRDNA_SMU_REG_RESPONSE),
	    (unsigned)amdrdna_far_read(device, AMDRDNA_SMU_FAR_FIRMWARE_STATE),
	    (unsigned)amdrdna_display_memory_state(device));

	/* The hub's memory state handshake, four times 5 ms apart (bit 31: the firmware asks; bit 30: the arbiter allows). */
	for (sample = 0U; sample < SMU_HANG_HANDSHAKES; sample++) {
		handshake[sample] = amdrdna_display_memory_handshake(device);
		kern_usleep_range(SMU_HANG_HANDSHAKE_US, SMU_HANG_HANDSHAKE_US + SMU_HANG_HANDSHAKE_US / 4U);
	}
	amdrdna_mark("smu: hung: display memory handshake 0x%08x 0x%08x 0x%08x 0x%08x",
	    (unsigned)handshake[0],
	    (unsigned)handshake[1],
	    (unsigned)handshake[2],
	    (unsigned)handshake[3]);
}

/* Waits for a nonzero answer word, each millisecond for 2 s; 0 when none came. */
static uint32_t
smu_wait_answer(
	struct amdrdna_device *device)
{
	uint32_t word;
	unsigned poll;

	/* Looks, then sleeps, until the word is set. */
	word = 0U;
	for (poll = 0U; poll < SMU_ANSWER_POLLS; poll++) {
		/* Stops at the answer. */
		word = amdrdna_block_read(device, AMDRDNA_BLOCK_SMU, AMDRDNA_SMU_WINDOW_MAILBOX, AMDRDNA_SMU_REG_RESPONSE);
		if (word != 0U)
			break;

		/* Sleeps one step. */
		kern_usleep_range(SMU_POLL_US, SMU_POLL_US + SMU_POLL_US / 4U);
	}

	/* The answer, or 0. */
	return word;
}

/* Reads one register of the thermal block. */
static uint32_t
thermal_read(
	struct amdrdna_device *device,
	uint32_t offset)
{
	uint32_t value;

	/* The registers are in the block's first window. */
	value = amdrdna_block_read(device, AMDRDNA_BLOCK_THM, AMDRDNA_THM_WINDOW_MAIN, offset);

	/* Succeeded: the register's value. */
	return value;
}

/* Writes one register of the thermal block. */
static void
thermal_write(
	struct amdrdna_device *device,
	uint32_t offset,
	uint32_t value)
{
	/* The registers are in the block's first window. */
	amdrdna_block_write(device, AMDRDNA_BLOCK_THM, AMDRDNA_THM_WINDOW_MAIN, offset, value);
}
