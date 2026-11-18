/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Counts the writes accepted submissions make to swapchain images.
 *
 * A presentation that copies a swapchain image to the display remembers the
 * image's count at that copy; while the count stays the same, a later
 * presentation of the image may show the earlier copy instead of copying
 * again (ws141-p016 (B)).  Only transfer commands that name a destination
 * image are recorded; a swapchain used any other way is never shown without
 * a copy.
 */

#include "internal.h"

static void content_add(struct VkCommandBuffer_T *command, struct vulkan_image *image);

/*
 * Notes that a command buffer's latest command writes an image.
 */
void
vulkan_content_note_image(
	struct VkCommandBuffer_T *command,
	VkImage image)
{
	struct vulkan_image *written;

	/* A null image writes nothing the presentation could show. */
	if (image == VK_NULL_HANDLE)
		return;

	/* Only a swapchain image is ever shown from an earlier copy. */
	written = vulkan_image(image);
	if (written->swapchain_owned == VK_FALSE)
		return;

	/* The image joins the recording's table, or makes it unknown. */
	content_add(command, written);

	/* Succeeded: an accepted submission of the recording counts the write. */
	return;
}

/*
 * Notes that a primary command buffer executes secondary ones.
 */
void
vulkan_content_note_secondaries(
	struct VkCommandBuffer_T *command,
	uint32_t count,
	const VkCommandBuffer *buffers)
{
	struct VkCommandBuffer_T *secondary;
	uint32_t index;
	uint32_t image;

	/* An absent array executes nothing. */
	if (buffers == NULL)
		return;

	/* Every secondary's writes become the primary's. */
	for (index = 0U; index < count; index++) {
		/* A null element executes nothing. */
		secondary = (struct VkCommandBuffer_T *)buffers[index];
		if (secondary == NULL)
			continue;

		/* A secondary that lost track of its writes makes the primary lose track too. */
		if (secondary->content_unknown != VK_FALSE)
			command->content_unknown = VK_TRUE;

		/* Each image the secondary names joins the primary's table. */
		for (image = 0U; image < secondary->content_count; image++)
			content_add(command, secondary->content_images[image]);
	}

	/* Succeeded: an accepted submission of the primary counts the secondaries' writes. */
	return;
}

/*
 * Forgets the writes of a recording that Begin or reset discards.
 */
void
vulkan_content_clear(
	struct VkCommandBuffer_T *command)
{
	/*
	 * An empty, known table: a new recording has written nothing yet, so
	 * the image pointers of the old one are never read again.
	 */
	command->content_count = 0U;
	command->content_unknown = VK_FALSE;

	/* Succeeded: the next recording starts its own table. */
	return;
}

/*
 * Counts every write of an accepted submission.
 *
 * The caller holds the device mutex and has just learned that the renderer
 * accepted the submission, so the counts advance before any later queue
 * operation can be accepted.  The images are alive because submitting a
 * command buffer whose recorded image was destroyed is invalid usage.
 */
void
vulkan_content_commit(
	struct VkDevice_T *device,
	uint32_t count,
	const VkSubmitInfo *submits)
{
	struct VkCommandBuffer_T *command;
	uint32_t submit;
	uint32_t index;
	uint32_t image;

	/* Each submitted command buffer contributes its own table. */
	for (submit = 0U; submit < count; submit++) {
		/* The command buffers of one batch, in order. */
		for (index = 0U; index < submits[submit].commandBufferCount; index++) {
			command = (struct VkCommandBuffer_T *)submits[submit].pCommandBuffers[index];

			/*
			 * The device-wide epoch stands for writes no table could
			 * name: every earlier copy of every swapchain image is
			 * then no longer known to match.
			 */
			if (command->content_unknown != VK_FALSE)
				__atomic_add_fetch(&device->content_epoch, 1U, __ATOMIC_RELEASE);

			/*
			 * Each named image's serial moves on, so the copy a
			 * presentation made of the image before this write is
			 * not shown for it.
			 */
			for (image = 0U; image < command->content_count; image++)
				__atomic_add_fetch(&command->content_images[image]->content_serial, 1U, __ATOMIC_RELEASE);
		}
	}

	/* Succeeded: every write of the submission is counted. */
	return;
}

/* Adds one image to a recording's table without repeats, or marks the table unknown when it is full. */
static void
content_add(
	struct VkCommandBuffer_T *command,
	struct vulkan_image *image)
{
	uint32_t index;

	/* An image already named needs no second entry. */
	for (index = 0U; index < command->content_count; index++) {
		if (command->content_images[index] == image)
			return;
	}

	/* A full table cannot name the image, so the recording's writes become unknown. */
	if (command->content_count == VULKAN_CONTENT_IMAGES) {
		command->content_unknown = VK_TRUE;
		return;
	}

	/* The image takes the next free entry. */
	command->content_images[command->content_count] = image;
	command->content_count++;

	/* Succeeded: the table names the image. */
	return;
}
