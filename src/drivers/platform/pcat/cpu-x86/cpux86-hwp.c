/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decoder of the HWP registers and of the hardware feedback table.
 */

#include "cpux86-hwp.h"

/*
 * The bit positions of IA32_HWP_REQUEST, as (lowest bit, width) pairs.
 *
 * Values referenced: Intel SDM Vol. 3B rev. 093, §17.4.4.1, Figure 17-6:
 * Minimum_Performance 7:0, Maximum_Performance 15:8,
 * Desired_Performance 23:16, Energy_Performance_Preference 31:24,
 * Activity_Window 41:32, Package_Control 42, bits 58:43 reserved,
 * Activity_Window Valid 59, EPP Valid 60, Desired Valid 61,
 * Maximum Valid 62, Minimum Valid 63.
 */
#define HWP_REQUEST_MINIMUM_LOW		0U
#define HWP_REQUEST_MAXIMUM_LOW		8U
#define HWP_REQUEST_DESIRED_LOW		16U
#define HWP_REQUEST_PREFERENCE_LOW	24U
#define HWP_REQUEST_WINDOW_LOW		32U
#define HWP_REQUEST_PACKAGE_CONTROL_LOW	42U
#define HWP_REQUEST_RESERVED_LOW	43U
#define HWP_REQUEST_RESERVED_WIDTH	16U
#define HWP_REQUEST_VALID_LOW		59U

/*
 * The widths of the IA32_HWP_REQUEST fields.
 *
 * Values referenced: Intel SDM Vol. 3B rev. 093, §17.4.4.1, Figure 17-6
 * (four 8-bit hints, the 10-bit Activity_Window, the 1-bit Package_Control
 * and the five valid bits).
 */
#define HWP_LEVEL_WIDTH			8U
#define HWP_WINDOW_WIDTH		10U
#define HWP_VALID_WIDTH			5U

/*
 * The activity window field: a 7-bit mantissa in bits 6:0 and a 3-bit
 * power-of-ten exponent in bits 9:7 of the 10-bit field.
 *
 * Values referenced: Intel SDM Vol. 3B rev. 093, §17.4.4.1
 * (Activity_Window: bits 38:32 mantissa, bits 41:39 exponent, in
 * microseconds).
 */
#define HWP_WINDOW_MANTISSA_WIDTH	7U
#define HWP_WINDOW_MANTISSA_MAX		127U
#define HWP_WINDOW_EXPONENT_COUNT	8U
#define HWP_WINDOW_FIELD_MAX		1023U

/*
 * The bytes of the feedback table the layout and the readers count in.
 *
 * Values referenced: Intel SDM Vol. 3B rev. 093, Table 17-7 notes 1 to 3
 * (the global header starts with the 8-byte timestamp of Table 17-8, then
 * one change byte per class and capability; every entry is rounded up to
 * a multiple of 8 bytes).
 */
#define FEEDBACK_TIMESTAMP_SIZE		8U
#define FEEDBACK_ROUND			8U

/*
 * The capabilities enumerated by leaf 06H EDX[7:0].
 *
 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-16 (EDX[7:0]):
 * bit 0 performance, bit 1 energy efficiency, always enumerated together.
 */
#define FEEDBACK_CAPABILITY_MASK	0xFFU
#define FEEDBACK_CAPABILITY_REQUIRED	0x03U

/*
 * The bits of IA32_HW_FEEDBACK_PTR and IA32_HW_FEEDBACK_CONFIG.
 *
 * Values referenced: Intel SDM Vol. 3B rev. 093, §17.6.4 (bit 0 valid,
 * bits 11:1 reserved, ADDR from bit 12 up to MAXPHYADDR - 1) and §17.6.5
 * (bit 0 enable, bit 1 Thread Director), and the page size of
 * Vol. 1 rev. 093, Table 21-16.
 */
#define FEEDBACK_POINTER_VALID		0x1U
#define FEEDBACK_POINTER_ALIGN_MASK	0xFFFU
#define FEEDBACK_PHYSICAL_BITS_MIN	12U
#define FEEDBACK_PHYSICAL_BITS_MAX	52U
#define FEEDBACK_CONFIG_ENABLE		0x1U
#define FEEDBACK_CONFIG_THREAD_DIRECTOR	0x2U

/*
 * The bits of IA32_PACKAGE_THERM_STATUS the acknowledgement writes.
 *
 * Values referenced: Intel SDM Vol. 3B rev. 093, §17.6.6 (bit 26, R/WC0)
 * and §17.8, Figure 17-33; Vol. 4 rev. 093, Table 2-2: the log bits 3
 * (PROCHOT), 5 (critical temperature), 7 (threshold 1), 9 (threshold 2)
 * and 11 (power limitation) are R/WC0, and bit 1 (thermal status log) is
 * written back as it was read.
 */
#define PACKAGE_THERM_LOG_BITS		0x00000AA8U
#define PACKAGE_THERM_STATUS_LOG_BIT	0x00000002U

/* The reserved bits of IA32_HWP_STATUS: bit 1 and bits 63:6. */
#define HWP_STATUS_RESERVED		(~(uint64_t)CPUX86_HWP_STATUS_DEFINED)

static uint32_t hwp_pow10(uint32_t exponent);
static uint32_t hwp_popcount8(uint32_t bits);
static int hwp_offset_add(uint64_t *offset, uint64_t term, uint64_t table_size);

/*
 * Reports whether HWP is enabled from IA32_PM_ENABLE.
 *
 * Only bit 0 is defined, so a register with any other bit set is refused.
 */
int
cpux86_hwp_enabled(
	uint64_t raw,
	uint32_t *enabled)
{
	uint64_t reserved;

	/* Refuses a missing output. */
	if (enabled == NULL)
		return EINVAL;

	/* Refuses a register whose reserved bits 63:1 are not zero. */
	reserved = cpux86_field64(raw, 1U, 63U);
	if (reserved != 0U)
		return EINVAL;

	/* Succeeded: HWP_ENABLE is bit 0. */
	*enabled = (uint32_t)cpux86_field64(raw, 0U, 1U);
	return 0;
}

/*
 * Decodes IA32_HWP_CAPABILITIES.
 *
 * Each of the four levels is one byte of the low 32 bits.
 */
int
cpux86_hwp_capabilities_decode(
	uint64_t raw,
	struct cpux86_hwp_capabilities *capabilities)
{
	uint64_t reserved;

	/* Refuses a missing output. */
	if (capabilities == NULL)
		return EINVAL;

	/* Refuses a register whose reserved bits 63:32 are not zero. */
	reserved = cpux86_field64(raw, 32U, 32U);
	if (reserved != 0U)
		return EINVAL;

	/* Splits the register into its four levels. */
	capabilities->highest = (uint32_t)cpux86_field64(raw, 0U, 8U);
	capabilities->guaranteed = (uint32_t)cpux86_field64(raw, 8U, 8U);
	capabilities->efficient = (uint32_t)cpux86_field64(raw, 16U, 8U);
	capabilities->lowest = (uint32_t)cpux86_field64(raw, 24U, 8U);

	/* Succeeded: the output holds the four levels as reported. */
	return 0;
}

/*
 * Builds an IA32_HWP_REQUEST value.
 *
 * Every check runs before the output is written, so a refused request
 * leaves the caller's register value as it was.
 */
int
cpux86_hwp_request_encode(
	const struct cpux86_hwp_request *request,
	const struct cpux86_hwp_capabilities *capabilities,
	uint64_t *raw)
{
	uint64_t value;

	/* Refuses a missing request or a missing output. */
	if (request == NULL || raw == NULL)
		return EINVAL;

	/* Refuses a hint that does not fit its 8-bit field. */
	if (request->minimum > 255U ||
	    request->maximum > 255U ||
	    request->desired > 255U ||
	    request->preference > 255U)
		return EINVAL;

	/* Refuses an activity window above its 10-bit field. */
	if (request->window > HWP_WINDOW_FIELD_MAX)
		return EINVAL;

	/* Refuses a package control or valid bits above their widths. */
	if (request->package_control > 1U ||
	    request->valid > 31U)
		return EINVAL;

	/* Checks the hints against the levels the processor just reported. */
	if (capabilities != NULL) {
		/* Refuses a minimum below the lowest level. */
		if (request->minimum < capabilities->lowest)
			return EINVAL;

		/* Refuses a minimum above the maximum. */
		if (request->minimum > request->maximum)
			return EINVAL;

		/* Refuses a maximum above the highest level. */
		if (request->maximum > capabilities->highest)
			return EINVAL;

		/* Refuses a desired level outside lowest..highest; 0 hands the choice to the hardware. */
		if (request->desired != 0U &&
		    (request->desired < capabilities->lowest ||
		     request->desired > capabilities->highest))
			return EINVAL;
	}

	/* Places every field at its bits, leaving bits 58:43 zero. */
	value = 0U;
	value = cpux86_insert64(value, HWP_REQUEST_MINIMUM_LOW, HWP_LEVEL_WIDTH, request->minimum);
	value = cpux86_insert64(value, HWP_REQUEST_MAXIMUM_LOW, HWP_LEVEL_WIDTH, request->maximum);
	value = cpux86_insert64(value, HWP_REQUEST_DESIRED_LOW, HWP_LEVEL_WIDTH, request->desired);
	value = cpux86_insert64(value, HWP_REQUEST_PREFERENCE_LOW, HWP_LEVEL_WIDTH, request->preference);
	value = cpux86_insert64(value, HWP_REQUEST_WINDOW_LOW, HWP_WINDOW_WIDTH, request->window);
	value = cpux86_insert64(value, HWP_REQUEST_PACKAGE_CONTROL_LOW, 1U, request->package_control);
	value = cpux86_insert64(value, HWP_REQUEST_VALID_LOW, HWP_VALID_WIDTH, request->valid);

	/* Succeeded: publishes the register value. */
	*raw = value;
	return 0;
}

/*
 * Decodes an IA32_HWP_REQUEST or IA32_HWP_REQUEST_PKG value.
 *
 * The package register has no bits above 41, but the decoder reads both
 * registers the same way and leaves their meaning to the caller.
 */
int
cpux86_hwp_request_decode(
	uint64_t raw,
	struct cpux86_hwp_request *request)
{
	uint64_t reserved;

	/* Refuses a missing output. */
	if (request == NULL)
		return EINVAL;

	/* Refuses a register whose reserved bits 58:43 are not zero. */
	reserved = cpux86_field64(raw, HWP_REQUEST_RESERVED_LOW, HWP_REQUEST_RESERVED_WIDTH);
	if (reserved != 0U)
		return EINVAL;

	/* Splits the register into its hints and its window. */
	request->minimum = (uint32_t)cpux86_field64(raw, HWP_REQUEST_MINIMUM_LOW, HWP_LEVEL_WIDTH);
	request->maximum = (uint32_t)cpux86_field64(raw, HWP_REQUEST_MAXIMUM_LOW, HWP_LEVEL_WIDTH);
	request->desired = (uint32_t)cpux86_field64(raw, HWP_REQUEST_DESIRED_LOW, HWP_LEVEL_WIDTH);
	request->preference = (uint32_t)cpux86_field64(raw, HWP_REQUEST_PREFERENCE_LOW, HWP_LEVEL_WIDTH);
	request->window = (uint32_t)cpux86_field64(raw, HWP_REQUEST_WINDOW_LOW, HWP_WINDOW_WIDTH);

	/* Splits off the package control bit and the valid bits. */
	request->package_control = (uint32_t)cpux86_field64(raw, HWP_REQUEST_PACKAGE_CONTROL_LOW, 1U);
	request->valid = (uint32_t)cpux86_field64(raw, HWP_REQUEST_VALID_LOW, HWP_VALID_WIDTH);

	/* Succeeded: the output holds every field as it is. */
	return 0;
}

/*
 * Encodes an activity window for IA32_HWP_REQUEST[41:32].
 *
 * Every exponent is tried in turn with the largest mantissa that keeps the
 * window at or below the request; the largest window wins, and a tie goes
 * to the smaller exponent because only a larger one beats it.
 */
int
cpux86_hwp_window_encode(
	uint32_t microseconds,
	uint32_t *field)
{
	uint32_t best_field;
	uint32_t best_window;
	uint32_t exponent;
	uint32_t mantissa;
	uint32_t scale;
	uint32_t window;

	/* Refuses a missing output. */
	if (field == NULL)
		return EINVAL;

	/* Refuses a window longer than the field can say. */
	if (microseconds > CPUX86_HWP_WINDOW_MAX_US)
		return ERANGE;

	/*
	 * Finds the largest representable window not above the request,
	 * trying the exponents from the smallest up.
	 */
	best_field = 0U;
	best_window = 0U;
	scale = 1U;
	for (exponent = 0U;
	     exponent < HWP_WINDOW_EXPONENT_COUNT;
	     exponent++) {
		/* Takes the largest mantissa that does not pass the request. */
		mantissa = microseconds / scale;
		if (mantissa > HWP_WINDOW_MANTISSA_MAX)
			mantissa = HWP_WINDOW_MANTISSA_MAX;

		/* Keeps this encoding only when it is strictly larger than the best so far. */
		window = mantissa * scale;
		if (window > best_window) {
			best_window = window;
			best_field = (exponent << HWP_WINDOW_MANTISSA_WIDTH) | mantissa;
		}


		/* Moves on to the next power of ten. */
		scale = scale * 10U;
	}

	/* Succeeded: publishes the encoding, 0 for a request of 0. */
	*field = best_field;
	return 0;
}

/*
 * Decodes an activity window field.
 *
 * The window is the mantissa times ten to the exponent, at most
 * 127 * 10^7, which fits 32 bits.
 */
int
cpux86_hwp_window_decode(
	uint32_t field,
	uint32_t *microseconds)
{
	uint32_t exponent;
	uint32_t mantissa;

	/* Refuses a missing output. */
	if (microseconds == NULL)
		return EINVAL;

	/* Refuses a field wider than its 10 bits. */
	if (field > HWP_WINDOW_FIELD_MAX)
		return EINVAL;

	/* Splits the field into mantissa and exponent. */
	mantissa = field & HWP_WINDOW_MANTISSA_MAX;
	exponent = field >> HWP_WINDOW_MANTISSA_WIDTH;

	/* Succeeded: the window is the mantissa scaled by the power of ten. */
	*microseconds = mantissa * hwp_pow10(exponent);
	return 0;
}

/*
 * Reports which request register is in force.
 *
 * The thread register is used when the control bit and the package control
 * bit agree, the package register when they differ.
 */
int
cpux86_hwp_source(
	uint32_t control_bit,
	uint32_t package_control,
	uint32_t *source)
{
	/* Refuses a missing output. */
	if (source == NULL)
		return EINVAL;

	/* Refuses a bit that is not 0 or 1. */
	if (control_bit > 1U || package_control > 1U)
		return EINVAL;

	/* The two bits agree: the logical processor's own register is used. */
	if (control_bit == package_control) {
		*source = CPUX86_HWP_SOURCE_THREAD;
	} else {
		/* The bits differ: the package register is used, with the valid bits' overrides. */
		*source = CPUX86_HWP_SOURCE_PACKAGE;
	}

	/* Succeeded: the source is published. */
	return 0;
}

/*
 * Decodes IA32_HWP_STATUS.
 *
 * Bit 1 and bits 63:6 are reserved, so only the five defined event bits
 * may be set.
 */
int
cpux86_hwp_status_decode(
	uint64_t raw,
	uint32_t *events)
{
	/* Refuses a missing output. */
	if (events == NULL)
		return EINVAL;

	/* Refuses a register with a reserved bit set. */
	if ((raw & HWP_STATUS_RESERVED) != 0U)
		return EINVAL;

	/* Succeeded: the defined bits are the events. */
	*events = (uint32_t)raw;
	return 0;
}

/*
 * Builds the IA32_HWP_STATUS value that acknowledges some events.
 *
 * Writing 0 clears an event and writing 1 leaves it, so the value is the
 * defined bits with the acknowledged events taken out.
 */
int
cpux86_hwp_status_ack(
	uint32_t events,
	uint64_t *raw)
{
	/* Refuses a missing output. */
	if (raw == NULL)
		return EINVAL;

	/* Refuses a bit that is not a defined event. */
	if ((events & ~CPUX86_HWP_STATUS_DEFINED) != 0U)
		return EINVAL;

	/* Succeeded: 0 at the events to clear, 1 at the other events, 0 at the reserved bits. */
	*raw = (uint64_t)(CPUX86_HWP_STATUS_DEFINED & ~events);
	return 0;
}

/*
 * Works out the layout of the feedback table from leaf 06H.
 *
 * The checks run in the order the header states, so a processor without
 * the interface is ENOTSUP before any malformed detail is looked at.  The
 * sizes are formed in 64 bits, so a forged class count or page count
 * cannot wrap.
 */
int
cpux86_feedback_layout(
	const struct cpux86_power *power,
	struct cpux86_feedback_layout *layout)
{
	uint32_t capabilities;
	uint32_t capability_bits;
	uint32_t classes;
	uint64_t cells;
	uint64_t header_size;
	uint64_t row_size;
	uint64_t table_size;

	/* Refuses a missing input or output. */
	if (power == NULL || layout == NULL)
		return EINVAL;

	/* Reports a processor without the hardware feedback interface. */
	if ((power->features & CPUX86_POWER_HW_FEEDBACK) == 0U)
		return ENOTSUP;

	/* Reports a processor that enumerates no capability. */
	capability_bits = power->feedback_capabilities & FEEDBACK_CAPABILITY_MASK;
	if (capability_bits == 0U)
		return ENOTSUP;

	/* Refuses an enumeration without performance and efficiency, which the specification always pairs. */
	if ((capability_bits & FEEDBACK_CAPABILITY_REQUIRED) != FEEDBACK_CAPABILITY_REQUIRED)
		return EINVAL;

	/* Takes the class count from Thread Director, and one class without it. */
	classes = 1U;
	if ((power->features & CPUX86_POWER_THREAD_DIRECTOR) != 0U)
		classes = power->feedback_classes;

	/* Refuses a Thread Director with no class. */
	if (classes == 0U)
		return EINVAL;

	/* Counts the capabilities and the bytes of one class-by-capability array. */
	capabilities = hwp_popcount8(capability_bits);
	cells = (uint64_t)capabilities * (uint64_t)classes;

	/* Rounds the header (timestamp and change bytes) and one row up to multiples of 8. */
	header_size = ((uint64_t)FEEDBACK_TIMESTAMP_SIZE + cells + (FEEDBACK_ROUND - 1U)) / FEEDBACK_ROUND * FEEDBACK_ROUND;
	row_size = (cells + (FEEDBACK_ROUND - 1U)) / FEEDBACK_ROUND * FEEDBACK_ROUND;

	/* Refuses a page count whose table size does not fit 32 bits. */
	table_size = (uint64_t)power->feedback_pages * CPUX86_FEEDBACK_PAGE_SIZE;
	if (table_size > 0xFFFFFFFFU)
		return EINVAL;

	/* Refuses a table too small for its header and one row. */
	if (header_size + row_size > table_size)
		return EINVAL;

	/* Succeeded: publishes the geometry; the rows are what follows the header. */
	layout->capabilities = capabilities;
	layout->classes = classes;
	layout->header_size = (uint32_t)header_size;
	layout->row_size = (uint32_t)row_size;
	layout->table_size = (uint32_t)table_size;
	layout->row_count = (uint32_t)((table_size - header_size) / row_size);
	return 0;
}

/*
 * Reads the timestamp of the last hardware update of a feedback table.
 *
 * The eight bytes are assembled from the lowest up, so the host's byte
 * order does not matter.
 */
int
cpux86_feedback_timestamp(
	const uint8_t *table,
	size_t table_size,
	const struct cpux86_feedback_layout *layout,
	uint64_t *timestamp)
{
	uint64_t value;
	uint32_t index;

	/* Refuses a missing table, layout or output. */
	if (table == NULL || layout == NULL || timestamp == NULL)
		return EINVAL;

	/* Refuses a table that cannot hold the timestamp. */
	if (table_size < FEEDBACK_TIMESTAMP_SIZE)
		return EINVAL;

	/* Refuses a table that cannot hold the whole header. */
	if (table_size < layout->header_size)
		return EINVAL;

	/* Assembles the little-endian timestamp from the highest byte down. */
	value = 0U;
	for (index = FEEDBACK_TIMESTAMP_SIZE;
	     index > 0U;
	     index--) {
		value = (value << 8) | (uint64_t)table[index - 1U];
	}

	/* Succeeded: publishes the timestamp. */
	*timestamp = value;
	return 0;
}

/*
 * Reads one change-flag byte of the feedback table header.
 *
 * The byte sits after the timestamp, one per class and capability.
 */
int
cpux86_feedback_change(
	const uint8_t *table,
	size_t table_size,
	const struct cpux86_feedback_layout *layout,
	uint32_t class_index,
	uint32_t capability,
	uint32_t *flags)
{
	uint64_t offset;
	int error;

	/* Refuses a missing table, layout or output. */
	if (table == NULL || layout == NULL || flags == NULL)
		return EINVAL;

	/* Refuses a class or a capability the layout does not have. */
	if (class_index >= layout->classes || capability >= layout->capabilities)
		return EINVAL;

	/* Finds the byte after the timestamp, refusing an offset outside the table. */
	offset = FEEDBACK_TIMESTAMP_SIZE;
	error = hwp_offset_add(&offset, (uint64_t)layout->capabilities * (uint64_t)class_index, (uint64_t)table_size);
	if (error != 0)
		return EINVAL;

	/* Moves to the capability within the class, refusing an offset outside the table. */
	error = hwp_offset_add(&offset, (uint64_t)capability, (uint64_t)table_size);
	if (error != 0)
		return EINVAL;

	/* Succeeded: publishes the flag byte. */
	*flags = table[offset];
	return 0;
}

/*
 * Reads one cell of the feedback table.
 *
 * The cell is found from the end of the header, the row, the class and the
 * capability; every step is checked against the table's size so a forged
 * layout cannot reach beyond it.
 */
int
cpux86_feedback_cell(
	const uint8_t *table,
	size_t table_size,
	const struct cpux86_feedback_layout *layout,
	uint32_t row,
	uint32_t class_index,
	uint32_t capability,
	uint32_t *value)
{
	uint64_t offset;
	int error;

	/* Refuses a missing table, layout or output. */
	if (table == NULL || layout == NULL || value == NULL)
		return EINVAL;

	/* Refuses a row, class or capability the layout does not have. */
	if (row >= layout->row_count ||
	    class_index >= layout->classes ||
	    capability >= layout->capabilities)
		return EINVAL;

	/* Starts after the header, refusing a header that fills the table. */
	offset = layout->header_size;
	error = hwp_offset_add(&offset, (uint64_t)row * (uint64_t)layout->row_size, (uint64_t)table_size);
	if (error != 0)
		return EINVAL;

	/* Moves to the class within the row, refusing an offset outside the table. */
	error = hwp_offset_add(&offset, (uint64_t)layout->capabilities * (uint64_t)class_index, (uint64_t)table_size);
	if (error != 0)
		return EINVAL;

	/* Moves to the capability within the class, refusing an offset outside the table. */
	error = hwp_offset_add(&offset, (uint64_t)capability, (uint64_t)table_size);
	if (error != 0)
		return EINVAL;

	/* Succeeded: publishes the cell. */
	*value = table[offset];
	return 0;
}

/*
 * Builds the IA32_HW_FEEDBACK_PTR value for a table.
 *
 * The address must be a page frame inside the physical address width; the
 * value is the address with the valid bit set.
 */
int
cpux86_feedback_pointer_encode(
	uint64_t physical,
	uint32_t physical_bits,
	uint64_t *raw)
{
	uint64_t limit;

	/* Refuses a missing output. */
	if (raw == NULL)
		return EINVAL;

	/* Refuses a physical address width the processor cannot have. */
	if (physical_bits < FEEDBACK_PHYSICAL_BITS_MIN ||
	    physical_bits > FEEDBACK_PHYSICAL_BITS_MAX)
		return EINVAL;

	/* Refuses an address that is not 4 KiB aligned, which would set reserved bits 11:1 or bit 0. */
	if ((physical & FEEDBACK_POINTER_ALIGN_MASK) != 0U)
		return EINVAL;

	/* Refuses an address the physical address width cannot reach. */
	limit = (uint64_t)1U << physical_bits;
	if (physical >= limit)
		return EINVAL;

	/* Succeeded: the page frame address with the valid bit. */
	*raw = physical | FEEDBACK_POINTER_VALID;
	return 0;
}

/*
 * Builds the IA32_HW_FEEDBACK_CONFIG value.
 *
 * A malformed request (an argument above 1, or Thread Director without the
 * interface enabled) is checked before a request the processor lacks the
 * feature for.
 */
int
cpux86_feedback_config_encode(
	uint32_t enable,
	uint32_t thread_director,
	uint32_t supported,
	uint64_t *raw)
{
	uint64_t value;

	/* Refuses a missing output. */
	if (raw == NULL)
		return EINVAL;

	/* Refuses an argument that is not 0 or 1. */
	if (enable > 1U || thread_director > 1U || supported > 1U)
		return EINVAL;

	/* Refuses Thread Director without the interface, which the hardware quietly ignores. */
	if (thread_director == 1U && enable == 0U)
		return EINVAL;

	/* Reports Thread Director on a processor that does not enumerate it, where the write faults. */
	if (thread_director == 1U && supported == 0U)
		return ENOTSUP;

	/* Places the two enable bits. */
	value = 0U;
	if (enable == 1U)
		value |= FEEDBACK_CONFIG_ENABLE;
	if (thread_director == 1U)
		value |= FEEDBACK_CONFIG_THREAD_DIRECTOR;

	/* Succeeded: publishes the configuration. */
	*raw = value;
	return 0;
}

/*
 * Reports whether the hardware has updated the feedback table.
 *
 * Only bit 26 is looked at; the other bits of the register are not this
 * function's concern.
 */
int
cpux86_feedback_updated(
	uint64_t raw,
	uint32_t *updated)
{
	/* Refuses a missing output. */
	if (updated == NULL)
		return EINVAL;

	/* Succeeded: bit 26 is the update flag. */
	*updated = (uint32_t)cpux86_field64(raw, CPUX86_PACKAGE_THERM_FEEDBACK, 1U);
	return 0;
}

/*
 * Builds the IA32_PACKAGE_THERM_STATUS value that hands the table back to
 * the hardware.
 *
 * The value does not copy the register: bit 26 is 0, the log bits are 1
 * so no logged event is lost, bit 1 keeps what was read, and every other
 * bit is 0.
 */
int
cpux86_feedback_ack(
	uint64_t raw,
	uint64_t *value)
{
	uint64_t status_log;

	/* Refuses a missing output. */
	if (value == NULL)
		return EINVAL;

	/* Keeps the thermal status log bit as it was read, the only bit taken from the register. */
	status_log = raw & PACKAGE_THERM_STATUS_LOG_BIT;

	/* Succeeded: the log bits stay as they are and bit 26 is cleared. */
	*value = (uint64_t)PACKAGE_THERM_LOG_BITS | status_log;
	return 0;
}

/*
 * Reports ten to the power of an exponent, for exponents 0 to 9.
 */
static uint32_t
hwp_pow10(
	uint32_t exponent)
{
	uint32_t power;
	uint32_t step;

	/* Multiplies by ten once per exponent step. */
	power = 1U;
	for (step = 0U;
	     step < exponent;
	     step++) {
		power = power * 10U;
	}

	/* Succeeded: the power of ten. */
	return power;
}

/*
 * Counts the set bits of an 8-bit value.
 */
static uint32_t
hwp_popcount8(
	uint32_t bits)
{
	uint32_t count;
	uint32_t index;

	/* Adds one for each of the eight bits that is set. */
	count = 0U;
	for (index = 0U;
	     index < 8U;
	     index++) {
		count += (bits >> index) & 1U;
	}

	/* Succeeded: the number of set bits. */
	return count;
}

/*
 * Adds a term to a table offset, refusing a result that is not a byte of
 * the table.
 *
 * The offset must already be inside the table and the term must keep it
 * inside; the subtraction below cannot wrap for that reason, so the sum is
 * never formed when it could overflow.
 */
static int
hwp_offset_add(
	uint64_t *offset,
	uint64_t term,
	uint64_t table_size)
{
	/* Refuses a starting offset that is already outside the table. */
	if (*offset >= table_size)
		return EINVAL;

	/* Refuses a term that would take the offset to the end of the table or beyond. */
	if (term >= table_size - *offset)
		return EINVAL;

	/* Succeeded: the offset moves on and is still inside the table. */
	*offset += term;
	return 0;
}
