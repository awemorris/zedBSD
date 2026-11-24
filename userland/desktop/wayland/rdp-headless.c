/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Headless offscreen Vulkan render target, BGRA->I420 conversion,
 * libmedia H.264 encoding, and socketpair IPC for wayland --headless.
 */

#include "rdp-headless.h"
#include "kwl.h"
#include "userland/desktop/libmedia/media-encoder.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

struct kwl_rdp {
	int fd;
	VkDevice device;
	VkPhysicalDevice physical;
	VkQueue queue;
	uint32_t queue_family;
	VkCommandPool cmd_pool;
	VkCommandBuffer cmd_buf;
	VkFence copy_fence;
	VkImage image;
	VkDeviceMemory image_mem;
	VkBuffer staging;
	VkDeviceMemory staging_mem;
	void *mapped;
	size_t staging_size;
	uint32_t width;
	uint32_t height;
	struct media_encoder *encoder;
	uint8_t *y_plane;
	uint8_t *u_plane;
	uint8_t *v_plane;
	unsigned force_key;
	int closed;
	uint8_t ctrl_buf[KWL_RDP_CTRL_HDR_SIZE];
	size_t ctrl_used;
};

static void put_be32(uint8_t *p, uint32_t v);
static uint32_t get_be32(const uint8_t *p);
static void put_be64(uint8_t *p, uint64_t v);
static uint64_t get_be64(const uint8_t *p);
static uint32_t find_mem_type(VkPhysicalDevice physical, uint32_t filter, VkMemoryPropertyFlags props);
static void rdp_destroy_targets(struct kwl_rdp *rdp);
static int rdp_create_targets(struct kwl_rdp *rdp, uint32_t width, uint32_t height);
static void bgra_to_i420(const uint8_t *bgra, uint32_t width, uint32_t height, uint8_t *y_out, uint8_t *u_out, uint8_t *v_out);
static int write_all_fd(int fd, const uint8_t *buf, size_t len);

static void
put_be32(
	uint8_t *p,
	uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)((v >> 16) & 0xFFU);
	p[2] = (uint8_t)((v >> 8) & 0xFFU);
	p[3] = (uint8_t)(v & 0xFFU);
}

static uint32_t
get_be32(
	const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) |
	       ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) |
	       (uint32_t)p[3];
}

static void
put_be64(
	uint8_t *p,
	uint64_t v)
{
	put_be32(p, (uint32_t)(v >> 32));
	put_be32(p + 4U, (uint32_t)(v & 0xFFFFFFFFU));
}

static uint64_t
get_be64(
	const uint8_t *p)
{
	return ((uint64_t)get_be32(p) << 32) | (uint64_t)get_be32(p + 4U);
}

/* Encodes a 12-byte control message (RESIZE or FORCE_KEY) in big-endian. */
void
kwl_rdp_pack_ctrl(
	uint8_t out[KWL_RDP_CTRL_HDR_SIZE],
	uint32_t type,
	uint32_t arg1,
	uint32_t arg2)
{
	put_be32(out, type);
	put_be32(out + 4U, arg1);
	put_be32(out + 8U, arg2);
}

/* Decodes a 12-byte control message from big-endian. */
void
kwl_rdp_unpack_ctrl(
	const uint8_t in[KWL_RDP_CTRL_HDR_SIZE],
	uint32_t *type,
	uint32_t *arg1,
	uint32_t *arg2)
{
	*type = get_be32(in);
	*arg1 = get_be32(in + 4U);
	*arg2 = get_be32(in + 8U);
}

/* Encodes a 20-byte frame header in big-endian. */
void
kwl_rdp_pack_frame_hdr(
	uint8_t out[KWL_RDP_FRAME_HDR_SIZE],
	uint32_t flags,
	uint64_t pts_us,
	uint32_t size)
{
	put_be32(out, KWL_RDP_MSG_FRAME);
	put_be32(out + 4U, flags);
	put_be64(out + 8U, pts_us);
	put_be32(out + 16U, size);
}

/* Decodes a 20-byte frame header from big-endian. */
void
kwl_rdp_unpack_frame_hdr(
	const uint8_t in[KWL_RDP_FRAME_HDR_SIZE],
	uint32_t *flags,
	uint64_t *pts_us,
	uint32_t *size)
{
	*flags = get_be32(in + 4U);
	*pts_us = get_be64(in + 8U);
	*size = get_be32(in + 16U);
}

/* Clamps and even-aligns headless dimensions into [320..3840] x [240..2160]. */
void
kwl_rdp_clamp_size(
	uint32_t *width,
	uint32_t *height)
{
	uint32_t w;
	uint32_t h;
	w = *width;
	h = *height;
	if (w < KWL_RDP_MIN_WIDTH)
		w = KWL_RDP_MIN_WIDTH;
	if (w > KWL_RDP_MAX_WIDTH)
		w = KWL_RDP_MAX_WIDTH;
	if (h < KWL_RDP_MIN_HEIGHT)
		h = KWL_RDP_MIN_HEIGHT;
	if (h > KWL_RDP_MAX_HEIGHT)
		h = KWL_RDP_MAX_HEIGHT;
	w &= ~1U;
	h &= ~1U;
	*width = w;
	*height = h;
}

static uint32_t
find_mem_type(
	VkPhysicalDevice physical,
	uint32_t filter,
	VkMemoryPropertyFlags props)
{
	VkPhysicalDeviceMemoryProperties mem_props;
	uint32_t i;
	vkGetPhysicalDeviceMemoryProperties(physical, &mem_props);
	for (i = 0U; i < mem_props.memoryTypeCount; i++) {
		if ((filter & (1U << i)) != 0U &&
		    (mem_props.memoryTypes[i].propertyFlags & props) == props)
			return i;
	}

	/* No matching memory type was found. */
	return UINT32_MAX;
}

static void
rdp_destroy_targets(
	struct kwl_rdp *rdp)
{
	if (rdp->encoder != NULL) {
		media_encoder_close(rdp->encoder);
		rdp->encoder = NULL;
	}

	/* Unmaps and releases the host staging buffer. */
	if (rdp->mapped != NULL && rdp->staging_mem != VK_NULL_HANDLE) {
		vkUnmapMemory(rdp->device, rdp->staging_mem);
		rdp->mapped = NULL;
	}

	/* Destroys the Vulkan staging buffer. */
	if (rdp->staging != VK_NULL_HANDLE) {
		vkDestroyBuffer(rdp->device, rdp->staging, NULL);
		rdp->staging = VK_NULL_HANDLE;
	}

	/* Frees staging buffer memory. */
	if (rdp->staging_mem != VK_NULL_HANDLE) {
		vkFreeMemory(rdp->device, rdp->staging_mem, NULL);
		rdp->staging_mem = VK_NULL_HANDLE;
	}

	/* Destroys the offscreen render target image. */
	if (rdp->image != VK_NULL_HANDLE) {
		vkDestroyImage(rdp->device, rdp->image, NULL);
		rdp->image = VK_NULL_HANDLE;
	}

	/* Frees offscreen image device memory. */
	if (rdp->image_mem != VK_NULL_HANDLE) {
		vkFreeMemory(rdp->device, rdp->image_mem, NULL);
		rdp->image_mem = VK_NULL_HANDLE;
	}

	/* Frees planar I420 conversion buffers. */
	free(rdp->y_plane);
	free(rdp->u_plane);
	free(rdp->v_plane);
	rdp->y_plane = NULL;
	rdp->u_plane = NULL;
	rdp->v_plane = NULL;
}

static int
rdp_create_targets(
	struct kwl_rdp *rdp,
	uint32_t width,
	uint32_t height)
{
	VkImageCreateInfo img_info;
	VkBufferCreateInfo buf_info;
	VkMemoryRequirements req;
	VkMemoryAllocateInfo alloc;
	struct media_encoder_config cfg;
	uint32_t mem_type;
	VkResult res;
	size_t y_size;
	size_t uv_size;
	kwl_rdp_clamp_size(&width, &height);
	rdp->width = width;
	rdp->height = height;
	rdp->staging_size = (size_t)width * (size_t)height * 4U;
	memset(&img_info, 0, sizeof(img_info));
	img_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	img_info.imageType = VK_IMAGE_TYPE_2D;
	img_info.format = VK_FORMAT_B8G8R8A8_UNORM;
	img_info.extent.width = width;
	img_info.extent.height = height;
	img_info.extent.depth = 1U;
	img_info.mipLevels = 1U;
	img_info.arrayLayers = 1U;
	img_info.samples = VK_SAMPLE_COUNT_1_BIT;
	img_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	img_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
			 VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	res = vkCreateImage(rdp->device, &img_info, NULL, &rdp->image);
	if (res != VK_SUCCESS)
		return EIO;
	vkGetImageMemoryRequirements(rdp->device, rdp->image, &req);
	mem_type = find_mem_type(rdp->physical, req.memoryTypeBits,
				 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (mem_type == UINT32_MAX)
		return EIO;
	memset(&alloc, 0, sizeof(alloc));
	alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = mem_type;
	res = vkAllocateMemory(rdp->device, &alloc, NULL, &rdp->image_mem);
	if (res != VK_SUCCESS)
		return EIO;
	res = vkBindImageMemory(rdp->device, rdp->image, rdp->image_mem, 0U);
	if (res != VK_SUCCESS)
		return EIO;
	memset(&buf_info, 0, sizeof(buf_info));
	buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buf_info.size = rdp->staging_size;
	buf_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	res = vkCreateBuffer(rdp->device, &buf_info, NULL, &rdp->staging);
	if (res != VK_SUCCESS)
		return EIO;
	vkGetBufferMemoryRequirements(rdp->device, rdp->staging, &req);
	mem_type = find_mem_type(rdp->physical, req.memoryTypeBits,
				 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
				 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (mem_type == UINT32_MAX)
		return EIO;
	memset(&alloc, 0, sizeof(alloc));
	alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = mem_type;
	res = vkAllocateMemory(rdp->device, &alloc, NULL, &rdp->staging_mem);
	if (res != VK_SUCCESS)
		return EIO;
	res = vkBindBufferMemory(rdp->device, rdp->staging, rdp->staging_mem, 0U);
	if (res != VK_SUCCESS)
		return EIO;
	res = vkMapMemory(rdp->device, rdp->staging_mem, 0U, rdp->staging_size,
			  0U, &rdp->mapped);
	if (res != VK_SUCCESS)
		return EIO;
	y_size = (size_t)width * (size_t)height;
	uv_size = (size_t)(width / 2U) * (size_t)(height / 2U);
	rdp->y_plane = (uint8_t *)malloc(y_size);
	rdp->u_plane = (uint8_t *)malloc(uv_size);
	rdp->v_plane = (uint8_t *)malloc(uv_size);
	if (rdp->y_plane == NULL || rdp->u_plane == NULL || rdp->v_plane == NULL)
		return ENOMEM;
	memset(&cfg, 0, sizeof(cfg));
	cfg.codec = MEDIA_CODEC_H264;
	cfg.profile = MEDIA_ENCODER_PROFILE_H264_CONSTRAINED_BASELINE;
	cfg.width = width;
	cfg.height = height;
	cfg.rate_num = 60U;
	cfg.rate_den = 1U;
	cfg.input = MEDIA_ENCODER_INPUT_I420;
	cfg.allow = MEDIA_ENCODER_SOFTWARE | MEDIA_ENCODER_HARDWARE;
	cfg.purpose = MEDIA_ENCODER_LOW_LATENCY;
	cfg.output = MEDIA_ENCODER_ANNEX_B;
	cfg.rate_control = MEDIA_ENCODER_RATE_CQP;
	cfg.qp = 26U;
	res = (VkResult)media_encoder_create(&cfg, &rdp->encoder);
	if ((int)res != 0)
		return EIO;
	rdp->force_key = 1U;
	return 0;
}

/* Initializes headless Vulkan offscreen image, staging buffer, and H.264 encoder. */
int
kwl_rdp_open(
	struct kwl_server *server,
	void *device,
	void *physical,
	void *queue,
	uint32_t queue_family,
	uint32_t width,
	uint32_t height,
	void **out_image)
{
	struct kwl_rdp *rdp;
	VkCommandPoolCreateInfo pool_info;
	VkCommandBufferAllocateInfo cmd_info;
	VkFenceCreateInfo fence_info;
	VkResult res;
	int err;
	rdp = (struct kwl_rdp *)calloc(1U, sizeof(*rdp));
	if (rdp == NULL)
		return ENOMEM;
	rdp->fd = server->rdp_fd;
	rdp->device = (VkDevice)device;
	rdp->physical = (VkPhysicalDevice)physical;
	rdp->queue = (VkQueue)queue;
	rdp->queue_family = queue_family;
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool_info.queueFamilyIndex = queue_family;
	res = vkCreateCommandPool(rdp->device, &pool_info, NULL, &rdp->cmd_pool);
	if (res != VK_SUCCESS) {
		free(rdp);
		return EIO;
	}

	/* Allocates the primary copy command buffer. */
	memset(&cmd_info, 0, sizeof(cmd_info));
	cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cmd_info.commandPool = rdp->cmd_pool;
	cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cmd_info.commandBufferCount = 1U;
	res = vkAllocateCommandBuffers(rdp->device, &cmd_info, &rdp->cmd_buf);
	if (res != VK_SUCCESS) {
		vkDestroyCommandPool(rdp->device, rdp->cmd_pool, NULL);
		free(rdp);
		return EIO;
	}

	/* Creates the fence used to wait on staging readback. */
	memset(&fence_info, 0, sizeof(fence_info));
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	res = vkCreateFence(rdp->device, &fence_info, NULL, &rdp->copy_fence);
	if (res != VK_SUCCESS) {
		vkDestroyCommandPool(rdp->device, rdp->cmd_pool, NULL);
		free(rdp);
		return EIO;
	}

	/* Allocates the offscreen image, staging buffer, and H.264 encoder. */
	err = rdp_create_targets(rdp, width, height);
	if (err != 0) {
		rdp_destroy_targets(rdp);
		vkDestroyFence(rdp->device, rdp->copy_fence, NULL);
		vkDestroyCommandPool(rdp->device, rdp->cmd_pool, NULL);
		free(rdp);
		return err;
	}

	/* Publishes the headless state on the compositor server. */
	server->rdp = rdp;
	if (out_image != NULL)
		*out_image = (void *)rdp->image;
	return 0;
}

/* Recreates offscreen image and encoder for a dynamic resize. */
int
kwl_rdp_resize(
	struct kwl_server *server,
	uint32_t width,
	uint32_t height,
	void **out_image)
{
	struct kwl_rdp *rdp;
	int err;
	rdp = server->rdp;
	if (rdp == NULL)
		return EINVAL;
	kwl_rdp_clamp_size(&width, &height);
	if (width == rdp->width && height == rdp->height) {
		rdp->force_key = 1U;
		return 0;
	}

	/* Waits for the GPU to finish before recreating targets. */
	vkDeviceWaitIdle(rdp->device);
	rdp_destroy_targets(rdp);
	err = rdp_create_targets(rdp, width, height);
	if (err != 0)
		return err;
	if (out_image != NULL)
		*out_image = (void *)rdp->image;
	kwl_output_size_set(server, width, height, 0U, server->scale_wanted);
	server->dirty = 1U;
	return 0;
}

static void
bgra_to_i420(
	const uint8_t *bgra,
	uint32_t width,
	uint32_t height,
	uint8_t *y_out,
	uint8_t *u_out,
	uint8_t *v_out)
{
	uint32_t y;
	uint32_t x;
	uint32_t uv_w;
	const uint8_t *row;
	uint8_t *y_row;
	int32_t b;
	int32_t g;
	int32_t r;
	int32_t yy;
	int32_t uu;
	int32_t vv;
	size_t uv_idx;
	uv_w = width / 2U;
	for (y = 0U; y < height; y++) {
		row = bgra + (size_t)y * (size_t)width * 4U;
		y_row = y_out + (size_t)y * (size_t)width;
		for (x = 0U; x < width; x++) {
			b = row[x * 4U + 0U];
			g = row[x * 4U + 1U];
			r = row[x * 4U + 2U];
			yy = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
			if (yy < 0)
				yy = 0;
			if (yy > 255)
				yy = 255;
			y_row[x] = (uint8_t)yy;
			if ((y & 1U) == 0U && (x & 1U) == 0U) {
				uu = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
				vv = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
				uv_idx = (size_t)(y / 2U) * (size_t)uv_w + (size_t)(x / 2U);
				if (uu < 0)
					uu = 0;
				if (uu > 255)
					uu = 255;
				if (vv < 0)
					vv = 0;
				if (vv > 255)
					vv = 255;
				u_out[uv_idx] = (uint8_t)uu;
				v_out[uv_idx] = (uint8_t)vv;
			}
		}
	}
}

static int
write_all_fd(
	int fd,
	const uint8_t *buf,
	size_t len)
{
	size_t off;
	ssize_t n;
	struct pollfd pfd;
	int prc;
	int err;
	off = 0U;
	while (off < len) {
		n = write(fd, buf + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				pfd.fd = fd;
				pfd.events = POLLOUT;
				pfd.revents = 0;
				prc = poll(&pfd, 1, 1000);
				if (prc <= 0)
					return EIO;
				continue;
			}

			/* Propagates non-retryable write errors. */
			err = errno;
			if (err == 0)
				err = EIO;
			return err;
		}

		/* Advances past the written bytes. */
		off += (size_t)n;
	}

	/* Succeeded. */
	return 0;
}

/* Reads back the rendered offscreen image, encodes H.264 Annex-B AU, and sends it on rdp_fd. */
int
kwl_rdp_encode_and_send(
	struct kwl_rdp *rdp,
	void *image,
	uint32_t width,
	uint32_t height,
	uint64_t pts_us)
{
	VkCommandBufferBeginInfo begin;
	VkImageMemoryBarrier barrier;
	VkBufferImageCopy region;
	VkSubmitInfo submit;
	struct media_encoder_picture pic;
	struct media_encoded pkt;
	uint8_t hdr[KWL_RDP_FRAME_HDR_SIZE];
	uint32_t send_flags;
	uint32_t out_flags;
	int err;
	VkResult vres;
	if (rdp == NULL || rdp->closed || rdp->fd < 0)
		return 0;
	if (width != rdp->width || height != rdp->height)
		return 0;
	vkResetFences(rdp->device, 1U, &rdp->copy_fence);
	vkResetCommandBuffer(rdp->cmd_buf, 0U);
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vres = vkBeginCommandBuffer(rdp->cmd_buf, &begin);
	if (vres != VK_SUCCESS)
		return EIO;
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.image = (VkImage)image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1U;
	barrier.subresourceRange.layerCount = 1U;
	vkCmdPipelineBarrier(rdp->cmd_buf,
			     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			     VK_PIPELINE_STAGE_TRANSFER_BIT,
			     0U, 0U, NULL, 0U, NULL, 1U, &barrier);
	memset(&region, 0, sizeof(region));
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1U;
	region.imageExtent.width = width;
	region.imageExtent.height = height;
	region.imageExtent.depth = 1U;
	vkCmdCopyImageToBuffer(rdp->cmd_buf, (VkImage)image,
			       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			       rdp->staging, 1U, &region);
	vres = vkEndCommandBuffer(rdp->cmd_buf);
	if (vres != VK_SUCCESS)
		return EIO;
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &rdp->cmd_buf;
	vres = vkQueueSubmit(rdp->queue, 1U, &submit, rdp->copy_fence);
	if (vres != VK_SUCCESS)
		return EIO;
	vres = vkWaitForFences(rdp->device, 1U, &rdp->copy_fence, VK_TRUE,
			       UINT64_MAX);
	if (vres != VK_SUCCESS)
		return EIO;
	bgra_to_i420((const uint8_t *)rdp->mapped, width, height,
		     rdp->y_plane, rdp->u_plane, rdp->v_plane);
	memset(&pic, 0, sizeof(pic));
	pic.plane[0] = rdp->y_plane;
	pic.plane[1] = rdp->u_plane;
	pic.plane[2] = rdp->v_plane;
	pic.stride[0] = width;
	pic.stride[1] = width / 2U;
	pic.stride[2] = width / 2U;
	send_flags = 0U;
	if (rdp->force_key != 0U)
		send_flags = MEDIA_ENCODE_FORCE_KEY;
	rdp->force_key = 0U;
	err = media_encoder_send(rdp->encoder, &pic, (int64_t)pts_us, send_flags);
	if (err != 0)
		return err;
	memset(&pkt, 0, sizeof(pkt));
	err = media_encoder_receive(rdp->encoder, 0U, &pkt);
	if (err != 0 || pkt.data == NULL || pkt.size == 0U)
		return err;
	out_flags = 0U;
	if ((pkt.flags & MEDIA_ENCODED_KEY) != 0U)
		out_flags = KWL_RDP_FRAME_KEY;
	kwl_rdp_pack_frame_hdr(hdr, out_flags, pts_us, (uint32_t)pkt.size);
	err = write_all_fd(rdp->fd, hdr, sizeof(hdr));
	if (err == 0)
		err = write_all_fd(rdp->fd, (const uint8_t *)pkt.data, pkt.size);
	if (err != 0)
		rdp->closed = 1;
	return err;
}

/* Polls rdp_fd for RESIZE / FORCE_KEY control messages from rdpd. */
void
kwl_rdp_poll(
	struct kwl_server *server)
{
	struct kwl_rdp *rdp;
	ssize_t n;
	uint32_t type;
	uint32_t arg1;
	uint32_t arg2;
	rdp = server->rdp;
	if (rdp == NULL || rdp->fd < 0 || rdp->closed)
		return;
	for (;;) {
		n = read(rdp->fd, rdp->ctrl_buf + rdp->ctrl_used,
			 sizeof(rdp->ctrl_buf) - rdp->ctrl_used);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			rdp->closed = 1;
			server->failed = 1;
			break;
		}

		/* Stops compositor if the rdpd IPC peer closed the socket. */
		if (n == 0) {
			rdp->closed = 1;
			server->failed = 1;
			break;
		}

		/* Accumulates and dispatches complete 12-byte control messages. */
		rdp->ctrl_used += (size_t)n;
		if (rdp->ctrl_used == sizeof(rdp->ctrl_buf)) {
			kwl_rdp_unpack_ctrl(rdp->ctrl_buf, &type, &arg1, &arg2);
			rdp->ctrl_used = 0U;
			if (type == KWL_RDP_MSG_RESIZE)
				kwl_compose_headless_resize(server, arg1, arg2);
			else if (type == KWL_RDP_MSG_FORCE_KEY)
				rdp->force_key = 1U;
		}
	}
}

/* Releases all headless Vulkan and encoder resources. */
void
kwl_rdp_close(
	struct kwl_server *server)
{
	struct kwl_rdp *rdp;
	rdp = server->rdp;
	if (rdp == NULL)
		return;
	rdp_destroy_targets(rdp);
	if (rdp->copy_fence != VK_NULL_HANDLE)
		vkDestroyFence(rdp->device, rdp->copy_fence, NULL);
	if (rdp->cmd_pool != VK_NULL_HANDLE)
		vkDestroyCommandPool(rdp->device, rdp->cmd_pool, NULL);
	free(rdp);
	server->rdp = NULL;
}
