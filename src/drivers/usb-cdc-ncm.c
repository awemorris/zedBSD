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

static int
range_valid(size_t total, size_t offset, size_t length)
{
	return offset <= total && length <= total - offset;
}

static int
checked_add(size_t left, size_t right, size_t *result)
{
	if (left > SIZE_MAX - right)
		return EOVERFLOW;
	*result = left + right;
	return 0;
}

static int
is_power_of_two(uint16_t value)
{
	return value != 0 && (value & (uint16_t)(value - 1U)) == 0;
}

static uint16_t
load_le16(const uint8_t *bytes)
{
	return (uint16_t)((uint16_t)bytes[0] |
	    ((uint16_t)bytes[1] << 8));
}

static uint32_t
load_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	    ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void
store_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static void
store_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

static uint32_t
clamp_nth16_size(uint32_t device_limit, uint32_t resource_limit)
{
	uint32_t result = device_limit;

	if (result > resource_limit)
		result = resource_limit;
	if (result > UINT16_MAX)
		result = UINT16_MAX;
	return result;
}

static int
congruent_at_or_after(size_t minimum, uint16_t divisor,
	uint16_t remainder, size_t *result)
{
	size_t modulus, delta;

	modulus = minimum % divisor;
	delta = ((size_t)remainder + divisor - modulus) % divisor;
	return checked_add(minimum, delta, result);
}

/* NCM aligns the Ethernet payload, fourteen bytes past the frame pointer. */
static int
datagram_at_or_after(size_t minimum, uint16_t divisor,
	uint16_t payload_remainder, size_t *result)
{
	uint16_t frame_remainder;

	frame_remainder = (uint16_t)((payload_remainder + divisor -
	    (DRV_USB_CDC_NCM_ETHERNET_HEADER_SIZE % divisor)) % divisor);
	return congruent_at_or_after(minimum, divisor, frame_remainder, result);
}

static int
finish_block_length(size_t *block_length, uint16_t short_packet_size,
	size_t no_zlp_exact_size)
{
	int error;

	if (short_packet_size == 0 ||
	    *block_length % short_packet_size != 0 ||
	    *block_length == no_zlp_exact_size)
		return 0;
	error = checked_add(*block_length, 1U, block_length);
	return error;
}

static int
make_layout(uint16_t divisor, uint16_t payload_remainder,
	uint16_t ndp_alignment, size_t datagram_length,
	uint16_t short_packet_size, size_t no_zlp_exact_size,
	struct ncm_layout *layout)
{
	struct ncm_layout after_datagram, before_datagram;
	size_t end;
	int error;

	/* Candidate 1: NTH, datagram, NDP. */
	error = datagram_at_or_after(DRV_USB_CDC_NCM_NTH16_SIZE, divisor,
	    payload_remainder, &after_datagram.datagram_offset);
	if (error != 0)
		return error;
	error = checked_add(after_datagram.datagram_offset, datagram_length,
	    &end);
	if (error != 0)
		return error;
	error = congruent_at_or_after(end, ndp_alignment, 0,
	    &after_datagram.ndp_offset);
	if (error != 0)
		return error;
	error = checked_add(after_datagram.ndp_offset,
	    DRV_USB_CDC_NCM_NDP16_MIN_SIZE, &after_datagram.block_length);
	if (error != 0)
		return error;
	error = finish_block_length(&after_datagram.block_length,
	    short_packet_size, no_zlp_exact_size);
	if (error != 0)
		return error;

	/* Candidate 2: NTH, NDP, datagram. */
	error = congruent_at_or_after(DRV_USB_CDC_NCM_NTH16_SIZE,
	    ndp_alignment, 0, &before_datagram.ndp_offset);
	if (error != 0)
		return error;
	error = checked_add(before_datagram.ndp_offset,
	    DRV_USB_CDC_NCM_NDP16_MIN_SIZE, &end);
	if (error != 0)
		return error;
	error = datagram_at_or_after(end, divisor, payload_remainder,
	    &before_datagram.datagram_offset);
	if (error != 0)
		return error;
	error = checked_add(before_datagram.datagram_offset, datagram_length,
	    &before_datagram.block_length);
	if (error != 0)
		return error;
	error = finish_block_length(&before_datagram.block_length,
	    short_packet_size, no_zlp_exact_size);
	if (error != 0)
		return error;

	*layout = after_datagram.block_length <= before_datagram.block_length ?
	    after_datagram : before_datagram;
	return 0;
}

static int
alignment_valid(uint16_t divisor, uint16_t remainder,
	uint16_t alignment, uint32_t ntb_size)
{
	return divisor != 0 && remainder < divisor && alignment >= 4U &&
	    is_power_of_two(alignment) && alignment < ntb_size;
}

static int
profile_valid(const struct drv_usb_cdc_ncm_profile *profile)
{
	if (profile == NULL ||
	    profile->ntb_in_max_size < DRV_USB_CDC_NCM_NTB_MIN_SIZE ||
	    profile->ntb_out_max_size == 0 ||
	    profile->ntb_in_max_size > UINT16_MAX ||
	    profile->ntb_out_max_size > UINT16_MAX ||
	    profile->max_datagram_size !=
		DRV_USB_CDC_NCM_MAX_DATAGRAM_SIZE ||
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
		return 0;
	if (!alignment_valid(profile->ndp_in_divisor,
	    profile->ndp_in_payload_remainder, profile->ndp_in_alignment,
	    profile->ntb_in_max_size))
		return 0;
	return alignment_valid(profile->ndp_out_divisor,
	    profile->ndp_out_payload_remainder, profile->ndp_out_alignment,
	    profile->ntb_out_max_size);
}

int
drv_usb_cdc_ncm_negotiate_nth16(const void *parameters,
	size_t parameters_length, const struct drv_usb_cdc_ncm_limits *limits,
	struct drv_usb_cdc_ncm_profile *profile)
{
	const uint8_t *bytes = parameters;
	struct drv_usb_cdc_ncm_profile candidate;
	struct ncm_layout layout;
	uint32_t device_in, device_out;
	uint16_t formats, device_tx_datagrams;
	int error;

	if (profile != NULL)
		memset(profile, 0, sizeof(*profile));
	if (bytes == NULL || limits == NULL || profile == NULL ||
	    parameters_length < DRV_USB_CDC_NCM_NTB_PARAMETERS_SIZE)
		return EINVAL;
	if (load_le16(bytes) != DRV_USB_CDC_NCM_NTB_PARAMETERS_SIZE)
		return EOPNOTSUPP;
	formats = load_le16(bytes + 2U);
	if ((formats & DRV_USB_CDC_NCM_NTB16_SUPPORTED) == 0)
		return EOPNOTSUPP;
	if (limits->ntb_in_max_size < DRV_USB_CDC_NCM_NTB_MIN_SIZE ||
	    limits->ntb_out_max_size == 0 ||
	    limits->rx_max_datagrams == 0 ||
	    limits->rx_max_datagrams > DRV_USB_CDC_NCM_MAX_RX_DATAGRAMS ||
	    limits->tx_max_datagrams == 0 ||
	    limits->tx_max_datagrams > DRV_USB_CDC_NCM_MAX_TX_DATAGRAMS ||
	    limits->ndp_chain_max == 0 ||
	    limits->ndp_chain_max > DRV_USB_CDC_NCM_MAX_NDP_CHAIN ||
	    limits->bulk_out_max_packet_size < 8U ||
	    !is_power_of_two(limits->bulk_out_max_packet_size) ||
	    limits->bulk_out_max_packet_size > limits->ntb_out_max_size)
		return EINVAL;

	device_in = load_le32(bytes + 4U);
	device_out = load_le32(bytes + 16U);
	if (device_in < DRV_USB_CDC_NCM_NTB_MIN_SIZE || device_out == 0)
		return EINVAL;

	memset(&candidate, 0, sizeof(candidate));
	candidate.ntb_in_max_size = clamp_nth16_size(device_in,
	    limits->ntb_in_max_size);
	candidate.ntb_out_max_size = clamp_nth16_size(device_out,
	    limits->ntb_out_max_size);
	candidate.ndp_in_divisor = load_le16(bytes + 8U);
	candidate.ndp_in_payload_remainder = load_le16(bytes + 10U);
	candidate.ndp_in_alignment = load_le16(bytes + 12U);
	candidate.ndp_out_divisor = load_le16(bytes + 20U);
	candidate.ndp_out_payload_remainder = load_le16(bytes + 22U);
	candidate.ndp_out_alignment = load_le16(bytes + 24U);
	candidate.rx_max_datagrams = limits->rx_max_datagrams;
	device_tx_datagrams = load_le16(bytes + 26U);
	candidate.tx_max_datagrams = limits->tx_max_datagrams;
	if (device_tx_datagrams != 0 &&
	    candidate.tx_max_datagrams > device_tx_datagrams)
		candidate.tx_max_datagrams = device_tx_datagrams;
	candidate.ndp_chain_max = limits->ndp_chain_max;
	candidate.max_datagram_size = DRV_USB_CDC_NCM_MAX_DATAGRAM_SIZE;
	candidate.bulk_out_max_packet_size =
	    limits->bulk_out_max_packet_size;
	candidate.set_ntb_format_required =
	    (formats & DRV_USB_CDC_NCM_NTB32_SUPPORTED) != 0;
	candidate.ntb_out_max_is_device_max =
	    candidate.ntb_out_max_size == device_out;
	if (!profile_valid(&candidate))
		return EINVAL;

	error = make_layout(candidate.ndp_in_divisor,
	    candidate.ndp_in_payload_remainder, candidate.ndp_in_alignment,
	    candidate.max_datagram_size, 0, 0, &layout);
	if (error != 0)
		return error;
	if (layout.block_length > candidate.ntb_in_max_size)
		return EMSGSIZE;
	error = make_layout(candidate.ndp_out_divisor,
	    candidate.ndp_out_payload_remainder, candidate.ndp_out_alignment,
	    candidate.max_datagram_size,
	    candidate.bulk_out_max_packet_size,
	    candidate.ntb_out_max_is_device_max ?
		candidate.ntb_out_max_size : 0, &layout);
	if (error != 0)
		return error;
	if (layout.block_length > candidate.ntb_out_max_size)
		return EMSGSIZE;

	*profile = candidate;
	return 0;
}

int
drv_usb_cdc_ncm_make_control_request(
	const struct drv_usb_cdc_ncm_profile *profile,
	enum drv_usb_cdc_ncm_control_step step,
	struct drv_usb_cdc_ncm_control_request *request)
{
	if (!profile_valid(profile) || request == NULL)
		return EINVAL;
	memset(request, 0, sizeof(*request));
	switch (step) {
	case DRV_USB_CDC_NCM_CONTROL_SELECT_NTH16:
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
		return EOPNOTSUPP;
	}
	return 0;
}

void
drv_usb_cdc_ncm_rx_reset(struct drv_usb_cdc_ncm_rx_state *state)
{
	if (state != NULL)
		memset(state, 0, sizeof(*state));
}

static int
ranges_overlap(const struct ncm_range *left, const struct ncm_range *right)
{
	size_t left_end = (size_t)left->offset + left->length;
	size_t right_end = (size_t)right->offset + right->length;

	return left->offset < right_end && right->offset < left_end;
}

static int
record_ndp(struct ncm_parse_result *result, uint16_t offset, uint16_t length)
{
	size_t index;

	for (index = 0; index < result->ndp_count; index++) {
		if (result->ndps[index].offset == offset)
			return ELOOP;
	}
	if (result->ndp_count >= DRV_USB_CDC_NCM_MAX_NDP_CHAIN)
		return EOVERFLOW;
	result->ndps[result->ndp_count].offset = offset;
	result->ndps[result->ndp_count].length = length;
	result->ndp_count++;
	return 0;
}

static int
record_datagram(const struct drv_usb_cdc_ncm_profile *profile,
	struct ncm_parse_result *result, uint16_t offset, uint16_t length)
{
	if (result->datagram_count >= profile->rx_max_datagrams ||
	    result->datagram_count >= DRV_USB_CDC_NCM_MAX_RX_DATAGRAMS)
		return EOVERFLOW;
	result->datagrams[result->datagram_count].offset = offset;
	result->datagrams[result->datagram_count].length = length;
	result->datagram_count++;
	return 0;
}

static int
validate_nonoverlap(const struct ncm_parse_result *result)
{
	const struct ncm_range nth = {0, DRV_USB_CDC_NCM_NTH16_SIZE};
	size_t left, right;

	for (left = 0; left < result->ndp_count; left++) {
		if (ranges_overlap(&nth, &result->ndps[left]))
			return EINVAL;
		for (right = left + 1U; right < result->ndp_count; right++) {
			if (ranges_overlap(&result->ndps[left],
			    &result->ndps[right]))
				return EINVAL;
		}
	}
	for (left = 0; left < result->datagram_count; left++) {
		if (ranges_overlap(&nth, &result->datagrams[left]))
			return EINVAL;
		for (right = 0; right < result->ndp_count; right++) {
			if (ranges_overlap(&result->datagrams[left],
			    &result->ndps[right]))
				return EINVAL;
		}
		for (right = left + 1U; right < result->datagram_count;
		    right++) {
			if (ranges_overlap(&result->datagrams[left],
			    &result->datagrams[right]))
				return EINVAL;
		}
	}
	return 0;
}

static int
parse_ndp_chain(const struct drv_usb_cdc_ncm_profile *profile,
	const uint8_t *bytes, struct ncm_parse_result *result,
	uint16_t first_ndp)
{
	uint16_t ndp_offset = first_ndp;

	while (ndp_offset != 0) {
		uint32_t signature;
		uint16_t ndp_length, next_ndp;
		size_t cursor, end;
		int terminated = 0;
		int error;

		if (result->ndp_count >= profile->ndp_chain_max)
			return EOVERFLOW;
		if (ndp_offset < DRV_USB_CDC_NCM_NTH16_SIZE ||
		    (ndp_offset & 3U) != 0 ||
		    ndp_offset % profile->ndp_in_alignment != 0 ||
		    !range_valid(result->block_length, ndp_offset,
			DRV_USB_CDC_NCM_NDP16_HEADER_SIZE))
			return EINVAL;
		signature = load_le32(bytes + ndp_offset);
		if (signature == DRV_USB_CDC_NCM_NDP16_CRC_SIGNATURE)
			return EOPNOTSUPP;
		if (signature != DRV_USB_CDC_NCM_NDP16_NOCRC_SIGNATURE)
			return EINVAL;
		ndp_length = load_le16(bytes + ndp_offset + 4U);
		next_ndp = load_le16(bytes + ndp_offset + 6U);
		if (ndp_length < DRV_USB_CDC_NCM_NDP16_MIN_SIZE ||
		    (ndp_length & 3U) != 0 ||
		    !range_valid(result->block_length, ndp_offset, ndp_length))
			return EINVAL;
		error = record_ndp(result, ndp_offset, ndp_length);
		if (error != 0)
			return error;

		cursor = (size_t)ndp_offset +
		    DRV_USB_CDC_NCM_NDP16_HEADER_SIZE;
		end = (size_t)ndp_offset + ndp_length;
		while (cursor < end) {
			uint16_t datagram_offset = load_le16(bytes + cursor);
			uint16_t datagram_length =
			    load_le16(bytes + cursor + 2U);

			cursor += DRV_USB_CDC_NCM_DPE16_SIZE;
			if (datagram_offset == 0 || datagram_length == 0) {
				terminated = 1;
				break;
			}
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
				return EINVAL;
			error = record_datagram(profile, result, datagram_offset,
			    datagram_length);
			if (error != 0)
				return error;
		}
		if (!terminated)
			return EINVAL;
		ndp_offset = next_ndp;
	}
	return validate_nonoverlap(result);
}

static int
validate_ntb16(const struct drv_usb_cdc_ncm_profile *profile,
	const struct drv_usb_cdc_ncm_rx_state *state, const void *ntb,
	size_t ntb_length, struct ncm_parse_result *result)
{
	const uint8_t *bytes = ntb;
	uint16_t wire_block_length, first_ndp;
	int error;

	memset(result, 0, sizeof(*result));
	if (!profile_valid(profile) || state == NULL || bytes == NULL ||
	    ntb_length < DRV_USB_CDC_NCM_NTH16_SIZE ||
	    ntb_length > profile->ntb_in_max_size || ntb_length > UINT16_MAX)
		return EINVAL;
	if (load_le32(bytes) == DRV_USB_CDC_NCM_NTH32_SIGNATURE)
		return EOPNOTSUPP;
	if (load_le32(bytes) != DRV_USB_CDC_NCM_NTH16_SIGNATURE ||
	    load_le16(bytes + 4U) != DRV_USB_CDC_NCM_NTH16_SIZE)
		return EINVAL;
	result->sequence = load_le16(bytes + 6U);
	wire_block_length = load_le16(bytes + 8U);
	if (wire_block_length == 0) {
		/* Zero is legal only when a short USB transfer delimits the NTB. */
		if (ntb_length >= profile->ntb_in_max_size)
			return EINVAL;
		result->block_length = (uint16_t)ntb_length;
	} else {
		if (wire_block_length != ntb_length)
			return EINVAL;
		result->block_length = wire_block_length;
	}
	first_ndp = load_le16(bytes + 10U);
	if (first_ndp == 0)
		return EINVAL;
	error = parse_ndp_chain(profile, bytes, result, first_ndp);
	return error;
}

int
drv_usb_cdc_ncm_parse_ntb16(
	const struct drv_usb_cdc_ncm_profile *profile,
	struct drv_usb_cdc_ncm_rx_state *state, const void *ntb,
	size_t ntb_length, drv_usb_cdc_ncm_datagram_fn deliver,
	void *argument, size_t *datagram_count)
{
	const uint8_t *bytes = ntb;
	struct ncm_parse_result result;
	size_t index;
	int error;

	if (datagram_count != NULL)
		*datagram_count = 0;
	error = validate_ntb16(profile, state, ntb, ntb_length, &result);
	if (error != 0)
		return error;
	if (state->sequence_initialized &&
	    result.sequence != state->expected_sequence &&
	    state->sequence_mismatches != UINT32_MAX)
		state->sequence_mismatches++;
	state->expected_sequence = (uint16_t)(result.sequence + 1U);
	state->sequence_initialized = 1;
	for (index = 0; index < result.datagram_count; index++) {
		if (deliver != NULL) {
			error = deliver(bytes + result.datagrams[index].offset,
			    result.datagrams[index].length, argument);
			if (error != 0) {
				if (datagram_count != NULL)
					*datagram_count = index;
				return error;
			}
		}
	}
	if (datagram_count != NULL)
		*datagram_count = result.datagram_count;
	return 0;
}

int
drv_usb_cdc_ncm_build_ntb16(
	const struct drv_usb_cdc_ncm_profile *profile, uint16_t sequence,
	const void *frame, size_t frame_length, void *ntb,
	size_t ntb_capacity, size_t *ntb_length)
{
	const uint8_t *frame_bytes = frame;
	uint8_t *bytes = ntb;
	struct ncm_layout layout;
	int error;

	if (ntb_length != NULL)
		*ntb_length = 0;
	if (!profile_valid(profile) || frame_bytes == NULL || bytes == NULL ||
	    ntb_length == NULL ||
	    frame_length < DRV_USB_CDC_NCM_ETHERNET_HEADER_SIZE ||
	    frame_length > profile->max_datagram_size)
		return EINVAL;
	error = make_layout(profile->ndp_out_divisor,
	    profile->ndp_out_payload_remainder, profile->ndp_out_alignment,
	    frame_length, profile->bulk_out_max_packet_size,
	    profile->ntb_out_max_is_device_max ?
		profile->ntb_out_max_size : 0, &layout);
	if (error != 0)
		return error;
	if (layout.block_length > ntb_capacity ||
	    layout.block_length > profile->ntb_out_max_size ||
	    layout.block_length > UINT16_MAX ||
	    layout.datagram_offset > UINT16_MAX ||
	    layout.ndp_offset > UINT16_MAX)
		return EMSGSIZE;

	/* memmove first permits a caller-owned frame inside the output buffer. */
	memmove(bytes + layout.datagram_offset, frame_bytes, frame_length);
	memset(bytes, 0, layout.datagram_offset);
	if (layout.block_length > layout.datagram_offset + frame_length)
		memset(bytes + layout.datagram_offset + frame_length, 0,
		    layout.block_length -
			(layout.datagram_offset + frame_length));

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
	return 0;
}
