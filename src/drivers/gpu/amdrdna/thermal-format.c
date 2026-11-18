/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The words of the GPU's thermal block and the tables of its power side:
 * the temperature the block reads, the fan controller's duty and mode, the
 * fan's speed from its tachometer, the thresholds of the thermal alerts,
 * the board's power table, the averaging settings and the watermark table
 * the system-management unit's firmware is handed, and the readings it
 * hands back.
 *
 * Nothing here touches the hardware: every function turns words or tables
 * read from the GPU, the video BIOS or a firmware file into numbers, or
 * numbers into words and tables to write, so the same code runs in the
 * kernel and in the host tests.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-hw.h"

/*
 * The temperature word: the highest reading in bits 8:0 and the reading
 * the trip compares in bits 17:9, both in degrees Celsius.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/thm/thm_11_0_2_sh_mask.h
 * (CG_MULT_THERMAL_STATUS__ASIC_MAX_TEMP_MASK,
 * CG_MULT_THERMAL_STATUS__CTF_TEMP_MASK,
 * CG_MULT_THERMAL_STATUS__CTF_TEMP__SHIFT).
 */
#define FORMAT_TEMPERATURE_HIGHEST_MASK		0x000001ffU
#define FORMAT_TEMPERATURE_SENSOR_MASK		0x0003fe00U
#define FORMAT_TEMPERATURE_SENSOR_SHIFT		9U

/*
 * The fan controller's words: the static duty in bits 7:0 of the duty
 * word, the duty that stands for 100 % in bits 7:0 of the limits word,
 * the lowest temperature in bits 7:0 and the mode in bits 13:11 of the
 * mode word (1 drives the static duty), and the duty actually driven in
 * bits 16:9 of the status word.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/thm/thm_11_0_2_sh_mask.h
 * (CG_FDO_CTRL0__FDO_STATIC_DUTY_MASK, CG_FDO_CTRL1__FMAX_DUTY100_MASK,
 * CG_FDO_CTRL2__TMIN_MASK, CG_FDO_CTRL2__FDO_PWM_MODE_MASK,
 * CG_FDO_CTRL2__FDO_PWM_MODE__SHIFT, CG_THERMAL_STATUS__FDO_PWM_DUTY_MASK,
 * CG_THERMAL_STATUS__FDO_PWM_DUTY__SHIFT) and
 * drivers/gpu/drm/amd/pm/powerplay/hwmgr/vega10_thermal.h
 * (FDO_PWM_MODE_STATIC, the value smu_v11_0.c writes).
 */
#define FORMAT_FAN_DUTY_MASK			0x000000ffU
#define FORMAT_FAN_FULL_MASK			0x000000ffU
#define FORMAT_FAN_LOWEST_MASK			0x000000ffU
#define FORMAT_FAN_MODE_MASK			0x00003800U
#define FORMAT_FAN_MODE_SHIFT			11U
#define FORMAT_FAN_MODE_STATIC			1U
#define FORMAT_FAN_DRIVEN_MASK			0x0001fe00U
#define FORMAT_FAN_DRIVEN_SHIFT			9U

/*
 * The tachometer counts the crystal's 25 MHz ticks of one fan period; the
 * speed in turns per minute is 25 MHz times 60 over the count.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c
 * (smu_v11_0_get_fan_speed_rpm: 2500 in 10 kHz units, times 60 and
 * 10000).
 */
#define FORMAT_TACHOMETER_TICKS_PER_MINUTE	1500000000ULL

/*
 * The alert control word: the high threshold in bits 7:0, the low one in
 * bits 15:8, the masks of the high, low and trigger alerts in bits 24, 25
 * and 26, the switch that sends the alerts to the interrupt controller in
 * bit 28 and the controller credits in bits 31:29 (5); and the enable
 * word's bits that clear the three alerts (3, 4 and 5).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/thm/thm_11_0_2_sh_mask.h
 * (THM_THERMAL_INT_CTRL__DIG_THERM_INTH_MASK,
 * THM_THERMAL_INT_CTRL__DIG_THERM_INTL_MASK,
 * THM_THERMAL_INT_CTRL__THERM_INTH_MASK_MASK,
 * THM_THERMAL_INT_CTRL__THERM_INTL_MASK_MASK,
 * THM_THERMAL_INT_CTRL__THERM_TRIGGER_MASK_MASK,
 * THM_THERMAL_INT_CTRL__THERM_IH_HW_ENA_MASK,
 * THM_THERMAL_INT_CTRL__MAX_IH_CREDIT_MASK,
 * THM_THERMAL_INT_ENA__THERM_INTH_CLR_MASK,
 * THM_THERMAL_INT_ENA__THERM_INTL_CLR_MASK,
 * THM_THERMAL_INT_ENA__THERM_TRIGGER_CLR_MASK) and
 * drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c (smu_v11_0_set_irq_state:
 * 5 credits).
 */
#define FORMAT_ALERT_HIGH_MASK			0x000000ffU
#define FORMAT_ALERT_LOW_MASK			0x0000ff00U
#define FORMAT_ALERT_LOW_SHIFT			8U
#define FORMAT_ALERT_HIGH_MASKED		0x01000000U
#define FORMAT_ALERT_LOW_MASKED			0x02000000U
#define FORMAT_ALERT_TRIGGER_MASKED		0x04000000U
#define FORMAT_ALERT_TO_CONTROLLER		0x10000000U
#define FORMAT_ALERT_CREDITS_MASK		0xe0000000U
#define FORMAT_ALERT_CREDITS			0xa0000000U
#define FORMAT_ALERT_CLEAR_ALL			0x00000038U

/*
 * The board's power table in the video BIOS (and in the firmware file):
 * its whole length, where the kind of thermal controller and the shutdown
 * temperature are, and where the part the unit's firmware takes starts.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/inc/smu_v11_0_pptable.h
 * (struct smu_11_0_powerplay_table, packed: thermal_controller_type,
 * software_shutdown_temp, smc_pptable, and its size).
 */
#define FORMAT_POWER_BYTES			1674U
#define FORMAT_POWER_CONTROLLER_AT		21U
#define FORMAT_POWER_SHUTDOWN_AT		32U
#define FORMAT_POWER_UNIT_PART_AT		482U

/*
 * The table the unit's firmware takes: the features it is to run (two
 * words), the edge, hotspot and memory limits in degrees Celsius, and the
 * board's part (from the I2C controllers through the board's reserved
 * words), which the board table replaces.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu11_driver_if_navi10.h
 * (PPTable_t, packed: FeaturesToRun, TedgeLimit, ThotspotLimit, TmemLimit,
 * I2cControllers through BoardReserved).
 */
#define FORMAT_UNIT_FEATURES_AT			4U
#define FORMAT_UNIT_EDGE_AT			52U
#define FORMAT_UNIT_HOTSPOT_AT			54U
#define FORMAT_UNIT_MEMORY_AT			56U
#define FORMAT_UNIT_BOARD_AT			964U
#define FORMAT_UNIT_BOARD_BYTES			196U

/*
 * The board table in the video BIOS: the one layout Navi 10 and Navi 14
 * use (format 4, content 5), its length, and where its board part starts
 * (right after the common header).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/atomfirmware.h
 * (struct atom_smc_dpm_info_v4_5: I2cControllers, and its size) and
 * drivers/gpu/drm/amd/pm/swsmu/smu11/navi10_ppt.c
 * (navi10_append_powerplay_table: format 4, content 5).
 */
#define FORMAT_BOARD_FORMAT			4U
#define FORMAT_BOARD_CONTENT			5U
#define FORMAT_BOARD_BYTES			200U
#define FORMAT_BOARD_PART_AT			4U

/*
 * The firmware file of the system-management unit: the header's layout
 * version, and after the common header and the program's start address,
 * either one power table (layout 2.0: its offset and size) or a list of
 * them (layout 2.1: the count and the list's offset; each entry is an ID,
 * an offset and a size).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h
 * (struct common_firmware_header, struct smc_firmware_header_v2_0,
 * struct smc_firmware_header_v2_1, struct smc_soft_pptable_entry).
 */
#define FORMAT_FILE_MAJOR_AT			8U
#define FORMAT_FILE_MINOR_AT			10U
#define FORMAT_FILE_TABLES_MAJOR		2U
#define FORMAT_FILE_ONE_TABLE			0U
#define FORMAT_FILE_TABLE_LIST			1U
#define FORMAT_FILE_FIRST_AT			36U
#define FORMAT_FILE_SECOND_AT			40U
#define FORMAT_FILE_HEADER_BYTES		44U
#define FORMAT_ENTRY_IDENTITY_AT		0U
#define FORMAT_ENTRY_OFFSET_AT			4U
#define FORMAT_ENTRY_BYTES_AT			8U
#define FORMAT_ENTRY_BYTES			12U

/*
 * The readings the unit's firmware hands back: the edge, hotspot and
 * memory temperatures (degrees Celsius) and the fan's speed (turns per
 * minute).  The older layout of the table puts them at the same places.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu11_driver_if_navi10.h
 * (SmuMetrics_t and SmuMetrics_legacy_t: TemperatureEdge,
 * TemperatureHotspot, TemperatureMem, CurrFanSpeed).
 */
#define FORMAT_READING_EDGE_AT			34U
#define FORMAT_READING_HOTSPOT_AT		36U
#define FORMAT_READING_MEMORY_AT		38U
#define FORMAT_READING_FAN_AT			62U

/*
 * The clocks in the same readings, in megahertz: the graphics, system and
 * memory clocks as they run now (the first three of the table's clocks),
 * the display's data return clock and the display clock (its sixth and
 * seventh), the graphics clock's average, and the graphics engine's average
 * activity in percent.  The older layout puts them at the same places.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu11_driver_if_navi10.h
 * (SmuMetrics_t and SmuMetrics_legacy_t: CurrClock with PPCLK_GFXCLK,
 * PPCLK_SOCCLK, PPCLK_UCLK, PPCLK_DCEFCLK and PPCLK_DISPCLK,
 * AverageGfxclkFrequencyPostDs and
 * AverageGfxclkFrequency, AverageGfxActivity).
 */
#define FORMAT_READING_GFX_CLOCK_AT		0U
#define FORMAT_READING_SOC_CLOCK_AT		2U
#define FORMAT_READING_MEMORY_CLOCK_AT		4U
#define FORMAT_READING_RETURN_CLOCK_AT		10U
#define FORMAT_READING_DISPLAY_CLOCK_AT		12U
#define FORMAT_READING_GFX_AVERAGE_AT		18U
#define FORMAT_READING_GFX_ACTIVITY_AT		24U

/*
 * The averaging settings: six 16-bit time constants from the start of the
 * table, one per average; the rest of the table is the firmware's own and
 * is handed over as zero.
 *
 * Value referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu11_driver_if_navi10.h
 * (DriverSmuConfig_t: GfxclkAverageLpfTau through SocketPowerLpfTau).
 */
#define FORMAT_SETTINGS_AVERAGES		6U

/*
 * A watermark row's fields: the clock's lowest and highest and the memory
 * clock's lowest and highest (16-bit, MHz), the hub's set (a byte), and
 * the top of a range that has none.
 *
 * Values referenced: Linux v6.12
 * drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu11_driver_if_navi10.h
 * (WatermarkRowGeneric_t) and
 * drivers/gpu/drm/amd/display/dc/dm_pp_smu.h
 * (PP_SMU_WM_SET_RANGE_CLK_UNCONSTRAINED_MAX).
 */
#define FORMAT_WATERMARK_MIN_CLOCK		0U
#define FORMAT_WATERMARK_MAX_CLOCK		2U
#define FORMAT_WATERMARK_MIN_MEMORY		4U
#define FORMAT_WATERMARK_MAX_MEMORY		6U
#define FORMAT_WATERMARK_SET			8U
#define FORMAT_WATERMARK_ANY			0xffffU

static void format_put16(uint8_t *at, uint32_t value);

/*
 * Reads the temperature word: the reading the trip compares (the one the
 * driver watches) and the highest reading, in degrees Celsius.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/powerplay/hwmgr/vega10_thermal.c
 * (vega10_thermal_get_temperature: the trip's reading is the temperature).
 */
void
amdrdna_temperature_read(
	uint32_t word,
	unsigned *sensor,
	unsigned *highest)
{
	/* The two fields. */
	*sensor = (word & FORMAT_TEMPERATURE_SENSOR_MASK) >> FORMAT_TEMPERATURE_SENSOR_SHIFT;
	*highest = word & FORMAT_TEMPERATURE_HIGHEST_MASK;
}

/*
 * Turns the fan controller's duty word into one with a static duty of a
 * percentage of the controller's full duty.
 *
 * Returns 0 with the word, or EINVAL when the limits word gives no full
 * duty (the controller cannot be driven) or the percentage is over 100.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c
 * (smu_v11_0_set_fan_speed_pwm: a 0 to 255 speed times the full duty over
 * 255).
 */
int
amdrdna_fan_duty_word(
	uint32_t old,
	uint32_t limits,
	unsigned percent,
	uint32_t *word)
{
	uint32_t full;
	uint32_t speed;
	uint32_t duty;

	/* The duty that stands for 100 %. */
	full = limits & FORMAT_FAN_FULL_MASK;
	if (full == 0U)
		return EINVAL;

	/* A percentage over 100 is refused. */
	if (percent > 100U)
		return EINVAL;

	/* The percentage as a 0 to 255 speed, then as the controller's duty. */
	speed = (uint32_t)(percent * 255U / 100U);
	duty = speed * full / 255U;

	/* Replaces the static duty, leaving the other bits. */
	*word = (old & ~FORMAT_FAN_DUTY_MASK) | (duty & FORMAT_FAN_DUTY_MASK);

	/* Succeeded: the word drives the duty asked for. */
	return 0;
}

/*
 * Turns the fan controller's mode word into one that drives the static
 * duty with no lowest temperature.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c
 * (smu_v11_0_set_fan_static_mode).
 */
uint32_t
amdrdna_fan_mode_word(
	uint32_t old)
{
	uint32_t word;

	/* No lowest temperature, and the static mode. */
	word = old & ~FORMAT_FAN_LOWEST_MASK;
	word &= ~FORMAT_FAN_MODE_MASK;
	word |= FORMAT_FAN_MODE_STATIC << FORMAT_FAN_MODE_SHIFT;

	/* Succeeded: the new mode word. */
	return word;
}

/*
 * Reports the fan's speed in turns per minute from its tachometer word;
 * a zero count (no period measured) reads as a stopped fan.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c
 * (smu_v11_0_get_fan_speed_rpm).
 */
uint32_t
amdrdna_fan_speed(
	uint32_t tachometer)
{
	uint64_t speed;

	/* No period means no speed to report. */
	if (tachometer == 0U)
		return 0U;

	/* The ticks of one minute over the ticks of one period. */
	speed = FORMAT_TACHOMETER_TICKS_PER_MINUTE / tachometer;

	/* Succeeded: the speed. */
	return (uint32_t)speed;
}

/*
 * Reports the duty the fan is driven at, in percent of the full duty,
 * from the status word and the limits word.  A controller without a full
 * duty reads as 0.
 */
unsigned
amdrdna_fan_driven(
	uint32_t status,
	uint32_t limits)
{
	uint32_t full;
	uint32_t duty;

	/* Without a full duty there is no percentage. */
	full = limits & FORMAT_FAN_FULL_MASK;
	if (full == 0U)
		return 0U;

	/* The duty driven, as a percentage, rounded to the nearest. */
	duty = (status & FORMAT_FAN_DRIVEN_MASK) >> FORMAT_FAN_DRIVEN_SHIFT;

	/* Succeeded: the percentage. */
	return (unsigned)((duty * 100U + full / 2U) / full);
}

/*
 * Turns the alert control word into one that sends the high and low
 * alerts to the interrupt controller at the thresholds given (degrees
 * Celsius, 0 to 255), with the trigger alert unmasked too.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c (smu_v11_0_set_irq_state,
 * the enable case).
 */
uint32_t
amdrdna_alert_word(
	uint32_t old,
	unsigned high,
	unsigned low)
{
	uint32_t word;

	/* Thresholds over 255 degrees stop at 255. */
	if (high > 255U)
		high = 255U;
	if (low > 255U)
		low = 255U;

	/* The credits and the switch to the controller, the high and low alerts unmasked. */
	word = old & ~FORMAT_ALERT_CREDITS_MASK;
	word |= FORMAT_ALERT_CREDITS;
	word |= FORMAT_ALERT_TO_CONTROLLER;
	word &= ~FORMAT_ALERT_HIGH_MASKED;
	word &= ~FORMAT_ALERT_LOW_MASKED;
	word &= ~FORMAT_ALERT_TRIGGER_MASKED;

	/* The two thresholds. */
	word &= ~(FORMAT_ALERT_HIGH_MASK | FORMAT_ALERT_LOW_MASK);
	word |= (uint32_t)high;
	word |= (uint32_t)low << FORMAT_ALERT_LOW_SHIFT;

	/* Succeeded: the new control word. */
	return word;
}

/*
 * Turns the alert control word into one with the high and low alerts
 * masked.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c (smu_v11_0_set_irq_state,
 * the disable case).
 */
uint32_t
amdrdna_alert_off_word(
	uint32_t old)
{
	uint32_t word;

	/* Both alerts masked. */
	word = old | FORMAT_ALERT_HIGH_MASKED | FORMAT_ALERT_LOW_MASKED;

	/* Succeeded: the new control word. */
	return word;
}

/*
 * Reports the enable word that clears the high, low and trigger alerts.
 */
uint32_t
amdrdna_alert_clear_word(void)
{
	/* The three clear bits. */
	return FORMAT_ALERT_CLEAR_ALL;
}

/*
 * Finds the board's power table a firmware file of the system-management
 * unit carries in place of the video BIOS's.  Layout 2.0 carries one
 * table, whatever the ID; layout 2.1 carries a list, and the entry with
 * the ID is taken.
 *
 * Returns 0 with the table's offset and size in the file, ENOENT when the
 * file carries no table (an older layout) or none with the ID, or EINVAL
 * for a file whose header or list does not lie inside it, or whose table
 * would run past its end.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c
 * (smu_v11_0_setup_pptable, smu_v11_0_set_pptable_v2_0,
 * smu_v11_0_set_pptable_v2_1).
 */
int
amdrdna_power_table_in_firmware(
	const uint8_t *file,
	size_t length,
	uint32_t identity,
	size_t *offset,
	size_t *bytes)
{
	uint16_t major;
	uint16_t minor;
	uint32_t count;
	uint32_t list;
	uint32_t index;
	size_t entry;
	uint32_t entry_identity;
	uint32_t found_offset;
	uint32_t found_bytes;
	bool found;

	/* A file shorter than the header with the tables carries none. */
	if (length < FORMAT_FILE_HEADER_BYTES)
		return EINVAL;

	/* Only layout 2 carries tables. */
	major = amdrdna_le16(file, FORMAT_FILE_MAJOR_AT);
	minor = amdrdna_le16(file, FORMAT_FILE_MINOR_AT);
	if (major != FORMAT_FILE_TABLES_MAJOR)
		return ENOENT;

	/* Takes the one table of layout 2.0, or the list's entry with the ID. */
	found = false;
	found_offset = 0U;
	found_bytes = 0U;
	if (minor == FORMAT_FILE_ONE_TABLE) {
		/* The one table, whatever the ID asked for. */
		found_offset = amdrdna_le32(file, FORMAT_FILE_FIRST_AT);
		found_bytes = amdrdna_le32(file, FORMAT_FILE_SECOND_AT);
		found = true;
	} else if (minor == FORMAT_FILE_TABLE_LIST) {
		/* The list must lie inside the file. */
		count = amdrdna_le32(file, FORMAT_FILE_FIRST_AT);
		list = amdrdna_le32(file, FORMAT_FILE_SECOND_AT);
		if ((uint64_t)list + (uint64_t)count * FORMAT_ENTRY_BYTES > length)
			return EINVAL;

		/* Looks for the entry with the ID. */
		for (index = 0U; index < count; index++) {
			/* Stops at the first entry with the ID. */
			entry = (size_t)list + (size_t)index * FORMAT_ENTRY_BYTES;
			entry_identity = amdrdna_le32(file, entry + FORMAT_ENTRY_IDENTITY_AT);
			if (entry_identity == identity) {
				found_offset = amdrdna_le32(file, entry + FORMAT_ENTRY_OFFSET_AT);
				found_bytes = amdrdna_le32(file, entry + FORMAT_ENTRY_BYTES_AT);
				found = true;
				break;
			}
		}
	} else {
		/* A layout 2 the driver does not know. */
		return EINVAL;
	}

	/* No entry has the ID. */
	if (!found)
		return ENOENT;

	/* The table must lie inside the file. */
	if ((uint64_t)found_offset + (uint64_t)found_bytes > length)
		return EINVAL;

	/* Succeeded: the table lies at the offset. */
	*offset = found_offset;
	*bytes = found_bytes;
	return 0;
}

/*
 * Puts together the table the unit's firmware takes: the firmware's part
 * of the board's power table (from the video BIOS or the firmware file),
 * with its board part replaced by the video BIOS's board table, and reads
 * the temperature limits it carries.
 *
 * Returns 0 with the table (AMDRDNA_SMU_POWER_TABLE_BYTES bytes) and the
 * limits, or EINVAL when the power table is too short or the board table
 * is missing, too short or of a layout the driver does not know.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu11/navi10_ppt.c
 * (navi10_setup_pptable, navi10_store_powerplay_table,
 * navi10_append_powerplay_table: the board part is copied through the
 * board's reserved words, and navi10_get_thermal_temperature_range).
 */
int
amdrdna_power_table_build(
	const uint8_t *power,
	size_t power_bytes,
	const uint8_t *board,
	size_t board_bytes,
	unsigned board_format,
	unsigned board_content,
	uint8_t *table,
	struct amdrdna_power_limits *limits)
{
	size_t at;

	/* The power table must hold the firmware's whole part. */
	if (power_bytes < FORMAT_POWER_BYTES)
		return EINVAL;

	/* The board table must be the one layout Navi 10 uses, whole. */
	if (board == NULL)
		return EINVAL;
	if (board_format != FORMAT_BOARD_FORMAT)
		return EINVAL;
	if (board_content != FORMAT_BOARD_CONTENT)
		return EINVAL;
	if (board_bytes < FORMAT_BOARD_BYTES)
		return EINVAL;

	/* Copies the firmware's part of the power table. */
	for (at = 0U; at < AMDRDNA_SMU_POWER_TABLE_BYTES; at++)
		table[at] = power[FORMAT_POWER_UNIT_PART_AT + at];

	/* Replaces its board part with the board table's. */
	for (at = 0U; at < FORMAT_UNIT_BOARD_BYTES; at++)
		table[FORMAT_UNIT_BOARD_AT + at] = board[FORMAT_BOARD_PART_AT + at];

	/* Reads the limits the finished table and the power table's header carry. */
	limits->edge = amdrdna_le16(table, FORMAT_UNIT_EDGE_AT);
	limits->hotspot = amdrdna_le16(table, FORMAT_UNIT_HOTSPOT_AT);
	limits->memory = amdrdna_le16(table, FORMAT_UNIT_MEMORY_AT);
	limits->shutdown = amdrdna_le16(power, FORMAT_POWER_SHUTDOWN_AT);
	limits->controller = power[FORMAT_POWER_CONTROLLER_AT];
	limits->features_low = amdrdna_le32(table, FORMAT_UNIT_FEATURES_AT);
	limits->features_high = amdrdna_le32(table, FORMAT_UNIT_FEATURES_AT + 4U);

	/* Succeeded: the table is ready for the firmware. */
	return 0;
}

/*
 * Reads the temperatures, the fan's speed and the clocks from a table of
 * readings the unit's firmware handed back (AMDRDNA_SMU_READINGS_BYTES bytes).
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu11/navi10_ppt.c
 * (navi1x_get_smu_metrics_data, navi10_get_smu_metrics_data,
 * navi10_get_legacy_smu_metrics_data).
 */
void
amdrdna_power_readings_read(
	const uint8_t *readings,
	struct amdrdna_power_readings *result)
{
	/* The three temperatures and the fan. */
	result->edge = amdrdna_le16(readings, FORMAT_READING_EDGE_AT);
	result->hotspot = amdrdna_le16(readings, FORMAT_READING_HOTSPOT_AT);
	result->memory = amdrdna_le16(readings, FORMAT_READING_MEMORY_AT);
	result->fan_speed = amdrdna_le16(readings, FORMAT_READING_FAN_AT);

	/* The clocks as they run now. */
	result->gfx_clock = amdrdna_le16(readings, FORMAT_READING_GFX_CLOCK_AT);
	result->system_clock = amdrdna_le16(readings, FORMAT_READING_SOC_CLOCK_AT);
	result->memory_clock = amdrdna_le16(readings, FORMAT_READING_MEMORY_CLOCK_AT);
	result->return_clock = amdrdna_le16(readings, FORMAT_READING_RETURN_CLOCK_AT);
	result->display_engine_clock = amdrdna_le16(readings, FORMAT_READING_DISPLAY_CLOCK_AT);

	/* The graphics clock's average and how busy the graphics engine was. */
	result->gfx_average = amdrdna_le16(readings, FORMAT_READING_GFX_AVERAGE_AT);
	result->gfx_activity = amdrdna_le16(readings, FORMAT_READING_GFX_ACTIVITY_AT);
}

/*
 * Reports whether the feature mask the unit's firmware reported has all
 * three features the driver asked for.
 */
bool
amdrdna_power_features_on(
	uint32_t low,
	uint32_t high)
{
	/* The critical-temperature stop. */
	if ((low & AMDRDNA_SMU_FEATURES_LOW) != AMDRDNA_SMU_FEATURES_LOW)
		return false;

	/* The fan and the thermal control. */
	if ((high & AMDRDNA_SMU_FEATURES_HIGH) != AMDRDNA_SMU_FEATURES_HIGH)
		return false;

	/* Succeeded: all three run. */
	return true;
}

/*
 * Puts together the firmware's watermark table
 * (AMDRDNA_SMU_WATERMARKS_BYTES bytes) for the memory clock's levels
 * (memory_mhz, count of them, at most AMDRDNA_SMU_WATERMARK_ROWS used): a
 * readers' row for each level, any display engine clock, the memory clock
 * from above the level before up to this level (from 0 for the first, to
 * 0xffff for the last), and the hub's watermark set of the row's number;
 * one writers' row for any clock with set 0.  The rows not used and the
 * firmware's own words go over as zero.
 *
 * Procedure referenced: Linux v6.12
 * drivers/gpu/drm/amd/display/dc/resource/dcn20/dcn20_resource.c
 * (dcn20_resource_construct: the reader and writer ranges),
 * drivers/gpu/drm/amd/display/dc/dml/dcn20/dcn20_fpu.c
 * (dcn20_fpu_set_wm_ranges, dcn20_update_bounding_box: a state for each
 * memory clock level) and
 * drivers/gpu/drm/amd/pm/swsmu/smu11/navi10_ppt.c
 * (navi10_set_watermarks_table).
 */
void
amdrdna_power_watermarks_build(
	const uint32_t *memory_mhz,
	unsigned count,
	uint8_t *table)
{
	unsigned at;
	unsigned row;
	uint32_t low;
	uint32_t high;
	uint8_t *entry;

	/* Everything starts as zero. */
	for (at = 0U; at < AMDRDNA_SMU_WATERMARKS_BYTES; at++)
		table[at] = 0U;

	/* At most a row a level. */
	if (count > AMDRDNA_SMU_WATERMARK_ROWS)
		count = AMDRDNA_SMU_WATERMARK_ROWS;

	/* The readers' rows, one for each memory clock level. */
	for (row = 0U; row < count; row++) {
		/* The memory clock's range: above the level before, up to this one; the last row has no top. */
		low = 0U;
		if (row > 0U)
			low = memory_mhz[row - 1U] + 1U;
		high = memory_mhz[row];
		if (row + 1U == count)
			high = FORMAT_WATERMARK_ANY;

		/* The row: any display engine clock, the memory clock's range, and the set. */
		entry = table + (AMDRDNA_SMU_WATERMARK_READERS * AMDRDNA_SMU_WATERMARK_ROWS + row) * AMDRDNA_SMU_WATERMARK_ROW_BYTES;
		format_put16(entry + FORMAT_WATERMARK_MIN_CLOCK, 0U);
		format_put16(entry + FORMAT_WATERMARK_MAX_CLOCK, FORMAT_WATERMARK_ANY);
		format_put16(entry + FORMAT_WATERMARK_MIN_MEMORY, low);
		format_put16(entry + FORMAT_WATERMARK_MAX_MEMORY, high);
		entry[FORMAT_WATERMARK_SET] = (uint8_t)row;
	}

	/* The writers' one row: any clock, set 0. */
	entry = table + AMDRDNA_SMU_WATERMARK_WRITERS * AMDRDNA_SMU_WATERMARK_ROWS * AMDRDNA_SMU_WATERMARK_ROW_BYTES;
	format_put16(entry + FORMAT_WATERMARK_MIN_CLOCK, 0U);
	format_put16(entry + FORMAT_WATERMARK_MAX_CLOCK, FORMAT_WATERMARK_ANY);
	format_put16(entry + FORMAT_WATERMARK_MIN_MEMORY, 0U);
	format_put16(entry + FORMAT_WATERMARK_MAX_MEMORY, FORMAT_WATERMARK_ANY);
	entry[FORMAT_WATERMARK_SET] = 0U;
}

/*
 * Puts together the table of the firmware's averaging settings
 * (AMDRDNA_SMU_SETTINGS_BYTES bytes): every average gets the same time
 * constant, AMDRDNA_SMU_AVERAGE_MS.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/smu11/navi10_ppt.c
 * (navi10_set_config_table).
 */
void
amdrdna_power_settings_build(
	uint8_t *table)
{
	unsigned at;
	unsigned average;

	/* The firmware's own part goes over as zero. */
	for (at = 0U; at < AMDRDNA_SMU_SETTINGS_BYTES; at++)
		table[at] = 0U;

	/* Each average's time constant, the low byte first. */
	for (average = 0U; average < FORMAT_SETTINGS_AVERAGES; average++) {
		table[average * 2U] = (uint8_t)(AMDRDNA_SMU_AVERAGE_MS & 0xffU);
		table[average * 2U + 1U] = (uint8_t)(AMDRDNA_SMU_AVERAGE_MS >> 8);
	}
}

/* Stores a 16-bit value, the low byte first. */
static void
format_put16(
	uint8_t *at,
	uint32_t value)
{
	/* The low byte, then the high. */
	at[0] = (uint8_t)(value & 0xffU);
	at[1] = (uint8_t)((value >> 8) & 0xffU);
}
