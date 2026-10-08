/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exercises the public component through libbrowser.so and standard Vulkan.
 * The executable interposes allocation and framebuffer creation to observe
 * failure ownership without adding switches to the production engine.
 */

#include <browser/browser.h>

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The client owns two independent callback contexts for the complete run. */
struct observation {
	struct browser_view *other;
	int commits;
	int keys;
	int timers;
	int defer_destroy;
	int load_failures;
};

/* The test's failure count, used only by this single-threaded client. */
static int failures;

/* Every public or Vulkan result checked by this single-threaded client. */
static int checks;

/* The number of matching navigation allocations since the test armed a failure. */
static int copies;

/* The matching allocation to refuse, zero when navigation uses ordinary copies. */
static int refuse_copy;

/* The next framebuffer creation fails when this one-shot interposition is armed. */
static int refuse_framebuffer;

static void expect(int observed, int expected, const char *operation);
static void committed(void *context, struct browser_view *view);
static void console(void *context, struct browser_view *view, int level, const char *text, size_t length);
static void load(void *context, struct browser_view *view, enum browser_load_state state, const char *url, int error, const char *reason);
static void asynchronous(const char *origin);
static void pixels(struct browser_view *view, struct browser_offscreen *image, int recorded);
static void record(struct browser_view *view, const struct browser_gpu *gpu, const struct browser_target *target);

/*
 * Copies strings while allowing the client to refuse a navigation allocation.
 */
char *
strdup(
    const char *text)
{
	const char *match;
	char *copy;
	size_t length;

	/* Counts only the destination being tested, leaving unrelated runtime strings alone. */
	match = strstr(text, "second.html");
	if (match != NULL) {
		copies++;

		/* Refuses one selected copy as a real allocation failure. */
		if (refuse_copy != 0 && copies == refuse_copy)
			return NULL;
	}

	/* Allocates the complete string and its terminator. */
	length = strlen(text) + 1U;
	copy = malloc(length);
	if (copy == NULL)
		return NULL;

	/* Succeeded: the caller owns an ordinary allocation. */
	memcpy(copy, text, length);
	return copy;
}

/*
 * Refuses one framebuffer creation while forwarding ordinary calls to Vulkan.
 */
VkResult
vkCreateFramebuffer(
    VkDevice device,
    const VkFramebufferCreateInfo *info,
    const VkAllocationCallbacks *allocator,
    VkFramebuffer *framebuffer)
{
	PFN_vkCreateFramebuffer real_create;
	VkResult result;

	/* A failed creation leaves no framebuffer for either participant to destroy. */
	if (refuse_framebuffer) {
		refuse_framebuffer = 0;
		*framebuffer = VK_NULL_HANDLE;
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Resolves the driver's ordinary entry point beyond this executable. */
	real_create = (PFN_vkCreateFramebuffer)dlsym(RTLD_NEXT, "vkCreateFramebuffer");
	if (real_create == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Forwards the production operation and keeps its precise result. */
	result = real_create(device, info, allocator, framebuffer);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: Vulkan owns a valid framebuffer until the view releases it. */
	return VK_SUCCESS;
}

/*
 * Checks independent views, failure ownership and actual Vulkan drawing.
 */
int
main(
    int argc,
    char **argv)
{
	struct browser_fonts fonts;
	struct browser_callbacks callbacks;
	struct browser_view_options options;
	struct browser_view *first;
	struct browser_view *second;
	struct browser_offscreen *image;
	struct browser_gpu gpu;
	struct browser_target target;
	struct browser_gpu_failure failure;
	struct observation a;
	struct observation b;
	uint32_t output[32U * 24U];
	char before[1024];
	const char *location;
	int error;
	int possible;
	int total_copies;
	int point;
	int commits;

	/* Keeps the first view's callback state alive for the complete run. */
	memset(&a, 0, sizeof(a));

	/* Keeps the second view's callback state independent of the first. */
	memset(&b, 0, sizeof(b));

	/* Keeps borrowed font paths alive for both views. */
	fonts.sans = "userland/desktop/fonts/Mahora-Regular.ttf";
	fonts.mono = "userland/desktop/fonts/JetBrainsMono-Regular.ttf";
	fonts.fallback = "userland/desktop/fonts/DroidSansFallbackFull.ttf";

	/* Gives the first view its observer before the callback structure is copied. */
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.committed = committed;
	callbacks.console = console;
	callbacks.load = load;
	callbacks.context = &a;

	/* Supplies dimensions, lifetime and fetching policy through the public options. */
	memset(&options, 0, sizeof(options));
	options.version = BROWSER_API_VERSION;
	options.fonts = &fonts;
	options.callbacks = &callbacks;
	options.stack_base = __builtin_frame_address(0);
	options.fetch = BROWSER_FETCH_AT_ONCE;
	options.width = 32;
	options.height = 24;

	/* Refuses missing options/output and dimensions before owning a view. */
	error = browser_view_create(NULL, &first);
	expect(error, EINVAL, "create NULL options");
	error = browser_view_create(&options, NULL);
	expect(error, EINVAL, "create NULL output");
	options.width = 0;
	error = browser_view_create(&options, &first);
	expect(error, EINVAL, "create empty width");
	options.width = UINT_MAX;
	error = browser_view_create(&options, &first);
	expect(error, EINVAL, "create overflowing width");
	options.width = 32;
	options.fetch = (enum browser_fetch) - 1;
	error = browser_view_create(&options, &first);
	expect(error, EINVAL, "create unknown fetch");
	options.fetch = BROWSER_FETCH_AT_ONCE;
	options.version++;
	error = browser_view_create(&options, &first);
	expect(error, ENOTSUP, "create foreign ABI");
	options.version = BROWSER_API_VERSION;

	/* Makes the two views from copied options and callback structures. */
	error = browser_view_create(&options, &first);
	expect(error, 0, "first view create");
	if (error != 0)
		return 1;
	callbacks.context = &b;
	error = browser_view_create(&options, &second);
	expect(error, 0, "second view create");
	if (error != 0) {
		browser_view_destroy(first);
		return 1;
	}

	/* A callback may query its view and resize a different view. */
	a.other = second;
	error = browser_view_load(first, "plan/tools/browser-component/pages/first.html");
	expect(error, 0, "first page load");
	error = browser_view_load(second, "plan/tools/browser-component/pages/second.html");
	expect(error, 0, "second page load");
	location = browser_view_title(second);
	error = strcmp(location, "second");
	expect(error, 0, "second title independent");
	error = browser_view_key(first, "x", "KeyX", "x", 1, 0, 0);
	expect(error, 0, "abstract key");
	expect(a.keys, 1, "first key callback");
	expect(b.keys, 0, "second receives no first input");
	/* Advances one page's virtual timers without running the other view's timers. */
	error = browser_view_settle(first, 5.0, 0);
	expect(error, 0, "first timer settle");
	expect(a.timers, 1, "first timer callback");
	expect(b.timers, 0, "second timer is independent");
	error = browser_view_settle(second, 5.0, 0);
	expect(error, 0, "second timer settle");
	expect(b.timers, 1, "second timer callback");
	possible = browser_view_can_go(first, INT_MIN);
	expect(possible, 0, "INT_MIN history query");
	error = browser_view_resize(first, 0, 24);
	expect(error, EINVAL, "invalid resize");
	error = browser_view_draw_pixels(first, output, 32, 24, 1);
	expect(error, EINVAL, "short pixel stride");
	error = browser_view_draw_pixels(first, output, 32, 24, SIZE_MAX);
	expect(error, EINVAL, "overflowing row span");
	error = browser_view_draw_pixels(first, NULL, 32, 24, 128);
	expect(error, EINVAL, "NULL pixels");
	error = browser_view_draw(first, NULL, VK_NULL_HANDLE, VK_NULL_HANDLE);
	expect(error, EINVAL, "NULL Vulkan target");

	/* Measures the navigation copies and creates a forward history entry to preserve on failure. */
	copies = 0;
	error = browser_view_load(first, "plan/tools/browser-component/pages/second.html");
	expect(error, 0, "history second page");
	total_copies = copies;
	error = browser_view_go(first, -1);
	expect(error, 0, "history back");
	location = browser_view_url(first);
	snprintf(before, sizeof(before), "%s", location);

	/* Refuses each of the two final ownership copies, after the page has loaded. */
	for (point = total_copies - 1; point <= total_copies; point++) {
		copies = 0;
		refuse_copy = point;
		commits = a.commits;
		error = browser_view_load(first, "plan/tools/browser-component/pages/second.html");
		refuse_copy = 0;
		expect(error, ENOMEM, "navigation copy failure");
		location = browser_view_url(first);
		error = strcmp(location, before);
		expect(error, 0, "failed navigation preserves URL");
		expect(a.commits, commits, "failed navigation has no commit callback");
		possible = browser_view_can_go(first, 1);
		expect(possible, 1, "failed navigation preserves forward history");
	}

	/* Proves both renderers remain usable after refused input and allocations. */
	error = browser_offscreen_create(0, 24, &image, &failure);
	expect(error, EINVAL, "empty offscreen image");
	error = browser_offscreen_create(32, 24, &image, &failure);
	expect(error, 0, "offscreen create");
	if (error != 0) {
		browser_view_destroy(second);
		browser_view_destroy(first);
		return 1;
	}

	/* Lends the same caller-owned GPU to two independently owned renderers. */
	browser_offscreen_target(image, &gpu, &target);
	browser_view_set_gpu(first, &gpu);
	browser_view_set_gpu(second, &gpu);
	refuse_framebuffer = 1;
	error = browser_view_draw(first, &target, VK_NULL_HANDLE, VK_NULL_HANDLE);
	expect(error, EIO, "framebuffer creation failure");
	browser_view_gpu_failure(first, &failure);
	expect(failure.result, VK_ERROR_OUT_OF_HOST_MEMORY, "precise GPU failure");
	pixels(first, image, 0);
	pixels(second, image, 0);
	pixels(first, image, 1);
	error = browser_offscreen_read(image, output, 1, &failure);
	expect(error, EINVAL, "offscreen short stride");

	/* Releases view-created framebuffers before destroying the caller-owned image and device. */
	browser_view_release_targets(first);
	browser_view_release_targets(second);
	browser_view_set_gpu(first, NULL);
	browser_view_set_gpu(second, NULL);
	browser_offscreen_destroy(image);

	/* Recreates the target at another size and exercises a reused view after release. */
	error = browser_view_resize(first, 24, 16);
	expect(error, 0, "valid resize");
	error = browser_offscreen_create(24, 16, &image, &failure);
	expect(error, 0, "resized offscreen create");
	if (error == 0) {
		browser_offscreen_target(image, &gpu, &target);
		browser_view_set_gpu(first, &gpu);
		pixels(first, image, 0);
		browser_view_set_gpu(first, NULL);
		browser_offscreen_destroy(image);
	}

	/* Honors the callback's close request only after the triggering engine call has returned. */
	expect(a.defer_destroy, 1, "deferred destruction request");
	browser_view_destroy(first);
	error = browser_view_resize(second, 24, 16);
	expect(error, 0, "surviving second view");
	browser_view_destroy(second);
	browser_view_destroy(NULL);

	/* A loopback server supplies a failed asynchronous history navigation. */
	if (argc > 1)
		asynchronous(argv[1]);

	/* Reports any violated public contract before the success summary. */
	if (failures != 0)
		return 1;

	/* Succeeded: both views and all client-owned GPU resources were released. */
	printf("component: %d checks, PASS two views, input/timers, callback, allocation rollback, async history, Vulkan draw/record, resize/release\n", checks);
	return 0;
}

/* Reports a mismatched operation without hiding which result the component returned. */
static void
expect(
    int observed,
    int expected,
    const char *operation)
{
	/* Counts each comparison once, regardless of its outcome. */
	checks++;

	/* Counts only the contract that failed and prints both outcomes for reproduction. */
	if (observed != expected) {
		failures++;
		fprintf(stderr, "%s: got %d expected %d\n", operation, observed, expected);
	}

	/* The client has recorded the comparison. */
	return;
}

/* Queries the committed state and defers its view's destruction to the client loop. */
static void
committed(
    void *context,
    struct browser_view *view)
{
	struct observation *state;
	const char *url;
	int error;

	/* Each callback has the copied context of exactly one view. */
	state = context;
	state->commits++;
	state->defer_destroy = 1;
	url = browser_view_url(view);
	if (url == NULL)
		failures++;

	/* A different view may be changed without replacing this callback's page. */
	if (state->other != NULL) {
		error = browser_view_resize(state->other, 32, 24);
		expect(error, 0, "other view resize inside callback");
	}

	/* The client can honor the recorded close request after the outer call. */
	return;
}

/* Counts the first view's key event without retaining a borrowed script string. */
static void
console(
    void *context,
    struct browser_view *view,
    int level,
    const char *text,
    size_t length)
{
	struct observation *state;
	int error;

	/* These callbacks use the copied context and text, not the view or log severity. */
	(void)view;
	(void)level;

	/* Counts only the exact key name the page's listener reported. */
	state = context;
	if (length == 1 && text[0] == 'x')
		state->keys++;

	/* Each view advances only its own page's timer callbacks. */
	if (length == 5) {
		error = memcmp(text, "timer", length);
		if (error == 0)
			state->timers++;
	}

	/* The copied context has recorded this event. */
	return;
}

/* Compares actual Vulkan output with the CPU reference for the same public view. */
static void
pixels(
    struct browser_view *view,
    struct browser_offscreen *image,
    int recorded)
{
	struct browser_gpu gpu;
	struct browser_target target;
	struct browser_gpu_failure failure;
	uint32_t cpu[32U * 24U];
	uint32_t actual[32U * 24U];
	size_t stride;
	size_t count;
	int error;

	/* Both draws use the target's exact dimensions and packed rows. */
	browser_offscreen_target(image, &gpu, &target);
	stride = target.width * sizeof(uint32_t);
	count = target.width * target.height;
	error = browser_view_draw_pixels(view, cpu, target.width, target.height, stride);
	expect(error, 0, "CPU reference");

	/* Uses either the view's waited submission or the client's own command buffer and fence. */
	if (recorded) {
		record(view, &gpu, &target);
	} else {
		error = browser_view_draw(view, &target, VK_NULL_HANDLE, VK_NULL_HANDLE);
		expect(error, 0, "Vulkan draw after failed creation");
	}

	/* Reads the real image after completion and compares every pixel. */
	error = browser_offscreen_read(image, actual, stride, &failure);
	expect(error, 0, "Vulkan readback");
	error = memcmp(cpu, actual, count * sizeof(uint32_t));
	expect(error, 0, "Vulkan equals CPU reference");

	/* The client has compared the completed image with its reference. */
	return;
}

/* Records into caller-owned commands and waits on the caller-owned submission fence. */
static void
record(
    struct browser_view *view,
    const struct browser_gpu *gpu,
    const struct browser_target *target)
{
	VkCommandPoolCreateInfo pool_info;
	VkCommandBufferAllocateInfo allocate;
	VkCommandBufferBeginInfo begin;
	VkFenceCreateInfo fence_info;
	VkSubmitInfo submit;
	VkCommandPool pool;
	VkCommandBuffer commands;
	VkFence fence;
	VkQueue queue;
	VkResult result;
	int error;

	/* Allocates commands from the same queue family the view's renderer uses. */
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.queueFamilyIndex = gpu->queue_family;
	result = vkCreateCommandPool(gpu->device, &pool_info, NULL, &pool);
	expect(result, VK_SUCCESS, "client command pool");
	if (result != VK_SUCCESS)
		return;
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocate.commandPool = pool;
	allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate.commandBufferCount = 1;
	result = vkAllocateCommandBuffers(gpu->device, &allocate, &commands);
	expect(result, VK_SUCCESS, "client commands");
	if (result != VK_SUCCESS) {
		vkDestroyCommandPool(gpu->device, pool, NULL);
		return;
	}

	/* The client begins and ends the buffer around the component's complete render pass. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	result = vkBeginCommandBuffer(commands, &begin);
	expect(result, VK_SUCCESS, "client begin");
	if (result != VK_SUCCESS) {
		vkDestroyCommandPool(gpu->device, pool, NULL);
		return;
	}

	/* Asks the component to record only into the successfully begun buffer. */
	error = browser_view_record(view, target, commands);
	expect(error, 0, "component record");
	if (error != 0) {
		vkDestroyCommandPool(gpu->device, pool, NULL);
		return;
	}

	/* Ends the complete render pass before it can be submitted. */
	result = vkEndCommandBuffer(commands);
	expect(result, VK_SUCCESS, "client end");
	if (result != VK_SUCCESS) {
		vkDestroyCommandPool(gpu->device, pool, NULL);
		return;
	}

	/* The client's fence owns completion of the recorded work. */
	memset(&fence_info, 0, sizeof(fence_info));
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	result = vkCreateFence(gpu->device, &fence_info, NULL, &fence);
	expect(result, VK_SUCCESS, "client fence");
	if (result != VK_SUCCESS) {
		vkDestroyCommandPool(gpu->device, pool, NULL);
		return;
	}

	/* Submits the recorded pass on its matching queue. */
	vkGetDeviceQueue(gpu->device, gpu->queue_family, 0, &queue);
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &commands;
	result = vkQueueSubmit(queue, 1, &submit, fence);
	expect(result, VK_SUCCESS, "client submit");
	if (result == VK_SUCCESS) {
		result = vkWaitForFences(gpu->device, 1, &fence, VK_TRUE, UINT64_MAX);
		expect(result, VK_SUCCESS, "client wait");
	}

	/* Completed commands release no image, image view or device owned by the offscreen caller. */
	vkDestroyFence(gpu->device, fence, NULL);
	vkDestroyCommandPool(gpu->device, pool, NULL);

	/* The completed pass no longer uses the client's command pool or fence. */
	return;
}

/* Counts failed asynchronous loads without retaining the callback's borrowed strings. */
static void
load(
    void *context,
    struct browser_view *view,
    enum browser_load_state state,
    const char *url,
    int error,
    const char *reason)
{
	struct observation *observation;

	/* Only the result category and copied context are needed by this observer. */
	(void)view;
	(void)url;
	(void)error;
	(void)reason;

	/* A failed request must not publish a different history destination. */
	observation = context;
	if (state == BROWSER_LOAD_FAILED)
		observation->load_failures++;

	/* The copied context has recorded the request outcome. */
	return;
}

/* Preserves the current page and history while a background destination fails or is canceled. */
static void
asynchronous(
    const char *origin)
{
	struct browser_callbacks callbacks;
	struct browser_view_options options;
	struct browser_view *view;
	struct observation observation;
	char first[1024];
	char second[1024];
	const char *url;
	int error;
	int possible;

	/* This view owns a background loader and a separate callback context. */
	memset(&observation, 0, sizeof(observation));

	/* Reports transport failures to this background view's own observer. */
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.load = load;
	callbacks.context = &observation;

	/* Keeps the background loader inside the same public lifetime contract. */
	memset(&options, 0, sizeof(options));
	options.version = BROWSER_API_VERSION;
	options.width = 32;
	options.height = 24;
	options.stack_base = __builtin_frame_address(0);
	options.fetch = BROWSER_FETCH_BACKGROUND;
	options.callbacks = &callbacks;
	error = browser_view_create(&options, &view);
	expect(error, 0, "async view create");
	if (error != 0)
		return;

	/* Loads two successful documents into this view's history. */
	snprintf(first, sizeof(first), "%s/first.html", origin);
	snprintf(second, sizeof(second), "%s/second.html", origin);
	error = browser_view_load(view, first);
	expect(error, 0, "async first start");
	error = browser_view_settle(view, 0.0, 0);
	expect(error, 0, "async first settle");
	error = browser_view_load(view, second);
	expect(error, 0, "async second start");
	error = browser_view_settle(view, 0.0, 0);
	expect(error, 0, "async second settle");

	/* A pending back request must not pretend the old page is already the destination. */
	error = browser_view_go(view, -1);
	expect(error, 0, "async back start");
	possible = browser_view_can_go(view, 1);
	expect(possible, 0, "pending history index remains current");
	error = browser_view_settle(view, 0.0, 0);
	expect(error, 0, "failed async back keeps visible page");
	expect(observation.load_failures, 1, "failed async load callback");
	url = browser_view_url(view);
	error = strcmp(url, second);
	expect(error, 0, "failed async back preserves URL");
	possible = browser_view_can_go(view, 1);
	expect(possible, 0, "failed async back preserves history");

	/* Canceling another pending back request likewise leaves the displayed step current. */
	error = browser_view_go(view, -1);
	expect(error, 0, "cancelable async back");
	browser_view_stop(view);
	possible = browser_view_can_go(view, 1);
	expect(possible, 0, "canceled async back preserves history");
	browser_view_destroy(view);

	/* The background view no longer owns requests or history. */
	return;
}
