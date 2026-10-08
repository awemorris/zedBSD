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

#include "../../tests/vkdemo/display.h"
#include "displays.h"

/* Bound the swapchain images and the buffers one frame may sample. */
/* The most displays the compositor follows at once (ws113-p004a). */
#define KWL_COMPOSE_DISPLAYS	8U
/* The room of a display's name (VkDisplayPropertiesKHR's displayName, cut). */
#define KWL_COMPOSE_NAME	64U
#define KWL_SWAPCHAIN_MAX	8U
#define KWL_FRAME_WINDOWS	64U

/* The longest list of an output's windows the log keeps (KWL RENDER, ws113-p007). */
#define KWL_RENDER_LOG		256U

/* The vertices of a quad: two triangles of a triangle list. */
#define KWL_QUAD_VERTICES	6U

/* The glass look's push constants: six vec4 (see shaders/panel.frag). */
#define KWL_PANEL_CONSTANTS	24U

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
	/* A host-written image (wl_shm, the arrow): its mapping and the length of a row in it. */
	void *map;
	VkDeviceSize row_pitch;
	/* A client's image waiting for its move to the general layout in the next frame (ws099-p016). */
	unsigned layout_pending;
};

/* The glass look's images and glyphs (glass.c). */
struct kwl_glass;

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

/* How many frames' damage is kept, for images that missed that many frames. */
#define KWL_DAMAGE_HISTORY	8U

/* The Vulkan device, the display output and the frame in flight. */
struct kwl_compose {
	VkInstance instance;
	VkPhysicalDevice physical;
	/* The display chosen before the OS acquires it for the swapchain, and its name (empty when it has none). */
	VkDisplayKHR display;
	char display_name[KWL_COMPOSE_NAME];
	VkDevice device;
	VkQueue queue;
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
	VkSampler linear_sampler;
	struct kwl_glass *glass;
	VkDescriptorPool descriptors;
	VkCommandPool pool;
	VkCommandBuffer command;
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
	VkFence fence;
	VkSemaphore acquired;
	struct vkdemo_display output;
	/* The swapchain images can be a copy's source (a test image's capture, ws173-p002). */
	unsigned readback;
	unsigned output_prepared;
	unsigned output_open;
	VkFormat format;
	VkImageView views[KWL_SWAPCHAIN_MAX];
	VkFramebuffer framebuffers[KWL_SWAPCHAIN_MAX];
	VkSemaphore rendered[KWL_SWAPCHAIN_MAX];
	struct kwl_object *held[KWL_FRAME_WINDOWS + 3U];
	struct kwl_object *callbacks;
	unsigned held_count;
	unsigned in_flight;
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
	int32_t history[KWL_DAMAGE_HISTORY][4];
	unsigned history_whole[KWL_DAMAGE_HISTORY];
	VkRenderPass pass_load;
	VkRect2D scissor_now;
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
void kwl_host_image_batch_begin(struct kwl_compose *compose);
VkResult kwl_host_image_batch_end(struct kwl_compose *compose);

/* The glass look (glass.c). */
int kwl_glass_open(struct kwl_server *server);
void kwl_glass_close(struct kwl_server *server);
void kwl_glass_draw(struct kwl_server *server, VkCommandBuffer command, struct kwl_object **windows, unsigned count);
void kwl_glass_draw_drag_badge(struct kwl_server *server, VkCommandBuffer command);
void kwl_glass_draw_drag_mark(struct kwl_server *server, VkCommandBuffer command, unsigned state);
void kwl_glass_draw_head(struct kwl_server *server, VkCommandBuffer command, struct kwl_object **windows, unsigned count);
void kwl_compose_cursor(struct kwl_server *server, VkCommandBuffer command);

/* Descriptor sets of the image layout, reused rather than freed (compose.c). */
VkResult kwl_compose_set_get(struct kwl_compose *compose, VkDescriptorSet *result);
void kwl_compose_set_put(struct kwl_compose *compose, VkDescriptorSet set);
int kwl_output_switch(struct kwl_server *server, VkDisplayKHR target);
VkResult kwl_compose_display_read(struct kwl_server *server, VkDisplayKHR display, uint32_t *width, uint32_t *height, uint32_t *refresh, char *name, size_t size);

/* Gives an image a second, linearly sampled descriptor set (compose.c). */
VkResult kwl_compose_linear_set(struct kwl_compose *compose, struct kwl_import *import);

/* The output's pass that keeps its pixels (backdrop.c). */
VkResult kwl_compose_load_pass(struct kwl_compose *compose);

/* The backdrop of the glass (backdrop.c). */
int kwl_backdrop_begin(struct kwl_server *server, VkCommandBuffer command);
void kwl_backdrop_end(struct kwl_server *server, VkCommandBuffer command);
void kwl_backdrop_reset(struct kwl_server *server);
void kwl_backdrop_destroy(struct kwl_compose *compose);

/* The clients' new images' move to the general layout, in the next frame (import.c). */
void kwl_import_layouts_record(struct kwl_compose *compose, VkCommandBuffer command);
void kwl_import_layouts_done(struct kwl_compose *compose);

/* The image window mode samples for a surface (compose.c). */
const struct kwl_import *kwl_compose_surface_image(const struct kwl_object *surface);

/* The display's acquisition and release through libkeiland-backend (os.c, ws131-p008). */
VkResult kwl_os_display_acquire(struct kwl_server *server, VkPhysicalDevice physical, VkDisplayKHR display);
void kwl_os_display_release(struct kwl_server *server, VkPhysicalDevice physical, VkDisplayKHR display);

/* The capture's copy of a frame's swapchain image (shot.c, ws173-p002). */
void kwl_shot_record(struct kwl_server *server, VkCommandBuffer command, VkImage image);

/* A part of an image drawn as a quad (uv: left, top, right, bottom) at a place and size of the desktop (compose.c). */
void kwl_compose_image_quad(struct kwl_server *server, VkCommandBuffer command, const struct kwl_import *import, int32_t x, int32_t y, uint32_t width, uint32_t height, const float *uv);

/* The glass look's wallpaper at the desktop's size, or NULL (glass.c). */
const struct kwl_import *kwl_glass_wallpaper(struct kwl_server *server);

/* The displays shown besides the output (heads.c, ws113-p004b). */
void kwl_heads_config_load(struct kwl_server *server);
void kwl_heads_sync(struct kwl_server *server);
void kwl_heads_close_display(struct kwl_server *server, VkDisplayKHR display);
void kwl_heads_close_all(struct kwl_server *server);
int kwl_heads_lost(struct kwl_server *server);
unsigned kwl_heads_acquire(struct kwl_server *server);
void kwl_heads_record(struct kwl_server *server, VkCommandBuffer command, VkImage source);
unsigned kwl_heads_waits(struct kwl_server *server, VkSemaphore *semaphores, VkPipelineStageFlags *stages, unsigned room);
unsigned kwl_heads_signals(struct kwl_server *server, VkSemaphore *semaphores, unsigned room);
void kwl_heads_present(struct kwl_server *server);
void kwl_heads_frame_skipped(struct kwl_server *server);
int kwl_displays_apply(struct kwl_server *server, const struct kwl_display_config *wanted, int *saved);
int kwl_displays_set_shown(struct kwl_server *server, const char *key, unsigned shown, int *saved);
void kwl_displays_anchor_follow(struct kwl_server *server);
void kwl_displays_move_failed(struct kwl_server *server, const char *back);
size_t kwl_displays_describe(struct kwl_server *server, char *text, size_t size);

#endif
