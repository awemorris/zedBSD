/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Vulkan executor of the nvrtx GPU device.
 *
 * libvulkan sends the zedBSD GPU command protocol (uapi/gpu-op.h) to a GPU
 * node; the executor decodes it in the kernel and does the work on the
 * GPU.  The wire's readers and writers and the generated record codec are
 * the i915 executor's, used unchanged (as the bcm2711 executor uses them);
 * everything else is the nvrtx driver's own.
 *
 * One executor session belongs to one open of the node.  It holds the
 * session's typed objects by their wire identity (an index of hashed
 * identities over a list), and the scratch one command decodes its records
 * into.  The node's lock serializes everything here.
 */

#ifndef DRIVERS_GPU_NVRTX_VK_H
#define DRIVERS_GPU_NVRTX_VK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/i915/vk/vk-codec.h"

/* Names a parameter a function does not use (as the driver's other files do). */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(parameter) ((void)(parameter))
#endif

/* The buckets of a session's identity index (a power of two). */
#define NVRTX_VK_OBJECT_BUCKETS		1024U

/* The most live objects one session holds. */
#define NVRTX_VK_OBJECT_LIMIT		4096U

/* The scratch one command decodes its records into. */
#define NVRTX_VK_ARENA_BYTES		(256U * 1024U)

struct nvrtx_vk_object;
struct nvrtx_vk_execute;

/*
 * One executor session.
 *
 * owner is the node's session the executor reads blobs through.  objects
 * lists every published identity, newest first, and buckets indexes them
 * by hashed identity; object_count counts the published ones, live_objects
 * the ones whose payload has not been destroyed yet (an unpublished object
 * lives on while something still holds it).  closing is set once the
 * session's close has begun: nothing is published after it.  last_opcode
 * and failed_opcode name the commands the decoder last took and refused.
 */
struct nvrtx_vk_session {
	void *owner;
	struct nvrtx_vk_object *objects;
	struct nvrtx_vk_object *buckets[NVRTX_VK_OBJECT_BUCKETS];
	uint32_t object_count;
	uint32_t live_objects;
	bool closing;
	uint32_t last_opcode;
	uint32_t failed_opcode;
	struct i915_wire_arena arena;
	/* The timelines the session's queues complete jobs on (bit n: timeline n), set by vkGetDeviceQueue2. */
	uint64_t timelines;
	/* The run its submissions are written into and the executor's programs (vk-execute.c), made at the first one. */
	struct nvrtx_vk_execute *execute;
};

/*
 * One published wire identity of a kind, and the payload it stands for.
 *
 * next and link chain it into the session's list (link is the pointer that
 * points at it, so it unlinks at once), bucket_next into its bucket.
 * references counts the holders: the registry while published, and every
 * dependent object or submitted work that retained it; the payload is
 * destroyed when the count reaches zero.
 */
struct nvrtx_vk_object {
	struct nvrtx_vk_object *next;
	struct nvrtx_vk_object **link;
	struct nvrtx_vk_object *bucket_next;
	struct nvrtx_vk_session *session;
	enum i915_vk_object_kind kind;
	uint64_t identity;
	uint32_t references;
	bool published;
	void *payload;
	int (*destroy)(struct nvrtx_vk_session *session, void *payload);
};

/* A command's handler: takes the command's body from reader and writes its reply; sets *handled when the opcode is its own. */
typedef int (*nvrtx_vk_route)(struct nvrtx_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);

/* Runs one decoded command (the transport's opcodes excepted). */
typedef int (*nvrtx_vk_dispatcher)(struct nvrtx_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);

int nvrtx_vk_session_open(void *owner, struct nvrtx_vk_session **session);
int nvrtx_vk_session_close(struct nvrtx_vk_session **session);
void nvrtx_vk_execute_close(struct nvrtx_vk_session *session);
void *nvrtx_vk_arena_take(struct nvrtx_vk_session *session, size_t bytes);

int nvrtx_vk_object_publish(struct nvrtx_vk_session *session, enum i915_vk_object_kind kind, uint64_t identity, void *payload, int (*destroy)(struct nvrtx_vk_session *, void *), struct nvrtx_vk_object **object);
struct nvrtx_vk_object *nvrtx_vk_object_find(struct nvrtx_vk_session *session, enum i915_vk_object_kind kind, uint64_t identity);
int nvrtx_vk_object_retain(struct nvrtx_vk_object *object);
int nvrtx_vk_object_release(struct nvrtx_vk_object *object);
int nvrtx_vk_object_remove(struct nvrtx_vk_session *session, enum i915_vk_object_kind kind, uint64_t identity);
int nvrtx_vk_objects_close(struct nvrtx_vk_session *session);

int nvrtx_vk_device_route(struct nvrtx_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);

int nvrtx_vk_stream_execute(struct nvrtx_vk_session *session, const void *wire, uint32_t bytes, nvrtx_vk_dispatcher dispatch);
int nvrtx_vk_dispatch(struct nvrtx_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);

/*
 * Attaches the storage of a blob created for a VkDeviceMemory (the blob's
 * blob_id is the memory's identity): the CPU's view, the GPU's address and
 * the node's backing, which the memory holds until it is destroyed (vk-memory.c).
 */
int nvrtx_vk_memory_attach(struct nvrtx_vk_session *session, uint64_t identity, uint64_t bytes, uint8_t *cpu, uint64_t va, void *backing);

/*
 * Finds a blob of the owner's (the node's session) by its resource id and
 * gives its CPU view: the first byte and the length.  Provided by the
 * node (node.c); the transport reads command streams from blobs and writes
 * replies into them.
 */
int nvrtx_vk_blob(void *owner, uint32_t resource_id, uint8_t **cpu, uint64_t *bytes);

/* The owner's GPU: its PCI product, and the bytes of VRAM the CPU's window gives the executor's memory. */
uint32_t nvrtx_vk_device_id(void *owner);
uint64_t nvrtx_vk_heap_bytes(void *owner);

/* The owner's GPU generation: the shader model its programs are compiled for (75, 86 or 89), and its 3D class. */
uint32_t nvrtx_vk_shader_model(void *owner);
uint32_t nvrtx_vk_threed_class(void *owner);

/*
 * Gives back the storage of a VkDeviceMemory once nothing holds it: the
 * GPU's mapping of the run at va and the reference to the blob's backing
 * the memory took when the blob was attached, once the session's runs have
 * ended.  Provided by the node; the caller holds the node's lock.
 */
void nvrtx_vk_memory_detach(void *owner, void *backing, uint64_t va, uint64_t bytes);

/*
 * Gives the pages of a part of a memory's storage (bytes from offset, at
 * va for the GPU, whole pages) a memory kind (NVRTX_MMU_KIND_*), as a depth
 * target with a stencil needs.  Provided by the node; the caller holds the
 * node's lock and makes sure no work uses the part meanwhile.
 */
int nvrtx_vk_memory_kind(void *owner, void *backing, uint64_t offset, uint64_t va, uint64_t bytes, uint32_t kind);

/*
 * Notes an image bound to a memory's storage: its shape (block linear or
 * not, its row pitch, size and offset in the storage).  The first image
 * bound gives the storage the shape an export of it must describe; once
 * the storage was exported as an image, every image bound must have the
 * described shape (EINVAL otherwise).  Provided by the node; the caller
 * holds the node's lock.
 */
int nvrtx_vk_memory_bind_image(void *backing, bool tiled, uint32_t pitch, uint32_t width, uint32_t height, uint64_t offset);

/*
 * Takes a blob of the owner's as the storage of an imported VkDeviceMemory
 * of bytes (provided by the node): maps it for the GPU, holds its backing,
 * and gives the CPU's view, the GPU's address and the backing, which
 * nvrtx_vk_memory_detach() gives back.  The caller holds the node's lock.
 */
int nvrtx_vk_memory_import(void *owner, uint32_t resource_id, uint64_t bytes, uint8_t **cpu, uint64_t *va, void **backing);

/*
 * Gives the CPU's view of a memory's storage (a backing nvrtx_vk_memory_attach()
 * or nvrtx_vk_memory_import() gave), mapping it into the window when it has no
 * place there yet.  The caller holds the node's lock.
 */
int nvrtx_vk_memory_cpu(void *owner, void *backing, uint8_t **cpu);

/*
 * The executor's own GPU memory (programs, push buffers, texture and
 * sampler tables), provided by the node: a run of bytes the CPU writes at
 * *cpu and the GPU reads at *va (in VRAM through the window, or with
 * nvrtx_vk_gpu_alloc_system() in coherent system memory), and its release once the session's runs
 * have ended.  The token names the run to the node.
 */
int nvrtx_vk_gpu_alloc(void *owner, uint64_t bytes, void **token, uint8_t **cpu, uint64_t *va);
int nvrtx_vk_gpu_alloc_system(void *owner, uint64_t bytes, void **token, uint8_t **cpu, uint64_t *va);
void nvrtx_vk_gpu_free(void *owner, void *token);

/*
 * A run the executor put on the GPU: the work channel's fence after it (0:
 * no run) and when it was submitted, in microseconds.
 */
struct nvrtx_vk_ticket {
	uint32_t fence;
	uint64_t submitted_us;
};

/*
 * The executor's runs, provided by the node (the caller holds the node's
 * lock).  nvrtx_vk_gpu_submit() puts a push buffer of bytes at va on the
 * GPU's work channel without waiting for it and names the run in *ticket;
 * the session's newest run is the one nvrtx_vk_gpu_last() gives and
 * nvrtx_vk_gpu_idle() waits for.  nvrtx_vk_gpu_wait() waits for a run, up
 * to its time from the submission; nvrtx_vk_gpu_passed() tells whether it
 * ended; nvrtx_vk_gpu_retire() counts an ended run's GPU time once and
 * forgets it.  The submission and the waits return EIO once a run of the
 * session did not end in time (its runs are lost).
 */
int nvrtx_vk_gpu_submit(void *owner, uint64_t va, uint32_t bytes, struct nvrtx_vk_ticket *ticket);
int nvrtx_vk_gpu_wait(void *owner, const struct nvrtx_vk_ticket *ticket);
bool nvrtx_vk_gpu_passed(void *owner, const struct nvrtx_vk_ticket *ticket);
void nvrtx_vk_gpu_retire(void *owner, struct nvrtx_vk_ticket *ticket);
void nvrtx_vk_gpu_last(void *owner, struct nvrtx_vk_ticket *ticket);
int nvrtx_vk_gpu_idle(void *owner);

/*
 * Tells whether the 3D state a run of the owner's leaves on its channel is
 * still there for its next run (no other work draws on the channel), so
 * the class's initial state need not be written again (provided by the
 * node).
 */
bool nvrtx_vk_gpu_state_kept(void *owner);

/*
 * Gives a token for a memory's backing (as nvrtx_vk_memory_attach() or
 * nvrtx_vk_memory_import() gave it) while only the owner's own runs may
 * have written it lately: equal nonzero tokens at two times mean nothing
 * else (the CPU, another open, another engine, a stop or reset) wrote it
 * in between; 0 means it is not known (provided by the node).
 */
uint64_t nvrtx_vk_gpu_quiet(void *owner, void *backing);

/*
 * Counts a transfer the CPU did through the window instead of the GPU, for
 * the node's counts (provided by the node).
 */
void nvrtx_vk_gpu_copied(void *owner, uint64_t bytes);

/*
 * Counts the draws of a push buffer about to run: the application's draws
 * and the executor's own quads, for the node's counts (provided by the
 * node).
 */
void nvrtx_vk_gpu_drawn(void *owner, uint32_t draws, uint32_t quads);

/*
 * The GPU's video decoder, provided by the node (the caller holds the
 * node's lock).  nvrtx_vk_video_available() tells whether the device has
 * a decoder that takes pictures and the copy engine its pictures are read
 * out with; nvrtx_vk_video_decode() runs a decoder push buffer of bytes
 * at va (in the executor's memory) and waits for it, returning EIO when
 * the decoder is not there or did not finish.
 */
bool nvrtx_vk_video_available(void *owner);
int nvrtx_vk_video_decode(void *owner, uint64_t va, uint32_t bytes);

/*
 * Copies a command stream of length bytes out of a blob's bytes in the
 * CPU's window, counting the copy and its time (provided by the node).
 */
void nvrtx_vk_blob_read(void *owner, uint8_t *to, const uint8_t *from, size_t length);

#endif
