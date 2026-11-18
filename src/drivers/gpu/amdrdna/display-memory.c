/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display's part of the memory clock's levels (amdrdna.smu.memory=2):
 * what the system-management unit's firmware must know before it moves the
 * memory clock with the load while the display scans out, and what the
 * display hub must do so a change does not starve it.
 *
 * Stage A4 runs the memory clock's levels held at the highest, with the
 * hub allowing the memory's state change by force.  Once the display is
 * the driver's (stage A6, a mode set, the reset's stage A4), the display's
 * step tells the firmware the watermark set of each memory clock range,
 * that one display runs, and the floors of the display engine's clock the
 * display's plan counts on and of the display clock's voltage (Linux gives
 * Navi10's system clock none); works the display's plan out at the lowest
 * memory clock level with the latency of a change between levels (longer
 * than Linux's, MEMORY_SWITCH_TENTHS) and writes it (the hub then allows a
 * change only when its buffer covers it, in the vertical blank); lets the
 * hub's watermarks alone decide the memory's self refresh and state
 * change; and lowers the memory clock's floor to the lowest level, so the
 * firmware moves the clock with the load.  A mode the hub cannot cover a
 * change in keeps the memory clock at its highest level with the plan
 * worked out for the firmware's short pauses of the memory, as Linux does;
 * a plan that covers neither keeps the change allowed by force.  Before a
 * mode set the memory clock goes back to its highest level with the change
 * allowed by force, and at the shutdown and the reset the firmware is also
 * told no display runs.  The power watch counts and marks the display's
 * underflows after the step.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-private.h"
#include "drivers/gpu/amdrdna/amdrdna-regs.h"

/*
 * The messages the step sends: a clock's floor and ceiling (the clock in
 * the high 16 bits, MHz in the low 16), and how many displays run.
 *
 * Values referenced: Linux v6.12
 * drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu_v11_0_ppsmc.h
 * (PPSMC_MSG_SetHardMinByFreq, PPSMC_MSG_SetHardMaxByFreq,
 * PPSMC_MSG_NumOfDisplays) and
 * drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c
 * (smu_v11_0_set_hard_freq_limited_range, smu_v11_0_display_clock_voltage_request).
 */
#define MEMORY_MESSAGE_FLOOR		0x1cU
#define MEMORY_MESSAGE_CEILING		0x1dU
#define MEMORY_MESSAGE_DISPLAYS		0x40U

/*
 * The firmware's numbers of the clocks the step gives floors: the memory
 * clock, the display engine's clock and the display clock (whose floor
 * keeps the voltage the display's pipe needs); and the level index that
 * asks how many levels a clock has.
 *
 * Values referenced: Linux v6.12
 * drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu11_driver_if_navi10.h
 * (PPCLK_UCLK, PPCLK_DCEFCLK, PPCLK_DISPCLK) and
 * drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c
 * (smu_v11_0_get_dpm_level_count).
 */
#define MEMORY_CLOCK_MEMORY		2U
#define MEMORY_CLOCK_DISPLAY		5U
#define MEMORY_CLOCK_PIPE		6U
#define MEMORY_LEVEL_COUNT		0xffU

/*
 * The display's clocks at the lowest memory clock level: the display
 * engine's clock at least 506 MHz, the fabric 1.08 times
 * the memory clock and at least the same, and 16 transfers a memory
 * clock.  Then the latency of the firmware's short pause of the memory
 * while the memory clock is held (the temperature read of the GDDR6), in
 * tenths of a microsecond.
 *
 * Values referenced: Linux v6.12
 * drivers/gpu/drm/amd/display/dc/dml/dcn20/dcn20_fpu.c
 * (dcn20_update_bounding_box: min_dcfclk 506, the FCLK:UCLK ratio 1.08,
 * dram_speed_mts = UCLK * 16; dcn2_0_soc: dummy_pstate_latency_us 5.0;
 * dcn20_validate_bandwidth_fp: the dummy latency's fallback).
 */
#define MEMORY_DISPLAY_FLOOR_MHZ	506U
#define MEMORY_FABRIC_PER_MILLE		1080U
#define MEMORY_TRANSFERS		16U
#define MEMORY_PAUSE_TENTHS		50U

/*
 * The latency of a change between the memory clock's levels the display's
 * plan covers, in tenths of a microsecond: 520 us.  Linux plans 404 us,
 * but it splits a 1920-pixel plane over two pipes, whose buffers cover a
 * change in the active area with some 390 us to spare; one pipe covers it
 * only in the vertical blank with none.  Under a heavy load the firmware's
 * changes took longer than 404 us there (the timing generator underflowed
 * at 1 to 1.5 of the changes in 6 s of load, none in 8 such runs with 520
 * us), so the plan covers 520 us (the vertical blank of 1080p60 holds up
 * to some 575).  amdrdna.smu.memory_latency= sets another.
 *
 * Value referenced: Linux v6.12
 * drivers/gpu/drm/amd/display/dc/dml/dcn20/dcn20_fpu.c
 * (dcn2_0_soc: dram_clock_change_latency_us 404.0, the latency it replaces).
 */
#define MEMORY_SWITCH_TENTHS		5200U

/*
 * The memory clock's level the floor goes to while the graphics engine
 * works (the power side's boost): the second.  At the lowest level's floor
 * the firmware let the memory clock fall to the lowest level (100 MHz)
 * again and again under a heavy 3D scene that does not read much memory,
 * which made its frames some 0.8 ms longer; Linux's firmware kept 500 MHz
 * under the same scene, and so does this floor.
 */
#define MEMORY_BUSY_LEVEL		1U

/* The underflows of the display the watch marks in the log; it counts the rest. */
#define MEMORY_UNDERFLOW_MARKS		20U

/*
 * The output's FIFO status word (between the timing generator and the
 * transmitter's encoder): its absolute dword index for the first encoder
 * and the step to the next, the level error (bit 0, the FIFO ran empty or
 * full: the encoder's stream broke), its acknowledgement (bit 8), and the
 * lowest and highest levels since the last calibration (bits 25:22,
 * 20:16).
 *
 * Values referenced: Linux v6.12
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_offset.h
 * (mmDIG0_DIG_FIFO_STATUS 0x206e, mmDIG1_DIG_FIFO_STATUS 0x216e, base
 * index 2) with navi10_ip_offset.h (DCN_BASE 0x34c0), and
 * dcn_2_0_0_sh_mask.h (DIG0_DIG_FIFO_STATUS__DIG_FIFO_LEVEL_ERROR_MASK,
 * __DIG_FIFO_ERROR_ACK_MASK, __DIG_FIFO_MINIMUM_LEVEL_MASK,
 * __DIG_FIFO_MAXIMUM_LEVEL_MASK).
 */
#define MEMORY_FIFO_STATUS		0x552eU
#define MEMORY_FIFO_STEP		0x0100U
#define MEMORY_FIFO_ERROR		0x00000001U
#define MEMORY_FIFO_ACK			0x00000100U
#define MEMORY_FIFO_MIN_SHIFT		22U
#define MEMORY_FIFO_MIN_FIELD		0x0000000fU
#define MEMORY_FIFO_MAX_SHIFT		16U
#define MEMORY_FIFO_MAX_FIELD		0x0000001fU

static uint32_t memory_floor(struct amdrdna_device *device, uint32_t clock, uint32_t wanted_mhz, int *error);
static int memory_table_send(struct amdrdna_device *device);
static void memory_plan_use(struct amdrdna_device *device, uint32_t display_mhz, uint32_t memory_mhz, uint32_t switch_tenths);

/*
 * Runs the display's step (the file's comment) once the display is the
 * driver's and the memory clock's levels run with amdrdna.smu.memory=2;
 * otherwise does nothing.  Each part's answer is marked; a part that
 * fails leaves the memory clock held at its highest level.  The caller
 * holds the display's mode lock or runs the bring-up or the reset.
 *
 * Procedure referenced: Linux v6.12
 * drivers/gpu/drm/amd/display/dc/clk_mgr/dcn20/dcn20_clk_mgr.c
 * (dcn2_update_clocks: the display count, the display engine's clock's
 * floor, the change's support, the memory clock's floor, the display
 * clock's voltage (PP_SMU_NV_DISPCLK); Navi10's
 * pp_smu functions in amdgpu_dm/amdgpu_dm_pp_smu.c have no system clock
 * floor),
 * drivers/gpu/drm/amd/display/dc/dml/dcn20/dcn20_fpu.c
 * (dcn20_validate_bandwidth_fp: the full latency, then the dummy one with
 * the change unsupported), drivers/gpu/drm/amd/pm/swsmu/smu11/navi10_ppt.c
 * (navi10_display_disable_memory_clock_switch: the floor at the highest
 * level while the change is unsupported, at the display's level else) and
 * drivers/gpu/drm/amd/display/dc/hubbub/dcn20/dcn20_hubbub.c (no force on
 * the state change).
 */
void
amdrdna_memory_display_start(
	struct amdrdna_device *device)
{
	struct amdrdna_thermal *thermal;
	struct amdrdna_mode_plan plan;
	uint32_t word;
	uint32_t lowest;
	uint32_t highest;
	struct amdrdna_mode_plan_input clocks;
	uint32_t display_mhz;
	uint32_t pipe_mhz;
	uint32_t latency;
	uint32_t memory_floor_mhz;
	uint32_t floor_level;
	uint32_t busy_level;
	int table_error;
	int count_error;
	int display_error;
	int pipe_error;
	int ceiling_error;
	int plan_error;
	int floor_error;
	bool follows;
	bool released;

	/* Only with the firmware moving the memory clock, its levels read and the display the driver's. */
	thermal = &device->thermal;
	if (thermal->memory_mode != 2U || thermal->memory_level_count == 0U || !device->display.taken)
		return;
	if ((thermal->features_low & AMDRDNA_SMU_FEATURE_MEMORY_LEVELS) == 0U || thermal->hung)
		return;
	lowest = thermal->memory_levels_mhz[0];
	highest = thermal->memory_levels_mhz[thermal->memory_level_count - 1U];

	/* No plan yet. */
	plan = (struct amdrdna_mode_plan){ 0 };

	/* The change allowed by force, the self refresh by the watermarks, while the firmware is told. */
	word = amdrdna_display_memory_switch_begin(device);

	/* The watermark sets of the memory clock's ranges, one display, and the floors of the display's clocks. */
	table_error = memory_table_send(device);
	count_error = amdrdna_smu_message(device, MEMORY_MESSAGE_DISPLAYS, 1U, NULL);
	display_mhz = memory_floor(device, MEMORY_CLOCK_DISPLAY, MEMORY_DISPLAY_FLOOR_MHZ, &display_error);
	pipe_mhz = 0U;
	pipe_error = amdrdna_mode_clocks(device, &clocks);
	if (pipe_error == 0) {
		/* The display and pipe clocks' voltage: a floor at the faster of the two, in whole MHz. */
		pipe_mhz = (clocks.display_khz > clocks.pipe_khz ? clocks.display_khz : clocks.pipe_khz) + 999U;
		pipe_mhz /= 1000U;
		pipe_error = amdrdna_smu_message(device, MEMORY_MESSAGE_FLOOR, (MEMORY_CLOCK_PIPE << 16) | pipe_mhz, NULL);
	}
	thermal->memory_display = true;

	/* The plan at the lowest level with a change's latency; the plan at the highest with the pause's when the hub cannot cover a change. */
	follows = false;
	plan_error = EIO;
	if (table_error == 0 && count_error == 0) {
		/* The change's latency (MEMORY_SWITCH_TENTHS), or amdrdna.smu.memory_latency= in tenths of a microsecond. */
		latency = amdrdna_parameter_number("amdrdna.smu.memory_latency", MEMORY_SWITCH_TENTHS);
		memory_plan_use(device, display_mhz, lowest, latency);
		plan_error = amdrdna_mode_replan(device, false, &plan);
		follows = plan_error == 0 && plan.memory_switch != 0U;
		if (!follows) {
			memory_plan_use(device, display_mhz, highest, MEMORY_PAUSE_TENTHS);
			plan_error = amdrdna_mode_replan(device, false, &plan);
		}
		if (plan_error == 0)
			plan_error = amdrdna_mode_replan(device, true, &plan);
	}

	/* The watermarks decide the self refresh and the change when the plan covers a change; else the change stays allowed by force. */
	released = false;
	if (plan_error == 0 && plan.memory_switch != 0U) {
		amdrdna_display_memory_switch_release(device, word);
		released = true;
	} else {
		amdrdna_display_memory_switch_hold(device, word);
		follows = false;
	}

	/* The memory clock's floor: its lowest level when the firmware may move it (amdrdna.smu.memory_follow=0 keeps it held), else its highest. */
	if (follows && amdrdna_parameter_number("amdrdna.smu.memory_follow", 1U) == 0U)
		follows = false;
	memory_floor_mhz = highest;
	if (follows) {
		/* The lowest level, or the level amdrdna.smu.memory_floor_level= names (for the display's checks). */
		floor_level = amdrdna_parameter_number("amdrdna.smu.memory_floor_level", 0U);
		if (floor_level >= thermal->memory_level_count)
			floor_level = thermal->memory_level_count - 1U;
		memory_floor_mhz = thermal->memory_levels_mhz[floor_level];
	}
	floor_error = amdrdna_smu_message(device, MEMORY_MESSAGE_FLOOR, (MEMORY_CLOCK_MEMORY << 16) | memory_floor_mhz, NULL);
	thermal->memory_follows = follows && floor_error == 0;

	/* The floor while the graphics engine works: the level amdrdna.smu.memory_busy_level= names (MEMORY_BUSY_LEVEL, 0 for none). */
	busy_level = amdrdna_parameter_number("amdrdna.smu.memory_busy_level", MEMORY_BUSY_LEVEL);
	if (busy_level >= thermal->memory_level_count)
		busy_level = thermal->memory_level_count - 1U;
	thermal->memory_idle_floor_mhz = memory_floor_mhz;
	thermal->memory_busy_floor_mhz = thermal->memory_levels_mhz[busy_level];

	/* amdrdna.smu.memory_pin=1 keeps the memory clock at the floor's level (the ceiling there too, for the display's checks). */
	ceiling_error = 0;
	if (thermal->memory_follows && amdrdna_parameter_number("amdrdna.smu.memory_pin", 0U) != 0U)
		ceiling_error = amdrdna_smu_message(device, MEMORY_MESSAGE_CEILING, (MEMORY_CLOCK_MEMORY << 16) | memory_floor_mhz, NULL);

	/* Marks each part's answer and the plan. */
	amdrdna_mark("memory display: table %d, displays %d, display clock floor %uMHz (%d), plan %d switch=%u watermark=%uns",
	    table_error,
	    count_error,
	    (unsigned)display_mhz,
	    display_error,
	    plan_error,
	    plan_error == 0 ? plan.memory_switch : 0U,
	    plan_error == 0 ? (unsigned)plan.switch_ns : 0U);
	amdrdna_mark("memory display: plan blank use %u start %u lines, prefetch %u/4, blank ttu %u, urgent %uns, refresh %u/%uns, trip %uns",
	    plan.vblank_use,
	    (unsigned)plan.start_lines,
	    (unsigned)plan.prefetch_quarters,
	    (unsigned)plan.vblank_ttu_cycles,
	    (unsigned)plan.urgent_ns,
	    (unsigned)plan.refresh_enter_exit_ns,
	    (unsigned)plan.refresh_exit_ns,
	    (unsigned)plan.trip_ns);
	amdrdna_mark("memory display: display clock voltage floor %uMHz (%d), forces %s, memory clock floor %uMHz (%d, ceiling %d), %s",
	    (unsigned)pipe_mhz,
	    pipe_error,
	    released ? "off" : "change allowed",
	    (unsigned)memory_floor_mhz,
	    floor_error,
	    ceiling_error,
	    thermal->memory_follows ? "follows the load" : "held");
}

/*
 * Holds the memory clock at its highest level with the change allowed by
 * force (before a mode set); the firmware still counts the display.  The
 * display's plan is worked out at the board's clocks until the step runs
 * again.  Does nothing before the display's step ran.
 */
void
amdrdna_memory_display_hold(
	struct amdrdna_device *device)
{
	struct amdrdna_thermal *thermal;
	uint32_t highest;
	int error;

	/* Only after the display's step. */
	thermal = &device->thermal;
	if (!thermal->memory_display)
		return;
	highest = thermal->memory_levels_mhz[thermal->memory_level_count - 1U];

	/* The change allowed by force, then the ceiling and the floor at the highest level. */
	(void)amdrdna_display_memory_switch_begin(device);
	error = amdrdna_smu_message(device, MEMORY_MESSAGE_CEILING, (MEMORY_CLOCK_MEMORY << 16) | highest, NULL);
	if (error == 0)
		error = amdrdna_smu_message(device, MEMORY_MESSAGE_FLOOR, (MEMORY_CLOCK_MEMORY << 16) | highest, NULL);
	if (error != 0)
		amdrdna_mark("memory display: the hold's floor refused (%d)", error);
	thermal->memory_follows = false;
	thermal->memory_plan_set = false;
}

/*
 * Ends the display's step (the shutdown, the reset): the memory clock held
 * at its highest level with the change allowed by force, and the firmware
 * told no display runs.  Does nothing before the display's step ran.
 *
 * Procedure referenced: Linux v6.12
 * drivers/gpu/drm/amd/display/dc/clk_mgr/dcn20/dcn20_clk_mgr.c
 * (dcn2_update_clocks: the display count 0 without a stream).
 */
void
amdrdna_memory_display_end(
	struct amdrdna_device *device)
{
	struct amdrdna_thermal *thermal;
	int error;

	/* Only after the display's step. */
	thermal = &device->thermal;
	if (!thermal->memory_display)
		return;

	/* Held at the highest level, then no display counted. */
	amdrdna_memory_display_hold(device);
	error = amdrdna_smu_message(device, MEMORY_MESSAGE_DISPLAYS, 0U, NULL);
	thermal->memory_display = false;
	amdrdna_mark("memory display: ended, held at the highest level, displays 0 (%d)", error);
}

/*
 * Gives a display plan's input the clocks the display's step works with
 * (the display engine's clock, the fabric, the memory's transfers and the
 * change's latency) in place of the board's, while the step has them.
 */
void
amdrdna_memory_plan_clocks(
	struct amdrdna_device *device,
	struct amdrdna_mode_plan_input *input)
{
	struct amdrdna_thermal *thermal;

	/* Only while the step has its clocks. */
	thermal = &device->thermal;
	if (!thermal->memory_plan_set)
		return;

	/* The display engine's clock when its floor was set, then the fabric, the transfers and the latency. */
	if (thermal->memory_plan_return_khz != 0U)
		input->return_khz = thermal->memory_plan_return_khz;
	input->fabric_khz = thermal->memory_plan_fabric_khz;
	input->memory_mts = thermal->memory_plan_mts;
	input->switch_tenths = thermal->memory_plan_switch_tenths;
}

/*
 * Moves the memory clock's floor with the graphics engine's boost (the
 * power watch, raising when work starts and lowering a second after it
 * ended): the busy floor while it works, the idle floor after.  Does
 * nothing unless the firmware moves the memory clock with a busy floor
 * above the idle one.
 */
void
amdrdna_memory_display_busy(
	struct amdrdna_device *device,
	bool raising)
{
	struct amdrdna_thermal *thermal;
	uint32_t mhz;
	int error;

	/* Only while the firmware moves the memory clock and the busy floor is higher. */
	thermal = &device->thermal;
	if (!thermal->memory_follows || thermal->memory_busy_floor_mhz <= thermal->memory_idle_floor_mhz)
		return;

	/* The floor for the engine's state. */
	mhz = thermal->memory_idle_floor_mhz;
	if (raising)
		mhz = thermal->memory_busy_floor_mhz;
	error = amdrdna_smu_message(device, MEMORY_MESSAGE_FLOOR, (MEMORY_CLOCK_MEMORY << 16) | mhz, NULL);
	if (error != 0)
		amdrdna_mark("memory display: the busy floor %uMHz refused (%d)", (unsigned)mhz, error);
}

/*
 * Looks for the display's underflow and the output FIFO's errors once a
 * second after the display's step (the power watch), except while a mode
 * set holds the memory clock, or without the step when the boot line asks
 * (amdrdna.smu.display_watch=1): an underflow of the plane or
 * the timing generator is counted, cleared, and marked with the memory
 * clock the last readings show (the first MEMORY_UNDERFLOW_MARKS of them).
 */
void
amdrdna_memory_display_watch(
	struct amdrdna_device *device)
{
	struct amdrdna_thermal *thermal;
	uint32_t underflow;
	uint32_t timing_underflow;
	uint32_t word;
	uint32_t at;

	/* Only after the display's step, not while a mode set holds the memory clock; without the step only when amdrdna.smu.display_watch=1. */
	thermal = &device->thermal;
	if (!device->display.taken)
		return;
	if (thermal->memory_mode == 2U && (!thermal->memory_display || !thermal->memory_plan_set))
		return;
	if (thermal->memory_mode != 2U && thermal->display_watch == 0U)
		return;

	/* The output FIFO's level error, acknowledged once read. */
	if (device->gop.has_output) {
		at = MEMORY_FIFO_STATUS + device->gop.output * MEMORY_FIFO_STEP;
		word = amdrdna_read(device, at);
		if ((word & MEMORY_FIFO_ERROR) != 0U) {
			amdrdna_write(device, at, word | MEMORY_FIFO_ACK);
			thermal->memory_fifo_errors++;
			if (thermal->memory_fifo_errors <= MEMORY_UNDERFLOW_MARKS)
				amdrdna_mark("memory display: output fifo error 0x%08x levels %u-%u (%llu so far), memory clock %uMHz",
				    (unsigned)word,
				    (unsigned)((word >> MEMORY_FIFO_MIN_SHIFT) & MEMORY_FIFO_MIN_FIELD),
				    (unsigned)((word >> MEMORY_FIFO_MAX_SHIFT) & MEMORY_FIFO_MAX_FIELD),
				    (unsigned long long)thermal->memory_fifo_errors,
				    thermal->memory_clock);
		}
	}

	/* The underflow bits, cleared once read. */
	underflow = amdrdna_display_underflow_take(device, &timing_underflow);
	if (underflow == 0U && timing_underflow == 0U)
		return;

	/* Counted, and marked while there are few. */
	thermal->memory_underflows++;
	if (thermal->memory_underflows <= MEMORY_UNDERFLOW_MARKS)
		amdrdna_mark("memory display: underflow %u/%u (%llu so far), memory clock %uMHz",
		    (unsigned)underflow,
		    (unsigned)timing_underflow,
		    (unsigned long long)thermal->memory_underflows,
		    thermal->memory_clock);
}

/*
 * Gives a clock a floor of wanted_mhz, within its lowest and highest
 * level.  Returns the floor given in MHz, or 0 with *error set when the
 * levels were not read or the firmware refused it.
 */
static uint32_t
memory_floor(
	struct amdrdna_device *device,
	uint32_t clock,
	uint32_t wanted_mhz,
	int *error)
{
	uint32_t count;
	uint32_t lowest;
	uint32_t highest;
	uint32_t mhz;

	/* The clock's levels: how many, the lowest and the highest. */
	count = 0U;
	*error = amdrdna_smu_clock_level(device, clock, MEMORY_LEVEL_COUNT, &count);
	if (*error == 0 && count == 0U)
		*error = EIO;
	if (*error != 0)
		return 0U;
	*error = amdrdna_smu_clock_level(device, clock, 0U, &lowest);
	if (*error != 0)
		return 0U;
	*error = amdrdna_smu_clock_level(device, clock, count - 1U, &highest);
	if (*error != 0)
		return 0U;

	/* The floor within the levels. */
	mhz = wanted_mhz;
	if (mhz < lowest)
		mhz = lowest;
	if (mhz > highest)
		mhz = highest;

	/* The floor. */
	*error = amdrdna_smu_message(device, MEMORY_MESSAGE_FLOOR, (clock << 16) | mhz, NULL);
	if (*error != 0)
		return 0U;

	/* Succeeded: the floor given. */
	return mhz;
}

/* Puts the watermark table of the memory clock's levels together and hands it to the firmware. */
static int
memory_table_send(
	struct amdrdna_device *device)
{
	uint8_t table[AMDRDNA_SMU_WATERMARKS_BYTES];
	int error;

	/* The table. */
	amdrdna_power_watermarks_build(device->thermal.memory_levels_mhz, device->thermal.memory_level_count, table);

	/* Handed over. */
	error = amdrdna_smu_table_send(device, AMDRDNA_SMU_WATERMARKS_TABLE, table, sizeof(table));
	if (error != 0)
		return error;

	/* Succeeded: the firmware has the table. */
	return 0;
}

/*
 * Has the display's plan worked out with the display engine's clock at
 * display_mhz (0: the board's), the memory clock at memory_mhz with its
 * fabric, and the change's latency switch_tenths (0: a change between
 * levels).
 */
static void
memory_plan_use(
	struct amdrdna_device *device,
	uint32_t display_mhz,
	uint32_t memory_mhz,
	uint32_t switch_tenths)
{
	struct amdrdna_thermal *thermal;
	uint32_t fabric_mhz;

	/* The fabric: 1.08 times the memory clock, at least the display's floor. */
	fabric_mhz = (uint32_t)((uint64_t)memory_mhz * MEMORY_FABRIC_PER_MILLE / 1000U);
	if (fabric_mhz < MEMORY_DISPLAY_FLOOR_MHZ)
		fabric_mhz = MEMORY_DISPLAY_FLOOR_MHZ;

	/* The clocks and the latency. */
	thermal = &device->thermal;
	thermal->memory_plan_return_khz = display_mhz * 1000U;
	thermal->memory_plan_fabric_khz = fabric_mhz * 1000U;
	thermal->memory_plan_mts = memory_mhz * MEMORY_TRANSFERS;
	thermal->memory_plan_switch_tenths = switch_tenths;
	thermal->memory_plan_set = true;
}
