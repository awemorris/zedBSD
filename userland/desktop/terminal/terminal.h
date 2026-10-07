/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of terminal, a VT100 terminal drawn with Vulkan in a
 * Wayland window.
 *
 * screen.c keeps the character grid and interprets what the shell writes;
 * width.c says how many cells a character takes; settings.c keeps the
 * terminal's settings between runs;
 * keys.c turns the compositor's key codes into the bytes a shell reads;
 * font.c draws the glyphs of a monospaced TrueType font into an atlas;
 * render.c draws the grid from that atlas; window.c holds the Wayland
 * window (libkeiland's application and window, WS131 p018) and turns its
 * input into the terminal's; menu.c gives the compositor the window's menus
 * (Shell, Edit, View, Session, Help) through libkeiland; tabs.c gives it
 * the window's tabs (the titlebar's TABS mode, ws035-p086); clipboard.c
 * and primary.c paste, drop and drag through libkeiland's window; main.c
 * runs a shell on a pseudo-terminal for each tab and ties them together.
 */

#ifndef TERMINAL_H
#define TERMINAL_H

#define VK_USE_PLATFORM_WAYLAND_KHR 1
#include <vulkan/vulkan.h>
#include <keiland/keiland.h>

#include <stddef.h>
#include <stdint.h>

#include "touch.h"
#include "width.h"

/* The largest grid the terminal keeps, whatever the window's size. */
#define TERMINAL_MAX_COLUMNS	240U
#define TERMINAL_MAX_ROWS	100U

/* How many lines that scrolled off the top of the screen the terminal keeps to scroll back to (ws035-p114). */
#define TERMINAL_HISTORY	1000U

/* How many numeric parameters one control sequence keeps. */
#define TERMINAL_PARAMETERS	16

/* The longest OSC string kept (the rest is dropped), and the longest title one sets (UTF-8 bytes). */
#define TERMINAL_OSC		256U
#define TERMINAL_TITLE		128U

/* The space between the window's edge and the grid, in pixels. */
#define TERMINAL_PADDING	8U

/* The default colours: light grey on a dark blue-grey, as 0xRRGGBB. */
#define TERMINAL_FOREGROUND	0xdcdfe6U
#define TERMINAL_BACKGROUND	0x1d2230U

/* The background of selected cells, a muted blue. */
#define TERMINAL_SELECTION	0x3a5a98U

/*
 * The colour themes of View > Theme (ws128-p006): the dark one the
 * terminal always had (TERMINAL_FOREGROUND on TERMINAL_BACKGROUND), a light
 * one, and white on black.  The cells keep the dark theme's colours for
 * the text's default ones; the drawing puts the theme's in their place.
 */
#define TERMINAL_THEME_DARK	0U
#define TERMINAL_THEME_LIGHT	1U
#define TERMINAL_THEME_CONTRAST	2U
#define TERMINAL_THEMES		3U

/* The longest text Edit > Find looks for, in bytes and in characters (ws128-p006). */
#define TERMINAL_SEARCH_BYTES	128U
#define TERMINAL_SEARCH_LENGTH	64U

/* The font sizes zooming stays within, its step, and the sizes the View menu names. */
#define TERMINAL_PIXELS_MIN	8U
#define TERMINAL_PIXELS_MAX	32U
#define TERMINAL_PIXELS_STEP	2U
#define TERMINAL_PIXELS_SMALL	12U
#define TERMINAL_PIXELS_MEDIUM	16U
#define TERMINAL_PIXELS_LARGE	20U
#define TERMINAL_PIXELS_HUGE	24U

/* How many menu choices, and tab requests, wait for the main loop at most. */
#define TERMINAL_ACTIONS	16U

/* The most tabs (shells) one window has, and the longest tab title. */
#define TERMINAL_TABS		8U
#define TERMINAL_TAB_TITLE	64U

/* How many pointer events wait for the main loop at most (ws035-p093). */
#define TERMINAL_POINTER_EVENTS	32U

/* How many touch inputs wait for the main loop at most (ws081-p011). */
#define TERMINAL_TOUCH_EVENTS	256U

/* The kinds of pointer events the main loop takes: the left button pressed or released, a motion. */
#define TERMINAL_POINTER_PRESS		1U
#define TERMINAL_POINTER_RELEASE	2U
#define TERMINAL_POINTER_MOTION		3U
#define TERMINAL_POINTER_MIDDLE		4U

/* What the titlebar asks of the tabs (tabs.c): none, a new tab, one chosen, one to close. */
#define TERMINAL_TAB_NONE	0U
#define TERMINAL_TAB_NEW	1U
#define TERMINAL_TAB_ACTIVATE	2U
#define TERMINAL_TAB_CLOSE	3U

/*
 * The actions the menus' items report (menu.c); the main loop carries them
 * out.  They are numbers of the terminal's own, apart from the items' IDs.
 */
enum terminal_action {
	TERMINAL_ACTION_NONE,
	TERMINAL_ACTION_NEW_WINDOW,
	TERMINAL_ACTION_CLOSE,
	TERMINAL_ACTION_COPY,
	TERMINAL_ACTION_PASTE,
	TERMINAL_ACTION_SELECT_ALL,
	TERMINAL_ACTION_ZOOM_IN,
	TERMINAL_ACTION_ZOOM_OUT,
	TERMINAL_ACTION_ZOOM_NORMAL,
	TERMINAL_ACTION_SIZE_SMALL,
	TERMINAL_ACTION_SIZE_MEDIUM,
	TERMINAL_ACTION_SIZE_LARGE,
	TERMINAL_ACTION_SIZE_HUGE,
	TERMINAL_ACTION_FULLSCREEN,
	TERMINAL_ACTION_INTERRUPT,
	TERMINAL_ACTION_END_OF_FILE,
	TERMINAL_ACTION_CLEAR,
	TERMINAL_ACTION_RESET,
	TERMINAL_ACTION_ABOUT,
	TERMINAL_ACTION_NEW_TAB,
	TERMINAL_ACTION_CLOSE_TAB,
	TERMINAL_ACTION_AMBIGUOUS_WIDE,
	TERMINAL_ACTION_FIND,
	TERMINAL_ACTION_FIND_NEXT,
	TERMINAL_ACTION_FIND_PREVIOUS,
	TERMINAL_ACTION_THEME_DARK,
	TERMINAL_ACTION_THEME_LIGHT,
	TERMINAL_ACTION_THEME_CONTRAST
};

/*
 * The colours of a theme (ws128-p006), as 0xRRGGBB: the text's default
 * foreground and background, the selection's background, a match's
 * background and the current match's, the text on a match, and the
 * search bar's background.
 */
struct terminal_theme {
	uint32_t foreground;
	uint32_t background;
	uint32_t selection;
	uint32_t match;
	uint32_t current;
	uint32_t match_text;
	uint32_t bar;
};

/*
 * One tab as the titlebar shows it: its ID (not 0) and its title.
 */
struct terminal_tab_view {
	uint32_t id;
	char title[TERMINAL_TAB_TITLE];
};

/* One thing the titlebar asked of the tabs: its kind (TERMINAL_TAB_*) and the tab (0 for a new one). */
struct terminal_tab_request {
	unsigned kind;
	uint32_t id;
};

/*
 * What the menus show of the terminal's state: whether something is
 * selected (Copy), whether the clipboard holds text (Paste), the font's
 * size (Zoom, Text Size), whether the window is fullscreen and whether
 * Ambiguous-width characters are wide (ws128-p009), the theme and whether
 * there is a text to find again (ws128-p006).
 */
struct terminal_menu_state {
	int selection;
	int clipboard;
	unsigned pixels;
	int fullscreen;
	int ambiguous_wide;
	unsigned theme;
	int can_find_again;
};

/*
 * The terminal's own settings kept between runs (settings.c, the desktop's
 * settings terminal.* through libkeiland, WS135): whether Ambiguous-width
 * characters are wide (ws128-p009), the font's size in pixels and the
 * colour theme (ws128-p006).  All zero is the default (a size of 0: the
 * size the terminal is started with).
 */
struct terminal_settings {
	int ambiguous_wide;
	unsigned font_size;
	unsigned theme;
};

/* The modifier bits of wl_keyboard.modifiers, as the compositor reports them. */
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

	/*
	 * Nonzero while a character written in the last column has left the
	 * cursor there with its wrap still to come (the right margin of xterm
	 * and the VT100, terminfo's am and xenl; ws128-p010, BUG-150).  The
	 * next printed character wraps first; a carriage return, a line feed,
	 * a move or an edit cancels the wrap, and SGR and the modes keep it.
	 */
	int wrap_pending;

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

	/*
	 * Whether Ambiguous-width characters written from now on take two cells
	 * (View > Treat Ambiguous-Width Characters as Wide, ws128-p009).  Cells
	 * already written keep their width.
	 */
	int ambiguous_wide;

	/* The parser: 0 text, 1 after ESC, 2 in a CSI sequence, 3 in an OSC string, 4 after ESC of a string's end. */
	int parser_state;
	int parameters[TERMINAL_PARAMETERS];
	int parameter_count;
	int private_mode;

	/*
	 * The OSC string being read (its first TERMINAL_OSC - 1 bytes), and the
	 * title OSC 0 or 2 last set (ws035-p091; empty until one is set).
	 */
	char osc[TERMINAL_OSC];
	unsigned osc_length;
	char title[TERMINAL_TITLE];

	/* A UTF-8 sequence in progress: the value so far, its least legal value and how many bytes remain. */
	uint32_t utf8_value;
	uint32_t utf8_minimum;
	unsigned utf8_remaining;

	/* Nonzero once a change needs a new frame. */
	int changed;

	/* Nonzero while the whole screen is selected (Edit > Select All), until a key is typed. */
	int selected;

	/*
	 * A range selected with the pointer (ws035-p093): whether there is one,
	 * and its first and last cells (column, line) in reading order.  A line
	 * is numbered from the first the screen ever showed, so the range stays
	 * on its text while the text scrolls (ws035-p114).
	 */
	int range;
	unsigned long range_from[2];
	unsigned long range_to[2];

	/*
	 * The scrollback (ws035-p114): the lines that scrolled off the top of
	 * the whole screen, a ring of TERMINAL_HISTORY lines of
	 * TERMINAL_MAX_COLUMNS cells each, where the oldest line is in the
	 * ring and how many are kept.  scrolled counts every line that ever
	 * scrolled off, so the screen's row r is line scrolled + r and the
	 * oldest line kept is line scrolled - history_count.  view is how many
	 * lines back from the live screen the window shows (0: the live
	 * screen); new output keeps a view that is back on the same text.
	 * view_offset (ws081-p011) moves the text down by that many pixels
	 * within a line while the fingers scroll it smoothly (negative past the
	 * live screen, a line or more past the oldest line kept); the wheel and
	 * the keys set it back to 0.
	 */
	struct terminal_cell history[TERMINAL_HISTORY * TERMINAL_MAX_COLUMNS];
	unsigned history_first;
	unsigned history_count;
	unsigned long scrolled;
	unsigned view;
	int view_offset;
};

/*
 * One pointer event for the main loop (ws035-p093): its kind
 * (TERMINAL_POINTER_*), where it was in the surface, in pixels, its time in
 * milliseconds and its serial (a press's starts a drag).
 */
struct terminal_pointer_event {
	unsigned kind;
	int32_t x;
	int32_t y;
	uint32_t time;
	uint32_t serial;
	uint32_t modifiers;
};

/*
 * The glyph atlas: every character drawn so far, one cell-sized slot each,
 * or two slots side by side for a character drawn two cells wide.
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

	/*
	 * The fallback font (ws128-p009), which draws the characters the face
	 * has no glyph for (CJK): its file, kept in memory, and its face (NULL
	 * without one).  It is drawn at the face's size.
	 */
	void *fallback_data;
	size_t fallback_size;
	struct truetype_face *fallback_face;

	/* The size glyphs are drawn at, in pixels. */
	unsigned pixels_size;

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

	/*
	 * The first of the two slots side by side that hold the replacement
	 * character two cells wide, which a wide character shows when the
	 * atlas is full (ws128-p009).
	 */
	unsigned wide_replacement;

	/*
	 * The characters in the slots: an open-addressing table from code point
	 * to slot.  A character drawn two cells wide is a key of its own (the
	 * code point with FONT_WIDE_KEY), in the first of two slots side by
	 * side, so that the same character can be in narrow and wide cells.
	 */
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
 * The Wayland window: libkeiland's application and its window (WS131
 * p018: the connection, the toplevel, the seat's input, the key repeat,
 * the menus, the tabs, the selections and drag and drop; the terminal draws
 * on its surface with its own Vulkan), and what its input has left for the
 * main loop.
 */
struct terminal_window {
	/* libkeiland's application and window, and the shells' descriptors the application watches. */
	struct kl_app *app;
	struct kl_window *kui;
	int watched[TERMINAL_TABS];
	unsigned watched_count;

	/* The touch inputs not yet taken by the main loop, oldest first (ws081-p011; a full queue drops the newest). */
	struct terminal_touch_event touches[TERMINAL_TOUCH_EVENTS];
	unsigned touch_count;

	/* Where the pointer is over the surface (pixels), and its events not yet taken by the main loop (ws035-p093). */
	int32_t pointer_x;
	int32_t pointer_y;
	struct terminal_pointer_event pointer_events[TERMINAL_POINTER_EVENTS];
	unsigned pointer_event_count;

	/* The size the compositor asked for, and whether it changed since it was last taken. */
	uint32_t width;
	uint32_t height;
	int resized;

	/* Whether the compositor asked the window to close. */
	int closed;

	/* The modifiers held, in the bits of wl_keyboard.modifiers (TERMINAL_MODIFIER_*). */
	uint32_t modifiers;

	/* The bytes the keys pressed since the last read produced, for the shell. */
	unsigned char input[256];
	size_t input_length;

	/*
	 * The input method's text being composed (BUG-155, text-input-v3
	 * through libkeiland's window): shown over the cells from the cursor
	 * until it is committed (its bytes then go to the shell like typed
	 * keys) or replaced; empty for none.  preedit_begin and preedit_end
	 * are the byte range of the segment being converted (-1, or equal,
	 * when there is none); preedit_changed says the main loop has to draw
	 * it again.
	 */
	char preedit[KL_WINDOW_TEXT_MAX];
	int32_t preedit_begin;
	int32_t preedit_end;
	int preedit_changed;

	/*
	 * The input method's last committed text, kept so that a deletion
	 * before the cursor (the on-screen keyboard's voice key replaces the
	 * kana it sent last) can be typed as that many characters' Backspace.
	 */
	char last_commit[KL_WINDOW_TEXT_MAX];

	/*
	 * The view's scrolling the main loop has not yet carried out
	 * (ws035-p114): wheel notches and Shift+Page Up or Down pages, each
	 * positive going back into the scrollback and negative toward the live
	 * screen; wheel is the wheel's distance not yet a whole notch.
	 */
	int scroll_notches;
	int scroll_pages;
	double wheel;

	/* Whether the compositor last configured the window fullscreen. */
	int fullscreen;

	/* The colour theme the window is drawn in (TERMINAL_THEME_*, ws128-p006). */
	unsigned theme;

	/*
	 * Edit > Find (ws128-p006): whether the search bar is open (the keys
	 * then edit the text looked for instead of going to the shell), the
	 * text (UTF-8, search_length bytes), whether it changed since the main
	 * loop last looked, and a step the main loop has yet to take (1 to the
	 * next older match, -1 to the next newer, 0 none).  The main loop
	 * answers with the match it found (search_found; its line, its first
	 * column and how many cells it covers), which the drawing marks.
	 */
	int search_open;
	char search_query[TERMINAL_SEARCH_BYTES];
	size_t search_length;
	int search_edited;
	int search_step;
	int search_found;
	unsigned long search_line;
	unsigned search_column;
	unsigned search_cells;

	/*
	 * The tabs in the titlebar (tabs.c): the tabs and the active one as
	 * last shown (and whether ever shown), and what the titlebar asked and
	 * the main loop has not yet carried out, oldest first.
	 */
	struct terminal_tab_view tabs_shown[TERMINAL_TABS];
	unsigned tabs_shown_count;
	uint32_t tabs_shown_active;
	int tabs_sent;
	struct terminal_tab_request tab_requests[TERMINAL_ACTIONS];
	unsigned tab_request_count;

	/*
	 * The menus (menu.c): whether the window shows them (not without the
	 * compositor's System Menu), the state they last showed, and the
	 * actions chosen but not yet carried out, oldest first.
	 */
	int menu_shown;
	struct terminal_menu_state menu_state;
	uint32_t actions[TERMINAL_ACTIONS];
	unsigned action_count;

	/* A drop on the window waiting for the main loop to paste it (clipboard.c, ws035-p088). */
	int drop_pending;
};

/* The clipboard, drops and drags (clipboard.c). */
void terminal_clipboard_set(struct terminal_window *window, const char *text, size_t length);
int terminal_clipboard_own(const struct terminal_window *window);
int terminal_clipboard_has_text(const struct terminal_window *window);
size_t terminal_clipboard_receive(struct terminal_window *window, char *text, size_t size);
size_t terminal_clipboard_drop(struct terminal_window *window, char *text, size_t size);
void terminal_clipboard_drag(struct terminal_window *window, const char *text, size_t length, uint32_t serial);

/* The primary selection (primary.c). */
void terminal_primary_set(struct terminal_window *window, const char *text, size_t length);
size_t terminal_primary_receive(struct terminal_window *window, char *text, size_t size);

/* The character grid (screen.c). */
void terminal_screen_init(struct terminal_screen *screen, unsigned columns, unsigned rows);
void terminal_screen_set_ambiguous_wide(struct terminal_screen *screen, int ambiguous_wide);
void terminal_screen_resize(struct terminal_screen *screen, unsigned columns, unsigned rows);
void terminal_screen_write(struct terminal_screen *screen, const unsigned char *bytes, size_t length);
struct terminal_cell *terminal_screen_cell(struct terminal_screen *screen, unsigned column, unsigned row);
struct terminal_cell *terminal_screen_line_cell(struct terminal_screen *screen, unsigned column, unsigned long line);
unsigned long terminal_screen_view_line(const struct terminal_screen *screen, unsigned row);
int terminal_screen_scroll_view(struct terminal_screen *screen, int lines);
size_t terminal_screen_text(struct terminal_screen *screen, char *text, size_t size);
int terminal_screen_in_range(const struct terminal_screen *screen, unsigned column, unsigned long line);

/* Finding text in the scrollback and the screen (search.c). */
size_t terminal_search_decode(const char *text, size_t length, uint32_t *query, size_t capacity);
unsigned terminal_search_bar_column(const char *text, size_t length, int ambiguous_wide);
int terminal_search_find(struct terminal_screen *screen, const uint32_t *query, size_t count, unsigned long line, unsigned column, int direction, unsigned long *found_line, unsigned *found_column, unsigned *cells);
int terminal_search_line(struct terminal_screen *screen, const uint32_t *query, size_t count, unsigned long line, unsigned char *marks);
void terminal_search_show(struct terminal_screen *screen, unsigned long line);

/* The settings kept between runs (settings.c). */
void terminal_settings_load(struct terminal_settings *settings);
int terminal_settings_save(const struct terminal_settings *settings);

/* The key codes (keys.c). */
size_t terminal_key_bytes(uint32_t key, uint32_t modifiers, unsigned char *bytes, size_t size);

/* The glyph atlas (font.c). */
int terminal_font_open(struct terminal_font *font, const char *path, unsigned pixels);
int terminal_font_attach(struct terminal_font *font, unsigned char *pixels, size_t row_pitch, unsigned width, unsigned height);
int terminal_font_resize(struct terminal_font *font, unsigned pixels);
int terminal_font_fallback(struct terminal_font *font, const char *path);
unsigned terminal_font_slot(struct terminal_font *font, uint32_t codepoint);
unsigned terminal_font_wide_slot(struct terminal_font *font, uint32_t codepoint);
void terminal_font_close(struct terminal_font *font);

/* The drawing (render.c). */
VkResult terminal_renderer_open(struct terminal_renderer *renderer, struct terminal_window *window, struct terminal_font *font);
VkResult terminal_renderer_resize(struct terminal_renderer *renderer, uint32_t width, uint32_t height);
VkResult terminal_renderer_draw(struct terminal_renderer *renderer, struct terminal_screen *screen, struct terminal_font *font, const struct terminal_window *window);
void terminal_renderer_close(struct terminal_renderer *renderer);

/* The window (window.c). */
int terminal_window_open(struct terminal_window *window, const char *display, uint32_t width, uint32_t height);
int terminal_window_dispatch(struct terminal_window *window, const int *others, unsigned count, int timeout, int *ready);
void terminal_window_close(struct terminal_window *window);
void terminal_window_type(struct terminal_window *window, const char *bytes, size_t length);
void terminal_window_set_fullscreen(struct terminal_window *window, int fullscreen);
uint64_t terminal_clock(void);

/* The tabs in the titlebar (tabs.c). */
void terminal_tabs_show(struct terminal_window *window, const struct terminal_tab_view *tabs, unsigned count, uint32_t active);
void terminal_tabs_input(struct terminal_window *window, const struct kl_window_event *event);
int terminal_tabs_take(struct terminal_window *window, struct terminal_tab_request *request);

/* The menus (menu.c). */
int terminal_menu_open(struct terminal_window *window, const struct terminal_menu_state *state);
void terminal_menu_refresh(struct terminal_window *window, const struct terminal_menu_state *state);
uint32_t terminal_menu_take(struct terminal_window *window);
void terminal_menu_chosen(struct terminal_window *window, const struct kl_window_event *event);
void terminal_menu_close(struct terminal_window *window);

#endif
