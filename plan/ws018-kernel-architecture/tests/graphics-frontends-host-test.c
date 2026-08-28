/* WS018 KA-T080: shared black-box contract for independent graphics devices.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/cdev.h"
#include "kern/graphics-device.h"
#include "kern/lock.h"
#include "kern/uaccess.h"

#if defined(GRAPHICS_TEST_PCAT)
#include "drivers/graphics/pcat/backend.h"
#define TEST_NAME "PC/AT"
#define backend_ready_fn pcat_graphics_backend_ready
#define backend_get_modes_fn pcat_graphics_backend_get_modes
#define backend_enter_fn pcat_graphics_backend_enter
#define backend_leave_fn pcat_graphics_backend_leave
#define backend_fill_fn pcat_graphics_backend_fill
#define backend_line_fn pcat_graphics_backend_line
#define backend_pattern_fill_fn pcat_graphics_backend_pattern_fill
#define backend_blit_fn pcat_graphics_backend_blit
#define backend_flush_fn pcat_graphics_backend_flush
#define backend_get_glyph_fn pcat_graphics_backend_get_glyph
#define backend_image pcat_graphics_image
#elif defined(GRAPHICS_TEST_PC98)
#include "drivers/graphics/pc98/backend.h"
#define TEST_NAME "PC-98"
#define backend_ready_fn pc98_graphics_backend_ready
#define backend_get_modes_fn pc98_graphics_backend_get_modes
#define backend_enter_fn pc98_graphics_backend_enter
#define backend_leave_fn pc98_graphics_backend_leave
#define backend_fill_fn pc98_graphics_backend_fill
#define backend_line_fn pc98_graphics_backend_line
#define backend_pattern_fill_fn pc98_graphics_backend_pattern_fill
#define backend_blit_fn pc98_graphics_backend_blit
#define backend_flush_fn pc98_graphics_backend_flush
#define backend_get_glyph_fn pc98_graphics_backend_get_glyph
#define backend_image pc98_graphics_image
#else
#error select one graphics frontend
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zedbsd/graphics.h>

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "%s:%d: %s: check failed: %s\n", __FILE__, \
		    __LINE__, TEST_NAME, #condition); \
		exit(1); \
	} \
} while (0)

struct user_region {
	uintptr_t address;
	uint8_t *memory;
	size_t size;
};

static struct user_region user_regions[16];
static unsigned user_region_count;
static uintptr_t next_user_address;
static uintptr_t fail_copyin_address;
static uintptr_t fail_copyout_address;
static unsigned lock_depth;
static unsigned suspend_count, resume_count;
static const struct cdev_ops *device_ops;

static int backend_ready;
static int backend_enter_succeeds = 1;
static int backend_render_succeeds = 1;
static unsigned enter_count, leave_count, fill_count, line_count;
static unsigned pattern_count, blit_count, flush_count, glyph_count;
static uint8_t blit_first_pixel[8];
static unsigned blit_format, blit_palette_size;
static uint32_t blit_palette[2];

static void
require_device_lock(void)
{
	CHECK(lock_depth == 1U);
}

static void
user_reset(void)
{
	user_region_count = 0;
	next_user_address = 0x10000U;
	fail_copyin_address = 0;
	fail_copyout_address = 0;
}

static uintptr_t
user_map(void *memory, size_t size)
{
	struct user_region *region;
	uintptr_t address;

	CHECK(user_region_count < ARRAY_COUNT(user_regions));
	address = next_user_address;
	next_user_address += (size + 0xffU) & ~(uintptr_t)0xffU;
	region = &user_regions[user_region_count++];
	region->address = address;
	region->memory = memory;
	region->size = size;
	return address;
}

static void *
user_lookup(uintptr_t address, size_t size)
{
	unsigned i;

	for (i = 0; i < user_region_count; i++) {
		struct user_region *region = &user_regions[i];
		if (address >= region->address && size <= region->size &&
		    address - region->address <= region->size - size)
			return region->memory + (address - region->address);
	}
	return NULL;
}

int
copyin(uintptr_t source, void *destination, size_t size)
{
	void *memory;

	require_device_lock();
	if (source == fail_copyin_address)
		return EFAULT;
	memory = user_lookup(source, size);
	if (memory == NULL)
		return EFAULT;
	memcpy(destination, memory, size);
	return 0;
}

int
copyout(const void *source, uintptr_t destination, size_t size)
{
	void *memory;

	require_device_lock();
	if (destination == fail_copyout_address)
		return EFAULT;
	memory = user_lookup(destination, size);
	if (memory == NULL)
		return EFAULT;
	memcpy(memory, source, size);
	return 0;
}

int
mutex_init(struct mutex *mutex, enum lock_rank rank, const char *name)
{
	(void)mutex;
	(void)rank;
	(void)name;
	return 0;
}

void
mutex_lock(struct mutex *mutex)
{
	(void)mutex;
	CHECK(lock_depth == 0U);
	lock_depth = 1U;
}

void
mutex_unlock(struct mutex *mutex)
{
	(void)mutex;
	CHECK(lock_depth == 1U);
	lock_depth = 0U;
}

void
hal_cons_suspend(void)
{
	require_device_lock();
	suspend_count++;
}

void
hal_cons_resume(void)
{
	if (lock_depth != 0U)
		require_device_lock();
	resume_count++;
}

int
cdev_register(const char *name, dev_t rdev, const struct cdev_ops *ops,
	void *data)
{
	CHECK(lock_depth == 0U);
	CHECK(!strcmp(name, "graphics"));
	CHECK(rdev == (dev_t)0x00010001U);
	CHECK(ops != NULL);
	CHECK(data == NULL);
	device_ops = ops;
	return 0;
}

int
backend_ready_fn(void)
{
	require_device_lock();
	return backend_ready;
}

size_t
backend_get_modes_fn(struct graphics_mode_info *modes, size_t capacity)
{
	static const struct graphics_mode_info available[] = {
		{ 800U, 600U, 32U, 3200U },
		{ 640U, 768U, 8U, 640U },
	};
	size_t i;

	require_device_lock();
	for (i = 0; modes != NULL && i < capacity && i < ARRAY_COUNT(available);
	     i++)
		modes[i] = available[i];
	return ARRAY_COUNT(available);
}

int
backend_enter_fn(struct graphics_mode *mode)
{
	require_device_lock();
	enter_count++;
	if (!backend_enter_succeeds)
		return 0;
	mode->width = 640U;
	mode->height = 480U;
	mode->bits_per_pixel = 8U;
	mode->stride = 640U;
	return 1;
}

void
backend_leave_fn(void)
{
	require_device_lock();
	leave_count++;
}

int
backend_fill_fn(const struct graphics_rect *rect, uint32_t color)
{
	(void)color;
	require_device_lock();
	CHECK(rect != NULL);
	fill_count++;
	return backend_render_succeeds;
}

int
backend_line_fn(unsigned x0, unsigned y0, unsigned x1, unsigned y1,
	uint32_t color)
{
	(void)x0; (void)y0; (void)x1; (void)y1; (void)color;
	require_device_lock();
	line_count++;
	return backend_render_succeeds;
}

int
backend_pattern_fill_fn(const struct graphics_rect *rect, uint32_t color,
	uint64_t pattern)
{
	(void)color; (void)pattern;
	require_device_lock();
	CHECK(rect != NULL);
	pattern_count++;
	return backend_render_succeeds;
}

int
backend_blit_fn(unsigned x, unsigned y, const struct backend_image *image,
	uint64_t pattern, int patterned)
{
	(void)x; (void)y; (void)pattern; (void)patterned;
	require_device_lock();
	CHECK(image != NULL && image->height == 1U);
	if (blit_count < ARRAY_COUNT(blit_first_pixel))
		blit_first_pixel[blit_count] = image->pixels[0];
	blit_format = image->format;
	blit_palette_size = image->palette_size;
	if (image->palette_size != 0U) {
		blit_palette[0] = image->palette[0];
		if (image->palette_size > 1U)
			blit_palette[1] = image->palette[1];
	}
	blit_count++;
	return backend_render_succeeds;
}

int
backend_flush_fn(const struct graphics_rect *rectangles, size_t count)
{
	(void)rectangles;
	(void)count;
	require_device_lock();
	flush_count++;
	return backend_render_succeeds;
}

int
backend_get_glyph_fn(uint32_t codepoint, uint8_t bitmap[32], unsigned *width,
	unsigned *height)
{
	unsigned i;

	require_device_lock();
	glyph_count++;
	if (codepoint >= 0xd800U && codepoint <= 0xdfffU)
		return 0;
	for (i = 0; i < 32U; i++)
		bitmap[i] = (uint8_t)(i + 1U);
	*width = 8U;
	*height = 16U;
	return 1;
}

static int
device_ioctl(struct file *file, unsigned long request, void *argument,
	size_t size)
{
	uintptr_t address = user_map(argument, size);
	return device_ops->ioctl(file, request, address);
}

static void
enter_graphics(struct file *owner)
{
	struct graphics_mode mode;

	user_reset();
	memset(&mode, 0, sizeof(mode));
	mode.preferred_width = 640U;
	mode.preferred_height = 480U;
	mode.preferred_bits_per_pixel = 8U;
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_ENTER, &mode, sizeof(mode)) == 0);
	CHECK(mode.width == 640U && mode.height == 480U);
	CHECK(mode.bits_per_pixel == 8U && mode.stride == 640U);
	CHECK(mode.capabilities == 0xffU);
}

static void
test_open_caps_modes(struct file *owner, struct file *other)
{
	struct graphics_caps caps;
	struct graphics_mode_info modes[1];
	struct graphics_mode_list list;
	uintptr_t modes_address;

	backend_ready = 0;
	CHECK(graphics_device_register() == 0 && device_ops != NULL);
	CHECK(device_ops->open(owner) == ENODEV);
	backend_ready = 1;
	CHECK(device_ops->open(owner) == 0);
	CHECK(device_ops->open(other) == EBUSY);

	user_reset();
	memset(&caps, 0, sizeof(caps));
	CHECK(device_ioctl(other, ZEDBSD_GRAPHICS_GET_CAPS, &caps,
	    sizeof(caps)) == EBADF);
	user_reset();
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_GET_MODE, &caps,
	    sizeof(caps)) == ENXIO);
	user_reset();
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_GET_CAPS, &caps,
	    sizeof(caps)) == 0);
	CHECK(caps.capabilities == 0xffU);
	CHECK(caps.maximum_width == 800U && caps.maximum_height == 768U);

	user_reset();
	memset(&list, 0, sizeof(list));
	list.capacity = 17U;
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_GET_MODES, &list,
	    sizeof(list)) == EINVAL);
	user_reset();
	memset(&list, 0, sizeof(list));
	list.capacity = 1U;
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_GET_MODES, &list,
	    sizeof(list)) == EINVAL);
	user_reset();
	memset(modes, 0, sizeof(modes));
	modes_address = user_map(modes, sizeof(modes));
	memset(&list, 0, sizeof(list));
	list.modes = (uapi_ptr_t)modes_address;
	list.capacity = 1U;
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_GET_MODES, &list,
	    sizeof(list)) == 0);
	CHECK(list.count == 2U && modes[0].width == 800U);
}

static void
test_enter_and_render(struct file *owner)
{
	struct graphics_mode mode;
	struct graphics_fill fill;
	struct graphics_line line;
	struct graphics_pattern_fill pattern;
	struct graphics_blit blit;
	struct graphics_flush flush;
	struct graphics_rect rectangles[2];
	struct graphics_glyph glyph;
	uint32_t palette[2] = { 0x112233U, 0xaabbccU };
	uint8_t pixels[4] = { 0U, 1U, 1U, 0U };
	uint8_t rgb_pixels[6] = { 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U };
	uint8_t mono = 0x80U;
	uint8_t bitmap[32];
	uintptr_t request_address, pixel_address, palette_address, bitmap_address;

	user_reset();
	memset(&mode, 0, sizeof(mode));
	mode.preferred_width = 640U;
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_ENTER, &mode,
	    sizeof(mode)) == EINVAL);
	user_reset();
	memset(&mode, 0, sizeof(mode));
	mode.preferred_bits_per_pixel = 16U;
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_ENTER, &mode,
	    sizeof(mode)) == EINVAL);

	backend_enter_succeeds = 0;
	user_reset();
	memset(&mode, 0, sizeof(mode));
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_ENTER, &mode,
	    sizeof(mode)) == ENODEV);
	CHECK(suspend_count == 1U && resume_count == 2U && leave_count == 1U);
	backend_enter_succeeds = 1;
	enter_graphics(owner);
	CHECK(suspend_count == 2U && enter_count == 2U);
	user_reset();
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_ENTER, &mode,
	    sizeof(mode)) == EINVAL);
	user_reset();
	memset(&mode, 0, sizeof(mode));
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_GET_MODE, &mode,
	    sizeof(mode)) == 0 && mode.width == 640U);

	user_reset();
	memset(&fill, 0, sizeof(fill));
	fill.rect = (struct graphics_rect){ 10U, 20U, 30U, 40U };
	fill.color = 0x123456U;
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_FILL_RECT, &fill,
	    sizeof(fill)) == 0 && fill_count == 1U);
	fill.rect.width = 0;
	user_reset();
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_FILL_RECT, &fill,
	    sizeof(fill)) == EINVAL);
	fill.rect = (struct graphics_rect){ 630U, 0U, 20U, 1U };
	user_reset();
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_FILL_RECT, &fill,
	    sizeof(fill)) == EINVAL);

	user_reset();
	memset(&line, 0, sizeof(line));
	line.x1 = 639U; line.y1 = 479U;
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_DRAW_LINE, &line,
	    sizeof(line)) == 0 && line_count == 1U);
	line.x1 = 640U;
	user_reset();
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_DRAW_LINE, &line,
	    sizeof(line)) == EINVAL);
	user_reset();
	memset(&pattern, 0, sizeof(pattern));
	pattern.rect = (struct graphics_rect){ 0U, 0U, 8U, 8U };
	pattern.pattern = UINT64_MAX;
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_PATTERN_FILL, &pattern,
	    sizeof(pattern)) == 0 && pattern_count == 1U);

	user_reset();
	pixel_address = user_map(pixels, sizeof(pixels));
	palette_address = user_map(palette, sizeof(palette));
	memset(&blit, 0, sizeof(blit));
	blit.width = 2U; blit.height = 2U; blit.stride = 2U;
	blit.format = ZEDBSD_GRAPHICS_FORMAT_INDEX8;
	blit.pixels = (uapi_ptr_t)pixel_address;
	blit.palette = (uapi_ptr_t)palette_address;
	blit.palette_count = 2U;
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_BLIT, &blit,
	    sizeof(blit)) == 0);
	CHECK(blit_count == 2U && blit_first_pixel[0] == 0U &&
	    blit_first_pixel[1] == 1U);
	CHECK(blit_format == ZEDBSD_GRAPHICS_FORMAT_INDEX8 &&
	    blit_palette_size == 2U && blit_palette[1] == palette[1]);

	user_reset();
	pixel_address = user_map(rgb_pixels, sizeof(rgb_pixels));
	memset(&blit, 0, sizeof(blit));
	blit.width = 2U; blit.height = 1U; blit.stride = 6U;
	blit.format = ZEDBSD_GRAPHICS_FORMAT_RGB24;
	blit.pixels = (uapi_ptr_t)pixel_address;
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_BLIT, &blit,
	    sizeof(blit)) == 0);
	CHECK(blit_count == 3U && blit_first_pixel[2] == rgb_pixels[0] &&
	    blit_format == ZEDBSD_GRAPHICS_FORMAT_RGB24 &&
	    blit_palette_size == 0U);
	blit.stride = 5U;
	user_reset();
	blit.pixels = (uapi_ptr_t)user_map(rgb_pixels, sizeof(rgb_pixels));
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_BLIT, &blit,
	    sizeof(blit)) == EINVAL);
	blit.stride = 6U;
	blit.palette = (uapi_ptr_t)user_map(palette, sizeof(palette));
	blit.palette_count = 2U;
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_BLIT, &blit,
	    sizeof(blit)) == EINVAL);

	user_reset();
	pixel_address = user_map(&mono, sizeof(mono));
	memset(&blit, 0, sizeof(blit));
	blit.width = 8U; blit.height = 1U; blit.stride = 1U;
	blit.format = ZEDBSD_GRAPHICS_FORMAT_MONO1;
	blit.pixels = (uapi_ptr_t)pixel_address;
	blit.foreground = 0xffffffU; blit.background = 0x010203U;
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_BLIT_PATTERN, &blit,
	    sizeof(blit)) == 0);
	CHECK(blit_count == 4U && blit_first_pixel[3] == 1U &&
	    blit_palette[0] == 0x010203U && blit_palette[1] == 0xffffffU);
	blit.width = 1025U; blit.stride = 129U;
	user_reset();
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_BLIT, &blit,
	    sizeof(blit)) == EINVAL);

	user_reset();
	pixel_address = user_map(pixels, sizeof(pixels));
	palette_address = user_map(palette, sizeof(palette));
	memset(&blit, 0, sizeof(blit));
	blit.width = 2U; blit.height = 2U; blit.stride = 2U;
	blit.format = ZEDBSD_GRAPHICS_FORMAT_INDEX8;
	blit.pixels = (uapi_ptr_t)pixel_address;
	blit.palette = (uapi_ptr_t)palette_address;
	blit.palette_count = 2U;
	fail_copyin_address = pixel_address + 2U;
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_BLIT, &blit,
	    sizeof(blit)) == EFAULT && blit_count == 5U);

	user_reset();
	pixel_address = user_map(pixels, sizeof(pixels));
	palette_address = user_map(palette, sizeof(palette));
	memset(&blit, 0, sizeof(blit));
	blit.width = 1U; blit.height = 1U; blit.stride = 1U;
	blit.format = ZEDBSD_GRAPHICS_FORMAT_INDEX8;
	blit.pixels = (uapi_ptr_t)pixel_address;
	blit.palette = (uapi_ptr_t)palette_address;
	blit.palette_count = 257U;
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_BLIT, &blit,
	    sizeof(blit)) == EINVAL);
	blit.palette_count = 2U;
	fail_copyin_address = palette_address;
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_BLIT, &blit,
	    sizeof(blit)) == EFAULT);

	user_reset();
	memset(&flush, 0, sizeof(flush));
	flush.rectangle_count = 33U;
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_FLUSH, &flush,
	    sizeof(flush)) == EINVAL);
	user_reset();
	memset(rectangles, 0, sizeof(rectangles));
	rectangles[0] = (struct graphics_rect){ 1U, 1U, 2U, 2U };
	rectangles[1] = (struct graphics_rect){ 639U, 479U, 1U, 1U };
	flush.rectangles = (uapi_ptr_t)user_map(rectangles, sizeof(rectangles));
	flush.rectangle_count = 2U;
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_FLUSH, &flush,
	    sizeof(flush)) == 0 && flush_count == 1U);

	user_reset();
	memset(bitmap, 0, sizeof(bitmap));
	bitmap_address = user_map(bitmap, sizeof(bitmap));
	memset(&glyph, 0, sizeof(glyph));
	glyph.codepoint = 'A';
	glyph.bitmap = (uapi_ptr_t)bitmap_address;
	glyph.bitmap_capacity = 31U;
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_GET_GLYPH, &glyph,
	    sizeof(glyph)) == EINVAL);
	glyph.bitmap_capacity = 32U;
	request_address = user_map(&glyph, sizeof(glyph));
	CHECK(device_ops->ioctl(owner, ZEDBSD_GRAPHICS_GET_GLYPH,
	    request_address) == 0);
	CHECK(glyph_count == 1U && glyph.width == 8U && glyph.height == 16U &&
	    glyph.bitmap_size == 16U && bitmap[0] == 1U && bitmap[15] == 16U);
	user_reset();
	request_address = user_map(&mode, sizeof(mode));
	CHECK(device_ops->ioctl(owner, 0xffffffffUL, request_address) ==
	    EOPNOTSUPP);

	backend_render_succeeds = 0;
	user_reset();
	fill.rect = (struct graphics_rect){ 0U, 0U, 1U, 1U };
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_FILL_RECT, &fill,
	    sizeof(fill)) == EIO);
	backend_render_succeeds = 1;
}

static void
test_cleanup(struct file *owner, struct file *other)
{
	struct graphics_mode mode;
	uintptr_t address;
	unsigned leaves_before = leave_count;

	graphics_device_restore_text();
	CHECK(leave_count == leaves_before + 1U);
	user_reset();
	memset(&mode, 0, sizeof(mode));
	CHECK(device_ioctl(owner, ZEDBSD_GRAPHICS_GET_MODE, &mode,
	    sizeof(mode)) == ENXIO);
	CHECK(device_ops->open(other) == EBUSY);
	CHECK(device_ops->close(owner) == 0);
	CHECK(device_ops->open(other) == 0);

	user_reset();
	memset(&mode, 0, sizeof(mode));
	address = user_map(&mode, sizeof(mode));
	fail_copyout_address = address;
	leaves_before = leave_count;
	CHECK(device_ops->ioctl(other, ZEDBSD_GRAPHICS_ENTER, address) == EFAULT);
	CHECK(leave_count == leaves_before + 1U);
	enter_graphics(other);
	leaves_before = leave_count;
	CHECK(device_ops->close(other) == 0);
	CHECK(leave_count == leaves_before + 1U);
	CHECK(device_ops->open(owner) == 0);
	CHECK(device_ops->close(other) == 0);
	CHECK(device_ops->open(other) == EBUSY);
	CHECK(device_ops->close(owner) == 0);
}

int
main(void)
{
	uint8_t owner_storage, other_storage;
	struct file *owner = (struct file *)(void *)&owner_storage;
	struct file *other = (struct file *)(void *)&other_storage;

	graphics_device_restore_text();
	CHECK(resume_count == 1U);
	test_open_caps_modes(owner, other);
	test_enter_and_render(owner);
	test_cleanup(owner, other);
	CHECK(lock_depth == 0U);
	printf("KA-T080 %s independent frontend: PASS\n", TEST_NAME);
	return 0;
}
