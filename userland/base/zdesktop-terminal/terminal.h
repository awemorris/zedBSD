/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of zdesktop-terminal, a VT100 terminal drawn with Vulkan in a
 * Wayland window.
 *
 * screen.c keeps the character grid and interprets what the shell writes;
 * keys.c turns the compositor's key codes into the bytes a shell reads;
 * font.c draws the glyphs of a monospaced TrueType font into an atlas;
 * render.c draws the grid from that atlas; window.c holds the Wayland
 * window and its keyboard; main.c runs the shell on a pseudo-terminal and
 * ties them together.
 */

#ifndef ZDESKTOP_TERMINAL_H
#define ZDESKTOP_TERMINAL_H

#define VK_USE_PLATFORM_WAYLAND_KHR 1
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>

#include <stddef.h>
#include <stdint.h>

/* The largest grid the terminal keeps, whatever the window's size. */
#define TERMINAL_MAX_COLUMNS	240U
#define TERMINAL_MAX_ROWS	100U

/* How many numeric parameters one control sequence keeps. */
#define TERMINAL_PARAMETERS	16

/* The space between the window's edge and the grid, in pixels. */
#define TERMINAL_PADDING	8U

/* The default colours: light grey on a dark blue-grey, as 0xRRGGBB. */
#define TERMINAL_FOREGROUND	0xdcdfe6U
#define TERMINAL_BACKGROUND	0x1d2230U

/* The modifier bits of wl_keyboard.modifiers, as zwl reports them. */
#define TERMINAL_MODIFIER_SHIFT		0x01U
#define TERMINAL_MODIFIER_CONTROL	0x04U
#define TERMINAL_MODIFIER_ALT		0x08U

/*
 * One character cell of the grid.
 *
 * A wide character takes two cells; the second is a continuation with no
 * character of its own.
 */
struct terminal_cell {
	/* The character shown, a Unicode code point. */
	uint32_t codepoint;

	/* Its colours, as 0xRRGGBB. */
	uint32_t foreground;
	uint32_t background;

	/* Nonzero for the right half of a wide character. */
	uint8_t continuation;
};

/*
 * The character grid and the state of the byte stream that fills it.
 *
 * One lives for the terminal's whole run; a resize keeps the cells that
 * still fit.
 */
struct terminal_screen {
	/* The grid's size in cells and its cells, row after row. */
	unsigned columns;
	unsigned rows;
	struct terminal_cell cells[TERMINAL_MAX_COLUMNS * TERMINAL_MAX_ROWS];

	/* Where the next character goes, and where ESC 7 saved it. */
	unsigned cursor_column;
	unsigned cursor_row;
	unsigned saved_column;
	unsigned saved_row;

	/* Whether the cursor is shown (DECTCEM, CSI ? 25 h / l). */
	int cursor_visible;

	/* The rows scrolled by a line feed at the bottom (DECSTBM), inclusive. */
	unsigned scroll_top;
	unsigned scroll_bottom;

	/* The colours new characters get, and whether they are swapped (SGR 7). */
	uint32_t foreground;
	uint32_t background;
	int inverse;
	int bold;

	/* The parser: 0 text, 1 after ESC, 2 in a CSI sequence, 3 in an OSC string, 4 after ESC of a string's end. */
	int parser_state;
	int parameters[TERMINAL_PARAMETERS];
	int parameter_count;
	int private_mode;

	/* A UTF-8 sequence in progress: the value so far, its least legal value and how many bytes remain. */
	uint32_t utf8_value;
	uint32_t utf8_minimum;
	unsigned utf8_remaining;

	/* Nonzero once a change needs a new frame. */
	int changed;
};

/*
 * The glyph atlas: every character drawn so far, one cell-sized slot each.
 *
 * The pixels live in an image the renderer owns; the atlas only decides
 * which slot holds which character and draws into the slot.  Slot 0 is a
 * solid block the cursor is drawn with.
 */
struct terminal_font {
	/* The font file, kept in memory for the face, and the face. */
	void *data;
	size_t size;
	struct truetype_face *face;

	/* The cell size in pixels and where the baseline is from the cell's top. */
	unsigned cell_width;
	unsigned cell_height;
	int baseline;

	/* The atlas image's size, its pixels (B8G8R8A8, borrowed from the renderer) and its row pitch. */
	unsigned atlas_width;
	unsigned atlas_height;
	unsigned char *pixels;
	size_t row_pitch;

	/* How many slots there are and how many are used. */
	unsigned slots;
	unsigned used;

	/* The characters in the slots: an open-addressing table from code point to slot. */
	uint32_t *keys;
	uint32_t *values;
	unsigned table_size;
};

/*
 * One swapchain image the renderer draws into; the image is the swapchain's.
 */
struct terminal_target {
	VkImage image;
	VkImageView view;
	VkFramebuffer framebuffer;
	VkSemaphore rendered;
};

/*
 * The Vulkan objects of the terminal's window.
 *
 * They are made once the window is configured and remade in part when the
 * window changes size.
 */
struct terminal_renderer {
	/* The instance, the surface of the window, the device and its queue. */
	VkInstance instance;
	VkSurfaceKHR surface;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	uint32_t family;

	/* The swapchain, its format and extent, and one target per image. */
	VkSwapchainKHR swapchain;
	VkFormat format;
	VkExtent2D extent;
	struct terminal_target *targets;
	uint32_t count;

	/* The pass and the pipeline that draw cells, and what the pipeline binds. */
	VkRenderPass pass;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkPipeline pipeline;
	VkDescriptorPool descriptor_pool;
	VkDescriptorSet set;
	VkSampler sampler;

	/* The glyph atlas: a host-written linear image and its view. */
	VkImage atlas;
	VkDeviceMemory atlas_memory;
	VkImageView atlas_view;

	/* The vertices of one frame, in host-visible memory mapped for good. */
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	void *vertex_map;
	size_t vertex_capacity;

	/* One command buffer, the fence that says it finished and the acquire semaphore. */
	VkCommandPool pool;
	VkCommandBuffer command;
	VkFence fence;
	VkSemaphore acquired;

	/* The Vulkan call that failed last, for the error line. */
	const char *operation;
};

/*
 * The Wayland window, its keyboard and the key being repeated.
 */
struct terminal_window {
	/* The connection and the globals bound from it. */
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct xdg_wm_base *shell;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;

	/* The window: its surface and roles. */
	struct wl_surface *surface;
	struct xdg_surface *role;
	struct xdg_toplevel *toplevel;

	/* The size the compositor asked for, and whether it changed since it was last taken. */
	uint32_t width;
	uint32_t height;
	int resized;

	/* Whether the first configure arrived, and whether the compositor asked the window to close. */
	int configured;
	int closed;

	/* The modifiers held, as wl_keyboard.modifiers reports them. */
	uint32_t modifiers;

	/* The bytes the keys pressed since the last read produced, for the shell. */
	unsigned char input[256];
	size_t input_length;

	/* The key held for repeating (0 when none) and when it repeats next, in milliseconds. */
	uint32_t repeat_key;
	uint64_t repeat_at;

	/* The repeat's first delay and its interval, in milliseconds (wl_keyboard.repeat_info). */
	uint32_t repeat_delay;
	uint32_t repeat_interval;
};

/* The character grid (screen.c). */
void terminal_screen_init(struct terminal_screen *screen, unsigned columns, unsigned rows);
void terminal_screen_resize(struct terminal_screen *screen, unsigned columns, unsigned rows);
void terminal_screen_write(struct terminal_screen *screen, const unsigned char *bytes, size_t length);
struct terminal_cell *terminal_screen_cell(struct terminal_screen *screen, unsigned column, unsigned row);

/* The key codes (keys.c). */
size_t terminal_key_bytes(uint32_t key, uint32_t modifiers, unsigned char *bytes, size_t size);

/* The glyph atlas (font.c). */
int terminal_font_open(struct terminal_font *font, const char *path, unsigned pixels);
int terminal_font_attach(struct terminal_font *font, unsigned char *pixels, size_t row_pitch, unsigned width, unsigned height);
unsigned terminal_font_slot(struct terminal_font *font, uint32_t codepoint);
void terminal_font_close(struct terminal_font *font);

/* The drawing (render.c). */
VkResult terminal_renderer_open(struct terminal_renderer *renderer, struct terminal_window *window, struct terminal_font *font);
VkResult terminal_renderer_resize(struct terminal_renderer *renderer, uint32_t width, uint32_t height);
VkResult terminal_renderer_draw(struct terminal_renderer *renderer, struct terminal_screen *screen, struct terminal_font *font);
void terminal_renderer_close(struct terminal_renderer *renderer);

/* The window (window.c). */
int terminal_window_open(struct terminal_window *window, const char *display, uint32_t width, uint32_t height);
int terminal_window_dispatch(struct terminal_window *window, int other, int timeout, int *other_ready);
void terminal_window_repeat(struct terminal_window *window, uint64_t now);
void terminal_window_close(struct terminal_window *window);
uint64_t terminal_clock(void);

#endif
