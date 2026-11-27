/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * USB Audio Class (UAC 1.0 and 2.0) descriptor definitions and parser.
 *
 * Everything here operates on raw descriptor byte arrays passed by the
 * caller without calling into the USB core, so the parser can be
 * exercised directly in host unit tests.
 */

#ifndef KERN_DRIVERS_USB_AUDIO_DESC_H
#define KERN_DRIVERS_USB_AUDIO_DESC_H

#include <stddef.h>
#include <stdint.h>

/* Maximum counts tracked per audio function. */
#define UAC_INTERFACES_MAX		8U
#define UAC_UNITS_MAX			32U
#define UAC_UNIT_SOURCES_MAX		8U
#define UAC_ALTS_MAX			32U
#define UAC_RATES_MAX			16U
#define UAC_CHANNELS_MAX		2U

/* USB Audio interface class, subclasses, and protocols. */
#define UAC_INTERFACE_CLASS		0x01U
#define UAC_SUBCLASS_AUDIOCONTROL	0x01U
#define UAC_SUBCLASS_AUDIOSTREAMING	0x02U
#define UAC_PROTO_V1			0x00U
#define UAC_PROTO_V2			0x20U

/* Class-specific descriptor types. */
#define UAC_CS_INTERFACE		0x24U
#define UAC_CS_ENDPOINT			0x25U

/* AudioControl class-specific descriptor subtypes. */
#define UAC_AC_HEADER			0x01U
#define UAC_AC_INPUT_TERMINAL		0x02U
#define UAC_AC_OUTPUT_TERMINAL		0x03U
#define UAC_AC_MIXER_UNIT		0x04U
#define UAC_AC_SELECTOR_UNIT		0x05U
#define UAC_AC_FEATURE_UNIT		0x06U
#define UAC_AC_CLOCK_SOURCE		0x0aU
#define UAC_AC_CLOCK_SELECTOR		0x0bU
#define UAC_AC_CLOCK_MULTIPLIER		0x0cU

/* AudioStreaming class-specific descriptor subtypes. */
#define UAC_AS_GENERAL			0x01U
#define UAC_AS_FORMAT_TYPE		0x02U

/* AudioStreaming class-specific endpoint subtypes. */
#define UAC_EP_GENERAL			0x01U

/* Format types and PCM tags. */
#define UAC_FORMAT_TYPE_I		0x01U
#define UAC1_FORMAT_PCM			0x0001U
#define UAC2_FORMAT_PCM_BIT		0x00000001U

/* Terminal types. */
#define UAC_TT_USB_STREAMING		0x0101U

/* Endpoint transfer and synchronization types. */
#define UAC_EP_ATTR_TYPE_MASK		0x03U
#define UAC_EP_ATTR_ISOCHRONOUS		0x01U
#define UAC_EP_ATTR_SYNC_SHIFT		2U
#define UAC_EP_ATTR_SYNC_MASK		0x03U
#define UAC_EP_SYNC_NONE		0U
#define UAC_EP_SYNC_ASYNC		1U
#define UAC_EP_SYNC_ADAPTIVE		2U
#define UAC_EP_SYNC_SYNC		3U
#define UAC_EP_ATTR_USAGE_SHIFT		4U
#define UAC_EP_ATTR_USAGE_MASK		0x03U
#define UAC_EP_USAGE_DATA		0U
#define UAC_EP_USAGE_FEEDBACK		1U
#define UAC_EP_USAGE_IMPLICIT_FB	2U

/* UAC 1.0 request codes. */
#define UAC1_SET_CUR			0x01U
#define UAC1_GET_CUR			0x81U
#define UAC1_GET_MIN			0x82U
#define UAC1_GET_MAX			0x83U
#define UAC1_GET_RES			0x84U

/* UAC 2.0 request codes. */
#define UAC2_CUR			0x01U
#define UAC2_RANGE			0x02U

/* Feature Unit control selectors. */
#define UAC_FU_MUTE_CONTROL		0x01U
#define UAC_FU_VOLUME_CONTROL		0x02U

/* Endpoint and Clock Source sampling frequency control selectors. */
#define UAC_EP_SAMPLING_FREQ_CONTROL	0x01U
#define UAC_CS_SAM_FREQ_CONTROL		0x01U

/* Normalized Feature Unit capability bits. */
#define UAC_FU_CAP_MUTE			(1U << 0)
#define UAC_FU_CAP_VOLUME		(1U << 1)
#define UAC_FU_CAP_MUTE_RW		(1U << 2)
#define UAC_FU_CAP_VOLUME_RW		(1U << 3)

/* UAC specification version of an AudioControl function. */
enum uac_version {
	UAC_VERSION_1 = 1,
	UAC_VERSION_2 = 2
};

/* Topology node kind inside an AudioControl interface. */
enum uac_unit_type {
	UAC_UNIT_INPUT_TERMINAL = 1,
	UAC_UNIT_OUTPUT_TERMINAL,
	UAC_UNIT_MIXER,
	UAC_UNIT_SELECTOR,
	UAC_UNIT_FEATURE,
	UAC_UNIT_CLOCK_SOURCE,
	UAC_UNIT_CLOCK_SELECTOR,
	UAC_UNIT_CLOCK_MULTIPLIER
};

/* One terminal, unit, or clock entity parsed from AudioControl. */
struct uac_unit {
	uint8_t id;
	enum uac_unit_type type;
	uint16_t terminal_type;
	uint8_t source_id;
	uint8_t clock_id;
	uint8_t source_ids[UAC_UNIT_SOURCES_MAX];
	unsigned source_count;
	uint32_t master_controls;
	uint32_t channel_controls[UAC_CHANNELS_MAX];
};

/* Parsed AudioControl topology and associated AudioStreaming interfaces. */
struct uac_control_desc {
	enum uac_version version;
	uint16_t bcd_adc;
	uint8_t stream_interfaces[UAC_INTERFACES_MAX];
	unsigned stream_interface_count;
	struct uac_unit units[UAC_UNITS_MAX];
	unsigned unit_count;
};

/* Resolved Feature Unit capabilities for one stream direction. */
struct uac_feature_unit {
	uint8_t unit_id;
	uint8_t has_mute;
	uint8_t mute_rw;
	uint8_t has_volume;
	uint8_t volume_rw;
	uint8_t master_mute;
	uint8_t master_volume;
	uint8_t channels;
};

/* Parsed AudioStreaming alternate setting with format, endpoints, and path. */
struct uac_as_alt {
	uint8_t interface_number;
	uint8_t alternate_setting;
	uint8_t terminal_link;
	uint8_t is_pcm;
	uint8_t channels;
	uint8_t subslot_size;
	uint8_t bit_resolution;
	uint8_t continuous_rates;
	uint32_t min_rate;
	uint32_t max_rate;
	uint32_t rates[UAC_RATES_MAX];
	unsigned rate_count;
	uint8_t ep_address;
	uint8_t ep_attributes;
	uint16_t ep_max_packet;
	uint8_t ep_interval;
	uint8_t ep_freq_control;
	uint8_t sync_ep_address;
	uint16_t sync_ep_max_packet;
	uint8_t sync_ep_interval;
	uint8_t clock_id;
	uint8_t capture;
	struct uac_feature_unit fu;
};

/*
 * Parses an array of class-specific AudioControl descriptors.
 */
int
uac_desc_parse_ac(
	enum uac_version version,
	const void *const *descs,
	const size_t *lengths,
	unsigned count,
	struct uac_control_desc *out);

/*
 * Parses a contiguous buffer of class-specific AudioControl descriptors.
 */
int
uac_desc_parse_ac_block(
	enum uac_version version,
	const uint8_t *buf,
	size_t len,
	struct uac_control_desc *out);

/*
 * Parses class-specific descriptors of one AudioStreaming alternate setting.
 */
int
uac_desc_parse_as_alt(
	enum uac_version version,
	uint8_t iface_num,
	uint8_t alt_num,
	const void *const *descs,
	const size_t *lengths,
	unsigned count,
	struct uac_as_alt *out);

/*
 * Attaches a standard isochronous endpoint descriptor to an AudioStreaming alt.
 */
void
uac_desc_add_ep(
	struct uac_as_alt *alt,
	uint8_t addr,
	uint8_t attr,
	uint16_t max_pkt,
	uint8_t interval,
	uint8_t synch_addr);

/*
 * Resolves stream direction, Feature Unit, and UAC 2.0 Clock Source.
 */
int
uac_desc_resolve_path(
	const struct uac_control_desc *ac,
	struct uac_as_alt *alt);

/*
 * Parses a UAC 2.0 Layout 3 Parameter Block of sample rate ranges.
 */
int
uac_desc_parse_clock_ranges(
	const uint8_t *buf,
	size_t len,
	struct uac_as_alt *alt);

/*
 * Reports whether an AudioStreaming alternate supports the given sample rate.
 */
int
uac_desc_alt_supports_rate(
	const struct uac_as_alt *alt,
	uint32_t rate);

#endif
