/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The camcap program: captures frames from /dev/camN.
 *
 * It sets a format, maps the buffers, streams a number of frames and
 * writes the last one (or every one) to a file: an MJPEG frame as a JPEG
 * with its Huffman tables completed, a YUYV frame as a PPM.  It tells
 * each frame's sequence, size, flags and time, the frame rate, and the
 * node's counters.  A mode the host cannot carry is told with the
 * payload it needs and the most the host takes.  --probe-only streams
 * without reading frames and tells only the counters.  --controls tells
 * every control's range and value; --control id=value sets one after the
 * format (it may be given up to eight times).
 *
 *   camcap [-d /dev/cam0] [-f mjpeg|yuyv] [-w width] [-h height]
 *          [-i interval_100ns] [-b buffers] [-n frames] [-o path] [-a]
 *          [-t timeout_ms] [-l] [--probe-only ms] [--controls]
 *          [--control id=value]
 */

#include "userland/tests/camcommon/camcommon.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <uapi/camera.h>

/* The defaults of the options. */
#define CAMCAP_DEFAULT_DEVICE		"/dev/cam0"
#define CAMCAP_DEFAULT_WIDTH		640U
#define CAMCAP_DEFAULT_HEIGHT		480U
#define CAMCAP_DEFAULT_BUFFERS		4U
#define CAMCAP_DEFAULT_FRAMES		30U
#define CAMCAP_DEFAULT_TIMEOUT_MS	3000U
#define CAMCAP_DEFAULT_OUTPUT		"camcap"

/* The most --control options one run takes. */
#define CAMCAP_CONTROLS_MAX		8U

/* The control ids --controls asks about: the sensor's and the USB camera's. */
#define CAMCAP_CONTROL_FIRST_SENSOR	1U
#define CAMCAP_CONTROL_LAST_SENSOR	10U
#define CAMCAP_CONTROL_FIRST_USB	32U
#define CAMCAP_CONTROL_LAST_USB		52U

/* The exit statuses: success, a usage error, a mode the host cannot carry, any other failure. */
#define CAMCAP_EXIT_OK			0
#define CAMCAP_EXIT_USAGE		1
#define CAMCAP_EXIT_NO_BANDWIDTH	2
#define CAMCAP_EXIT_FAILED		3

/*
 * What the command line asks for.
 *
 * probe_ms nonzero streams for that long without reading frames.
 */
struct camcap_options {
	const char *device;
	const char *output;
	uint32_t format;
	uint32_t width;
	uint32_t height;
	uint32_t interval_100ns;
	unsigned buffers;
	unsigned frames;
	unsigned timeout_ms;
	unsigned probe_ms;
	int save_all;
	int list;
	int list_controls;
	unsigned control_count;
	uint32_t control_ids[CAMCAP_CONTROLS_MAX];
	int64_t control_values[CAMCAP_CONTROLS_MAX];
};

/*
 * One mapped buffer.
 *
 * address is MAP_FAILED until the buffer is mapped.
 */
struct camcap_buffer {
	void *address;
	size_t bytes;
};

static int camcap_parse(int argc, char **argv, struct camcap_options *options);
static void camcap_usage(void);
static int camcap_set_format(int descriptor, const struct camcap_options *options, const struct cam_info *info, struct cam_format *format);
static int camcap_map(int descriptor, unsigned count, struct camcap_buffer *buffers);
static void camcap_unmap(unsigned count, struct camcap_buffer *buffers);
static int camcap_queue(int descriptor, unsigned index);
static int camcap_capture(int descriptor, const struct camcap_options *options, const struct cam_format *format, struct camcap_buffer *buffers);
static int camcap_save(const struct camcap_options *options, const struct cam_format *format, const uint8_t *frame, uint32_t used, unsigned number);
static void camcap_sleep_ms(unsigned milliseconds);
static void camcap_record(void *record, size_t size);
static int camcap_parse_control(const char *text, struct camcap_options *options);
static void camcap_print_controls(int descriptor);
static int camcap_set_controls(int descriptor, const struct camcap_options *options);

/*
 * Captures frames as the command line asks.
 */
int
main(
	int argc,
	char **argv)
{
	struct camcap_buffer buffers[CAM_BUFFERS_MAX];
	struct cam_request_buffers request;
	struct camcap_options options;
	struct cam_format format;
	struct cam_info info;
	unsigned count;
	int descriptor;
	int status;
	int error;

	/* The options. */
	error = camcap_parse(argc, argv, &options);
	if (error != 0) {
		camcap_usage();
		return CAMCAP_EXIT_USAGE;
	}

	/* The node. */
	descriptor = open(options.device, O_RDWR);
	if (descriptor < 0) {
		fprintf(stderr, "camcap: %s: %s\n", options.device, strerror(errno));
		return CAMCAP_EXIT_FAILED;
	}

	/* What it is. */
	error = camcommon_get_info(descriptor, &info);
	if (error != 0) {
		fprintf(stderr, "camcap: CAM_GET_INFO: %s\n", strerror(error));
		close(descriptor);
		return CAMCAP_EXIT_FAILED;
	}

	/* Tells what the camera is. */
	camcommon_print_info(&info);

	/* --controls tells the controls; with -l too it goes on to the formats. */
	if (options.list_controls) {
		camcap_print_controls(descriptor);
		if (!options.list) {
			close(descriptor);
			return CAMCAP_EXIT_OK;
		}
	}

	/* -l lists the formats and ends there. */
	if (options.list) {
		error = camcommon_print_formats(descriptor, &info);
		close(descriptor);
		if (error != 0)
			return CAMCAP_EXIT_FAILED;
		return CAMCAP_EXIT_OK;
	}

	/* The format; a mode the host cannot carry has its own status. */
	error = camcap_set_format(descriptor, &options, &info, &format);
	if (error != 0) {
		close(descriptor);
		if (error == ENOSPC)
			return CAMCAP_EXIT_NO_BANDWIDTH;
		return CAMCAP_EXIT_FAILED;
	}

	/* The controls asked for, now that this open owns the camera. */
	error = camcap_set_controls(descriptor, &options);
	if (error != 0) {
		close(descriptor);
		return CAMCAP_EXIT_FAILED;
	}

	/* The buffers. */
	camcap_record(&request, sizeof(request));
	request.count = options.buffers;
	request.memory = CAM_MEMORY_DEVICE;
	status = ioctl(descriptor, CAM_REQUEST_BUFFERS, &request);
	if (status < 0 || request.count == 0U) {
		fprintf(stderr, "camcap: CAM_REQUEST_BUFFERS: %s\n", strerror(errno));
		close(descriptor);
		return CAMCAP_EXIT_FAILED;
	}

	/* Tells how many buffers the framework gave and how big they are. */
	count = request.count;
	printf("buffers: %u of %u bytes\n", count, (unsigned)request.bytes);

	/* Each buffer mapped. */
	error = camcap_map(descriptor, count, buffers);
	if (error != 0) {
		camcap_unmap(count, buffers);
		close(descriptor);
		return CAMCAP_EXIT_FAILED;
	}

	/* The frames. */
	error = camcap_capture(descriptor, &options, &format, buffers);

	/* The counters, then everything given back. */
	(void)camcommon_print_counters(descriptor);
	camcap_unmap(count, buffers);
	close(descriptor);

	/* Reports a capture that failed. */
	if (error != 0)
		return CAMCAP_EXIT_FAILED;

	/* Succeeded: the frames are captured. */
	return CAMCAP_EXIT_OK;
}

/* Reads the command line; EINVAL for anything it does not understand. */
static int
camcap_parse(
	int argc,
	char **argv,
	struct camcap_options *options)
{
	const char *value;
	const char *option;
	int index;
	int order;
	int error;

	/* The defaults. */
	memset(options, 0, sizeof(*options));
	options->device = CAMCAP_DEFAULT_DEVICE;
	options->output = CAMCAP_DEFAULT_OUTPUT;
	options->format = CAM_FORMAT_MJPEG;
	options->width = CAMCAP_DEFAULT_WIDTH;
	options->height = CAMCAP_DEFAULT_HEIGHT;
	options->buffers = CAMCAP_DEFAULT_BUFFERS;
	options->frames = CAMCAP_DEFAULT_FRAMES;
	options->timeout_ms = CAMCAP_DEFAULT_TIMEOUT_MS;

	/* Each option; those with a value take the next argument. */
	for (index = 1; index < argc; index++) {
		option = argv[index];

		/* Takes the flag that saves every frame. */
		order = strcmp(option, "-a");
		if (order == 0) {
			options->save_all = 1;
			continue;
		}

		/* Takes the flag that lists the camera's modes. */
		order = strcmp(option, "-l");
		if (order == 0) {
			options->list = 1;
			continue;
		}

		/* Takes the flag that lists the camera's controls. */
		order = strcmp(option, "--controls");
		if (order == 0) {
			options->list_controls = 1;
			continue;
		}

		/* Every other option has a value. */
		if (index + 1 >= argc)
			return EINVAL;
		index++;
		value = argv[index];

		/* Takes the device path. */
		order = strcmp(option, "-d");
		if (order == 0) {
			options->device = value;
			continue;
		}

		/* Takes the output path. */
		order = strcmp(option, "-o");
		if (order == 0) {
			options->output = value;
			continue;
		}

		/* Takes the pixel format, which is one of two names. */
		order = strcmp(option, "-f");
		if (order == 0) {
			order = strcmp(value, "mjpeg");
			if (order == 0) {
				options->format = CAM_FORMAT_MJPEG;
				continue;
			}

			/* The other name the tool takes. */
			order = strcmp(value, "yuyv");
			if (order == 0) {
				options->format = CAM_FORMAT_YUYV;
				continue;
			}

			/* Refuses a format name the tool does not know. */
			return EINVAL;
		}

		/* Takes the width. */
		order = strcmp(option, "-w");
		if (order == 0) {
			options->width = (uint32_t)strtoul(value, NULL, 0);
			continue;
		}

		/* Takes the height. */
		order = strcmp(option, "-h");
		if (order == 0) {
			options->height = (uint32_t)strtoul(value, NULL, 0);
			continue;
		}

		/* Takes the frame interval. */
		order = strcmp(option, "-i");
		if (order == 0) {
			options->interval_100ns = (uint32_t)strtoul(value, NULL, 0);
			continue;
		}

		/* Takes the buffer count. */
		order = strcmp(option, "-b");
		if (order == 0) {
			options->buffers = (unsigned)strtoul(value, NULL, 0);
			continue;
		}

		/* Takes the frame count. */
		order = strcmp(option, "-n");
		if (order == 0) {
			options->frames = (unsigned)strtoul(value, NULL, 0);
			continue;
		}

		/* Takes the wait for one frame. */
		order = strcmp(option, "-t");
		if (order == 0) {
			options->timeout_ms = (unsigned)strtoul(value, NULL, 0);
			continue;
		}

		/* Takes the time to leave the stream running without saving. */
		order = strcmp(option, "--probe-only");
		if (order == 0) {
			options->probe_ms = (unsigned)strtoul(value, NULL, 0);
			continue;
		}

		/* Takes a control to set, written id=value. */
		order = strcmp(option, "--control");
		if (order == 0) {
			error = camcap_parse_control(value, options);
			if (error != 0)
				return EINVAL;

			continue;
		}

		/* Refuses an option the tool does not know. */
		return EINVAL;
	}

	/* The buffer count the framework takes. */
	if (options->buffers == 0U || options->buffers > CAM_BUFFERS_MAX)
		return EINVAL;

	/* Succeeded: the options are read. */
	return 0;
}

/* Tells how to call the program. */
static void
camcap_usage(
	void)
{
	fprintf(stderr,
	    "usage: camcap [-d /dev/cam0] [-f mjpeg|yuyv] [-w width] [-h height]\n"
	    "              [-i interval_100ns] [-b buffers] [-n frames] [-o path] [-a]\n"
	    "              [-t timeout_ms] [-l] [--probe-only ms] [--controls]\n"
	    "              [--control id=value]\n");
}

/*
 * Sets the format and tells the answer.  ENOSPC is told with the payload
 * the mode needs and the most the host carries.
 */
static int
camcap_set_format(
	int descriptor,
	const struct camcap_options *options,
	const struct cam_info *info,
	struct cam_format *format)
{
	int status;
	int error;

	/* The format asked for. */
	camcap_record(format, sizeof(*format));
	format->format = options->format;
	format->width = options->width;
	format->height = options->height;
	format->interval_100ns = options->interval_100ns;

	/* SET_FORMAT. */
	status = ioctl(descriptor, CAM_SET_FORMAT, format);
	if (status < 0) {
		error = errno;
		if (error == ENOSPC) {
			fprintf(stderr, "camcap: the host cannot carry this mode: payload %u, host takes up to %u\n",
			    (unsigned)format->payload_bytes, (unsigned)info->host_payload_max);
		} else {
			fprintf(stderr, "camcap: CAM_SET_FORMAT: %s\n", strerror(error));
		}

		/* Reports why the camera refused the format. */
		return error;
	}

	/* The answer. */
	printf("format: %s %ux%u interval %u stride %u image %u payload %u\n", camcommon_format_name(format->format),
	    (unsigned)format->width, (unsigned)format->height, (unsigned)format->interval_100ns,
	    (unsigned)format->stride_bytes, (unsigned)format->image_bytes, (unsigned)format->payload_bytes);

	/* Succeeded: the format is set. */
	return 0;
}

/* Maps every buffer for reading. */
static int
camcap_map(
	int descriptor,
	unsigned count,
	struct camcap_buffer *buffers)
{
	struct cam_buffer buffer;
	unsigned index;
	int status;

	/* None mapped yet. */
	for (index = 0U; index < count; index++)
		buffers[index].address = MAP_FAILED;

	/* Each buffer's place, then its mapping. */
	for (index = 0U; index < count; index++) {
		camcap_record(&buffer, sizeof(buffer));
		buffer.index = index;
		status = ioctl(descriptor, CAM_QUERY_BUFFER, &buffer);
		if (status < 0) {
			fprintf(stderr, "camcap: CAM_QUERY_BUFFER %u: %s\n", index, strerror(errno));
			return errno;
		}

		/* The buffer, read only. */
		buffers[index].bytes = buffer.bytes;
		buffers[index].address = mmap(NULL, buffer.bytes, PROT_READ, MAP_SHARED, descriptor, (off_t)buffer.mmap_offset);
		if (buffers[index].address == MAP_FAILED) {
			fprintf(stderr, "camcap: mmap of buffer %u: %s\n", index, strerror(errno));
			return errno;
		}
	}

	/* Succeeded: every buffer is mapped. */
	return 0;
}

/* Unmaps the buffers that are mapped. */
static void
camcap_unmap(
	unsigned count,
	struct camcap_buffer *buffers)
{
	unsigned index;

	/* Each mapped buffer. */
	for (index = 0U; index < count; index++) {
		if (buffers[index].address != MAP_FAILED)
			(void)munmap(buffers[index].address, buffers[index].bytes);
		buffers[index].address = MAP_FAILED;
	}
}

/* Gives a buffer to the stream. */
static int
camcap_queue(
	int descriptor,
	unsigned index)
{
	struct cam_buffer buffer;
	int status;

	/* QUEUE_BUFFER of the index. */
	camcap_record(&buffer, sizeof(buffer));
	buffer.index = index;
	buffer.memory = CAM_MEMORY_DEVICE;
	buffer.fd = -1;
	status = ioctl(descriptor, CAM_QUEUE_BUFFER, &buffer);
	if (status < 0) {
		fprintf(stderr, "camcap: CAM_QUEUE_BUFFER %u: %s\n", index, strerror(errno));
		return errno;
	}

	/* Succeeded: the buffer waits for a frame. */
	return 0;
}

/*
 * Streams: every buffer queued, then the frames read and queued again,
 * or with --probe-only a wait, then the stream stopped.
 */
static int
camcap_capture(
	int descriptor,
	const struct camcap_options *options,
	const struct cam_format *format,
	struct camcap_buffer *buffers)
{
	struct cam_buffer buffer;
	uint64_t first_ns;
	uint64_t last_ns;
	unsigned buffer_count;
	unsigned index;
	unsigned taken;
	int status;
	int error;

	/* Every buffer is given to the stream. */
	buffer_count = options->buffers;
	for (index = 0U; index < buffer_count; index++) {
		if (buffers[index].address == MAP_FAILED)
			break;
		error = camcap_queue(descriptor, index);
		if (error != 0)
			return error;
	}

	/* The stream starts. */
	status = ioctl(descriptor, CAM_STREAM_ON);
	if (status < 0) {
		error = errno;
		fprintf(stderr, "camcap: CAM_STREAM_ON: %s\n", strerror(error));
		return error;
	}

	/* --probe-only: the stream runs for the time given, and only the counters are told. */
	if (options->probe_ms != 0U) {
		camcap_sleep_ms(options->probe_ms);
		(void)ioctl(descriptor, CAM_STREAM_OFF);
		return 0;
	}

	/* Each frame: told, saved when asked, and its buffer queued again. */
	error = 0;
	first_ns = 0U;
	last_ns = 0U;
	for (taken = 0U; taken < options->frames; taken++) {
		camcap_record(&buffer, sizeof(buffer));
		buffer.memory = CAM_MEMORY_DEVICE;
		buffer.timeout_ms = options->timeout_ms;
		status = ioctl(descriptor, CAM_DEQUEUE_BUFFER, &buffer);
		if (status < 0) {
			error = errno;
			fprintf(stderr, "camcap: CAM_DEQUEUE_BUFFER: %s\n", strerror(error));
			break;
		}

		/* The frame's record. */
		printf("frame %u: sequence %u bytes %u flags 0x%x time %llu\n", taken, (unsigned)buffer.metadata.sequence,
		    (unsigned)buffer.metadata.used_bytes, (unsigned)buffer.metadata.flags,
		    (unsigned long long)buffer.metadata.timestamp_ns);
		if (taken == 0U)
			first_ns = buffer.metadata.timestamp_ns;
		last_ns = buffer.metadata.timestamp_ns;

		/* The frame saved: every one with -a, else the last. */
		if (options->save_all || taken + 1U == options->frames) {
			error = camcap_save(options, format, buffers[buffer.index].address, buffer.metadata.used_bytes, taken);
			if (error != 0)
				break;
		}

		/* The buffer back to the stream. */
		error = camcap_queue(descriptor, buffer.index);
		if (error != 0)
			break;
	}

	/* The rate over the frames read. */
	if (taken > 1U && last_ns > first_ns) {
		printf("rate: %.2f frames a second over %u frames\n",
		    (double)(taken - 1U) * 1e9 / (double)(last_ns - first_ns), taken);
	}

	/* The stream stops. */
	(void)ioctl(descriptor, CAM_STREAM_OFF);

	/* Reports a capture cut short. */
	if (error != 0)
		return error;

	/* Succeeded: every frame was read. */
	return 0;
}

/*
 * Writes one frame: MJPEG as <output>[-n].jpg with its Huffman tables
 * completed, YUYV as <output>[-n].ppm in BT.601 limited range.
 */
static int
camcap_save(
	const struct camcap_options *options,
	const struct cam_format *format,
	const uint8_t *frame,
	uint32_t used,
	unsigned number)
{
	struct camcommon_yuv_coefficients coefficients;
	char path[512];
	uint8_t *output;
	size_t capacity;
	size_t length;
	int inserted;
	const char *added;
	int error;

	/* An MJPEG frame: the DHT added when the device left it out. */
	if (format->format == CAM_FORMAT_MJPEG) {
		if (options->save_all)
			snprintf(path, sizeof(path), "%s-%u.jpg", options->output, number);
		else
			snprintf(path, sizeof(path), "%s.jpg", options->output);
		capacity = (size_t)used + camcommon_mjpeg_dht_size();
		output = malloc(capacity);
		if (output == NULL)
			return ENOMEM;
		error = camcommon_mjpeg_complete(frame, used, output, capacity, &length, &inserted);
		if (error == 0)
			error = camcommon_write_file(path, output, length);
		free(output);
		if (error != 0) {
			fprintf(stderr, "camcap: %s: %s\n", path, strerror(error));
			return error;
		}

		/* Tells where the file went and whether the table was added. */
		added = "";
		if (inserted)
			added = ", DHT added";
		printf("saved %s (%u bytes%s)\n", path, (unsigned)length, added);

		/* Succeeded: the frame is saved. */
		return 0;
	}

	/* A YUYV frame, as RGB. */
	if (options->save_all)
		snprintf(path, sizeof(path), "%s-%u.ppm", options->output, number);
	else
		snprintf(path, sizeof(path), "%s.ppm", options->output);
	capacity = (size_t)format->width * format->height * 3U;
	output = malloc(capacity);
	if (output == NULL)
		return ENOMEM;
	error = camcommon_yuv_coefficients_init(&coefficients, CAMCOMMON_MATRIX_BT601, CAMCOMMON_RANGE_LIMITED);
	if (error == 0)
		error = camcommon_yuyv_to_rgb(frame, format->width, format->height, format->stride_bytes, &coefficients, output);
	if (error == 0)
		error = camcommon_write_ppm(path, output, format->width, format->height);
	free(output);
	if (error != 0) {
		fprintf(stderr, "camcap: %s: %s\n", path, strerror(error));
		return error;
	}

	/* Succeeded: the frame is written. */
	printf("saved %s\n", path);
	return 0;
}

/* Sleeps for about the milliseconds given. */
static void
camcap_sleep_ms(
	unsigned milliseconds)
{
	struct timespec delay;

	/* The delay in seconds and nanoseconds. */
	delay.tv_sec = (time_t)(milliseconds / 1000U);
	delay.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
	(void)nanosleep(&delay, NULL);
}

/* Clears a record and stamps its version and size. */
static void
camcap_record(
	void *record,
	size_t size)
{
	uint32_t *words;

	/* Zeros, then the two leading words. */
	memset(record, 0, size);
	words = record;
	words[0] = CAM_ABI_VERSION;
	words[1] = (uint32_t)size;
}

/* Reads one --control id=value into the options; EINVAL for a bad one or too many. */
static int
camcap_parse_control(
	const char *text,
	struct camcap_options *options)
{
	const char *equals;
	char *end;
	unsigned long id;
	long long value;

	/* Room for one more. */
	if (options->control_count >= CAMCAP_CONTROLS_MAX)
		return EINVAL;

	/* The id before the '=' and the value after it, both whole numbers. */
	equals = strchr(text, '=');
	if (equals == NULL ||
	    equals == text ||
	    equals[1] == '\0')
		return EINVAL;
	id = strtoul(text, &end, 0);
	if (end != equals)
		return EINVAL;
	value = strtoll(equals + 1, &end, 0);
	if (*end != '\0')
		return EINVAL;

	/* Succeeded: the control is kept for after the format. */
	options->control_ids[options->control_count] = (uint32_t)id;
	options->control_values[options->control_count] = (int64_t)value;
	options->control_count++;
	return 0;
}

/* Tells every control the camera has: its name, range, step, default and value. */
static void
camcap_print_controls(
	int descriptor)
{
	struct cam_control_info info;
	struct cam_control control;
	const char *switch_text;
	const char *read_only_text;
	uint32_t id;
	int status;

	/* The sensor's ids, then the USB camera's. */
	for (id = CAMCAP_CONTROL_FIRST_SENSOR; id <= CAMCAP_CONTROL_LAST_USB; id++) {
		if (id > CAMCAP_CONTROL_LAST_SENSOR && id < CAMCAP_CONTROL_FIRST_USB)
			continue;

		/* The range; a control the camera does not have is not told. */
		camcap_record(&info, sizeof(info));
		info.id = id;
		status = ioctl(descriptor, CAM_GET_CONTROL_INFO, &info);
		if (status < 0) {
			printf("control %u: CAM_GET_CONTROL_INFO: %s\n", (unsigned)id, strerror(errno));
			continue;
		}

		/* A control the camera does not have is not told. */
		if ((info.flags & CAM_CONTROL_UNSUPPORTED) != 0U)
			continue;

		/* The words for the control's kind. */
		switch_text = "";
		if ((info.flags & CAM_CONTROL_BOOLEAN) != 0U)
			switch_text = " switch";
		read_only_text = "";
		if ((info.flags & CAM_CONTROL_READ_ONLY) != 0U)
			read_only_text = " read-only";

		/* The value now. */
		camcap_record(&control, sizeof(control));
		control.id = id;
		status = ioctl(descriptor, CAM_GET_CONTROL, &control);
		if (status < 0) {
			printf("control %u %s: %lld..%lld step %lld default %lld%s%s, value: %s\n", (unsigned)id, info.name,
			    (long long)info.minimum, (long long)info.maximum, (long long)info.step, (long long)info.default_value,
			    switch_text, read_only_text, strerror(errno));
			continue;
		}

		/* Tells the control with its range and its value. */
		printf("control %u %s: %lld..%lld step %lld default %lld%s%s, value %lld\n", (unsigned)id, info.name,
		    (long long)info.minimum, (long long)info.maximum, (long long)info.step, (long long)info.default_value,
		    switch_text, read_only_text, (long long)control.value);
	}
}

/* Sets the controls the command line asked for and reads each back. */
static int
camcap_set_controls(
	int descriptor,
	const struct camcap_options *options)
{
	struct cam_control control;
	unsigned index;
	int status;
	int error;

	/* Each one in the order given. */
	for (index = 0U; index < options->control_count; index++) {
		camcap_record(&control, sizeof(control));
		control.id = options->control_ids[index];
		control.value = options->control_values[index];
		status = ioctl(descriptor, CAM_SET_CONTROL, &control);
		if (status < 0) {
			error = errno;
			fprintf(stderr, "camcap: control %u = %lld: %s\n", (unsigned)control.id, (long long)control.value,
			    strerror(error));
			return error;
		}

		/* What the device took. */
		camcap_record(&control, sizeof(control));
		control.id = options->control_ids[index];
		status = ioctl(descriptor, CAM_GET_CONTROL, &control);
		if (status == 0)
			printf("control %u set, now %lld\n", (unsigned)control.id, (long long)control.value);
	}

	/* Succeeded: every control asked for is set. */
	return 0;
}
