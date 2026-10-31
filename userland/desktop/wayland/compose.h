/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Vulkan state of window mode, shared by compose.c and import.c.
 *
 * The device, pipelines and descriptor pool live as long as the compositor.
 * The display surface and its swapchain (vkdemo's standard display-plane
 * selection, userland/tests/vkdemo/display.c) exist in window mode; closing
 * them (the greeter's hand-over, the exit) returns the display lease.  One frame is in flight at a time; its
 * fence is exported as an fd the event loop polls, and the buffers and frame
 * callbacks the frame used are held until it signals.
 */

#ifndef KWL_COMPOSE_H
#define KWL_COMPOSE_H

#include "kwl.h"

#include <vulkan/vulkan.h>
#include <pthread.h>

#include "../../tests/vkdemo/display.h"
#include "displays.h"
#include "draw-list.h"
#include "render-thread.h"
#include "scene.h"

/* Bound the swapchain images and the buffers one frame may sample. */
/* The most displays the compositor follows at once (ws113-p004a). */
#define KWL_COMPOSE_DISPLAYS	8U
/* The room of a display's name (VkDisplayPropertiesKHR's displayName, cut). */
#define KWL_COMPOSE_NAME	64U
#define KWL_SWAPCHAIN_MAX	8U
#define KWL_FRAME_WINDOWS	64U

/* The most images the game mode's display swapchain lends a client (scanout.c). */
#define KWL_GAME_IMAGES		8U

/* The longest list of an output's windows the log keeps (KWL RENDER, ws113-p007). */
#define KWL_RENDER_LOG		256U

/* The vertices of a quad: two triangles of a triangle list. */
#define KWL_QUAD_VERTICES	6U

/* The glass look's push constants: six vec4 (see shaders/panel.frag). */
#define KWL_PANEL_CONSTANTS	24U

/*
 * The immutable glass, shadow, image, solid, ring, text and blur modes, and
 * the dark glass (mode -1), which has a variant of its own so that light
 * glass leaves out the darkening (ws211-p011).
 */
#define KWL_PANEL_MODES		8U

/* The variant of the dark glass among the panel pipelines. */
#define KWL_PANEL_DARK_GLASS	7U

/*
 * The shapes whose insides are drawn by themselves, without working out
 * the distance to the edge, and without blending when opaque (glass.c): their place
 * among the inside pipelines and shaders.h's kwl_panel_inside_variants.
 */
#define KWL_PANEL_INSIDE_GLASS		0U
#define KWL_PANEL_INSIDE_IMAGE		1U
#define KWL_PANEL_INSIDE_SOLID		2U
#define KWL_PANEL_INSIDE_DARK_GLASS	3U
#define KWL_PANEL_INSIDE_SHADOW		4U
#define KWL_PANEL_INSIDE_MAP		5U
#define KWL_PANEL_INSIDE_CROSSFADE	6U
#define KWL_PANEL_INSIDES		7U

/* Bound the buffers that hold a descriptor set at once. */
#define KWL_DESCRIPTOR_MAX	512U

/* The window-mode background, a dark blue-grey (0x20, 0x30, 0x40). */
#define KWL_BACKGROUND_RED	(32.0f / 255.0f)
#define KWL_BACKGROUND_GREEN	(48.0f / 255.0f)
#define KWL_BACKGROUND_BLUE	(64.0f / 255.0f)

/*
 * How a window is drawn (design D10): an opaque quad, a quad blended by its
 * alpha, and later effects that read what is already drawn under it.
 */
enum kwl_draw {
	KWL_DRAW_OPAQUE,
	KWL_DRAW_ALPHA
};

/* A buffer's image as window mode samples it. */
struct kwl_import {
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	VkDescriptorSet set;
	/* The same image sampled linearly, for drawing it smaller (Wiseview). */
	VkDescriptorSet linear_set;
	uint32_t width;
	uint32_t height;
	enum kwl_draw draw;
	/*
	 * A host-written image (wl_shm, the arrow, the glass look's pictures):
	 * the CPU writes a separate linear staging image through map, and the
	 * frame copies it into the sampled image above once written is set.
	 * The sampled image is never mapped, so a GPU may keep a converted copy
	 * of it until the next copy.  host_next links every host image.
	 */
	void *map;
	VkDeviceSize row_pitch;
	VkImage staging;
	VkDeviceMemory staging_memory;
	unsigned written;
	/* Counts the staging's writes (kwl_host_image_written), so that a copy of it can tell it is stale. */
	uint32_t version;
	/*
	 * The staging's rows written since the last take (last excluded): the
	 * frame that takes the image copies only those rows.  Empty
	 * when first equals last.
	 */
	uint32_t dirty_first;
	uint32_t dirty_last;
	struct kwl_import *host_next;
	/* A client's image waiting for its move to the general layout in the next frame (ws099-p016). */
	unsigned layout_pending;
	/*
	 * The import's own number among every image the compositor made
	 * (ws213-p003, p001 4.7): no two imports share it, so a record that
	 * reuses the address of one released earlier is told apart from it
	 * (the cursor plane's held images).  0 for an import not made yet.
	 */
	uint64_t generation;
	/*
	 * The task that moves the image to its layout (struct kwl_compose's
	 * tasks), 0 for none: the drawings given after it come after it in the
	 * queue, but the cursor thread is given the image only once the task
	 * has completed, and the image is let go only after it has.
	 */
	uint64_t task;
};

/* The glass look's images and glyphs (glass.c). */
struct kwl_glass;

struct kwl_compose;

/* Releases the objects of a record let go (kwl_compose_bury), once nothing given before uses them, and clears it. */
typedef void kwl_grave_destroy_fn(struct kwl_compose *compose, struct kwl_import *import);

/*
 * An image let go while a drawing or a task given before may still use it
 * (compose.c): a copy of its record (the record's own place is cleared and
 * may be used again at once; nothing reads a record from another thread,
 * the lists' copies hold their own values), how many drawings had been
 * given then (drawings) and how to release its objects.  They are released
 * once that many drawings have completed and the record's own task has.
 */
struct kwl_grave {
	struct kwl_grave *next;
	uint64_t drawings;
	struct kwl_import record;
	kwl_grave_destroy_fn *destroy;
};

/* One small image of the backdrop (backdrop.c): drawn into by the small pass, and sampled linearly. */
struct kwl_backdrop_target {
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	VkFramebuffer framebuffer;
	VkDescriptorSet set;
};

/*
 * The backdrop of the glass (backdrop.c, ws035-p057): the scene under a
 * window drawn small and blurred.  state says whether it was tried, is
 * ready or cannot be made on this device; the two images take turns in the
 * blur; pass draws them (compose->pass_load takes the output's pass up again).  It is
 * made the first time a frame needs it and lives as long as the output.
 */
struct kwl_backdrop {
	unsigned state;
	uint32_t width;
	uint32_t height;
	VkRenderPass pass;
	struct kwl_backdrop_target targets[2];
};

/*
 * The glass maps (glass-map.c): how many there are, and the values that
 * tell one from another (a glass's colour and opacity, whether it is
 * lifted and darkened, its sheen and the height and top of its box when it
 * has one, and a whole-output picture's number, 0 for glass).
 */
#define KWL_GLASS_MAPS		8U
#define KWL_GLASS_MAP_KEY	10U

/*
 * The pictures among the maps (the last value of a key, 0 for glass):
 * App Home's stage over the whole output, a soft shadow around a box (its
 * own size, drawn anywhere), and Wiseview's ground (the wallpaper, and its
 * glass, each under Wiseview's shade, and both side by side in one image
 * twice the output's width).  How many shadows have one.
 */
#define KWL_GLASS_PICTURE_STAGE	1U
#define KWL_GLASS_PICTURE_SHADOW	2U
#define KWL_GLASS_PICTURE_WISEVIEW_WALL	3U
#define KWL_GLASS_PICTURE_WISEVIEW_GLASS	4U
#define KWL_GLASS_PICTURE_WISEVIEW_GROUND	5U
#define KWL_SHADOW_MAPS		12U

/* How many keys without a map the maps remember (glass-map.c). */
#define KWL_GLASS_CANDIDATES	16U

/* Draws a whole-output picture into its map (glass-map.c), as the frame would draw it on the output. */
typedef void (*kwl_glass_picture_draw)(struct kwl_server *server, struct kwl_list *command);

/*
 * One map: a glass's colour worked out for every output pixel, or a
 * whole-output picture that does not change (with what draws it).  Its
 * key; whether it is wanted or drawn; which blurred wallpaper (its generation and version) and
 * size it was drawn from; the last frame that used it; and the image the
 * frame reads (nearest sampling).
 */
struct kwl_glass_map {
	unsigned state;
	float key[KWL_GLASS_MAP_KEY];
	kwl_glass_picture_draw draw;
	uint64_t source;
	uint32_t version;
	uint32_t width;
	uint32_t height;
	uint64_t used;
	uint32_t image_width;
	uint32_t image_height;
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	VkFramebuffer framebuffer;
	VkDescriptorSet set;
};

/*
 * The glass maps of the output: the passes that draw them (every pixel
 * written, or cleared first for a shadow) and the output's size, made the
 * first time one is drawn and gone with the output's size; failed when the
 * device could not make them (everything is then drawn as before).
 */
struct kwl_glass_maps {
	unsigned failed;
	uint32_t width;
	uint32_t height;
	VkRenderPass pass;
	VkRenderPass clear_pass;
	struct kwl_glass_map maps[KWL_GLASS_MAPS];
	struct kwl_glass_map shadows[KWL_SHADOW_MAPS];
	/*
	 * The keys asked for lately that have no map yet, each with the frame
	 * that asked (a ring, next the slot to write): a key takes a map only
	 * when another frame asks for it again, so that a key seen once (a
	 * shape on its way in an animation) never takes the map of a lasting one.
	 */
	float candidates[KWL_GLASS_CANDIDATES][KWL_GLASS_MAP_KEY];
	uint64_t candidate_frames[KWL_GLASS_CANDIDATES];
	unsigned candidate_next;
};

/*
 * The pictures of the layers (layer-cache.c, ws211-p002): App Home's content,
 * the desktop layer, and the window a dock or an undock animates
 * (ws211-p007), each an image of the output's size drawn into offscreen
 * and sampled linearly.  KWL_LAYER_* name them.
 */
#define KWL_LAYER_HOME		0U
#define KWL_LAYER_DESKTOP	1U
#define KWL_LAYER_WINDOW	2U
#define KWL_LAYERS		3U

/* One layer's picture: its image and memory, view, framebuffer of the layers' pass, and descriptor set. */
struct kwl_layer_image {
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	VkFramebuffer framebuffer;
	VkDescriptorSet set;
};

/*
 * The layers' pictures (layer-cache.c, ws211-p002): made with the output
 * (ready, or unavailable for the output's life) in a generation of their
 * own, the pass that clears and draws one, and for each the key it was
 * drawn with and whether it is drawn (valid).  inflight names the pictures
 * the submission in flight draws.  Home's picture remembers what it was
 * drawn from (the wallpaper's version, the tiles, the page, the accent and
 * the output's size).  decided_anim is the animation (home_start_ms) whose
 * first frame decided to use the pictures (using, until a frame could not)
 * and drew the desktop's, noting the windows it drew by their map order;
 * home_later says Home's picture is drawn in the opening's second frame.
 * use_pictures and use_drawn count an animation's frames by how they were
 * drawn; bake_draws and bake_us are the last drawing's draws and recording
 * time, and bake_times the starts of the last two drawings on their own
 * (two a second at most).  anim_decided is the window's animation
 * (server->anim_serial) whose first frame decided whether its frames use
 * the desktop's and the window's pictures (anim_using, until a frame could
 * not), and anim_rect the rectangle of the window's picture its body was
 * drawn in (ws211-p007).  home_bounds is the box (left, top, right,
 * bottom, in the picture's pixels) of everything Home's picture drew, so
 * a frame draws only that part of it (outside it the picture is clear);
 * home_bounded says it is known.
 */
struct kwl_layers {
	unsigned ready;
	unsigned failed;
	uint64_t generation;
	uint32_t width;
	uint32_t height;
	VkRenderPass pass;
	struct kwl_layer_image images[KWL_LAYERS];
	uint64_t keys[KWL_LAYERS];
	unsigned valid[KWL_LAYERS];
	unsigned inflight;
	uint32_t home_wallpaper;
	uint32_t home_tiles;
	unsigned home_page;
	int32_t home_accent;
	uint32_t home_width;
	uint32_t home_height;
	uint64_t decided_anim;
	unsigned using;
	unsigned home_later;
	unsigned noting;
	unsigned window_count;
	uint64_t window_orders[KWL_FRAME_WINDOWS];
	unsigned use_pictures;
	unsigned use_drawn;
	unsigned bake_draws;
	uint64_t bake_us;
	uint64_t bake_times[2];
	uint64_t anim_decided;
	unsigned anim_using;
	int32_t anim_rect[4];
	float home_bounds[4];
	unsigned home_bounded;
};

/* How many new client images wait at most for their move to the general layout in the next frame (ws099-p016). */
#define KWL_LAYOUTS_MAX		32U

/* The displays shown besides the output (heads.c, ws113-p004b): one for each display followed but the output's. */
#define KWL_HEADS		(KWL_COMPOSE_DISPLAYS - 1U)

/*
 * A display shown besides the output (heads.c, ws113-p004b): the output
 * is the desktop's display (the anchor), and each head shows the same
 * desktop (mirror) or its own part of the logical plane (extended).
 *
 * A head has its own surface and swapchain on its display, a view, a
 * framebuffer and a present semaphore for each swapchain image, and the
 * semaphore of its acquire.  `x`, `y`, `width` and `height` are its
 * rectangle of the plane; `refresh` its mode's refresh in mHz.  `dirty`
 * asks an extended head to be drawn at the next frame (it shows the
 * wallpaper, which does not change between frames), `wallpaper` the
 * wallpaper image it shows.  `in_frame` and `image` say that it acquired
 * an image for the frame being recorded.  `lost` marks a head whose
 * display went (an acquire or a present said so): the next look closes
 * it.  `global` is its wl_output global's name (0: none).
 */
struct kwl_head {
	unsigned open;
	unsigned lost;
	VkDisplayKHR display;
	char name[KWL_COMPOSE_NAME];
	struct vkdemo_display output;
	VkImageView views[KWL_SWAPCHAIN_MAX];
	VkFramebuffer framebuffers[KWL_SWAPCHAIN_MAX];
	VkSemaphore rendered[KWL_SWAPCHAIN_MAX];
	VkSemaphore acquired;
	int32_t x;
	int32_t y;
	uint32_t width;
	uint32_t height;
	uint32_t refresh;
	unsigned dirty;
	VkImage wallpaper;
	unsigned showed;
	unsigned in_frame;
	uint32_t image;
	uint32_t global;
};

struct kwl_cursor_thread;


/*
 * Where one image of the game mode's display swapchain is (scanout.c).
 */
enum kwl_game_state {
	/* The swapchain has it: shown, queued for the display, or not acquired yet. */
	KWL_GAME_CHAIN = 0,
	/* The present thread acquired it: the client's to draw into, or drawn and committed. */
	KWL_GAME_FREE,
	/* Given to the present thread to be presented. */
	KWL_GAME_PRESENTING
};

/*
 * The game mode's display swapchain, whose images are lent to the client
 * of a fullscreen game or video (scanout.c, game.c): the display surface
 * and FIFO swapchain the display thread made at the display's mode with
 * images that may go out (output, in format), the uses they allow, the
 * client's image count it was made for, the present thread's acquire
 * fence, and each image's exported fd until it is sent (-1 after).  The
 * main thread's part: the lend's serial, each image's state, the buffer
 * that holds it (tagged by the client, NULL before) and whether the game
 * mode holds that buffer for the swapchain (held), the image in flight on
 * the present thread (-1 for none), when the lend was offered, and the
 * lent frames presented.  The main thread owns the record; the display
 * thread writes only the Vulkan part, inside its jobs.
 */
struct kwl_game_chain {
	struct vkdemo_display output;
	VkImageUsageFlags usage;
	uint32_t wanted;
	VkFence acquired;
	int fds[KWL_GAME_IMAGES];
	uint32_t serial;
	enum kwl_game_state state[KWL_GAME_IMAGES];
	struct kwl_object *buffers[KWL_GAME_IMAGES];
	unsigned held[KWL_GAME_IMAGES];
	int flight;
	uint64_t lent_ms;
	uint64_t shown;
	/* The next game chain whose release the display thread has not done yet (scanout.c's list). */
	struct kwl_game_chain *next_closing;
};

/*
 * What is left of a head the main thread let go (heads.c): its swapchain
 * and surface, its display, and its name, for the display thread to
 * destroy and give back (kwl_heads_rest_destroy).
 */
struct kwl_head_rest {
	struct vkdemo_display output;
	VkDisplayKHR display;
	char name[KWL_COMPOSE_NAME];
};

/*
 * The displays connected, as the display thread read them
 * (kwl_display_list_read): the read's result, how many, and each one's
 * handle, name and physical size.
 */
struct kwl_display_list {
	VkResult result;
	uint32_t count;
	VkDisplayKHR displays[KWL_COMPOSE_DISPLAYS];
	char names[KWL_COMPOSE_DISPLAYS][KWL_COMPOSE_NAME];
	uint32_t widths[KWL_COMPOSE_DISPLAYS];
	uint32_t heights[KWL_COMPOSE_DISPLAYS];
};

/*
 * A head to be made on a display (heads.c): the head chosen for it
 * (slot), the display's place in the last enumeration (index), the
 * display and its name; and what the display thread made: the display's
 * size and refresh, the swapchain's record, and 0 or the errno of making
 * it (kwl_heads_make).
 */
struct kwl_head_opening {
	unsigned slot;
	unsigned index;
	VkDisplayKHR display;
	char name[KWL_COMPOSE_NAME];
	uint32_t width;
	uint32_t height;
	uint32_t refresh;
	struct vkdemo_display output;
	int error;
};

/*
 * A cursor plane taken off its output for its close (cursor-plane.c): the
 * cursor thread's record, the plane's copies the thread reads until it has
 * stopped, and whether the plane showed the cursor.
 */
struct kwl_cursor_parts {
	struct kwl_cursor_thread *thread;
	struct kwl_import *copies[KWL_CURSOR_COPIES];
	unsigned shown;
};

/*
 * The output's cursor plane (cursor-plane.c, ws141-p014): a small
 * VK_KHR_display plane above the output's, with per-pixel premultiplied
 * alpha, that shows the cursor without composing a frame.  Since
 * ws213-p004 the cursor thread (cursor-thread.c) presents it from the
 * cursor's box; this is the main thread's side of it (design 6.2).
 *
 * open says the plane and its thread run; shown that the plane shows the
 * cursor (the frames draw none).  width and height are the plane's extent.
 * copies are the plane's two images of a client's cursor, which the main
 * thread writes and the cursor thread copies from (design 4.7); copy_source,
 * copy_generation and copy_version say which client image and which of its
 * versions copy_index holds.  published_* is what the box was last given
 * (the image, its generation, version and hotspot, and whether the plane
 * shows the cursor), parked whether the main thread parked the thread
 * (the game mode, a sleep's request), and thread the cursor thread.
 */
struct kwl_cursor_plane {
	unsigned open;
	unsigned shown;
	uint32_t width;
	uint32_t height;
	struct kwl_import *copies[KWL_CURSOR_COPIES];
	unsigned copy_valid;
	const struct kwl_import *copy_source;
	uint64_t copy_generation;
	uint32_t copy_version;
	unsigned copy_index;
	unsigned published;
	const struct kwl_import *published_image;
	uint64_t published_generation;
	uint32_t published_version;
	int32_t published_hotspot_x;
	int32_t published_hotspot_y;
	unsigned published_show;
	unsigned parked;
	struct kwl_cursor_thread *thread;
};

/*
 * An output's turn image (turn.c, WS217): the desktop drawn at the logical
 * size, which the turn pass copies turned onto the swapchain's image each
 * frame.  It lives while the output is open at an angle the compositor
 * turns (90 and 270, or 180 where the hardware cannot); image is null
 * otherwise.  frame is the number of the last frame drawn into it (0: its
 * contents are unknown, the next frame draws it whole).
 */
struct kwl_turn {
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	VkFramebuffer framebuffer;
	VkDescriptorSet set;
	uint32_t width;
	uint32_t height;
	unsigned rotation;
	uint64_t frame;
};

/* How many frames' damage is kept, for images that missed that many frames. */
#define KWL_DAMAGE_HISTORY	8U

/* The Vulkan device, the display output and the frame in flight. */
/*
 * How many frames (or layers' pictures) may be recorded and submitted
 * before the oldest completes: each has a slot of its own.
 */
#define KWL_FRAME_SLOTS		KWL_RENDER_FLIGHTS

/*
 * What one submission owns until it completes: the command buffers it is
 * recorded into (its host images' copies, then its drawing), its fence,
 * the client buffers it samples and the frame callbacks it is to send,
 * whether it copies host images, and what its completion applies (the
 * frame's number and start, whether it draws a layer's picture and the
 * layers' generation then, whether it wrote the cursor-free scene).  The
 * joined quads' vertices of the slot are its part of the batch buffer.
 */
struct kwl_frame_slot {
	VkCommandBuffer command;
	VkCommandBuffer copy_command;
	VkFence fence;
	struct kwl_object *held[KWL_FRAME_WINDOWS + 3U];
	struct kwl_object *callbacks;
	unsigned held_count;
	unsigned copies;
	uint64_t number;
	uint64_t start_cycles;
	uint64_t start_ms;
	unsigned bake;
	uint64_t layers_generation;
	unsigned scene_written;
	unsigned flying;
};

struct kwl_compose {
	VkInstance instance;
	VkPhysicalDevice physical;
	/* The display chosen before the OS acquires it for the swapchain, and its name (empty when it has none). */
	VkDisplayKHR display;
	char display_name[KWL_COMPOSE_NAME];
	VkDevice device;
	VkQueue queue;
	/*
	 * The queue's lock: Vulkan wants a queue's use (submit, present, wait
	 * idle, and a device's wait idle over all its queues) from one thread at
	 * a time, and the render thread, the cursor thread and the main thread
	 * all use this queue.  Held only around those calls.
	 */
	pthread_mutex_t queue_lock;
	uint32_t family;
	VkRenderPass pass;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkPipeline pipelines[2];
	VkSampler sampler;
	/*
	 * A quad's two triangles, (0,0) (1,0) (0,1) and (0,1) (1,0) (1,1), as a
	 * vertex buffer: i915's native compiler does not take gl_VertexIndex,
	 * and its executor draws triangle lists, not strips.
	 */
	VkBuffer corners;
	VkDeviceMemory corners_memory;
	/* The glass look's pipeline (shapes, glass, text) and the sampler of its blurred wallpaper. */
	VkPipelineLayout panel_layout;
	VkPipeline panel_pipeline;
	/* Each known shape mode executes only its own fragment operations. */
	VkPipeline panel_pipelines[KWL_PANEL_MODES];
	/*
	 * The insides of glass, image, solid, dark glass and shadow shapes
	 * (KWL_PANEL_INSIDE_*): blended, and without blending for opaque ones.
	 */
	VkPipeline panel_inside_pipelines[KWL_PANEL_INSIDES];
	VkPipeline panel_opaque_pipelines[KWL_PANEL_INSIDES];
	VkSampler linear_sampler;
	struct kwl_glass *glass;
	VkDescriptorPool descriptors;
	VkCommandPool pool;
	/*
	 * The submissions' slots (each its command buffers, fence, holds and
	 * callbacks), and the slot the drawing being recorded goes into.
	 */
	struct kwl_frame_slot slots[KWL_FRAME_SLOTS];
	unsigned walk_slot;
	/*
	 * The display list the drawing writes (draw-list.c, ws213-p003): reset
	 * when a frame's or a layer's drawing begins, and replayed into command
	 * (its copies into copy_command) when the drawing ends, before the
	 * submission.  It keeps its memory from one drawing to the next.
	 */
	struct kwl_list list;
	/* When the drawing into the list began (kwl_cycles), for KWL PERF walk. */
	uint64_t walk_start_cycles;
	/* The generation the next image made gets (import.c, shm.c); it starts at 1, so 0 is no image. */
	uint64_t next_generation;
	/*
	 * The start's images (ws035-p131): while setup_batching is set, a new
	 * host image's move to the general layout is recorded into
	 * setup_command (made by the first one) instead of being submitted and
	 * waited for alone; kwl_host_image_batch_end submits them all and waits
	 * once.  Images made later (clients' wl_shm buffers, cursors) are moved
	 * one by one as before.
	 */
	unsigned setup_batching;
	VkCommandBuffer setup_command;
	/* Every host image, whose written staging the next frame copies into its sampled image (shm.c). */
	struct kwl_import *host_images;
	VkSemaphore acquired;
	struct vkdemo_display output;
	/*
	 * The output's rotation (WS217): the physical mode of its display,
	 * which the swapchain, the mode's refresh and the game mode's claim
	 * take (the logical size, server->width and height, is this mode
	 * turned by the rotation; kwl_output_size_set writes both); the angle
	 * the user chose (displays.conf, or --rotate; 0 for an angle the
	 * display cannot take, the file's line kept); the angle shown; how it
	 * is shown (the display hardware's angle, the scanout's, and the
	 * compositor's turn pass's); whether the display's hardware turns the
	 * picture by 180 (not read yet: ws217-p010); whether the GPU is offered
 * the turn pass without a test's policy (turn_default); the angles the display may
	 * take (KWL_ROTATION_*) and why not the others (KWL_ROTATE_WHY_*).
	 * They change only while the output is closed, at its opening, or with
	 * the render thread parked.
	 */
	uint32_t mode_width;
	uint32_t mode_height;
	unsigned rotation_selected;
	unsigned rotation;
	unsigned scanout_rotation;
	unsigned turn_rotation;
	unsigned hw180;
	unsigned turn_default;
	unsigned rotations;
	unsigned rotate_why;
	/*
	 * The logical size of the image the anchor's frames are drawn into
	 * (WS217): the scene's, the damage's, the layers', the backdrop's and
	 * the mirror's source size.  It is the swapchain's while nothing turns
	 * the picture (compose_targets sets it with the output's targets).
	 */
	uint32_t target_width;
	uint32_t target_height;
	/*
	 * What the output showed when it last opened or turned (review-4 C7):
	 * the logical size and the angles, which each opening compares the
	 * new state with (kwl_output_rotated), and the steps the last opening
	 * took.  compose_display sets the first, the size the look is made at.
	 */
	struct kwl_rotate_state opened;
	unsigned opened_steps;
	/*
	 * The output's turn image (WS217, turn.c), and the turn's passes and
	 * pipeline, made the first time an output turns and kept for the
	 * compositor's life as the output's format is; whether the mirror's
	 * copy from the turn image was said once.
	 */
	struct kwl_turn turn;
	VkRenderPass turn_clear_pass;
	VkRenderPass turn_load_pass;
	VkPipeline turn_pipeline;
	VkDisplayKHR opened_display;
	unsigned mirror_turn_logged;
	/* The last user's angle written to displays.conf: 0, its errno, or -1 when nothing was to be written. */
	int rotate_saved;
	/* The swapchain images can be a copy's source (a test image's capture, ws173-p002). */
	unsigned readback;
	/* Cursor-free scene is optional and never replaces redraws after scene changes. */
	struct kwl_scene scene;
	/* The cursor plane, when the output has one (cursor-plane.c). */
	struct kwl_cursor_plane cursor_plane;
	unsigned output_prepared;
	unsigned output_open;
	VkFormat format;
	VkImageView views[KWL_SWAPCHAIN_MAX];
	VkFramebuffer framebuffers[KWL_SWAPCHAIN_MAX];
	VkSemaphore rendered[KWL_SWAPCHAIN_MAX];
	/* How many submissions are in flight (their slots, oldest first from walk_slot back). */
	unsigned in_flight;
	/* The host images the last submitted frame copied first (KWL DRAWS, ws211-p002). */
	unsigned frame_copies;
	/*
	 * What the list has bound since its drawing began or its last pass
	 * began (ws211-p003 段 1): the pipeline, and the descriptor
	 * set with the layout it was bound with (a set bound with another
	 * layout is not the same binding, review-1 M1).  VK_NULL_HANDLE: not
	 * known, the next draw binds.
	 */
	VkPipeline bound_pipeline;
	VkPipelineLayout bound_layout;
	VkDescriptorSet bound_set;
	/*
	 * The solid quads drawn together (batch.c, ws211-p003 段 2): the
	 * pipeline and the vertex buffer (host-visible, mapped while the
	 * compositor runs; the replay writes the list's vertices to it), how
	 * much of it the frame being recorded used, and
	 * the quads waiting for one draw: their first vertex's byte offset,
	 * their number, the set and the screen they were drawn with, and the
	 * box around them in the pass's pixels (left, top, right, bottom;
	 * ws211-p009, for the damage's rectangles they are drawn in).  ready
	 * once made; off with KWL_BATCH=0 or when it could not be made.
	 * Glyphs are drawn together the same way with their own pipeline
	 * (ws270-p007; text_ready once it is made), in the same buffer: text
	 * says the quads waiting are glyphs.
	 */
	struct {
		unsigned ready;
		unsigned text_ready;
		unsigned text;
		VkPipeline pipeline;
		VkPipeline text_pipeline;
		VkBuffer buffer;
		VkDeviceMemory memory;
		uint8_t *map;
		uint32_t capacity;
		uint32_t used;
		uint32_t first;
		uint32_t count;
		VkDescriptorSet set;
		float screen[2];
		float bounds[4];
		unsigned overflowed;
	} batch;
	/* The layers' generation when the drawing being recorded began (layer-cache.c); its slot keeps it. */
	uint64_t frame_layers_generation;
	/* The layers' pictures (layer-cache.c, ws211-p002). */
	struct kwl_layers layers;
	/* The colours of flat glass worked out beforehand (glass-map.c). */
	struct kwl_glass_maps glass_maps;
	/*
	 * While bounding is set, every draw grows bounds (left, top, right,
	 * bottom, in the pass's pixels, within its scissor) and bounded says
	 * one was drawn (compose-bind.c): what a layer's picture covers.
	 */
	unsigned bounding;
	unsigned bounded;
	float bounds[4];
	/* Descriptor sets images gave back, for the next images (kwl_compose_set_get). */
	VkDescriptorSet spare_sets[KWL_DESCRIPTOR_MAX];
	unsigned spare_count;
	/*
	 * Whether the frame's fence is exported as an fd the event loop polls
	 * (VK_KHR_external_fence_fd); without it the loop asks the fence's
	 * status each pass (the native i915, which has no kernel fences).
	 */
	unsigned fence_fd;
	/* The export entrypoint resolved once for the device while fence_fd is set. */
	PFN_vkGetFenceFdKHR get_fence_fd;
	/*
	 * VK_EXT_display_control (ws113-p004a): the device enabled it when the
	 * library offers it, and its hotplug entry point; NULL elsewhere (a
	 * renderer without it, Linux), where no hotplug is followed.
	 */
	PFN_vkRegisterDeviceEventEXT register_device_event;
	/*
	 * The displays followed (output-switch.c, ws113-p004a): the hotplug
	 * fence registered last (VK_NULL_HANDLE: none), when it was last looked
	 * at; the display the compositor started on (the machine's own when no
	 * name says which is built in); and the displays connected at the last
	 * enumeration with their names and, as bits by their place, those whose
	 * swapchain was refused for the limit of outputs shown at once (tried
	 * again after the next hotplug).
	 */
	VkFence hotplug;
	uint64_t hotplug_checked_ms;
	VkDisplayKHR boot_display;
	VkDisplayKHR displays[KWL_COMPOSE_DISPLAYS];
	char display_names[KWL_COMPOSE_DISPLAYS][KWL_COMPOSE_NAME];
	/* The displays are being read by the display thread for a look of the hotplug's tick (output-switch.c). */
	unsigned listing;
	unsigned display_count;
	uint32_t limited;
	/*
	 * The displays the output moved to that failed at the move's first
	 * frame (BUG-266), a bit for each place of the last enumeration: the
	 * output does not move to them again until the next hotplug.  Unlike
	 * limited, they may still be heads (which light a display another
	 * way).
	 */
	uint32_t move_failed;
	/*
	 * A move not proven yet (BUG-266): switch_proving is 1 from a move
	 * until its first frame is presented; switch_failed is 1 when the
	 * display refused that frame, until the output goes back to
	 * switch_from, the display it left (named switch_from_name).
	 */
	unsigned switch_proving;
	unsigned switch_failed;
	VkDisplayKHR switch_from;
	char switch_from_name[KWL_COMPOSE_NAME];
	/*
	 * The display under the output was lost (unplugged): 1 until the output
	 * moves (output-switch.c), 2 when no display took it (or the list of
	 * displays could not be read) and the next hotplug is waited for; no
	 * frame is drawn meanwhile.
	 */
	unsigned output_lost;
	uint64_t frame_start_cycles;
	uint64_t frame_start_ms;
	/*
	 * The render thread (render-thread.c, ws213-p004): it acquires, records
	 * the list, submits, presents and waits for the fence.  render_started
	 * says it runs.  want_kind and want_number are the want the main thread
	 * last gave it (want_heads its heads), ready the ready the main thread
	 * holds for it (ready_valid: one is held, not answered yet); park_depth
	 * counts the parks the main thread is inside (the thread is parked
	 * while it is not 0).  walk_cycles is the walk of the drawing in
	 * flight, kept for its KWL PERF walk sample at its completion;
	 * frame_number the frame number it was given.  want_ms is when the
	 * want's number last moved, flight_ms when the drawing in flight was
	 * given, done_ms when the last one completed; stall_ms and stall_lines
	 * pace the KWL RENDER stalled lines of a wait for the thread that lasts.
	 */
	struct kwl_render_thread render;
	unsigned render_started;
	enum kwl_render_kind want_kind;
	uint64_t want_number;
	unsigned want_head_count;
	unsigned want_heads[KWL_RENDER_HEADS];
	struct kwl_render_message ready;
	unsigned ready_valid;
	unsigned park_depth;
	/*
	 * The display thread (display-thread.c): one thread of a
	 * lane that makes and destroys the output's swapchain, its surface and
	 * the cursor plane's, takes and gives back the display and asks the
	 * backend about the game mode, while the event loop goes on.
	 * display_started says it runs.  display_busy says a change of the
	 * display runs (from its begin to its end): nothing is wanted of the
	 * render thread meanwhile.  display_parking says its park is asked and
	 * not answered yet; display_step and display_context are what runs
	 * when it is (the render thread parked and drained).  display_inline
	 * counts the callers that run the changes' steps on the event loop's
	 * thread and wait (the end, the hand-over, the seat's pause), as they do
	 * without the thread.  display_in_step counts the steps of a change
 * running on the event loop's thread now (a caller then is part of the
 * change; any other waits for the change's end first).  display_began_ms
 * is when the change began.
	 */
	struct kwl_lane display_lane;
	unsigned display_started;
	unsigned display_busy;
	unsigned display_parking;
	kwl_display_fn *display_step;
	void *display_context;
	unsigned display_inline;
	unsigned display_in_step;
	uint64_t display_began_ms;
	uint64_t walk_cycles;
	uint64_t frame_number;
	uint64_t want_ms;
	uint64_t flight_ms;
	uint64_t done_ms;
	uint64_t stall_ms;
	unsigned stall_lines;
	/*
	 * The main thread's tasks (render-thread.h): command buffers of
	 * task_pool, which only the main thread uses, that the render thread
	 * submits with a fence of their own before the drawings given after
	 * them.  task_command is the task being recorded (numbered task_open,
	 * 0 for none), given at the next flush; task_given is the number of
	 * the last task given, task_done that of the last whose completion was
	 * applied (tasks complete in their order, so every task up to it has).
	 * task_error is a failure to give one, which ends the compositor at
	 * the next poll.
	 */
	VkCommandPool task_pool;
	VkCommandBuffer task_command;
	uint64_t task_open;
	uint64_t task_given;
	uint64_t task_done;
	VkResult task_error;
	/*
	 * The drawings (frames and layers' pictures) given to the render thread
	 * and those whose completion was applied (they complete in their order),
	 * whether one is being written into the list (from its begin to its
	 * giving, or to its failure), and the images let
	 * go meanwhile (kwl_compose_bury), oldest first.
	 */
	uint64_t drawings_given;
	uint64_t drawings_done;
	unsigned drawing_open;
	struct kwl_grave *graves;
	/*
	 * The backdrop (backdrop.c); the framebuffer of the frame being
	 * recorded, which its pass is taken up again on; and the blurred scene
	 * the glass samples from now on in the frame (VK_NULL_HANDLE: the
	 * blurred wallpaper).
	 */
	struct kwl_backdrop backdrop;
	VkFramebuffer framebuffer_now;
	VkDescriptorSet backdrop_set;
	/*
	 * The damage (ws035-p055): the frame number each swapchain image was
	 * last drawn in (0: never since the output opened); each recent
	 * frame's damage (left, top, right, bottom) or the whole output; the
	 * output's pass that keeps the image's pixels (made the first time it
	 * is needed); and the scissor the frame being recorded draws within.
	 */
	uint64_t image_frames[KWL_SWAPCHAIN_MAX];
	int32_t history[KWL_DAMAGE_HISTORY][KWL_DAMAGE_RECTS][4];
	unsigned history_count[KWL_DAMAGE_HISTORY];
	unsigned history_whole[KWL_DAMAGE_HISTORY];
	VkRenderPass pass_load;
	VkRect2D scissor_now;
	/*
	 * The anchor's pass of the frame being recorded (ws211-p009): its
	 * render area (the box around the damage, or the whole output), which
	 * the glass's backdrop takes the pass up again with.
	 */
	VkRect2D pass_area;
	/*
	 * What the frame's list asks of the GPU (KWL PASSES, --log-frames,
	 * ws270-p007): counted when the list goes to the render thread, printed
	 * with the frame's draws when it completes.
	 */
	struct kwl_list_passes frame_passes;
	/*
	 * The damage's rectangles the frame draws apart (ws211-p009 D3,
	 * compose-bind.c): clip_count of them, disjoint, set only while the
	 * anchor's pass on clip_framebuffer is recorded and the damage has two
	 * or more (0 otherwise: a draw is kept to scissor_now alone).  A draw
	 * there is made once for each rectangle it meets, with the scissor cut
	 * to it.  clip_scissor is the scissor the last of them set, valid while
	 * the list's scissor_serial is still clip_serial (no one else set one).
	 */
	VkRect2D clips[KWL_DAMAGE_RECTS];
	unsigned clip_count;
	VkFramebuffer clip_framebuffer;
	VkRect2D clip_scissor;
	uint64_t clip_serial;
	unsigned clip_scissor_valid;
	/* Offscreen backdrop passes use scaled coordinates and must retain the complete scene. */
	unsigned backdrop_recording;
	/*
	 * The clients' images imported since the last frame (ws099-p016): their
	 * move to the general layout is recorded at the start of the next
	 * frame's commands (kwl_import_layouts_record) instead of being
	 * submitted and waited for at the import, and they are forgotten once
	 * that frame is submitted (kwl_import_layouts_done).
	 */
	struct kwl_import *layouts[KWL_LAYOUTS_MAX];
	unsigned layout_count;
	/*
	 * The displays shown at once (heads.c, ws113-p004b): the mode
	 * (KWL_DISPLAYS_EXTENDED or _MIRROR), the output's place in the logical
	 * plane, the heads, the choice displays.conf keeps (read once at the
	 * start, written when a choice is applied), a display kept off by a
	 * choice of its own (the built-in panel under a closed lid,
	 * ws052-p012; VK_NULL_HANDLE: none), whether the heads are to be
	 * brought in line with the displays at the next look (the output was
	 * opened again), the next wl_output global name a head gets, and
	 * whether the mirror's missing copy source was logged.
	 */
	unsigned display_mode;
	int32_t output_x;
	int32_t output_y;
	struct kwl_head heads[KWL_HEADS];
	struct kwl_display_config config;
	unsigned config_loaded;
	VkDisplayKHR kept_off;
	unsigned heads_stale;
	uint32_t next_global;
	unsigned mirror_unsupported_logged;
	/* The heads' windows of the frame being recorded (bottom to top; ws113-p007), and how many. */
	struct kwl_object **frame_heads;
	unsigned frame_head_count;
	/* Each output's windows of the last frame, as logged (KWL RENDER). */
	char render_lists[KWL_PLANE_SLOTS][KWL_RENDER_LOG];
};

/* Host-written images (shm.c), sampled with the given sampler. */
VkResult kwl_host_image_create(struct kwl_compose *compose, uint32_t width, uint32_t height, VkSampler sampler, struct kwl_import *import);
void kwl_host_image_release(struct kwl_compose *compose, struct kwl_import *import);
void kwl_host_image_move(struct kwl_compose *compose, struct kwl_import *from, struct kwl_import *to);
void kwl_host_image_written(struct kwl_import *import);
void kwl_host_image_written_rows(struct kwl_import *import, uint32_t first, uint32_t last);
void kwl_host_images_take(struct kwl_compose *compose, struct kwl_list *list);
unsigned kwl_host_images_record(const struct kwl_list *list, VkCommandBuffer command);
void kwl_host_image_batch_begin(struct kwl_compose *compose);
VkResult kwl_host_image_batch_end(struct kwl_compose *compose);

/* The glass look (glass.c). */
int kwl_glass_open(struct kwl_server *server);
void kwl_glass_close(struct kwl_server *server);
void kwl_glass_draw(struct kwl_server *server, struct kwl_list *command, struct kwl_object **windows, unsigned count);
void kwl_glass_draw_drag_badge(struct kwl_server *server, struct kwl_list *command);
void kwl_glass_draw_drag_mark(struct kwl_server *server, struct kwl_list *command, unsigned state);
void kwl_glass_draw_head(struct kwl_server *server, struct kwl_list *command, struct kwl_object **windows, unsigned count);
void kwl_compose_cursor(struct kwl_server *server, struct kwl_list *command);
void kwl_compose_count_draw(struct kwl_server *server, VkPipeline pipeline, VkDescriptorSet set, unsigned shape);

/* Counts the pixels a draw covers inside the pass's area, blended or not, for the frame's log; returns them. */
uint64_t kwl_compose_count_pixels(struct kwl_server *server, float x, float y, float width, float height, int blended);

/* Counts a draw of a kind and its pixels for the frame's log (KWL MODES). */
void kwl_compose_count_kind(struct kwl_server *server, unsigned kind, uint64_t pixels);
void kwl_compose_bind(struct kwl_server *server, struct kwl_list *command, VkPipeline pipeline, VkPipelineLayout layout, VkDescriptorSet set);
void kwl_compose_bind_forget(struct kwl_compose *compose);
void kwl_compose_bounds_begin(struct kwl_compose *compose);
int kwl_compose_bounds_end(struct kwl_compose *compose, float *bounds);
int kwl_compose_clip_visible(struct kwl_compose *compose, float x, float y, float width, float height);
void kwl_compose_clip_draw(struct kwl_server *server, struct kwl_list *command, float x, float y, float width, float height, uint32_t vertices);
void kwl_compose_clip_draw_rect(struct kwl_server *server, struct kwl_list *command, const VkRect2D *rect, uint32_t vertices);
VkResult kwl_compose_pipeline_input(struct kwl_compose *compose, VkShaderModule vertex, VkShaderModule fragment, VkPipelineLayout layout, const VkPipelineVertexInputStateCreateInfo *input, VkPipeline *pipeline);
void kwl_batch_open(struct kwl_server *server);
void kwl_batch_close(struct kwl_compose *compose);
void kwl_batch_begin(struct kwl_compose *compose);
int kwl_batch_add(struct kwl_server *server, struct kwl_list *command, const float *constants, VkDescriptorSet set);
int kwl_batch_add_text(struct kwl_server *server, struct kwl_list *command, const float *constants, VkDescriptorSet set);
void kwl_batch_flush(struct kwl_server *server, struct kwl_list *command);
void kwl_compose_count_commands(struct kwl_server *server, unsigned pushes, unsigned draws);
VkResult kwl_compose_submit_quiet(struct kwl_server *server);
int kwl_compose_render_start(struct kwl_server *server);
int kwl_compose_want_layer(struct kwl_server *server);
int kwl_compose_host_writable(struct kwl_server *server);
void kwl_compose_park(struct kwl_server *server);
void kwl_compose_want_nothing(struct kwl_server *server);
void kwl_compose_output_let_go(struct kwl_server *server, struct kwl_head_rest *heads, unsigned *head_count);
int kwl_compose_output_take(struct kwl_server *server, unsigned scene_supported, struct kwl_cursor_thread *cursor, VkExtent2D plane_extent, uint32_t plane_index);

/* The display thread and the changes of the display (display-thread.c). */
void kwl_display_start(struct kwl_server *server);
void kwl_display_stop(struct kwl_server *server);
void kwl_display_take(struct kwl_server *server);
void kwl_display_parked(struct kwl_server *server);
int kwl_display_busy(struct kwl_server *server);
int kwl_display_begin(struct kwl_server *server, kwl_display_fn *parked, void *context);
void kwl_display_end(struct kwl_server *server);
void kwl_display_close(struct kwl_server *server, kwl_display_fn *then, void *context);
void kwl_display_open(struct kwl_server *server, kwl_display_fn *then, void *context);
void kwl_display_heads(struct kwl_server *server, struct kwl_head_rest *rests, unsigned rest_count, struct kwl_head_opening *opens, unsigned open_count, kwl_display_fn *then, void *context);
void kwl_display_list_read(struct kwl_server *server, struct kwl_display_list *list);
void kwl_display_enumerate(struct kwl_server *server, struct kwl_display_list *list, kwl_display_fn *then, void *context);
void kwl_display_scanout_open(struct kwl_server *server, struct kwl_game_chain *chain, kwl_display_fn *then, void *context);
void kwl_display_scanout_close(struct kwl_server *server, struct kwl_game_chain *chain, kwl_display_fn *then, void *context);
int kwl_display_output_open(struct kwl_server *server, kwl_display_fn *then, void *context);
int kwl_display_output_close(struct kwl_server *server, kwl_display_fn *then, void *context);
void kwl_compose_unpark(struct kwl_server *server);
void kwl_compose_forget(struct kwl_server *server, unsigned output, unsigned heads_mask);
uint64_t kwl_compose_generation(struct kwl_compose *compose);
VkResult kwl_compose_task_record(struct kwl_compose *compose, VkCommandBuffer *command);
uint64_t kwl_compose_task_current(struct kwl_compose *compose);
void kwl_compose_task_flush(struct kwl_compose *compose);
int kwl_compose_task_ready(struct kwl_compose *compose, const struct kwl_import *import);
int kwl_compose_tasks_busy(struct kwl_compose *compose);
void kwl_compose_bury(struct kwl_compose *compose, struct kwl_import *import, kwl_grave_destroy_fn *destroy);
void kwl_queue_lock(struct kwl_compose *compose);
void kwl_queue_unlock(struct kwl_compose *compose);
void kwl_device_wait_idle(struct kwl_compose *compose);
void kwl_compose_list_begin(struct kwl_compose *compose);
void kwl_glass_glyphs_take(struct kwl_compose *compose);
void kwl_layers_open(struct kwl_server *server);
void kwl_layers_close(struct kwl_compose *compose);
void kwl_layers_complete(struct kwl_server *server, uint64_t generation);
void kwl_layers_inband(struct kwl_server *server, struct kwl_list *command, struct kwl_object **windows, unsigned count);
int kwl_layers_pictured(struct kwl_server *server, const struct kwl_object *surface);
int kwl_layers_home_hidden(struct kwl_server *server);
int kwl_layers_home_bounds(struct kwl_server *server, float *bounds);
void kwl_layers_note(struct kwl_server *server, const struct kwl_object *surface);
int kwl_layers_showing(struct kwl_server *server);
VkDescriptorSet kwl_layers_set(struct kwl_server *server, unsigned which);
void kwl_layers_anim_inband(struct kwl_server *server, struct kwl_list *command, struct kwl_object **windows, unsigned count);
int kwl_layers_anim_showing(struct kwl_server *server, struct kwl_object **windows, unsigned count);
VkDescriptorSet kwl_layers_anim_picture(struct kwl_server *server, float uv[4]);

/* Descriptor sets of the image layout, reused rather than freed (compose.c). */
VkResult kwl_compose_set_get(struct kwl_compose *compose, VkDescriptorSet *result);
void kwl_compose_set_put(struct kwl_compose *compose, VkDescriptorSet set);
int kwl_output_switch(struct kwl_server *server, VkDisplayKHR target);
int kwl_output_move(struct kwl_server *server, const VkDisplayKHR *candidates, const unsigned char *clear_kept, unsigned count, kwl_display_fn *then, void *context);
VkRenderPass kwl_compose_target_pass(const struct kwl_compose *compose, unsigned slot, unsigned load);
VkResult kwl_compose_display_read(struct kwl_server *server, VkDisplayKHR display, uint32_t *width, uint32_t *height, uint32_t *refresh, char *name, size_t size);

/* Gives an image a second, linearly sampled descriptor set (compose.c). */
VkResult kwl_compose_linear_set(struct kwl_compose *compose, struct kwl_import *import);

/* The turn pass of a turned output (turn.c, WS217). */
VkResult kwl_turn_open(struct kwl_compose *compose, uint32_t width, uint32_t height, unsigned rotation, struct kwl_turn *turn);
void kwl_turn_close(struct kwl_compose *compose, struct kwl_turn *turn);
void kwl_turn_destroy_shared(struct kwl_compose *compose);
void kwl_turn_record(struct kwl_server *server, struct kwl_list *command, uint32_t image, const VkRect2D *area, const VkRect2D *rects, unsigned count);

/* What the anchor's frames are drawn into: the turn image or the swapchain's (compose.c, WS217). */
VkImage kwl_compose_target_image(const struct kwl_compose *compose, uint32_t image);
VkImageLayout kwl_compose_target_layout(const struct kwl_compose *compose);
VkResult kwl_compose_pipeline_opaque(struct kwl_compose *compose, VkShaderModule vertex, VkShaderModule fragment, VkPipeline *pipeline);

/* The output's pass that keeps its pixels (backdrop.c). */
VkResult kwl_compose_load_pass(struct kwl_compose *compose);

/* The backdrop of the glass (backdrop.c). */
int kwl_backdrop_begin(struct kwl_server *server, struct kwl_list *command);
void kwl_backdrop_end(struct kwl_server *server, struct kwl_list *command);
void kwl_backdrop_reset(struct kwl_server *server);
void kwl_backdrop_destroy(struct kwl_compose *compose);

/* The colours of flat glass worked out beforehand (glass-map.c). */
int kwl_glass_map_set(struct kwl_server *server, const float *constants, VkDescriptorSet set, VkDescriptorSet *map_set);
int kwl_glass_picture_set(struct kwl_server *server, unsigned picture, kwl_glass_picture_draw draw, VkDescriptorSet *picture_set);
int kwl_glass_shadow_set(struct kwl_server *server, const float *box, float radius, float soft, const float *colour, float *origin, float *size, VkDescriptorSet *shadow_set);
void kwl_glass_maps_inband(struct kwl_server *server, struct kwl_list *command);
int kwl_glass_maps_wanted(struct kwl_server *server);
void kwl_glass_maps_destroy(struct kwl_compose *compose);

/* The clients' new images' move to the general layout, in the next frame (import.c). */
void kwl_import_layouts_record(struct kwl_compose *compose, struct kwl_list *command);
void kwl_import_layouts_done(struct kwl_compose *compose);

/* The image window mode samples for a surface (compose.c). */
const struct kwl_import *kwl_compose_surface_image(const struct kwl_object *surface);

/* The cursor's image and hotspot: 1 with an image, 0 when nothing shows, -1 when only composing can show it (compose.c). */
int kwl_compose_cursor_source(struct kwl_server *server, const struct kwl_import **image, int32_t *hotspot_x, int32_t *hotspot_y);

/* The cursor plane's swapchain, made after the output's and destroyed before it (cursor-plane.c). */
void kwl_cursor_plane_close(struct kwl_server *server);
struct kwl_cursor_thread *kwl_cursor_plane_make(struct kwl_compose *compose, const struct vkdemo_display *output, VkExtent2D *extent, uint32_t *index);
void kwl_cursor_plane_adopt(struct kwl_server *server, struct kwl_cursor_thread *cursor, VkExtent2D extent, uint32_t index);
void kwl_cursor_plane_detach(struct kwl_server *server, struct kwl_cursor_parts *parts);
int kwl_cursor_plane_stop(struct kwl_compose *compose, struct kwl_cursor_thread *cursor);
void kwl_cursor_plane_detached(struct kwl_server *server, struct kwl_cursor_parts *parts, int error);

/* The display's acquisition and release through libkeiland-backend (os.c, ws131-p008). */
VkResult kwl_os_display_acquire(struct kwl_server *server, VkPhysicalDevice physical, VkDisplayKHR display);
void kwl_os_display_release(struct kwl_server *server, VkPhysicalDevice physical, VkDisplayKHR display);

/* The capture's copy of a frame's swapchain image (shot.c, ws173-p002). */
void kwl_shot_record(struct kwl_server *server, struct kwl_list *command, VkImage image);

/* A part of an image drawn as a quad (uv: left, top, right, bottom) at a place and size of the desktop (compose.c). */
void kwl_compose_image_quad(struct kwl_server *server, struct kwl_list *command, const struct kwl_import *import, int32_t x, int32_t y, uint32_t width, uint32_t height, const float *uv);

/* The glass look's wallpaper at the desktop's size, or NULL (glass.c). */
const struct kwl_import *kwl_glass_wallpaper(struct kwl_server *server);

/* The displays shown besides the output (heads.c, ws113-p004b). */
void kwl_heads_config_load(struct kwl_server *server);
int kwl_heads_config_save(struct kwl_server *server);
uint64_t kwl_heads_config_ticket(void);
int kwl_heads_config_settled(uint64_t ticket, int *error);
void kwl_heads_sync(struct kwl_server *server);
void kwl_heads_close_display(struct kwl_server *server, VkDisplayKHR display);
void kwl_heads_close_all(struct kwl_server *server, struct kwl_head_rest *rests, unsigned *count);
void kwl_heads_rest_destroy(struct kwl_server *server, struct kwl_head_rest *rests, unsigned count);
void kwl_heads_make(struct kwl_server *server, struct kwl_head_opening *opening);
int kwl_heads_sync_later(struct kwl_server *server);
int kwl_displays_anchor_follow_later(struct kwl_server *server);
int kwl_heads_lost(struct kwl_server *server);
unsigned kwl_heads_wanted(struct kwl_server *server, unsigned *slots, unsigned room);
unsigned kwl_heads_take(struct kwl_server *server, const struct kwl_render_held *held);
void kwl_heads_record(struct kwl_server *server, struct kwl_list *command, VkImage source, VkImageLayout layout);
void kwl_heads_presented(struct kwl_server *server, const struct kwl_render_outcome *outcome);
void kwl_heads_frame_skipped(struct kwl_server *server);
int kwl_displays_apply(struct kwl_server *server, const struct kwl_display_config *wanted, int *saved);
int kwl_displays_set_shown(struct kwl_server *server, const char *key, unsigned shown, int *saved);
void kwl_displays_anchor_follow(struct kwl_server *server);
void kwl_displays_move_failed(struct kwl_server *server, const char *back);
size_t kwl_displays_describe(struct kwl_server *server, char *text, size_t size);

#endif
