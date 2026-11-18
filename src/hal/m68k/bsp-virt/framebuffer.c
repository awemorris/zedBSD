/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The virt board's framebuffer.
 *
 * The board's display is a virtio GPU behind one of the virtio slots.  The
 * HAL gives it one two-dimensional resource whose backing store is a block
 * of guest memory, 640 by 400 pixels of 32 bits, so that 80 by 25 cells of
 * the 8x16 font fill it exactly, and shows the resource on the first
 * scanout.  The device copies changed rows from the block and shows them
 * when asked; the console asks after each change.
 *
 * The device is driven by polling through one control queue of a few
 * descriptors: each command is a request and a response, and the HAL
 * waits for the device to have used them.  The slot's registers and the
 * queue's structures are little-endian.
 *
 * Values referenced: OASIS Virtual I/O Device (VIRTIO) Version 1.2 (the
 * MMIO transport's registers and initialization, the split virtqueue's
 * layout, the GPU device's commands, structures and formats); QEMU
 * v10.0.0 hw/display/virtio-gpu.c (the pixel formats' byte order).
 */

#include <hal/hal.h>
#include "../m68040/asm.h"
#include "bsp.h"
#include "font.h"
#include "framebuffer.h"

/* The screen: its size in pixels, the bytes of a row, and of the whole. */
#define FRAMEBUFFER_WIDTH	640U
#define FRAMEBUFFER_HEIGHT	400U
#define FRAMEBUFFER_STRIDE	(FRAMEBUFFER_WIDTH * 4U)
#define FRAMEBUFFER_PIXELS	(FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT)

/* The virtio slots: how many, how far apart, and their registers. */
#define VIRTIO_SLOTS			128U
#define VIRTIO_SLOT_SIZE		0x200U
#define VIRTIO_MAGIC			0x000U
#define VIRTIO_VERSION			0x004U
#define VIRTIO_DEVICE_ID		0x008U
#define VIRTIO_DEVICE_FEATURES		0x010U
#define VIRTIO_DEVICE_FEATURES_SEL	0x014U
#define VIRTIO_DRIVER_FEATURES		0x020U
#define VIRTIO_DRIVER_FEATURES_SEL	0x024U
#define VIRTIO_QUEUE_SEL		0x030U
#define VIRTIO_QUEUE_NUM_MAX		0x034U
#define VIRTIO_QUEUE_NUM		0x038U
#define VIRTIO_QUEUE_READY		0x044U
#define VIRTIO_QUEUE_NOTIFY		0x050U
#define VIRTIO_STATUS			0x070U
#define VIRTIO_QUEUE_DESC_LOW		0x080U
#define VIRTIO_QUEUE_DESC_HIGH		0x084U
#define VIRTIO_QUEUE_DRIVER_LOW		0x090U
#define VIRTIO_QUEUE_DRIVER_HIGH	0x094U
#define VIRTIO_QUEUE_DEVICE_LOW		0x0a0U
#define VIRTIO_QUEUE_DEVICE_HIGH	0x0a4U

/* The slot's magic ("virt"), the transport version and the GPU's device id. */
#define VIRTIO_MAGIC_VALUE		0x74726976U
#define VIRTIO_VERSION_MODERN		2U
#define VIRTIO_ID_GPU			16U

/* The device status bits and the one feature the HAL asks for (bit 32). */
#define VIRTIO_STATUS_ACKNOWLEDGE	1U
#define VIRTIO_STATUS_DRIVER		2U
#define VIRTIO_STATUS_DRIVER_OK		4U
#define VIRTIO_STATUS_FEATURES_OK	8U
#define VIRTIO_F_VERSION_1_HIGH		1U

/* The descriptor flags. */
#define VIRTQ_DESC_F_NEXT		1U
#define VIRTQ_DESC_F_WRITE		2U

/* The control queue's size. */
#define QUEUE_SIZE			8U

/* The GPU's commands, its success response and the pixel format. */
#define GPU_CMD_RESOURCE_CREATE_2D	0x0101U
#define GPU_CMD_SET_SCANOUT		0x0103U
#define GPU_CMD_RESOURCE_FLUSH		0x0104U
#define GPU_CMD_TRANSFER_TO_HOST_2D	0x0105U
#define GPU_CMD_RESOURCE_ATTACH_BACKING	0x0106U
#define GPU_RESP_OK_NODATA		0x1100U
#define GPU_FORMAT_X8R8G8B8_UNORM	4U

/* The one resource the HAL makes. */
#define GPU_RESOURCE_ID			1U

/* How many times the completion of one command is looked for. */
#define GPU_POLLS			10000000U

/* The split virtqueue's structures. */
struct virtq_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};

struct virtq_avail {
	uint16_t flags;
	uint16_t idx;
	uint16_t ring[QUEUE_SIZE];
	uint16_t used_event;
};

struct virtq_used_elem {
	uint32_t id;
	uint32_t len;
};

struct virtq_used {
	uint16_t flags;
	uint16_t idx;
	struct virtq_used_elem ring[QUEUE_SIZE];
	uint16_t avail_event;
};

/* Every command's header, and the rectangle most commands carry. */
struct gpu_header {
	uint32_t type;
	uint32_t flags;
	uint64_t fence_id;
	uint32_t ctx_id;
	uint8_t ring_idx;
	uint8_t padding[3];
};

struct gpu_rect {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
};

/* The largest command: the header and 32 bytes after it. */
struct gpu_command {
	struct gpu_header header;
	uint32_t words[8];
};

_Static_assert(sizeof(struct virtq_desc) == 16U, "a descriptor is 16 bytes");
_Static_assert(sizeof(struct gpu_header) == 24U, "a GPU header is 24 bytes");

/*
 * The queue's structures, the command and its response, in the image so
 * that their addresses are fixed.
 */
static struct virtq_desc queue_desc[QUEUE_SIZE] __attribute__((aligned(16)));
static struct virtq_avail queue_avail __attribute__((aligned(4)));
static struct virtq_used queue_used __attribute__((aligned(4)));
static struct gpu_command gpu_request __attribute__((aligned(16)));
static struct gpu_header gpu_response __attribute__((aligned(16)));

/* The device's slot, and the next index the HAL gives the queue. */
static uintptr_t gpu_slot;
static uint16_t queue_next;

/*
 * The screen's memory, the resource's backing store.  The kernel's own
 * console draws into it after the HAL's early console.
 */
static uint32_t framebuffer_pixels[FRAMEBUFFER_PIXELS] __attribute__((aligned(4096)));

/* The rows drawn since the last flush, as a half-open range; empty when equal. */
static unsigned dirty_first;
static unsigned dirty_end;

/*
 * Nonzero once the device shows the memory; until then nothing is drawn.
 */
static int framebuffer_on;

/*
 * The colours of the sixteen text attributes, as XRGB8888 values.
 *
 * Values referenced: the IBM VGA text-mode palette, the sixteen default
 * attribute colours.
 */
static const uint32_t framebuffer_palette[16] = {
	0x000000U, 0x0000aaU, 0x00aa00U, 0x00aaaaU,
	0xaa0000U, 0xaa00aaU, 0xaa5500U, 0xaaaaaaU,
	0x555555U, 0x5555ffU, 0x55ff55U, 0x55ffffU,
	0xff5555U, 0xff55ffU, 0xffff55U, 0xffffffU
};

static uint32_t slot_read(uint32_t offset);
static void slot_write(uint32_t offset, uint32_t value);
static int find_gpu(void);
static int start_queue(void);
static int gpu_submit(uint32_t type, const uint32_t *words, unsigned count);
static void mark_dirty(unsigned first, unsigned end);

/*
 * Finds the GPU, makes the resource on the screen's memory and shows it.
 *
 * Returns HAL_OK once the device shows it, or why it does not.
 */
int
virt_framebuffer_init(
	void)
{
	uint32_t words[8];
	int error;

	/* The device, its queue, and a black screen. */
	error = find_gpu();
	if (error != HAL_OK)
		return error;
	error = start_queue();
	if (error != HAL_OK)
		return error;
	hal_memset(framebuffer_pixels, 0, sizeof(framebuffer_pixels));

	/* The resource: its id, format and size. */
	words[0] = __builtin_bswap32(GPU_RESOURCE_ID);
	words[1] = __builtin_bswap32(GPU_FORMAT_X8R8G8B8_UNORM);
	words[2] = __builtin_bswap32(FRAMEBUFFER_WIDTH);
	words[3] = __builtin_bswap32(FRAMEBUFFER_HEIGHT);
	error = gpu_submit(GPU_CMD_RESOURCE_CREATE_2D, words, 4U);
	if (error != HAL_OK)
		return error;

	/* Its backing store: one entry, the screen's memory (a 64-bit address, the length, padding). */
	words[0] = __builtin_bswap32(GPU_RESOURCE_ID);
	words[1] = __builtin_bswap32(1U);
	words[2] = __builtin_bswap32((uint32_t)(uintptr_t)framebuffer_pixels);
	words[3] = 0;
	words[4] = __builtin_bswap32((uint32_t)sizeof(framebuffer_pixels));
	words[5] = 0;
	error = gpu_submit(GPU_CMD_RESOURCE_ATTACH_BACKING, words, 6U);
	if (error != HAL_OK)
		return error;

	/* The first scanout shows the whole resource. */
	words[0] = 0;
	words[1] = 0;
	words[2] = __builtin_bswap32(FRAMEBUFFER_WIDTH);
	words[3] = __builtin_bswap32(FRAMEBUFFER_HEIGHT);
	words[4] = 0;
	words[5] = __builtin_bswap32(GPU_RESOURCE_ID);
	error = gpu_submit(GPU_CMD_SET_SCANOUT, words, 6U);
	if (error != HAL_OK)
		return error;

	/* Succeeded: drawing reaches the screen at the next flush. */
	framebuffer_on = 1;
	mark_dirty(0, FRAMEBUFFER_HEIGHT);
	virt_framebuffer_flush();
	return HAL_OK;
}

/*
 * Sends the rows drawn since the last flush to the device and shows them.
 */
void
virt_framebuffer_flush(
	void)
{
	uint32_t words[8];
	uint32_t offset;
	unsigned height;

	/* Nothing to send before the screen is shown, or when nothing changed. */
	if (!framebuffer_on || dirty_first >= dirty_end)
		return;
	height = dirty_end - dirty_first;
	offset = dirty_first * FRAMEBUFFER_STRIDE;

	/* Copies the rows: the rectangle, the offset in the store (64 bits), the resource. */
	words[0] = 0;
	words[1] = __builtin_bswap32(dirty_first);
	words[2] = __builtin_bswap32(FRAMEBUFFER_WIDTH);
	words[3] = __builtin_bswap32(height);
	words[4] = __builtin_bswap32(offset);
	words[5] = 0;
	words[6] = __builtin_bswap32(GPU_RESOURCE_ID);
	words[7] = 0;
	(void)gpu_submit(GPU_CMD_TRANSFER_TO_HOST_2D, words, 8U);

	/* Shows them: the rectangle, the resource. */
	words[4] = words[6];
	words[5] = 0;
	(void)gpu_submit(GPU_CMD_RESOURCE_FLUSH, words, 6U);

	/* The rows are on the screen. */
	dirty_first = 0;
	dirty_end = 0;
}

/*
 * Reports whether the screen is shown.
 */
int
virt_framebuffer_ready(
	void)
{
	/* Succeeded: whether virt_framebuffer_init() succeeded. */
	return framebuffer_on;
}

/*
 * Draws one text cell: a glyph in the attribute's colours.
 */
void
virt_framebuffer_cell(
	unsigned row,
	unsigned column,
	int character,
	uint8_t attribute)
{
	const uint8_t *glyph;
	uint32_t foreground;
	uint32_t background;
	uint32_t value;
	unsigned line;
	unsigned bit;
	uint8_t bits;
	size_t first;

	/* Nothing to draw before the screen is shown, or off the grid. */
	if (!framebuffer_on)
		return;
	if (row >= VIRT_CONSOLE_ROWS || column >= VIRT_CONSOLE_COLUMNS)
		return;

	/* The glyph of the character (the font covers every byte value) and its colours. */
	glyph = &virt_font8x16[((unsigned)character & 0xffU) * VIRT_FONT_HEIGHT];
	foreground = framebuffer_palette[attribute & 15U];
	background = framebuffer_palette[(attribute >> 4) & 15U];
	first = (size_t)row * VIRT_FONT_HEIGHT * FRAMEBUFFER_WIDTH + (size_t)column * 8U;
	mark_dirty(row * VIRT_FONT_HEIGHT, (row + 1U) * VIRT_FONT_HEIGHT);

	/* Each row of the glyph, from its leftmost pixel in the high bit. */
	for (line = 0; line < VIRT_FONT_HEIGHT; line++) {
		bits = glyph[line];
		for (bit = 0; bit < 8U; bit++) {
			value = background;
			if ((bits & (0x80U >> bit)) != 0)
				value = foreground;
			framebuffer_pixels[first + (size_t)line * FRAMEBUFFER_WIDTH + bit] = value;
		}
	}
}

/*
 * Draws the cursor: the two bottom pixel rows of a cell in white.
 */
void
virt_framebuffer_cursor(
	unsigned row,
	unsigned column)
{
	unsigned line;
	unsigned bit;
	size_t first;

	/* Nothing to draw before the screen is shown, or off the grid. */
	if (!framebuffer_on)
		return;
	if (row >= VIRT_CONSOLE_ROWS || column >= VIRT_CONSOLE_COLUMNS)
		return;

	/* Whitens rows 14 and 15 of the cell. */
	first = (size_t)row * VIRT_FONT_HEIGHT * FRAMEBUFFER_WIDTH + (size_t)column * 8U;
	mark_dirty(row * VIRT_FONT_HEIGHT, (row + 1U) * VIRT_FONT_HEIGHT);
	for (line = VIRT_FONT_HEIGHT - 2U; line < VIRT_FONT_HEIGHT; line++) {
		for (bit = 0; bit < 8U; bit++)
			framebuffer_pixels[first + (size_t)line * FRAMEBUFFER_WIDTH + bit] = framebuffer_palette[15];
	}
}

/*
 * Writes the screen's address and the device's slot to the boot log.
 */
void
virt_framebuffer_describe(
	void)
{
	/* Describes a shown screen; a missing one was reported when it was looked for. */
	if (!framebuffer_on)
		return;
	hal_printf("M68K VIRT FRAMEBUFFER %ux%u phys=%x gpu=%x\n",
		   FRAMEBUFFER_WIDTH,
		   FRAMEBUFFER_HEIGHT,
		   (uint32_t)(uintptr_t)framebuffer_pixels,
		   (uint32_t)gpu_slot);
}

/* Reads a little-endian register of the device's slot. */
static uint32_t
slot_read(
	uint32_t offset)
{
	uint32_t value;

	/* The processor reads the bytes big-endian. */
	value = *(volatile uint32_t *)(gpu_slot + offset);
	return __builtin_bswap32(value);
}

/* Writes a little-endian register of the device's slot. */
static void
slot_write(
	uint32_t offset,
	uint32_t value)
{
	/* The processor writes the bytes big-endian. */
	*(volatile uint32_t *)(gpu_slot + offset) = __builtin_bswap32(value);
}

/* Finds the first virtio slot that holds a GPU of the modern transport. */
static int
find_gpu(
	void)
{
	uintptr_t base;
	unsigned slot;
	uint32_t value;

	/* The board must name its slots. */
	base = virt_board()->virtio_base;
	if (base == 0)
		return HAL_ERR_UNSUPPORTED;

	/* Looks at each slot in turn. */
	for (slot = 0; slot < VIRTIO_SLOTS; slot++) {
		gpu_slot = base + slot * VIRTIO_SLOT_SIZE;
		value = slot_read(VIRTIO_MAGIC);
		if (value != VIRTIO_MAGIC_VALUE)
			continue;
		value = slot_read(VIRTIO_VERSION);
		if (value != VIRTIO_VERSION_MODERN)
			continue;
		value = slot_read(VIRTIO_DEVICE_ID);
		if (value == VIRTIO_ID_GPU)
			return HAL_OK;
	}

	/* Failed: the machine has no GPU. */
	gpu_slot = 0;
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Resets the device, agrees on the version-1 feature alone and sets up
 * the control queue.
 */
static int
start_queue(
	void)
{
	uint32_t features;
	uint32_t status;

	/* Resets, then acknowledges the device as a driver. */
	slot_write(VIRTIO_STATUS, 0);
	status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
	slot_write(VIRTIO_STATUS, status);

	/* Requires the version-1 feature and accepts nothing else. */
	slot_write(VIRTIO_DEVICE_FEATURES_SEL, 1U);
	features = slot_read(VIRTIO_DEVICE_FEATURES);
	if ((features & VIRTIO_F_VERSION_1_HIGH) == 0)
		return HAL_ERR_UNSUPPORTED;
	slot_write(VIRTIO_DRIVER_FEATURES_SEL, 0);
	slot_write(VIRTIO_DRIVER_FEATURES, 0);
	slot_write(VIRTIO_DRIVER_FEATURES_SEL, 1U);
	slot_write(VIRTIO_DRIVER_FEATURES, VIRTIO_F_VERSION_1_HIGH);
	status |= VIRTIO_STATUS_FEATURES_OK;
	slot_write(VIRTIO_STATUS, status);
	features = slot_read(VIRTIO_STATUS);
	if ((features & VIRTIO_STATUS_FEATURES_OK) == 0)
		return HAL_ERR_UNSUPPORTED;

	/* The control queue: its size and the three structures. */
	slot_write(VIRTIO_QUEUE_SEL, 0);
	features = slot_read(VIRTIO_QUEUE_NUM_MAX);
	if (features < QUEUE_SIZE)
		return HAL_ERR_UNSUPPORTED;
	hal_memset(queue_desc, 0, sizeof(queue_desc));
	hal_memset(&queue_avail, 0, sizeof(queue_avail));
	hal_memset(&queue_used, 0, sizeof(queue_used));
	queue_next = 0;
	slot_write(VIRTIO_QUEUE_NUM, QUEUE_SIZE);
	slot_write(VIRTIO_QUEUE_DESC_LOW, (uint32_t)(uintptr_t)queue_desc);
	slot_write(VIRTIO_QUEUE_DESC_HIGH, 0);
	slot_write(VIRTIO_QUEUE_DRIVER_LOW, (uint32_t)(uintptr_t)&queue_avail);
	slot_write(VIRTIO_QUEUE_DRIVER_HIGH, 0);
	slot_write(VIRTIO_QUEUE_DEVICE_LOW, (uint32_t)(uintptr_t)&queue_used);
	slot_write(VIRTIO_QUEUE_DEVICE_HIGH, 0);
	slot_write(VIRTIO_QUEUE_READY, 1U);

	/* The driver is ready. */
	status |= VIRTIO_STATUS_DRIVER_OK;
	slot_write(VIRTIO_STATUS, status);

	/* Succeeded: commands can be sent. */
	return HAL_OK;
}

/*
 * Sends one command (its type and the little-endian words after the
 * header) and waits for the device's response.
 */
static int
gpu_submit(
	uint32_t type,
	const uint32_t *words,
	unsigned count)
{
	uint32_t polls;
	uint16_t used;
	unsigned index;

	/* The request: the header and the words. */
	hal_memset(&gpu_request, 0, sizeof(gpu_request));
	hal_memset(&gpu_response, 0, sizeof(gpu_response));
	gpu_request.header.type = __builtin_bswap32(type);
	for (index = 0; index < count; index++)
		gpu_request.words[index] = words[index];

	/* Two descriptors: the request the device reads, the response it writes. */
	queue_desc[0].addr = __builtin_bswap64((uint64_t)(uintptr_t)&gpu_request);
	queue_desc[0].len = __builtin_bswap32((uint32_t)(sizeof(gpu_request.header) + count * 4U));
	queue_desc[0].flags = __builtin_bswap16(VIRTQ_DESC_F_NEXT);
	queue_desc[0].next = __builtin_bswap16(1U);
	queue_desc[1].addr = __builtin_bswap64((uint64_t)(uintptr_t)&gpu_response);
	queue_desc[1].len = __builtin_bswap32((uint32_t)sizeof(gpu_response));
	queue_desc[1].flags = __builtin_bswap16(VIRTQ_DESC_F_WRITE);
	queue_desc[1].next = 0;

	/* Offers the chain and tells the device. */
	queue_avail.ring[queue_next % QUEUE_SIZE] = 0;
	hal_mb();
	queue_next++;
	queue_avail.idx = __builtin_bswap16(queue_next);
	hal_mb();
	slot_write(VIRTIO_QUEUE_NOTIFY, 0);

	/* Waits until the device has used the chain. */
	for (polls = 0; polls < GPU_POLLS; polls++) {
		used = __builtin_bswap16(*(volatile uint16_t *)&queue_used.idx);
		if (used == queue_next)
			break;
	}

	/* A device that never answered is reported. */
	if (polls == GPU_POLLS)
		return HAL_ERR_TIMEOUT;

	/* Succeeded only when the device said so. */
	hal_mb();
	if (__builtin_bswap32(gpu_response.type) != GPU_RESP_OK_NODATA)
		return HAL_ERR_IO;
	return HAL_OK;
}

/* Widens the range of rows to send at the next flush. */
static void
mark_dirty(
	unsigned first,
	unsigned end)
{
	/* An empty range takes the rows as they are; another one grows. */
	if (dirty_first >= dirty_end) {
		dirty_first = first;
		dirty_end = end;
		return;
	}

	/* Otherwise the range grows to take the rows in. */
	if (first < dirty_first)
		dirty_first = first;
	if (end > dirty_end)
		dirty_end = end;
}
