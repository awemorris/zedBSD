/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Pairs cached native display nodes with independently discovered Vulkan renderers.
 */

#include "wsi-internal.h"

#include <uapi/gpu-scanout.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* One immutable node identity retains a separate discovery open until instance destruction. */
struct wsi_display_node {
	struct wsi_display_node *next;
	struct gpu_device_info info;
	uint64_t renderer;
	char path[262];
	int fd;
	pthread_mutex_t mutex;
};

/* One published inventory owns all nodes and uses its instance's allocation policy. */
struct wsi_display_inventory {
	struct wsi_display_inventory *next;
	struct VkInstance_T *instance;
	struct vulkan_allocator allocator;
	struct wsi_display_node *nodes;
};

/* Immutable inventories are published together and removed only during external instance teardown. */
static struct wsi_display_inventory *inventories;
/* Serializes inventory publication; native requests use each node's independent mutex. */
static pthread_mutex_t inventory_mutex = PTHREAD_MUTEX_INITIALIZER;

static VkResult inventory_get(struct VkInstance_T *instance, struct wsi_display_inventory **result);
static VkResult inventory_create(struct VkInstance_T *instance, struct wsi_display_inventory **result);
static VkResult inventory_add(struct wsi_display_inventory *inventory, const char *name);
static void inventory_free(struct wsi_display_inventory *inventory);
static int node_ioctl(struct wsi_display_node *node, unsigned long command, void *argument);
static VkBool32 node_is_renderer(struct wsi_display_inventory *inventory, struct wsi_display_node *node);
static VkBool32 node_name(const char *name);
static uint64_t renderer_identity(struct wsi_display_inventory *inventory, struct VkPhysicalDevice_T *physical);

/*
 * Enumerates only displays assigned to this Vulkan renderer by stable native identity.
 */
VkResult
vulkan_wsi_display_node_query(
	struct VkPhysicalDevice_T *physical,
	uint32_t index,
	uint32_t *count,
	struct gpu_display_info *request,
	uint64_t *device_id,
	char *path)
{
	struct wsi_display_inventory *inventory;
	struct wsi_display_node *node;
	struct gpu_display_info probe;
	uint64_t renderer;
	uint32_t total;
	VkResult error;
	int status;
	VkBool32 found;

	/* Discovery is cached once; ordinary presentation never scans /dev or opens renderer contexts. */
	error = inventory_get(physical->instance, &inventory);
	if (error != VK_SUCCESS)
		return error;

	/* A physical renderer may legitimately have no assigned native display. */
	renderer = renderer_identity(inventory, physical);
	total = 0U;
	found = VK_FALSE;
	for (node = inventory->nodes; node != NULL; node = node->next) {
		/* Render-only nodes and another renderer's outputs cannot enter this enumeration. */
		if (renderer == 0U ||
		    node->renderer != renderer ||
		    (node->info.roles & GPU_DEVICE_DISPLAY) == 0U)
			continue;

		/* Dynamic output counts are queried through the cached display-only admission lock. */
		memset(&probe, 0, sizeof(probe));
		probe.version = GPU_ABI_VERSION;
		probe.size = sizeof(probe);
		probe.index = GPU_DISPLAY_COUNT_ONLY;
		status = node_ioctl(node, GPU_DISPLAY_QUERY, &probe);
		if (status != 0)
			return VK_ERROR_SURFACE_LOST_KHR;

		/* Count arithmetic must not alias two native ordinals after wraparound. */
		if (probe.count > UINT32_MAX - total)
			return VK_ERROR_OUT_OF_HOST_MEMORY;

		/* The requested ordinal is local to exactly one paired display node. */
		if (index != UINT32_MAX &&
		    index >= total &&
		    index - total < probe.count) {
			memset(request, 0, sizeof(*request));
			request->version = GPU_ABI_VERSION;
			request->size = sizeof(*request);
			request->index = index - total;
			status = node_ioctl(node, GPU_DISPLAY_QUERY, request);
			if (status != 0 || request->count != probe.count)
				return VK_ERROR_SURFACE_LOST_KHR;

			/* Native identity includes its device, so equal local display IDs never collide. */
			*device_id = node->info.device_id;
			memcpy(path, node->path, sizeof(node->path));
			found = VK_TRUE;
		}

		/* Independent nodes contribute disjoint ordinal intervals in identity order. */
		total += probe.count;
	}

	/* An absent indexed output reflects a topology change, not a replacement identity. */
	if (index != UINT32_MAX && found == VK_FALSE)
		return VK_ERROR_SURFACE_LOST_KHR;

	/* Succeeded: zero outputs is a valid renderer inventory. */
	*count = total;
	return VK_SUCCESS;
}

/*
 * Performs a mode or constraint query on the exact display node named by an output snapshot.
 */
int
vulkan_wsi_display_node_ioctl(
	struct VkPhysicalDevice_T *physical,
	const struct vulkan_wsi_output *output,
	unsigned long command,
	void *argument)
{
	struct wsi_display_inventory *inventory;
	struct wsi_display_node *node;
	uint64_t renderer;
	VkResult error;
	int status;

	/* Snapshot identity must resolve within this instance's cached paired inventory. */
	error = inventory_get(physical->instance, &inventory);
	if (error != VK_SUCCESS) {
		errno = ENODEV;
		return -1;
	}

	/* A saved ordinal or reused path cannot substitute a different native device. */
	renderer = renderer_identity(inventory, physical);
	for (node = inventory->nodes; node != NULL; node = node->next) {
		/* Both the immutable device identity and assigned rendering companion must match. */
		if (node->info.device_id != output->device_identifier || node->renderer != renderer)
			continue;

		/* The matching node retains the exact snapshot's discovery file description. */
		break;
	}

	/* An absent pair cannot substitute another device merely because its path was reused. */
	if (node == NULL) {
		errno = ENODEV;
		return -1;
	}

	/* Native admission is independent from renderer transactions and preserves its own errno. */
	status = node_ioctl(node, command, argument);
	if (status != 0)
		return -1;

	/* Succeeded: the request used the snapshot's exact paired display device. */
	return 0;
}

/*
 * Sums the topology sequences of the display nodes assigned to this renderer.
 *
 * Each node's sequence only grows, so the sum grows exactly when one of them
 * does: a hotplug fence compares it with the sum taken at registration.  The
 * query is the non-destructive one on the node's discovery open, which no
 * lease shares, and it acknowledges nothing.  A node without topology events
 * contributes nothing.
 */
VkResult
vulkan_wsi_display_node_topology(
	struct VkPhysicalDevice_T *physical,
	uint64_t *sequence)
{
	struct wsi_display_inventory *inventory;
	struct wsi_display_node *node;
	struct gpu_display_events request;
	uint64_t renderer;
	uint64_t total;
	VkResult error;
	int status;

	/* The cached inventory names the nodes and their discovery opens. */
	error = inventory_get(physical->instance, &inventory);
	if (error != VK_SUCCESS)
		return error;

	/* Adds the current sequence of every display node assigned to this renderer. */
	renderer = renderer_identity(inventory, physical);
	total = 0U;
	for (node = inventory->nodes; node != NULL; node = node->next) {
		/* Another renderer's outputs and render-only nodes have no hotplug for this device. */
		if (renderer == 0U ||
		    node->renderer != renderer ||
		    (node->info.roles & GPU_DEVICE_DISPLAY) == 0U)
			continue;

		/* Samples the node's topology sequence without acknowledging it. */
		memset(&request, 0, sizeof(request));
		request.version = GPU_ABI_VERSION;
		request.size = sizeof(request);
		status = node_ioctl(node, GPU_DISPLAY_EVENTS, &request);
		if (status != 0) {
			/* A node without topology events never reports a hotplug. */
			if (errno == EOPNOTSUPP)
				continue;

			/* A failed node cannot say whether its topology changed. */
			return VK_ERROR_SURFACE_LOST_KHR;
		}

		/* The sum cannot wrap: each sequence counts events from one. */
		total += request.sequence;
	}

	/* Succeeded: the caller compares this sum with an earlier one. */
	*sequence = total;
	return VK_SUCCESS;
}

/*
 * Retires cached native discovery opens after every instance-owned surface has been destroyed.
 */
void
vulkan_wsi_display_nodes_finish(
	struct VkInstance_T *instance)
{
	struct wsi_display_inventory **link;
	struct wsi_display_inventory *inventory;

	/* Instance destruction is externally synchronized; publication only needs a short list lock. */
	pthread_mutex_lock(&inventory_mutex);

	/* Locate only the inventory whose externally synchronized instance is retiring. */
	link = &inventories;
	while (*link != NULL && (*link)->instance != instance)
		link = &(*link)->next;

	/* An instance which never queried direct displays has no native inventory. */
	inventory = *link;
	if (inventory != NULL)
		*link = inventory->next;

	pthread_mutex_unlock(&inventory_mutex);

	/* Native close and application allocation callbacks execute after removing the public inventory. */
	if (inventory != NULL)
		inventory_free(inventory);

	/* Succeeded: this instance retains no display discovery file descriptions. */
	return;
}

/* Reuses an immutable published inventory or builds one outside publication serialization. */
static VkResult
inventory_get(
	struct VkInstance_T *instance,
	struct wsi_display_inventory **result)
{
	struct wsi_display_inventory *inventory;
	struct wsi_display_inventory *candidate;
	VkResult error;

	/* Application callbacks and native opens are never called under the inventory lock. */
	pthread_mutex_lock(&inventory_mutex);

	/* Instance identity selects its one immutable, already-published native discovery snapshot. */
	for (inventory = inventories; inventory != NULL; inventory = inventory->next) {
		/* All physical devices of one instance share its single native identity inventory. */
		if (inventory->instance == instance)
			break;
	}

	pthread_mutex_unlock(&inventory_mutex);

	/* Already published inventories are immutable until the externally synchronized instance teardown. */
	if (inventory != NULL) {
		*result = inventory;
		return VK_SUCCESS;
	}

	/* A complete private candidate includes pairing decisions before any reader can see it. */
	error = inventory_create(instance, &candidate);
	if (error != VK_SUCCESS)
		return error;

	/* A concurrent query may have published an equivalent inventory during allocation. */
	pthread_mutex_lock(&inventory_mutex);

	/* Instance identity selects its one immutable, already-published native discovery snapshot. */
	for (inventory = inventories; inventory != NULL; inventory = inventory->next) {
		/* Identity is the instance object, independent of public handle representation. */
		if (inventory->instance == instance)
			break;
	}

	/* Exactly one candidate transfers ownership to the global inventory list. */
	if (inventory == NULL) {
		inventory = candidate;
		candidate = NULL;
		inventory->next = inventories;
		inventories = inventory;
	}

	pthread_mutex_unlock(&inventory_mutex);

	/* A losing candidate has no external users and can close all its discovery opens now. */
	if (candidate != NULL)
		inventory_free(candidate);

	/* Succeeded: the caller borrows the inventory through its live instance. */
	*result = inventory;
	return VK_SUCCESS;
}

/* Captures device identity and assigns each display to an actual available Vulkan renderer. */
static VkResult
inventory_create(
	struct VkInstance_T *instance,
	struct wsi_display_inventory **result)
{
	struct wsi_display_inventory *inventory;
	struct wsi_display_node *node;
	struct wsi_display_node *renderer;
	struct dirent *entry;
	DIR *directory;
	uint64_t first_renderer;
	VkBool32 selected;
	VkResult error;
	int status;

	/* Saved instance allocation policy owns all implicit native discovery objects. */
	inventory = vulkan_allocate(&instance->object.allocator, sizeof(*inventory), sizeof(void *), VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
	if (inventory == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* The candidate retains its instance allocator through publication or rollback. */
	memset(inventory, 0, sizeof(*inventory));
	inventory->instance = instance;
	inventory->allocator = instance->object.allocator;

	/* A finite directory traversal discovers device nodes without assuming contiguous minor numbers. */
	directory = opendir("/dev");
	if (directory == NULL) {
		inventory_free(inventory);
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* Every accepted node is inserted by stable device identity rather than filesystem traversal order. */
	error = VK_SUCCESS;
	while (1) {
		/* A null entry ends traversal only when the directory operation left no error. */
		errno = 0;
		entry = readdir(directory);
		if (entry == NULL) {
			/* A partial native inventory is never published as complete on an I/O error. */
			if (errno != 0)
				error = VK_ERROR_INITIALIZATION_FAILED;
			break;
		}

		/* Unrelated /dev entries are not opened or probed. */
		selected = node_name(entry->d_name);
		if (selected == VK_FALSE)
			continue;

		/* Node setup unwinds its own ownership on every failure. */
		error = inventory_add(inventory, entry->d_name);
		if (error != VK_SUCCESS)
			break;
	}

	/* Directory ownership ends before the inventory is either paired or rolled back. */
	status = closedir(directory);
	if (status != 0 && error == VK_SUCCESS)
		error = VK_ERROR_INITIALIZATION_FAILED;

	/* A partial or unclosed directory snapshot never becomes a public inventory. */
	if (error != VK_SUCCESS) {
		inventory_free(inventory);
		return error;
	}

	/* The default is the lowest stable native identity actually represented by a Vulkan physical device. */
	first_renderer = 0U;
	for (node = inventory->nodes; node != NULL; node = node->next) {
		/* A role bit alone does not establish support by the Vulkan renderer implementation. */
		selected = node_is_renderer(inventory, node);
		if (selected != VK_FALSE) {
			first_renderer = node->info.device_id;
			break;
		}
	}

	/* Companion hints select only existing renderers; unpaired display-only nodes use the explicit default. */
	for (node = inventory->nodes; node != NULL; node = node->next) {
		/* Rendering nodes select themselves unless a valid companion explicitly overrides them. */
		node->renderer = first_renderer;
		selected = node_is_renderer(inventory, node);
		if (selected != VK_FALSE)
			node->renderer = node->info.device_id;

		/* An absent companion is not a reason to hide a display which supports copied presentation. */
		for (renderer = inventory->nodes; renderer != NULL; renderer = renderer->next) {
			/* Only the exact hinted native identity is eligible to replace the default. */
			if (renderer->info.device_id != node->info.companion_id)
				continue;

			/* Backend hints cannot create a Vulkan renderer from a display-only node. */
			selected = node_is_renderer(inventory, renderer);
			if (selected != VK_FALSE)
				node->renderer = renderer->info.device_id;
			break;
		}
	}

	/* Succeeded: all pairing decisions are fixed for this instance's native node inventory. */
	*result = inventory;
	return VK_SUCCESS;
}

/* Opens one compatible native node and inserts its fully initialized identity in stable order. */
static VkResult
inventory_add(
	struct wsi_display_inventory *inventory,
	const char *name)
{
	struct wsi_display_node *node;
	struct wsi_display_node **link;
	size_t length;
	int status;
	int error;

	/* The exact native path must fit the same bounded path contract used by rendering contexts. */
	length = strlen(name);
	if (length > 256U)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* A node wrapper owns the bounded path before any native open can succeed. */
	node = vulkan_allocate(&inventory->allocator, sizeof(*node), sizeof(void *), VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
	if (node == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Native identity and lock ownership are filled only after the exact path is stable. */
	memset(node, 0, sizeof(*node));
	memcpy(node->path, "/dev/", 5U);
	memcpy(node->path + 5U, name, length + 1U);

	/* A vanished or inaccessible unrelated node contributes no display inventory. */
	node->fd = open(node->path, O_RDWR | O_CLOEXEC);
	if (node->fd < 0) {
		vulkan_free(&inventory->allocator, node);
		return VK_SUCCESS;
	}

	/* A kernel device identity is queried without issuing any renderer protocol commands. */
	node->info.version = GPU_ABI_VERSION;
	node->info.size = sizeof(node->info);
	status = ioctl(node->fd, GPU_DEVICE_QUERY, &node->info);
	error = errno;
	if (status != 0) {
		close(node->fd);
		vulkan_free(&inventory->allocator, node);

		/* Old or incompatible native interfaces are absent rather than assigned guessed identities. */
		if (error == ENOTTY ||
		    error == ENOTSUP ||
		    error == ENODEV)
			return VK_SUCCESS;

		/* An operational discovery failure is not an unsupported old interface. */
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* A zero identity cannot safely distinguish a reused node from an earlier device generation. */
	if (node->info.device_id == 0U) {
		close(node->fd);
		vulkan_free(&inventory->allocator, node);
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* Every cached discovery fd has a distinct admission lock unrelated to renderer transactions. */
	status = pthread_mutex_init(&node->mutex, NULL);
	if (status != 0) {
		close(node->fd);
		vulkan_free(&inventory->allocator, node);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Stable identity order makes the default renderer independent of readdir order. */
	link = &inventory->nodes;
	while (*link != NULL && (*link)->info.device_id < node->info.device_id)
		link = &(*link)->next;

	/* Multiple paths to the same device cannot publish duplicate physical outputs. */
	if (*link != NULL && (*link)->info.device_id == node->info.device_id) {
		pthread_mutex_destroy(&node->mutex);
		close(node->fd);
		vulkan_free(&inventory->allocator, node);
		return VK_SUCCESS;
	}

	/* Succeeded: the private inventory now owns this complete node. */
	node->next = *link;
	*link = node;
	return VK_SUCCESS;
}

/* Releases a private or withdrawn inventory without holding publication or native locks. */
static void
inventory_free(
	struct wsi_display_inventory *inventory)
{
	struct wsi_display_node *node;
	struct wsi_display_node *next;

	/* No native callback or output identity can borrow a node after its inventory is withdrawn. */
	for (node = inventory->nodes; node != NULL; node = next) {
		next = node->next;
		close(node->fd);
		pthread_mutex_destroy(&node->mutex);
		vulkan_free(&inventory->allocator, node);
	}

	/* Succeeded: the instance allocator retires the final inventory wrapper. */
	vulkan_free(&inventory->allocator, inventory);
	return;
}

/* Serializes a raw native discovery transaction without retaining any renderer context lock. */
static int
node_ioctl(
	struct wsi_display_node *node,
	unsigned long command,
	void *argument)
{
	int status;
	int error;

	/* Shared inventory readers cannot overlap requests on one native open. */
	pthread_mutex_lock(&node->mutex);

	status = ioctl(node->fd, command, argument);
	error = errno;

	pthread_mutex_unlock(&node->mutex);

	/* Error identity survives mutex operations before returning to the adapter. */
	if (status != 0) {
		errno = error;
		return -1;
	}

	/* Succeeded: all request data is available outside the native admission lock. */
	return 0;
}

/* Checks actual Vulkan discovery rather than treating a role bit as renderer implementation support. */
static VkBool32
node_is_renderer(
	struct wsi_display_inventory *inventory,
	struct wsi_display_node *node)
{
	struct VkPhysicalDevice_T *physical;
	uint32_t index;
	int equal;

	/* Display-only devices never become fabricated VkPhysicalDevice handles. */
	if ((node->info.roles & GPU_DEVICE_RENDER) == 0U)
		return VK_FALSE;

	/* One native renderer may supply several Vulkan physical devices through the same context. */
	for (index = 0U; index < inventory->instance->physical_device_count; index++) {
		/* Match only the native path retained by an actual discovered rendering context. */
		physical = inventory->instance->physical_devices[index];
		equal = strcmp(physical->object.context->device_path, node->path);
		if (equal == 0)
			break;
	}

	/* No compatible Vulkan renderer was actually discovered for this native node. */
	if (index == inventory->instance->physical_device_count)
		return VK_FALSE;

	/* Succeeded: this native node is represented by a real Vulkan physical device. */
	return VK_TRUE;
}

/* Accepts only ordinary GPU minor names, including sparse and multi-digit device numbers. */
static VkBool32
node_name(
	const char *name)
{
	size_t index;

	/* Short-circuiting keeps the prefix check within the terminating byte of short names. */
	if (name[0] != 'g' ||
	    name[1] != 'p' ||
	    name[2] != 'u' ||
	    name[3] == '\0')
		return VK_FALSE;

	/* Suffix bytes must be decimal digits rather than an unrelated driver-specific endpoint. */
	for (index = 3U; name[index] != '\0'; index++) {
		/* Every suffix character belongs to the numeric minor namespace. */
		if (name[index] < '0' || name[index] > '9')
			return VK_FALSE;
	}

	/* Succeeded: this name belongs to the ordinary GPU node namespace. */
	return VK_TRUE;
}

/* Resolves the physical renderer's stable device identity within the cached inventory. */
static uint64_t
renderer_identity(
	struct wsi_display_inventory *inventory,
	struct VkPhysicalDevice_T *physical)
{
	struct wsi_display_node *node;
	int equal;

	/* The rendering context retained the exact device path when its instance was created. */
	for (node = inventory->nodes; node != NULL; node = node->next) {
		/* The retained renderer path identifies its matching cached native identity. */
		equal = strcmp(node->path, physical->object.context->device_path);
		if (equal == 0)
			break;
	}

	/* An absent renderer has no native display association in this inventory. */
	if (node == NULL)
		return 0U;

	/* Succeeded: the physical renderer resolves to this stable native identity. */
	return node->info.device_id;
}
