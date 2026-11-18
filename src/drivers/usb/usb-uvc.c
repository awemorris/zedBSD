/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The USB Video Class driver "uvc": binding and the camera source.
 *
 * The driver binds a VideoControl interface (class 0x0E, subclass 0x01).
 * Its header names the streaming interfaces; the first is claimed and its
 * alternate 0 read for the formats, frames and intervals.  A streaming
 * interface with an endpoint at alternate 0 moves its video by bulk;
 * otherwise each higher alternate has one isochronous IN endpoint, and
 * the alternates the host can take are marked.  What was read is told in
 * the log, and the camera is registered with the framework as /dev/camN.
 *
 * The framework calls the source operations under its own device mutex.
 * The stream itself is in usb-uvc-stream.c.  Only the framework stops a
 * stream: the detach retires the camera, which stops it; the quiesce
 * halts the transfers and tells the framework the stream failed.
 */

#include <drivers/camera/cam.h>
#include <drivers/usb/usb-uvc-desc.h>
#include <drivers/usb/usb-uvc.h>
#include <drivers/usb/usb.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <uapi/errno.h>

/* Marks a parameter a function takes for its signature's sake. */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(parameter) ((void)(parameter))
#endif

/* The buffers a stream may have. */
#define UVC_MAX_BUFFERS			8U

/* The longest line of the descriptor dump. */
#define UVC_LOG_LINE			160U

/* The limit of one control request on the default control pipe. */
#define UVC_UNIT_CONTROL_TIMEOUT_MS	1000U

/* The largest value of a control the driver offers, in bytes. */
#define UVC_CONTROL_BYTES_MAX		4U

/* The interfaces the driver takes: a camera's VideoControl interface. */
static const struct drv_usb_id usb_uvc_ids[] = {
	{
		.match_flags = DRV_USB_ID_IF_CLASS | DRV_USB_ID_IF_SUBCLASS,
		.interface_class = UVC_INTERFACE_CLASS,
		.interface_subclass = UVC_SUBCLASS_VIDEOCONTROL
	}
};

static int usb_uvc_attach(struct drv_usb_interface *interface, const struct drv_usb_id *id);
static int usb_uvc_detach(struct drv_usb_interface *interface, unsigned flags);
static int usb_uvc_quiesce(struct drv_usb_interface *interface);
static void usb_uvc_identity(struct usb_uvc *uvc);
static int usb_uvc_read_control(struct usb_uvc *uvc);
static int usb_uvc_claim_stream(struct usb_uvc *uvc);
static void usb_uvc_read_stream(struct usb_uvc *uvc);
static void usb_uvc_read_alternates(struct usb_uvc *uvc);
static void usb_uvc_dump(const struct usb_uvc *uvc);
static void usb_uvc_dump_format(const struct usb_uvc *uvc, const struct uvc_format *format);
static void usb_uvc_dump_alternates(const struct usb_uvc *uvc);
static int usb_uvc_register_camera(struct usb_uvc *uvc);
static void usb_uvc_free(struct usb_uvc *uvc);
static int usb_uvc_interval_at(const struct uvc_frame *frame, unsigned position, uint32_t *interval_100ns);
static unsigned usb_uvc_interval_count(const struct uvc_frame *frame);
static int usb_uvc_enum_format(void *argument, unsigned index, struct cam_format_desc *desc);
static int usb_uvc_enum_mode(void *argument, uint32_t format, unsigned index, struct cam_mode_desc *desc);
static int usb_uvc_enum_interval(void *argument, uint32_t format, unsigned mode, unsigned index, uint32_t *interval_100ns);
static int usb_uvc_try_mode(void *argument, const struct drv_cam_mode_request *request, struct drv_cam_mode_result *result);
static int usb_uvc_set_mode(void *argument, const struct drv_cam_mode_request *request, struct drv_cam_mode_result *result);
static int usb_uvc_start(void *argument);
static void usb_uvc_stop(void *argument);
static int usb_uvc_read_counters(void *argument, struct cam_counters *counters);
static uint32_t usb_uvc_host_payload_max(void *argument);
static int usb_uvc_control_info(void *argument, uint32_t id, struct cam_control_info *info);
static int usb_uvc_get_control(void *argument, uint32_t id, int64_t *value);
static int usb_uvc_set_control(void *argument, uint32_t id, int64_t value);
static int usb_uvc_unit_request(struct usb_uvc *uvc, uint8_t request, const struct uvc_control_map *map, uint8_t unit_id, uint8_t *bytes, size_t length);
static int usb_uvc_unit_value(struct usb_uvc *uvc, uint8_t request, const struct uvc_control_map *map, uint8_t unit_id, int64_t *value);
static unsigned usb_uvc_control_count(const struct usb_uvc *uvc);

/* The driver, registered once at boot. */
static struct drv_usb_driver usb_uvc_driver = {
	.name = "uvc",
	.ids = usb_uvc_ids,
	.id_count = sizeof(usb_uvc_ids) / sizeof(usb_uvc_ids[0]),
	.attach = usb_uvc_attach,
	.detach = usb_uvc_detach,
	.quiesce = usb_uvc_quiesce
};

/* What a USB camera does for the framework. */
static const struct drv_cam_source_ops usb_uvc_source = {
	.enum_format = usb_uvc_enum_format,
	.enum_mode = usb_uvc_enum_mode,
	.enum_interval = usb_uvc_enum_interval,
	.try_mode = usb_uvc_try_mode,
	.set_mode = usb_uvc_set_mode,
	.start = usb_uvc_start,
	.stop = usb_uvc_stop,
	.control_info = usb_uvc_control_info,
	.get_control = usb_uvc_get_control,
	.set_control = usb_uvc_set_control,
	.read_counters = usb_uvc_read_counters,
	.host_payload_max = usb_uvc_host_payload_max
};

/*
 * Registers the driver with the USB core.
 */
int
drv_usb_uvc_driver_register(
	void)
{
	int error;

	/* The core matches it against every interface from now on. */
	error = drv_usb_driver_register(&usb_uvc_driver);
	if (error != 0)
		return error;

	/* Succeeded: the driver is registered. */
	return 0;
}

/* Binds a camera: its descriptors, its streaming interface and its node. */
static int
usb_uvc_attach(
	struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	const struct uvc_format *offered;
	struct usb_uvc *uvc;
	int error;

	UNUSED_PARAMETER(id);

	/* The camera's record. */
	uvc = kern_calloc(1U, sizeof(*uvc));
	if (uvc == NULL)
		return ENOMEM;

	/* Remembers the interface and the device behind it. */
	uvc->control_interface = interface;
	uvc->device = drv_usb_interface_device(interface);

	/* The lock that serializes the negotiation, the stream and the detach. */
	error = mutex_init(&uvc->mutex, LOCK_RANK_DEVICE, "uvc");
	if (error != 0) {
		kern_free(uvc);
		return error;
	}

	/* The stream's handshake with its transfers. */
	uvc_stream_init(uvc);

	/* The product's name, the place and the bus speed. */
	usb_uvc_identity(uvc);

	/* The VideoControl header: the version, the clock and the streaming interfaces. */
	error = usb_uvc_read_control(uvc);
	if (error != 0) {
		usb_uvc_free(uvc);
		return error;
	}

	/* The first streaming interface, claimed for this camera. */
	error = usb_uvc_claim_stream(uvc);
	if (error != 0) {
		usb_uvc_free(uvc);
		return error;
	}

	/* Its formats, frames and intervals, then its transport and alternates. */
	usb_uvc_read_stream(uvc);
	usb_uvc_read_alternates(uvc);

	/* What was read, in the log. */
	usb_uvc_dump(uvc);

	/* A camera without a format the driver offers is not registered. */
	offered = uvc_desc_offered_format(&uvc->stream, 0U);
	if (offered == NULL) {
		kern_logf("usb-uvc: %s: no format offered\n", uvc->tag);
		usb_uvc_free(uvc);
		return ENODEV;
	}

	/* A camera without a way to move its video is not registered either. */
	if (uvc->transport == UVC_TRANSPORT_NONE) {
		kern_logf("usb-uvc: %s: no video endpoint\n", uvc->tag);
		usb_uvc_free(uvc);
		return ENODEV;
	}

	/* The record the detach finds. */
	error = drv_usb_interface_set_driver_data(interface, uvc);
	if (error != 0) {
		usb_uvc_free(uvc);
		return error;
	}

	/* The node, published by the framework. */
	error = usb_uvc_register_camera(uvc);
	if (error != 0) {
		(void)drv_usb_interface_set_driver_data(interface, NULL);
		usb_uvc_free(uvc);
		return error;
	}

	/* Succeeded: the camera is in service. */
	return 0;
}

/*
 * Gives the camera up: the framework retires its node, which stops a
 * running stream, then the streaming interface and the record go.
 */
static int
usb_uvc_detach(
	struct drv_usb_interface *interface,
	unsigned flags)
{
	struct usb_uvc *uvc;
	int error;

	UNUSED_PARAMETER(flags);

	/* The camera the interface had. */
	uvc = drv_usb_interface_driver_data(interface);
	if (uvc == NULL)
		return 0;

	/* The framework stops the stream and calls the source no more. */
	if (uvc->camera != NULL) {
		drv_cam_unregister(uvc->camera);
		uvc->camera = NULL;
	}

	/*
	 * A transfer a halt could not get back is tried again; one the host
	 * controller still holds keeps everything, and the core tries the
	 * detach again later.
	 */
	mutex_lock(&uvc->mutex);
	error = uvc_stream_halt(uvc);
	mutex_unlock(&uvc->mutex);

	/* Keeps the camera bound while a transfer is still out. */
	if (error != 0)
		return error;

	/* The streaming interface is given back, then the record. */
	(void)drv_usb_interface_set_driver_data(interface, NULL);
	usb_uvc_free(uvc);

	/* Succeeded: the camera is given up. */
	return 0;
}

/*
 * Stops the camera's transfers for the USB core's suspend or reset.
 *
 * The transfers and the worker are halted and the framework is told the
 * stream failed, so a reader sees EIO and stops the stream, which then
 * finds nothing left to halt.  The binding stays.
 */
static int
usb_uvc_quiesce(
	struct drv_usb_interface *interface)
{
	struct usb_uvc *uvc;
	unsigned was_running;
	int error;

	/* The camera the interface has. */
	uvc = drv_usb_interface_driver_data(interface);
	if (uvc == NULL)
		return 0;

	/* Halts a running stream. */
	mutex_lock(&uvc->mutex);
	was_running = uvc->running;
	error = uvc_stream_halt(uvc);
	mutex_unlock(&uvc->mutex);

	/* A stream that ran fails, so its reader stops it. */
	if (was_running != 0U && uvc->camera != NULL)
		drv_cam_source_fault(uvc->camera, EIO);

	/* Reports a transfer the host controller still holds. */
	if (error != 0)
		return error;

	/* Succeeded: nothing of the stream runs. */
	return 0;
}

/* Builds the product's name, the place, the log's tag and the bus speed. */
static void
usb_uvc_identity(
	struct usb_uvc *uvc)
{
	const struct drv_usb_device_descriptor *descriptor;
	enum drv_usb_speed speed;
	struct drv_usb_bus *bus;
	unsigned bus_number;
	unsigned port;

	/* The bus and the port, as the place and the log's tag. */
	bus = drv_usb_device_bus(uvc->device);
	bus_number = drv_usb_bus_number(bus);
	port = drv_usb_device_port(uvc->device);
	kern_snprintf(uvc->tag, sizeof(uvc->tag), "%u-%u", bus_number, port);
	kern_snprintf(uvc->location, sizeof(uvc->location), "usb %u-%u", bus_number, port);

	/* The product's name, or a plain one. */
	descriptor = drv_usb_device_descriptor(uvc->device);
	kern_snprintf(uvc->product, sizeof(uvc->product), "USB Video");
	if (descriptor->product_string != 0U)
		(void)drv_usb_device_get_string(uvc->device, descriptor->product_string, 0, uvc->product, sizeof(uvc->product));

	/* The speed the alternates are judged at. */
	speed = drv_usb_device_speed(uvc->device);
	switch (speed) {
	case DRV_USB_SPEED_FULL:
		uvc->speed = UVC_SPEED_FULL;
		break;
	case DRV_USB_SPEED_HIGH:
		uvc->speed = UVC_SPEED_HIGH;
		break;
	case DRV_USB_SPEED_SUPER:
	case DRV_USB_SPEED_SUPER_PLUS:
		uvc->speed = UVC_SPEED_SUPER;
		break;
	default:
		uvc->speed = UVC_SPEED_OTHER;
		break;
	}
}

/*
 * Reads the VideoControl interface's class-specific descriptors.
 * ENODEV without a header or a streaming interface.
 */
static int
usb_uvc_read_control(
	struct usb_uvc *uvc)
{
	const struct drv_usb_host_interface *alternate;
	const void *descriptor;
	unsigned count;
	unsigned index;
	size_t length;
	int error;

	/* The descriptors that follow the interface's active alternate. */
	alternate = drv_usb_interface_active_alternate(uvc->control_interface);
	if (alternate == NULL)
		return ENODEV;

	/* Reads each, until one is too short to read. */
	count = drv_usb_host_interface_extra_count(alternate);
	for (index = 0U; index < count; index++) {
		error = drv_usb_host_interface_extra(alternate, index, &descriptor, &length);
		if (error != 0)
			break;

		/* Parses the descriptor into the VideoControl record. */
		error = uvc_desc_control_feed(&uvc->control, descriptor, length);
		if (error != 0) {
			kern_logf("usb-uvc: %s: VideoControl descriptor %u too short (%u bytes)\n", uvc->tag, index, (unsigned)length);
			break;
		}
	}

	/* A VideoControl interface without its header is not a camera's. */
	if (uvc->control.has_header == 0U) {
		kern_logf("usb-uvc: %s: no VideoControl header\n", uvc->tag);
		return ENODEV;
	}

	/* Nor is one without a streaming interface. */
	if (uvc->control.stream_count == 0U) {
		kern_logf("usb-uvc: %s: no streaming interface\n", uvc->tag);
		return ENODEV;
	}

	/* Succeeded: the header is read. */
	return 0;
}

/*
 * Claims the first streaming interface the header names.  ENODEV when
 * it is not there or another driver has it.
 */
static int
usb_uvc_claim_stream(
	struct usb_uvc *uvc)
{
	const struct drv_usb_interface_descriptor *descriptor;
	struct drv_usb_configuration *configuration;
	struct drv_usb_interface *stream;
	int error;

	/* The interface in the active configuration. */
	configuration = drv_usb_device_active_configuration(uvc->device);
	if (configuration == NULL)
		return ENODEV;

	/* The streaming interface the control header names. */
	stream = drv_usb_configuration_find_interface(configuration, uvc->control.streams[0]);
	if (stream == NULL) {
		kern_logf("usb-uvc: %s: streaming interface %u not found\n", uvc->tag, (unsigned)uvc->control.streams[0]);
		return ENODEV;
	}

	/* It must be a VideoStreaming interface. */
	descriptor = drv_usb_interface_descriptor(stream);
	if (descriptor->interface_class != UVC_INTERFACE_CLASS ||
	    descriptor->interface_subclass != UVC_SUBCLASS_VIDEOSTREAMING) {
		kern_logf("usb-uvc: %s: interface %u is not a streaming interface\n", uvc->tag, (unsigned)uvc->control.streams[0]);
		return ENODEV;
	}

	/* Claimed for this camera. */
	error = drv_usb_interface_claim(uvc->control_interface, stream);
	if (error != 0) {
		kern_logf("usb-uvc: %s: streaming interface %u not claimed (%d)\n", uvc->tag, (unsigned)uvc->control.streams[0], error);
		return ENODEV;
	}

	/* Succeeded: the streaming interface is this camera's. */
	uvc->stream_interface = stream;
	uvc->stream_claimed = 1U;
	return 0;
}

/* Reads the streaming interface's formats, frames and intervals from alternate 0. */
static void
usb_uvc_read_stream(
	struct usb_uvc *uvc)
{
	const struct drv_usb_host_interface *alternate;
	const void *descriptor;
	unsigned count;
	unsigned index;
	size_t length;
	int error;

	/* Alternate 0 carries the class-specific descriptors. */
	alternate = drv_usb_interface_find_alternate(uvc->stream_interface, 0U);
	if (alternate == NULL)
		return;

	/* Reads each; one too short stops the parsing, and what was read before stays. */
	count = drv_usb_host_interface_extra_count(alternate);
	for (index = 0U; index < count; index++) {
		error = drv_usb_host_interface_extra(alternate, index, &descriptor, &length);
		if (error != 0)
			break;

		/* Parses the descriptor into the VideoStreaming record. */
		error = uvc_desc_stream_feed(&uvc->stream, descriptor, length);
		if (error != 0) {
			kern_logf("usb-uvc: %s: streaming descriptor %u too short (%u bytes), parsing stopped\n", uvc->tag, index,
			    (unsigned)length);
			break;
		}
	}
}

/*
 * Finds how the device moves its video: bulk on alternate 0, or one
 * isochronous IN endpoint on each higher alternate.
 */
static void
usb_uvc_read_alternates(
	struct usb_uvc *uvc)
{
	const struct drv_usb_superspeed_endpoint_companion_descriptor *companion;
	const struct drv_usb_endpoint_descriptor *endpoint_descriptor;
	const struct drv_usb_interface_descriptor *interface_descriptor;
	const struct drv_usb_host_interface *alternate;
	struct drv_usb_endpoint *endpoint;
	enum drv_usb_transfer_type type;
	uint32_t superspeed_bytes;
	unsigned endpoints;
	unsigned count;
	unsigned index;
	unsigned point;
	bool input;

	/* A bulk IN endpoint at alternate 0 makes a bulk device. */
	alternate = drv_usb_interface_find_alternate(uvc->stream_interface, 0U);
	if (alternate != NULL) {
		endpoints = drv_usb_host_interface_endpoint_count(alternate);
		for (point = 0U; point < endpoints; point++) {
			endpoint = drv_usb_host_interface_endpoint(alternate, point);
			type = drv_usb_endpoint_type(endpoint);
			input = drv_usb_endpoint_is_input(endpoint);
			if (type != DRV_USB_TRANSFER_BULK || !input)
				continue;
			uvc->transport = UVC_TRANSPORT_BULK;
			uvc->bulk_endpoint = endpoint;
			uvc->bulk_packet_bytes = (uint16_t)(drv_usb_endpoint_max_packet_size(endpoint) & 0x07ffU);
			return;
		}
	}

	/* Otherwise every alternate's isochronous IN endpoint. */
	count = drv_usb_interface_alternate_count(uvc->stream_interface);
	for (index = 0U; index < count; index++) {
		alternate = drv_usb_interface_alternate(uvc->stream_interface, index);
		if (alternate == NULL)
			continue;

		/* Each of its endpoints, for the isochronous IN one. */
		interface_descriptor = drv_usb_host_interface_descriptor(alternate);
		endpoints = drv_usb_host_interface_endpoint_count(alternate);
		for (point = 0U; point < endpoints; point++) {
			endpoint = drv_usb_host_interface_endpoint(alternate, point);
			type = drv_usb_endpoint_type(endpoint);
			input = drv_usb_endpoint_is_input(endpoint);
			if (type != DRV_USB_TRANSFER_ISOCHRONOUS || !input)
				continue;

			/* An alternate beyond the table is left out. */
			if (uvc->alternate_count >= UVC_ALTERNATES_MAX)
				break;

			/* At SuperSpeed the companion tells the bytes an interval. */
			superspeed_bytes = 0U;
			companion = drv_usb_endpoint_superspeed_companion(endpoint);
			if (companion != NULL)
				superspeed_bytes = companion->bytes_per_interval;

			/* The alternate's entry, marked when the host takes it. */
			endpoint_descriptor = drv_usb_endpoint_descriptor(endpoint);
			uvc_alternate_describe(&uvc->alternates[uvc->alternate_count],
			    interface_descriptor->alternate_setting,
			    endpoint_descriptor->address,
			    endpoint_descriptor->maximum_packet_size,
			    endpoint_descriptor->interval,
			    uvc->speed,
			    superspeed_bytes);
			uvc->alternate_count++;
			uvc->transport = UVC_TRANSPORT_ISOCHRONOUS;
			break;
		}
	}
}

/* Tells what was read: the version and clock, each format, the colour and the alternates. */
static void
usb_uvc_dump(
	const struct usb_uvc *uvc)
{
	const char *transport;
	const char *parsing;
	unsigned processing_unit_id;
	unsigned camera_terminal_id;
	unsigned control_count;
	unsigned index;

	/* The transport's name. */
	transport = "none";
	if (uvc->transport == UVC_TRANSPORT_ISOCHRONOUS)
		transport = "isochronous";
	else if (uvc->transport == UVC_TRANSPORT_BULK)
		transport = "bulk";

	/* The version, the clock, the interface and the endpoint. */
	kern_logf("usb-uvc: %s: %s, UVC %x.%02x, clock %u Hz, VS interface %u, endpoint 0x%02x %s\n",
	    uvc->tag,
	    uvc->product,
	    (unsigned)(uvc->control.bcd_uvc >> 8),
	    (unsigned)(uvc->control.bcd_uvc & 0xffU),
	    (unsigned)uvc->control.clock_hz,
	    (unsigned)uvc->control.streams[0],
	    (unsigned)uvc->stream.endpoint_address,
	    transport);

	/* The unit and terminal numbers, zero for a unit the device does not have. */
	processing_unit_id = 0U;
	if (uvc->control.processing_unit != 0U)
		processing_unit_id = (unsigned)uvc->control.processing_unit_id;
	camera_terminal_id = 0U;
	if (uvc->control.camera_terminal != 0U)
		camera_terminal_id = (unsigned)uvc->control.camera_terminal_id;

	/* How many controls the driver and the device have in common. */
	control_count = usb_uvc_control_count(uvc);

	/* The units that carry controls, with their bmControls. */
	kern_logf("usb-uvc: %s: processing unit %u controls 0x%x, camera terminal %u controls 0x%x, %u controls offered\n",
	    uvc->tag,
	    processing_unit_id,
	    (unsigned)uvc->control.processing_controls,
	    camera_terminal_id,
	    (unsigned)uvc->control.camera_controls,
	    control_count);

	/* Each format with its frames. */
	for (index = 0U; index < uvc->stream.format_count; index++)
		usb_uvc_dump_format(uvc, &uvc->stream.formats[index]);

	/* What the parser left out. */
	if (uvc->stream.stopped != 0U ||
	    uvc->stream.dropped_frames != 0U ||
	    uvc->stream.dropped_intervals != 0U) {
		parsing = "complete";
		if (uvc->stream.stopped != 0U)
			parsing = "stopped";
		kern_logf("usb-uvc: %s: parsing %s, %u frames and %u intervals left out\n",
		    uvc->tag,
		    parsing,
		    uvc->stream.dropped_frames,
		    uvc->stream.dropped_intervals);
	}

	/* The alternates and the host's limit. */
	if (uvc->transport == UVC_TRANSPORT_ISOCHRONOUS)
		usb_uvc_dump_alternates(uvc);
	if (uvc->transport == UVC_TRANSPORT_BULK)
		kern_logf("usb-uvc: %s: bulk packet %u\n", uvc->tag, (unsigned)uvc->bulk_packet_bytes);
}

/* Tells one format: its kind, its frames with their rates, and its colour. */
static void
usb_uvc_dump_format(
	const struct usb_uvc *uvc,
	const struct uvc_format *format)
{
	const struct uvc_frame *frame;
	char line[UVC_LOG_LINE];
	const char *name;
	uint32_t rate;
	size_t used;
	unsigned index;
	unsigned interval;
	int written;

	/* The format's name, or its FourCC when the driver does not offer it. */
	name = "MJPEG";
	if (format->kind == UVC_FORMAT_UNCOMPRESSED) {
		if (format->cam_format == CAM_FORMAT_YUYV)
			name = "YUY2";
		else if (format->cam_format == CAM_FORMAT_UYVY)
			name = "UYVY";
		else if (format->cam_format == CAM_FORMAT_NV12)
			name = "NV12";
		else
			name = NULL;
	}

	/* The format's own line. */
	if (name != NULL) {
		kern_logf("usb-uvc: %s: format %u %s %u bpp (default frame %u), %u frames\n", uvc->tag, (unsigned)format->index, name,
		    (unsigned)format->bits_per_pixel, (unsigned)format->default_frame, format->frame_count);
	} else {
		kern_logf("usb-uvc: %s: format %u %c%c%c%c not offered\n", uvc->tag, (unsigned)format->index, format->guid[0],
		    format->guid[1], format->guid[2], format->guid[3]);
	}

	/* A line a frame: its size and its rates in frames a second. */
	for (index = 0U; index < format->frame_count; index++) {
		frame = &format->frames[index];
		used = 0U;
		line[0] = '\0';
		for (interval = 0U; interval < frame->interval_count; interval++) {
			rate = 0U;
			if (frame->intervals[interval] != 0U)
				rate = 10000000U / frame->intervals[interval];
			written = kern_snprintf(line + used, sizeof(line) - used, " %u", (unsigned)rate);
			if (written < 0 || (size_t)written >= sizeof(line) - used)
				break;
			used += (size_t)written;
		}

		/* A continuous frame tells its range instead. */
		if (frame->interval_type == 0U) {
			kern_logf("usb-uvc: %s:   frame %u %ux%u, intervals %u..%u step %u, buffer %u\n", uvc->tag,
			    (unsigned)frame->index, (unsigned)frame->width, (unsigned)frame->height, (unsigned)frame->interval_min,
			    (unsigned)frame->interval_max, (unsigned)frame->interval_step, (unsigned)frame->max_frame_bytes);
		} else {
			kern_logf("usb-uvc: %s:   frame %u %ux%u fps%s, buffer %u\n", uvc->tag, (unsigned)frame->index,
			    (unsigned)frame->width, (unsigned)frame->height, line, (unsigned)frame->max_frame_bytes);
		}
	}

	/* The colour, when the device told it. */
	if (format->has_color != 0U) {
		kern_logf("usb-uvc: %s:   colorformat primaries %u transfer %u matrix %u\n", uvc->tag,
		    (unsigned)format->color_primaries, (unsigned)format->color_transfer, (unsigned)format->color_matrix);
	}
}

/* Tells the alternates: each one's capacity, the taken ones marked, and the host's limit. */
static void
usb_uvc_dump_alternates(
	const struct usb_uvc *uvc)
{
	const struct uvc_alternate *alternate;
	char line[UVC_LOG_LINE];
	const char *taken;
	size_t used;
	unsigned host_max;
	unsigned index;
	int written;

	/* Each alternate: setting/packet x transactions, a star when the host takes it. */
	used = 0U;
	line[0] = '\0';
	for (index = 0U; index < uvc->alternate_count; index++) {
		alternate = &uvc->alternates[index];
		taken = "";
		if (alternate->accepted != 0U)
			taken = "*";
		written = kern_snprintf(line + used, sizeof(line) - used, " %u:%ux%u%s", (unsigned)alternate->setting,
		    (unsigned)alternate->packet_bytes, (unsigned)alternate->transactions, taken);
		if (written < 0 || (size_t)written >= sizeof(line) - used)
			break;
		used += (size_t)written;
	}

	/* The host controller's limit. */
	host_max = (unsigned)uvc_alternate_host_max(uvc->alternates, uvc->alternate_count);

	/* The line, with the host's limit. */
	kern_logf("usb-uvc: %s: alternates%s (host accepts up to %u)\n", uvc->tag, line, host_max);
}

/* Registers the camera with the framework. */
static int
usb_uvc_register_camera(
	struct usb_uvc *uvc)
{
	struct drv_cam_device_desc desc;
	const struct uvc_format *format;
	unsigned control_count;
	unsigned index;
	int error;

	/* The camera as the framework shows it. */
	kern_memset(&desc, 0, sizeof(desc));
	desc.driver_name = "uvc";
	kern_snprintf(desc.device_name, sizeof(desc.device_name), "%s", uvc->product);
	kern_snprintf(desc.location, sizeof(desc.location), "%s", uvc->location);
	desc.source = &usb_uvc_source;
	desc.source_argument = uvc;
	desc.flags = DRV_CAM_SOURCE_CPU_WRITES;
	desc.max_buffers = UVC_MAX_BUFFERS;
	desc.buffer_alignment = 1U;
	desc.clock_hz = uvc->control.clock_hz;

	/* The kinds of format it offers. */
	for (index = 0U; index < uvc->stream.format_count; index++) {
		format = &uvc->stream.formats[index];
		if (format->cam_format == 0U)
			continue;
		if (format->kind == UVC_FORMAT_MJPEG)
			desc.capabilities |= CAM_CAP_COMPRESSED;
		else
			desc.capabilities |= CAM_CAP_YUV;
	}

	/* Controls, when the device declares any the driver knows. */
	control_count = usb_uvc_control_count(uvc);
	if (control_count != 0U)
		desc.capabilities |= CAM_CAP_CONTROLS;

	/* The node. */
	error = drv_cam_register(&desc, &uvc->camera);
	if (error != 0) {
		kern_logf("usb-uvc: %s: camera not registered (%d)\n", uvc->tag, error);
		return error;
	}

	/* Succeeded: /dev/camN is the camera's. */
	return 0;
}

/* Gives back the streaming interface and frees the record. */
static void
usb_uvc_free(
	struct usb_uvc *uvc)
{
	/* The streaming interface, when it was claimed. */
	if (uvc->stream_claimed != 0U)
		(void)drv_usb_interface_release(uvc->control_interface, uvc->stream_interface);
	uvc->stream_claimed = 0U;

	/* The record. */
	kern_free(uvc);
}

/*
 * Reports a frame's interval at a position of the framework's order: the
 * default first, then the descriptor's discrete intervals in their order
 * without the default.  ENOENT past the last.
 */
static int
usb_uvc_interval_at(
	const struct uvc_frame *frame,
	unsigned position,
	uint32_t *interval_100ns)
{
	unsigned index;
	unsigned seen;

	/* The default comes first. */
	if (position == 0U) {
		*interval_100ns = frame->default_interval;
		return 0;
	}

	/* A continuous frame has only the default in the list. */
	if (frame->interval_type == 0U)
		return ENOENT;

	/* The others in the descriptor's order, without the default. */
	seen = 1U;
	for (index = 0U; index < frame->interval_count; index++) {
		if (frame->intervals[index] == frame->default_interval)
			continue;
		if (seen == position) {
			*interval_100ns = frame->intervals[index];
			return 0;
		}

		/* Counts this interval as passed. */
		seen++;
	}

	/* The position is past the last interval. */
	return ENOENT;
}

/* Reports how many intervals the framework's order of a frame holds. */
static unsigned
usb_uvc_interval_count(
	const struct uvc_frame *frame)
{
	unsigned index;
	unsigned count;

	/* The default, then each other discrete interval. */
	count = 1U;
	if (frame->interval_type == 0U)
		return count;

	/* Each interval that is not the default. */
	for (index = 0U; index < frame->interval_count; index++) {
		if (frame->intervals[index] != frame->default_interval)
			count++;
	}

	/* Succeeded: the count. */
	return count;
}

/* Answers the framework's format enumeration with the formats the driver offers. */
static int
usb_uvc_enum_format(
	void *argument,
	unsigned index,
	struct cam_format_desc *desc)
{
	const struct uvc_format *format;
	struct usb_uvc *uvc;

	/* The index-th offered format. */
	uvc = argument;
	format = uvc_desc_offered_format(&uvc->stream, index);
	if (format == NULL)
		return ENOENT;

	/* Its code, kind and size. */
	desc->format = format->cam_format;
	desc->flags = CAM_FORMAT_SUPPORTED;
	desc->bits_per_pixel = format->bits_per_pixel;
	desc->mode_count = format->frame_count;

	/* The pixel groups: two pixels in four bytes for the packed YUV, none for MJPEG. */
	if (format->kind == UVC_FORMAT_MJPEG) {
		desc->flags |= CAM_FORMAT_COMPRESSED;
	} else if (format->cam_format == CAM_FORMAT_NV12) {
		desc->bytes_per_group = 6U;
		desc->pixels_per_group = 4U;
	} else {
		desc->bytes_per_group = 4U;
		desc->pixels_per_group = 2U;
	}

	/* Succeeded: the format is described. */
	return 0;
}

/* Answers the framework's mode enumeration with a format's frames. */
static int
usb_uvc_enum_mode(
	void *argument,
	uint32_t format_code,
	unsigned index,
	struct cam_mode_desc *desc)
{
	const struct uvc_format *format;
	const struct uvc_frame *frame;
	struct usb_uvc *uvc;
	unsigned position;
	uint32_t interval;
	int error;

	/* The format the code names. */
	uvc = argument;
	format = uvc_desc_find_format(&uvc->stream, format_code);
	if (format == NULL)
		return EINVAL;

	/* Its index-th frame. */
	if (index >= format->frame_count)
		return ENOENT;

	/* Selects the frame. */
	frame = &format->frames[index];

	/* The frame's size and its largest frame. */
	desc->width = frame->width;
	desc->height = frame->height;
	desc->max_frame_bytes = uvc_desc_image_bytes(format, frame, 0U);

	/* A continuous range: its bounds, with the default as the one inline interval. */
	if (frame->interval_type == 0U) {
		desc->interval_type = CAM_INTERVAL_CONTINUOUS;
		desc->interval_min = frame->interval_min;
		desc->interval_max = frame->interval_max;
		desc->interval_step = frame->interval_step;
		desc->interval_count = 1U;
		desc->intervals[0] = frame->default_interval;
		return 0;
	}

	/* Discrete intervals: the count, and the first ones inline. */
	desc->interval_type = CAM_INTERVAL_DISCRETE;
	desc->interval_count = usb_uvc_interval_count(frame);
	for (position = 0U; position < CAM_MODE_INTERVALS_INLINE; position++) {
		error = usb_uvc_interval_at(frame, position, &interval);
		if (error != 0)
			break;
		desc->intervals[position] = interval;
	}

	/* Succeeded: the mode is described. */
	return 0;
}

/* Answers the framework's interval enumeration beyond the inline ones. */
static int
usb_uvc_enum_interval(
	void *argument,
	uint32_t format_code,
	unsigned mode,
	unsigned index,
	uint32_t *interval_100ns)
{
	const struct uvc_format *format;
	struct usb_uvc *uvc;
	int error;

	/* The format and its frame. */
	uvc = argument;
	format = uvc_desc_find_format(&uvc->stream, format_code);
	if (format == NULL)
		return EINVAL;
	if (mode >= format->frame_count)
		return ENOENT;

	/* The interval at the position in the framework's order. */
	error = usb_uvc_interval_at(&format->frames[mode], index, interval_100ns);
	if (error != 0)
		return error;

	/* Succeeded: the interval. */
	return 0;
}

/* Answers TRY_FORMAT with the device's PROBE answer. */
static int
usb_uvc_try_mode(
	void *argument,
	const struct drv_cam_mode_request *request,
	struct drv_cam_mode_result *result)
{
	struct usb_uvc *uvc;
	int error;

	/* PROBE only. */
	uvc = argument;
	mutex_lock(&uvc->mutex);
	error = uvc_stream_negotiate(uvc, request, result, 0U);
	mutex_unlock(&uvc->mutex);

	/* Reports a refused mode, ENOSPC with the answer filled. */
	if (error != 0)
		return error;

	/* Succeeded: the device's answer. */
	return 0;
}

/* Answers SET_FORMAT: PROBE, then COMMIT of the answer. */
static int
usb_uvc_set_mode(
	void *argument,
	const struct drv_cam_mode_request *request,
	struct drv_cam_mode_result *result)
{
	struct usb_uvc *uvc;
	int error;

	/* PROBE and COMMIT. */
	uvc = argument;
	mutex_lock(&uvc->mutex);
	error = uvc_stream_negotiate(uvc, request, result, 1U);
	mutex_unlock(&uvc->mutex);

	/* Reports a refused mode, ENOSPC with the answer filled. */
	if (error != 0)
		return error;

	/* Succeeded: the mode is committed. */
	return 0;
}

/* Starts the committed stream for the framework. */
static int
usb_uvc_start(
	void *argument)
{
	struct usb_uvc *uvc;
	int error;

	/* The transfers and the worker. */
	uvc = argument;
	mutex_lock(&uvc->mutex);
	error = uvc_stream_start(uvc);
	mutex_unlock(&uvc->mutex);

	/* Reports a stream that did not start; nothing of it is left running. */
	if (error != 0)
		return error;

	/* Succeeded: frames are coming. */
	return 0;
}

/*
 * Stops the stream for the framework: the transfers and the worker, then
 * the slot being filled.
 */
static void
usb_uvc_stop(
	void *argument)
{
	struct usb_uvc *uvc;
	int error;

	/* The halt, harmless after a quiesce did it already. */
	uvc = argument;
	mutex_lock(&uvc->mutex);
	error = uvc_stream_halt(uvc);
	if (error != 0)
		kern_logf("usb-uvc: %s: a transfer did not retire (%d)\n", uvc->tag, error);

	/* The slot being filled goes back to the framework. */
	uvc_stream_abandon(uvc);
	mutex_unlock(&uvc->mutex);
}

/* Adds the transport's counts to the framework's counters. */
static int
usb_uvc_read_counters(
	void *argument,
	struct cam_counters *counters)
{
	struct usb_uvc *uvc;

	/* The assembler's and the stream's counts. */
	uvc = argument;
	counters->invalid_header = uvc->assembler.invalid_header;
	counters->short_header = uvc->assembler.short_header;
	counters->missed_packets = uvc->assembler.missed_packets;
	counters->packet_errors = uvc->assembler.packet_errors;
	counters->no_signal = uvc->no_signal;
	counters->resubmit_failures = uvc->resubmit_failures;
	counters->halt_timeouts = uvc->halt_timeouts;

	/* Succeeded: the counts are copied. */
	return 0;
}

/*
 * Reports the largest payload an interval of the host carries: the
 * largest alternate the host takes, or 0 (no limit) for a bulk device.
 */
static uint32_t
usb_uvc_host_payload_max(
	void *argument)
{
	struct usb_uvc *uvc;
	uint32_t host_max;

	/* A bulk device has no per-interval limit. */
	uvc = argument;
	if (uvc->transport != UVC_TRANSPORT_ISOCHRONOUS)
		return 0U;

	/* The largest alternate the host controller takes. */
	host_max = uvc_alternate_host_max(uvc->alternates, uvc->alternate_count);

	/* Succeeded: the limit per interval. */
	return host_max;
}

/*
 * Describes a control: unsupported unless the device declares it and
 * answers GET_INFO with GET, read-only without SET, and its range, step
 * and default from GET_MIN, GET_MAX, GET_RES and GET_DEF.  A switch is
 * 0 to 1; the auto-exposure switch reads the device's default mode.
 */
static int
usb_uvc_control_info(
	void *argument,
	uint32_t id,
	struct cam_control_info *info)
{
	const struct uvc_control_map *map;
	struct usb_uvc *uvc;
	uint8_t bytes[UVC_CONTROL_BYTES_MAX];
	uint8_t unit_id;
	int64_t value;
	int present;
	int error;

	/* A control the driver does not know, or the device does not declare, stays unsupported. */
	uvc = argument;
	info->flags = CAM_CONTROL_UNSUPPORTED;
	map = uvc_control_lookup(id);
	if (map == NULL)
		return 0;

	/* Names the control. */
	kern_snprintf(info->name, sizeof(info->name), "%s", map->name);

	/* A control no unit of the device declares stays unsupported. */
	present = uvc_control_present(&uvc->control, map, &unit_id);
	if (!present)
		return 0;

	/* GET_INFO says what the device lets the host do; a control it does not answer stays unsupported. */
	kern_memset(bytes, 0, sizeof(bytes));
	error = usb_uvc_unit_request(uvc, UVC_GET_INFO, map, unit_id, bytes, 1U);
	if (error != 0 || (bytes[0] & UVC_INFO_GET) == 0U)
		return 0;
	info->flags = 0U;
	if ((bytes[0] & UVC_INFO_SET) == 0U)
		info->flags |= CAM_CONTROL_READ_ONLY;

	/* A switch or a fixed range: only the default is asked. */
	if ((map->flags & (UVC_CONTROL_BOOLEAN | UVC_CONTROL_FIXED_RANGE)) != 0U) {
		if ((map->flags & UVC_CONTROL_BOOLEAN) != 0U)
			info->flags |= CAM_CONTROL_BOOLEAN;
		info->minimum = map->minimum;
		info->maximum = uvc_control_fixed_maximum(map, uvc->control.bcd_uvc);
		info->step = 1;
		error = usb_uvc_unit_value(uvc, UVC_GET_DEF, map, unit_id, &value);
		if (error != 0)
			return error;
		info->default_value = value;

		/* Succeeded: the switch or the fixed range. */
		return 0;
	}

	/* The device's range, step and default. */
	error = usb_uvc_unit_value(uvc, UVC_GET_MIN, map, unit_id, &info->minimum);
	if (error == 0)
		error = usb_uvc_unit_value(uvc, UVC_GET_MAX, map, unit_id, &info->maximum);
	if (error == 0)
		error = usb_uvc_unit_value(uvc, UVC_GET_RES, map, unit_id, &info->step);
	if (error == 0)
		error = usb_uvc_unit_value(uvc, UVC_GET_DEF, map, unit_id, &info->default_value);

	/* Reports a device that declared the control but did not answer for it. */
	if (error != 0)
		return error;

	/* Succeeded: the control's range. */
	return 0;
}

/* Reads a control's current value (GET_CUR). */
static int
usb_uvc_get_control(
	void *argument,
	uint32_t id,
	int64_t *value)
{
	const struct uvc_control_map *map;
	struct usb_uvc *uvc;
	uint8_t unit_id;
	int present;
	int error;

	/* Refuses a control the driver does not know. */
	uvc = argument;
	map = uvc_control_lookup(id);
	if (map == NULL)
		return EOPNOTSUPP;

	/* Refuses a control the device does not have. */
	present = uvc_control_present(&uvc->control, map, &unit_id);
	if (!present)
		return EOPNOTSUPP;

	/* The device's current value. */
	error = usb_uvc_unit_value(uvc, UVC_GET_CUR, map, unit_id, value);
	if (error != 0)
		return error;

	/* Succeeded: the value. */
	return 0;
}

/*
 * Sets a control (SET_CUR).  A switch takes 0 or 1; the auto-exposure
 * switch becomes the device's manual or automatic mode.  A value outside
 * the device's range is EINVAL, and a control GET_INFO calls read-only
 * or disabled by an automatic mode is EACCES.
 */
static int
usb_uvc_set_control(
	void *argument,
	uint32_t id,
	int64_t value)
{
	const struct uvc_control_map *map;
	struct usb_uvc *uvc;
	uint8_t bytes[UVC_CONTROL_BYTES_MAX];
	uint8_t unit_id;
	int64_t minimum;
	int64_t maximum;
	int present;
	int error;

	/* Refuses a control the driver does not know. */
	uvc = argument;
	map = uvc_control_lookup(id);
	if (map == NULL)
		return EOPNOTSUPP;

	/* Refuses a control the device does not have. */
	present = uvc_control_present(&uvc->control, map, &unit_id);
	if (!present)
		return EOPNOTSUPP;

	/* Refuses a control the device does not let the host set now. */
	kern_memset(bytes, 0, sizeof(bytes));
	error = usb_uvc_unit_request(uvc, UVC_GET_INFO, map, unit_id, bytes, 1U);
	if (error != 0)
		return error;
	if ((bytes[0] & UVC_INFO_SET) == 0U || (bytes[0] & UVC_INFO_DISABLED) != 0U)
		return EACCES;

	/* The range: a switch's or a fixed one, else the device's. */
	if ((map->flags & (UVC_CONTROL_BOOLEAN | UVC_CONTROL_FIXED_RANGE)) != 0U) {
		minimum = map->minimum;
		maximum = uvc_control_fixed_maximum(map, uvc->control.bcd_uvc);
	} else {
		error = usb_uvc_unit_value(uvc, UVC_GET_MIN, map, unit_id, &minimum);
		if (error == 0)
			error = usb_uvc_unit_value(uvc, UVC_GET_MAX, map, unit_id, &maximum);
		if (error != 0)
			return error;
	}

	/* Refuses a value outside the range the device accepts. */
	if (value < minimum || value > maximum)
		return EINVAL;

	/* The bytes: the auto-exposure mode the device offers, or the value itself. */
	kern_memset(bytes, 0, sizeof(bytes));
	if ((map->flags & UVC_CONTROL_AE_MODE) != 0U) {
		error = usb_uvc_unit_request(uvc, UVC_GET_RES, map, unit_id, bytes, map->size);
		if (error != 0)
			return error;
		bytes[0] = uvc_ae_mode_choose(bytes[0], value);
		if (bytes[0] == 0U)
			return EINVAL;
	} else {
		error = uvc_control_encode(map, value, bytes);
		if (error != 0)
			return error;
	}

	/* SET_CUR. */
	error = usb_uvc_unit_request(uvc, UVC_SET_CUR, map, unit_id, bytes, map->size);
	if (error != 0)
		return error;

	/* Succeeded: the device has the value. */
	return 0;
}

/*
 * Issues one class request to a unit of the VideoControl interface.
 *
 * Value(s) referenced: USB Video Class 1.1 §4.2 (bmRequestType 0x21 for
 * SET and 0xA1 for GET, wValue the control selector in the high byte,
 * wIndex the unit or terminal ID in the high byte and the interface in
 * the low byte).
 */
static int
usb_uvc_unit_request(
	struct usb_uvc *uvc,
	uint8_t request,
	const struct uvc_control_map *map,
	uint8_t unit_id,
	uint8_t *bytes,
	size_t length)
{
	uint8_t request_type;
	uint16_t value;
	uint16_t index;
	size_t actual;
	int error;

	/* GET requests read, SET requests write. */
	request_type = DRV_USB_REQUEST_CLASS | DRV_USB_RECIP_INTERFACE;
	if ((request & DRV_USB_DIR_IN) != 0U)
		request_type |= DRV_USB_DIR_IN;

	/* The selector, the unit and the VideoControl interface. */
	value = (uint16_t)((uint16_t)map->selector << 8);
	index = (uint16_t)(((uint16_t)unit_id << 8) | (uint16_t)drv_usb_interface_number(uvc->control_interface));

	/* The request on the default control pipe. */
	actual = 0U;
	error = drv_usb_control(uvc->device, request_type, request, value, index, bytes, length, UVC_UNIT_CONTROL_TIMEOUT_MS, &actual);
	if (error != 0)
		return error;

	/* Refuses a short answer to a GET, which would read as a wrong value. */
	if ((request & DRV_USB_DIR_IN) != 0U && actual < length)
		return EIO;

	/* Succeeded: bytes holds the answer, or the device has the value. */
	return 0;
}

/* Reads one value of a control (GET_CUR, MIN, MAX, RES or DEF) as the framework sees it. */
static int
usb_uvc_unit_value(
	struct usb_uvc *uvc,
	uint8_t request,
	const struct uvc_control_map *map,
	uint8_t unit_id,
	int64_t *value)
{
	uint8_t bytes[UVC_CONTROL_BYTES_MAX];
	int error;

	/* The field of the control's size. */
	kern_memset(bytes, 0, sizeof(bytes));
	error = usb_uvc_unit_request(uvc, request, map, unit_id, bytes, map->size);
	if (error != 0)
		return error;

	/* The auto-exposure mode reads as a switch; the others as numbers. */
	if ((map->flags & UVC_CONTROL_AE_MODE) != 0U)
		*value = uvc_ae_mode_automatic(bytes[0]);
	else
		*value = uvc_control_decode(map, bytes);

	/* Succeeded: the value. */
	return 0;
}

/* Counts the controls the driver knows that the device declares. */
static unsigned
usb_uvc_control_count(
	const struct usb_uvc *uvc)
{
	const struct uvc_control_map *map;
	uint8_t unit_id;
	unsigned count;
	unsigned index;
	int present;

	/* Every known control the device's units declare. */
	count = 0U;
	for (index = 0U; ; index++) {
		map = uvc_control_at(index);
		if (map == NULL)
			break;
		present = uvc_control_present(&uvc->control, map, &unit_id);
		if (present)
			count++;
	}

	/* The number. */
	return count;
}
