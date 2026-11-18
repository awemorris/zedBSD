/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* KMS inquiry, duplicated master ownership and the portable double-dumb-buffer copy path. */
#include "compat.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* Bounds each protocol or device wait with the same monotonic time units. */
#define KMS_FLIP_TIMEOUT_NS UINT64_C(5000000000)

/* The inquiry descriptor never owns master; initialized once and retained with process-lifetime mode handles. */
static int kms_query_fd = -1;

/* Connected display records remain stable after the first inquiry, protected during publication by compat_mutex. */
static struct compat_display *kms_displays;

/* The one inquiry result and count are published together under compat_mutex. */
static unsigned kms_initialized;

/* Counts the published process-lifetime display list under compat_mutex. */
static uint32_t kms_display_count;

/* Retains the one initial inquiry outcome under compat_mutex, including failure. */
static VkResult kms_inquiry_result;

static VkResult kms_initialize(void);
static VkResult kms_enumerate(int fd);
static VkResult kms_connector(int fd, uint32_t id, const uint32_t *crtcs, uint32_t crtc_count);
static uint32_t kms_choose_crtc(int fd, const struct drm_mode_get_connector *connector, const uint32_t *encoders, const uint32_t *crtcs, uint32_t crtc_count);
static const char *kms_connector_name(uint32_t type);
static VkResult kms_ioctl_result(int error);
static VkResult kms_flip_wait(struct compat_display *display, uint64_t token);
static void kms_records_free(void);

/*
 * Publishes a stable connector/mode list without ever acquiring display ownership during inquiry.
 */
VkResult
compat_kms_displays(
	struct compat_display **displays,
	uint32_t *count)
{
	VkResult result;

	/* Serializes the initial inquiry and its process-lifetime records. */
	(void)pthread_mutex_lock(&compat_mutex);

	/* Performs inquiry once; an explicit none setting opens no device. */
	if (kms_initialized == 0) {
		kms_inquiry_result = kms_initialize();
		kms_initialized = 1;
	}

	/* Publishes only a completely initialized, stable handle list. */
	*displays = kms_displays;
	*count = kms_display_count;
	result = kms_inquiry_result;

	/* Ends record protection before the caller queries or acquires a display. */
	(void)pthread_mutex_unlock(&compat_mutex);

	/* Reports the retained inquiry failure without publishing success. */
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: the caller received the stable display inquiry. */
	return VK_SUCCESS;
}

/*
 * Checks card identity without changing master or descriptor ownership.
 */
VkResult
compat_kms_card_matches(
	int fd)
{
	struct stat source;
	struct stat inquiry;
	int error;

	/* Requires a live caller descriptor with an actual device identity. */
	error = fstat(fd, &source);
	if (error != 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* The inquiry descriptor identifies the card that owns our connector handles. */
	error = fstat(kms_query_fd, &inquiry);
	if (error != 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Descriptors from another card cannot address these connector identities. */
	if (source.st_rdev != inquiry.st_rdev)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Succeeded: both descriptors refer to the selected DRM card. */
	return VK_SUCCESS;
}

/*
 * Acquires a duplicated caller descriptor, or acquires master on our inquiry file for direct root applications.
 */
VkResult
compat_kms_acquire(
	struct compat_display *display,
	int fd)
{
	struct drm_mode_crtc saved;
	struct drm_auth authentication;
	unsigned owned_master;
	int owned;
	int error;

	/* Direct applications acquire master only when beginning an actual display session. */
	owned_master = 0;
	if (fd < 0) {
		fd = kms_query_fd;
		owned_master = 1;
	}

	/* Requires the descriptor to identify the card whose connector handles were enumerated. */
	error = compat_kms_card_matches(fd);
	if (error != VK_SUCCESS)
		return error;

	/* Takes an independently closeable descriptor while preserving the caller's file ownership. */
	owned = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	if (owned < 0)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Only an independent direct application asks this library to take master. */
	if (owned_master != 0) {
		error = ioctl(owned, DRM_IOCTL_SET_MASTER, NULL);
		if (error != 0) {
			(void)close(owned);
			return VK_ERROR_INITIALIZATION_FAILED;
		}
	} else {
		/* AUTH_MAGIC requires current master; nonexistent magic zero returns EINVAL without changing authority. */
		memset(&authentication, 0, sizeof(authentication));
		error = ioctl(owned, DRM_IOCTL_AUTH_MAGIC, &authentication);
		if (error != 0 && errno != EINVAL) {
			(void)close(owned);
			return VK_ERROR_INITIALIZATION_FAILED;
		}
	}

	/* Saves the CRTC before any scanout change so destruction restores the console. */
	memset(&saved, 0, sizeof(saved));
	saved.crtc_id = display->crtc;
	error = ioctl(owned, DRM_IOCTL_MODE_GETCRTC, &saved);
	if (error != 0) {
		(void)close(owned);
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* Repeated logind acquisition replaces only our duplicate of the current master file. */
	if (display->master_fd >= 0)
		(void)close(display->master_fd);

	/* Preserves the original console state across repeated acquisition of the same live session. */
	if (display->saved_valid == 0) {
		display->saved = saved;
		display->saved_valid = 1;
	}

	/* The acquired descriptor remains owned until explicit release or session retirement. */
	display->master_fd = owned;
	/* Borrowed seat authority stays with logind even when our duplicate retires. */
	display->master_owned = owned_master;

	/* Succeeded: KMS submissions use our duplicate and never the caller's descriptor directly. */
	return VK_SUCCESS;
}

/*
 * Restores the acquired CRTC state before any framebuffer backing memory is destroyed.
 */
void
compat_kms_restore(
	struct compat_display *display)
{
	struct drm_mode_crtc restore;
	uint32_t connector;
	int error;

	/* An unacquired or already retired display has no saved scanout ownership. */
	if (display->master_fd < 0)
		return;

	/* Restoration is necessary only after this WSI has actually changed scanout. */
	if (display->active == 0)
		return;

	/* Uses the acquired console's mode, framebuffer and origin with our connector identity. */
	restore = display->saved;
	connector = display->connector;
	restore.set_connectors_ptr = 0;
	restore.count_connectors = 0;
	if (restore.mode_valid != 0) {
		restore.set_connectors_ptr = (uint64_t)(uintptr_t)&connector;
		restore.count_connectors = 1;
	}

	/* Losing master can prevent restoration; the resumed seat or console then owns its next modeset. */
	error = ioctl(display->master_fd, DRM_IOCTL_MODE_SETCRTC, &restore);
	if (error != 0)
		fprintf(stderr, "libvulkan-compat: restore CRTC failed: %s\n", strerror(errno));

	/* Marks our scanout retired so repeated cleanup never restores a removed dumb framebuffer. */
	display->active = 0;
	display->scanout_chain = NULL;

	/* Succeeded: this WSI no longer claims active scanout. */
	return;
}

/*
 * Ends explicit display ownership while preserving the caller's independent descriptor lifetime.
 */
void
compat_kms_release(
	struct compat_display *display)
{
	/* A release without acquisition owns no master descriptor. */
	if (display->master_fd < 0)
		return;

	/* Returns scanout to the pre-acquisition CRTC before dropping master. */
	compat_kms_restore(display);
	if (display->master_owned != 0)
		(void)ioctl(display->master_fd, DRM_IOCTL_DROP_MASTER, NULL);
	(void)close(display->master_fd);
	display->master_fd = -1;
	display->master_owned = 0;
	display->saved_valid = 0;

	/* Succeeded: the display duplicate and its library-owned authority have retired. */
	return;
}

/*
 * Allocates one mapped XRGB scanout buffer without requiring any rendering-driver import extension.
 */
VkResult
compat_kms_dumb_create(
	struct compat_display *display,
	VkExtent2D extent,
	struct compat_dumb *buffer)
{
	VkResult translated_error;
	struct drm_mode_create_dumb create;
	struct drm_mode_map_dumb map;
	struct drm_mode_fb_cmd2 framebuffer;
	int error;

	/* Retains the creating file even if logind later replaces the display's current master descriptor. */
	buffer->fd = fcntl(display->master_fd, F_DUPFD_CLOEXEC, 0);
	if (buffer->fd < 0)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Requests a four-byte pixel buffer with the selected physical mode dimensions. */
	memset(&create, 0, sizeof(create));
	create.width = extent.width;
	create.height = extent.height;
	create.bpp = 32;
	error = ioctl(buffer->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
	if (error != 0) {
		translated_error = kms_ioctl_result(error);

		/* Reports the corresponding Vulkan KMS failure. */
		return translated_error;
	}

	/* Retains the handle immediately so every later failure can destroy this allocation. */
	buffer->handle = create.handle;
	buffer->pitch = create.pitch;
	buffer->size = create.size;
	if (buffer->size > SIZE_MAX)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Gets the kernel's mmap offset for this descriptor's dumb handle. */
	memset(&map, 0, sizeof(map));
	map.handle = buffer->handle;
	error = ioctl(buffer->fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
	if (error != 0) {
		translated_error = kms_ioctl_result(error);

		/* Reports the corresponding Vulkan KMS failure. */
		return translated_error;
	}

	/* Maps only the allocated scanout storage; no host DRM device participates in tests. */
	buffer->mapping = mmap(NULL, (size_t)buffer->size, PROT_READ | PROT_WRITE, MAP_SHARED, buffer->fd, (off_t)map.offset);
	if (buffer->mapping == MAP_FAILED) {
		buffer->mapping = NULL;
		return VK_ERROR_MEMORY_MAP_FAILED;
	}

	/* Registers the one-plane XRGB framebuffer with the kernel's actual pitch. */
	memset(&framebuffer, 0, sizeof(framebuffer));
	framebuffer.width = extent.width;
	framebuffer.height = extent.height;
	framebuffer.pixel_format = 0x34325258U;
	framebuffer.handles[0] = buffer->handle;
	framebuffer.pitches[0] = buffer->pitch;
	error = ioctl(buffer->fd, DRM_IOCTL_MODE_ADDFB2, &framebuffer);
	if (error != 0) {
		translated_error = kms_ioctl_result(error);

		/* Reports the corresponding Vulkan KMS failure. */
		return translated_error;
	}

	/* Retains the framebuffer identity until scanout retirement precedes its removal. */
	buffer->framebuffer = framebuffer.fb_id;

	/* Succeeded: this swapchain owns one mapped, registered KMS buffer. */
	return VK_SUCCESS;
}

/*
 * Removes a dumb framebuffer only after the chain has restored the prior CRTC.
 */
void
compat_kms_dumb_destroy(
	struct compat_display *display,
	struct compat_dumb *buffer)
{
	struct drm_mode_destroy_dumb destroy;

	/* The retained creating file owns this allocation even after a seat resume. */
	(void)display;

	/* Mapped CPU storage retires before its underlying handle. */
	if (buffer->mapping != NULL) {
		(void)munmap(buffer->mapping, (size_t)buffer->size);
		buffer->mapping = NULL;
	}

	/* A partially created buffer may never have been registered as a framebuffer. */
	if (buffer->framebuffer != 0) {
		(void)ioctl(buffer->fd, DRM_IOCTL_MODE_RMFB, &buffer->framebuffer);
		buffer->framebuffer = 0;
	}

	/* The creating master file owns the dumb allocation's handle. */
	if (buffer->handle != 0) {
		memset(&destroy, 0, sizeof(destroy));
		destroy.handle = buffer->handle;
		(void)ioctl(buffer->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
		buffer->handle = 0;
	}

	/* Closing the last duplicate also retires any kernel handles left by a revoked file. */
	if (buffer->fd >= 0) {
		(void)close(buffer->fd);
		buffer->fd = -1;
	}
}

/*
 * Copies completed Vulkan readback rows to the back buffer and performs one bounded FIFO flip.
 */
VkResult
compat_kms_present(
	struct compat_swapchain *chain)
{
	VkResult translated_error;
	struct compat_display *display;
	struct compat_dumb *buffer;
	struct drm_mode_crtc crtc;
	struct drm_mode_crtc_page_flip flip;
	uint32_t connector;
	uint32_t row;
	unsigned back;
	int error;
	VkResult result;

	/* Readback completion was established by the caller's Vulkan fence wait. */
	display = chain->surface->kms;
	back = chain->front ^ 1U;
	buffer = &chain->dumb[back];
	for (row = 0; row < chain->extent.height; row++) {
		/* KMS pitch may differ from the tightly packed Vulkan copy buffer's row size. */
		memcpy((unsigned char *)buffer->mapping + (size_t)row * buffer->pitch, (unsigned char *)chain->readback_mapping + (size_t)row * chain->extent.width * 4, (size_t)chain->extent.width * 4);
	}

	/* The first present sets mode and scanout together; subsequent presents flip the back buffer. */
	if (chain->scanout == 0) {
		/* Uses the selected connector mode without depending on backend display extensions. */
		memset(&crtc, 0, sizeof(crtc));
		connector = display->connector;
		crtc.set_connectors_ptr = (uint64_t)(uintptr_t)&connector;
		crtc.count_connectors = 1;
		crtc.crtc_id = display->crtc;
		crtc.fb_id = buffer->framebuffer;
		crtc.mode_valid = 1;
		crtc.mode = chain->surface->display_mode->mode;
		error = ioctl(display->master_fd, DRM_IOCTL_MODE_SETCRTC, &crtc);
		if (error != 0) {
			translated_error = kms_ioctl_result(error);

			/* Reports the corresponding Vulkan KMS failure. */
			return translated_error;
		}

		/* Owns scanout only after the successful first modeset. */
		display->active = 1;
		display->scanout_chain = chain;
		chain->scanout = 1;
	} else {
		/* The chain address identifies this flip's completion in the master descriptor's event stream. */
		memset(&flip, 0, sizeof(flip));
		flip.crtc_id = display->crtc;
		flip.fb_id = buffer->framebuffer;
		flip.flags = DRM_MODE_PAGE_FLIP_EVENT;
		flip.user_data = (uint64_t)(uintptr_t)chain;
		error = ioctl(display->master_fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip);
		if (error != 0) {
			translated_error = kms_ioctl_result(error);

			/* Reports the corresponding Vulkan KMS failure. */
			return translated_error;
		}

		/* A finite event wait works for logind's nonblocking descriptor as well as direct root ownership. */
		result = kms_flip_wait(display, flip.user_data);
		if (result != VK_SUCCESS)
			return result;
	}

	/* Reuses the old front buffer only after the new front has reached scanout. */
	chain->front = back;

	/* Succeeded: the copied image is displayed by KMS. */
	return VK_SUCCESS;
}

/* Selects the requested card, dropping automatic master before querying connectors. */
static VkResult
kms_initialize(
	void)
{
	const char *requested;
	char path[64];
	struct drm_mode_card_res resources;
	uint32_t index;
	int fd;
	int error;
	int differs;
	VkResult result;

	/* An explicit none selection opens no DRM device and enumerates zero displays. */
	requested = getenv("KEILAND_DRM_DEVICE");
	if (requested != NULL) {
		differs = strcmp(requested, "none");
		if (differs == 0)
			return VK_SUCCESS;
	}

	/* An explicit card path is authoritative; otherwise scans the finite primary-node range. */
	for (index = 0; index < 16; index++) {
		/* Builds the default candidate without requiring libdrm. */
		(void)snprintf(path, sizeof(path), "/dev/dri/card%u", index);
		if (requested != NULL)
			(void)snprintf(path, sizeof(path), "%s", requested);

		/* Inquiry descriptors carry close-on-exec and never retain automatic master ownership. */
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			/* Explicit inaccessible paths cannot silently select another card. */
			if (requested != NULL)
				return VK_ERROR_INITIALIZATION_FAILED;

			/* Tries the next primary node without leaving an open descriptor. */
			continue;
		}

		/* The kernel grants automatic master to the first file; inquiry must relinquish it immediately. */
		(void)ioctl(fd, DRM_IOCTL_DROP_MASTER, NULL);
		memset(&resources, 0, sizeof(resources));
		error = ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources);
		if (error != 0) {
			/* A non-KMS candidate cannot provide display handles. */
			(void)close(fd);
			if (requested != NULL)
				return VK_ERROR_INITIALIZATION_FAILED;

			/* Continues the bounded primary-node inquiry. */
			continue;
		}

		/* Selects the first card that actually exposes connectors. */
		if (resources.count_connectors == 0) {
			(void)close(fd);
			if (requested != NULL)
				return VK_SUCCESS;

			/* A connector-less primary node is not an available display card. */
			continue;
		}

		/* Enumeration owns its complete stable connector and mode list on success. */
		result = kms_enumerate(fd);
		if (result != VK_SUCCESS) {
			(void)close(fd);
			kms_records_free();
			return result;
		}

		/* Publishes the selected inquiry file only after complete enumeration. */
		kms_query_fd = fd;
		return VK_SUCCESS;
	}

	/* A headless machine has a valid zero-display Vulkan enumeration. */
	return VK_SUCCESS;
}

/* Performs the two-stage kernel resource enumeration with explicitly bounded allocation counts. */
static VkResult
kms_enumerate(
	int fd)
{
	struct drm_mode_card_res resources;
	uint32_t *connectors;
	uint32_t *crtcs;
	uint32_t connector_count;
	uint32_t crtc_count;
	uint32_t index;
	int error;
	VkResult result;

	/* Measures connector and CRTC identities before allocating their arrays. */
	memset(&resources, 0, sizeof(resources));
	error = ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources);
	if (error != 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Bounds kernel-supplied allocation arithmetic even on an unexpected resource response. */
	connector_count = resources.count_connectors;
	crtc_count = resources.count_crtcs;
	if (connector_count > 4096)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* The CRTC count also determines encoder-mask indexing below. */
	if (crtc_count > 32)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Allocates independent arrays whose ownership ends after connector construction. */
	connectors = calloc((size_t)connector_count + 1, sizeof(*connectors));
	if (connectors == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* A failed second allocation retains no first resource array. */
	crtcs = calloc((size_t)crtc_count + 1, sizeof(*crtcs));
	if (crtcs == NULL) {
		free(connectors);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Requests only connector and CRTC arrays; framebuffer and encoder arrays are not needed here. */
	resources.connector_id_ptr = (uint64_t)(uintptr_t)connectors;
	resources.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
	resources.count_fbs = 0;
	resources.count_encoders = 0;
	error = ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources);
	if (error != 0) {
		free(crtcs);
		free(connectors);
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* A hotplug that outgrows either allocation is reported rather than reading an incomplete list. */
	if (resources.count_connectors > connector_count) {
		free(crtcs);
		free(connectors);
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* A changed CRTC count likewise requires a later process inquiry. */
	if (resources.count_crtcs > crtc_count) {
		free(crtcs);
		free(connectors);
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* Constructs only connected connectors with supported modes. */
	result = VK_SUCCESS;
	for (index = 0; index < resources.count_connectors; index++) {
		/* Any partial failure leaves the complete display list unpublished. */
		result = kms_connector(fd, connectors[index], crtcs, resources.count_crtcs);
		if (result != VK_SUCCESS)
			break;
	}

	/* Releases identity arrays after all connector records have retained their required values. */
	free(crtcs);
	free(connectors);

	/* Reports why the connector list could not be constructed. */
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: every connected connector has a stable display record. */
	return VK_SUCCESS;
}

/* Constructs one stable connected display and its finite kernel-provided mode list. */
static VkResult
kms_connector(
	int fd,
	uint32_t id,
	const uint32_t *crtcs,
	uint32_t crtc_count)
{
	struct drm_mode_get_connector connector;
	struct drm_mode_modeinfo *modes;
	struct compat_display *display;
	struct compat_display_mode *mode;
	uint32_t *encoders;
	uint32_t mode_count;
	uint32_t encoder_count;
	uint32_t crtc;
	uint32_t index;
	int error;

	/* Measures this connector's mode and possible encoder arrays. */
	memset(&connector, 0, sizeof(connector));
	connector.connector_id = id;
	error = ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector);
	if (error != 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Disconnected outputs and connected outputs without modes are absent from Vulkan enumeration. */
	if (connector.connection != 1)
		return VK_SUCCESS;

	/* A connector with no mode cannot supply a display surface. */
	if (connector.count_modes == 0)
		return VK_SUCCESS;

	/* Caps unexpected kernel responses before allocating their arrays. */
	mode_count = connector.count_modes;
	encoder_count = connector.count_encoders;
	if (mode_count > 4096)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Encoder indices are used only within a finite measured array. */
	if (encoder_count > 4096)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Reads the mode timings into independent temporary storage. */
	modes = calloc(mode_count, sizeof(*modes));
	if (modes == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Encoder candidates determine which CRTC can address this connector. */
	encoders = calloc((size_t)encoder_count + 1, sizeof(*encoders));
	if (encoders == NULL) {
		free(modes);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Fetches exactly the measured mode/encoder capacity without unneeded property arrays. */
	connector.modes_ptr = (uint64_t)(uintptr_t)modes;
	connector.encoders_ptr = (uint64_t)(uintptr_t)encoders;
	connector.count_props = 0;
	error = ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector);
	if (error != 0) {
		free(encoders);
		free(modes);
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* Hotplug growth cannot be mistaken for a completely initialized mode list. */
	if (connector.count_modes > mode_count) {
		free(encoders);
		free(modes);
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* Encoder growth is similarly rejected before indexing the temporary array. */
	if (connector.count_encoders > encoder_count) {
		free(encoders);
		free(modes);
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* A disconnected connector cannot publish stale modes measured before hotplug. */
	if (connector.connection != 1 || connector.count_modes == 0) {
		free(encoders);
		free(modes);
		return VK_SUCCESS;
	}

	/* Prefers the existing encoder CRTC, otherwise one allowed by a possible encoder mask. */
	crtc = kms_choose_crtc(fd, &connector, encoders, crtcs, crtc_count);
	free(encoders);
	if (crtc == 0) {
		free(modes);
		return VK_SUCCESS;
	}

	/* Allocates the stable display record before publishing any mode handles. */
	display = calloc(1, sizeof(*display));
	if (display == NULL) {
		free(modes);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Retains connector identity, physical size and an unacquired master descriptor. */
	display->connector = id;
	display->crtc = crtc;
	display->width_mm = connector.mm_width;
	display->height_mm = connector.mm_height;
	display->master_fd = -1;
	(void)snprintf(display->name, sizeof(display->name), "%s-%u", kms_connector_name(connector.connector_type), connector.connector_type_id);
	display->next = kms_displays;
	kms_displays = display;

	/* Mode records remain stable even when callers free their enumeration result arrays. */
	for (index = 0; index < connector.count_modes; index++) {
		/* Allocates one mode at a time so failure cleanup can walk every retained record. */
		mode = calloc(1, sizeof(*mode));
		if (mode == NULL) {
			free(modes);
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		}

		/* Appends each timing to the display's process-lifetime handle list. */
		mode->display = display;
		mode->mode = modes[index];
		mode->next = display->modes;
		display->modes = mode;
		if (display->preferred == NULL)
			display->preferred = mode;

		/* A kernel preferred timing determines physicalResolution for ordinary compositor startup. */
		if ((mode->mode.type & DRM_MODE_TYPE_PREFERRED) != 0)
			display->preferred = mode;
	}

	/* Publishes the completed connector count and releases its temporary mode array. */
	kms_display_count++;
	free(modes);

	/* Succeeded: this connected display's handle and all mode handles remain stable. */
	return VK_SUCCESS;
}

/* Finds an encoder's current CRTC or a CRTC allowed by its kernel mask. */
static uint32_t
kms_choose_crtc(
	int fd,
	const struct drm_mode_get_connector *connector,
	const uint32_t *encoders,
	const uint32_t *crtcs,
	uint32_t crtc_count)
{
	struct drm_mode_get_encoder encoder;
	uint32_t index;
	uint32_t other;
	int error;

	/* An already routed encoder preserves the console's existing CRTC selection. */
	if (connector->encoder_id != 0) {
		memset(&encoder, 0, sizeof(encoder));
		encoder.encoder_id = connector->encoder_id;
		error = ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &encoder);
		if (error == 0) {
			/* A nonzero current CRTC is already routed to this connector. */
			if (encoder.crtc_id != 0)
				return encoder.crtc_id;
		}
	}

	/* Otherwise selects one CRTC from the finite allowed encoder masks. */
	for (index = 0; index < connector->count_encoders; index++) {
		/* A failed encoder inquiry cannot supply a trustworthy CRTC mask. */
		memset(&encoder, 0, sizeof(encoder));
		encoder.encoder_id = encoders[index];
		error = ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &encoder);
		if (error != 0)
			continue;

		/* Kernel encoder masks contain at most the 32 resource-indexed CRTCs. */
		for (other = 0; other < crtc_count; other++) {
			/* Returns a compatible CRTC identity rather than confusing its mask index with an object ID. */
			if ((encoder.possible_crtcs & (1U << other)) != 0)
				return crtcs[other];
		}
	}

	/* This connector cannot be driven by any enumerated CRTC. */
	return 0;
}

/* Names the common connector categories without linking libdrm's convenience helpers. */
static const char *
kms_connector_name(
	uint32_t type)
{
	/* Virtual outputs identify QEMU's scanout connector. */
	if (type == DRM_MODE_CONNECTOR_VIRTUAL)
		return "Virtual";

	/* DisplayPort outputs use the conventional DP spelling. */
	if (type == DRM_MODE_CONNECTOR_DisplayPort)
		return "DP";

	/* HDMI digital outputs retain their connector category. */
	if (type == DRM_MODE_CONNECTOR_HDMIA)
		return "HDMI-A";

	/* Embedded display panels use their eDP category. */
	if (type == DRM_MODE_CONNECTOR_eDP)
		return "eDP";

	/* VGA retains its analog connector category. */
	if (type == DRM_MODE_CONNECTOR_VGA)
		return "VGA";

	/* Unknown categories remain distinguishable by the appended connector number. */
	return "Connector";
}

/* Maps lost master to the Vulkan resume contract while reporting other KMS failures explicitly. */
static VkResult
kms_ioctl_result(
	int error)
{
	/* Only failed DRM operations require an errno translation. */
	if (error != 0) {
		/* Access loss invalidates the chain without blocking indefinitely. */
		if (errno == EACCES)
			return VK_ERROR_OUT_OF_DATE_KHR;

		/* logind revocation may report permission loss instead of access denial. */
		if (errno == EPERM)
			return VK_ERROR_OUT_OF_DATE_KHR;

		/* Other failures describe an unusable surface rather than a completed presentation. */
		return VK_ERROR_SURFACE_LOST_KHR;
	}

	/* Succeeded: the DRM operation needs no error translation. */
	return VK_SUCCESS;
}

/* Waits for the matching page-flip event within a finite monotonic deadline. */
static VkResult
kms_flip_wait(
	struct compat_display *display,
	uint64_t token)
{
	VkResult translated_error;
	struct pollfd descriptor;
	unsigned char events[4096];
	struct drm_event header;
	struct drm_event_vblank flip;
	uint64_t start;
	uint64_t elapsed;
	size_t offset;
	ssize_t bytes;
	int error;
	int milliseconds;

	/* All events are read only after poll, including on logind's nonblocking file. */
	start = compat_time();
	for (;;) {
		/* Preserves a finite completion deadline while bounding each nonblocking poll interval. */
		elapsed = compat_time() - start;
		if (elapsed >= KMS_FLIP_TIMEOUT_NS)
			return VK_ERROR_SURFACE_LOST_KHR;

		/* Rounds the remaining finite interval and caps each poll at the agreed 100 milliseconds. */
		milliseconds = (int)((KMS_FLIP_TIMEOUT_NS - elapsed + 999999) / 1000000);
		if (milliseconds > 100)
			milliseconds = 100;

		/* Polls before reading the seat service's potentially nonblocking file. */
		descriptor.fd = display->master_fd;
		descriptor.events = POLLIN;
		descriptor.revents = 0;
		error = poll(&descriptor, 1, milliseconds);
		if (error == 0)
			continue;

		/* An interrupted wait retains the original deadline. */
		if (error < 0) {
			if (errno == EINTR)
				continue;

			/* Other poll errors invalidate this presentation session. */
			return VK_ERROR_SURFACE_LOST_KHR;
		}

		/* A revoked or closed DRM file cannot produce a trustworthy completion event. */
		if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
			return VK_ERROR_OUT_OF_DATE_KHR;

		/* Reads one complete available DRM event batch after readability was established. */
		bytes = read(display->master_fd, events, sizeof(events));
		if (bytes < 0) {
			/* Nonblocking races and interruptions remain inside the same deadline. */
			if (errno == EAGAIN)
				continue;

			/* An interrupted read likewise retries without restarting the timer. */
			if (errno == EINTR)
				continue;

			/* Access loss follows the compositor's out-of-date resume contract. */
			translated_error = kms_ioctl_result(-1);

			/* Reports the corresponding Vulkan KMS failure. */
			return translated_error;
		}

		/* A zero-byte event read means the DRM connection cannot complete the flip. */
		if (bytes == 0)
			return VK_ERROR_SURFACE_LOST_KHR;

		/* Validates each kernel event's length before copying its aligned local representation. */
		offset = 0;
		while ((size_t)bytes - offset >= sizeof(header)) {
			/* Unaligned event streams are decoded through memcpy rather than direct pointer casts. */
			memcpy(&header, events + offset, sizeof(header));
			if (header.length < sizeof(header))
				return VK_ERROR_SURFACE_LOST_KHR;

			/* No event can extend beyond this read batch. */
			if (header.length > (size_t)bytes - offset)
				return VK_ERROR_SURFACE_LOST_KHR;

			/* A page-flip completion carries the submitted chain identity. */
			if (header.type == DRM_EVENT_FLIP_COMPLETE) {
				if (header.length >= sizeof(flip)) {
					memcpy(&flip, events + offset, sizeof(flip));
					if (flip.user_data == token)
						return VK_SUCCESS;
				}
			}

			/* Advances by the validated event length to inspect the next event in this batch. */
			offset += header.length;
		}
	}
}

/* Unwinds a failed inquiry before publishing any incomplete display or mode list. */
static void
kms_records_free(
	void)
{
	struct compat_display *display;
	struct compat_display_mode *mode;

	/* Every retained mode belongs to exactly one still-owned display record. */
	while (kms_displays != NULL) {
		/* Removes this display from the unpublished inquiry list. */
		display = kms_displays;
		kms_displays = display->next;
		while (display->modes != NULL) {
			/* Removes one process-lifetime mode that was never published on this failed inquiry. */
			mode = display->modes;
			display->modes = mode->next;
			free(mode);
		}

		/* No mode now refers to this unpublished display record. */
		free(display);
	}

	/* An unsuccessful inquiry exposes no partial connector count. */
	kms_display_count = 0;

	/* Succeeded: no partially enumerated connector or mode remains. */
	return;
}
