/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libyolox: YOLOX-Nano object detection with Vulkan compute or on the CPU.
 *
 * A model is a ZNET file read once; it does not change and may be shared
 * between threads.  A runner executes the model on one backend at one
 * input size and is used by one thread.  Every function returns 0 or a
 * negative YOLOX_ERROR_* code that yolox_reason() names.
 */

#ifndef LIBYOLOX_YOLOX_H
#define LIBYOLOX_YOLOX_H

#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The error codes. */
#define YOLOX_ERROR_ARGUMENT		(-1)
#define YOLOX_ERROR_MEMORY		(-2)
#define YOLOX_ERROR_IO			(-3)
#define YOLOX_ERROR_FORMAT		(-4)
#define YOLOX_ERROR_UNSUPPORTED		(-5)
#define YOLOX_ERROR_BUSY		(-6)
#define YOLOX_ERROR_TIMEOUT		(-7)
#define YOLOX_ERROR_NOT_SHARED		(-8)
#define YOLOX_ERROR_DEVICE_LOST		(-9)
#define YOLOX_ERROR_FUSED		(-10)
#define YOLOX_ERROR_VULKAN		(-11)
#define YOLOX_ERROR_NOT_DEBUG		(-12)
#define YOLOX_ERROR_IDLE		(-13)

/* A runner that keeps every tensor for yolox_runner_read_tensor(); for tests. */
#define YOLOX_RUNNER_DEBUG		0x1U

/* A read ZNET model; immutable and shareable between threads. */
struct yolox_model;

/* One backend at one input size; used by one thread. */
struct yolox_runner;

/* The backend a runner executes on. */
enum yolox_backend {
	YOLOX_BACKEND_AUTO = 0,
	YOLOX_BACKEND_VULKAN = 1,
	YOLOX_BACKEND_CPU = 2
};

/* The pixel layout of an input image. */
enum yolox_pixel_format {
	YOLOX_PIXEL_RGB8 = 0,
	YOLOX_PIXEL_RGBA8 = 1,
	YOLOX_PIXEL_BGRA8 = 2
};

/*
 * An input image: its layout, its size (1 to 16384 each way) and the
 * bytes from one row to the next (at least width times the pixel size).
 */
struct yolox_image {
	enum yolox_pixel_format format;
	uint32_t width;
	uint32_t height;
	uint32_t row_bytes;
};

/*
 * One detection, in the input image's coordinates (not clipped).  score
 * is the objectness times the class probability.
 */
struct yolox_detection {
	float x1;
	float y1;
	float x2;
	float y2;
	float score;
	uint32_t class_index;
};

/*
 * How a runner is made.
 *
 * struct_size is sizeof(struct yolox_runner_config) as the caller knows
 * it; the library reads no more than that and takes defaults for the
 * rest.  yolox_runner_config_default() fills a complete one.  The Vulkan
 * fields share the application's device; with device NULL the library
 * makes its own.
 */
struct yolox_runner_config {
	uint32_t struct_size;
	uint32_t flags;
	enum yolox_backend backend;
	uint32_t input_size;
	float score_threshold;
	float nms_threshold;
	int class_aware;
	uint32_t max_dispatch_macs;
	PFN_vkGetInstanceProcAddr get_instance_proc_addr;
	VkInstance instance;
	VkPhysicalDevice physical_device;
	VkDevice device;
	uint32_t queue_family;
	VkQueue queue;
	void (*queue_lock)(void *context);
	void (*queue_unlock)(void *context);
	void *queue_context;
};

/* Reads and checks a ZNET file. */
int yolox_model_load(const char *path, struct yolox_model **model);

/* Frees a model; its runners must be freed first. */
void yolox_model_free(struct yolox_model *model);

/* Names a COCO class, or NULL past the last. */
const char *yolox_class_name(uint32_t class_index);

/* Names an error code. */
const char *yolox_reason(int code);

/* Fills a configuration with its size and the defaults. */
void yolox_runner_config_default(struct yolox_runner_config *config);

/* Makes a runner of a model. */
int yolox_runner_create(const struct yolox_model *model, const struct yolox_runner_config *config, struct yolox_runner **runner);

/* Reports the backend a runner chose. */
enum yolox_backend yolox_runner_backend(const struct yolox_runner *runner);

/* Frees a runner. */
void yolox_runner_free(struct yolox_runner *runner);

/*
 * Detects objects in a host image.  count receives the number of
 * detections after NMS; the first min(count, capacity) are written,
 * highest score first.
 */
int yolox_detect(struct yolox_runner *runner, const struct yolox_image *image, const void *pixels, struct yolox_detection *detections, uint32_t capacity, uint32_t *count);

/*
 * Submits an image in the shared device's buffer (a Vulkan runner on the
 * application's device only), waiting on wait first when it is not
 * VK_NULL_HANDLE.
 */
int yolox_submit_buffer(struct yolox_runner *runner, const struct yolox_image *image, VkBuffer buffer, VkDeviceSize offset, VkSemaphore wait);

/* Collects the detections of the submitted image. */
int yolox_collect(struct yolox_runner *runner, uint64_t timeout_ns, struct yolox_detection *detections, uint32_t capacity, uint32_t *count);

/*
 * Copies a tensor of the last execution, by its ZNET number, as NCHW
 * float32 (a debug runner only).  count is the tensor's element count.
 */
int yolox_runner_read_tensor(struct yolox_runner *runner, uint32_t tensor, float *values, size_t count);

#ifdef __cplusplus
}
#endif

#endif
