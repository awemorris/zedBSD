/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/packages/lang/noct/runtime/zedbsd-api.h"

#include <zedbsd/console.h>
#include <zedbsd/dirent.h>
#include <zedbsd/graphics.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <noct/beui.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int console_fd = -1;
static int graphics_fd = -1;
static struct noct_beui_display_info display_info;

static int get_console(void)
{
	if (console_fd < 0)
		console_fd = open("/dev/console", O_RDWR);
	return console_fd;
}

static int screen_clear(void *context)
{
	(void)context;
	return get_console() >= 0 && ioctl(console_fd, ZEDBSD_CONSOLE_CLEAR) == 0;
}
static int screen_clear_row(void *context, unsigned row)
{
	struct zedbsd_console_row request = { row };
	(void)context;
	return get_console() >= 0 && ioctl(console_fd, ZEDBSD_CONSOLE_CLEAR_ROW, &request) == 0;
}
static int screen_put_utf8(void *context, unsigned row, unsigned column,
			   const char *text, unsigned length, uint8_t attribute)
{
	struct zedbsd_console_write_at request;
	(void)context;
	request.row = row; request.column = column; request.attribute = attribute;
	request.address = (uapi_ptr_t)(uintptr_t)text; request.length = length;
	return get_console() >= 0 && ioctl(console_fd, ZEDBSD_CONSOLE_WRITE_AT, &request) == 0;
}
static int screen_put(void *context, unsigned row, unsigned column,
		      const char *text, uint8_t attribute)
{
	return screen_put_utf8(context, row, column, text,
		(unsigned)strlen(text), attribute);
}
static int screen_clear_to_eol(void *context, unsigned row, unsigned column)
{
	struct zedbsd_console_position request = { row, column };
	(void)context;
	return get_console() >= 0 && ioctl(console_fd, ZEDBSD_CONSOLE_CLEAR_TO_EOL, &request) == 0;
}
static int screen_set_cursor(void *context, unsigned row, unsigned column)
{
	struct zedbsd_console_cursor request = { row, column, 1 };
	(void)context;
	return get_console() >= 0 && ioctl(console_fd, ZEDBSD_CONSOLE_SET_CURSOR, &request) == 0;
}
static int screen_show_cursor(void *context, int visible)
{
	struct zedbsd_console_cursor request = { 0, 0, visible != 0 };
	(void)context;
	return get_console() >= 0 && ioctl(console_fd, ZEDBSD_CONSOLE_SHOW_CURSOR, &request) == 0;
}
static int keyboard_event(unsigned long command)
{
	struct zedbsd_console_event event;
	if (get_console() < 0 || ioctl(console_fd, command, &event) != 0)
		return -1;
	return (int)event.value;
}
static int keyboard_poll(void *context) { (void)context; return keyboard_event(ZEDBSD_CONSOLE_POLL_EVENT); }
static int keyboard_read(void *context) { (void)context; return keyboard_event(ZEDBSD_CONSOLE_READ_EVENT); }
static int clock_second(void *context)
{
	struct timespec now;
	(void)context;
	return clock_gettime(CLOCK_MONOTONIC, &now) == 0 ? (int)(now.tv_sec % 60) : -1;
}
static int file_size(void *context, const char *path, uint32_t *size)
{
	struct stat status;
	(void)context;
	if (size == NULL || stat(path, &status) != 0 || status.st_size < 0)
		return 0;
	*size = (uint32_t)status.st_size;
	return (off_t)*size == status.st_size;
}
static int file_read_at(void *context, const char *path, uint32_t offset,
			void *buffer, uint32_t length)
{
	int fd; ssize_t count;
	(void)context;
	fd = open(path, O_RDONLY);
	if (fd < 0) return 0;
	if (lseek(fd, (off_t)offset, SEEK_SET) != (off_t)offset) {
		(void)close(fd); return 0;
	}
	count = read(fd, buffer, length);
	(void)close(fd);
	return count == (ssize_t)length;
}
static int directory_read(void *context, const char *path, unsigned index,
			  struct zedbsd_noct_dirent *entry)
{
	DIR *directory; struct dirent *item = NULL; struct stat status;
	char child[512]; unsigned i;
	(void)context;
	if (entry == NULL || (directory = opendir(path)) == NULL) return -1;
	for (i = 0; i <= index; i++) {
		item = readdir(directory);
		if (item == NULL) break;
	}
	if (item != NULL) {
		strncpy(entry->name, item->d_name, sizeof(entry->name) - 1U);
		entry->name[sizeof(entry->name) - 1U] = '\0';
		/* FAT 8.3 directory entries are conventionally returned in upper
		 * case, while zedBSD paths are case-insensitive and the user-facing
		 * namespace is rooted directly at /.  Present one stable,
		 * lower-case spelling so POSIX clients can perform case-sensitive
		 * completion on the names they typed. */
		for (char *name = entry->name; *name != '\0'; name++)
			if (*name >= 'A' && *name <= 'Z')
				*name = (char)(*name - 'A' + 'a');
		if (!strcmp(path, "/"))
			snprintf(child, sizeof(child), "/%s", item->d_name);
		else
			snprintf(child, sizeof(child), "%s/%s", path, item->d_name);
		if (stat(child, &status) == 0 && status.st_size >= 0)
			entry->size = (uint64_t)status.st_size;
		else
			entry->size = 0;
		entry->attributes = item->d_type == DT_DIR ? 0x10U : 0x20U;
	}
	(void)closedir(directory);
	return item == NULL ? 0 : 1;
}

static int display_enter(void *context, struct noct_beui_display_info *info)
{
	struct zedbsd_graphics_mode request;
	(void)context;
	if (info == NULL) return 0;
	if (graphics_fd < 0) graphics_fd = open("/dev/graphics", O_RDWR);
	if (graphics_fd < 0) {
		fprintf(stderr, "NOCT.ELF: open /dev/graphics failed: %d\n", errno);
		return 0;
	}
	memset(&request, 0, sizeof(request));
	request.preferred_bits_per_pixel = info->preferred_bits_per_pixel;
	if (ioctl(graphics_fd, ZEDBSD_GRAPHICS_ENTER, &request) != 0) {
		fprintf(stderr, "NOCT.ELF: graphics enter failed: %d\n", errno);
		(void)close(graphics_fd); graphics_fd = -1; return 0;
	}
	info->width = request.width; info->height = request.height;
	info->bits_per_pixel = request.bits_per_pixel; info->stride = request.stride;
	display_info = *info;
	return 1;
}
static void display_leave(void *context)
{
	(void)context;
	if (graphics_fd >= 0) (void)close(graphics_fd);
	graphics_fd = -1;
	memset(&display_info, 0, sizeof(display_info));
}
static int display_poll(void *context) { (void)context; return 1; }
static void rect_copy(struct zedbsd_graphics_rect *to, const struct noct_beui_rect *from)
{
	to->x = from->x; to->y = from->y; to->width = from->width; to->height = from->height;
}
static int display_fill(void *context, const struct noct_beui_rect *rect, uint32_t color)
{
	struct zedbsd_graphics_fill request;
	(void)context; memset(&request, 0, sizeof(request)); rect_copy(&request.rect, rect); request.color = color;
	return graphics_fd >= 0 && ioctl(graphics_fd, ZEDBSD_GRAPHICS_FILL_RECT, &request) == 0;
}
static int display_line(void *context, unsigned x0, unsigned y0, unsigned x1, unsigned y1, uint32_t color)
{
	struct zedbsd_graphics_line request = { x0, y0, x1, y1, color, 0 };
	(void)context; return graphics_fd >= 0 && ioctl(graphics_fd, ZEDBSD_GRAPHICS_DRAW_LINE, &request) == 0;
}
static int display_pattern(void *context, const struct noct_beui_rect *rect, uint32_t color, uint64_t pattern)
{
	struct zedbsd_graphics_pattern_fill request;
	(void)context; memset(&request, 0, sizeof(request)); rect_copy(&request.rect, rect); request.color = color; request.pattern = pattern;
	return graphics_fd >= 0 && ioctl(graphics_fd, ZEDBSD_GRAPHICS_PATTERN_FILL, &request) == 0;
}
static int display_image_common(void *context, unsigned x, unsigned y,
				const struct noct_beui_image *image, uint64_t pattern, int patterned)
{
	struct zedbsd_graphics_blit request;
	int result;
	(void)context;
	if (image == NULL) return 0;
	memset(&request, 0, sizeof(request));
	request.x = x; request.y = y; request.width = image->width; request.height = image->height;
	request.format = image->format == NOCT_BEUI_IMAGE_RGB24 ? ZEDBSD_GRAPHICS_FORMAT_RGB24 : ZEDBSD_GRAPHICS_FORMAT_INDEX8;
	request.stride = (uint32_t)image->stride; request.pixels = (uapi_ptr_t)(uintptr_t)image->pixels;
	/* RGB24 has no palette; the graphics ABI requires both fields to remain
	 * zero.  noct_beui_image embeds palette storage even for RGB24 images,
	 * so do not infer palette presence from the array address. */
	if (request.format == ZEDBSD_GRAPHICS_FORMAT_INDEX8) {
		request.palette = (uapi_ptr_t)(uintptr_t)image->palette;
		request.palette_count = image->palette_size;
	}
	request.pattern = pattern;
	if (graphics_fd < 0)
		return 0;
	result = ioctl(graphics_fd, patterned ? ZEDBSD_GRAPHICS_BLIT_PATTERN :
		ZEDBSD_GRAPHICS_BLIT, &request);
	if (result != 0)
		fprintf(stderr,
		    "NOCT.ELF: graphics blit failed: errno=%d x=%u y=%u "
		    "width=%u height=%u format=%u stride=%u pixels=%#x "
		    "palette=%#x palette_count=%u display=%ux%ux%u\n",
		    errno, request.x, request.y, request.width, request.height,
		    request.format, request.stride, request.pixels,
		    request.palette, request.palette_count, display_info.width,
		    display_info.height, display_info.bits_per_pixel);
	return result == 0;
}
static int display_image(void *c, unsigned x, unsigned y, const struct noct_beui_image *i) { return display_image_common(c, x, y, i, 0, 0); }
static int display_image_pattern(void *c, unsigned x, unsigned y, const struct noct_beui_image *i, uint64_t p) { return display_image_common(c, x, y, i, p, 1); }
static int display_flush(void *context, const struct noct_beui_rect *rectangles, size_t count)
{
	struct zedbsd_graphics_rect converted[32]; struct zedbsd_graphics_flush request; size_t i;
	(void)context; if (count > 32U) return 0;
	for (i = 0; i < count; i++) rect_copy(&converted[i], &rectangles[i]);
	request.rectangles = count == 0 ? 0U : (uapi_ptr_t)(uintptr_t)converted; request.rectangle_count = (uint32_t)count;
	return graphics_fd >= 0 && ioctl(graphics_fd, ZEDBSD_GRAPHICS_FLUSH, &request) == 0;
}
static int glyph_measure(void *context, uint32_t codepoint, unsigned *width, unsigned *height)
{
	struct zedbsd_graphics_glyph request; uint8_t bitmap[32];
	(void)context; memset(&request, 0, sizeof(request)); request.codepoint = codepoint;
	request.bitmap = (uapi_ptr_t)(uintptr_t)bitmap; request.bitmap_capacity = sizeof(bitmap);
	if (graphics_fd < 0 || ioctl(graphics_fd, ZEDBSD_GRAPHICS_GET_GLYPH, &request) != 0) return 0;
	*width = request.width; *height = request.height; return 1;
}
static int glyph_draw(void *context, unsigned x, unsigned y, uint32_t codepoint, uint32_t foreground, uint32_t background)
{
	struct zedbsd_graphics_glyph glyph; struct zedbsd_graphics_blit blit; uint8_t bitmap[32];
	(void)context; memset(&glyph, 0, sizeof(glyph)); glyph.codepoint = codepoint;
	glyph.bitmap = (uapi_ptr_t)(uintptr_t)bitmap; glyph.bitmap_capacity = sizeof(bitmap);
	if (graphics_fd < 0 || ioctl(graphics_fd, ZEDBSD_GRAPHICS_GET_GLYPH, &glyph) != 0) return 0;
	memset(&blit, 0, sizeof(blit)); blit.x = x; blit.y = y; blit.width = glyph.width; blit.height = glyph.height;
	blit.format = ZEDBSD_GRAPHICS_FORMAT_MONO1; blit.stride = glyph.stride;
	blit.pixels = (uapi_ptr_t)(uintptr_t)bitmap; blit.foreground = foreground; blit.background = background;
	return ioctl(graphics_fd, ZEDBSD_GRAPHICS_BLIT, &blit) == 0;
}
static uint64_t milliseconds(void *context)
{
	struct timespec now; (void)context;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
static int key_state(void *context, int key)
{
	struct zedbsd_console_key_state request = { (uint32_t)key, 0 };
	(void)context;
	return get_console() >= 0 && ioctl(console_fd, ZEDBSD_CONSOLE_KEY_STATE, &request) == 0 ? request.down : -1;
}
static void input_drain(void *context) { (void)context; if (get_console() >= 0) (void)ioctl(console_fd, ZEDBSD_CONSOLE_DRAIN_INPUT); }

static struct noct_beui_hal beui = {
	.display = { NULL, display_enter, display_leave, display_poll, display_fill,
		display_line, display_pattern, display_image, display_image_pattern, display_flush },
	.glyph = { NULL, glyph_measure, glyph_draw },
	.clock = { NULL, milliseconds },
	.input = { NULL, key_state, input_drain },
};
static const struct zedbsd_noct_services services = {
	NULL, &beui, screen_clear, screen_clear_row, screen_put, screen_put_utf8,
	screen_clear_to_eol, screen_set_cursor, screen_show_cursor,
	keyboard_poll, keyboard_read, clock_second, file_size, file_read_at,
	directory_read,
};

const struct zedbsd_noct_services *zedbsd_user_noct_services(void) { return &services; }
