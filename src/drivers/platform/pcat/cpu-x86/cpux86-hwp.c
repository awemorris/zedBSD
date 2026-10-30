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
 * Reports whether HWP is enabled from IA32_PM_ENABLE.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_hwp_enabled(
	uint64_t raw,
	uint32_t *enabled)
{
	/* The skeleton reads none of its arguments. */
	(void)raw;
	(void)enabled;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Decodes IA32_HWP_CAPABILITIES.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_hwp_capabilities_decode(
	uint64_t raw,
	struct cpux86_hwp_capabilities *capabilities)
{
	/* The skeleton reads none of its arguments. */
	(void)raw;
	(void)capabilities;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Builds an IA32_HWP_REQUEST value.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_hwp_request_encode(
	const struct cpux86_hwp_request *request,
	const struct cpux86_hwp_capabilities *capabilities,
	uint64_t *raw)
{
	/* The skeleton reads none of its arguments. */
	(void)request;
	(void)capabilities;
	(void)raw;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Decodes an IA32_HWP_REQUEST or IA32_HWP_REQUEST_PKG value.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_hwp_request_decode(
	uint64_t raw,
	struct cpux86_hwp_request *request)
{
	/* The skeleton reads none of its arguments. */
	(void)raw;
	(void)request;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Encodes an activity window for IA32_HWP_REQUEST[41:32].
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_hwp_window_encode(
	uint32_t microseconds,
	uint32_t *field)
{
	/* The skeleton reads none of its arguments. */
	(void)microseconds;
	(void)field;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Decodes an activity window field.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_hwp_window_decode(
	uint32_t field,
	uint32_t *microseconds)
{
	/* The skeleton reads none of its arguments. */
	(void)field;
	(void)microseconds;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Reports which request register is in force.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_hwp_source(
	uint32_t control_bit,
	uint32_t package_control,
	uint32_t *source)
{
	/* The skeleton reads none of its arguments. */
	(void)control_bit;
	(void)package_control;
	(void)source;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Decodes IA32_HWP_STATUS.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_hwp_status_decode(
	uint64_t raw,
	uint32_t *events)
{
	/* The skeleton reads none of its arguments. */
	(void)raw;
	(void)events;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Builds the IA32_HWP_STATUS value that acknowledges some events.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_hwp_status_ack(
	uint32_t events,
	uint64_t *raw)
{
	/* The skeleton reads none of its arguments. */
	(void)events;
	(void)raw;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Works out the layout of the feedback table from leaf 06H.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_feedback_layout(
	const struct cpux86_power *power,
	struct cpux86_feedback_layout *layout)
{
	/* The skeleton reads none of its arguments. */
	(void)power;
	(void)layout;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Reads the timestamp of the last hardware update of a feedback table.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_feedback_timestamp(
	const uint8_t *table,
	size_t table_size,
	const struct cpux86_feedback_layout *layout,
	uint64_t *timestamp)
{
	/* The skeleton reads none of its arguments. */
	(void)table;
	(void)table_size;
	(void)layout;
	(void)timestamp;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Reads one change-flag byte of the feedback table header.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
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
	/* The skeleton reads none of its arguments. */
	(void)table;
	(void)table_size;
	(void)layout;
	(void)class_index;
	(void)capability;
	(void)flags;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Reads one cell of the feedback table.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
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
	/* The skeleton reads none of its arguments. */
	(void)table;
	(void)table_size;
	(void)layout;
	(void)row;
	(void)class_index;
	(void)capability;
	(void)value;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Builds the IA32_HW_FEEDBACK_PTR value for a table.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_feedback_pointer_encode(
	uint64_t physical,
	uint32_t physical_bits,
	uint64_t *raw)
{
	/* The skeleton reads none of its arguments. */
	(void)physical;
	(void)physical_bits;
	(void)raw;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Builds the IA32_HW_FEEDBACK_CONFIG value.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_feedback_config_encode(
	uint32_t enable,
	uint32_t thread_director,
	uint32_t supported,
	uint64_t *raw)
{
	/* The skeleton reads none of its arguments. */
	(void)enable;
	(void)thread_director;
	(void)supported;
	(void)raw;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Reports whether the hardware has updated the feedback table.
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_feedback_updated(
	uint64_t raw,
	uint32_t *updated)
{
	/* The skeleton reads none of its arguments. */
	(void)raw;
	(void)updated;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}

/*
 * Builds the IA32_PACKAGE_THERM_STATUS value that hands the table back to
 *
 * Not implemented yet: the skeleton answers as an unimplemented call.
 */
int
cpux86_feedback_ack(
	uint64_t raw,
	uint64_t *value)
{
	/* The skeleton reads none of its arguments. */
	(void)raw;
	(void)value;

	/* Reports that the call is not implemented. */
	return ENOSYS;
}
