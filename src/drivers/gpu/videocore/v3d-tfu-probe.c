/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The texture formatting unit's probe, run by the boot diagnostic on request.
 *
 * Two questions about the unit are answered on the board, because no public
 * document answers them:
 *
 *   - whether it lays one- and two-byte raster texels out as UIF the same way
 *     as four-byte ones, and how long a large plane takes, which decides
 *     whether a decoded NV12 picture's planes can be sampled as R8 and RG8
 *     textures;
 *   - which input configuration, if any, makes it read a two- or three-plane
 *     YUV picture and write RGBA8, which would let the unit convert a decoded
 *     picture's colours as well.
 *
 * Every answer is a kernel log line starting "v3d: tfu-probe".  A trial the
 * unit faults on is reset and the next trial runs; the probe changes no
 * state the driver uses afterwards.
 */
#include <kern/clock.h>
#include <kern/dcache.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/buffer.h"
#include "drivers/gpu/bcm2711/native-state.h"
#include "drivers/gpu/bcm2711/v3d-job.h"

/* The unit's output format code for strict UIF without XOR. */
#define PROBE_OUTPUT_UIF 6U

/* The texture type codes of the unit's output: R8, RG8 and RGBA8. */
#define PROBE_TYPE_R8 0U
#define PROBE_TYPE_RG8 2U
#define PROBE_TYPE_RGBA8 4U

/* The bit positions of the output type and the input format in the input configuration. */
#define PROBE_TYPE_SHIFT 9U
#define PROBE_FORMAT_SHIFT 18U

/* The YUV picture's extent: four by two regions of sixteen by sixteen pixels. */
#define PROBE_YUV_WIDTH 64U
#define PROBE_YUV_HEIGHT 32U
#define PROBE_YUV_REGION 16U
#define PROBE_YUV_COLOURS 8U

/* Where the planes of a YUV trial start in the input interval. */
#define PROBE_CHROMA_OFFSET 0x10000U
#define PROBE_V_OFFSET 0x18000U

/* Bytes after an output that must keep their fill: the unit writes no further. */
#define PROBE_GUARD 64U

/* The fill of an output before a trial, which no conversion writes everywhere. */
#define PROBE_FILL 0xa5U

/* How far one channel may be from the expected colour and still match. */
#define PROBE_TOLERANCE 6

/* How the chroma of a YUV trial is laid out. */
enum probe_chroma {
	/* Cb and Cr interleaved in one plane, as NV12 has them; the U-plane register is zero. */
	PROBE_CHROMA_INTERLEAVED,
	/* Cr in the chroma register's plane and Cb in the U-plane register's, as I420 has them apart. */
	PROBE_CHROMA_PLANAR
};

/*
 * One YUV trial: an input format code and the planes' layout.
 *
 * Format 0 is raster, 1 and 2 are the 128- and 256-byte column layouts; the
 * codes from 3 to 10 have no public meaning and are tried for a YUV one.
 */
struct probe_trial {
	uint32_t format;
	enum probe_chroma chroma;
	uint32_t pitch;
};

/*
 * One region's colour of the YUV picture, in limited-range Y, Cb and Cr.
 *
 * The colours stay away from the range's ends, so no conversion clips them and
 * each matrix gives clearly different red, green and blue.
 */
struct probe_colour {
	uint8_t y;
	uint8_t cb;
	uint8_t cr;
};

/*
 * The colours of the YUV picture's regions, row by row.
 *
 * It is read-only for the whole boot.
 */
static const struct probe_colour probe_colours[PROBE_YUV_COLOURS] = {
	{128U, 128U, 128U},
	{100U, 100U, 170U},
	{150U, 90U, 100U},
	{90U, 170U, 110U},
	{180U, 110U, 140U},
	{60U, 140U, 120U},
	{200U, 128U, 128U},
	{120U, 150U, 90U}
};

/*
 * The YUV trials, in the order they run.
 *
 * Raster and the column layouts come first, because they are the likeliest;
 * a column layout's pitch is its column width, so a 64-pixel picture is one
 * column.  It is read-only for the whole boot.
 */
static const struct probe_trial probe_trials[] = {
	{0U, PROBE_CHROMA_INTERLEAVED, 128U},
	{0U, PROBE_CHROMA_PLANAR, 128U},
	{1U, PROBE_CHROMA_INTERLEAVED, 128U},
	{2U, PROBE_CHROMA_INTERLEAVED, 256U},
	{3U, PROBE_CHROMA_INTERLEAVED, 128U},
	{4U, PROBE_CHROMA_INTERLEAVED, 128U},
	{5U, PROBE_CHROMA_INTERLEAVED, 128U},
	{6U, PROBE_CHROMA_INTERLEAVED, 128U},
	{7U, PROBE_CHROMA_INTERLEAVED, 128U},
	{8U, PROBE_CHROMA_INTERLEAVED, 128U},
	{9U, PROBE_CHROMA_INTERLEAVED, 128U},
	{10U, PROBE_CHROMA_INTERLEAVED, 128U}
};

/* The names of the colour matrices a YUV output is compared with, by matrix number. */
static const char *const probe_matrix_names[3] = {
	"bt601-limited",
	"bt709-limited",
	"bt601-full"
};

static int probe_plane(struct bcm2711_v3d *engine, const struct bcm2711_v3d_tfu_probe_area *area, uint32_t texel_bytes, uint32_t width, uint32_t height, uint32_t pitch);
static int probe_yuv(struct bcm2711_v3d *engine, const struct bcm2711_v3d_tfu_probe_area *area, const struct probe_trial *trial);
static void probe_yuv_fill(const struct bcm2711_v3d_tfu_probe_area *area, const struct probe_trial *trial);
static void probe_yuv_judge(const struct probe_trial *trial, const uint8_t *output, uint32_t output_bytes);
static int probe_recover(struct bcm2711_v3d *engine);
static void probe_expected(uint32_t matrix, const struct probe_colour *colour, int *rgb);
static uint8_t probe_plane_byte(uint32_t x, uint32_t y, uint32_t channel);
static uint64_t probe_elapsed_us(uint64_t started, uint64_t finished, uint64_t frequency);

/*
 * Runs the texture formatting unit's probe in the boot diagnostic's storage.
 *
 * The caller owns the engine as the boot diagnostic does: no client, job or
 * other owner of the unit exists yet.  Only a failed reset after a faulting
 * trial is reported; every trial's own outcome is a log line.
 */
int
bcm2711_v3d_tfu_probe(
	struct bcm2711_v3d *engine,
	const struct bcm2711_v3d_tfu_probe_area *area)
{
	uint32_t index;
	int error;

	/* Says the probe began, so a hang inside it is told apart from one after it. */
	kern_logf("v3d: tfu-probe begin\n");

	/* One-byte texels: a partial column and block row, then the largest plane the storage holds. */
	error = probe_plane(engine, area, 1U, 96U, 40U, 128U);
	if (error == 0)
		error = probe_plane(engine, area, 1U, 512U, 480U, 512U);

	/* Two-byte texels: the same shapes as an NV12 picture's chroma. */
	if (error == 0)
		error = probe_plane(engine, area, 2U, 48U, 20U, 128U);
	if (error == 0)
		error = probe_plane(engine, area, 2U, 256U, 240U, 512U);

	/* Tries every YUV input configuration while the unit can still be reset. */
	for (index = 0; error == 0 && index < sizeof(probe_trials) / sizeof(probe_trials[0]); index++)
		error = probe_yuv(engine, area, &probe_trials[index]);

	/* Only a failed reset ends the probe early. */
	if (error != 0) {
		kern_logf("v3d: tfu-probe stopped: reset failed %d\n", error);
		return error;
	}

	/* Succeeded: every trial ran and logged its outcome. */
	kern_logf("v3d: tfu-probe end\n");
	return 0;
}

/*
 * Converts one raster plane of one- or two-byte texels to UIF and compares every texel.
 *
 * It returns nonzero only when a faulting trial could not be reset.
 */
static int
probe_plane(
	struct bcm2711_v3d *engine,
	const struct bcm2711_v3d_tfu_probe_area *area,
	uint32_t texel_bytes,
	uint32_t width,
	uint32_t height,
	uint32_t pitch)
{
	struct bcm2711_v3d_job job;
	struct bcm2711_v3d_job_result result;
	uint8_t *input;
	uint8_t *output;
	uint32_t output_bytes;
	uint32_t input_bytes;
	uint32_t texture_type;
	uint32_t x;
	uint32_t y;
	uint32_t channel;
	uint32_t offset;
	uint32_t index;
	uint32_t mismatches;
	uint32_t first_x;
	uint32_t first_y;
	uint8_t first_got;
	uint8_t first_want;
	uint8_t wanted;
	uint64_t started;
	uint64_t finished;
	uint64_t frequency;
	bool started_valid;
	bool finished_valid;
	bool overrun;
	int error;

	/* Measures both images and refuses a shape the storage cannot hold. */
	error = bcm2711_native_texture_size_texel(width, height, texel_bytes, &output_bytes);
	if (error != 0)
		return 0;
	input_bytes = pitch * height;
	if (input_bytes > area->input_bytes || output_bytes + PROBE_GUARD > area->output_bytes) {
		kern_logf("v3d: tfu-probe plane bpp=%u %ux%u too large\n", texel_bytes, width, height);
		return 0;
	}

	/* Fills every raster byte with a value that depends on its texel and channel. */
	input = area->input;
	output = area->output;
	for (y = 0; y < height; y++) {
		/* Each row's visible texels; the pitch's padding keeps its old bytes. */
		for (x = 0; x < width * texel_bytes; x++)
			input[y * pitch + x] = probe_plane_byte(x / texel_bytes, y, x % texel_bytes);
	}

	/* Fills the output, so a texel the unit did not write cannot match. */
	kern_memset(output, PROBE_FILL, output_bytes + PROBE_GUARD);

	/* The texture type of the texel size. */
	texture_type = PROBE_TYPE_R8;
	if (texel_bytes == 2U)
		texture_type = PROBE_TYPE_RG8;

	/* Raster input in whole texels, strict UIF output, as a sampled plane would use it. */
	kern_memset(&job, 0, sizeof(job));
	job.kind = BCM2711_V3D_JOB_TFU;
	job.command.tfu.input = area->input_address;
	job.command.tfu.input_bytes = input_bytes;
	job.command.tfu.output = area->output_address;
	job.command.tfu.output_bytes = output_bytes;
	job.command.tfu.output_format = PROBE_OUTPUT_UIF;
	job.command.tfu.stride = pitch / texel_bytes;
	job.command.tfu.size = (height << 16) | width;
	job.command.tfu.configuration = texture_type << PROBE_TYPE_SHIFT;
	kern_dcache_clean_range(input, input_bytes);
	kern_dcache_clean_range(output, output_bytes + PROBE_GUARD);
	kern_io_write_barrier();

	/* Runs the conversion; the counter brackets the job and its IRQ wait. */
	started_valid = kern_rtc_read_counter(&started, &frequency);
	error = bcm2711_v3d_job_run(engine, &job, &result);
	finished_valid = kern_rtc_read_counter(&finished, &frequency);
	if (error != 0) {
		kern_logf("v3d: tfu-probe plane bpp=%u %ux%u error %d\n", texel_bytes, width, height, error);
		return probe_recover(engine);
	}

	/* Makes the unit's output visible to the CPU. */
	kern_dcache_invalidate_range(output, output_bytes + PROBE_GUARD);
	kern_io_read_barrier();

	/* The bytes after the UIF size keep their fill. */
	overrun = false;
	for (index = 0; index < PROBE_GUARD; index++) {
		if (output[output_bytes + index] != PROBE_FILL)
			overrun = true;
	}

	/* Compares every channel of every visible texel at its UIF place. */
	mismatches = 0;
	first_x = 0;
	first_y = 0;
	first_got = 0;
	first_want = 0;
	for (y = 0; y < height; y++) {
		/* One row's texels and their channels. */
		for (x = 0; x < width; x++) {
			(void)bcm2711_native_texture_offset(x, y, height, texel_bytes, &offset);
			for (channel = 0; channel < texel_bytes; channel++) {
				/* Keeps the first difference, which tells a layout from a stride mistake. */
				wanted = probe_plane_byte(x, y, channel);
				if (output[offset + channel] != wanted) {
					if (mismatches == 0) {
						first_x = x;
						first_y = y;
						first_got = output[offset + channel];
						first_want = wanted;
					}

					/* Counts every differing byte. */
					mismatches++;
				}
			}
		}
	}

	/* Prints the outcome and the conversion time, which decides whether the unit can keep up. */
	if (!started_valid || !finished_valid || frequency == 0) {
		started = 0;
		finished = 0;
		frequency = 1;
	}

	/* One line holds the shape, the time, the mismatch count and the first difference. */
	kern_logf(
		"v3d: tfu-probe plane bpp=%u %ux%u pitch=%u us=%llu mismatches=%u first=%u,%u got=%02x want=%02x overrun=%u\n",
		texel_bytes,
		width,
		height,
		pitch,
		(unsigned long long)probe_elapsed_us(started, finished, frequency),
		mismatches,
		first_x,
		first_y,
		first_got,
		first_want,
		(unsigned)overrun);

	/* Succeeded: the trial ran and its outcome is logged. */
	return 0;
}

/*
 * Runs one YUV input configuration and logs what the unit wrote.
 *
 * It returns nonzero only when a faulting trial could not be reset.
 */
static int
probe_yuv(
	struct bcm2711_v3d *engine,
	const struct bcm2711_v3d_tfu_probe_area *area,
	const struct probe_trial *trial)
{
	struct bcm2711_v3d_job job;
	struct bcm2711_v3d_job_result result;
	uint32_t output_bytes;
	uint32_t luma_bytes;
	uint32_t chroma_bytes;
	int error;

	/* Measures the RGBA8 output and the planes. */
	error = bcm2711_native_texture_size_texel(PROBE_YUV_WIDTH, PROBE_YUV_HEIGHT, 4U, &output_bytes);
	if (error != 0)
		return 0;
	luma_bytes = trial->pitch * PROBE_YUV_HEIGHT;
	chroma_bytes = trial->pitch * (PROBE_YUV_HEIGHT / 2U);

	/* Writes the picture's planes and fills the output. */
	probe_yuv_fill(area, trial);
	kern_memset(area->output, PROBE_FILL, output_bytes + PROBE_GUARD);

	/* Luma in the first plane, chroma in the chroma register, and a separate U plane when planar. */
	kern_memset(&job, 0, sizeof(job));
	job.kind = BCM2711_V3D_JOB_TFU;
	job.command.tfu.input = area->input_address;
	job.command.tfu.input_bytes = luma_bytes;
	job.command.tfu.chroma = area->input_address + PROBE_CHROMA_OFFSET;
	job.command.tfu.chroma_bytes = chroma_bytes;
	if (trial->chroma == PROBE_CHROMA_PLANAR) {
		/* I420 apart: Cr in the chroma register's plane, Cb in the U plane. */
		job.command.tfu.chroma = area->input_address + PROBE_V_OFFSET;
		job.command.tfu.u_plane = area->input_address + PROBE_CHROMA_OFFSET;
		job.command.tfu.u_plane_bytes = chroma_bytes;
	}

	/* RGBA8 strict UIF output from the trial's input format. */
	job.command.tfu.output = area->output_address;
	job.command.tfu.output_bytes = output_bytes;
	job.command.tfu.output_format = PROBE_OUTPUT_UIF;
	job.command.tfu.stride = trial->pitch;
	job.command.tfu.size = (PROBE_YUV_HEIGHT << 16) | PROBE_YUV_WIDTH;
	job.command.tfu.configuration = PROBE_TYPE_RGBA8 << PROBE_TYPE_SHIFT;
	job.command.tfu.configuration |= trial->format << PROBE_FORMAT_SHIFT;
	kern_dcache_clean_range(area->input, PROBE_V_OFFSET + chroma_bytes);
	kern_dcache_clean_range(area->output, output_bytes + PROBE_GUARD);
	kern_io_write_barrier();

	/* Runs the trial; a fault or a missing completion is this configuration's answer. */
	error = bcm2711_v3d_job_run(engine, &job, &result);
	if (error != 0) {
		kern_logf(
			"v3d: tfu-probe yuv format=%u chroma=%u pitch=%u error %d\n",
			trial->format,
			(unsigned)trial->chroma,
			trial->pitch,
			error);
		return probe_recover(engine);
	}

	/* Makes the unit's output visible to the CPU. */
	kern_dcache_invalidate_range(area->output, output_bytes + PROBE_GUARD);
	kern_io_read_barrier();

	/* Compares the regions' colours with every matrix and byte order. */
	probe_yuv_judge(trial, area->output, output_bytes);

	/* Succeeded: the trial ran and its outcome is logged. */
	return 0;
}

/* Writes the YUV picture's luma and chroma planes for one trial's layout. */
static void
probe_yuv_fill(
	const struct bcm2711_v3d_tfu_probe_area *area,
	const struct probe_trial *trial)
{
	const struct probe_colour *colour;
	uint8_t *luma;
	uint8_t *chroma;
	uint8_t *v_plane;
	uint32_t x;
	uint32_t y;
	uint32_t region;

	/* The planes start at fixed places of the input interval, with zero between them. */
	luma = area->input;
	chroma = area->input + PROBE_CHROMA_OFFSET;
	v_plane = area->input + PROBE_V_OFFSET;
	kern_memset(area->input, 0, PROBE_V_OFFSET + trial->pitch * PROBE_YUV_HEIGHT);

	/* Writes the luma of every pixel from its region's colour. */
	for (y = 0; y < PROBE_YUV_HEIGHT; y++) {
		/* One row of the luma plane. */
		for (x = 0; x < PROBE_YUV_WIDTH; x++) {
			region = (y / PROBE_YUV_REGION) * (PROBE_YUV_WIDTH / PROBE_YUV_REGION) + x / PROBE_YUV_REGION;
			luma[y * trial->pitch + x] = probe_colours[region].y;
		}
	}

	/* Writes the chroma of every two-by-two block from its region's colour. */
	for (y = 0; y < PROBE_YUV_HEIGHT / 2U; y++) {
		/* One row of the chroma plane or planes. */
		for (x = 0; x < PROBE_YUV_WIDTH / 2U; x++) {
			region = (y * 2U / PROBE_YUV_REGION) * (PROBE_YUV_WIDTH / PROBE_YUV_REGION) + x * 2U / PROBE_YUV_REGION;
			colour = &probe_colours[region];
			if (trial->chroma == PROBE_CHROMA_INTERLEAVED) {
				/* NV12: Cb then Cr in one plane. */
				chroma[y * trial->pitch + x * 2U] = colour->cb;
				chroma[y * trial->pitch + x * 2U + 1U] = colour->cr;
			} else {
				/* I420 apart: Cb in its plane and Cr in the other. */
				chroma[y * trial->pitch + x] = colour->cb;
				v_plane[y * trial->pitch + x] = colour->cr;
			}
		}
	}

	/* Succeeded: both planes hold the picture. */
	return;
}

/* Logs the centre pixel of every region and how well each matrix and byte order explains them. */
static void
probe_yuv_judge(
	const struct probe_trial *trial,
	const uint8_t *output,
	uint32_t output_bytes)
{
	uint32_t pixels[PROBE_YUV_COLOURS];
	uint32_t matches[3][2];
	uint32_t region;
	uint32_t matrix;
	uint32_t order;
	uint32_t channel;
	uint32_t offset;
	uint32_t x;
	uint32_t y;
	uint32_t written;
	uint32_t index;
	uint8_t got;
	int expected[3];
	int difference;
	bool close;

	/* Counts the output bytes the unit changed, which tells "no write" from a wrong colour. */
	written = 0;
	for (index = 0; index < output_bytes; index++) {
		if (output[index] != PROBE_FILL)
			written++;
	}

	/* Reads the centre pixel of every region from its UIF place. */
	for (region = 0; region < PROBE_YUV_COLOURS; region++) {
		x = (region % (PROBE_YUV_WIDTH / PROBE_YUV_REGION)) * PROBE_YUV_REGION + PROBE_YUV_REGION / 2U;
		y = (region / (PROBE_YUV_WIDTH / PROBE_YUV_REGION)) * PROBE_YUV_REGION + PROBE_YUV_REGION / 2U;
		(void)bcm2711_native_texture_offset(x, y, PROBE_YUV_HEIGHT, 4U, &offset);
		kern_memcpy(&pixels[region], output + offset, 4U);
	}

	/* Counts, for every matrix and byte order, the regions whose colour it explains. */
	kern_memset(matches, 0, sizeof(matches));
	for (matrix = 0; matrix < 3U; matrix++) {
		/* Red first in memory (order 0) or blue first (order 1). */
		for (order = 0; order < 2U; order++) {
			for (region = 0; region < PROBE_YUV_COLOURS; region++) {
				/* A region matches when all three channels are close. */
				probe_expected(matrix, &probe_colours[region], expected);
				close = true;
				for (channel = 0; channel < 3U; channel++) {
					/* Blue first puts the channels in reverse order. */
					index = channel;
					if (order == 1U)
						index = 2U - channel;
					got = (uint8_t)(pixels[region] >> (index * 8U));
					difference = (int)got - expected[channel];
					if (difference > PROBE_TOLERANCE || difference < -PROBE_TOLERANCE)
						close = false;
				}

				/* Counts a region all of whose channels are close. */
				if (close)
					matches[matrix][order]++;
			}
		}
	}

	/* One line holds the trial, the written count, the centre pixels and the match counts. */
	kern_logf(
		"v3d: tfu-probe yuv format=%u chroma=%u pitch=%u written=%u px=%08x %08x %08x %08x %08x %08x %08x %08x\n",
		trial->format,
		(unsigned)trial->chroma,
		trial->pitch,
		written,
		pixels[0],
		pixels[1],
		pixels[2],
		pixels[3],
		pixels[4],
		pixels[5],
		pixels[6],
		pixels[7]);
	for (matrix = 0; matrix < 3U; matrix++) {
		/* Names a matrix that explains every region, in the order that does. */
		if (matches[matrix][0] == PROBE_YUV_COLOURS) {
			kern_logf("v3d: tfu-probe yuv format=%u chroma=%u MATCH %s rgba\n", trial->format, (unsigned)trial->chroma, probe_matrix_names[matrix]);
		} else if (matches[matrix][1] == PROBE_YUV_COLOURS) {
			kern_logf("v3d: tfu-probe yuv format=%u chroma=%u MATCH %s bgra\n", trial->format, (unsigned)trial->chroma, probe_matrix_names[matrix]);
		}
	}

	/* The counts of every matrix and order, which show a near miss too. */
	kern_logf(
		"v3d: tfu-probe yuv format=%u chroma=%u matches 601=%u/%u 709=%u/%u full=%u/%u\n",
		trial->format,
		(unsigned)trial->chroma,
		matches[0][0],
		matches[0][1],
		matches[1][0],
		matches[1][1],
		matches[2][0],
		matches[2][1]);

	/* Succeeded: the trial's outcome is logged. */
	return;
}

/*
 * Resets the engine after a trial that left it faulted.
 *
 * A trial that failed without a fault needs nothing.  It returns nonzero only
 * when the reset failed, which ends the probe.
 */
static int
probe_recover(
	struct bcm2711_v3d *engine)
{
	int reset;

	/* A trial refused before launch, or one that retired, leaves the engine usable. */
	if (!engine->hardware.faulted)
		return 0;

	/* Boot owns all DMA here, so a global reset recovers the unit. */
	reset = bcm2711_v3d_hardware_reset(engine);
	if (reset != 0)
		return reset;

	/* Succeeded: the engine was reset and the next trial may run. */
	kern_logf("v3d: tfu-probe reset after a faulting trial\n");
	return 0;
}

/*
 * Computes the expected red, green and blue of one colour with one matrix.
 *
 * Matrix 0 is BT.601 and 1 is BT.709, both limited range; 2 is BT.601 full
 * range.  The coefficients are scaled by 1000 and the result is clamped.
 */
static void
probe_expected(
	uint32_t matrix,
	const struct probe_colour *colour,
	int *rgb)
{
	int luma;
	int cb;
	int cr;
	int channel;

	/* Centres the chroma and, for a limited range, scales the luma. */
	cb = (int)colour->cb - 128;
	cr = (int)colour->cr - 128;
	luma = ((int)colour->y - 16) * 1164;
	if (matrix == 2U)
		luma = (int)colour->y * 1000;

	/* Applies the matrix's chroma coefficients. */
	if (matrix == 1U) {
		/* BT.709. */
		rgb[0] = luma + 1793 * cr;
		rgb[1] = luma - 213 * cb - 533 * cr;
		rgb[2] = luma + 2112 * cb;
	} else if (matrix == 2U) {
		/* BT.601 full range. */
		rgb[0] = luma + 1402 * cr;
		rgb[1] = luma - 344 * cb - 714 * cr;
		rgb[2] = luma + 1772 * cb;
	} else {
		/* BT.601 limited range. */
		rgb[0] = luma + 1596 * cr;
		rgb[1] = luma - 392 * cb - 813 * cr;
		rgb[2] = luma + 2017 * cb;
	}

	/* Rounds back to bytes and clamps each channel. */
	for (channel = 0; channel < 3; channel++) {
		rgb[channel] = (rgb[channel] + 500) / 1000;
		if (rgb[channel] < 0)
			rgb[channel] = 0;
		if (rgb[channel] > 255)
			rgb[channel] = 255;
	}

	/* Succeeded: rgb holds the colour's three channels. */
	return;
}

/* Gives a raster plane's byte a value that depends on its texel and channel. */
static uint8_t
probe_plane_byte(
	uint32_t x,
	uint32_t y,
	uint32_t channel)
{
	/* Different steps in x, y and channel keep neighbouring bytes apart. */
	return (uint8_t)(x * 7U + y * 13U + channel * 101U + 1U);
}

/* Converts a counter interval to microseconds. */
static uint64_t
probe_elapsed_us(
	uint64_t started,
	uint64_t finished,
	uint64_t frequency)
{
	/* An interval that went backwards has no length. */
	if (finished < started)
		return 0;

	/* The interval in microseconds. */
	return (finished - started) * 1000000U / frequency;
}
