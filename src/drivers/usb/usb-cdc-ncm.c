/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * USB CDC NCM NTH16/NDP16 wire codec
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#include <drivers/usb-cdc-ncm.h>
#include <errno.h>
#include <limits.h>
#include <string.h>

struct ncm_range {
	uint16_t offset;
	uint16_t length;
};

struct ncm_parse_result {
	struct ncm_range ndps[DRV_USB_CDC_NCM_MAX_NDP_CHAIN];
	struct ncm_range datagrams[DRV_USB_CDC_NCM_MAX_RX_DATAGRAMS];
	size_t ndp_count;
	size_t datagram_count;
	uint16_t sequence;
	uint16_t block_length;
};

struct ncm_layout {
	size_t datagram_offset;
	size_t ndp_offset;
	size_t block_length;
};

static uint16_t load_le16(const uint8_t *bytes);
static int is_power_of_two(uint16_t value);
static uint32_t load_le32(const uint8_t *bytes);
static uint32_t clamp_nth16_size(uint32_t device_limit, uint32_t resource_limit);
static int profile_valid(const struct drv_usb_cdc_ncm_profile *profile);
static int alignment_valid(uint16_t divisor, uint16_t remainder, uint16_t alignment, uint32_t ntb_size);
static int make_layout(uint16_t divisor, uint16_t payload_remainder, uint16_t ndp_alignment, size_t datagram_length, uint16_t short_packet_size, size_t no_zlp_exact_size, struct ncm_layout *layout);
static int datagram_at_or_after(size_t minimum, uint16_t divisor, uint16_t payload_remainder, size_t *result);
static int congruent_at_or_after(size_t minimum, uint16_t divisor, uint16_t remainder, size_t *result);
static int checked_add(size_t left, size_t right, size_t *result);
static int finish_block_length(size_t *block_length, uint16_t short_packet_size, size_t no_zlp_exact_size);
static void store_le32(uint8_t *bytes, uint32_t value);
static void store_le16(uint8_t *bytes, uint16_t value);
static int validate_ntb16(const struct drv_usb_cdc_ncm_profile *profile, const struct drv_usb_cdc_ncm_rx_state *state, const void *ntb, size_t ntb_length, struct ncm_parse_result *result);
static int parse_ndp_chain(const struct drv_usb_cdc_ncm_profile *profile, const uint8_t *bytes, struct ncm_parse_result *result, uint16_t first_ndp);
static int range_valid(size_t total, size_t offset, size_t length);
static int record_ndp(struct ncm_parse_result *result, uint16_t offset, uint16_t length);
static int record_datagram(const struct drv_usb_cdc_ncm_profile *profile, struct ncm_parse_result *result, uint16_t offset, uint16_t length);
static int validate_nonoverlap(const struct ncm_parse_result *result);
static int ranges_overlap(const struct ncm_range *left, const struct ncm_range *right);

/*
 * Implements the drv usb cdc ncm negotiate nth16 operation.
 */
int
drv_usb_cdc_ncm_negotiate_nth16(
	const void *parameters,
	size_t parameters_length,
	const struct drv_usb_cdc_ncm_limits *limits,
	struct drv_usb_cdc_ncm_profile *profile)
{
	const uint8_t *bytes = parameters;
	struct drv_usb_cdc_ncm_profile candidate;
	struct ncm_layout layout;
	uint32_t device_in, device_out;
	uint16_t formats, device_tx_datagrams;
	int error;

	/* Handles the profile availability. */
	if (profile != NULL)
		memset(profile, 0, sizeof(*profile));

	/* Handles the bytes availability. */
	if (bytes == NULL || limits == NULL || profile == NULL ||
	    parameters_length < DRV_USB_CDC_NCM_NTB_PARAMETERS_SIZE)

		/* Returns the computed result. */
		return EINVAL;

	/* Checks the load le16 result. */
	if (load_le16(bytes) != DRV_USB_CDC_NCM_NTB_PARAMETERS_SIZE)
		return EOPNOTSUPP;
	formats = load_le16(bytes + 2U);

	/* Handles the formats condition. */
	if ((formats & DRV_USB_CDC_NCM_NTB16_SUPPORTED) == 0)
		return EOPNOTSUPP;

	/* Checks the power of two result. */
	if (limits->ntb_in_max_size < DRV_USB_CDC_NCM_NTB_MIN_SIZE ||
	    limits->ntb_out_max_size == 0 || limits->rx_max_datagrams == 0 ||
	    limits->rx_max_datagrams > DRV_USB_CDC_NCM_MAX_RX_DATAGRAMS ||
	    limits->tx_max_datagrams == 0 ||
	    limits->tx_max_datagrams > DRV_USB_CDC_NCM_MAX_TX_DATAGRAMS ||
	    limits->ndp_chain_max == 0 ||
	    limits->ndp_chain_max > DRV_USB_CDC_NCM_MAX_NDP_CHAIN ||
	    limits->bulk_out_max_packet_size < 8U ||
	    !is_power_of_two(limits->bulk_out_max_packet_size) ||
	    limits->bulk_out_max_packet_size > limits->ntb_out_max_size)

		/* Returns the computed result. */
		return EINVAL;

	device_in = load_le32(bytes + 4U);
	device_out = load_le32(bytes + 16U);

	/* Handles the device in condition. */
	if (device_in < DRV_USB_CDC_NCM_NTB_MIN_SIZE || device_out == 0)
		return EINVAL;

	/* Builds the parameter set from the device's own limits. */
	memset(&candidate, 0, sizeof(candidate));
	candidate.ntb_in_max_size =
		clamp_nth16_size(device_in, limits->ntb_in_max_size);
	candidate.ntb_out_max_size =
		clamp_nth16_size(device_out, limits->ntb_out_max_size);
	candidate.ndp_in_divisor = load_le16(bytes + 8U);
	candidate.ndp_in_payload_remainder = load_le16(bytes + 10U);
	candidate.ndp_in_alignment = load_le16(bytes + 12U);
	candidate.ndp_out_divisor = load_le16(bytes + 20U);
	candidate.ndp_out_payload_remainder = load_le16(bytes + 22U);
	candidate.ndp_out_alignment = load_le16(bytes + 24U);
	candidate.rx_max_datagrams = limits->rx_max_datagrams;
	device_tx_datagrams = load_le16(bytes + 26U);
	candidate.tx_max_datagrams = limits->tx_max_datagrams;

	/* Handles the device tx datagrams condition. */
	if (device_tx_datagrams != 0 &&
	    candidate.tx_max_datagrams > device_tx_datagrams)
		candidate.tx_max_datagrams = device_tx_datagrams;
	candidate.ndp_chain_max = limits->ndp_chain_max;
	candidate.max_datagram_size = DRV_USB_CDC_NCM_MAX_DATAGRAM_SIZE;
	candidate.bulk_out_max_packet_size = limits->bulk_out_max_packet_size;
	candidate.set_ntb_format_required =
		(formats & DRV_USB_CDC_NCM_NTB32_SUPPORTED) != 0;
	candidate.ntb_out_max_is_device_max =
		candidate.ntb_out_max_size == device_out;

	/* Checks the profile valid result. */
	if (!profile_valid(&candidate))
		return EINVAL;

	error = make_layout(candidate.ndp_in_divisor,
			    candidate.ndp_in_payload_remainder,
			    candidate.ndp_in_alignment,
			    candidate.max_datagram_size, 0, 0, &layout);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the layout condition. */
	if (layout.block_length > candidate.ntb_in_max_size)
		return EMSGSIZE;
	error = make_layout(
		candidate.ndp_out_divisor, candidate.ndp_out_payload_remainder,
		candidate.ndp_out_alignment, candidate.max_datagram_size,
		candidate.bulk_out_max_packet_size,
		candidate.ntb_out_max_is_device_max ? candidate.ntb_out_max_size
						    : 0,
		&layout);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the layout condition. */
	if (layout.block_length > candidate.ntb_out_max_size)
		return EMSGSIZE;

	*profile = candidate;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv usb cdc ncm make control request operation.
 */
int
drv_usb_cdc_ncm_make_control_request(
	const struct drv_usb_cdc_ncm_profile *profile,
	enum drv_usb_cdc_ncm_control_step step,
	struct drv_usb_cdc_ncm_control_request *request)
{
	/* Checks the profile valid result. */
	if (!profile_valid(profile) || request == NULL)
		return EINVAL;
	memset(request, 0, sizeof(*request));
	/* Dispatch the selected operation case. */
	switch (step) {
	case DRV_USB_CDC_NCM_CONTROL_SELECT_NTH16:
		/* Handles the profile condition. */
		if (profile->set_ntb_format_required == 0)
			return EOPNOTSUPP;
		request->request = DRV_USB_CDC_NCM_SET_NTB_FORMAT;
		break;
	case DRV_USB_CDC_NCM_CONTROL_SET_INPUT_SIZE:
		request->request = DRV_USB_CDC_NCM_SET_NTB_INPUT_SIZE;
		request->length = 4U;
		store_le32(request->payload, profile->ntb_in_max_size);
		break;
	case DRV_USB_CDC_NCM_CONTROL_SET_MAX_DATAGRAM_SIZE:
		request->request = DRV_USB_CDC_NCM_SET_MAX_DATAGRAM_SIZE;
		request->length = 2U;
		store_le16(request->payload, profile->max_datagram_size);
		break;
	case DRV_USB_CDC_NCM_CONTROL_DISABLE_CRC:
		request->request = DRV_USB_CDC_NCM_SET_CRC_MODE;
		break;
	default:
		/* Returns the computed result. */
		return EOPNOTSUPP;
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv usb cdc ncm rx reset operation.
 */
void
drv_usb_cdc_ncm_rx_reset(
	struct drv_usb_cdc_ncm_rx_state *state)
{
	/* Handles the state availability. */
	if (state != NULL)
		memset(state, 0, sizeof(*state));
}

/*
 * Implements the drv usb cdc ncm parse ntb16 operation.
 */
int
drv_usb_cdc_ncm_parse_ntb16(
	const struct drv_usb_cdc_ncm_profile *profile,
	struct drv_usb_cdc_ncm_rx_state *state,
	const void *ntb,
	size_t ntb_length,
	drv_usb_cdc_ncm_datagram_fn deliver,
	void *argument,
	size_t *datagram_count)
{
	const uint8_t *bytes = ntb;
	struct ncm_parse_result result;
	size_t index;
	int error;

	/* Handles the datagram count availability. */
	if (datagram_count != NULL)
		*datagram_count = 0;
	error = validate_ntb16(profile, state, ntb, ntb_length, &result);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the state condition. */
	if (state->sequence_initialized &&
	    result.sequence != state->expected_sequence &&
	    state->sequence_mismatches != UINT32_MAX)
		state->sequence_mismatches++;
	state->expected_sequence = (uint16_t)(result.sequence + 1U);
	state->sequence_initialized = 1;
	/* Process each remaining element. */
	for (index = 0; index < result.datagram_count; index++) {
		/* Handles the deliver availability. */
		if (deliver != NULL) {
			error = deliver(bytes + result.datagrams[index].offset,
					result.datagrams[index].length,
					argument);

			/* Checks the operation status. */
			if (error != 0) {
				/* Handles the datagram count availability. */
				if (datagram_count != NULL)
					*datagram_count = index;
				/* Returns the computed result. */
				return error;
			}
		}
	}

	/* Handles the datagram count availability. */
	if (datagram_count != NULL)
		*datagram_count = result.datagram_count;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv usb cdc ncm build ntb16 operation.
 */
int
drv_usb_cdc_ncm_build_ntb16(
	const struct drv_usb_cdc_ncm_profile *profile,
	uint16_t sequence,
	const void *frame,
	size_t frame_length,
	void *ntb,
	size_t ntb_capacity,
	size_t *ntb_length)
{
	const uint8_t *frame_bytes = frame;
	uint8_t *bytes = ntb;
	struct ncm_layout layout;
	int error;

	/* Handles the ntb length availability. */
	if (ntb_length != NULL)
		*ntb_length = 0;
	/* Checks the profile valid result. */
	if (!profile_valid(profile) || frame_bytes == NULL || bytes == NULL ||
	    ntb_length == NULL ||
	    frame_length < DRV_USB_CDC_NCM_ETHERNET_HEADER_SIZE ||
	    frame_length > profile->max_datagram_size)

		/* Returns the computed result. */
		return EINVAL;
	error = make_layout(
		profile->ndp_out_divisor, profile->ndp_out_payload_remainder,
		profile->ndp_out_alignment, frame_length,
		profile->bulk_out_max_packet_size,
		profile->ntb_out_max_is_device_max ? profile->ntb_out_max_size
						   : 0,
		&layout);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the layout condition. */
	if (layout.block_length > ntb_capacity ||
	    layout.block_length > profile->ntb_out_max_size ||
	    layout.block_length > UINT16_MAX ||
	    layout.datagram_offset > UINT16_MAX ||
	    layout.ndp_offset > UINT16_MAX)

		/* Returns the computed result. */
		return EMSGSIZE;

	/*
 * memmove first permits a caller-owned frame inside the output buffer.
	 */
	memmove(bytes + layout.datagram_offset, frame_bytes, frame_length);
	memset(bytes, 0, layout.datagram_offset);

	/* Handles the layout condition. */
	if (layout.block_length > layout.datagram_offset + frame_length) {
		memset(bytes + layout.datagram_offset + frame_length, 0,
		       layout.block_length -
			       (layout.datagram_offset + frame_length));
	}

	/* Writes the block header and the single datagram pointer. */
	store_le32(bytes, DRV_USB_CDC_NCM_NTH16_SIGNATURE);
	store_le16(bytes + 4U, DRV_USB_CDC_NCM_NTH16_SIZE);
	store_le16(bytes + 6U, sequence);
	store_le16(bytes + 8U, (uint16_t)layout.block_length);
	store_le16(bytes + 10U, (uint16_t)layout.ndp_offset);
	store_le32(bytes + layout.ndp_offset,
		   DRV_USB_CDC_NCM_NDP16_NOCRC_SIGNATURE);
	store_le16(bytes + layout.ndp_offset + 4U,
		   DRV_USB_CDC_NCM_NDP16_MIN_SIZE);
	store_le16(bytes + layout.ndp_offset + 6U, 0);
	store_le16(bytes + layout.ndp_offset + 8U,
		   (uint16_t)layout.datagram_offset);
	store_le16(bytes + layout.ndp_offset + 10U, (uint16_t)frame_length);
	store_le16(bytes + layout.ndp_offset + 12U, 0);
	store_le16(bytes + layout.ndp_offset + 14U, 0);
	*ntb_length = layout.block_length;
	/* Reports successful completion. */
	return 0;
}

/* Supports the load le16 operation. */
static uint16_t
load_le16(
	const uint8_t *bytes)
{
	/* Returns the computed result. */
	return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

/* Supports the is power of two operation. */
static int
is_power_of_two(
	uint16_t value)
{
	/* Returns the computed result. */
	return value != 0 && (value & (uint16_t)(value - 1U)) == 0;
}

/* Supports the load le32 operation. */
static uint32_t
load_le32(
	const uint8_t *bytes)
{
	/* Returns the computed result. */
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/* Supports the clamp nth16 size operation. */
static uint32_t
clamp_nth16_size(
	uint32_t device_limit,
	uint32_t resource_limit)
{
	uint32_t result = device_limit;

	/* Checks the operation result. */
	if (result > resource_limit)
		result = resource_limit;

	/* Checks the operation result. */
	if (result > UINT16_MAX)
		result = UINT16_MAX;

	/* Returns the computed result. */
	return result;
}

/* Supports the profile valid operation. */
static int
profile_valid(
	const struct drv_usb_cdc_ncm_profile *profile)
{
	int function_result;

	/* Checks the power of two result. */
	if (profile == NULL ||
	    profile->ntb_in_max_size < DRV_USB_CDC_NCM_NTB_MIN_SIZE ||
	    profile->ntb_out_max_size == 0 ||
	    profile->ntb_in_max_size > UINT16_MAX ||
	    profile->ntb_out_max_size > UINT16_MAX ||
	    profile->max_datagram_size != DRV_USB_CDC_NCM_MAX_DATAGRAM_SIZE ||
	    profile->rx_max_datagrams == 0 ||
	    profile->rx_max_datagrams > DRV_USB_CDC_NCM_MAX_RX_DATAGRAMS ||
	    profile->tx_max_datagrams == 0 ||
	    profile->tx_max_datagrams > DRV_USB_CDC_NCM_MAX_TX_DATAGRAMS ||
	    profile->ndp_chain_max == 0 ||
	    profile->ndp_chain_max > DRV_USB_CDC_NCM_MAX_NDP_CHAIN ||
	    profile->bulk_out_max_packet_size < 8U ||
	    !is_power_of_two(profile->bulk_out_max_packet_size) ||
	    profile->bulk_out_max_packet_size > profile->ntb_out_max_size ||
	    profile->set_ntb_format_required > 1U ||
	    profile->ntb_out_max_is_device_max > 1U)

		/* Reports successful completion. */
		return 0;

	/* Checks the alignment valid result. */
	if (!alignment_valid(
		    profile->ndp_in_divisor, profile->ndp_in_payload_remainder,
		    profile->ndp_in_alignment, profile->ntb_in_max_size))

		/* Reports successful completion. */
		return 0;

	/* Obtains the alignment valid result. */
	function_result = alignment_valid(
		profile->ndp_out_divisor, profile->ndp_out_payload_remainder,
		profile->ndp_out_alignment, profile->ntb_out_max_size);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the alignment valid operation. */
static int
alignment_valid(
	uint16_t divisor,
	uint16_t remainder,
	uint16_t alignment,
	uint32_t ntb_size)
{
	int function_result;

	/* Computes the function result. */
	function_result = divisor != 0 && remainder < divisor &&
			  alignment >= 4U && is_power_of_two(alignment) &&
			  alignment < ntb_size;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the make layout operation. */
static int
make_layout(
	uint16_t divisor,
	uint16_t payload_remainder,
	uint16_t ndp_alignment,
	size_t datagram_length,
	uint16_t short_packet_size,
	size_t no_zlp_exact_size,
	struct ncm_layout *layout)
{
	struct ncm_layout after_datagram, before_datagram;
	size_t end;
	int error;

	/* Candidate 1: NTH, datagram, NDP. */
	error = datagram_at_or_after(DRV_USB_CDC_NCM_NTH16_SIZE, divisor,
				     payload_remainder,
				     &after_datagram.datagram_offset);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = checked_add(after_datagram.datagram_offset, datagram_length,
			    &end);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = congruent_at_or_after(end, ndp_alignment, 0,
				      &after_datagram.ndp_offset);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = checked_add(after_datagram.ndp_offset,
			    DRV_USB_CDC_NCM_NDP16_MIN_SIZE,
			    &after_datagram.block_length);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = finish_block_length(&after_datagram.block_length,
				    short_packet_size, no_zlp_exact_size);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Candidate 2: NTH, NDP, datagram. */
	error = congruent_at_or_after(DRV_USB_CDC_NCM_NTH16_SIZE, ndp_alignment,
				      0, &before_datagram.ndp_offset);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = checked_add(before_datagram.ndp_offset,
			    DRV_USB_CDC_NCM_NDP16_MIN_SIZE, &end);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = datagram_at_or_after(end, divisor, payload_remainder,
				     &before_datagram.datagram_offset);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = checked_add(before_datagram.datagram_offset, datagram_length,
			    &before_datagram.block_length);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = finish_block_length(&before_datagram.block_length,
				    short_packet_size, no_zlp_exact_size);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	*layout = after_datagram.block_length <= before_datagram.block_length
			  ? after_datagram
			  : before_datagram;

	/* Reports successful completion. */
	return 0;
}

/* NCM aligns the Ethernet payload, fourteen bytes past the frame pointer. */
static int
datagram_at_or_after(
	size_t minimum,
	uint16_t divisor,
	uint16_t payload_remainder,
	size_t *result)
{
	int function_result;
	uint16_t frame_remainder;

	frame_remainder =
		(uint16_t)((payload_remainder + divisor -
			    (DRV_USB_CDC_NCM_ETHERNET_HEADER_SIZE % divisor)) %
			   divisor);

	/* Obtains the congruent at or after result. */
	function_result = congruent_at_or_after(minimum, divisor,
						frame_remainder, result);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the congruent at or after operation. */
static int
congruent_at_or_after(
	size_t minimum,
	uint16_t divisor,
	uint16_t remainder,
	size_t *result)
{
	int function_result;
	size_t modulus, delta;

	modulus = minimum % divisor;
	delta = ((size_t)remainder + divisor - modulus) % divisor;

	/* Obtains the checked add result. */
	function_result = checked_add(minimum, delta, result);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the checked add operation. */
static int
checked_add(
	size_t left,
	size_t right,
	size_t *result)
{
	/* Handles the left condition. */
	if (left > SIZE_MAX - right)
		return EOVERFLOW;
	*result = left + right;
	/* Reports successful completion. */
	return 0;
}

/* Supports the finish block length operation. */
static int
finish_block_length(
	size_t *block_length,
	uint16_t short_packet_size,
	size_t no_zlp_exact_size)
{
	int error;

	/* Handles the short packet size condition. */
	if (short_packet_size == 0 || *block_length % short_packet_size != 0 ||
	    *block_length == no_zlp_exact_size)
		/* Reports successful completion. */
		return 0;
	error = checked_add(*block_length, 1U, block_length);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the store le32 operation. */
static void
store_le32(
	uint8_t *bytes,
	uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

/* Supports the store le16 operation. */
static void
store_le16(
	uint8_t *bytes,
	uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

/* Supports the validate ntb16 operation. */
static int
validate_ntb16(
	const struct drv_usb_cdc_ncm_profile *profile,
	const struct drv_usb_cdc_ncm_rx_state *state,
	const void *ntb,
	size_t ntb_length,
	struct ncm_parse_result *result)
{
	const uint8_t *bytes = ntb;
	uint16_t wire_block_length, first_ndp;
	int error;

	memset(result, 0, sizeof(*result));

	/* Checks the profile valid result. */
	if (!profile_valid(profile) || state == NULL || bytes == NULL ||
	    ntb_length < DRV_USB_CDC_NCM_NTH16_SIZE ||
	    ntb_length > profile->ntb_in_max_size || ntb_length > UINT16_MAX)

		/* Returns the computed result. */
		return EINVAL;

	/* Checks the load le32 result. */
	if (load_le32(bytes) == DRV_USB_CDC_NCM_NTH32_SIGNATURE)
		return EOPNOTSUPP;

	/* Checks the load le32 result. */
	if (load_le32(bytes) != DRV_USB_CDC_NCM_NTH16_SIGNATURE ||
	    load_le16(bytes + 4U) != DRV_USB_CDC_NCM_NTH16_SIZE)

		/* Returns the computed result. */
		return EINVAL;
	result->sequence = load_le16(bytes + 6U);
	wire_block_length = load_le16(bytes + 8U);

	/* Handles the wire block length condition. */
	if (wire_block_length == 0) {
		/*
 * Zero is legal only when a short USB transfer delimits the
		 * NTB. */
		if (ntb_length >= profile->ntb_in_max_size)
			return EINVAL;
		result->block_length = (uint16_t)ntb_length;
	} else {
		/* Handles the wire block length condition. */
		if (wire_block_length != ntb_length)
			return EINVAL;
		result->block_length = wire_block_length;
	}
	first_ndp = load_le16(bytes + 10U);

	/* Handles the first ndp condition. */
	if (first_ndp == 0)
		return EINVAL;
	error = parse_ndp_chain(profile, bytes, result, first_ndp);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the parse ndp chain operation. */
static int
parse_ndp_chain(
	const struct drv_usb_cdc_ncm_profile *profile,
	const uint8_t *bytes,
	struct ncm_parse_result *result,
	uint16_t first_ndp)
{
	int function_result;
	uint16_t datagram_offset;
	uint16_t datagram_length;
	uint32_t signature;
	uint16_t ndp_length, next_ndp;
	size_t cursor, end;
	int terminated;
	int error;
	uint16_t ndp_offset = first_ndp;

	/* Continue while the operation condition remains true. */
	while (ndp_offset != 0) {
		terminated = 0;

		/* Checks the operation result. */
		if (result->ndp_count >= profile->ndp_chain_max)
			return EOVERFLOW;

		/* Checks the range valid result. */
		if (ndp_offset < DRV_USB_CDC_NCM_NTH16_SIZE ||
		    (ndp_offset & 3U) != 0 ||
		    ndp_offset % profile->ndp_in_alignment != 0 ||
		    !range_valid(result->block_length, ndp_offset,
				 DRV_USB_CDC_NCM_NDP16_HEADER_SIZE))

			/* Returns the computed result. */
			return EINVAL;
		signature = load_le32(bytes + ndp_offset);

		/* Handles the signature condition. */
		if (signature == DRV_USB_CDC_NCM_NDP16_CRC_SIGNATURE)
			return EOPNOTSUPP;

		/* Handles the signature condition. */
		if (signature != DRV_USB_CDC_NCM_NDP16_NOCRC_SIGNATURE)
			return EINVAL;
		ndp_length = load_le16(bytes + ndp_offset + 4U);
		next_ndp = load_le16(bytes + ndp_offset + 6U);

		/* Checks the range valid result. */
		if (ndp_length < DRV_USB_CDC_NCM_NDP16_MIN_SIZE ||
		    (ndp_length & 3U) != 0 ||
		    !range_valid(result->block_length, ndp_offset, ndp_length))

			/* Returns the computed result. */
			return EINVAL;
		error = record_ndp(result, ndp_offset, ndp_length);

		/* Checks the operation status. */
		if (error != 0)
			return error;

		cursor = (size_t)ndp_offset + DRV_USB_CDC_NCM_NDP16_HEADER_SIZE;
		end = (size_t)ndp_offset + ndp_length;
		/* Continue while the operation condition remains true. */
		while (cursor < end) {
			datagram_offset = load_le16(bytes + cursor);
			datagram_length = load_le16(bytes + cursor + 2U);

			cursor += DRV_USB_CDC_NCM_DPE16_SIZE;

			/* Handles the datagram offset condition. */
			if (datagram_offset == 0 || datagram_length == 0) {
				terminated = 1;
				break;
			}

			/* Checks the range valid result. */
			if (datagram_offset < DRV_USB_CDC_NCM_NTH16_SIZE ||
			    datagram_length <
				    DRV_USB_CDC_NCM_ETHERNET_HEADER_SIZE ||
			    datagram_length > profile->max_datagram_size ||
			    ((size_t)datagram_offset +
			     DRV_USB_CDC_NCM_ETHERNET_HEADER_SIZE) %
					    profile->ndp_in_divisor !=
				    profile->ndp_in_payload_remainder ||
			    !range_valid(result->block_length, datagram_offset,
					 datagram_length))

				/* Returns the computed result. */
				return EINVAL;
			error = record_datagram(profile, result,
						datagram_offset,
						datagram_length);

			/* Checks the operation status. */
			if (error != 0)
				return error;
		}

		/* Handles the terminated condition. */
		if (!terminated)
			return EINVAL;
		ndp_offset = next_ndp;
	}

	/* Obtains the validate nonoverlap result. */
	function_result = validate_nonoverlap(result);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the range valid operation. */
static int
range_valid(
	size_t total,
	size_t offset,
	size_t length)
{
	/* Returns the computed result. */
	return offset <= total && length <= total - offset;
}

/* Supports the record ndp operation. */
static int
record_ndp(
	struct ncm_parse_result *result,
	uint16_t offset,
	uint16_t length)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < result->ndp_count; index++) {
		/* Checks the operation result. */
		if (result->ndps[index].offset == offset)
			return ELOOP;
	}

	/* Checks the operation result. */
	if (result->ndp_count >= DRV_USB_CDC_NCM_MAX_NDP_CHAIN)
		return EOVERFLOW;
	result->ndps[result->ndp_count].offset = offset;
	result->ndps[result->ndp_count].length = length;
	result->ndp_count++;

	/* Reports successful completion. */
	return 0;
}

/* Supports the record datagram operation. */
static int
record_datagram(
	const struct drv_usb_cdc_ncm_profile *profile,
	struct ncm_parse_result *result,
	uint16_t offset,
	uint16_t length)
{
	/* Checks the operation result. */
	if (result->datagram_count >= profile->rx_max_datagrams ||
	    result->datagram_count >= DRV_USB_CDC_NCM_MAX_RX_DATAGRAMS)

		/* Returns the computed result. */
		return EOVERFLOW;
	result->datagrams[result->datagram_count].offset = offset;
	result->datagrams[result->datagram_count].length = length;
	result->datagram_count++;

	/* Reports successful completion. */
	return 0;
}

/* Supports the validate nonoverlap operation. */
static int
validate_nonoverlap(
	const struct ncm_parse_result *result)
{
	const struct ncm_range nth = {0, DRV_USB_CDC_NCM_NTH16_SIZE};
	size_t left, right;

	/* Process each remaining element. */
	for (left = 0; left < result->ndp_count; left++) {
		/* Checks the ranges overlap result. */
		if (ranges_overlap(&nth, &result->ndps[left]))
			return EINVAL;
		/* Process each remaining element. */
		for (right = left + 1U; right < result->ndp_count; right++) {
			/* Checks the ranges overlap result. */
			if (ranges_overlap(&result->ndps[left],
					   &result->ndps[right]))

				/* Returns the computed result. */
				return EINVAL;
		}
	}
	/* Process each remaining element. */
	for (left = 0; left < result->datagram_count; left++) {
		/* Checks the ranges overlap result. */
		if (ranges_overlap(&nth, &result->datagrams[left]))
			return EINVAL;
		/* Process each remaining element. */
		for (right = 0; right < result->ndp_count; right++) {
			/* Checks the ranges overlap result. */
			if (ranges_overlap(&result->datagrams[left],
					   &result->ndps[right]))

				/* Returns the computed result. */
				return EINVAL;
		}
		/* Process each remaining element. */
		for (right = left + 1U; right < result->datagram_count;
		     right++) {
			/* Checks the ranges overlap result. */
			if (ranges_overlap(&result->datagrams[left],
					   &result->datagrams[right]))

				/* Returns the computed result. */
				return EINVAL;
		}
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the ranges overlap operation. */
static int
ranges_overlap(
	const struct ncm_range *left,
	const struct ncm_range *right)
{
	size_t left_end = (size_t)left->offset + left->length;
	size_t right_end = (size_t)right->offset + right->length;

	/* Returns the computed result. */
	return left->offset < right_end && right->offset < left_end;
}
