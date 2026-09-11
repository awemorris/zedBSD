/* CDC NCM NTH16/NDP16 production-code fixture. */
#include <drivers/usb-cdc-ncm.h>

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BUFFER_SIZE 4096U

struct delivery_log {
	uint8_t frames[DRV_USB_CDC_NCM_MAX_RX_DATAGRAMS][64];
	size_t lengths[DRV_USB_CDC_NCM_MAX_RX_DATAGRAMS];
	size_t count;
	int fail_at;
};

static uint16_t
get_le16(const uint8_t *bytes)
{
	return (uint16_t)((uint16_t)bytes[0] |
	    ((uint16_t)bytes[1] << 8));
}

static uint32_t
get_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	    ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void
put_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static void
put_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

static void
make_parameters(uint8_t parameters[DRV_USB_CDC_NCM_NTB_PARAMETERS_SIZE])
{
	memset(parameters, 0, DRV_USB_CDC_NCM_NTB_PARAMETERS_SIZE);
	put_le16(parameters, DRV_USB_CDC_NCM_NTB_PARAMETERS_SIZE);
	put_le16(parameters + 2U, DRV_USB_CDC_NCM_NTB16_SUPPORTED |
	    DRV_USB_CDC_NCM_NTB32_SUPPORTED);
	put_le32(parameters + 4U, 100000U);
	put_le16(parameters + 8U, 4U);
	put_le16(parameters + 10U, 0U);
	put_le16(parameters + 12U, 4U);
	put_le32(parameters + 16U, 8192U);
	put_le16(parameters + 20U, 16U);
	put_le16(parameters + 22U, 6U);
	put_le16(parameters + 24U, 16U);
	put_le16(parameters + 26U, 8U);
}

static struct drv_usb_cdc_ncm_limits
make_limits(void)
{
	const struct drv_usb_cdc_ncm_limits limits = {
	    .ntb_in_max_size = BUFFER_SIZE,
	    .ntb_out_max_size = 3072U,
	    .rx_max_datagrams = 8U,
	    .tx_max_datagrams = 2U,
	    .ndp_chain_max = 4U,
	    .bulk_out_max_packet_size = 64U,
	};

	return limits;
}

static struct drv_usb_cdc_ncm_profile
make_profile(void)
{
	uint8_t parameters[DRV_USB_CDC_NCM_NTB_PARAMETERS_SIZE];
	struct drv_usb_cdc_ncm_limits limits = make_limits();
	struct drv_usb_cdc_ncm_profile profile;

	make_parameters(parameters);
	assert(drv_usb_cdc_ncm_negotiate_nth16(parameters,
	    sizeof(parameters), &limits, &profile) == 0);
	return profile;
}

static int
deliver_frame(const void *frame, size_t length, void *argument)
{
	struct delivery_log *log = argument;

	assert(frame != NULL);
	assert(length >= DRV_USB_CDC_NCM_ETHERNET_HEADER_SIZE);
	assert(length <= DRV_USB_CDC_NCM_MAX_DATAGRAM_SIZE);
	if (log->fail_at >= 0 && log->count == (size_t)log->fail_at)
		return ENOBUFS;
	assert(log->count < DRV_USB_CDC_NCM_MAX_RX_DATAGRAMS);
	log->lengths[log->count] = length;
	memcpy(log->frames[log->count], frame,
	    length < sizeof(log->frames[0]) ? length : sizeof(log->frames[0]));
	log->count++;
	return 0;
}

static void
reset_log(struct delivery_log *log)
{
	memset(log, 0, sizeof(*log));
	log->fail_at = -1;
}

static size_t
make_two_datagram_ntb(uint8_t *bytes, uint16_t sequence)
{
	const uint16_t ndp = 12U;
	const uint16_t first = 34U;
	const uint16_t second = 50U;
	const uint16_t block = 65U;
	size_t index;

	memset(bytes, 0, BUFFER_SIZE);
	put_le32(bytes, DRV_USB_CDC_NCM_NTH16_SIGNATURE);
	put_le16(bytes + 4U, DRV_USB_CDC_NCM_NTH16_SIZE);
	put_le16(bytes + 6U, sequence);
	put_le16(bytes + 8U, block);
	put_le16(bytes + 10U, ndp);
	put_le32(bytes + ndp, DRV_USB_CDC_NCM_NDP16_NOCRC_SIGNATURE);
	put_le16(bytes + ndp + 4U, 20U);
	put_le16(bytes + ndp + 6U, 0U);
	put_le16(bytes + ndp + 8U, first);
	put_le16(bytes + ndp + 10U, 14U);
	put_le16(bytes + ndp + 12U, second);
	put_le16(bytes + ndp + 14U, 15U);
	put_le16(bytes + ndp + 16U, 0U);
	put_le16(bytes + ndp + 18U, 0U);
	for (index = 0; index < 14U; index++)
		bytes[first + index] = (uint8_t)(0x20U + index);
	for (index = 0; index < 15U; index++)
		bytes[second + index] = (uint8_t)(0x80U + index);
	return block;
}

static size_t
make_chained_ntb(uint8_t *bytes, uint16_t sequence)
{
	const uint16_t first_ndp = 12U;
	const uint16_t second_ndp = 28U;
	const uint16_t first = 46U;
	const uint16_t second = 62U;
	const uint16_t block = 76U;

	memset(bytes, 0, BUFFER_SIZE);
	put_le32(bytes, DRV_USB_CDC_NCM_NTH16_SIGNATURE);
	put_le16(bytes + 4U, DRV_USB_CDC_NCM_NTH16_SIZE);
	put_le16(bytes + 6U, sequence);
	put_le16(bytes + 8U, block);
	put_le16(bytes + 10U, first_ndp);

	put_le32(bytes + first_ndp,
	    DRV_USB_CDC_NCM_NDP16_NOCRC_SIGNATURE);
	put_le16(bytes + first_ndp + 4U, 16U);
	put_le16(bytes + first_ndp + 6U, second_ndp);
	put_le16(bytes + first_ndp + 8U, first);
	put_le16(bytes + first_ndp + 10U, 14U);

	put_le32(bytes + second_ndp,
	    DRV_USB_CDC_NCM_NDP16_NOCRC_SIGNATURE);
	put_le16(bytes + second_ndp + 4U, 16U);
	put_le16(bytes + second_ndp + 6U, 0U);
	put_le16(bytes + second_ndp + 8U, second);
	put_le16(bytes + second_ndp + 10U, 14U);
	memset(bytes + first, 0x3c, 14U);
	memset(bytes + second, 0xc3, 14U);
	return block;
}

static void
test_negotiation(void)
{
	uint8_t parameters[DRV_USB_CDC_NCM_NTB_PARAMETERS_SIZE];
	uint8_t changed[DRV_USB_CDC_NCM_NTB_PARAMETERS_SIZE];
	struct drv_usb_cdc_ncm_limits limits = make_limits();
	struct drv_usb_cdc_ncm_profile profile;
	struct drv_usb_cdc_ncm_control_request request;

	make_parameters(parameters);
	assert(drv_usb_cdc_ncm_negotiate_nth16(parameters,
	    sizeof(parameters), &limits, &profile) == 0);
	assert(profile.ntb_in_max_size == BUFFER_SIZE);
	assert(profile.ntb_out_max_size == 3072U);
	assert(profile.ndp_in_divisor == 4U);
	assert(profile.ndp_out_divisor == 16U);
	assert(profile.ndp_out_payload_remainder == 6U);
	assert(profile.ndp_out_alignment == 16U);
	assert(profile.rx_max_datagrams == 8U);
	assert(profile.tx_max_datagrams == 2U);
	assert(profile.ndp_chain_max == 4U);
	assert(profile.max_datagram_size == 1514U);
	assert(profile.bulk_out_max_packet_size == 64U);
	assert(profile.set_ntb_format_required == 1U);
	assert(profile.ntb_out_max_is_device_max == 0);

	assert(drv_usb_cdc_ncm_make_control_request(&profile,
	    DRV_USB_CDC_NCM_CONTROL_SELECT_NTH16, &request) == 0);
	assert(request.request == DRV_USB_CDC_NCM_SET_NTB_FORMAT);
	assert(request.value == 0 && request.length == 0);
	assert(drv_usb_cdc_ncm_make_control_request(&profile,
	    DRV_USB_CDC_NCM_CONTROL_SET_INPUT_SIZE, &request) == 0);
	assert(request.request == DRV_USB_CDC_NCM_SET_NTB_INPUT_SIZE);
	assert(request.value == 0 && request.length == 4U);
	assert(get_le32(request.payload) == BUFFER_SIZE);
	assert(drv_usb_cdc_ncm_make_control_request(&profile,
	    DRV_USB_CDC_NCM_CONTROL_SET_MAX_DATAGRAM_SIZE, &request) == 0);
	assert(request.request == DRV_USB_CDC_NCM_SET_MAX_DATAGRAM_SIZE);
	assert(request.value == 0 && request.length == 2U);
	assert(get_le16(request.payload) == 1514U);
	assert(drv_usb_cdc_ncm_make_control_request(&profile,
	    DRV_USB_CDC_NCM_CONTROL_DISABLE_CRC, &request) == 0);
	assert(request.request == DRV_USB_CDC_NCM_SET_CRC_MODE);
	assert(request.value == 0 && request.length == 0);
	assert(drv_usb_cdc_ncm_make_control_request(&profile,
	    (enum drv_usb_cdc_ncm_control_step)99, &request) == EOPNOTSUPP);
	assert(drv_usb_cdc_ncm_make_control_request(&profile,
	    DRV_USB_CDC_NCM_CONTROL_DISABLE_CRC, NULL) == EINVAL);

	memcpy(changed, parameters, sizeof(changed));
	put_le16(changed + 2U, DRV_USB_CDC_NCM_NTB32_SUPPORTED);
	assert(drv_usb_cdc_ncm_negotiate_nth16(changed, sizeof(changed),
	    &limits, &profile) == EOPNOTSUPP);
	memcpy(changed, parameters, sizeof(changed));
	put_le16(changed + 2U, DRV_USB_CDC_NCM_NTB16_SUPPORTED | 0x8000U);
	assert(drv_usb_cdc_ncm_negotiate_nth16(changed, sizeof(changed),
	    &limits, &profile) == 0);
	assert(profile.set_ntb_format_required == 0);
	assert(drv_usb_cdc_ncm_make_control_request(&profile,
	    DRV_USB_CDC_NCM_CONTROL_SELECT_NTH16, &request) == EOPNOTSUPP);
	memcpy(changed, parameters, sizeof(changed));
	put_le16(changed, 29U);
	assert(drv_usb_cdc_ncm_negotiate_nth16(changed, sizeof(changed),
	    &limits, &profile) == EOPNOTSUPP);
	assert(drv_usb_cdc_ncm_negotiate_nth16(parameters,
	    sizeof(parameters) - 1U, &limits, &profile) == EINVAL);
	memcpy(changed, parameters, sizeof(changed));
	put_le16(changed + 14U, 1U);
	assert(drv_usb_cdc_ncm_negotiate_nth16(changed, sizeof(changed),
	    &limits, &profile) == 0);

	memcpy(changed, parameters, sizeof(changed));
	put_le32(changed + 4U, 2047U);
	assert(drv_usb_cdc_ncm_negotiate_nth16(changed, sizeof(changed),
	    &limits, &profile) == EINVAL);
	memcpy(changed, parameters, sizeof(changed));
	put_le32(changed + 16U, 1600U);
	limits.ntb_out_max_size = 1600U;
	assert(drv_usb_cdc_ncm_negotiate_nth16(changed, sizeof(changed),
	    &limits, &profile) == 0);
	assert(profile.ntb_out_max_size == 1600U);
	assert(profile.ntb_out_max_is_device_max == 1U);
	memcpy(changed, parameters, sizeof(changed));
	put_le32(changed + 16U, 0U);
	assert(drv_usb_cdc_ncm_negotiate_nth16(changed, sizeof(changed),
	    &limits, &profile) == EINVAL);
	limits = make_limits();
	limits.ntb_in_max_size = 2047U;
	assert(drv_usb_cdc_ncm_negotiate_nth16(parameters,
	    sizeof(parameters), &limits, &profile) == EINVAL);
	limits = make_limits();
	limits.rx_max_datagrams = 0;
	assert(drv_usb_cdc_ncm_negotiate_nth16(parameters,
	    sizeof(parameters), &limits, &profile) == EINVAL);
	limits = make_limits();
	limits.ndp_chain_max = DRV_USB_CDC_NCM_MAX_NDP_CHAIN + 1U;
	assert(drv_usb_cdc_ncm_negotiate_nth16(parameters,
	    sizeof(parameters), &limits, &profile) == EINVAL);
	limits = make_limits();
	limits.bulk_out_max_packet_size = 0;
	assert(drv_usb_cdc_ncm_negotiate_nth16(parameters,
	    sizeof(parameters), &limits, &profile) == EINVAL);
	limits = make_limits();
	limits.bulk_out_max_packet_size = 48U;
	assert(drv_usb_cdc_ncm_negotiate_nth16(parameters,
	    sizeof(parameters), &limits, &profile) == EINVAL);

	limits = make_limits();
	memcpy(changed, parameters, sizeof(changed));
	put_le16(changed + 8U, 0U);
	assert(drv_usb_cdc_ncm_negotiate_nth16(changed, sizeof(changed),
	    &limits, &profile) == EINVAL);
	limits = make_limits();
	memcpy(changed, parameters, sizeof(changed));
	put_le16(changed + 8U, 6U);
	assert(drv_usb_cdc_ncm_negotiate_nth16(changed, sizeof(changed),
	    &limits, &profile) == 0);
	assert(profile.ndp_in_divisor == 6U);
	put_le16(changed + 8U, 1U);
	put_le16(changed + 10U, 0U);
	assert(drv_usb_cdc_ncm_negotiate_nth16(changed, sizeof(changed),
	    &limits, &profile) == 0);
	memcpy(changed, parameters, sizeof(changed));
	put_le16(changed + 10U, 4U);
	assert(drv_usb_cdc_ncm_negotiate_nth16(changed, sizeof(changed),
	    &limits, &profile) == EINVAL);
	memcpy(changed, parameters, sizeof(changed));
	put_le16(changed + 12U, 2U);
	assert(drv_usb_cdc_ncm_negotiate_nth16(changed, sizeof(changed),
	    &limits, &profile) == EINVAL);
	memcpy(changed, parameters, sizeof(changed));
	put_le16(changed + 24U, 6U);
	assert(drv_usb_cdc_ncm_negotiate_nth16(changed, sizeof(changed),
	    &limits, &profile) == EINVAL);

	/* A zero device TX-datagram limit means unlimited, then resource-clamp. */
	memcpy(changed, parameters, sizeof(changed));
	put_le16(changed + 26U, 0U);
	assert(drv_usb_cdc_ncm_negotiate_nth16(changed, sizeof(changed),
	    &limits, &profile) == 0);
	assert(profile.tx_max_datagrams == limits.tx_max_datagrams);
	put_le16(changed + 26U, 1U);
	assert(drv_usb_cdc_ncm_negotiate_nth16(changed, sizeof(changed),
	    &limits, &profile) == 0);
	assert(profile.tx_max_datagrams == 1U);

	/* Large device values remain bounded by the HCD and NTH16. */
	memcpy(changed, parameters, sizeof(changed));
	put_le32(changed + 4U, UINT32_MAX);
	put_le32(changed + 16U, UINT32_MAX);
	limits.ntb_in_max_size = UINT32_MAX;
	limits.ntb_out_max_size = UINT32_MAX;
	assert(drv_usb_cdc_ncm_negotiate_nth16(changed, sizeof(changed),
	    &limits, &profile) == 0);
	assert(profile.ntb_in_max_size == UINT16_MAX);
	assert(profile.ntb_out_max_size == UINT16_MAX);
	assert(profile.ntb_out_max_is_device_max == 0);

	/* Legal fields that cannot fit an MTU-sized NTB fail negotiation. */
	make_parameters(changed);
	put_le32(changed + 4U, 2048U);
	put_le16(changed + 8U, 1024U);
	put_le16(changed + 10U, 900U);
	limits = make_limits();
	limits.ntb_in_max_size = 2048U;
	assert(drv_usb_cdc_ncm_negotiate_nth16(changed, sizeof(changed),
	    &limits, &profile) == EMSGSIZE);
}

static void
test_exact_device_max_without_zlp(void)
{
	uint8_t parameters[DRV_USB_CDC_NCM_NTB_PARAMETERS_SIZE];
	uint8_t frame[DRV_USB_CDC_NCM_MAX_DATAGRAM_SIZE];
	uint8_t ntb[DRV_USB_CDC_NCM_NTB_MIN_SIZE];
	struct drv_usb_cdc_ncm_limits limits = make_limits();
	struct drv_usb_cdc_ncm_profile profile;
	size_t used;

	/* The NDP-before-data layout is exactly the advertised 2048-byte max. */
	make_parameters(parameters);
	put_le32(parameters + 16U, DRV_USB_CDC_NCM_NTB_MIN_SIZE);
	put_le16(parameters + 20U, 1024U);
	put_le16(parameters + 22U, 548U);
	put_le16(parameters + 24U, 4U);
	limits.ntb_out_max_size = DRV_USB_CDC_NCM_NTB_MIN_SIZE;
	assert(drv_usb_cdc_ncm_negotiate_nth16(parameters,
	    sizeof(parameters), &limits, &profile) == 0);
	assert(profile.ntb_out_max_size == DRV_USB_CDC_NCM_NTB_MIN_SIZE);
	assert(profile.ntb_out_max_is_device_max == 1U);

	memset(frame, 0x6d, sizeof(frame));
	assert(drv_usb_cdc_ncm_build_ntb16(&profile, 0, frame,
	    sizeof(frame), ntb, sizeof(ntb), &used) == 0);
	assert(used == DRV_USB_CDC_NCM_NTB_MIN_SIZE);
	assert(used % profile.bulk_out_max_packet_size == 0);
	assert(get_le16(ntb + 8U) == used);

	/* A same-sized local clamp is not the device's transfer boundary. */
	profile.ntb_out_max_is_device_max = 0;
	assert(drv_usb_cdc_ncm_build_ntb16(&profile, 0, frame,
	    sizeof(frame), ntb, sizeof(ntb), &used) == EMSGSIZE);
	assert(used == 0);

	put_le32(parameters + 16U, 4096U);
	assert(drv_usb_cdc_ncm_negotiate_nth16(parameters,
	    sizeof(parameters), &limits, &profile) == EMSGSIZE);
}

static void
test_build_and_round_trip(void)
{
	struct drv_usb_cdc_ncm_profile profile = make_profile();
	struct drv_usb_cdc_ncm_profile after_profile;
	struct drv_usb_cdc_ncm_rx_state state;
	struct delivery_log log;
	uint8_t frame[64], maximum_frame[DRV_USB_CDC_NCM_MAX_DATAGRAM_SIZE];
	uint8_t ntb[BUFFER_SIZE], alias[BUFFER_SIZE];
	size_t used, count, index, ndp, datagram;

	for (index = 0; index < sizeof(frame); index++)
		frame[index] = (uint8_t)(index ^ 0x5aU);
	assert(drv_usb_cdc_ncm_build_ntb16(&profile, 0, frame,
	    sizeof(frame), ntb, sizeof(ntb), &used) == 0);
	assert(get_le32(ntb) == DRV_USB_CDC_NCM_NTH16_SIGNATURE);
	assert(get_le16(ntb + 4U) == DRV_USB_CDC_NCM_NTH16_SIZE);
	assert(get_le16(ntb + 6U) == 0);
	assert(get_le16(ntb + 8U) == used);
	ndp = get_le16(ntb + 10U);
	assert((ndp & 3U) == 0 && ndp % profile.ndp_out_alignment == 0);
	assert(get_le32(ntb + ndp) ==
	    DRV_USB_CDC_NCM_NDP16_NOCRC_SIGNATURE);
	assert(get_le16(ntb + ndp + 4U) ==
	    DRV_USB_CDC_NCM_NDP16_MIN_SIZE);
	datagram = get_le16(ntb + ndp + 8U);
	assert((datagram + DRV_USB_CDC_NCM_ETHERNET_HEADER_SIZE) %
	    profile.ndp_out_divisor == profile.ndp_out_payload_remainder);
	assert(get_le16(ntb + ndp + 10U) == sizeof(frame));
	assert(get_le16(ntb + ndp + 12U) == 0);
	assert(get_le16(ntb + ndp + 14U) == 0);
	assert(memcmp(ntb + datagram, frame, sizeof(frame)) == 0);

	/* The receive constraints are independent; use matching values here. */
	profile.ndp_in_divisor = profile.ndp_out_divisor;
	profile.ndp_in_payload_remainder = profile.ndp_out_payload_remainder;
	profile.ndp_in_alignment = profile.ndp_out_alignment;
	drv_usb_cdc_ncm_rx_reset(&state);
	reset_log(&log);
	assert(drv_usb_cdc_ncm_parse_ntb16(&profile, &state, ntb, used,
	    deliver_frame, &log, &count) == 0);
	assert(count == 1U && log.count == 1U);
	assert(log.lengths[0] == sizeof(frame));
	assert(memcmp(log.frames[0], frame, sizeof(frame)) == 0);
	assert(state.expected_sequence == 1U);

	/* Aliased input is supported so a later driver may reuse a TX buffer. */
	memset(alias, 0xa5, sizeof(alias));
	memcpy(alias, frame, sizeof(frame));
	assert(drv_usb_cdc_ncm_build_ntb16(&profile, 1, alias,
	    sizeof(frame), alias, sizeof(alias), &used) == 0);
	ndp = get_le16(alias + 10U);
	datagram = get_le16(alias + ndp + 8U);
	assert(ndp < datagram);
	assert(memcmp(alias + datagram, frame, sizeof(frame)) == 0);

	/* Exercise aliased input with the datagram-before-NDP layout as well. */
	after_profile = profile;
	after_profile.ndp_out_divisor = 4U;
	after_profile.ndp_out_payload_remainder = 0U;
	after_profile.ndp_out_alignment = 4U;
	memset(alias, 0xa5, sizeof(alias));
	memcpy(alias, frame, 14U);
	assert(drv_usb_cdc_ncm_build_ntb16(&after_profile, 1, alias, 14U,
	    alias, sizeof(alias), &used) == 0);
	ndp = get_le16(alias + 10U);
	datagram = get_le16(alias + ndp + 8U);
	assert(ndp > datagram);
	assert(memcmp(alias + datagram, frame, 14U) == 0);

	/* A max-MTU frame is legal in both directions. */
	memset(maximum_frame, 0x69, sizeof(maximum_frame));
	assert(drv_usb_cdc_ncm_build_ntb16(&profile, 0, maximum_frame,
	    sizeof(maximum_frame), ntb, sizeof(ntb), &used) == 0);
	drv_usb_cdc_ncm_rx_reset(&state);
	reset_log(&log);
	assert(drv_usb_cdc_ncm_parse_ntb16(&profile, &state, ntb, used,
	    deliver_frame, &log, &count) == 0);
	assert(count == 1U && log.lengths[0] == sizeof(maximum_frame));

	/* Exact max-packet lengths receive a trailing byte instead of a ZLP. */
	assert(drv_usb_cdc_ncm_build_ntb16(&profile, 0, frame, 24U, ntb,
	    sizeof(ntb), &used) == 0);
	assert(used == 65U);
	assert(used % profile.bulk_out_max_packet_size != 0);
	assert(ntb[used - 1U] == 0);
	assert(drv_usb_cdc_ncm_build_ntb16(&profile, 0, frame, 24U, ntb,
	    used - 1U, &count) == EMSGSIZE);
	assert(count == 0);
	assert(drv_usb_cdc_ncm_build_ntb16(&profile, 0, frame, 24U, ntb,
	    used, &count) == 0);
	assert(count == used);

	assert(drv_usb_cdc_ncm_build_ntb16(&profile, 0, frame, 13U,
	    ntb, sizeof(ntb), &used) == EINVAL);
	assert(used == 0);
	assert(drv_usb_cdc_ncm_build_ntb16(&profile, 0, frame,
	    profile.max_datagram_size + 1U, ntb, sizeof(ntb), &used) ==
	    EINVAL);
	assert(drv_usb_cdc_ncm_build_ntb16(&profile, 0, frame,
	    sizeof(frame), ntb, 20U, &used) == EMSGSIZE);
	assert(drv_usb_cdc_ncm_build_ntb16(&profile, 0, frame,
	    sizeof(frame), ntb, sizeof(ntb), NULL) == EINVAL);
}

static void
test_multiple_and_chained(void)
{
	struct drv_usb_cdc_ncm_profile profile = make_profile();
	struct drv_usb_cdc_ncm_rx_state state;
	struct delivery_log log;
	uint8_t ntb[BUFFER_SIZE];
	size_t length, count;

	length = make_two_datagram_ntb(ntb, 0);
	drv_usb_cdc_ncm_rx_reset(&state);
	reset_log(&log);
	assert(drv_usb_cdc_ncm_parse_ntb16(&profile, &state, ntb, length,
	    deliver_frame, &log, &count) == 0);
	assert(count == 2U && log.count == 2U);
	assert(log.lengths[0] == 14U && log.lengths[1] == 15U);
	assert(log.frames[0][0] == 0x20U && log.frames[1][0] == 0x80U);

	length = make_chained_ntb(ntb, 0);
	drv_usb_cdc_ncm_rx_reset(&state);
	reset_log(&log);
	assert(drv_usb_cdc_ncm_parse_ntb16(&profile, &state, ntb, length,
	    deliver_frame, &log, &count) == 0);
	assert(count == 2U && log.count == 2U);
	assert(log.frames[0][0] == 0x3cU && log.frames[1][0] == 0xc3U);

	/* Entries after the first null DPE are intentionally ignored. */
	length = make_two_datagram_ntb(ntb, 0);
	put_le16(ntb + 24U, 0U);
	put_le16(ntb + 26U, 0U);
	put_le16(ntb + 28U, UINT16_MAX);
	put_le16(ntb + 30U, UINT16_MAX);
	drv_usb_cdc_ncm_rx_reset(&state);
	reset_log(&log);
	assert(drv_usb_cdc_ncm_parse_ntb16(&profile, &state, ntb, length,
	    deliver_frame, &log, &count) == 0);
	assert(count == 1U && log.count == 1U);

	/* Either zero field makes a DPE null; an all-null NTB is harmless. */
	length = make_two_datagram_ntb(ntb, 0);
	put_le16(ntb + 24U, 0U);
	drv_usb_cdc_ncm_rx_reset(&state);
	reset_log(&log);
	assert(drv_usb_cdc_ncm_parse_ntb16(&profile, &state, ntb, length,
	    deliver_frame, &log, &count) == 0);
	assert(count == 1U && log.count == 1U);
	length = make_two_datagram_ntb(ntb, 0);
	put_le16(ntb + 26U, 0U);
	drv_usb_cdc_ncm_rx_reset(&state);
	reset_log(&log);
	assert(drv_usb_cdc_ncm_parse_ntb16(&profile, &state, ntb, length,
	    deliver_frame, &log, &count) == 0);
	assert(count == 1U && log.count == 1U);
	length = make_two_datagram_ntb(ntb, 0);
	put_le16(ntb + 20U, 0U);
	drv_usb_cdc_ncm_rx_reset(&state);
	reset_log(&log);
	assert(drv_usb_cdc_ncm_parse_ntb16(&profile, &state, ntb, length,
	    deliver_frame, &log, &count) == 0);
	assert(count == 0 && log.count == 0);
	assert(state.expected_sequence == 1U);
}

static void
expect_rejected(const struct drv_usb_cdc_ncm_profile *profile,
	const uint8_t *ntb, size_t length)
{
	struct drv_usb_cdc_ncm_rx_state state;
	struct delivery_log log;
	size_t count = 99U;

	drv_usb_cdc_ncm_rx_reset(&state);
	reset_log(&log);
	assert(drv_usb_cdc_ncm_parse_ntb16(profile, &state, ntb, length,
	    deliver_frame, &log, &count) != 0);
	assert(log.count == 0);
	assert(count == 0);
	assert(state.expected_sequence == 0);
	assert(state.sequence_initialized == 0);
	assert(state.sequence_mismatches == 0);
}

static void
test_malformed(void)
{
	struct drv_usb_cdc_ncm_profile profile = make_profile();
	struct drv_usb_cdc_ncm_profile limited;
	uint8_t base[BUFFER_SIZE], changed[BUFFER_SIZE];
	size_t length;

	length = make_two_datagram_ntb(base, 0);

#define REJECT_CHANGE(statement) do { \
	memcpy(changed, base, sizeof(changed)); \
	statement; \
	expect_rejected(&profile, changed, length); \
} while (0)

	REJECT_CHANGE(changed[0] ^= 1U);
	REJECT_CHANGE(put_le32(changed, DRV_USB_CDC_NCM_NTH32_SIGNATURE));
	REJECT_CHANGE(put_le16(changed + 4U, 16U));
	REJECT_CHANGE(put_le16(changed + 8U, (uint16_t)(length - 1U)));
	REJECT_CHANGE(put_le16(changed + 10U, 0U));
	REJECT_CHANGE(put_le16(changed + 10U, 14U));
	REJECT_CHANGE(put_le16(changed + 10U, UINT16_MAX));
	REJECT_CHANGE(put_le32(changed + 12U,
	    DRV_USB_CDC_NCM_NDP16_CRC_SIGNATURE));
	REJECT_CHANGE(put_le32(changed + 12U, UINT32_C(0x12345678)));
	REJECT_CHANGE(put_le16(changed + 16U, 12U));
	REJECT_CHANGE(put_le16(changed + 16U, 18U));
	REJECT_CHANGE(put_le16(changed + 16U, UINT16_MAX));
	REJECT_CHANGE(put_le16(changed + 26U, 1U));
	REJECT_CHANGE(put_le16(changed + 22U, 13U));
	REJECT_CHANGE(put_le16(changed + 22U, 1515U));
	REJECT_CHANGE(put_le16(changed + 20U, 35U));
	REJECT_CHANGE(put_le16(changed + 20U, UINT16_MAX));
	REJECT_CHANGE(put_le16(changed + 24U, 34U));

#undef REJECT_CHANGE
	memcpy(changed, base, sizeof(changed));
	put_le16(changed + 28U, 34U);
	put_le16(changed + 30U, 14U);
	expect_rejected(&profile, changed, length);

	/* Self-looping and over-budget NDP chains. */
	length = make_chained_ntb(changed, 0);
	put_le16(changed + 18U, 12U);
	expect_rejected(&profile, changed, length);
	length = make_chained_ntb(changed, 0);
	limited = profile;
	limited.ndp_chain_max = 1U;
	expect_rejected(&limited, changed, length);

	/* Two DPEs may not alias the same datagram. */
	length = make_two_datagram_ntb(changed, 0);
	put_le16(changed + 24U, 34U);
	put_le16(changed + 26U, 14U);
	expect_rejected(&profile, changed, length);
	length = make_two_datagram_ntb(changed, 0);
	put_le16(changed + 24U, 46U);
	expect_rejected(&profile, changed, length);
	limited = profile;
	limited.rx_max_datagrams = 1U;
	expect_rejected(&limited, changed, length);

	/* Datagram-vs-NDP and NDP-vs-NDP overlap are rejected explicitly. */
	length = make_two_datagram_ntb(changed, 0);
	limited = profile;
	limited.ndp_in_payload_remainder = 2U;
	put_le16(changed + 20U, 12U);
	put_le16(changed + 24U, 0U);
	put_le16(changed + 26U, 0U);
	expect_rejected(&limited, changed, length);

	memset(changed, 0, sizeof(changed));
	put_le32(changed, DRV_USB_CDC_NCM_NTH16_SIGNATURE);
	put_le16(changed + 4U, DRV_USB_CDC_NCM_NTH16_SIZE);
	put_le16(changed + 8U, 60U);
	put_le16(changed + 10U, 12U);
	put_le32(changed + 12U, DRV_USB_CDC_NCM_NDP16_NOCRC_SIGNATURE);
	put_le16(changed + 16U, 20U);
	put_le16(changed + 18U, 28U);
	put_le32(changed + 28U, DRV_USB_CDC_NCM_NDP16_NOCRC_SIGNATURE);
	put_le16(changed + 32U, 16U);
	put_le16(changed + 36U, 46U);
	put_le16(changed + 38U, 14U);
	memset(changed + 46U, 0x77, 14U);
	expect_rejected(&profile, changed, 60U);

	/* A zero block length needs an actual USB short transfer. */
	length = make_two_datagram_ntb(changed, 0);
	put_le16(changed + 8U, 0U);
	{
		struct drv_usb_cdc_ncm_rx_state state;
		size_t count;

		drv_usb_cdc_ncm_rx_reset(&state);
		assert(drv_usb_cdc_ncm_parse_ntb16(&profile, &state, changed,
		    length, NULL, NULL, &count) == 0);
		assert(count == 2U);
	}
	memset(changed + length, 0, profile.ntb_in_max_size - length);
	expect_rejected(&profile, changed, profile.ntb_in_max_size);

	expect_rejected(&profile, base, length - 1U);
	expect_rejected(&profile, base, profile.ntb_in_max_size + 1U);
}

static void
test_sequence_and_callback(void)
{
	struct drv_usb_cdc_ncm_profile profile = make_profile();
	struct drv_usb_cdc_ncm_rx_state state, state_before;
	struct delivery_log log;
	uint8_t ntb[BUFFER_SIZE];
	size_t length, count;

	/* A malformed predecessor cannot initialize the sequence transaction. */
	drv_usb_cdc_ncm_rx_reset(&state);
	state_before = state;
	length = make_two_datagram_ntb(ntb, 99U);
	ntb[0] ^= 1U;
	reset_log(&log);
	assert(drv_usb_cdc_ncm_parse_ntb16(&profile, &state, ntb, length,
	    deliver_frame, &log, &count) != 0);
	assert(log.count == 0 && count == 0);
	assert(memcmp(&state, &state_before, sizeof(state)) == 0);

	/* The first completely valid NTB establishes the wire baseline even when
	 * the function did not start at sequence zero. */
	length = make_two_datagram_ntb(ntb, 37U);
	reset_log(&log);
	assert(drv_usb_cdc_ncm_parse_ntb16(&profile, &state, ntb, length,
	    deliver_frame, &log, &count) == 0);
	assert(state.sequence_initialized == 1U);
	assert(state.expected_sequence == 38U);
	assert(state.sequence_mismatches == 0);

	/* A fully valid gap is consumed and immediately resynchronizes. */
	length = make_two_datagram_ntb(ntb, 41U);
	reset_log(&log);
	assert(drv_usb_cdc_ncm_parse_ntb16(&profile, &state, ntb, length,
	    deliver_frame, &log, &count) == 0);
	assert(state.expected_sequence == 42U);
	assert(state.sequence_mismatches == 1U);

	/* Structural failure must not move either sequence field, even when its
	 * wire sequence also differs from the current baseline. */
	length = make_two_datagram_ntb(ntb, 99U);
	ntb[0] ^= 1U;
	state_before = state;
	reset_log(&log);
	assert(drv_usb_cdc_ncm_parse_ntb16(&profile, &state, ntb, length,
	    deliver_frame, &log, &count) != 0);
	assert(log.count == 0 && count == 0);
	assert(memcmp(&state, &state_before, sizeof(state)) == 0);

	length = make_two_datagram_ntb(ntb, 42U);
	reset_log(&log);
	assert(drv_usb_cdc_ncm_parse_ntb16(&profile, &state, ntb, length,
	    deliver_frame, &log, &count) == 0);
	assert(state.expected_sequence == 43U);
	assert(state.sequence_mismatches == 1U);
	/* A backward mismatch follows the same delivery and resync policy. */
	length = make_two_datagram_ntb(ntb, 7U);
	reset_log(&log);
	assert(drv_usb_cdc_ncm_parse_ntb16(&profile, &state, ntb, length,
	    deliver_frame, &log, &count) == 0);
	assert(state.expected_sequence == 8U);
	assert(state.sequence_mismatches == 2U);

	/* Sequence wrap is ordinary synchronized progression. */
	length = make_two_datagram_ntb(ntb, UINT16_MAX);
	reset_log(&log);
	assert(drv_usb_cdc_ncm_parse_ntb16(&profile, &state, ntb, length,
	    deliver_frame, &log, &count) == 0);
	assert(state.expected_sequence == 0);
	assert(state.sequence_mismatches == 3U);
	length = make_two_datagram_ntb(ntb, 0);
	reset_log(&log);
	assert(drv_usb_cdc_ncm_parse_ntb16(&profile, &state, ntb, length,
	    deliver_frame, &log, &count) == 0);
	assert(state.expected_sequence == 1U);
	assert(state.sequence_mismatches == 3U);

	/* UINT16_MAX is also a valid first accepted sequence, not a sentinel. */
	drv_usb_cdc_ncm_rx_reset(&state);
	length = make_two_datagram_ntb(ntb, UINT16_MAX);
	reset_log(&log);
	assert(drv_usb_cdc_ncm_parse_ntb16(&profile, &state, ntb, length,
	    deliver_frame, &log, &count) == 0);
	assert(state.expected_sequence == 0);
	assert(state.sequence_initialized == 1U);
	assert(state.sequence_mismatches == 0);

	drv_usb_cdc_ncm_rx_reset(&state);
	length = make_two_datagram_ntb(ntb, 0);
	reset_log(&log);
	log.fail_at = 1;
	assert(drv_usb_cdc_ncm_parse_ntb16(&profile, &state, ntb, length,
	    deliver_frame, &log, &count) == ENOBUFS);
	assert(log.count == 1U && count == 1U);
	assert(state.expected_sequence == 1U);
	assert(state.sequence_initialized == 1U);
	assert(state.sequence_mismatches == 0);
}

static void
test_deterministic_mutations(void)
{
	static const uint8_t masks[] = {1U, 0x80U, 0xffU};
	struct drv_usb_cdc_ncm_profile profile = make_profile();
	struct drv_usb_cdc_ncm_rx_state state;
	struct delivery_log log;
	uint8_t frame[64], base[BUFFER_SIZE], changed[BUFFER_SIZE];
	size_t used, index, mask_index, count;

	memset(frame, 0x5a, sizeof(frame));
	profile.ndp_in_divisor = profile.ndp_out_divisor;
	profile.ndp_in_payload_remainder = profile.ndp_out_payload_remainder;
	profile.ndp_in_alignment = profile.ndp_out_alignment;
	assert(drv_usb_cdc_ncm_build_ntb16(&profile, 0, frame,
	    sizeof(frame), base, sizeof(base), &used) == 0);
	for (index = 0; index < used; index++) {
		for (mask_index = 0; mask_index < sizeof(masks); mask_index++) {
			memcpy(changed, base, used);
			changed[index] ^= masks[mask_index];
			drv_usb_cdc_ncm_rx_reset(&state);
			reset_log(&log);
			(void)drv_usb_cdc_ncm_parse_ntb16(&profile, &state,
			    changed, used, deliver_frame, &log, &count);
		}
	}
	for (index = 0; index < used + 2U; index++) {
		drv_usb_cdc_ncm_rx_reset(&state);
		reset_log(&log);
		(void)drv_usb_cdc_ncm_parse_ntb16(&profile, &state, base,
		    index, deliver_frame, &log, &count);
	}
}

int
main(void)
{
	test_negotiation();
	test_exact_device_max_without_zlp();
	test_build_and_round_trip();
	test_multiple_and_chained();
	test_malformed();
	test_sequence_and_callback();
	test_deterministic_mutations();
	puts("USB CDC NCM NTH16/NDP16 wire codec: PASS");
	return 0;
}
