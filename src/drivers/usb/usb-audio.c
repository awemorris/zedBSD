/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * USB Audio Class (UAC 1.0 and 2.0) driver: device binding, descriptor
 * discovery, audio framework registration, and Feature Unit mixer control.
 */

#include <drivers/audio/audio.h>
#include <drivers/usb/usb-audio.h>
#include <drivers/usb/usb.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <uapi/errno.h>

#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(parameter) ((void)(parameter))
#endif

/* Control transfer timeout for mixer and clock queries. */
#define USB_AUDIO_CTL_TIMEOUT_MS	1000U

/* Extra descriptors per interface or alternate setting. */
#define USB_AUDIO_EXTRA_MAX		32U

static const struct drv_usb_id usb_audio_ids[] = {
	{
		.match_flags = DRV_USB_ID_IF_CLASS | DRV_USB_ID_IF_SUBCLASS,
		.interface_class = UAC_INTERFACE_CLASS,
		.interface_subclass = UAC_SUBCLASS_AUDIOCONTROL
	}
};

static int usb_audio_attach(struct drv_usb_interface *interface, const struct drv_usb_id *id);
static int usb_audio_detach(struct drv_usb_interface *interface, unsigned flags);
static int usb_audio_quiesce(struct drv_usb_interface *interface);
static int usb_audio_read_control(struct usb_audio *audio);
static int usb_audio_scan_streams(struct usb_audio *audio);
static int usb_audio_parse_stream_iface(struct usb_audio *audio, struct drv_usb_interface *iface);
static void usb_audio_query_uac2_rates(struct usb_audio *audio, struct uac_as_alt *alt);
static void usb_audio_build_formats(struct usb_audio *audio);
static void usb_audio_add_format(struct usb_audio *audio, uint32_t fmt, uint32_t channels, uint32_t rate);
static void usb_audio_init_mixer(struct usb_audio *audio);
static int usb_audio_fu_req(struct usb_audio *audio, uint8_t fu_id, uint8_t bm_req, uint8_t req, uint8_t cs, uint8_t ch, void *buf, size_t len);
static const struct uac_feature_unit *usb_audio_primary_fu(const struct usb_audio *audio);
static void usb_audio_free(struct usb_audio *audio);
static int usb_audio_op_prepare(void *priv, int capture, const struct audio_format *format, const struct drv_dma_buffer *buffer, uint32_t fragment_bytes, uint32_t fragment_count);
static int usb_audio_op_start(void *priv, int capture);
static void usb_audio_op_stop(void *priv, int capture);
static uint32_t usb_audio_op_position(void *priv, int capture);
static int usb_audio_op_get_volume(void *priv, struct audio_volume *volume);
static int usb_audio_op_set_volume(void *priv, const struct audio_volume *volume);

/* USB driver table registered at boot. */
static struct drv_usb_driver usb_audio_driver = {
	.name = "usb-audio",
	.ids = usb_audio_ids,
	.id_count = sizeof(usb_audio_ids) / sizeof(usb_audio_ids[0]),
	.attach = usb_audio_attach,
	.detach = usb_audio_detach,
	.quiesce = usb_audio_quiesce
};

/*
 * Registers the USB Audio Class driver with the USB core.
 */
int
drv_usb_audio_driver_register(
	void)
{
	int error;

	error = drv_usb_driver_register(&usb_audio_driver);
	if (error != 0)
		return error;

	/* Succeeded: driver registered. */
	return 0;
}

/* Probes and binds a USB AudioControl interface and its AudioStreaming siblings. */
static int
usb_audio_attach(
	struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	const struct drv_usb_interface_descriptor *idesc;
	const struct drv_usb_device_descriptor *ddesc;
	struct drv_usb_bus *bus;
	struct usb_audio *audio;
	struct drv_dma_device *dma;
	int error;

	UNUSED_PARAMETER(id);

	audio = kern_calloc(1U, sizeof(*audio));
	if (audio == NULL)
		return ENOMEM;

	audio->control_iface = interface;
	audio->control_iface_num = (uint8_t)drv_usb_interface_number(interface);
	audio->device = drv_usb_interface_device(interface);
	audio->speed = drv_usb_device_speed(audio->device);

	idesc = drv_usb_interface_descriptor(interface);
	audio->version = (idesc != NULL && idesc->interface_protocol == UAC_PROTO_V2) ?
	    UAC_VERSION_2 : UAC_VERSION_1;

	bus = drv_usb_device_bus(audio->device);
	kern_snprintf(audio->tag, sizeof(audio->tag), "%u-%u",
	    drv_usb_bus_number(bus), drv_usb_device_port(audio->device));

	ddesc = drv_usb_device_descriptor(audio->device);
	kern_snprintf(audio->product, sizeof(audio->product), "USB Audio");
	if (ddesc != NULL && ddesc->product_string != 0U)
		(void)drv_usb_device_get_string(audio->device, ddesc->product_string, 0, audio->product, sizeof(audio->product));

	error = mutex_init(&audio->mutex, LOCK_RANK_DEVICE, "usb-audio");
	if (error != 0) {
		kern_free(audio);
		return error;
	}

	usb_audio_stream_init(audio, &audio->playback, 0U);
	usb_audio_stream_init(audio, &audio->capture, 1U);

	error = usb_audio_read_control(audio);
	if (error != 0) {
		usb_audio_free(audio);
		return error;
	}

	error = usb_audio_scan_streams(audio);
	if (error != 0) {
		usb_audio_free(audio);
		return error;
	}

	usb_audio_build_formats(audio);
	if (audio->format_count == 0U) {
		kern_logf("usb-audio: %s: no supported PCM formats\n", audio->tag);
		usb_audio_free(audio);
		return ENODEV;
	}

	usb_audio_init_mixer(audio);

	audio->ops.playback = audio->playback.available;
	audio->ops.capture = audio->capture.available;
	audio->ops.formats = audio->formats;
	audio->ops.format_count = audio->format_count;
	audio->ops.prepare = usb_audio_op_prepare;
	audio->ops.start = usb_audio_op_start;
	audio->ops.stop = usb_audio_op_stop;
	audio->ops.position = usb_audio_op_position;
	audio->ops.get_volume = usb_audio_op_get_volume;
	audio->ops.set_volume = usb_audio_op_set_volume;

	error = drv_usb_interface_set_driver_data(interface, audio);
	if (error != 0) {
		usb_audio_free(audio);
		return error;
	}

	dma = drv_usb_device_dma(audio->device);
	error = drv_audio_register(&audio->ops, audio, dma, &audio->audio_dev);
	if (error != 0) {
		(void)drv_usb_interface_set_driver_data(interface, NULL);
		usb_audio_free(audio);
		return error;
	}

	kern_logf("usb-audio: %s: %s (UAC %u.0, playback=%u, capture=%u, formats=%u)\n",
	    audio->tag, audio->product, (unsigned)audio->version,
	    audio->playback.available, audio->capture.available, audio->format_count);

	/* Succeeded: /dev/dspN and /dev/mixerN published. */
	return 0;
}

/* Detaches the USB Audio device, deferring cleanup if /dev/dspN is still open. */
static int
usb_audio_detach(
	struct drv_usb_interface *interface,
	unsigned flags)
{
	struct usb_audio *audio;
	int p_err;
	int c_err;
	int error;

	UNUSED_PARAMETER(flags);

	audio = drv_usb_interface_driver_data(interface);
	if (audio == NULL)
		return 0;

	mutex_lock(&audio->mutex);
	audio->disconnected = 1U;
	p_err = usb_audio_stream_stop(&audio->playback);
	c_err = usb_audio_stream_stop(&audio->capture);
	mutex_unlock(&audio->mutex);

	if (p_err != 0)
		return p_err;
	if (c_err != 0)
		return c_err;

	if (audio->audio_dev != NULL) {
		error = drv_audio_unregister(audio->audio_dev);
		if (error != 0) {
			audio->unregister_pending = 1U;
			return error;
		}
		audio->audio_dev = NULL;
	}

	(void)drv_usb_interface_set_driver_data(interface, NULL);
	usb_audio_free(audio);

	/* Succeeded: audio device unregistered and freed. */
	return 0;
}

/* Halts running streams during USB core suspend or reset. */
static int
usb_audio_quiesce(
	struct drv_usb_interface *interface)
{
	struct usb_audio *audio;
	int p_err;
	int c_err;

	audio = drv_usb_interface_driver_data(interface);
	if (audio == NULL)
		return 0;

	mutex_lock(&audio->mutex);
	p_err = usb_audio_stream_stop(&audio->playback);
	c_err = usb_audio_stream_stop(&audio->capture);
	mutex_unlock(&audio->mutex);

	if (p_err != 0)
		return p_err;
	if (c_err != 0)
		return c_err;

	/* Succeeded: both streams quiesced. */
	return 0;
}

/* Parses class-specific AudioControl descriptors from the active alternate. */
static int
usb_audio_read_control(
	struct usb_audio *audio)
{
	const struct drv_usb_host_interface *alt;
	const void *descs[USB_AUDIO_EXTRA_MAX];
	size_t lengths[USB_AUDIO_EXTRA_MAX];
	unsigned count;
	unsigned index;

	alt = drv_usb_interface_active_alternate(audio->control_iface);
	if (alt == NULL)
		return ENODEV;

	count = drv_usb_host_interface_extra_count(alt);
	if (count > USB_AUDIO_EXTRA_MAX)
		count = USB_AUDIO_EXTRA_MAX;

	for (index = 0U; index < count; index++) {
		if (drv_usb_host_interface_extra(alt, index, &descs[index], &lengths[index]) != 0) {
			descs[index] = NULL;
			lengths[index] = 0U;
		}
	}

	return uac_desc_parse_ac(audio->version, descs, lengths, count, &audio->control);
}

/* Claims and parses AudioStreaming interfaces associated with AudioControl. */
static int
usb_audio_scan_streams(
	struct usb_audio *audio)
{
	struct drv_usb_configuration *cfg;
	struct drv_usb_interface *iface;
	const struct drv_usb_interface_descriptor *idesc;
	unsigned count;
	unsigned index;
	int error;

	cfg = drv_usb_device_active_configuration(audio->device);
	if (cfg == NULL)
		return ENODEV;

	if (audio->version == UAC_VERSION_1 && audio->control.stream_interface_count > 0U) {
		for (index = 0U; index < audio->control.stream_interface_count; index++) {
			iface = drv_usb_configuration_find_interface(cfg, audio->control.stream_interfaces[index]);
			if (iface == NULL)
				continue;
			error = drv_usb_interface_claim(audio->control_iface, iface);
			if (error != 0)
				continue;
			if (audio->claimed_count < USB_AUDIO_STREAM_IFACES_MAX) {
				audio->claimed_ifaces[audio->claimed_count] = iface;
				audio->claimed_count++;
			}
			(void)usb_audio_parse_stream_iface(audio, iface);
		}
		return 0;
	}

	count = drv_usb_configuration_interface_count(cfg);
	for (index = 0U; index < count; index++) {
		iface = drv_usb_configuration_interface(cfg, index);
		if (iface == NULL || iface == audio->control_iface)
			continue;
		idesc = drv_usb_interface_descriptor(iface);
		if (idesc == NULL)
			continue;
		if (idesc->interface_class != UAC_INTERFACE_CLASS ||
		    idesc->interface_subclass != UAC_SUBCLASS_AUDIOSTREAMING)
			continue;

		error = drv_usb_interface_claim(audio->control_iface, iface);
		if (error != 0)
			continue;
		if (audio->claimed_count < USB_AUDIO_STREAM_IFACES_MAX) {
			audio->claimed_ifaces[audio->claimed_count] = iface;
			audio->claimed_count++;
		}
		(void)usb_audio_parse_stream_iface(audio, iface);
	}

	return 0;
}

/* Parses all non-zero alternates of one AudioStreaming interface. */
static int
usb_audio_parse_stream_iface(
	struct usb_audio *audio,
	struct drv_usb_interface *iface)
{
	unsigned alt_count;
	unsigned alt_idx;
	const struct drv_usb_host_interface *hif;
	const struct drv_usb_interface_descriptor *idesc;
	const void *descs[USB_AUDIO_EXTRA_MAX];
	size_t lengths[USB_AUDIO_EXTRA_MAX];
	unsigned extra_cnt;
	unsigned ep_cnt;
	unsigned i;
	struct drv_usb_endpoint *ep;
	const struct drv_usb_endpoint_descriptor *epd;
	struct uac_as_alt parsed;
	struct usb_audio_stream *target;

	alt_count = drv_usb_interface_alternate_count(iface);
	for (alt_idx = 0U; alt_idx < alt_count; alt_idx++) {
		hif = drv_usb_interface_alternate(iface, alt_idx);
		if (hif == NULL)
			continue;
		idesc = drv_usb_host_interface_descriptor(hif);
		if (idesc == NULL || idesc->alternate_setting == 0U)
			continue;

		extra_cnt = drv_usb_host_interface_extra_count(hif);
		if (extra_cnt > USB_AUDIO_EXTRA_MAX)
			extra_cnt = USB_AUDIO_EXTRA_MAX;
		for (i = 0U; i < extra_cnt; i++) {
			if (drv_usb_host_interface_extra(hif, i, &descs[i], &lengths[i]) != 0) {
				descs[i] = NULL;
				lengths[i] = 0U;
			}
		}

		if (uac_desc_parse_as_alt(audio->version, idesc->interface_number, idesc->alternate_setting,
		    descs, lengths, extra_cnt, &parsed) != 0)
			continue;

		ep_cnt = drv_usb_host_interface_endpoint_count(hif);
		for (i = 0U; i < ep_cnt; i++) {
			ep = drv_usb_host_interface_endpoint(hif, i);
			if (ep == NULL)
				continue;
			epd = drv_usb_endpoint_descriptor(ep);
			if (epd == NULL)
				continue;
			uac_desc_add_ep(&parsed, epd->address, epd->attributes,
			    epd->maximum_packet_size & 0x07ffU, epd->interval, 0U);
		}

		if (uac_desc_resolve_path(&audio->control, &parsed) != 0)
			continue;

		if (audio->version == UAC_VERSION_2 && parsed.clock_id != 0U)
			usb_audio_query_uac2_rates(audio, &parsed);

		target = (parsed.capture != 0U) ? &audio->capture : &audio->playback;
		if (target->usb_iface == NULL) {
			target->usb_iface = iface;
			target->iface_number = idesc->interface_number;
		}
		if (target->usb_iface == iface && target->alt_count < UAC_ALTS_MAX) {
			target->alts[target->alt_count] = parsed;
			target->alt_count++;
			target->available = 1U;
		}
	}

	return 0;
}

/* Queries UAC 2.0 Clock Source supported sample rates via RANGE request. */
static void
usb_audio_query_uac2_rates(
	struct usb_audio *audio,
	struct uac_as_alt *alt)
{
	uint8_t buf[64];
	size_t actual;
	int error;

	actual = 0U;
	error = drv_usb_control(
		audio->device,
		DRV_USB_DIR_IN | DRV_USB_REQUEST_CLASS | DRV_USB_RECIP_INTERFACE,
		UAC2_RANGE,
		(uint16_t)(UAC_CS_SAM_FREQ_CONTROL << 8),
		(uint16_t)(((uint16_t)alt->clock_id << 8) | audio->control_iface_num),
		buf,
		sizeof(buf),
		USB_AUDIO_CTL_TIMEOUT_MS,
		&actual);
	if (error == 0 && actual >= 2U)
		(void)uac_desc_parse_clock_ranges(buf, actual, alt);
}

/* Builds the deduplicated audio_format table exposed to drv_audio_register(). */
static void
usb_audio_build_formats(
	struct usb_audio *audio)
{
	static const uint32_t common_rates[] = { 48000U, 44100U, 96000U, 32000U, 16000U };
	const struct usb_audio_stream *streams[2];
	unsigned s;
	unsigned a;
	unsigned r;
	const struct uac_as_alt *alt;
	uint32_t uapi_fmt;

	streams[0] = &audio->playback;
	streams[1] = &audio->capture;

	for (s = 0U; s < 2U; s++) {
		if (streams[s]->available == 0U)
			continue;

		for (a = 0U; a < streams[s]->alt_count; a++) {
			alt = &streams[s]->alts[a];
			if (alt->is_pcm == 0U || alt->channels == 0U || alt->channels > UAC_CHANNELS_MAX)
				continue;

			if (alt->subslot_size == 2U)
				uapi_fmt = KERN_AUDIO_FORMAT_S16_LE;
			else if (alt->subslot_size == 3U || alt->subslot_size == 4U)
				uapi_fmt = KERN_AUDIO_FORMAT_S32_LE;
			else
				continue;

			for (r = 0U; r < alt->rate_count; r++)
				usb_audio_add_format(audio, uapi_fmt, alt->channels, alt->rates[r]);

			if (alt->continuous_rates != 0U) {
				for (r = 0U; r < sizeof(common_rates) / sizeof(common_rates[0]); r++) {
					if (uac_desc_alt_supports_rate(alt, common_rates[r]) != 0)
						usb_audio_add_format(audio, uapi_fmt, alt->channels, common_rates[r]);
				}
			}
		}
	}
}

/* Appends a format entry if not already present. */
static void
usb_audio_add_format(
	struct usb_audio *audio,
	uint32_t fmt,
	uint32_t channels,
	uint32_t rate)
{
	unsigned i;

	if (rate == 0U || audio->format_count >= USB_AUDIO_FORMATS_MAX)
		return;

	for (i = 0U; i < audio->format_count; i++) {
		if (audio->formats[i].format == fmt &&
		    audio->formats[i].channels == channels &&
		    audio->formats[i].rate == rate)
			return;
	}

	audio->formats[audio->format_count].format = fmt;
	audio->formats[audio->format_count].channels = channels;
	audio->formats[audio->format_count].rate = rate;
	audio->formats[audio->format_count].reserved = 0U;
	audio->format_count++;
}

/* Caches Feature Unit volume range (GET_MIN/GET_MAX/GET_RES) at probe time. */
static void
usb_audio_init_mixer(
	struct usb_audio *audio)
{
	const struct uac_feature_unit *fu;
	uint8_t ch;
	uint8_t buf[16];
	int error;

	audio->cached_vol.left = 100U;
	audio->cached_vol.right = 100U;
	audio->cached_vol.muted = 0U;
	audio->vol_range.valid = 0U;

	fu = usb_audio_primary_fu(audio);
	if (fu == NULL || fu->has_volume == 0U)
		return;

	ch = (fu->master_volume != 0U) ? 0U : 1U;
	if (audio->version == UAC_VERSION_1) {
		error = usb_audio_fu_req(audio, fu->unit_id, DRV_USB_DIR_IN, UAC1_GET_MIN, UAC_FU_VOLUME_CONTROL, ch, buf, 2U);
		if (error != 0)
			return;
		audio->vol_range.min_db = (int16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8));

		error = usb_audio_fu_req(audio, fu->unit_id, DRV_USB_DIR_IN, UAC1_GET_MAX, UAC_FU_VOLUME_CONTROL, ch, buf, 2U);
		if (error != 0)
			return;
		audio->vol_range.max_db = (int16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8));

		error = usb_audio_fu_req(audio, fu->unit_id, DRV_USB_DIR_IN, UAC1_GET_RES, UAC_FU_VOLUME_CONTROL, ch, buf, 2U);
		if (error == 0)
			audio->vol_range.res_db = (int16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8));
		else
			audio->vol_range.res_db = 256;
	} else {
		error = usb_audio_fu_req(audio, fu->unit_id, DRV_USB_DIR_IN, UAC2_RANGE, UAC_FU_VOLUME_CONTROL, ch, buf, 8U);
		if (error != 0)
			return;
		audio->vol_range.min_db = (int16_t)((uint16_t)buf[2] | ((uint16_t)buf[3] << 8));
		audio->vol_range.max_db = (int16_t)((uint16_t)buf[4] | ((uint16_t)buf[5] << 8));
		audio->vol_range.res_db = (int16_t)((uint16_t)buf[6] | ((uint16_t)buf[7] << 8));
	}

	if (audio->vol_range.max_db > audio->vol_range.min_db)
		audio->vol_range.valid = 1U;
}

/* Issues a class-specific control request to a Feature Unit. */
static int
usb_audio_fu_req(
	struct usb_audio *audio,
	uint8_t fu_id,
	uint8_t dir,
	uint8_t req,
	uint8_t cs,
	uint8_t ch,
	void *buf,
	size_t len)
{
	return drv_usb_control(
		audio->device,
		dir | DRV_USB_REQUEST_CLASS | DRV_USB_RECIP_INTERFACE,
		req,
		(uint16_t)(((uint16_t)cs << 8) | ch),
		(uint16_t)(((uint16_t)fu_id << 8) | audio->control_iface_num),
		buf,
		len,
		USB_AUDIO_CTL_TIMEOUT_MS,
		NULL);
}

/* Returns the primary Feature Unit (playback preferred, else capture). */
static const struct uac_feature_unit *
usb_audio_primary_fu(
	const struct usb_audio *audio)
{
	unsigned i;

	for (i = 0U; i < audio->playback.alt_count; i++) {
		if (audio->playback.alts[i].fu.unit_id != 0U)
			return &audio->playback.alts[i].fu;
	}

	for (i = 0U; i < audio->capture.alt_count; i++) {
		if (audio->capture.alts[i].fu.unit_id != 0U)
			return &audio->capture.alts[i].fu;
	}

	return NULL;
}

/* Releases claimed interfaces and frees the usb_audio structure. */
static void
usb_audio_free(
	struct usb_audio *audio)
{
	unsigned i;

	for (i = 0U; i < audio->claimed_count; i++) {
		if (audio->claimed_ifaces[i] != NULL)
			(void)drv_usb_interface_release(audio->control_iface, audio->claimed_ifaces[i]);
	}

	kern_free(audio);
}

/* Audio framework prepare callback. */
static int
usb_audio_op_prepare(
	void *priv,
	int capture,
	const struct audio_format *format,
	const struct drv_dma_buffer *buffer,
	uint32_t fragment_bytes,
	uint32_t fragment_count)
{
	struct usb_audio *audio;
	struct usb_audio_stream *stream;
	int error;

	audio = (struct usb_audio *)priv;
	if (audio == NULL)
		return EINVAL;

	mutex_lock(&audio->mutex);
	if (audio->disconnected != 0U) {
		mutex_unlock(&audio->mutex);
		return ENODEV;
	}
	stream = (capture != 0) ? &audio->capture : &audio->playback;
	error = usb_audio_stream_prepare(stream, format, buffer, fragment_bytes, fragment_count);
	mutex_unlock(&audio->mutex);

	return error;
}

/* Audio framework start callback. */
static int
usb_audio_op_start(
	void *priv,
	int capture)
{
	struct usb_audio *audio;
	struct usb_audio_stream *stream;
	int error;

	audio = (struct usb_audio *)priv;
	if (audio == NULL)
		return EINVAL;

	mutex_lock(&audio->mutex);
	if (audio->disconnected != 0U) {
		mutex_unlock(&audio->mutex);
		return ENODEV;
	}
	stream = (capture != 0) ? &audio->capture : &audio->playback;
	error = usb_audio_stream_start(stream);
	mutex_unlock(&audio->mutex);

	return error;
}

/* Audio framework stop callback. */
static void
usb_audio_op_stop(
	void *priv,
	int capture)
{
	struct usb_audio *audio;
	struct usb_audio_stream *stream;

	audio = (struct usb_audio *)priv;
	if (audio == NULL)
		return;

	mutex_lock(&audio->mutex);
	stream = (capture != 0) ? &audio->capture : &audio->playback;
	(void)usb_audio_stream_stop(stream);
	mutex_unlock(&audio->mutex);
}

/* Audio framework position callback (called under IRQ-safe lock). */
static uint32_t
usb_audio_op_position(
	void *priv,
	int capture)
{
	struct usb_audio *audio;
	struct usb_audio_stream *stream;

	audio = (struct usb_audio *)priv;
	if (audio == NULL)
		return 0U;

	stream = (capture != 0) ? &audio->capture : &audio->playback;
	return usb_audio_stream_position(stream);
}

/* Audio framework get_volume callback. */
static int
usb_audio_op_get_volume(
	void *priv,
	struct audio_volume *volume)
{
	struct usb_audio *audio;
	const struct uac_feature_unit *fu;
	uint8_t req;
	uint8_t buf[2];
	uint8_t ch;
	int16_t raw;
	int32_t span;
	int32_t pct;

	audio = (struct usb_audio *)priv;
	if (audio == NULL || volume == NULL)
		return EINVAL;

	mutex_lock(&audio->mutex);
	if (audio->disconnected != 0U) {
		mutex_unlock(&audio->mutex);
		return ENODEV;
	}

	*volume = audio->cached_vol;
	fu = usb_audio_primary_fu(audio);
	if (fu == NULL) {
		mutex_unlock(&audio->mutex);
		return 0;
	}

	req = (audio->version == UAC_VERSION_1) ? UAC1_GET_CUR : UAC2_CUR;
	if (fu->has_mute != 0U) {
		ch = (fu->master_mute != 0U) ? 0U : 1U;
		if (usb_audio_fu_req(audio, fu->unit_id, DRV_USB_DIR_IN, req, UAC_FU_MUTE_CONTROL, ch, buf, 1U) == 0)
			volume->muted = (buf[0] != 0U) ? 1U : 0U;
	}

	if (fu->has_volume != 0U && audio->vol_range.valid != 0U) {
		ch = (fu->master_volume != 0U) ? 0U : 1U;
		span = (int32_t)audio->vol_range.max_db - (int32_t)audio->vol_range.min_db;
		if (usb_audio_fu_req(audio, fu->unit_id, DRV_USB_DIR_IN, req, UAC_FU_VOLUME_CONTROL, ch, buf, 2U) == 0) {
			raw = (int16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8));
			pct = ((int32_t)(raw - audio->vol_range.min_db) * 100 + (span / 2)) / span;
			if (pct < 0)
				pct = 0;
			if (pct > 100)
				pct = 100;
			volume->left = (uint32_t)pct;
			volume->right = (uint32_t)pct;
		}
	}

	audio->cached_vol = *volume;
	mutex_unlock(&audio->mutex);
	return 0;
}

/* Audio framework set_volume callback. */
static int
usb_audio_op_set_volume(
	void *priv,
	const struct audio_volume *volume)
{
	struct usb_audio *audio;
	const struct uac_feature_unit *fu;
	uint8_t req;
	uint8_t buf[2];
	uint8_t ch;
	int32_t span;
	int32_t db;
	uint32_t pct;

	audio = (struct usb_audio *)priv;
	if (audio == NULL || volume == NULL)
		return EINVAL;

	mutex_lock(&audio->mutex);
	if (audio->disconnected != 0U) {
		mutex_unlock(&audio->mutex);
		return ENODEV;
	}

	audio->cached_vol = *volume;
	if (audio->cached_vol.left > 100U)
		audio->cached_vol.left = 100U;
	if (audio->cached_vol.right > 100U)
		audio->cached_vol.right = 100U;
	audio->cached_vol.muted = (audio->cached_vol.muted != 0U) ? 1U : 0U;

	fu = usb_audio_primary_fu(audio);
	if (fu == NULL) {
		mutex_unlock(&audio->mutex);
		return 0;
	}

	req = (audio->version == UAC_VERSION_1) ? UAC1_SET_CUR : UAC2_CUR;
	if (fu->has_mute != 0U && fu->mute_rw != 0U) {
		ch = (fu->master_mute != 0U) ? 0U : 1U;
		buf[0] = (uint8_t)audio->cached_vol.muted;
		(void)usb_audio_fu_req(audio, fu->unit_id, DRV_USB_DIR_OUT, req, UAC_FU_MUTE_CONTROL, ch, buf, 1U);
	}

	if (fu->has_volume != 0U && fu->volume_rw != 0U && audio->vol_range.valid != 0U) {
		span = (int32_t)audio->vol_range.max_db - (int32_t)audio->vol_range.min_db;
		if (fu->master_volume != 0U) {
			pct = (audio->cached_vol.left + audio->cached_vol.right) / 2U;
			db = (int32_t)audio->vol_range.min_db + ((int32_t)pct * span + 50) / 100;
			buf[0] = (uint8_t)(db & 0xff);
			buf[1] = (uint8_t)((db >> 8) & 0xff);
			(void)usb_audio_fu_req(audio, fu->unit_id, DRV_USB_DIR_OUT, req, UAC_FU_VOLUME_CONTROL, 0U, buf, 2U);
		} else {
			db = (int32_t)audio->vol_range.min_db + ((int32_t)audio->cached_vol.left * span + 50) / 100;
			buf[0] = (uint8_t)(db & 0xff);
			buf[1] = (uint8_t)((db >> 8) & 0xff);
			(void)usb_audio_fu_req(audio, fu->unit_id, DRV_USB_DIR_OUT, req, UAC_FU_VOLUME_CONTROL, 1U, buf, 2U);

			if (fu->channels >= 2U) {
				db = (int32_t)audio->vol_range.min_db + ((int32_t)audio->cached_vol.right * span + 50) / 100;
				buf[0] = (uint8_t)(db & 0xff);
				buf[1] = (uint8_t)((db >> 8) & 0xff);
				(void)usb_audio_fu_req(audio, fu->unit_id, DRV_USB_DIR_OUT, req, UAC_FU_VOLUME_CONTROL, 2U, buf, 2U);
			}
		}
	}

	mutex_unlock(&audio->mutex);
	return 0;
}
