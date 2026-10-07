/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The window-system half of Notes: the Wayland window and its input
 * (window.c), the Vulkan drawing (render.c), the frame's geometry
 * (geometry.c), the toolbar (ui.c) and the System Menu (menu.c).
 */

#ifndef NOTES_APP_H
#define NOTES_APP_H

#define VK_USE_PLATFORM_WAYLAND_KHR 1
#include <vulkan/vulkan.h>
#include <keiland/keiland.h>

#include "notes.h"
#include "touch.h"

/*
 * The toolbar's band in pixels, above the page area: the glass card of the
 * buttons floats in it (ui.c).
 */
#define NOTES_TOOLBAR_HEIGHT	68U

/*
 * The toolbar's picture reaches below its band by the height of a notice:
 * a status too long to stand beside the card shows there, centred under
 * it and over the top of the page, for as long as it lasts (ui.c).  The
 * rows are transparent otherwise.
 */
#define NOTES_TOOLBAR_IMAGE_HEIGHT	(NOTES_TOOLBAR_HEIGHT + 44U)

/* The room around the page, in pixels. */
#define NOTES_PAGE_MARGIN	16.0f

/* How many input events, keys and menu choices wait for the main loop at most. */
#define NOTES_INPUTS		4096U
#define NOTES_KEYS		64U
#define NOTES_ACTIONS		32U

/* How many touch inputs wait for the main loop at most (ws081-p013). */
#define NOTES_TOUCH_EVENTS	256U

/* The most buttons the toolbar has. */
#define NOTES_BUTTONS		32U

/*
 * The largest page picture, in pixels on either side (ws081-p013: a page
 * zoomed by the fingers is larger than the window; Vulkan guarantees 4096).
 */
#define NOTES_PICTURE_MAX	4096U

/* The pen's colours and widths the toolbar offers. */
#define NOTES_COLORS		5U
#define NOTES_WIDTHS		3U

/* The pressure a pointer draws with, which has none of its own. */
#define NOTES_POINTER_PRESSURE	0.5f

/* The modifier bits of wl_keyboard.modifiers, as the compositor reports them. */
#define NOTES_MODIFIER_SHIFT	0x01U
#define NOTES_MODIFIER_CONTROL	0x04U
#define NOTES_MODIFIER_ALT	0x08U

/*
 * What the toolbar's buttons, the menus and the keys ask for; the main
 * loop carries each out.  A colour and a width are the base plus their
 * index.
 */
#define NOTES_ACTION_NONE		0U
#define NOTES_ACTION_PEN		1U
#define NOTES_ACTION_HIGHLIGHTER	2U
#define NOTES_ACTION_ERASER		3U
#define NOTES_ACTION_UNDO		4U
#define NOTES_ACTION_REDO		5U
#define NOTES_ACTION_PREVIOUS_PAGE	6U
#define NOTES_ACTION_NEXT_PAGE		7U
#define NOTES_ACTION_NEW_PAGE		8U
#define NOTES_ACTION_SAVE		9U
#define NOTES_ACTION_OPEN		10U
#define NOTES_ACTION_CLOSE		11U
#define NOTES_ACTION_FULLSCREEN	12U
#define NOTES_ACTION_LEAVE_FULLSCREEN	13U
#define NOTES_ACTION_FINGER		14U
#define NOTES_ACTION_SAVE_AS		15U
#define NOTES_ACTION_COLOR		20U
#define NOTES_ACTION_WIDTH		30U

/*
 * ws175-p008: the Select tool (the PDF's images and graphics chosen,
 * moved, sized), and what it does to the object chosen: an image inserted
 * (a file chosen), the object's image replaced, the object deleted, the
 * page's own object put back as the page has it.
 */
#define NOTES_ACTION_SELECT		16U
#define NOTES_ACTION_INSERT_IMAGE	17U
#define NOTES_ACTION_REPLACE_IMAGE	18U
#define NOTES_ACTION_DELETE_OBJECT	19U
#define NOTES_ACTION_RESET_OBJECT	40U

/*
 * ws175-p008 (with ws079-p017): the Text tool (words put on the page in a
 * box), the box opened on the chosen line of text, the font of the box or
 * of the chosen text changed to the next one, and its size made smaller
 * or larger.
 */
#define NOTES_ACTION_TEXT		41U
#define NOTES_ACTION_EDIT_TEXT		42U
#define NOTES_ACTION_FONT		43U
#define NOTES_ACTION_SIZE_DOWN		44U
#define NOTES_ACTION_SIZE_UP		45U

/* ws175-p009: a clean copy of the notebook saved as another file (File > Save Clean Copy). */
#define NOTES_ACTION_SAVE_CLEAN	46U

/* ws177-p011: the clean copy saved last opened in place of the notebook (File > Open Clean Copy). */
#define NOTES_ACTION_OPEN_CLEAN	47U

/* The pipelines a draw uses (render.c). */
#define NOTES_PIPE_STENCIL	0U
#define NOTES_PIPE_FRINGE	1U
#define NOTES_PIPE_COVER	2U
#define NOTES_PIPE_PLAIN	3U
#define NOTES_PIPE_TEXTURE	4U
#define NOTES_PIPES		5U

/* The floats of one vertex: x, y, then u, v (a fringe's distance or a texture place), then red, green, blue, alpha. */
#define NOTES_VERTEX_FLOATS	8U

/*
 * The pictures a texture draw shows: the toolbar, the page with its
 * finished strokes, the page of the PDF the notebook writes on (drawn
 * into the page's picture under the strokes), and the overlay the text box
 * is drawn on (ws175-p008, the window's size).
 */
#define NOTES_TEXTURE_TOOLBAR	0U
#define NOTES_TEXTURE_PAGE	1U
#define NOTES_TEXTURE_BACKGROUND	2U
#define NOTES_TEXTURE_OVERLAY	3U
#define NOTES_TEXTURES		4U

/* How many of the window's inputs wait for the text box at most (ws175-p008). */
#define NOTES_BOX_EVENTS	64U

/*
 * One key press for the main loop: the evdev code and the modifiers held.
 */
struct notes_key {
	uint32_t key;
	uint32_t modifiers;
};

struct notes_window;

/*
 * The Wayland window and what arrived for the main loop.
 *
 * The window is libkeiland's application's (WS131 p018: the toplevel, the
 * seat's input, the menus and a pen tablet; Notes draws on its surface with
 * its own Vulkan).  The pointer's left button is turned into
 * NOTES_SOURCE_POINTER input events (window.c); a tablet's tools add pen
 * and eraser events with their pressure and tilt through
 * notes_window_input() (tablet.c).  ws081-p013: the touch screen's events
 * wait in a queue of their own for touch.c (a window with wl_touch hears
 * fingers only by it, so a finger no longer writes as the pointer).
 */
struct notes_window {
	/*
	 * libkeiland's application and window, and the connection and the
	 * toplevel they own (borrowed, for the file chooser).
	 */
	struct kl_app *app;
	struct kl_window *kui;
	struct wl_display *display;
	struct xdg_toplevel *toplevel;

	/* The size the compositor gave, whether it changed since taken, and whether the window is fullscreen. */
	uint32_t width;
	uint32_t height;
	int resized;
	int fullscreen;

	/* Whether the compositor asked the window to close. */
	int closed;

	/* The pointer's place (surface pixels), whether its left button is held down, and the modifiers held (NOTES_MODIFIER_*). */
	float pointer_x;
	float pointer_y;
	int pointer_down;
	uint32_t modifiers;

	/* The input events, keys and menu choices not yet taken by the main loop, oldest first. */
	struct notes_input inputs[NOTES_INPUTS];
	unsigned input_count;
	struct notes_key keys[NOTES_KEYS];
	unsigned key_count;
	uint32_t actions[NOTES_ACTIONS];
	unsigned action_count;

	/* The touch inputs not yet taken by the main loop, oldest first (a full queue drops the newest). */
	struct notes_touch_event touches[NOTES_TOUCH_EVENTS];
	unsigned touch_count;

	/* Whether the window shows the menus (menu.c; not without the System Menu). */
	int menu_shown;

	/*
	 * ws175-p008: while the text box is open, the window's inputs it takes
	 * (the pointer, the keys with their repeats, an input method's text) wait
	 * for it here, oldest first; the keys then do not reach the main loop's.
	 */
	int box_open;
	struct kl_window_event box_events[NOTES_BOX_EVENTS];
	unsigned box_count;

	/*
	 * ws177-p012: the box's rectangle in the window, and the finger that
	 * went down on it (box_touching): its touches are the box's alone, so
	 * that a finger moves the caret and selects instead of moving the page.
	 */
	struct kl_rect box_rect;
	int box_touching;
	int32_t box_touch_id;
};

/*
 * One rectangle of the toolbar that does something when pressed.
 */
struct notes_button {
	int32_t x;
	int32_t y;
	int32_t width;
	int32_t height;
	uint32_t action;
};

/*
 * What the toolbar and the menus show of Notes' state: the tool (its
 * NOTES_ACTION_PEN, _HIGHLIGHTER or _ERASER) and whether the eraser cuts
 * parts, the colour's and the width's index, the page and the count,
 * whether undo and redo can go, whether there are unsaved changes and
 * fullscreen, whether one finger writes (the toolbar's Finger,
 * ws081-p015), and a status line.  ws175-p008: whether the page takes an
 * inserted image, and whether an object is chosen, its image can be
 * replaced and it has an edit to reset.
 */
struct notes_ui_state {
	unsigned tool;
	int erase_parts;
	unsigned color;
	unsigned width;
	size_t page;
	size_t page_count;
	int can_undo;
	int can_redo;
	int dirty;
	int fullscreen;
	int finger_write;
	const char *status;
	int can_insert;
	int selected;
	int can_replace;
	int can_reset;

	/*
	 * ws175-p008: whether the chosen object is a line of text and its words
	 * can be changed; the font's name the toolbar shows (the Text tool's, the
	 * box's, the chosen text's) and the size in points (0: not shown).
	 */
	int text_selected;
	int can_edit_text;
	const char *font_label;
	float text_size;

	/* ws177-p011: whether a clean copy was saved this session, which File > Open Clean Copy opens. */
	int can_open_clean;
};

/*
 * The toolbar: a picture drawn on the CPU (B8G8R8A8, the window's width by
 * NOTES_TOOLBAR_HEIGHT) that the renderer shows at the top, the buttons on
 * it, and the font its labels are drawn with.
 */
struct notes_ui {
	/* The font file and face (NULL without a font: the buttons are drawn without labels). */
	void *font_data;
	size_t font_size;
	struct truetype_face *face;
	unsigned font_pixels;
	int ascent;

	/*
	 * The picture: its size, its pixels (borrowed from the renderer) and
	 * their row pitch, and whether the layout draws into it (0 while it only
	 * measures the card).
	 */
	uint32_t width;
	uint32_t height;
	unsigned char *pixels;
	size_t pitch;
	int drawing;

	/* The buttons, as last laid out. */
	struct notes_button buttons[NOTES_BUTTONS];
	unsigned button_count;
};

/*
 * The text box (box.c, ws175-p008 with ws079-p017): libkeiland's widget
 * over the page, drawn on the renderer's overlay -- a field for a line's
 * words (NOTES_BOX_LINE), or a text area for the words put on a page
 * (NOTES_BOX_AREA, several lines).  It holds its widgets' state, its own
 * font, the canvas over the overlay, the words as they were when it was
 * opened, whether it is open and its rectangle in the window, the
 * rectangle it covered when last drawn (with its shadow), what the
 * widget reported since taken (KL_FIELD_*), and whether it wants another
 * frame (keys typed faster than the widget takes them wait for the next
 * frames, one at a time).
 */
#define NOTES_BOX_LINE		0U
#define NOTES_BOX_AREA		1U
struct notes_box {
	struct kl_ui *ui;
	struct kl_text text;
	int font_ready;
	struct kl_canvas canvas;
	int canvas_ready;
	unsigned shape;
	struct kl_field field;
	struct kl_text_area area;
	char initial[KL_TEXT_AREA_MAX];
	int open;
	struct kl_rect rect;
	struct kl_rect drawn;
	unsigned reported;
	int again;
};

/*
 * One draw call of a frame: a pipeline, a run of vertices, and whether it
 * is clipped, with the rectangle it is clipped to (x, y, width, height).
 * A texture draw also names its picture (NOTES_TEXTURE_*).
 */
struct notes_draw {
	unsigned pipe;
	unsigned texture;
	uint32_t first;
	uint32_t count;
	int clipped;
	int32_t clip[4];
};

/*
 * The geometry of one frame, built on the CPU (geometry.c) and handed to
 * the renderer: the vertices, the draw calls in order, and the clipping
 * the draws added next take.
 */
struct notes_frame {
	float *vertices;
	size_t vertex_count;
	size_t vertex_capacity;
	struct notes_draw *draws;
	size_t draw_count;
	size_t draw_capacity;
	int clipped;
	int32_t clip[4];
	int error;
};

/*
 * Where the page is in the window: its top left in pixels, and pixels per point.
 */
struct notes_view {
	float x;
	float y;
	float scale;
};

/*
 * One swapchain image the renderer draws into; the image is the swapchain's.
 */
struct notes_target {
	VkImage image;
	VkImageView view;
	VkFramebuffer framebuffer;
	VkSemaphore rendered;
};

/*
 * The Vulkan objects of the window.
 *
 * They are made once the window is configured; the swapchain, the stencil
 * buffer and the toolbar's image are made again when the window's size
 * changes.
 */
struct notes_renderer {
	/* The instance, the window's surface, the device and its queue. */
	VkInstance instance;
	VkSurfaceKHR surface;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	uint32_t family;
	VkPhysicalDeviceMemoryProperties memory;

	/* The swapchain, its format and extent, and one target per image. */
	VkSwapchainKHR swapchain;
	VkFormat format;
	VkExtent2D extent;
	struct notes_target *targets;
	uint32_t count;

	/* The stencil buffer the strokes are filled through, shared by every target. */
	VkFormat stencil_format;
	VkImageAspectFlags stencil_aspect;
	VkImage stencil;
	VkDeviceMemory stencil_memory;
	VkImageView stencil_view;

	/* The pass, the pipelines and what they bind: one set for each picture (NOTES_TEXTURE_*). */
	VkRenderPass pass;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkPipeline pipes[NOTES_PIPES];
	VkDescriptorPool descriptor_pool;
	VkDescriptorSet sets[NOTES_TEXTURES];
	VkSampler sampler;

	/*
	 * The page's picture: the page and its finished strokes, drawn by the
	 * same pipelines through two passes that differ only in what they start
	 * from -- a cleared picture, or the picture as the last frame left it
	 * (to add strokes on top) -- and sampled by the frame.  Its size in
	 * pixels (0: not made), and a serial that grows each time it is made
	 * again, which tells the caller that its content is gone.  The picture
	 * has a stencil buffer of its own, of its size (ws081-p013: a zoomed
	 * page is larger than the window's).
	 */
	VkRenderPass page_clear_pass;
	VkRenderPass page_load_pass;
	VkImage page;
	VkDeviceMemory page_memory;
	VkImageView page_view;
	VkImage page_stencil;
	VkDeviceMemory page_stencil_memory;
	VkImageView page_stencil_view;
	VkFramebuffer page_framebuffer;
	uint32_t page_width;
	uint32_t page_height;
	unsigned long page_serial;

	/*
	 * The toolbar's image: host-written and linear, its rows (mapped for as
	 * long as the image lives) and their pitch, and whether it has left its
	 * first layout.
	 */
	VkImage toolbar;
	VkDeviceMemory toolbar_memory;
	VkImageView toolbar_view;
	unsigned char *toolbar_pixels;
	size_t toolbar_pitch;
	int toolbar_ready;

	/*
	 * The background's image, like the toolbar's (host-written, linear,
	 * mapped): the page of the PDF the notebook writes on as the host drew
	 * it, its size (0: not made) and whether it has left its first layout.
	 */
	VkImage background;
	VkDeviceMemory background_memory;
	VkImageView background_view;
	unsigned char *background_pixels;
	size_t background_pitch;
	uint32_t background_width;
	uint32_t background_height;
	int background_ready;

	/*
	 * The overlay's image (ws175-p008), like the background's: the text box
	 * as the host drew it at the window's size.
	 */
	VkImage overlay;
	VkDeviceMemory overlay_memory;
	VkImageView overlay_view;
	unsigned char *overlay_pixels;
	size_t overlay_pitch;
	uint32_t overlay_width;
	uint32_t overlay_height;
	int overlay_ready;

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

/* The window (window.c). */
int notes_window_open(struct notes_window *window, uint32_t width, uint32_t height, int fullscreen);
int notes_window_dispatch(struct notes_window *window, int timeout);
void notes_window_close(struct notes_window *window);
void notes_window_input(struct notes_window *window, const struct notes_input *input);
void notes_window_set_title(struct notes_window *window, const char *title);
void notes_window_set_fullscreen(struct notes_window *window, int fullscreen);
void notes_window_box(struct notes_window *window, int open);
void notes_window_box_rect(struct notes_window *window, const struct kl_rect *rect);
uint64_t notes_clock(void);

/* The pen through the tablet protocol (tablet.c). */
void notes_tablet_input(struct notes_window *window, const struct kl_window_event *event);

/* The menus (menu.c). */
int notes_menu_open(struct notes_window *window);
void notes_menu_refresh(struct notes_window *window, const struct notes_ui_state *state);
void notes_menu_close(struct notes_window *window);
void notes_menu_chosen(struct notes_window *window, const struct kl_window_event *event);

/* The drawing (render.c). */
VkResult notes_renderer_open(struct notes_renderer *renderer, struct notes_window *window);
VkResult notes_renderer_resize(struct notes_renderer *renderer, uint32_t width, uint32_t height);
VkResult notes_renderer_page(struct notes_renderer *renderer, uint32_t width, uint32_t height);
VkResult notes_renderer_draw(struct notes_renderer *renderer, const struct notes_frame *frame, const struct notes_frame *page_frame, int page_clear);
void notes_renderer_toolbar(struct notes_renderer *renderer, unsigned char **pixels, size_t *pitch);
VkResult notes_renderer_background(struct notes_renderer *renderer, uint32_t width, uint32_t height, unsigned char **pixels, size_t *pitch);
VkResult notes_renderer_overlay(struct notes_renderer *renderer, uint32_t width, uint32_t height, unsigned char **pixels, size_t *pitch, int *made);
void notes_renderer_close(struct notes_renderer *renderer);

/* The frame's geometry (geometry.c). */
void notes_frame_begin(struct notes_frame *frame);
void notes_frame_free(struct notes_frame *frame);
void notes_frame_rect(struct notes_frame *frame, float x, float y, float width, float height, uint32_t color);
void notes_frame_gradient(struct notes_frame *frame, float x, float y, float width, float height, uint32_t top, uint32_t bottom);
void notes_frame_clip(struct notes_frame *frame, int enabled, float x, float y, float width, float height);
void notes_frame_polygon(struct notes_frame *frame, const struct pdf_point *points, size_t count, const struct notes_view *view, uint32_t color);
void notes_frame_texture(struct notes_frame *frame, unsigned texture, float x, float y, float width, float height);
void notes_view_layout(struct notes_view *view, uint32_t width, uint32_t height, float page_width, float page_height);

/* The image files put on a page (picture-file.c, ws175-p008). */
int notes_picture_load(struct notes_document *document, const char *path, struct notes_image **image);

/* The text box (box.c, ws175-p008). */
int notes_box_open(struct notes_box *box, unsigned shape, const struct kl_rect *rect, const char *text);
void notes_box_close(struct notes_box *box);
void notes_box_input(struct notes_box *box, const struct kl_window_event *event);
int notes_box_draw(struct notes_box *box, struct notes_renderer *renderer, struct kl_window *window, uint64_t now_us);
unsigned notes_box_take(struct notes_box *box);
const char *notes_box_text(const struct notes_box *box);
const char *notes_box_initial(const struct notes_box *box);
void notes_box_revert(struct notes_box *box);
void notes_box_focus(struct notes_box *box);
int notes_box_hit(const struct notes_box *box, float x, float y);
void notes_box_free(struct notes_box *box);

/* The toolbar (ui.c). */
int notes_ui_open(struct notes_ui *ui, const char *font_path);
void notes_ui_close(struct notes_ui *ui);
void notes_ui_draw(struct notes_ui *ui, unsigned char *pixels, size_t pitch, uint32_t width, uint32_t height, const struct notes_ui_state *state);
uint32_t notes_ui_hit(const struct notes_ui *ui, float x, float y);
uint32_t notes_ui_color(unsigned tool, unsigned index);
float notes_ui_width(unsigned tool, unsigned index);

#endif
