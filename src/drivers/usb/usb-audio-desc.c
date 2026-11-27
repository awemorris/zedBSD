/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * USB Audio Class (UAC 1.0 and 2.0) descriptor parser.
 *
 * Parses AudioControl topology (terminals, Feature Units, Clock Sources)
 * and AudioStreaming alternate settings (Format Type I, discrete and
 * continuous sample rates, data and feedback endpoints) without touching
 * USB core state.
 */

#include <drivers/usb/usb-audio-desc.h>

#include <stddef.h>
#include <stdint.h>
#include <uapi/errno.h>

/* Minimum descriptor lengths from UAC 1.0 and UAC 2.0 specifications. */
#define UAC1_AC_HEADER_MIN		8U
#define UAC2_AC_HEADER_MIN		9U
#define UAC1_INPUT_TERMINAL_MIN		12U
#define UAC2_INPUT_TERMINAL_MIN		17U
#define UAC1_OUTPUT_TERMINAL_MIN	9U
#define UAC2_OUTPUT_TERMINAL_MIN	12U
#define UAC1_FEATURE_UNIT_MIN		7U
#define UAC2_FEATURE_UNIT_MIN		6U
#define UAC_MIXER_SELECTOR_MIN		6U
#define UAC2_CLOCK_SOURCE_MIN		8U
#define UAC2_CLOCK_SELECTOR_MIN		7U
#define UAC2_CLOCK_MULTIPLIER_MIN	7U
#define UAC1_AS_GENERAL_MIN		7U
#define UAC2_AS_GENERAL_MIN		16U
#define UAC1_FORMAT_TYPE_I_MIN		11U
#define UAC1_FORMAT_TYPE_I_CONT_MIN	14U
#define UAC2_FORMAT_TYPE_I_MIN		6U
#define UAC1_EP_GENERAL_MIN		7U
#define UAC2_EP_GENERAL_MIN		8U

/* Maximum depth when walking topology or clock chains to prevent cycles. */
#define UAC_GRAPH_DEPTH_MAX		16U

static void uac_memzero(void *dst, size_t len);
static uint16_t uac_get_le16(const uint8_t *p);
static uint32_t uac_get_le24(const uint8_t *p);
static uint32_t uac_get_le32(const uint8_t *p);
static int uac_parse_ac_one(enum uac_version version, const uint8_t *d, size_t len, struct uac_control_desc *out);
static int uac_parse_ac_header(enum uac_version version, const uint8_t *d, size_t len, struct uac_control_desc *out);
static int uac_parse_input_terminal(enum uac_version version, const uint8_t *d, size_t len, struct uac_control_desc *out);
static int uac_parse_output_terminal(enum uac_version version, const uint8_t *d, size_t len, struct uac_control_desc *out);
static int uac_parse_feature_unit(enum uac_version version, const uint8_t *d, size_t len, struct uac_control_desc *out);
static int uac_parse_mixer_or_selector(enum uac_unit_type type, const uint8_t *d, size_t len, struct uac_control_desc *out);
static int uac_parse_clock_entity(uint8_t subtype, const uint8_t *d, size_t len, struct uac_control_desc *out);
static uint32_t uac1_decode_fu_bits(uint32_t raw);
static uint32_t uac2_decode_fu_bits(uint32_t raw);
static int uac_parse_as_one(enum uac_version version, const uint8_t *d, size_t len, struct uac_as_alt *out);
static const struct uac_unit *uac_find_unit(const struct uac_control_desc *ac, uint8_t id);
static uint8_t uac_resolve_clock(const struct uac_control_desc *ac, uint8_t clock_id);
static int uac_find_playback_fu(const struct uac_control_desc *ac, uint8_t unit_id, uint8_t target_id, unsigned depth, uint8_t *fu_id);
static int uac_find_capture_fu(const struct uac_control_desc *ac, uint8_t unit_id, unsigned depth, uint8_t *fu_id);
static void uac_fill_fu(const struct uac_unit *unit, uint8_t channels, struct uac_feature_unit *fu);

/*
 * Parses an array of class-specific AudioControl descriptors.
 */
int
uac_desc_parse_ac(
	enum uac_version version,
	const void *const *descs,
	const size_t *lengths,
	unsigned count,
	struct uac_control_desc *out)
{
	unsigned index;
	const uint8_t *d;
	size_t len;
	int error;

	if (descs == NULL || lengths == NULL || out == NULL)
		return EINVAL;

	uac_memzero(out, sizeof(*out));
	out->version = version;

	/* Inspects each extra descriptor belonging to the AudioControl interface. */
	for (index = 0U; index < count; index++) {
		d = descs[index];
		len = lengths[index];
		if (d == NULL || len < 3U)
			continue;
		if (d[1] != UAC_CS_INTERFACE)
			continue;

		error = uac_parse_ac_one(version, d, len, out);
		if (error != 0)
			return error;
	}

	/* Succeeded: topology and stream interface list populated. */
	return 0;
}

/*
 * Parses a contiguous buffer of class-specific AudioControl descriptors.
 */
int
uac_desc_parse_ac_block(
	enum uac_version version,
	const uint8_t *buf,
	size_t len,
	struct uac_control_desc *out)
{
	size_t offset;
	uint8_t dlen;
	int error;

	if (buf == NULL || out == NULL)
		return EINVAL;

	uac_memzero(out, sizeof(*out));
	out->version = version;

	/* Walks length-prefixed descriptors until the buffer is exhausted. */
	offset = 0U;
	while (offset + 2U <= len) {
		dlen = buf[offset];
		if (dlen < 2U || offset + dlen > len)
			break;

		if (dlen >= 3U && buf[offset + 1U] == UAC_CS_INTERFACE) {
			error = uac_parse_ac_one(version, &buf[offset], dlen, out);
			if (error != 0)
				return error;
		}

		offset += dlen;
	}

	/* Succeeded: contiguous AudioControl block parsed. */
	return 0;
}

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
	struct uac_as_alt *out)
{
	unsigned index;
	const uint8_t *d;
	size_t len;
	int error;

	if (descs == NULL || lengths == NULL || out == NULL)
		return EINVAL;

	uac_memzero(out, sizeof(*out));
	out->interface_number = iface_num;
	out->alternate_setting = alt_num;

	/* Parses each class-specific interface or endpoint descriptor in the alt. */
	for (index = 0U; index < count; index++) {
		d = descs[index];
		len = lengths[index];
		if (d == NULL || len < 3U)
			continue;

		error = uac_parse_as_one(version, d, len, out);
		if (error != 0)
			return error;
	}

	/* Succeeded: alternate format descriptor state populated. */
	return 0;
}

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
	uint8_t synch_addr)
{
	uint8_t xfer_type;
	uint8_t usage;

	if (alt == NULL)
		return;

	xfer_type = attr & UAC_EP_ATTR_TYPE_MASK;
	if (xfer_type != UAC_EP_ATTR_ISOCHRONOUS)
		return;

	/* Separates explicit feedback IN endpoints from audio data endpoints. */
	usage = (attr >> UAC_EP_ATTR_USAGE_SHIFT) & UAC_EP_ATTR_USAGE_MASK;
	if (usage == UAC_EP_USAGE_FEEDBACK) {
		alt->sync_ep_address = addr;
		alt->sync_ep_max_packet = max_pkt;
		alt->sync_ep_interval = interval;
		return;
	}

	if (alt->ep_address == 0U) {
		alt->ep_address = addr;
		alt->ep_attributes = attr;
		alt->ep_max_packet = max_pkt;
		alt->ep_interval = interval;
		if (synch_addr != 0U && alt->sync_ep_address == 0U)
			alt->sync_ep_address = synch_addr;
	} else if (alt->sync_ep_address == 0U && (addr & 0x80U) != 0U) {
		/* Some devices omit bmUsageType=Feedback and rely on bSynchAddress. */
		alt->sync_ep_address = addr;
		alt->sync_ep_max_packet = max_pkt;
		alt->sync_ep_interval = interval;
	}
}

/*
 * Resolves stream direction, Feature Unit, and UAC 2.0 Clock Source.
 */
int
uac_desc_resolve_path(
	const struct uac_control_desc *ac,
	struct uac_as_alt *alt)
{
	const struct uac_unit *term;
	const struct uac_unit *other;
	const struct uac_unit *fu_unit;
	unsigned index;
	uint8_t fu_id;

	if (ac == NULL || alt == NULL)
		return EINVAL;

	if (alt->terminal_link == 0U)
		return ENOENT;

	term = uac_find_unit(ac, alt->terminal_link);
	if (term == NULL)
		return ENOENT;

	fu_id = 0U;

	/* Input USB Streaming terminal means playback; Output USB Streaming means capture. */
	if (term->type == UAC_UNIT_INPUT_TERMINAL) {
		alt->capture = 0U;
		if (ac->version == UAC_VERSION_2 && term->clock_id != 0U)
			alt->clock_id = uac_resolve_clock(ac, term->clock_id);

		for (index = 0U; index < ac->unit_count; index++) {
			other = &ac->units[index];
			if (other->type != UAC_UNIT_OUTPUT_TERMINAL)
				continue;

			if (uac_find_playback_fu(ac, other->id, term->id, 0U, &fu_id) == 0) {
				if (alt->clock_id == 0U && ac->version == UAC_VERSION_2 && other->clock_id != 0U)
					alt->clock_id = uac_resolve_clock(ac, other->clock_id);
				break;
			}
		}
	} else if (term->type == UAC_UNIT_OUTPUT_TERMINAL) {
		alt->capture = 1U;
		if (ac->version == UAC_VERSION_2 && term->clock_id != 0U)
			alt->clock_id = uac_resolve_clock(ac, term->clock_id);

		(void)uac_find_capture_fu(ac, term->source_id, 0U, &fu_id);
	} else {
		return ENOENT;
	}

	if (fu_id != 0U) {
		fu_unit = uac_find_unit(ac, fu_id);
		if (fu_unit != NULL)
			uac_fill_fu(fu_unit, alt->channels, &alt->fu);
	}

	/* Succeeded: direction, clock source, and Feature Unit resolved. */
	return 0;
}

/*
 * Parses a UAC 2.0 Layout 3 Parameter Block of sample rate ranges.
 */
int
uac_desc_parse_clock_ranges(
	const uint8_t *buf,
	size_t len,
	struct uac_as_alt *alt)
{
	uint16_t num_ranges;
	uint16_t index;
	size_t offset;
	uint32_t rmin;
	uint32_t rmax;
	uint32_t rres;

	if (buf == NULL || alt == NULL || len < 2U)
		return EINVAL;

	num_ranges = uac_get_le16(buf);
	alt->rate_count = 0U;
	alt->continuous_rates = 0U;
	alt->min_rate = 0U;
	alt->max_rate = 0U;

	/* Each Layout 3 subrange is 12 bytes: dMIN(4), dMAX(4), dRES(4). */
	offset = 2U;
	for (index = 0U; index < num_ranges; index++) {
		if (offset + 12U > len)
			break;

		rmin = uac_get_le32(&buf[offset]);
		rmax = uac_get_le32(&buf[offset + 4U]);
		rres = uac_get_le32(&buf[offset + 8U]);
		offset += 12U;

		if (rmin == rmax) {
			if (alt->rate_count < UAC_RATES_MAX) {
				alt->rates[alt->rate_count] = rmin;
				alt->rate_count++;
			}
		} else if (rmin < rmax && rres == 0U) {
			alt->continuous_rates = 1U;
			alt->min_rate = rmin;
			alt->max_rate = rmax;
		} else if (rmin < rmax && alt->rate_count < UAC_RATES_MAX) {
			alt->rates[alt->rate_count] = rmin;
			alt->rate_count++;
			if (alt->rate_count < UAC_RATES_MAX) {
				alt->rates[alt->rate_count] = rmax;
				alt->rate_count++;
			}
		}
	}

	if (alt->rate_count == 0U && alt->continuous_rates == 0U)
		return ENOENT;

	/* Succeeded: sample rates extracted from UAC 2.0 clock range block. */
	return 0;
}

/*
 * Reports whether an AudioStreaming alternate supports the given sample rate.
 */
int
uac_desc_alt_supports_rate(
	const struct uac_as_alt *alt,
	uint32_t rate)
{
	unsigned index;

	if (alt == NULL || rate == 0U)
		return 0;

	if (alt->continuous_rates != 0U) {
		if (rate >= alt->min_rate && rate <= alt->max_rate)
			return 1;
	}

	for (index = 0U; index < alt->rate_count; index++) {
		if (alt->rates[index] == rate)
			return 1;
	}

	/* Not supported by this alternate. */
	return 0;
}

/* Zeroes a byte range without external libc dependencies. */
static void
uac_memzero(
	void *dst,
	size_t len)
{
	uint8_t *p;
	size_t i;

	p = (uint8_t *)dst;
	for (i = 0U; i < len; i++)
		p[i] = 0U;
}

/* Reads a 16-bit little-endian integer. */
static uint16_t
uac_get_le16(
	const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* Reads a 24-bit little-endian integer. */
static uint32_t
uac_get_le24(
	const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

/* Reads a 32-bit little-endian integer. */
static uint32_t
uac_get_le32(
	const uint8_t *p)
{
	return (uint32_t)p[0] |
	       ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

/* Dispatches one AudioControl class-specific descriptor by bDescriptorSubtype. */
static int
uac_parse_ac_one(
	enum uac_version version,
	const uint8_t *d,
	size_t len,
	struct uac_control_desc *out)
{
	uint8_t subtype;

	subtype = d[2];
	switch (subtype) {
	case UAC_AC_HEADER:
		return uac_parse_ac_header(version, d, len, out);
	case UAC_AC_INPUT_TERMINAL:
		return uac_parse_input_terminal(version, d, len, out);
	case UAC_AC_OUTPUT_TERMINAL:
		return uac_parse_output_terminal(version, d, len, out);
	case UAC_AC_FEATURE_UNIT:
		return uac_parse_feature_unit(version, d, len, out);
	case UAC_AC_MIXER_UNIT:
		return uac_parse_mixer_or_selector(UAC_UNIT_MIXER, d, len, out);
	case UAC_AC_SELECTOR_UNIT:
		return uac_parse_mixer_or_selector(UAC_UNIT_SELECTOR, d, len, out);
	case UAC_AC_CLOCK_SOURCE:
	case UAC_AC_CLOCK_SELECTOR:
	case UAC_AC_CLOCK_MULTIPLIER:
		if (version == UAC_VERSION_2)
			return uac_parse_clock_entity(subtype, d, len, out);
		return 0;
	default:
		return 0;
	}
}

/* Parses an AudioControl Header descriptor (UAC 1.0 or UAC 2.0). */
static int
uac_parse_ac_header(
	enum uac_version version,
	const uint8_t *d,
	size_t len,
	struct uac_control_desc *out)
{
	uint8_t count;
	uint8_t index;

	if (version == UAC_VERSION_1) {
		if (len < UAC1_AC_HEADER_MIN)
			return EINVAL;

		out->bcd_adc = uac_get_le16(&d[3]);
		count = d[7];
		for (index = 0U; index < count; index++) {
			if (8U + (size_t)index >= len)
				break;
			if (out->stream_interface_count < UAC_INTERFACES_MAX) {
				out->stream_interfaces[out->stream_interface_count] = d[8U + index];
				out->stream_interface_count++;
			}
		}

		return 0;
	}

	if (len < UAC2_AC_HEADER_MIN)
		return EINVAL;

	out->bcd_adc = uac_get_le16(&d[3]);

	/* Succeeded: UAC 2.0 AudioControl header parsed. */
	return 0;
}

/* Parses an Input Terminal descriptor. */
static int
uac_parse_input_terminal(
	enum uac_version version,
	const uint8_t *d,
	size_t len,
	struct uac_control_desc *out)
{
	struct uac_unit *unit;

	if (version == UAC_VERSION_1 && len < UAC1_INPUT_TERMINAL_MIN)
		return EINVAL;
	if (version == UAC_VERSION_2 && len < UAC2_INPUT_TERMINAL_MIN)
		return EINVAL;

	if (out->unit_count >= UAC_UNITS_MAX)
		return 0;

	unit = &out->units[out->unit_count];
	out->unit_count++;

	unit->id = d[3];
	unit->type = UAC_UNIT_INPUT_TERMINAL;
	unit->terminal_type = uac_get_le16(&d[4]);
	if (version == UAC_VERSION_2)
		unit->clock_id = d[7];

	/* Succeeded: Input Terminal recorded. */
	return 0;
}

/* Parses an Output Terminal descriptor. */
static int
uac_parse_output_terminal(
	enum uac_version version,
	const uint8_t *d,
	size_t len,
	struct uac_control_desc *out)
{
	struct uac_unit *unit;

	if (version == UAC_VERSION_1 && len < UAC1_OUTPUT_TERMINAL_MIN)
		return EINVAL;
	if (version == UAC_VERSION_2 && len < UAC2_OUTPUT_TERMINAL_MIN)
		return EINVAL;

	if (out->unit_count >= UAC_UNITS_MAX)
		return 0;

	unit = &out->units[out->unit_count];
	out->unit_count++;

	unit->id = d[3];
	unit->type = UAC_UNIT_OUTPUT_TERMINAL;
	unit->terminal_type = uac_get_le16(&d[4]);
	unit->source_id = d[7];
	if (version == UAC_VERSION_2)
		unit->clock_id = d[8];

	/* Succeeded: Output Terminal recorded. */
	return 0;
}

/* Parses a Feature Unit descriptor for UAC 1.0 or UAC 2.0. */
static int
uac_parse_feature_unit(
	enum uac_version version,
	const uint8_t *d,
	size_t len,
	struct uac_control_desc *out)
{
	struct uac_unit *unit;
	uint8_t ctrl_size;
	size_t offset;
	unsigned ch;
	uint32_t raw;

	if (version == UAC_VERSION_1 && len < UAC1_FEATURE_UNIT_MIN)
		return EINVAL;
	if (version == UAC_VERSION_2 && len < UAC2_FEATURE_UNIT_MIN)
		return EINVAL;

	if (out->unit_count >= UAC_UNITS_MAX)
		return 0;

	unit = &out->units[out->unit_count];
	out->unit_count++;

	unit->id = d[3];
	unit->type = UAC_UNIT_FEATURE;
	unit->source_id = d[4];

	if (version == UAC_VERSION_1) {
		ctrl_size = d[5];
		if (ctrl_size == 0U)
			return 0;

		offset = 6U;
		if (offset + ctrl_size <= len) {
			raw = d[offset];
			if (ctrl_size >= 2U)
				raw |= (uint32_t)d[offset + 1U] << 8;
			unit->master_controls = uac1_decode_fu_bits(raw);
		}

		for (ch = 0U; ch < UAC_CHANNELS_MAX; ch++) {
			offset += ctrl_size;
			if (offset + ctrl_size > len)
				break;
			raw = d[offset];
			if (ctrl_size >= 2U)
				raw |= (uint32_t)d[offset + 1U] << 8;
			unit->channel_controls[ch] = uac1_decode_fu_bits(raw);
		}

		return 0;
	}

	/* UAC 2.0 uses 4-byte bmaControls starting at offset 5 (excluding iFeature). */
	offset = 5U;
	if (offset + 4U <= len - 1U) {
		raw = uac_get_le32(&d[offset]);
		unit->master_controls = uac2_decode_fu_bits(raw);
	}

	for (ch = 0U; ch < UAC_CHANNELS_MAX; ch++) {
		offset += 4U;
		if (offset + 4U > len - 1U)
			break;
		raw = uac_get_le32(&d[offset]);
		unit->channel_controls[ch] = uac2_decode_fu_bits(raw);
	}

	/* Succeeded: Feature Unit recorded. */
	return 0;
}

/* Parses a Mixer or Selector Unit so topology traversal can cross it. */
static int
uac_parse_mixer_or_selector(
	enum uac_unit_type type,
	const uint8_t *d,
	size_t len,
	struct uac_control_desc *out)
{
	struct uac_unit *unit;
	uint8_t pins;
	uint8_t index;

	if (len < UAC_MIXER_SELECTOR_MIN)
		return EINVAL;

	if (out->unit_count >= UAC_UNITS_MAX)
		return 0;

	unit = &out->units[out->unit_count];
	out->unit_count++;

	unit->id = d[3];
	unit->type = type;
	pins = d[4];
	for (index = 0U; index < pins; index++) {
		if (5U + (size_t)index >= len)
			break;
		if (unit->source_count < UAC_UNIT_SOURCES_MAX) {
			unit->source_ids[unit->source_count] = d[5U + index];
			unit->source_count++;
		}
	}

	if (unit->source_count > 0U)
		unit->source_id = unit->source_ids[0];

	/* Succeeded: Mixer or Selector Unit recorded. */
	return 0;
}

/* Parses a UAC 2.0 Clock Source, Clock Selector, or Clock Multiplier. */
static int
uac_parse_clock_entity(
	uint8_t subtype,
	const uint8_t *d,
	size_t len,
	struct uac_control_desc *out)
{
	struct uac_unit *unit;

	if (out->unit_count >= UAC_UNITS_MAX)
		return 0;

	if (subtype == UAC_AC_CLOCK_SOURCE) {
		if (len < UAC2_CLOCK_SOURCE_MIN)
			return EINVAL;
		unit = &out->units[out->unit_count];
		out->unit_count++;
		unit->id = d[3];
		unit->type = UAC_UNIT_CLOCK_SOURCE;
		unit->master_controls = d[5];
		return 0;
	}

	if (subtype == UAC_AC_CLOCK_SELECTOR) {
		if (len < UAC2_CLOCK_SELECTOR_MIN)
			return EINVAL;
		unit = &out->units[out->unit_count];
		out->unit_count++;
		unit->id = d[3];
		unit->type = UAC_UNIT_CLOCK_SELECTOR;
		if (d[4] > 0U && 5U < len)
			unit->source_id = d[5];
		return 0;
	}

	if (len < UAC2_CLOCK_MULTIPLIER_MIN)
		return EINVAL;

	unit = &out->units[out->unit_count];
	out->unit_count++;
	unit->id = d[3];
	unit->type = UAC_UNIT_CLOCK_MULTIPLIER;
	unit->source_id = d[6];

	/* Succeeded: UAC 2.0 clock entity recorded. */
	return 0;
}

/* Maps UAC 1.0 Feature Unit bmaControls bits to normalized capability flags. */
static uint32_t
uac1_decode_fu_bits(
	uint32_t raw)
{
	uint32_t caps;

	caps = 0U;
	if ((raw & 0x01U) != 0U)
		caps |= UAC_FU_CAP_MUTE | UAC_FU_CAP_MUTE_RW;
	if ((raw & 0x02U) != 0U)
		caps |= UAC_FU_CAP_VOLUME | UAC_FU_CAP_VOLUME_RW;

	return caps;
}

/* Maps UAC 2.0 Feature Unit 2-bit control fields to normalized capability flags. */
static uint32_t
uac2_decode_fu_bits(
	uint32_t raw)
{
	uint32_t caps;
	uint32_t mute_bits;
	uint32_t vol_bits;

	caps = 0U;
	mute_bits = raw & 0x03U;
	vol_bits = (raw >> 2) & 0x03U;

	if (mute_bits == 0x01U || mute_bits == 0x03U)
		caps |= UAC_FU_CAP_MUTE;
	if (mute_bits == 0x03U)
		caps |= UAC_FU_CAP_MUTE_RW;

	if (vol_bits == 0x01U || vol_bits == 0x03U)
		caps |= UAC_FU_CAP_VOLUME;
	if (vol_bits == 0x03U)
		caps |= UAC_FU_CAP_VOLUME_RW;

	return caps;
}

/* Parses one AudioStreaming CS_INTERFACE or CS_ENDPOINT descriptor. */
static int
uac_parse_as_one(
	enum uac_version version,
	const uint8_t *d,
	size_t len,
	struct uac_as_alt *out)
{
	uint8_t dtype;
	uint8_t subtype;
	uint8_t freq_type;
	uint8_t index;
	uint32_t bm_formats;

	dtype = d[1];
	subtype = d[2];

	if (dtype == UAC_CS_ENDPOINT && subtype == UAC_EP_GENERAL) {
		if (version == UAC_VERSION_1) {
			if (len < UAC1_EP_GENERAL_MIN)
				return EINVAL;
			if ((d[3] & 0x01U) != 0U)
				out->ep_freq_control = 1U;
		} else {
			if (len < UAC2_EP_GENERAL_MIN)
				return EINVAL;
		}
		return 0;
	}

	if (dtype != UAC_CS_INTERFACE)
		return 0;

	if (subtype == UAC_AS_GENERAL) {
		if (version == UAC_VERSION_1) {
			if (len < UAC1_AS_GENERAL_MIN)
				return EINVAL;
			out->terminal_link = d[3];
			if (uac_get_le16(&d[5]) == UAC1_FORMAT_PCM)
				out->is_pcm = 1U;
		} else {
			if (len < UAC2_AS_GENERAL_MIN)
				return EINVAL;
			out->terminal_link = d[3];
			bm_formats = uac_get_le32(&d[6]);
			if (d[5] == UAC_FORMAT_TYPE_I && (bm_formats & UAC2_FORMAT_PCM_BIT) != 0U)
				out->is_pcm = 1U;
			out->channels = d[10];
		}
		return 0;
	}

	if (subtype == UAC_AS_FORMAT_TYPE) {
		if (len < 4U || d[3] != UAC_FORMAT_TYPE_I)
			return 0;

		if (version == UAC_VERSION_2) {
			if (len < UAC2_FORMAT_TYPE_I_MIN)
				return EINVAL;
			out->subslot_size = d[4];
			out->bit_resolution = d[5];
			return 0;
		}

		if (len < UAC1_FORMAT_TYPE_I_MIN)
			return EINVAL;

		out->channels = d[4];
		out->subslot_size = d[5];
		out->bit_resolution = d[6];
		freq_type = d[7];

		if (freq_type == 0U) {
			if (len < UAC1_FORMAT_TYPE_I_CONT_MIN)
				return EINVAL;
			out->continuous_rates = 1U;
			out->min_rate = uac_get_le24(&d[8]);
			out->max_rate = uac_get_le24(&d[11]);
		} else {
			for (index = 0U; index < freq_type; index++) {
				if (8U + ((size_t)index + 1U) * 3U > len)
					break;
				if (out->rate_count < UAC_RATES_MAX) {
					out->rates[out->rate_count] = uac_get_le24(&d[8U + (size_t)index * 3U]);
					out->rate_count++;
				}
			}
		}
	}

	/* Succeeded: AudioStreaming descriptor parsed. */
	return 0;
}

/* Finds a unit or terminal by its bUnitID / bTerminalID. */
static const struct uac_unit *
uac_find_unit(
	const struct uac_control_desc *ac,
	uint8_t id)
{
	unsigned index;

	if (id == 0U)
		return NULL;

	for (index = 0U; index < ac->unit_count; index++) {
		if (ac->units[index].id == id)
			return &ac->units[index];
	}

	return NULL;
}

/* Follows UAC 2.0 Clock Selector / Multiplier chains to the underlying Clock Source. */
static uint8_t
uac_resolve_clock(
	const struct uac_control_desc *ac,
	uint8_t clock_id)
{
	const struct uac_unit *unit;
	unsigned depth;

	for (depth = 0U; depth < UAC_GRAPH_DEPTH_MAX; depth++) {
		unit = uac_find_unit(ac, clock_id);
		if (unit == NULL)
			return 0U;

		if (unit->type == UAC_UNIT_CLOCK_SOURCE)
			return unit->id;

		if (unit->type == UAC_UNIT_CLOCK_SELECTOR ||
		    unit->type == UAC_UNIT_CLOCK_MULTIPLIER) {
			clock_id = unit->source_id;
			continue;
		}

		return 0U;
	}

	return 0U;
}

/* Walks upstream from an Output Terminal to target_id, recording the nearest Feature Unit. */
static int
uac_find_playback_fu(
	const struct uac_control_desc *ac,
	uint8_t unit_id,
	uint8_t target_id,
	unsigned depth,
	uint8_t *fu_id)
{
	const struct uac_unit *unit;
	unsigned index;
	uint8_t candidate;

	if (unit_id == target_id)
		return 0;

	if (depth >= UAC_GRAPH_DEPTH_MAX)
		return ENOENT;

	unit = uac_find_unit(ac, unit_id);
	if (unit == NULL)
		return ENOENT;

	if (unit->type == UAC_UNIT_MIXER || unit->type == UAC_UNIT_SELECTOR) {
		for (index = 0U; index < unit->source_count; index++) {
			candidate = 0U;
			if (uac_find_playback_fu(ac, unit->source_ids[index], target_id, depth + 1U, &candidate) == 0) {
				if (*fu_id == 0U)
					*fu_id = candidate;
				return 0;
			}
		}
		return ENOENT;
	}

	if (unit->source_id == 0U)
		return ENOENT;

	candidate = 0U;
	if (uac_find_playback_fu(ac, unit->source_id, target_id, depth + 1U, &candidate) != 0)
		return ENOENT;

	if (unit->type == UAC_UNIT_FEATURE)
		*fu_id = unit->id;
	else
		*fu_id = candidate;

	return 0;
}

/* Walks upstream from a USB Streaming Output Terminal to find the capture Feature Unit. */
static int
uac_find_capture_fu(
	const struct uac_control_desc *ac,
	uint8_t unit_id,
	unsigned depth,
	uint8_t *fu_id)
{
	const struct uac_unit *unit;

	if (depth >= UAC_GRAPH_DEPTH_MAX || unit_id == 0U)
		return ENOENT;

	unit = uac_find_unit(ac, unit_id);
	if (unit == NULL)
		return ENOENT;

	if (unit->type == UAC_UNIT_FEATURE) {
		*fu_id = unit->id;
		return 0;
	}

	if (unit->type == UAC_UNIT_INPUT_TERMINAL)
		return 0;

	return uac_find_capture_fu(ac, unit->source_id, depth + 1U, fu_id);
}

/* Copies normalized Feature Unit capabilities into the stream descriptor. */
static void
uac_fill_fu(
	const struct uac_unit *unit,
	uint8_t channels,
	struct uac_feature_unit *fu)
{
	uac_memzero(fu, sizeof(*fu));
	fu->unit_id = unit->id;
	fu->channels = channels;

	if ((unit->master_controls & UAC_FU_CAP_MUTE) != 0U) {
		fu->has_mute = 1U;
		fu->master_mute = 1U;
		if ((unit->master_controls & UAC_FU_CAP_MUTE_RW) != 0U)
			fu->mute_rw = 1U;
	} else if ((unit->channel_controls[0] & UAC_FU_CAP_MUTE) != 0U) {
		fu->has_mute = 1U;
		fu->master_mute = 0U;
		if ((unit->channel_controls[0] & UAC_FU_CAP_MUTE_RW) != 0U)
			fu->mute_rw = 1U;
	}

	if ((unit->master_controls & UAC_FU_CAP_VOLUME) != 0U) {
		fu->has_volume = 1U;
		fu->master_volume = 1U;
		if ((unit->master_controls & UAC_FU_CAP_VOLUME_RW) != 0U)
			fu->volume_rw = 1U;
	} else if ((unit->channel_controls[0] & UAC_FU_CAP_VOLUME) != 0U) {
		fu->has_volume = 1U;
		fu->master_volume = 0U;
		if ((unit->channel_controls[0] & UAC_FU_CAP_VOLUME_RW) != 0U)
			fu->volume_rw = 1U;
	}
}
