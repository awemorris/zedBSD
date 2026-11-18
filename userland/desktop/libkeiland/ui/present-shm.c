/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The shared-memory presenter of the library's windows (ws090-p004, the
 * buffers of libkeiland's file chooser): a frame drawn on the CPU is
 * copied into a wl_shm buffer of ARGB8888 (premultiplied, blended by its
 * alpha as the compositor's glass needs) and attached to the window's surface.
 * Two buffers alternate; a frame waits for one the compositor has given
 * back.  It serves small windows of a library, and windows where Vulkan
 * is missing (ws035's compositing design keeps wl_shm as the auxiliary
 * path).
 */

#include "window.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* wl_shm's format of premultiplied 0xAARRGGBB words. */
#define SHM_FORMAT_ARGB8888	0U

/*
 * A serial for the shared memory objects' names, so that two buffers of
 * the process never ask for the same name.  It only grows.
 */
static unsigned shm_serial;

static struct keiui_shm_buffer *shm_ready(struct kl_window *window);
static void shm_release(void *data, struct wl_buffer *buffer);

/* A buffer given back by the compositor. */
static const struct wl_buffer_listener shm_listener = {
	shm_release
};

/*
 * Shows a frame through a shared-memory buffer.  Returns 0, EAGAIN when
 * the compositor still reads both buffers (the frame is drawn again
 * later), or an errno value of making a buffer.
 */
int
keiui_shm_present(
	struct kl_window *window,
	const uint32_t *pixels,
	size_t stride)
{
	struct keiui_shm_buffer *buffer;
	int line;

	/* A buffer the compositor does not read, of the frame's size. */
	buffer = shm_ready(window);
	if (buffer == NULL)
		return EAGAIN;

	/* The frame's rows into it. */
	for (line = 0; line < buffer->height; line++)
		memcpy(buffer->pixels + (size_t)line * (size_t)buffer->width, pixels + (size_t)line * stride, (size_t)buffer->width * sizeof(pixels[0]));

	/* Shown with the next commit, the whole of it changed. */
	wl_surface_attach(window->surface, buffer->buffer, 0, 0);
	wl_surface_damage_buffer(window->surface, 0, 0, buffer->width, buffer->height);
	wl_surface_commit(window->surface);
	buffer->busy = 1;

	/* Succeeded: the frame is on its way. */
	return 0;
}

/*
 * Frees the shared-memory buffers.
 */
void
keiui_shm_close(
	struct kl_window *window)
{
	int index;

	/* Each buffer. */
	for (index = 0; index < KEIUI_SHM_BUFFERS; index++)
		keiui_shm_free(&window->buffers[index]);
}

/*
 * Makes a wl_shm buffer of a size in a shared memory object of its own
 * (the window's frames, and a drag's icon, clipboard.c).  Returns 0 or an
 * errno value.
 */
int
keiui_shm_make(
	struct kl_window *window,
	struct keiui_shm_buffer *buffer,
	int width,
	int height)
{
	struct wl_shm_pool *pool;
	char name[64];
	void *mapped;
	size_t size;
	int descriptor;
	int status;

	/* A shared memory object with a name nobody else uses, unlinked at once. */
	size = (size_t)width * (size_t)height * 4U;
	shm_serial++;
	snprintf(name, sizeof(name), "/keiui-window-%ld-%u", (long)getpid(), shm_serial);
	descriptor = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (descriptor < 0)
		return errno;
	(void)shm_unlink(name);

	/* Its size. */
	status = ftruncate(descriptor, (off_t)size);
	if (status != 0) {
		status = errno;
		close(descriptor);
		return status;
	}

	/* Mapped for drawing. */
	mapped = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
	if (mapped == MAP_FAILED) {
		status = errno;
		close(descriptor);
		return status;
	}

	/* The compositor's pool over it. */
	pool = wl_shm_create_pool(window->shm, descriptor, (int32_t)size);
	close(descriptor);
	if (pool == NULL) {
		munmap(mapped, size);
		return ENOMEM;
	}

	/* The buffer from the pool (the pool is not needed after). */
	buffer->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	if (buffer->buffer == NULL) {
		munmap(mapped, size);
		return ENOMEM;
	}

	/* The compositor says when it gives the buffer back. */
	buffer->window = window;
	(void)wl_buffer_add_listener(buffer->buffer, &shm_listener, buffer);

	/* Succeeded: the buffer, free to draw into. */
	buffer->pixels = mapped;
	buffer->size = size;
	buffer->width = width;
	buffer->height = height;
	buffer->busy = 0;
	return 0;
}

/*
 * Frees a buffer and its memory.
 */
void
keiui_shm_free(
	struct keiui_shm_buffer *buffer)
{
	/* The protocol object, then the memory. */
	if (buffer->buffer != NULL)
		wl_buffer_destroy(buffer->buffer);
	if (buffer->pixels != NULL)
		munmap(buffer->pixels, buffer->size);
	memset(buffer, 0, sizeof(*buffer));
}

/* Finds a buffer the compositor has given back, of the frame's size (remade when the size changed); NULL when none is free. */
static struct keiui_shm_buffer *
shm_ready(
	struct kl_window *window)
{
	struct keiui_shm_buffer *buffer;
	int index;
	int error;

	/* The first free buffer. */
	for (index = 0; index < KEIUI_SHM_BUFFERS; index++) {
		buffer = &window->buffers[index];
		if (buffer->busy)
			continue;

		/* Of the size already. */
		if (buffer->buffer != NULL && buffer->width == (int)window->shm_width && buffer->height == (int)window->shm_height)
			return buffer;

		/* Remade at the size. */
		keiui_shm_free(buffer);
		error = keiui_shm_make(window, buffer, (int)window->shm_width, (int)window->shm_height);
		if (error != 0)
			return NULL;
		return buffer;
	}

	/* Both are the compositor's still. */
	return NULL;
}

/* The compositor gave a buffer back: free to draw into. */
static void
shm_release(
	void *data,
	struct wl_buffer *buffer)
{
	struct keiui_shm_buffer *owned;

	/* Free again. */
	(void)buffer;
	owned = data;
	owned->busy = 0;

	/* A window on another's connection draws a frame that waited for it. */
	keiui_window_wake(owned->window);
}
