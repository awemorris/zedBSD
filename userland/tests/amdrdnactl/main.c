/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amdrdna control tool: reads the driver's state, saves the GPU's
 * video BIOS image, reads the display and its picture, and resets the
 * GPU, through the driver's GPU device (/dev/gpu0 by default): its
 * reports (GPU_DIAGNOSE, include/uapi/amdrdna.h) and the reset
 * (GPU_DEVICE_RESET).
 *
 *   amdrdnactl [-f DEVICE] [status]       prints the state, one "key=value" per line
 *   amdrdnactl [-f DEVICE] vbios FILE     writes the video BIOS image to FILE
 *   amdrdnactl [-f DEVICE] display        prints the display the driver took
 *                                         over: its registers and counts
 *   amdrdnactl [-f DEVICE] screen FILE    writes the picture the display shows
 *                                         to FILE as a PPM image (root)
 *   amdrdnactl [-f DEVICE] watch SECONDS  reads the surface the plane shows
 *                                         every 2 ms and counts the reads of
 *                                         the console's surface and the runs
 *                                         of them; the first runs' pictures
 *                                         are read and their pixels that are
 *                                         not black counted (root)
 *   amdrdnactl gpu [DEVICE]               checks the GPU device the driver
 *                                         registered
 *   amdrdnactl modes [DEVICE]             prints the display's modes
 *   amdrdnactl reset [DEVICE]             resets the GPU (root); the other
 *                                         opens of the device are told it
 *                                         was lost
 *   amdrdnactl hotplug off|on [DEVICE]    acts as if the monitor went away
 *                                         or came (root)
 *   amdrdnactl present WIDTHxHEIGHT [SECONDS] [DEVICE]
 *                                         sets a mode by a present
 *
 * The exit status is 0, 1 with "amdrdnactl: <step>: <reason>" on a failure,
 * or 2 for a wrong command line.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <uapi/amdrdna.h>
#include <uapi/gpu.h>

#include "amdrdnactl.h"

/* The default device: the first GPU. */
#define CONTROL_DEVICE		"/dev/gpu0"

/* The driver whose reports the tool reads. */
#define CONTROL_DRIVER		"amdrdna"

/* The bytes one request of the image copies. */
#define CONTROL_CHUNK		4096U

/* The watch's period between two reads of the display, in microseconds. */
#define CONTROL_WATCH_US	2000U

/* The runs of the console's surface whose picture the watch reads. */
#define CONTROL_WATCH_SCREENS	3U

/* The largest picture the driver keeps, 4096 by 2160 pixels of 4 bytes, after its head. */
#define CONTROL_SCREEN_BYTES	(sizeof(struct amdrdna_screen) + (size_t)16384U * 2160U)

/* The names of the stages, by number. */
static const char *const stage_names[AMDRDNA_STAGE_COUNT] = {
	"none", "A0", "A1", "A1v", "A2", "A3t", "A3", "A4", "A5", "A5s",
	"A6", "A7", "A8", "A9", "A5c", "A10", "A11", "A12", "A13"
};

static int control_fail(const char *step, int error);
static int control_status(int descriptor);
static int control_vbios(int descriptor, const char *path);
static int control_display(int descriptor);
static int control_screen(int descriptor, const char *path);
static int control_watch(int descriptor, const char *seconds);
static void control_watch_screen(int descriptor, uint64_t console);
static int control_read_screen(int descriptor, unsigned char *buffer, struct amdrdna_screen **head, size_t *rows);
static const char *stage_name(uint32_t stage);
static void usage(void);

/*
 * Runs one command on the GPU device.
 */
int
main(
	int argc,
	char **argv)
{
	const char *path;
	const char *command;
	int descriptor;
	int first;
	int result;
	int named;
	int is_status;
	int is_vbios;
	int is_display;
	int is_screen;
	int is_watch;
	int is_gpu;
	int is_modes;
	int is_reset;
	int is_hotplug;
	int is_present;
	int on;
	uint32_t width;
	uint32_t height;
	unsigned seconds;
	char *end;

	/* The reset opens the GPU device itself (one may be named). */
	is_reset = 0;
	if (argc >= 2)
		is_reset = strcmp(argv[1], "reset") == 0;
	if (is_reset && argc <= 3) {
		path = CONTROL_DEVICE;
		if (argc == 3)
			path = argv[2];
		result = control_reset(path);
		return result;
	}

	/* The output's off and on: the GPU device (one may be named). */
	is_hotplug = 0;
	if (argc >= 3)
		is_hotplug = strcmp(argv[1], "hotplug") == 0;
	if (is_hotplug && argc <= 4) {
		path = CONTROL_DEVICE;
		if (argc == 4)
			path = argv[3];
		on = strcmp(argv[2], "on") == 0;
		result = control_hotplug(path, on);
		return result;
	}

	/* A present of another size: WIDTHxHEIGHT, the seconds held, the GPU device. */
	is_present = 0;
	if (argc >= 3)
		is_present = strcmp(argv[1], "present") == 0;
	if (is_present && argc <= 5) {
		width = (uint32_t)strtoul(argv[2], &end, 10);
		height = 0U;
		if (*end == 'x')
			height = (uint32_t)strtoul(end + 1, NULL, 10);
		seconds = 3U;
		if (argc >= 4)
			seconds = (unsigned)strtoul(argv[3], NULL, 10);
		path = CONTROL_DEVICE;
		if (argc == 5)
			path = argv[4];
		if (width == 0U || height == 0U) {
			fprintf(stderr, "amdrdnactl: present WIDTHxHEIGHT [SECONDS] [GPU-DEVICE]\n");
			return 2;
		}
		result = control_present(path, width, height, seconds);
		return result;
	}

	/* The GPU device check and the modes, on the device named or the first. */
	is_gpu = 0;
	is_modes = 0;
	if (argc >= 2) {
		is_gpu = strcmp(argv[1], "gpu") == 0;
		is_modes = strcmp(argv[1], "modes") == 0;
	}
	if (is_gpu || is_modes) {
		path = CONTROL_DEVICE;
		if (argc == 3)
			path = argv[2];
		if (is_modes)
			result = control_modes(path);
		else
			result = control_gpu(path);
		return result;
	}

	/* The device: -f names another one. */
	path = CONTROL_DEVICE;
	first = 1;
	named = 0;
	if (argc >= 3)
		named = strcmp(argv[1], "-f") == 0;
	if (named) {
		path = argv[2];
		first = 3;
	}

	/* The command, status by default, and which one it is. */
	command = "status";
	if (first < argc)
		command = argv[first];
	is_status = strcmp(command, "status") == 0;
	is_vbios = strcmp(command, "vbios") == 0;
	is_display = strcmp(command, "display") == 0;
	is_screen = strcmp(command, "screen") == 0;
	is_watch = strcmp(command, "watch") == 0;

	/* Opens the device, which must be the driver's. */
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return control_fail(path, errno);
	result = control_driver_check(descriptor, CONTROL_DRIVER);
	if (result != 0) {
		(void)close(descriptor);
		return control_fail(path, result);
	}

	/* Runs the command with the words it takes. */
	if (is_status && first + 1 >= argc) {
		result = control_status(descriptor);
	} else if (is_vbios && first + 2 == argc) {
		result = control_vbios(descriptor, argv[first + 1]);
	} else if (is_display && first + 1 == argc) {
		result = control_display(descriptor);
	} else if (is_screen && first + 2 == argc) {
		result = control_screen(descriptor, argv[first + 1]);
	} else if (is_watch && first + 2 == argc) {
		result = control_watch(descriptor, argv[first + 1]);
	} else {
		usage();
		result = 2;
	}

	/* Closes the device and reports the command's result. */
	(void)close(descriptor);
	return result;
}

/* Reports a failed step and gives the failing exit status. */
static int
control_fail(
	const char *step,
	int error)
{
	/* The line a reader of the run sees. */
	fprintf(stderr, "amdrdnactl: %s: %s\n", step, strerror(error));
	return 1;
}

/* Prints the driver's state. */
static int
control_status(
	int descriptor)
{
	struct amdrdna_status status;
	unsigned posted;
	unsigned on_gpu;
	unsigned training;
	unsigned ours;
	unsigned irq_on;
	unsigned fault;
	unsigned hung;
	int result;

	/* Asks the driver for its state, the whole report of this interface. */
	memset(&status, 0, sizeof(status));
	result = control_report(descriptor, AMDRDNA_DIAGNOSE_STATUS, 0U, 0U, &status, sizeof(status), NULL);
	if (result != 0)
		return control_fail("status", result);
	if (status.version != AMDRDNA_INTERFACE_VERSION || status.size != sizeof(status))
		return control_fail("status", EIO);

	/* Turns the flags the lines print into 0 or 1. */
	posted = (status.flags & AMDRDNA_STATUS_POSTED) != 0U;
	on_gpu = (status.flags & AMDRDNA_STATUS_GOP_ON_GPU) != 0U;
	training = (status.flags & AMDRDNA_STATUS_TRAINING_ALLOWED) != 0U;
	ours = (status.flags & AMDRDNA_STATUS_SECURE_OS_OURS) != 0U;
	irq_on = (status.flags & AMDRDNA_STATUS_INTERRUPTS_ON) != 0U;
	fault = (status.flags & AMDRDNA_STATUS_THERMAL_FAULT) != 0U;
	hung = (status.flags & AMDRDNA_STATUS_SMU_HUNG) != 0U;

	/* Prints the stage machine, the identity and what each stage read. */
	printf("stage=%s\n", stage_name(status.stage));
	printf("target=%s\n", stage_name(status.target));
	printf("running=%s\n", stage_name(status.running));
	printf("failed=%s error=%d\n", stage_name(status.failed_stage), (int)status.failed_error);
	printf("flags=0x%08x\n", (unsigned)status.flags);
	printf("pci=%08x rev=%02x subsystem=%08x\n", (unsigned)status.pci_id, (unsigned)status.pci_revision, (unsigned)status.pci_subsystem);
	printf("gc=%u.%u.%u dcn=%u.%u.%u psp=%u.%u.%u smu=%u.%u.%u\n",
	    (unsigned)(status.graphics_version >> 16), (unsigned)((status.graphics_version >> 8) & 0xffU), (unsigned)(status.graphics_version & 0xffU),
	    (unsigned)(status.display_version >> 16), (unsigned)((status.display_version >> 8) & 0xffU), (unsigned)(status.display_version & 0xffU),
	    (unsigned)(status.security_version >> 16), (unsigned)((status.security_version >> 8) & 0xffU), (unsigned)(status.security_version & 0xffU),
	    (unsigned)(status.power_version >> 16), (unsigned)((status.power_version >> 8) & 0xffU), (unsigned)(status.power_version & 0xffU));
	printf("vram=%uMiB bar0=%uMiB cpu_window=%uMiB rebar=0x%08x/0x%08x\n",
	    (unsigned)status.vram_mib, (unsigned)status.bar0_mib, (unsigned)status.cpu_window_mib,
	    (unsigned)status.rebar_capability, (unsigned)status.rebar_control);
	printf("posted=%u sos=0x%08x boot=0x%08x smu=0x%08x grbm=0x%08x scratch7=0x%08x fb=0x%llx\n",
	    posted,
	    (unsigned)status.secure_os_word, (unsigned)status.boot_mailbox, (unsigned)status.power_firmware_state,
	    (unsigned)status.engine_status, (unsigned)status.bios_scratch_7, (unsigned long long)status.frame_buffer_base);
	printf("gop otg=%u hubp=%u dig=%u kind=%u %ux%u on %ux%u shape=%u pitch=0x%x format=%u vram=+0x%llx on_gpu=%u\n",
	    (unsigned)status.gop_timing, (unsigned)status.gop_plane, (unsigned)status.gop_output, (unsigned)status.gop_output_kind,
	    (unsigned)status.gop_source_width, (unsigned)status.gop_source_height,
	    (unsigned)status.gop_width, (unsigned)status.gop_height, (unsigned)status.gop_shape,
	    (unsigned)status.gop_pitch_word, (unsigned)status.gop_format,
	    (unsigned long long)status.gop_vram_offset, on_gpu);
	printf("vbios source=%u length=%u fwinfo=%u.%u caps=0x%x pptid=%u shutdown=%uC training=%u\n",
	    (unsigned)status.vbios_source, (unsigned)status.vbios_length,
	    (unsigned)(status.firmware_table_version >> 8), (unsigned)(status.firmware_table_version & 0xffU),
	    (unsigned)status.firmware_capability_bits, (unsigned)status.power_table_id, (unsigned)status.shutdown_temperature,
	    training);
	printf("memory gart=0x%llx+%uMiB cpu_free=%uMiB gpu_free=%uMiB irq=%u signals=%u faults=%u underflow=%u\n",
	    (unsigned long long)status.gart_base, (unsigned)status.gart_mib,
	    (unsigned)status.cpu_pool_free_mib, (unsigned)status.gpu_pool_free_mib,
	    (unsigned)status.interrupt_kind, (unsigned)status.interrupt_signals,
	    (unsigned)status.vm_faults, (unsigned)status.display_underflow);
	printf("training result=%u reason=%u ms=%u\n",
	    (unsigned)status.training_result, (unsigned)status.training_reason, (unsigned)status.training_ms);
	printf("psp tmr=%uKiB programs=%u sos=0x%08x warnings=%u ours=%u irq_on=%u entries=%u unknown=%u\n",
	    (unsigned)status.region_kib, (unsigned)status.programs_loaded, (unsigned)status.secure_os_version,
	    (unsigned)status.security_warnings,
	    ours,
	    irq_on,
	    (unsigned)status.interrupt_entries, (unsigned)status.interrupt_unknown);
	printf("power smu=%u.%u.%u if=0x%x fan_mode=%u duty=%u%% fan=%urpm driven=%u%% temp=%uC max=%uC limit=%uC alerts=%u fault=%u hung=%u\n",
	    (unsigned)((status.smu_version >> 16) & 0xffU), (unsigned)((status.smu_version >> 8) & 0xffU), (unsigned)(status.smu_version & 0xffU),
	    (unsigned)status.smu_interface, (unsigned)status.fan_mode, (unsigned)status.fan_duty,
	    (unsigned)status.fan_rpm, (unsigned)status.fan_driven, (unsigned)status.temperature,
	    (unsigned)status.temperature_highest, (unsigned)status.temperature_limit, (unsigned)status.thermal_alerts,
	    fault,
	    hung);
	printf("engines flags=0x%x compute=%u gb=0x%08x rb=0x%x cu=%u tcc_off=0x%x fences=%u eop=%u traps=%u copied=%uB dispatch=%u\n",
	    (unsigned)status.engine_flags, (unsigned)status.compute_queues, (unsigned)status.address_config,
	    (unsigned)status.backends, (unsigned)status.compute_units, (unsigned)status.channels_off,
	    (unsigned)status.fences, (unsigned)status.end_of_pipe_interrupts, (unsigned)status.copy_traps,
	    (unsigned)status.copied_bytes, (unsigned)status.dispatch_words);

	/* The address spaces and the GPU device (stage A5s). */
	printf("sessions spaces=%u held=%u flushes=%u device=0x%x\n",
	    (unsigned)(status.spaces & 0xffffU), (unsigned)(status.spaces >> 16),
	    (unsigned)status.context_flushes, (unsigned)status.device_flags);

	/* The memory clock, and the display hub's memory state and underflow as they stand (from stage A6). */
	printf("clocks mem=%uMHz dcef=%uMHz state=0x%x handshake=0x%08x underflow=%u timing_underflow=%u switch_mark=%u index=0x%x\n",
	    (unsigned)status.memory_clock, (unsigned)status.return_clock,
	    (unsigned)status.memory_state, (unsigned)status.memory_handshake,
	    (unsigned)status.underflow_now, (unsigned)status.timing_underflow_now,
	    (unsigned)status.switch_mark, (unsigned)status.handshake_index);
	return 0;
}

/* Writes the video BIOS image to a file. */
static int
control_vbios(
	int descriptor,
	const char *path)
{
	unsigned char chunk[CONTROL_CHUNK];
	uint64_t copied;
	uint64_t total;
	uint64_t offset;
	size_t written;
	FILE *stream;
	int result;

	/* Opens the file the image goes to. */
	stream = fopen(path, "wb");
	if (stream == NULL)
		return control_fail(path, errno);

	/* Copies the image one chunk at a time until the driver has none left. */
	offset = 0U;
	for (;;) {
		/* Asks for the chunk at the offset. */
		result = control_report_part(descriptor, AMDRDNA_DIAGNOSE_VBIOS, 0U, offset, chunk, sizeof(chunk), &copied, &total);
		if (result != 0) {
			(void)fclose(stream);
			return control_fail("vbios", result);
		}

		/* An empty chunk ends the image. */
		if (copied == 0U)
			break;

		/* Writes the chunk. */
		written = fwrite(chunk, 1U, (size_t)copied, stream);
		if (written != copied) {
			(void)fclose(stream);
			return control_fail(path, EIO);
		}

		/* Moves on, and stops at the image's end. */
		offset += copied;
		if (offset >= total)
			break;
	}

	/* Closes the file; a failed close loses the image. */
	result = fclose(stream);
	if (result != 0)
		return control_fail(path, errno);

	/* Says where the image went. */
	printf("vbios=%s bytes=%llu\n", path, (unsigned long long)offset);
	return 0;
}

/* Prints the display the driver took over: its picture, its addresses, its registers and its counts. */
static int
control_display(
	int descriptor)
{
	struct amdrdna_display_state state;
	int result;

	/* Asks the driver for the display's state. */
	memset(&state, 0, sizeof(state));
	result = control_report(descriptor, AMDRDNA_DIAGNOSE_DISPLAY, 0U, 0U, &state, sizeof(state), NULL);
	if (result != 0)
		return control_fail("display", result);

	/* The takeover, the picture and the addresses. */
	printf("display flags=0x%04x otg=%u hubp=%u hpd=%u %ux%u stride=%u refresh=%u.%03uHz\n",
	    (unsigned)state.flags, (unsigned)state.timing, (unsigned)state.plane, (unsigned)state.hotplug,
	    (unsigned)state.width, (unsigned)state.height, (unsigned)state.stride,
	    (unsigned)(state.refresh_millihz / 1000U), (unsigned)(state.refresh_millihz % 1000U));
	printf("address console=0x%llx requested=0x%llx shown=0x%llx plane=0x%llx cursor=0x%llx\n",
	    (unsigned long long)state.console_address, (unsigned long long)state.requested_address,
	    (unsigned long long)state.shown_address, (unsigned long long)state.plane_address,
	    (unsigned long long)state.cursor_address);

	/* The registers as read now. */
	printf("timing frame=0x%06x position=0x%08x sync=0x%08x combiner=0x%08x\n",
	    (unsigned)state.frame_count, (unsigned)state.position, (unsigned)state.sync_status, (unsigned)state.combiner_input);
	printf("plane control=0x%08x flip=0x%08x flip_irq=0x%08x\n",
	    (unsigned)state.plane_control, (unsigned)state.flip_control, (unsigned)state.flip_interrupt);
	printf("cursor control=0x%08x size=0x%08x position=0x%08x hot=0x%08x pixel=0x%08x\n",
	    (unsigned)state.cursor_control, (unsigned)state.cursor_size, (unsigned)state.cursor_position,
	    (unsigned)state.cursor_hot_spot, (unsigned)state.pixel_cursor);
	printf("hotplug status=0x%08x control=0x%08x\n", (unsigned)state.hotplug_status, (unsigned)state.hotplug_control);

	/* The counts. */
	printf("counts frame_irq=%llu flip_irq=%llu hotplug_irq=%llu flips=%llu refreshes=%llu topology=%llu presents=%llu leases=%llu\n",
	    (unsigned long long)state.frame_signals, (unsigned long long)state.flip_signals,
	    (unsigned long long)state.hotplug_signals, (unsigned long long)state.flips,
	    (unsigned long long)state.refresh_count, (unsigned long long)state.topology,
	    (unsigned long long)state.presents, (unsigned long long)state.leases);
	return 0;
}

/*
 * Writes the picture the display shows to a file as a binary PPM (P6):
 * the driver's rows of 32-bit B, G, R, A pixels become R, G, B triples.
 */
static int
control_screen(
	int descriptor,
	const char *path)
{
	struct amdrdna_screen *screen;
	unsigned char *buffer;
	unsigned char *pixels;
	unsigned char *rgb;
	FILE *stream;
	size_t rows;
	size_t written;
	uint32_t x;
	uint32_t y;
	int result;

	/* A buffer for the head and the largest picture the driver keeps. */
	buffer = malloc(CONTROL_SCREEN_BYTES);
	if (buffer == NULL)
		return control_fail("screen", ENOMEM);

	/* Asks the driver for the head and the rows. */
	result = control_read_screen(descriptor, buffer, &screen, &rows);
	if (result != 0) {
		free(buffer);
		return control_fail("screen", result);
	}
	pixels = buffer + sizeof(*screen);

	/* The R, G, B triples of every row copied. */
	rgb = malloc((size_t)screen->width * screen->height * 3U);
	if (rgb == NULL) {
		free(buffer);
		return control_fail("screen", ENOMEM);
	}

	/* Each row the driver copied, pixel by pixel. */
	for (y = 0U; y < rows; y++) {
		for (x = 0U; x < screen->width; x++) {
			/* B, G, R, A in, R, G, B out. */
			rgb[((size_t)y * screen->width + x) * 3U] = pixels[(size_t)y * screen->stride + x * 4U + 2U];
			rgb[((size_t)y * screen->width + x) * 3U + 1U] = pixels[(size_t)y * screen->stride + x * 4U + 1U];
			rgb[((size_t)y * screen->width + x) * 3U + 2U] = pixels[(size_t)y * screen->stride + x * 4U];
		}
	}

	/* Writes the file. */
	stream = fopen(path, "wb");
	if (stream == NULL) {
		free(rgb);
		free(buffer);
		return control_fail(path, errno);
	}

	/* The header, the rows, the close. */
	fprintf(stream, "P6\n%u %u\n255\n", (unsigned)screen->width, (unsigned)y);
	written = fwrite(rgb, 1U, (size_t)screen->width * y * 3U, stream);
	result = fclose(stream);
	if (written != (size_t)screen->width * y * 3U || result != 0) {
		free(rgb);
		free(buffer);
		return control_fail(path, EIO);
	}

	/* Says what was written. */
	printf("screen=%s %ux%u stride=%u address=0x%llx\n", path, (unsigned)screen->width, (unsigned)y,
	    (unsigned)screen->stride, (unsigned long long)screen->address);
	free(rgb);
	free(buffer);
	return 0;
}

/*
 * Reads the head and the rows of the picture the display shows into
 * buffer (CONTROL_SCREEN_BYTES); head points at the head in buffer, the
 * rows follow it, and rows gets the whole rows copied.  Returns 0 or the
 * request's errno (EIO for a head of another interface).
 */
static int
control_read_screen(
	int descriptor,
	unsigned char *buffer,
	struct amdrdna_screen **head,
	size_t *rows)
{
	struct amdrdna_screen *screen;
	uint64_t copied;
	int result;

	/* The report: the head, then the rows. */
	copied = 0U;
	result = control_report(descriptor, AMDRDNA_DIAGNOSE_SCREEN, 0U, 0U, buffer, CONTROL_SCREEN_BYTES, &copied);
	if (result != 0)
		return result;
	screen = (struct amdrdna_screen *)(void *)buffer;
	if (copied < sizeof(*screen) || screen->version != AMDRDNA_INTERFACE_VERSION || screen->size != sizeof(*screen))
		return EIO;
	if (screen->stride == 0U || screen->stride < screen->width * 4U)
		return EIO;

	/* The whole rows copied, as many as the picture has. */
	*rows = (size_t)((copied - sizeof(*screen)) / screen->stride);
	if (*rows > screen->height)
		*rows = screen->height;
	*head = screen;
	return 0;
}

/*
 * Reads the display's state every CONTROL_WATCH_US for SECONDS and prints
 * one line: the reads, those that found the plane on the console's
 * surface, the runs of such reads, and the leases and presentations over
 * the watch.
 */
static int
control_watch(
	int descriptor,
	const char *seconds)
{
	struct amdrdna_display_state state;
	unsigned long long first_leases;
	unsigned long long first_presents;
	unsigned long long samples;
	unsigned long long console;
	unsigned long long runs;
	unsigned long total;
	unsigned long turn;
	unsigned screens;
	int on_console;
	int result;

	/* The reads the seconds hold. */
	total = strtoul(seconds, NULL, 10) * (1000000UL / CONTROL_WATCH_US);
	if (total == 0UL) {
		usage();
		return 2;
	}

	/* Counts every read. */
	samples = 0ULL;
	console = 0ULL;
	runs = 0ULL;
	on_console = 0;
	screens = 0U;
	first_leases = 0ULL;
	first_presents = 0ULL;
	memset(&state, 0, sizeof(state));
	for (turn = 0UL; turn < total; turn++) {
		/* One read of the display. */
		memset(&state, 0, sizeof(state));
		result = control_report(descriptor, AMDRDNA_DIAGNOSE_DISPLAY, 0U, 0U, &state, sizeof(state), NULL);
		if (result != 0)
			return control_fail("watch", result);
		if (turn == 0UL) {
			first_leases = (unsigned long long)state.leases;
			first_presents = (unsigned long long)state.presents;
		}

		/* The console's surface on the plane, and where a run of it starts. */
		samples++;
		if (state.shown_address == state.console_address) {
			console++;
			if (!on_console) {
				runs++;
				if (screens < CONTROL_WATCH_SCREENS) {
					control_watch_screen(descriptor, state.console_address);
					screens++;
				}
			}
			on_console = 1;
		} else {
			on_console = 0;
		}

		/* The next read a period later. */
		(void)usleep(CONTROL_WATCH_US);
	}

	/* The one line. */
	printf("watch samples=%llu console=%llu runs=%llu leases=%llu presents=%llu console_now=%d\n",
	    samples, console, runs, (unsigned long long)state.leases - first_leases,
	    (unsigned long long)state.presents - first_presents, on_console);
	return 0;
}

/*
 * Reads the picture the plane shows when it is still the console's surface
 * and prints its pixels that are not black (the colour bytes, not the
 * fourth).
 */
static void
control_watch_screen(
	int descriptor,
	uint64_t console)
{
	struct amdrdna_screen *screen;
	unsigned long long lit;
	unsigned long long pixels;
	unsigned char *buffer;
	const uint32_t *words;
	size_t rows;
	uint32_t x;
	uint32_t y;
	int result;

	/* A buffer for the head and the largest picture the driver keeps. */
	buffer = malloc(CONTROL_SCREEN_BYTES);
	if (buffer == NULL)
		return;

	/* Asks the driver for the rows of the surface the plane shows now. */
	result = control_read_screen(descriptor, buffer, &screen, &rows);
	if (result != 0) {
		printf("watch screen failed errno=%d\n", result);
		free(buffer);
		return;
	}

	/* The plane moved on before the read: another surface was read. */
	if (screen->address != console) {
		printf("watch screen address=0x%llx (not the console's)\n", (unsigned long long)screen->address);
		free(buffer);
		return;
	}

	/* Counts the pixels that are not black, row by row. */
	words = (const uint32_t *)(const void *)(buffer + sizeof(*screen));
	lit = 0ULL;
	pixels = 0ULL;
	for (y = 0U; y < rows; y++) {
		for (x = 0U; x < screen->width; x++) {
			pixels++;
			if ((words[(size_t)y * (screen->stride / 4U) + x] & 0x00ffffffU) != 0U)
				lit++;
		}
	}
	printf("watch screen address=0x%llx pixels=%llu not_black=%llu\n", (unsigned long long)screen->address, pixels, lit);
	free(buffer);
}

/* Names a stage number. */
static const char *
stage_name(
	uint32_t stage)
{
	/* A number past the last stage has no name. */
	if (stage >= AMDRDNA_STAGE_COUNT)
		return "?";

	/* The stage's name. */
	return stage_names[stage];
}

/* Prints the command line. */
static void
usage(void)
{
	/* The one usage line. */
	fprintf(stderr, "usage: amdrdnactl [-f DEVICE] [status | vbios FILE | display | screen FILE | watch SECONDS] | amdrdnactl gpu|modes [DEVICE] | amdrdnactl reset [DEVICE] | amdrdnactl hotplug off|on [DEVICE] | amdrdnactl present WIDTHxHEIGHT [SECONDS] [DEVICE]\n");
}

/*
 * Checks that the GPU device open at descriptor is the named driver's,
 * whose reports the tool reads.  Returns 0, the query's errno, or ENODEV
 * for another driver's device.
 */
int
control_driver_check(
	int descriptor,
	const char *driver)
{
	struct gpu_info info;
	int result;

	/* The device's description. */
	memset(&info, 0, sizeof(info));
	info.version = GPU_ABI_VERSION;
	info.size = sizeof(info);
	result = ioctl(descriptor, GPU_GET_INFO, &info);
	if (result != 0)
		return errno;

	/* The driver's name must be the one asked for. */
	info.driver_name[sizeof(info.driver_name) - 1U] = '\0';
	if (strcmp(info.driver_name, driver) != 0)
		return ENODEV;

	/* Succeeded: the device is the driver's. */
	return 0;
}

/*
 * Reads a report whole into buffer (capacity bytes), from its start;
 * copied gets the bytes read (may be NULL, when the report must fill the
 * buffer exactly).  Returns 0, the request's errno, or EIO for a
 * report of another length than the buffer when copied is NULL.
 */
int
control_report(
	int descriptor,
	uint32_t kind,
	uint32_t argument,
	uint64_t offset,
	void *buffer,
	uint64_t capacity,
	uint64_t *copied)
{
	uint64_t bytes;
	uint64_t total;
	int result;

	/* The report from offset. */
	result = control_report_part(descriptor, kind, argument, offset, buffer, capacity, &bytes, &total);
	if (result != 0)
		return result;

	/* A fixed-size report fills the buffer. */
	if (copied == NULL) {
		if (bytes != capacity || total != capacity)
			return EIO;
		return 0;
	}

	/* Succeeded: the bytes read. */
	*copied = bytes;
	return 0;
}

/*
 * Reads part of a report: from offset into buffer, at most capacity
 * bytes; copied and total get the bytes read and the report's length.
 * Returns 0 or the request's errno.
 */
int
control_report_part(
	int descriptor,
	uint32_t kind,
	uint32_t argument,
	uint64_t offset,
	void *buffer,
	uint64_t capacity,
	uint64_t *copied,
	uint64_t *total)
{
	struct gpu_diagnose request;
	int result;

	/* The request. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.kind = kind;
	request.argument = argument;
	request.offset = offset;
	request.address = (uint64_t)(uintptr_t)buffer;
	request.capacity = capacity;
	result = ioctl(descriptor, GPU_DIAGNOSE, &request);
	if (result != 0)
		return errno;

	/* Succeeded: the part read and the report's length. */
	*copied = request.bytes;
	*total = request.total;
	return 0;
}
