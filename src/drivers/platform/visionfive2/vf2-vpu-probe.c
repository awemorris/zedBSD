/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test kernels' probe of the Wave511 decoder's behaviour, built only
 * with CONFIG_DRIVER_VF2_VPU_PROBE.
 *
 * It reads a few H.264 and H.265 streams from the root file system, cuts
 * them into pictures and decodes each stream in several ways, writing one
 * line per picture: the frame buffer the picture went into, the one the
 * core gave out, its order count and the FNV-1a of its visible NV12
 * bytes, which the host compares with a reference decode.  The ways are:
 *
 *   A  the core picks the frame buffer, each one handed back after its
 *      picture is read (the baseline, and whether a picture comes out
 *      with no delay);
 *   B  the host picks the frame buffer by raising every other one's
 *      display flag, the references' too (whether the flags steer the
 *      core and leave the references readable);
 *   C  as B, with the picked pair's linear buffer first pointed at one of
 *      two other pictures (whether one pair can be updated alone);
 *   D  as A, the read position moved back only by the position the
 *      picture command carries (whether the query is needed);
 *   E  as A, the ring's old bytes read through the cached direct map
 *      before the new bytes are written uncached (whether the core reads
 *      through the CPU's caches);
 *   F  as B, the frame buffer read through the cached direct map before
 *      the decode and both ways after it (whether the core's writes reach
 *      the CPU's caches).
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/platform/visionfive2/vf2-vpu-probe.h"
#include "drivers/gpu/powervr/firmware-file.h"
#include "drivers/vpu/wave5.h"
#include <kern/dcache.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/* Where the streams are and how large one may be. */
#define PROBE_DIRECTORY		"/var/vpu-probe/"
#define PROBE_STREAM_LIMIT	(8U * 1024U * 1024U)

/* The most pictures a stream is cut into, and the ring an instance reads. */
#define PROBE_MAX_UNITS		64U
#define PROBE_RING		(4U * 1024U * 1024U)

/* The FNV-1a 32 offset basis and prime. */
#define PROBE_FNV_BASIS		0x811c9dc5U
#define PROBE_FNV_PRIME		0x01000193U

/* The bytes the pool's and the destinations' pictures are filled with before a decode. */
#define PROBE_POOL_FILL		0x3cU
#define PROBE_DESTINATION_FILL	0xa5U

/* The highest address the core names. */
#define PROBE_DMA_LIMIT		0xffffffffULL

/* The ways a stream is decoded (see the file's comment), as bits of a stream's set. */
#define RUN_A		0x01U
#define RUN_B		0x02U
#define RUN_C		0x04U
#define RUN_D		0x08U
#define RUN_E		0x10U
#define RUN_F		0x20U
#define RUN_EVERY	0x3fU
#define RUN_COUNT	6U

/*
 * One stream of the probe: its file below the probe's directory, the name
 * its lines carry, its codec, its visible size and the ways it is decoded.
 */
struct probe_stream {
	const char *file;
	const char *name;
	unsigned codec;
	uint32_t width;
	uint32_t height;
	unsigned runs;
};

/* One picture of a stream: where its bytes start and how many there are. */
struct probe_unit {
	size_t offset;
	size_t length;
};

/* One picture buffer of the probe's, with its uncached view and its planes' bus addresses. */
struct probe_buffer {
	struct kern_pmem run;
	uint8_t *view;
	struct drv_wave5_picture picture;
};

/* The text the probe appends to: its bytes, its room and its length. */
struct probe_text {
	char *bytes;
	size_t capacity;
	size_t length;
};

/*
 * What one decode of a stream works with: the stream, its bytes and
 * pictures, the way, the frame buffers, the destinations, the stride and
 * the planes' offsets, and when each buffer was last decoded into.
 */
struct probe_run {
	const struct probe_stream *stream;
	const uint8_t *bytes;
	struct probe_unit units[PROBE_MAX_UNITS];
	unsigned unit_count;
	unsigned way;
	struct drv_wave5_stream *instance;
	struct drv_wave5_sequence sequence;
	struct probe_buffer pool[DRV_WAVE5_MAX_BUFFERS];
	struct probe_buffer destinations[2];
	unsigned count;
	uint32_t stride;
	size_t chroma_offset;
	size_t buffer_size;
	unsigned last_used[DRV_WAVE5_MAX_BUFFERS];
};

/* The probe's streams. */
static const struct probe_stream probe_streams[] = {
	{ "h264-pyramid.h264", "h264-pyramid", DRV_WAVE5_H264, 352U, 288U, RUN_EVERY },
	{ "h264-main.h264", "h264-main", DRV_WAVE5_H264, 352U, 288U, RUN_EVERY },
	{ "h264-slices.h264", "h264-slices", DRV_WAVE5_H264, 352U, 288U, RUN_A },
	{ "h265-b.h265", "h265-b", DRV_WAVE5_H265, 352U, 288U, RUN_EVERY },
	{ "h264-1080.h264", "h264-1080", DRV_WAVE5_H264, 1920U, 1080U, RUN_A },
	{ "h265-1080.h265", "h265-1080", DRV_WAVE5_H265, 1920U, 1080U, RUN_A }
};

/* The letters the ways' lines carry, in bit order. */
static const char probe_way_names[RUN_COUNT] = { 'A', 'B', 'C', 'D', 'E', 'F' };

/*
 * The uncached view of RAM, which only architectures with devices that do
 * not snoop the CPU caches provide; the probe needs it.
 */
extern int kern_pmem_map_uncached(const struct kern_pmem *run, void **mapped) __attribute__((weak));
extern int kern_pmem_unmap_uncached(void *mapped, size_t size) __attribute__((weak));

static void put(struct probe_text *text, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void probe_stream(struct drv_wave5 *core, const struct probe_stream *stream, struct probe_text *text);
static int decode_stream(struct drv_wave5 *core, struct probe_run *run, struct probe_text *text);
static int decode_unit(struct probe_run *run, unsigned unit, struct probe_text *text);
static unsigned cut_units(const uint8_t *bytes, size_t size, unsigned codec, struct probe_unit *units);
static int unit_starts(const uint8_t *bytes, size_t size, size_t at, unsigned codec, int has_picture, int *picture);
static int take_buffers(struct probe_run *run);
static void give_buffers(struct probe_run *run);
static int take_buffer(size_t size, size_t chroma_offset, struct probe_buffer *buffer);
static void give_buffer(struct probe_buffer *buffer);
static uint32_t hash_picture(const struct probe_run *run, const uint8_t *bytes);
static void touch_cached(const uint8_t *bytes, size_t size);
static void drop_cached(const void *bytes, size_t size);
static unsigned least_used(const struct probe_run *run);

/*
 * Decodes the probe's streams in each of its ways and appends the lines.
 */
size_t
drv_vf2_vpu_probe_run(
	struct drv_wave5 *core,
	char *text,
	size_t capacity,
	size_t length)
{
	struct probe_text out;
	unsigned position;

	/* Appends to the text as it is. */
	out.bytes = text;
	out.capacity = capacity;
	out.length = length;

	/* Probes each stream. */
	for (position = 0; position < sizeof(probe_streams) / sizeof(probe_streams[0]); position++)
		probe_stream(core, &probe_streams[position], &out);

	/* Succeeded: the text's new length. */
	put(&out, "probe end\n");
	return out.length;
}

/* Appends a formatted line to the text, dropping what does not fit. */
static void
put(
	struct probe_text *text,
	const char *format,
	...)
{
	va_list arguments;
	size_t room;
	int written;

	/* Formats into the room left; a line that does not fit is dropped whole. */
	room = text->capacity - text->length;
	va_start(arguments, format);
	written = kern_vsnprintf(text->bytes + text->length, room, format, arguments);
	va_end(arguments);
	if (written < 0 || (size_t)written >= room) {
		text->bytes[text->length] = '\0';
		return;
	}

	/* Keeps the line. */
	text->length += (size_t)written;
}

/* Reads one stream, cuts it and decodes it in each of its ways. */
static void
probe_stream(
	struct drv_wave5 *core,
	const struct probe_stream *stream,
	struct probe_text *text)
{
	struct probe_run *run;
	char path[96];
	uint8_t *bytes;
	size_t size;
	unsigned way;
	int error;

	/* Reads the stream's file; an absent one is skipped. */
	kern_snprintf(path, sizeof(path), "%s%s", PROBE_DIRECTORY, stream->file);
	error = powervr_firmware_read(path, PROBE_STREAM_LIMIT, &bytes, &size);
	if (error != 0) {
		put(text, "stream %s absent %d\n", stream->name, error);
		return;
	}

	/* Allocates the run's state. */
	run = kern_calloc(1, sizeof(*run));
	if (run == NULL) {
		kern_free(bytes);
		put(text, "stream %s no memory\n", stream->name);
		return;
	}

	/* Cuts the stream into pictures. */
	run->stream = stream;
	run->bytes = bytes;
	run->unit_count = cut_units(bytes, size, stream->codec, run->units);
	put(text, "stream %s bytes=%u units=%u\n", stream->name, (unsigned)size, run->unit_count);

	/* Decodes it each way it asks for, stopping at a decode that left memory the core may use. */
	for (way = 0; way < RUN_COUNT; way++) {
		if ((stream->runs & (1U << way)) == 0)
			continue;
		run->way = 1U << way;
		error = decode_stream(core, run, text);
		put(text, "run %s %c end %d\n", stream->name, probe_way_names[way], error);
		if (error == EBUSY)
			break;
	}

	/* Frees the stream. */
	kern_free(run);
	kern_free(bytes);
}

/*
 * Decodes one stream one way.  EBUSY means the instance could not be
 * destroyed, so its memory was kept.
 */
static int
decode_stream(
	struct drv_wave5 *core,
	struct probe_run *run,
	struct probe_text *text)
{
	struct drv_wave5_picture linear[DRV_WAVE5_MAX_BUFFERS];
	const struct probe_unit *unit;
	unsigned taken;
	unsigned polled;
	uint32_t reasons;
	uint32_t flags;
	uint32_t hash;
	uint32_t pattern_hash;
	unsigned rewind;
	unsigned position;
	unsigned written;
	int error;
	int closed;

	/* Starts with a clean slate of buffers and use. */
	run->count = 0;
	run->instance = NULL;
	kern_memset(run->last_used, 0, sizeof(run->last_used));
	kern_memset(run->pool, 0, sizeof(run->pool));
	kern_memset(run->destinations, 0, sizeof(run->destinations));
	if (run->unit_count == 0)
		return EINVAL;

	/* Opens an instance and reports the reasons its creation raised. */
	drv_wave5_interrupts(core, &taken, &polled, &reasons);
	error = drv_wave5_open(core, run->stream->codec, PROBE_RING, &run->instance);
	drv_wave5_interrupts(core, &taken, &polled, &reasons);
	put(text, "open %s %d irq=%u polled=%u reasons=%08x\n", run->stream->name, error, taken, polled, reasons);
	if (error != 0)
		return error;

	/* Sets the sequence up from the first picture. */
	unit = &run->units[0];
	rewind = DRV_WAVE5_REWIND_QUERY;
	if (run->way == RUN_D)
		rewind = DRV_WAVE5_REWIND_REGISTER;
	error = drv_wave5_load(run->instance, run->bytes + unit->offset, unit->length, rewind);
	if (error == 0)
		error = drv_wave5_init_sequence(run->instance, &run->sequence);
	drv_wave5_interrupts(core, &taken, &polled, &reasons);
	put(text,
	    "sequence %s %d ok=%u err=%08x size=%ux%u required=%u crop=%u,%u,%u,%u depth=%u,%u profile=%u rd=%08x irq=%u polled=%u reasons=%08x\n",
	    run->stream->name,
	    error,
	    run->sequence.success,
	    run->sequence.error,
	    run->sequence.width,
	    run->sequence.height,
	    run->sequence.required,
	    run->sequence.crop_left,
	    run->sequence.crop_right,
	    run->sequence.crop_top,
	    run->sequence.crop_bottom,
	    run->sequence.luma_depth,
	    run->sequence.chroma_depth,
	    run->sequence.profile,
	    run->sequence.read_position,
	    taken,
	    polled,
	    reasons);

	/* Takes and registers one more frame buffer than the firmware asks for. */
	if (error == 0) {
		run->count = run->sequence.required + 1U;
		if (run->count > DRV_WAVE5_MAX_BUFFERS)
			error = ERANGE;
	}
	if (error == 0)
		error = take_buffers(run);
	if (error == 0) {
		for (position = 0; position < run->count; position++)
			linear[position] = run->pool[position].picture;
		error = drv_wave5_register(run->instance, linear, run->count, run->stride);
	}
	put(text, "register %s %d count=%u stride=%u\n", run->stream->name, error, run->count, run->stride);

	/* Decodes each picture, stopping at the first that fails. */
	for (position = 0; error == 0 && position < run->unit_count; position++)
		error = decode_unit(run, position, text);

	/* Reports, for C, how many pool buffers the core wrote although every picture went elsewhere. */
	if (run->way == RUN_C && run->count != 0) {
		written = 0;
		kern_memset(run->destinations[0].view, PROBE_POOL_FILL, run->buffer_size);
		pattern_hash = hash_picture(run, run->destinations[0].view);
		for (position = 0; position < run->count; position++) {
			hash = hash_picture(run, run->pool[position].view);
			if (hash != pattern_hash)
				written++;
		}
		put(text, "pool %s written=%u of %u\n", run->stream->name, written, run->count);
	}

	/* Reports the display flags at the end. */
	flags = 0;
	if (run->count != 0)
		(void)drv_wave5_display_flags(run->instance, 0, 0, &flags);
	put(text, "flags %s %08x\n", run->stream->name, flags);

	/* Destroys the instance; its memory is kept when the firmware would not let it go. */
	closed = drv_wave5_close(run->instance);
	run->instance = NULL;
	if (closed != 0) {
		put(text, "close %s %d, buffers kept\n", run->stream->name, closed);
		return EBUSY;
	}

	/* Gives the buffers back. */
	give_buffers(run);

	/* Reports the first failure. */
	if (error != 0)
		return error;

	/* Succeeded: every picture was decoded. */
	return 0;
}

/* Decodes one picture of a run and appends its line. */
static int
decode_unit(
	struct probe_run *run,
	unsigned unit,
	struct probe_text *text)
{
	struct drv_wave5_result result;
	const struct probe_unit *piece;
	struct probe_buffer *target;
	uint8_t *ring_view;
	uint64_t ring_address;
	uint8_t *direct;
	uint32_t mask;
	uint32_t hash;
	uint32_t cached_hash;
	uint32_t flags;
	unsigned rewind;
	unsigned chosen;
	int index;
	int error;

	/* Reads the ring's old bytes through the direct map first for E, so stale lines are cached. */
	piece = &run->units[unit];
	if (run->way == RUN_E) {
		drv_wave5_ring(run->instance, &ring_view, &ring_address);
		direct = kern_pmem_to_kernel((kern_physaddr_t)ring_address);
		if (direct != NULL) {
			drop_cached(direct, piece->length);
			touch_cached(direct, piece->length);
		}
	}

	/* Puts the picture in the ring. */
	rewind = DRV_WAVE5_REWIND_QUERY;
	if (run->way == RUN_D)
		rewind = DRV_WAVE5_REWIND_REGISTER;
	error = drv_wave5_load(run->instance, run->bytes + piece->offset, piece->length, rewind);
	if (error != 0) {
		put(text, "frame %s %c n=%u load %d\n", run->stream->name, probe_way_names[__builtin_ctz(run->way)], unit, error);
		return error;
	}

	/* For B, C and F picks the least recently used frame buffer and flags every other one. */
	chosen = 0;
	if (run->way == RUN_B || run->way == RUN_C || run->way == RUN_F) {
		chosen = least_used(run);
		mask = (uint32_t)((1ULL << run->count) - 1U);
		error = drv_wave5_display_flags(run->instance, mask & ~(1U << chosen), 1U << chosen, &flags);
		if (error != 0)
			return error;
	}

	/* For C points the picked pair's linear buffer at a destination, filled to show what is written. */
	if (run->way == RUN_C) {
		target = &run->destinations[unit & 1U];
		kern_memset(target->view, PROBE_DESTINATION_FILL, run->buffer_size);
		error = drv_wave5_replace(run->instance, chosen, &target->picture);
		if (error != 0) {
			put(text, "frame %s C n=%u replace %d\n", run->stream->name, unit, error);
			return error;
		}
	}

	/* For F fills the picked buffer and caches its filled lines through the direct map. */
	direct = NULL;
	if (run->way == RUN_F) {
		kern_memset(run->pool[chosen].view, PROBE_POOL_FILL, run->buffer_size);
		direct = kern_pmem_to_kernel(run->pool[chosen].run.paddr);
		if (direct != NULL) {
			drop_cached(direct, run->buffer_size);
			touch_cached(direct, run->buffer_size);
		}
	}

	/* Decodes the picture. */
	error = drv_wave5_decode(run->instance, &result);

	/* Hashes the picture given out: the core's choice for A, D and E, the host's otherwise. */
	index = result.display;
	if (run->way == RUN_B || run->way == RUN_C || run->way == RUN_F)
		index = result.decoded;
	hash = 0;
	cached_hash = 0;
	if (error == 0 && index >= 0 && (unsigned)index < run->count) {
		target = &run->pool[index];
		if (run->way == RUN_C)
			target = &run->destinations[unit & 1U];
		hash = hash_picture(run, target->view);
		if (run->way == RUN_F && direct != NULL && (unsigned)index == chosen) {
			cached_hash = hash_picture(run, direct);
			drop_cached(direct, run->buffer_size);
		}
	}

	/* Drops the ring's lines cached for E. */
	if (run->way == RUN_E) {
		direct = kern_pmem_to_kernel((kern_physaddr_t)ring_address);
		if (direct != NULL)
			drop_cached(direct, piece->length);
	}

	/* Appends the picture's line. */
	put(text,
	    "frame %s %c n=%u err=%d ok=%u fail=%08x warn=%08x dec=%d disp=%d pick=%u poc=%d type=%08x change=%08x flags=%08x hash=%08x cached=%08x ticks=%llu us=%llu rd=%08x size=%ux%u\n",
	    run->stream->name,
	    probe_way_names[__builtin_ctz(run->way)],
	    unit,
	    error,
	    result.success,
	    result.error,
	    result.warning,
	    (int)result.decoded,
	    (int)result.display,
	    chosen,
	    (int)result.order,
	    result.type,
	    result.notification,
	    result.display_flags,
	    hash,
	    cached_hash,
	    (unsigned long long)result.ticks,
	    (unsigned long long)result.wall,
	    result.read_position,
	    result.width,
	    result.height);
	if (error != 0)
		return error;

	/* For A, D and E hands the picture's frame buffer back to the core. */
	if ((run->way == RUN_A || run->way == RUN_D || run->way == RUN_E) && result.display >= 0) {
		error = drv_wave5_display_flags(run->instance, 0, 1U << (unsigned)result.display, &flags);
		if (error != 0)
			return error;
	}

	/* Remembers when the decoded buffer was last used. */
	if (result.decoded >= 0 && (unsigned)result.decoded < run->count)
		run->last_used[result.decoded] = unit + 1U;

	/* Succeeded: the picture was decoded. */
	return 0;
}

/*
 * Cuts an Annex B stream into pictures: each picture starts at the parameter
 * sets, delimiters or prefix messages before its first slice, or at that
 * slice.  Reports how many pictures there are.
 *
 * Procedure referenced:
 * ITU-T H.264 (08/2021) 7.4.1.2.3 (order of NAL units and access units:
 * the first slice's first_mb_in_slice 0); ITU-T H.265 (09/2023) 7.4.2.4.4
 * (the first slice segment's first_slice_segment_in_pic_flag).
 */
static unsigned
cut_units(
	const uint8_t *bytes,
	size_t size,
	unsigned codec,
	struct probe_unit *units)
{
	unsigned count;
	size_t at;
	size_t begin;
	size_t start;
	int has_picture;
	int picture;
	int boundary;

	/* Walks the start codes, closing a picture where the next one begins. */
	count = 0;
	begin = 0;
	has_picture = 0;
	for (at = 0; at + 3U < size; at++) {
		/* Looks only at start codes. */
		if (bytes[at] != 0 || bytes[at + 1U] != 0 || bytes[at + 2U] != 1)
			continue;

		/* A picture starts here when the unit begins a new one after a slice. */
		picture = 0;
		boundary = unit_starts(bytes, size, at + 3U, codec, has_picture, &picture);
		if (boundary && count < PROBE_MAX_UNITS) {
			start = at;
			if (start > begin && bytes[start - 1U] == 0)
				start--;
			units[count].offset = begin;
			units[count].length = start - begin;
			count++;
			begin = start;
			has_picture = 0;
		}
		if (picture)
			has_picture = 1;
		at += 2U;
	}

	/* The last picture runs to the end. */
	if (has_picture && count < PROBE_MAX_UNITS) {
		units[count].offset = begin;
		units[count].length = size - begin;
		count++;
	}

	/* Succeeded: the pictures found. */
	return count;
}

/*
 * Reports whether the NAL unit at a position begins a new picture after
 * one that already has a slice, and whether it is a slice itself.
 *
 * Values referenced:
 * ITU-T H.264 (08/2021) Table 7-1 (nal_unit_type 1 to 5 slices; 6, 7, 8,
 * 9 and 14 to 18 before the first slice); ITU-T H.265 (09/2023) Table 7-1
 * (nal_unit_type 0 to 31 slices; 32 to 35, 39, 41 to 44 and 48 to 55
 * before the first slice).
 */
static int
unit_starts(
	const uint8_t *bytes,
	size_t size,
	size_t at,
	unsigned codec,
	int has_picture,
	int *picture)
{
	unsigned type;
	int first;

	/* A unit too short to read is not a boundary. */
	if (at + 2U >= size)
		return 0;

	/* An H.264 slice begins a picture when its first macroblock is 0 (a leading 1 bit). */
	if (codec == DRV_WAVE5_H264) {
		type = bytes[at] & 0x1fU;
		if (type >= 1U && type <= 5U) {
			*picture = 1;
			first = (bytes[at + 1U] & 0x80U) != 0;
			if (first && has_picture)
				return 1;
			return 0;
		}
		if ((type >= 6U && type <= 9U) || (type >= 14U && type <= 18U)) {
			if (has_picture)
				return 1;
		}
		return 0;
	}

	/* An H.265 slice segment begins a picture when its first flag is set. */
	type = (bytes[at] >> 1) & 0x3fU;
	if (type <= 31U) {
		*picture = 1;
		first = (bytes[at + 2U] & 0x80U) != 0;
		if (first && has_picture)
			return 1;
		return 0;
	}
	if ((type >= 32U && type <= 35U) || type == 39U || (type >= 41U && type <= 44U) || (type >= 48U && type <= 55U)) {
		if (has_picture)
			return 1;
	}

	/* Any other unit belongs to the picture before it. */
	return 0;
}

/*
 * Takes the run's frame buffers, and for C its two destinations: NV12
 * pictures of the sequence's coded size, the stride 32-byte aligned and
 * the chroma plane on a page of its own, filled with the pool's byte.
 */
static int
take_buffers(
	struct probe_run *run)
{
	size_t luma;
	unsigned position;
	int error;

	/* Works out the stride and the planes. */
	run->stride = (run->sequence.width + 31U) & ~31U;
	luma = (size_t)run->stride * run->sequence.height;
	run->chroma_offset = (luma + 0xfffU) & ~(size_t)0xfffU;
	run->buffer_size = (run->chroma_offset + luma / 2U + 0xfffU) & ~(size_t)0xfffU;

	/* Takes the pool. */
	for (position = 0; position < run->count; position++) {
		error = take_buffer(run->buffer_size, run->chroma_offset, &run->pool[position]);
		if (error != 0) {
			give_buffers(run);
			return error;
		}
	}

	/* Takes the destinations for C. */
	if (run->way == RUN_C) {
		for (position = 0; position < 2U; position++) {
			error = take_buffer(run->buffer_size, run->chroma_offset, &run->destinations[position]);
			if (error != 0) {
				give_buffers(run);
				return error;
			}
		}
	}

	/* Succeeded: every buffer is filled and mapped. */
	return 0;
}

/* Gives the run's buffers back. */
static void
give_buffers(
	struct probe_run *run)
{
	unsigned position;

	/* Each buffer; one never taken is empty. */
	for (position = 0; position < DRV_WAVE5_MAX_BUFFERS; position++)
		give_buffer(&run->pool[position]);
	give_buffer(&run->destinations[0]);
	give_buffer(&run->destinations[1]);
}

/* Takes one picture buffer below the core's reach, mapped uncached and filled. */
static int
take_buffer(
	size_t size,
	size_t chroma_offset,
	struct probe_buffer *buffer)
{
	void *view;
	int error;

	/* Refuses an architecture without uncached views. */
	if (kern_pmem_map_uncached == NULL || kern_pmem_unmap_uncached == NULL)
		return ENOTSUP;

	/* Takes the run. */
	error = kern_pmem_alloc_limited(size, 0x1000U, PROBE_DMA_LIMIT, 0, &buffer->run);
	if (error != 0)
		return error;

	/* Maps it uncached. */
	error = kern_pmem_map_uncached(&buffer->run, &view);
	if (error != 0) {
		(void)kern_pmem_free(&buffer->run);
		return error;
	}

	/* Fills it and names its planes. */
	buffer->view = view;
	kern_memset(buffer->view, PROBE_POOL_FILL, size);
	buffer->picture.luma = buffer->run.paddr;
	buffer->picture.chroma = buffer->run.paddr + chroma_offset;

	/* Succeeded: the buffer is the run's. */
	return 0;
}

/* Gives one picture buffer back; an empty one is left alone. */
static void
give_buffer(
	struct probe_buffer *buffer)
{
	/* An empty buffer has nothing to give. */
	if (buffer->view == NULL)
		return;

	/* Removes the view, then frees the run. */
	(void)kern_pmem_unmap_uncached(buffer->view, buffer->run.size);
	(void)kern_pmem_free(&buffer->run);
	buffer->view = NULL;
}

/* Hashes the visible bytes of an NV12 picture: the luma rows, then the chroma rows. */
static uint32_t
hash_picture(
	const struct probe_run *run,
	const uint8_t *bytes)
{
	const volatile uint8_t *row;
	uint32_t hash;
	uint32_t line;
	uint32_t column;

	/* The luma plane's visible rows. */
	hash = PROBE_FNV_BASIS;
	for (line = 0; line < run->stream->height; line++) {
		row = bytes + (size_t)line * run->stride;
		for (column = 0; column < run->stream->width; column++) {
			hash ^= row[column];
			hash *= PROBE_FNV_PRIME;
		}
	}

	/* The interleaved chroma plane's visible rows. */
	for (line = 0; line < run->stream->height / 2U; line++) {
		row = bytes + run->chroma_offset + (size_t)line * run->stride;
		for (column = 0; column < run->stream->width; column++) {
			hash ^= row[column];
			hash *= PROBE_FNV_PRIME;
		}
	}

	/* Succeeded: the picture's hash. */
	return hash;
}

/* Reads a range through the direct map one cache line at a time, so its lines are cached. */
static void
touch_cached(
	const uint8_t *bytes,
	size_t size)
{
	const volatile uint8_t *line;
	size_t offset;
	uint8_t sum;

	/* One read per 64-byte line. */
	sum = 0;
	for (offset = 0; offset < size; offset += 64U) {
		line = bytes + offset;
		sum = (uint8_t)(sum + *line);
	}
	(void)sum;
}

/* Writes back and discards a direct-map range's cached lines. */
static void
drop_cached(
	const void *bytes,
	size_t size)
{
	/* Writes back, then discards. */
	kern_dcache_clean_range(bytes, size);
	kern_dcache_invalidate_range(bytes, size);
}

/* Picks the frame buffer decoded into longest ago, an unused one first. */
static unsigned
least_used(
	const struct probe_run *run)
{
	unsigned position;
	unsigned best;

	/* The lowest last use wins; ties go to the lower index. */
	best = 0;
	for (position = 1; position < run->count; position++) {
		if (run->last_used[position] < run->last_used[best])
			best = position;
	}

	/* Succeeded: the buffer to decode into. */
	return best;
}
