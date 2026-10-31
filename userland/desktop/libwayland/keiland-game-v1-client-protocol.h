/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the compositor's game protocol (kl_game_v1 and
 * kl_game_surface_v1, version 1): in the game mode the compositor lends a
 * fullscreen surface the images its display shows, and the client draws
 * straight into them.
 *
 * The client's Vulkan WSI (libvulkan's Wayland surface) speaks it; an
 * application never does.  It tells the chain it draws with (chain); the
 * compositor sends each image it lends (image: its fd, whose description
 * the kernel gives the client's import) and then the lend (lend: the
 * images' count, size, format, uses and layout).  The client
 * makes a wl_buffer of each image with kl_gpu_buffer_v1 and names it
 * (tag); a lent buffer starts in the compositor's hands, and
 * wl_buffer.release gives it to the client to draw into.  A client that
 * cannot draw into the images declines the lend.  withdraw ends a lend:
 * its buffers stay the client's ordinary buffers, composed as any other.
 *
 * The header is private: it is not installed, and libvulkan includes it by
 * its path in the tree.
 */

#ifndef KERN_KEILAND_GAME_V1_CLIENT_PROTOCOL_H
#define KERN_KEILAND_GAME_V1_CLIENT_PROTOCOL_H

#include <wayland/wayland-client-core.h>
#include <wayland/wayland-client-protocol.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The objects the protocol names; only libwayland knows what is in them. */
struct kl_game_v1;
struct kl_game_surface_v1;

/* The interfaces' descriptions (game-protocol.c). */
extern const struct wl_interface kl_game_v1_interface;
extern const struct wl_interface kl_game_surface_v1_interface;

/* kl_game_v1: the global, which makes a surface's game object. */
#define KL_GAME_V1_DESTROY 0U
void kl_game_v1_destroy(struct kl_game_v1 *object);
#define KL_GAME_V1_GET_GAME_SURFACE 1U
struct kl_game_surface_v1 *kl_game_v1_get_game_surface(struct kl_game_v1 *object, struct wl_surface *surface);

/*
 * kl_game_surface_v1: a surface's game object.
 *
 * image: lent image index of the lend serial and its fd (the listener's).
 * lend: every image of the lend serial was sent; count images of width x
 * height, format (a VkFormat), the uses they allow (VkImageUsageFlags) and
 * their layout: the kernel's tiling value of the images (GPU_IMAGE_* of
 * <uapi/gpu.h>, the same as in each image's kernel description), or
 * KL_GAME_SURFACE_V1_LAYOUT_DESCRIBED when the compositor leaves it to
 * the images' own descriptions.  withdraw: the lend serial is over.
 */
#define KL_GAME_SURFACE_V1_LAYOUT_DESCRIBED 0U
struct kl_game_surface_v1_listener {
	void (*image)(void *data, struct kl_game_surface_v1 *object, uint32_t serial, uint32_t index, int32_t fd);
	void (*lend)(void *data, struct kl_game_surface_v1 *object, uint32_t serial, uint32_t count, uint32_t width, uint32_t height, uint32_t format, uint32_t usage, uint32_t layout);
	void (*withdraw)(void *data, struct kl_game_surface_v1 *object, uint32_t serial);
};
int kl_game_surface_v1_add_listener(struct kl_game_surface_v1 *object, const struct kl_game_surface_v1_listener *listener, void *data);
#define KL_GAME_SURFACE_V1_DESTROY 0U
void kl_game_surface_v1_destroy(struct kl_game_surface_v1 *object);
#define KL_GAME_SURFACE_V1_CHAIN 1U
void kl_game_surface_v1_chain(struct kl_game_surface_v1 *object, uint32_t format, uint32_t usage, uint32_t width, uint32_t height, uint32_t count);
#define KL_GAME_SURFACE_V1_TAG 2U
void kl_game_surface_v1_tag(struct kl_game_surface_v1 *object, struct wl_buffer *buffer, uint32_t serial, uint32_t index);
#define KL_GAME_SURFACE_V1_DECLINE 3U
void kl_game_surface_v1_decline(struct kl_game_surface_v1 *object, uint32_t serial);

#ifdef __cplusplus
}
#endif

#endif
