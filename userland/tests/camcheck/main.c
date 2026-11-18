/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The camcheck program: judges the frames of /dev/camN against the test
 * pattern a PC shows through an HDMI capture device.
 *
 * It captures N frames and, for each, reads the colour patches and the
 * frame counter.  A YUYV stream's matrix and range are guessed from the
 * first frame and told; MJPEG frames are decoded with libjpeg-compat.
 * The patches must lie within the tolerance (8 of 255 for YUYV, 16 for
 * MJPEG), the counter must never go down (its steps are told, 2 and more
 * being normal), no frame may carry the ERROR flag and the sequence may
 * not skip (a skip is a dropped frame).  --cycles stops and starts the
 * stream that many times, a frame each, and compares the kernel's
 * allocation counters before and after.  --second-open tells whether a
 * second open is kept off the owner's stream.  The first frame is
 * written as a PPM.
 *
 *   camcheck [-d /dev/cam0] [-f mjpeg|yuyv] [-w width] [-h height]
 *            [-i interval_100ns] [-b buffers] [-n frames] [-t timeout_ms]
 *            [-o path.ppm] [--tolerance n] [--cycles n] [--second-open]
 *
 * The last line is "camcheck: PASS" or "camcheck: FAIL (...)".
 */

#include "userland/tests/camcheck/analyze.h"
#include "userland/tests/camcommon/camcommon.h"

#include <compat/jpeglib.h>
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <uapi/camera.h>
#include <uapi/io-stats.h>

/* The defaults of the options. */
#define CAMCHECK_DEFAULT_DEVICE		"/dev/cam0"
#define CAMCHECK_DEFAULT_WIDTH		640U
#define CAMCHECK_DEFAULT_HEIGHT		480U
#define CAMCHECK_DEFAULT_BUFFERS	4U
#define CAMCHECK_DEFAULT_FRAMES		100U
#define CAMCHECK_DEFAULT_TIMEOUT_MS	3000U
#define CAMCHECK_DEFAULT_OUTPUT		"camcheck.ppm"

/* The patches' tolerance, of 255: YUYV, and MJPEG (its compression moves colours further). */
#define CAMCHECK_TOLERANCE_YUYV		8.0
#define CAMCHECK_TOLERANCE_MJPEG	16.0

/* The exit statuses: PASS, a usage error, a mode the host cannot carry, a failure to run, FAIL. */
#define CAMCHECK_EXIT_PASS		0
#define CAMCHECK_EXIT_USAGE		1
#define CAMCHECK_EXIT_NO_BANDWIDTH	2
#define CAMCHECK_EXIT_FAILED		3
#define CAMCHECK_EXIT_FAIL		4

/*
 * What the command line asks for.
 *
 * tolerance 0 takes the format's default.
 */
struct camcheck_options {
	const char *device;
	const char *output;
	uint32_t format;
	uint32_t width;
	uint32_t height;
	uint32_t interval_100ns;
	unsigned buffers;
	unsigned frames;
	unsigned timeout_ms;
	unsigned cycles;
	double tolerance;
	int second_open;
};

/*
 * One mapped buffer.
 *
 * address is MAP_FAILED until the buffer is mapped.
 */
struct camcheck_buffer {
	void *address;
	size_t bytes;
};

/*
 * What the frames showed: the counts the verdict is made of, the guessed
 * matrix and range of a YUYV stream, the largest patch error and its
 * patch, and the counter's run.
 */
struct camcheck_result {
	unsigned frames;
	unsigned error_frames;
	unsigned incomplete_frames;
	unsigned decode_failures;
	unsigned patch_failures;
	uint64_t sequence_gaps;
	uint32_t matrix;
	uint32_t range;
	double worst_error;
	unsigned worst_patch;
	struct camcheck_counter_run counter;
};

/*
 * The error manager of one JPEG decoding: libjpeg's own, and where its
 * fatal errors jump back to.
 */
struct camcheck_jpeg_error {
	struct jpeg_error_mgr manager;
	jmp_buf back;
};

/* The allocation counters compared around --cycles, as alloc and free pairs. */
static const unsigned camcheck_leak_pairs[][2] = {
	{ IO_USB_TRANSFER_RESERVE_ALLOC, IO_USB_TRANSFER_RESERVE_FREE },
	{ IO_USB_BUFFER_ALLOC, IO_USB_BUFFER_FREE },
	{ IO_DMA_ALLOC, IO_DMA_FREE }
};

/* The names of the patches, for the report. */
static const char *const camcheck_patch_names[CAMCHECK_PATCHES] = {
	"red", "green", "blue", "white", "black", "grey", "yellow", "cyan", "magenta"
};

static int camcheck_parse(int argc, char **argv, struct camcheck_options *options);
static void camcheck_usage(void);
static int camcheck_set_format(int descriptor, const struct camcheck_options *options, const struct cam_info *info, struct cam_format *format);
static int camcheck_map(int descriptor, unsigned count, struct camcheck_buffer *buffers);
static void camcheck_unmap(unsigned count, struct camcheck_buffer *buffers);
static int camcheck_queue(int descriptor, unsigned index);
static int camcheck_queue_all(int descriptor, unsigned count);
static int camcheck_dequeue(int descriptor, unsigned timeout_ms, struct cam_buffer *buffer);
static int camcheck_second_open(const struct camcheck_options *options);
static int camcheck_stream(int descriptor, const struct camcheck_options *options, const struct cam_format *format, unsigned count, struct camcheck_buffer *buffers, struct camcheck_result *result);
static int camcheck_frame(const struct camcheck_options *options, const struct cam_format *format, const uint8_t *frame, uint32_t used, unsigned number, struct camcheck_result *result);
static int camcheck_jpeg_decode(const uint8_t *frame, uint32_t used, uint32_t width, uint32_t height, uint8_t *rgb);
static void camcheck_jpeg_exit(j_common_ptr info);
static void camcheck_jpeg_quiet(j_common_ptr info, int level);
static int camcheck_cycles(int descriptor, const struct camcheck_options *options, unsigned count, int *balanced);
static int camcheck_io_stats(struct io_stats *stats);
static void camcheck_report(const struct camcheck_options *options, const struct camcheck_result *result);
static void camcheck_record(void *record, size_t size);

/*
 * Checks the frames as the command line asks.
 */
int
main(
	int argc,
	char **argv)
{
	struct camcheck_buffer buffers[CAM_BUFFERS_MAX];
	struct cam_request_buffers request;
	struct camcheck_options options;
	struct camcheck_result result;
	struct cam_format format;
	struct cam_info info;
	char reasons[256];
	unsigned count;
	int descriptor;
	int balanced;
	int status;
	int error;

	/* The options. */
	error = camcheck_parse(argc, argv, &options);
	if (error != 0) {
		camcheck_usage();
		return CAMCHECK_EXIT_USAGE;
	}

	/* The node and what it is. */
	descriptor = open(options.device, O_RDWR);
	if (descriptor < 0) {
		fprintf(stderr, "camcheck: %s: %s\n", options.device, strerror(errno));
		return CAMCHECK_EXIT_FAILED;
	}

	/* Asks what the camera is. */
	error = camcommon_get_info(descriptor, &info);
	if (error != 0) {
		fprintf(stderr, "camcheck: CAM_GET_INFO: %s\n", strerror(error));
		close(descriptor);
		return CAMCHECK_EXIT_FAILED;
	}

	/* Tells what the camera is. */
	camcommon_print_info(&info);

	/* The format; a mode the host cannot carry has its own status. */
	error = camcheck_set_format(descriptor, &options, &info, &format);
	if (error != 0) {
		close(descriptor);
		if (error == ENOSPC)
			return CAMCHECK_EXIT_NO_BANDWIDTH;
		return CAMCHECK_EXIT_FAILED;
	}

	/* The buffers, mapped. */
	camcheck_record(&request, sizeof(request));
	request.count = options.buffers;
	request.memory = CAM_MEMORY_DEVICE;
	status = ioctl(descriptor, CAM_REQUEST_BUFFERS, &request);
	if (status < 0 || request.count == 0U) {
		fprintf(stderr, "camcheck: CAM_REQUEST_BUFFERS: %s\n", strerror(errno));
		close(descriptor);
		return CAMCHECK_EXIT_FAILED;
	}

	/* Maps the buffers the framework gave. */
	count = request.count;
	error = camcheck_map(descriptor, count, buffers);
	if (error != 0) {
		camcheck_unmap(count, buffers);
		close(descriptor);
		return CAMCHECK_EXIT_FAILED;
	}

	/* The verdict starts empty. */
	memset(&result, 0, sizeof(result));
	reasons[0] = '\0';

	/* A second open, while this one owns the buffers. */
	if (options.second_open) {
		error = camcheck_second_open(&options);
		if (error != 0)
			strncat(reasons, " second-open", sizeof(reasons) - strlen(reasons) - 1U);
	}

	/* The frames. */
	error = camcheck_stream(descriptor, &options, &format, count, buffers, &result);
	if (error != 0) {
		(void)camcommon_print_counters(descriptor);
		camcheck_unmap(count, buffers);
		close(descriptor);
		return CAMCHECK_EXIT_FAILED;
	}

	/* Tells what the frames showed. */
	camcheck_report(&options, &result);

	/* The stops and starts. */
	balanced = 1;
	if (options.cycles != 0U) {
		error = camcheck_cycles(descriptor, &options, count, &balanced);
		if (error != 0)
			strncat(reasons, " cycles", sizeof(reasons) - strlen(reasons) - 1U);
		if (!balanced)
			strncat(reasons, " leak", sizeof(reasons) - strlen(reasons) - 1U);
	}

	/* The counters, then everything given back. */
	(void)camcommon_print_counters(descriptor);
	camcheck_unmap(count, buffers);
	close(descriptor);

	/* The verdict's other reasons. */
	if (result.frames != options.frames)
		strncat(reasons, " frames", sizeof(reasons) - strlen(reasons) - 1U);
	if (result.error_frames != 0U)
		strncat(reasons, " error-flag", sizeof(reasons) - strlen(reasons) - 1U);
	if (result.sequence_gaps != 0U)
		strncat(reasons, " dropped", sizeof(reasons) - strlen(reasons) - 1U);
	if (result.decode_failures != 0U)
		strncat(reasons, " decode", sizeof(reasons) - strlen(reasons) - 1U);
	if (result.patch_failures != 0U)
		strncat(reasons, " colour", sizeof(reasons) - strlen(reasons) - 1U);
	if (result.counter.decreases != 0U)
		strncat(reasons, " counter", sizeof(reasons) - strlen(reasons) - 1U);

	/* Reports a FAIL with its reasons. */
	if (reasons[0] != '\0') {
		printf("camcheck: FAIL (%s)\n", reasons + 1);
		return CAMCHECK_EXIT_FAIL;
	}

	/* Succeeded: every check passed. */
	printf("camcheck: PASS\n");
	return CAMCHECK_EXIT_PASS;
}

/* Reads the command line; EINVAL for anything it does not understand. */
static int
camcheck_parse(
	int argc,
	char **argv,
	struct camcheck_options *options)
{
	const char *value;
	const char *option;
	int index;
	int order;

	/* The defaults. */
	memset(options, 0, sizeof(*options));
	options->device = CAMCHECK_DEFAULT_DEVICE;
	options->output = CAMCHECK_DEFAULT_OUTPUT;
	options->format = CAM_FORMAT_YUYV;
	options->width = CAMCHECK_DEFAULT_WIDTH;
	options->height = CAMCHECK_DEFAULT_HEIGHT;
	options->buffers = CAMCHECK_DEFAULT_BUFFERS;
	options->frames = CAMCHECK_DEFAULT_FRAMES;
	options->timeout_ms = CAMCHECK_DEFAULT_TIMEOUT_MS;

	/* Each option; those with a value take the next argument. */
	for (index = 1; index < argc; index++) {
		option = argv[index];

		/* Takes the flag that runs the check of a second open. */
		order = strcmp(option, "--second-open");
		if (order == 0) {
			options->second_open = 1;
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

		/* Takes how far a patch may differ from its expected color. */
		order = strcmp(option, "--tolerance");
		if (order == 0) {
			options->tolerance = strtod(value, NULL);
			continue;
		}

		/* Takes how many times the stream stops and starts again. */
		order = strcmp(option, "--cycles");
		if (order == 0) {
			options->cycles = (unsigned)strtoul(value, NULL, 0);
			continue;
		}

		/* Refuses an option the tool does not know. */
		return EINVAL;
	}

	/* The counts the framework takes, and at least one frame. */
	if (options->buffers < 2U ||
	    options->buffers > CAM_BUFFERS_MAX ||
	    options->frames == 0U)
		return EINVAL;

	/* The format's tolerance unless one was given. */
	if (options->tolerance <= 0.0) {
		options->tolerance = CAMCHECK_TOLERANCE_YUYV;
		if (options->format == CAM_FORMAT_MJPEG)
			options->tolerance = CAMCHECK_TOLERANCE_MJPEG;
	}

	/* Succeeded: the options are read. */
	return 0;
}

/* Tells how to call the program. */
static void
camcheck_usage(
	void)
{
	fprintf(stderr,
	    "usage: camcheck [-d /dev/cam0] [-f mjpeg|yuyv] [-w width] [-h height]\n"
	    "                [-i interval_100ns] [-b buffers] [-n frames] [-t timeout_ms]\n"
	    "                [-o path.ppm] [--tolerance n] [--cycles n] [--second-open]\n");
}

/* Sets the format and tells the answer; ENOSPC is told with the payload and the host's most. */
static int
camcheck_set_format(
	int descriptor,
	const struct camcheck_options *options,
	const struct cam_info *info,
	struct cam_format *format)
{
	int status;
	int error;

	/* The format asked for. */
	camcheck_record(format, sizeof(*format));
	format->format = options->format;
	format->width = options->width;
	format->height = options->height;
	format->interval_100ns = options->interval_100ns;

	/* SET_FORMAT. */
	status = ioctl(descriptor, CAM_SET_FORMAT, format);
	if (status < 0) {
		error = errno;
		if (error == ENOSPC) {
			fprintf(stderr, "camcheck: the host cannot carry this mode: payload %u, host takes up to %u\n",
			    (unsigned)format->payload_bytes, (unsigned)info->host_payload_max);
		} else {
			fprintf(stderr, "camcheck: CAM_SET_FORMAT: %s\n", strerror(error));
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
camcheck_map(
	int descriptor,
	unsigned count,
	struct camcheck_buffer *buffers)
{
	struct cam_buffer buffer;
	unsigned index;
	int status;

	/* None mapped yet. */
	for (index = 0U; index < count; index++)
		buffers[index].address = MAP_FAILED;

	/* Each buffer's place, then its mapping. */
	for (index = 0U; index < count; index++) {
		camcheck_record(&buffer, sizeof(buffer));
		buffer.index = index;
		status = ioctl(descriptor, CAM_QUERY_BUFFER, &buffer);
		if (status < 0) {
			fprintf(stderr, "camcheck: CAM_QUERY_BUFFER %u: %s\n", index, strerror(errno));
			return errno;
		}

		/* The buffer, read only. */
		buffers[index].bytes = buffer.bytes;
		buffers[index].address = mmap(NULL, buffer.bytes, PROT_READ, MAP_SHARED, descriptor, (off_t)buffer.mmap_offset);
		if (buffers[index].address == MAP_FAILED) {
			fprintf(stderr, "camcheck: mmap of buffer %u: %s\n", index, strerror(errno));
			return errno;
		}
	}

	/* Succeeded: every buffer is mapped. */
	return 0;
}

/* Unmaps the buffers that are mapped. */
static void
camcheck_unmap(
	unsigned count,
	struct camcheck_buffer *buffers)
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
camcheck_queue(
	int descriptor,
	unsigned index)
{
	struct cam_buffer buffer;
	int status;

	/* QUEUE_BUFFER of the index. */
	camcheck_record(&buffer, sizeof(buffer));
	buffer.index = index;
	buffer.memory = CAM_MEMORY_DEVICE;
	buffer.fd = -1;
	status = ioctl(descriptor, CAM_QUEUE_BUFFER, &buffer);
	if (status < 0) {
		fprintf(stderr, "camcheck: CAM_QUEUE_BUFFER %u: %s\n", index, strerror(errno));
		return errno;
	}

	/* Succeeded: the buffer waits for a frame. */
	return 0;
}

/* Gives every buffer to the stream. */
static int
camcheck_queue_all(
	int descriptor,
	unsigned count)
{
	unsigned index;
	int error;

	/* Each buffer in turn. */
	for (index = 0U; index < count; index++) {
		error = camcheck_queue(descriptor, index);
		if (error != 0)
			return error;
	}

	/* Succeeded: every buffer waits. */
	return 0;
}

/* Takes the next finished frame, waiting at most timeout_ms. */
static int
camcheck_dequeue(
	int descriptor,
	unsigned timeout_ms,
	struct cam_buffer *buffer)
{
	int status;

	/* DEQUEUE_BUFFER with the wait's limit. */
	camcheck_record(buffer, sizeof(*buffer));
	buffer->memory = CAM_MEMORY_DEVICE;
	buffer->timeout_ms = timeout_ms;
	status = ioctl(descriptor, CAM_DEQUEUE_BUFFER, buffer);
	if (status < 0) {
		fprintf(stderr, "camcheck: CAM_DEQUEUE_BUFFER: %s\n", strerror(errno));
		return errno;
	}

	/* Succeeded: the buffer holds a frame. */
	return 0;
}

/*
 * Opens the node a second time while the first owns its buffers: the
 * second may ask what the camera is, but TRY_FORMAT, SET_FORMAT and
 * REQUEST_BUFFERS must be refused with EBUSY.  EPERM when one is not.
 */
static int
camcheck_second_open(
	const struct camcheck_options *options)
{
	struct cam_request_buffers request;
	struct cam_format format;
	struct cam_info info;
	const char *answer;
	int descriptor;
	int refused;
	int status;
	int error;

	/* The second open, and what it may still ask. */
	descriptor = open(options->device, O_RDWR);
	if (descriptor < 0) {
		fprintf(stderr, "camcheck: second open: %s\n", strerror(errno));
		return errno;
	}

	/* A second open may still ask what the camera is. */
	error = camcommon_get_info(descriptor, &info);
	if (error != 0)
		fprintf(stderr, "camcheck: second open: CAM_GET_INFO: %s\n", strerror(error));

	/* TRY_FORMAT while the owner holds buffers. */
	refused = 1;
	camcheck_record(&format, sizeof(format));
	format.format = options->format;
	format.width = options->width;
	format.height = options->height;
	status = ioctl(descriptor, CAM_TRY_FORMAT, &format);
	if (status == 0 || errno != EBUSY)
		refused = 0;
	answer = "accepted";
	if (status != 0)
		answer = strerror(errno);
	printf("second open: TRY_FORMAT %s\n", answer);

	/* SET_FORMAT. */
	status = ioctl(descriptor, CAM_SET_FORMAT, &format);
	if (status == 0 || errno != EBUSY)
		refused = 0;
	answer = "accepted";
	if (status != 0)
		answer = strerror(errno);
	printf("second open: SET_FORMAT %s\n", answer);

	/* REQUEST_BUFFERS. */
	camcheck_record(&request, sizeof(request));
	request.count = 2U;
	request.memory = CAM_MEMORY_DEVICE;
	status = ioctl(descriptor, CAM_REQUEST_BUFFERS, &request);
	if (status == 0 || errno != EBUSY)
		refused = 0;
	answer = "accepted";
	if (status != 0)
		answer = strerror(errno);
	printf("second open: REQUEST_BUFFERS %s\n", answer);
	close(descriptor);

	/* Reports a second open that reached the owner's stream. */
	if (error != 0 || !refused)
		return EPERM;

	/* Succeeded: the second open was kept off. */
	return 0;
}

/*
 * Streams: every buffer queued, the frames read, judged and queued again,
 * the stream stopped.
 */
static int
camcheck_stream(
	int descriptor,
	const struct camcheck_options *options,
	const struct cam_format *format,
	unsigned count,
	struct camcheck_buffer *buffers,
	struct camcheck_result *result)
{
	struct cam_buffer buffer;
	uint32_t last_sequence;
	unsigned taken;
	int status;
	int error;

	/* Every buffer given, the stream started. */
	error = camcheck_queue_all(descriptor, count);
	if (error != 0)
		return error;
	status = ioctl(descriptor, CAM_STREAM_ON);
	if (status < 0) {
		error = errno;
		fprintf(stderr, "camcheck: CAM_STREAM_ON: %s\n", strerror(error));
		return error;
	}

	/* Each frame: its flags and sequence, its picture, and its buffer back. */
	last_sequence = 0U;
	for (taken = 0U; taken < options->frames; taken++) {
		error = camcheck_dequeue(descriptor, options->timeout_ms, &buffer);
		if (error != 0)
			break;
		result->frames++;
		if ((buffer.metadata.flags & CAM_FRAME_ERROR) != 0U)
			result->error_frames++;
		if ((buffer.metadata.flags & CAM_FRAME_INCOMPLETE) != 0U)
			result->incomplete_frames++;
		if (taken != 0U && buffer.metadata.sequence != last_sequence + 1U)
			result->sequence_gaps += (uint32_t)(buffer.metadata.sequence - last_sequence - 1U);
		last_sequence = buffer.metadata.sequence;

		/* The picture judged. */
		error = camcheck_frame(options, format, buffers[buffer.index].address, buffer.metadata.used_bytes, taken, result);
		if (error != 0)
			result->decode_failures++;

		/* The buffer back to the stream. */
		error = camcheck_queue(descriptor, buffer.index);
		if (error != 0)
			break;
	}

	/* The stream stops; a cut-short run is in the counts, not a failure to run. */
	(void)ioctl(descriptor, CAM_STREAM_OFF);

	/* Succeeded: the frames are counted. */
	return 0;
}

/*
 * Judges one frame: its patches and its counter.  The first frame of a
 * YUYV stream sets the guessed matrix and range, and the first frame is
 * written as a PPM.
 */
static int
camcheck_frame(
	const struct camcheck_options *options,
	const struct cam_format *format,
	const uint8_t *frame,
	uint32_t used,
	unsigned number,
	struct camcheck_result *result)
{
	struct camcommon_yuv_coefficients coefficients;
	struct camcheck_sample patches[CAMCHECK_PATCHES];
	struct camcheck_sample rgb_patches[CAMCHECK_PATCHES];
	struct camcheck_sample square;
	struct camcheck_region region;
	double brightness[CAMCHECK_COUNTER_BITS];
	double black;
	double white;
	double error_value;
	const char *matrix_name;
	const char *range_name;
	uint8_t *rgb;
	uint32_t value;
	size_t rgb_bytes;
	unsigned worst;
	unsigned index;
	int yuyv;
	int error;

	/* The frame as RGB: a YUYV frame converted with the guessed matrix once known, an MJPEG one decoded. */
	yuyv = format->format == CAM_FORMAT_YUYV;
	rgb_bytes = (size_t)format->width * format->height * 3U;
	rgb = malloc(rgb_bytes);
	if (rgb == NULL)
		return ENOMEM;
	if (!yuyv) {
		error = camcheck_jpeg_decode(frame, used, format->width, format->height, rgb);
		if (error != 0) {
			free(rgb);
			return error;
		}
	}

	/* Each patch: YUV for a YUYV frame, RGB for a decoded one. */
	for (index = 0U; index < CAMCHECK_PATCHES; index++) {
		camcheck_patch_region(index, &region);
		if (yuyv)
			error = camcheck_sample_yuyv(frame, format->width, format->height, format->stride_bytes, &region, &patches[index]);
		else
			error = camcheck_sample_rgb(rgb, format->width, format->height, &region, &patches[index]);
		if (error != 0) {
			free(rgb);
			return error;
		}
	}

	/* A YUYV stream: the matrix and range from its first frame, then each patch as RGB. */
	if (yuyv) {
		if (number == 0U) {
			camcheck_estimate(patches, &result->matrix, &result->range);
			matrix_name = "BT.601";
			if (result->matrix == CAMCHECK_MATRIX_BT709)
				matrix_name = "BT.709";
			range_name = "limited";
			if (result->range == CAMCHECK_RANGE_FULL)
				range_name = "full";
			printf("estimate: %s %s range\n", matrix_name, range_name);
		}

		/* Converts each patch with the estimate. */
		for (index = 0U; index < CAMCHECK_PATCHES; index++)
			camcheck_yuv_to_rgb(patches[index].value, result->matrix, result->range, rgb_patches[index].value);
	} else {
		memcpy(rgb_patches, patches, sizeof(rgb_patches));
	}

	/* The patches against the pattern. */
	error_value = camcheck_patch_error(rgb_patches, &worst);
	if (error_value > options->tolerance)
		result->patch_failures++;
	if (error_value > result->worst_error) {
		result->worst_error = error_value;
		result->worst_patch = worst;
	}

	/* The counter: each square's brightness between the black and the white patch. */
	black = patches[CAMCHECK_PATCH_BLACK].value[0];
	white = patches[CAMCHECK_PATCH_WHITE].value[0];
	if (!yuyv) {
		black = (patches[CAMCHECK_PATCH_BLACK].value[0] + patches[CAMCHECK_PATCH_BLACK].value[1] + patches[CAMCHECK_PATCH_BLACK].value[2]) / 3.0;
		white = (patches[CAMCHECK_PATCH_WHITE].value[0] + patches[CAMCHECK_PATCH_WHITE].value[1] + patches[CAMCHECK_PATCH_WHITE].value[2]) / 3.0;
	}

	/* Reads the brightness of each square of the counter. */
	for (index = 0U; index < CAMCHECK_COUNTER_BITS; index++) {
		camcheck_counter_region(index, &region);
		if (yuyv)
			error = camcheck_sample_yuyv(frame, format->width, format->height, format->stride_bytes, &region, &square);
		else
			error = camcheck_sample_rgb(rgb, format->width, format->height, &region, &square);
		if (error != 0) {
			free(rgb);
			return error;
		}

		/* A decoded frame has three channels, which are averaged to one gray level. */
		brightness[index] = square.value[0];
		if (!yuyv)
			brightness[index] = (square.value[0] + square.value[1] + square.value[2]) / 3.0;
	}

	/* Reads the counter the squares spell and adds it to the run. */
	value = camcheck_counter_decode(brightness, black, white);
	camcheck_counter_add(&result->counter, value);

	/* The first frame written, YUYV converted in the guessed matrix and range. */
	if (number == 0U) {
		error = 0;
		if (yuyv) {
			error = camcommon_yuv_coefficients_init(&coefficients, result->matrix, result->range);
			if (error == 0)
				error = camcommon_yuyv_to_rgb(frame, format->width, format->height, format->stride_bytes, &coefficients, rgb);
		}

		/* Writes the picture the frame shows. */
		if (error == 0)
			error = camcommon_write_ppm(options->output, rgb, format->width, format->height);
		if (error != 0)
			fprintf(stderr, "camcheck: %s: %s\n", options->output, strerror(error));
		else
			printf("saved %s\n", options->output);
	}

	/* Succeeded: the frame is judged. */
	free(rgb);
	return 0;
}

/*
 * Decodes an MJPEG frame (its Huffman tables completed when it has none)
 * into packed RGB of the format's size; EINVAL for a frame of another
 * size or one libjpeg-compat cannot read.
 */
static int
camcheck_jpeg_decode(
	const uint8_t *frame,
	uint32_t used,
	uint32_t width,
	uint32_t height,
	uint8_t *rgb)
{
	struct jpeg_decompress_struct info;
	struct camcheck_jpeg_error failure;
	uint8_t *volatile complete;
	JSAMPROW rows[1];
	size_t capacity;
	size_t length;
	int inserted;
	int error;

	/* The frame with its Huffman tables. */
	capacity = (size_t)used + camcommon_mjpeg_dht_size();
	complete = malloc(capacity);
	if (complete == NULL)
		return ENOMEM;
	error = camcommon_mjpeg_complete(frame, used, complete, capacity, &length, &inserted);
	if (error != 0) {
		free(complete);
		return error;
	}

	/* A decompressor whose errors come back here (setjmp stands in the condition itself). */
	memset(&info, 0, sizeof(info));
	info.err = jpeg_std_error(&failure.manager);
	failure.manager.error_exit = camcheck_jpeg_exit;
	failure.manager.emit_message = camcheck_jpeg_quiet;
	if (setjmp(failure.back) != 0) {
		jpeg_destroy_decompress(&info);
		free(complete);
		return EINVAL;
	}

	/* The header, RGB asked for. */
	jpeg_create_decompress(&info);
	jpeg_mem_src(&info, complete, (unsigned long)length);
	(void)jpeg_read_header(&info, TRUE);
	info.out_color_space = JCS_RGB;
	(void)jpeg_start_decompress(&info);
	if (info.output_width != width ||
	    info.output_height != height ||
	    info.output_components != 3) {
		jpeg_destroy_decompress(&info);
		free(complete);
		return EINVAL;
	}

	/* Each row into its place. */
	while (info.output_scanline < info.output_height) {
		rows[0] = rgb + (size_t)info.output_scanline * width * 3U;
		(void)jpeg_read_scanlines(&info, rows, 1U);
	}

	/* The decoding ends. */
	(void)jpeg_finish_decompress(&info);
	jpeg_destroy_decompress(&info);
	free(complete);

	/* Succeeded: rgb holds the picture. */
	return 0;
}

/* Jumps back to the decoder on a fatal libjpeg error. */
static void
camcheck_jpeg_exit(
	j_common_ptr info)
{
	struct camcheck_jpeg_error *failure;

	/* The manager is the first member of the failure record. */
	failure = (struct camcheck_jpeg_error *)info->err;
	longjmp(failure->back, 1);
}

/* Keeps libjpeg's warnings off the output. */
static void
camcheck_jpeg_quiet(
	j_common_ptr info,
	int level)
{
	(void)info;
	(void)level;
}

/*
 * Stops and starts the stream cycles times, a frame each, and compares
 * the kernel's allocation counters before and after: every pair's
 * allocations must be matched by as many frees.
 */
static int
camcheck_cycles(
	int descriptor,
	const struct camcheck_options *options,
	unsigned count,
	int *balanced)
{
	struct cam_buffer buffer;
	struct io_stats *before;
	struct io_stats *after;
	uint64_t allocations;
	uint64_t frees;
	unsigned cycle;
	unsigned pair;
	unsigned failures;
	int status;
	int error;

	/* Room for the two snapshots, and the first. */
	before = malloc(sizeof(*before));
	after = malloc(sizeof(*after));
	if (before == NULL || after == NULL) {
		free(before);
		free(after);
		return ENOMEM;
	}

	/* The first snapshot of the counters. */
	error = camcheck_io_stats(before);
	if (error != 0) {
		free(before);
		free(after);
		return error;
	}

	/* Each cycle: the buffers given, the stream started, a frame, the stream stopped. */
	failures = 0U;
	for (cycle = 0U; cycle < options->cycles; cycle++) {
		error = camcheck_queue_all(descriptor, count);
		if (error == 0) {
			status = ioctl(descriptor, CAM_STREAM_ON);
			if (status < 0)
				error = errno;
		}

		/* Waits for one frame. */
		if (error == 0)
			error = camcheck_dequeue(descriptor, options->timeout_ms, &buffer);
		(void)ioctl(descriptor, CAM_STREAM_OFF);
		if (error != 0)
			failures++;
	}

	/* The second snapshot, and each pair compared. */
	error = camcheck_io_stats(after);
	*balanced = 1;
	for (pair = 0U; error == 0 && pair < sizeof(camcheck_leak_pairs) / sizeof(camcheck_leak_pairs[0]); pair++) {
		allocations = after->events[camcheck_leak_pairs[pair][0]].calls - before->events[camcheck_leak_pairs[pair][0]].calls;
		frees = after->events[camcheck_leak_pairs[pair][1]].calls - before->events[camcheck_leak_pairs[pair][1]].calls;
		printf("cycles: event %u/%u allocations %llu frees %llu\n", camcheck_leak_pairs[pair][0], camcheck_leak_pairs[pair][1],
		    (unsigned long long)allocations, (unsigned long long)frees);
		if (allocations != frees)
			*balanced = 0;
	}

	/* Tells how many cycles gave a frame. */
	if (error == 0) {
		printf("cycles: %u of %u started and gave a frame; xHCI requests made %llu\n", options->cycles - failures,
		    options->cycles,
		    (unsigned long long)(after->events[IO_XHCI_REQUEST_ALLOC].calls - before->events[IO_XHCI_REQUEST_ALLOC].calls));
	}

	/* Gives back the two snapshots. */
	free(before);
	free(after);

	/* Reports a counter that could not be read, or a cycle that failed. */
	if (error != 0)
		return error;
	if (failures != 0U)
		return EIO;

	/* Succeeded: every cycle gave a frame. */
	return 0;
}

/* Reads the kernel's I/O counters (vfs.io.stats). */
static int
camcheck_io_stats(
	struct io_stats *stats)
{
	size_t length;
	int status;

	/* The snapshot, of this program's layout. */
	length = sizeof(*stats);
	status = sysctlbyname("vfs.io.stats", stats, &length, NULL, 0);
	if (status != 0) {
		fprintf(stderr, "camcheck: vfs.io.stats: %s\n", strerror(errno));
		return errno;
	}

	/* Refuses a snapshot of another layout than this program's. */
	if (length != sizeof(*stats) ||
	    stats->version != IO_STATS_VERSION ||
	    stats->count != IO_STAT_COUNT) {
		fprintf(stderr, "camcheck: vfs.io.stats: another layout\n");
		return EINVAL;
	}

	/* Succeeded: the counters are read. */
	return 0;
}

/* Tells what the frames showed. */
static void
camcheck_report(
	const struct camcheck_options *options,
	const struct camcheck_result *result)
{
	const struct camcheck_counter_run *counter;

	/* The frames and their flags. */
	printf("frames: %u of %u, error %u, incomplete %u, dropped %llu, undecodable %u\n", result->frames, options->frames,
	    result->error_frames, result->incomplete_frames, (unsigned long long)result->sequence_gaps, result->decode_failures);

	/* The colours. */
	printf("colour: worst %.1f of 255 (%s), tolerance %.1f, frames over it %u\n", result->worst_error,
	    camcheck_patch_names[result->worst_patch], options->tolerance, result->patch_failures);

	/* The counter's steps. */
	counter = &result->counter;
	printf("counter: last %u, decreases %u, steps 0:%u 1:%u 2:%u 3+:%u\n", (unsigned)counter->last, counter->decreases,
	    counter->increments[0], counter->increments[1], counter->increments[2], counter->increments[3]);
}

/* Clears a record and stamps its version and size. */
static void
camcheck_record(
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
