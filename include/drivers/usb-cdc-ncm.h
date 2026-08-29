/*
 * USB CDC NCM NTH16/NDP16 wire codec
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#ifndef ZEDBSD_DRIVERS_USB_CDC_NCM_H
#define ZEDBSD_DRIVERS_USB_CDC_NCM_H

#include <stddef.h>
#include <stdint.h>

#define DRV_USB_CDC_NCM_MTU 1500U
#define DRV_USB_CDC_NCM_ETHERNET_HEADER_SIZE 14U
#define DRV_USB_CDC_NCM_MAX_DATAGRAM_SIZE \
	(DRV_USB_CDC_NCM_ETHERNET_HEADER_SIZE + DRV_USB_CDC_NCM_MTU)

#define DRV_USB_CDC_NCM_NTB_PARAMETERS_SIZE 28U
#define DRV_USB_CDC_NCM_NTB_MIN_SIZE 2048U
#define DRV_USB_CDC_NCM_NTH16_SIZE 12U
#define DRV_USB_CDC_NCM_NDP16_HEADER_SIZE 8U
#define DRV_USB_CDC_NCM_DPE16_SIZE 4U
#define DRV_USB_CDC_NCM_NDP16_MIN_SIZE 16U

#define DRV_USB_CDC_NCM_NTH16_SIGNATURE UINT32_C(0x484d434e)
#define DRV_USB_CDC_NCM_NTH32_SIGNATURE UINT32_C(0x686d636e)
#define DRV_USB_CDC_NCM_NDP16_NOCRC_SIGNATURE UINT32_C(0x304d434e)
#define DRV_USB_CDC_NCM_NDP16_CRC_SIGNATURE UINT32_C(0x314d434e)

#define DRV_USB_CDC_NCM_NTB16_SUPPORTED UINT16_C(0x0001)
#define DRV_USB_CDC_NCM_NTB32_SUPPORTED UINT16_C(0x0002)

#define DRV_USB_CDC_NCM_GET_NTB_PARAMETERS UINT8_C(0x80)
#define DRV_USB_CDC_NCM_GET_NTB_FORMAT UINT8_C(0x83)
#define DRV_USB_CDC_NCM_SET_NTB_FORMAT UINT8_C(0x84)
#define DRV_USB_CDC_NCM_GET_NTB_INPUT_SIZE UINT8_C(0x85)
#define DRV_USB_CDC_NCM_SET_NTB_INPUT_SIZE UINT8_C(0x86)
#define DRV_USB_CDC_NCM_GET_MAX_DATAGRAM_SIZE UINT8_C(0x87)
#define DRV_USB_CDC_NCM_SET_MAX_DATAGRAM_SIZE UINT8_C(0x88)
#define DRV_USB_CDC_NCM_GET_CRC_MODE UINT8_C(0x89)
#define DRV_USB_CDC_NCM_SET_CRC_MODE UINT8_C(0x8a)

#define DRV_USB_CDC_NCM_MAX_NDP_CHAIN 8U
#define DRV_USB_CDC_NCM_MAX_RX_DATAGRAMS 32U
#define DRV_USB_CDC_NCM_MAX_TX_DATAGRAMS 32U

/*
 * Resource limits owned by the driver/HCD, not by the USB function.  The
 * negotiation result never exceeds these limits or the NTH16 wire limit.
 * bulk_out_max_packet_size comes from the selected bulk-OUT endpoint.
 */
struct drv_usb_cdc_ncm_limits {
	uint32_t ntb_in_max_size;
	uint32_t ntb_out_max_size;
	uint16_t rx_max_datagrams;
	uint16_t tx_max_datagrams;
	uint16_t ndp_chain_max;
	uint16_t bulk_out_max_packet_size;
};

/*
 * IN is device-to-host and OUT is host-to-device, as named by CDC NCM.
 * The alignment fields are retained in their wire direction so the USB
 * driver cannot accidentally use the device's receive constraints for TX.
 */
struct drv_usb_cdc_ncm_profile {
	uint32_t ntb_in_max_size;
	uint32_t ntb_out_max_size;
	uint16_t ndp_in_divisor;
	uint16_t ndp_in_payload_remainder;
	uint16_t ndp_in_alignment;
	uint16_t ndp_out_divisor;
	uint16_t ndp_out_payload_remainder;
	uint16_t ndp_out_alignment;
	uint16_t rx_max_datagrams;
	uint16_t tx_max_datagrams;
	uint16_t ndp_chain_max;
	uint16_t max_datagram_size;
	uint16_t bulk_out_max_packet_size;
	uint8_t set_ntb_format_required;
	/* True only when ntb_out_max_size is the function's advertised maximum. */
	uint8_t ntb_out_max_is_device_max;
};

struct drv_usb_cdc_ncm_rx_state {
	uint16_t expected_sequence;
	uint8_t sequence_initialized;
	uint32_t sequence_mismatches;
};

enum drv_usb_cdc_ncm_control_step {
	DRV_USB_CDC_NCM_CONTROL_SELECT_NTH16 = 0,
	DRV_USB_CDC_NCM_CONTROL_SET_INPUT_SIZE,
	DRV_USB_CDC_NCM_CONTROL_SET_MAX_DATAGRAM_SIZE,
	DRV_USB_CDC_NCM_CONTROL_DISABLE_CRC
};

/*
 * wIndex (the Communications Interface number) and bmRequestType belong to
 * the later USB binding.  This object describes only the profile-specific
 * request, wValue, and little-endian data stage.
 */
struct drv_usb_cdc_ncm_control_request {
	uint8_t request;
	uint16_t value;
	uint16_t length;
	uint8_t payload[4];
};

typedef int (*drv_usb_cdc_ncm_datagram_fn)(const void *frame,
	size_t frame_length, void *argument);

/* Register the integrated CDC NCM communication-interface driver. */
int drv_usb_cdc_ncm_driver_register(void);

int drv_usb_cdc_ncm_negotiate_nth16(const void *parameters,
	size_t parameters_length, const struct drv_usb_cdc_ncm_limits *limits,
	struct drv_usb_cdc_ncm_profile *profile);

int drv_usb_cdc_ncm_make_control_request(
	const struct drv_usb_cdc_ncm_profile *profile,
	enum drv_usb_cdc_ncm_control_step step,
	struct drv_usb_cdc_ncm_control_request *request);

void drv_usb_cdc_ncm_rx_reset(struct drv_usb_cdc_ncm_rx_state *state);

/*
 * The complete NTB is validated before deliver is called.  A callback error
 * can stop delivery after earlier, already-valid datagrams were delivered;
 * sequence state still advances because the USB NTB itself was consumed.
 * The first structurally valid NTB establishes the sequence baseline.  A
 * later structurally valid mismatch is accepted and resynchronizes the next
 * expected sequence; malformed NTBs never change sequence state.
 */
int drv_usb_cdc_ncm_parse_ntb16(
	const struct drv_usb_cdc_ncm_profile *profile,
	struct drv_usb_cdc_ncm_rx_state *state, const void *ntb,
	size_t ntb_length, drv_usb_cdc_ncm_datagram_fn deliver,
	void *argument, size_t *datagram_count);

/*
 * The returned length is the NTH16 wBlockLength.  When the natural length is
 * an exact bulk-OUT max-packet multiple, the builder normally includes one
 * trailing zero byte in wBlockLength so the HCD does not need a separate zero
 * packet.  CDC NCM exempts a transfer that already equals the function's
 * advertised dwNtbOutMaxSize; a locally clamped maximum is not exempt.
 */
int drv_usb_cdc_ncm_build_ntb16(
	const struct drv_usb_cdc_ncm_profile *profile, uint16_t sequence,
	const void *frame, size_t frame_length, void *ntb,
	size_t ntb_capacity, size_t *ntb_length);

#endif
