/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerVR render node's own state, shared by its operations, its
 * queue and its recovery.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_RENDER_PRIVATE_H
#define KERN_DRIVERS_GPU_POWERVR_RENDER_PRIVATE_H

#include <stdint.h>

#include <drivers/gpu/gpu.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <kern/waitq.h>

#include "drivers/gpu/powervr/powervr-private.h"
#include "drivers/gpu/powervr/fixed-area.h"
#include "drivers/gpu/powervr/gpu-mmu.h"
#include "drivers/gpu/powervr/render.h"
#include "drivers/gpu/powervr/render-target-layout.h"
#include "drivers/gpu/powervr/render-kick-layout.h"


/*
 * Whether a session writes its compute context's switch state (the
 * programs that save and restore the shared registers) and the compute
 * command its resume control word.  With the state zero, or the state
 * without the word, the compute data master locked up on the VF2 even on
 * a stream of its end alone; build with -DPOWERVR_COMPUTE_SWITCH_STATE=0
 * only to look into that again.
 */
#ifndef POWERVR_COMPUTE_SWITCH_STATE
#define POWERVR_COMPUTE_SWITCH_STATE	1
#endif

/*
 * Whether a session writes its render context's static switch state
 * (the VDM and TA state buffers and their store and resume tasks).  Set
 * to 0 for now to see whether the geometry data master runs with the
 * state all zero, as the firmware should not read it without a resume.
 */
#ifndef POWERVR_RENDER_SWITCH_STATE
#define POWERVR_RENDER_SWITCH_STATE	1
#endif

/*
 * The most a session holds, the largest allocation, and how many native
 * commands one session may have in flight.
 */
#define POWERVR_RENDER_RESOURCES	256U
#define POWERVR_RENDER_MAX_BYTES	0x10000000ULL
#define POWERVR_RENDER_PENDING		64U

/*
 * One allocation: a physically contiguous run below the core's DMA limit
 * and its uncached kernel view, the only way the host touches it.
 *
 * References are counted under the render node's buffer guard: each
 * session resource, each export and each display-node lookup holds one.
 * Pages is the run's page list, made by the first image export and kept
 * with the buffer.  Next links a buffer kept after a fault.
 */
struct powervr_buffer {
	struct powervr_buffer *next;
	unsigned references;
	struct kern_pmem run;
	volatile uint8_t *view;
	uint64_t bytes;
	uint64_t *pages;
	uint64_t page_count;

	/* Mapped through the system level cache: its lines are written back and dropped before the pages are freed. */
	unsigned gpu_cached;
};

/*
 * One session's handle on a buffer, mapped at a device address of the
 * session's memory context.  A blob or an import also has the identifier
 * the renderer names it by, and is listed in its session's blobs through
 * next_blob.
 */
struct powervr_resource {
	struct powervr_session *session;
	struct powervr_buffer *buffer;
	uint64_t address;
	struct powervr_gpu_space *space;
	int mappable;
	uint32_t identifier;
	struct powervr_resource *next_blob;
	/* Diagnostics: every live handle of the session, under the core's lock, and who made it. */
	struct powervr_resource *next_all;
	uintptr_t origin;
	unsigned shareable;
	unsigned deferred;
};

/*
 * One export: the buffer it keeps alive, and the image it was exported
 * as, if any.  Live exports are listed under the render node's buffer
 * guard, so a lookup can find them.
 */
struct powervr_shared {
	struct powervr_shared *next;
	struct powervr_buffer *buffer;
	struct gpu_image_descriptor image;
	int has_image;
};

/*
 * One native command in flight: the timeline value that retires it, and
 * whom to tell with which error.  Marker is the job marker slot it frees
 * when it is delivered, or POWERVR_NO_MARKER.
 */
struct powervr_pending {
	unsigned kind;
	uint32_t value;
	uint64_t submitted;
	struct drv_gpu_completion *completion;
	int error;
	unsigned marker;
};

/* The catalogues powervr_target_catalogues reports: VCE 0 to 3, TE 0 to 3, ALIST. */
#define POWERVR_TARGET_CATALOGUES	9U

/* No job marker. */
#define POWERVR_NO_MARKER	0xffffffffU

/*
 * The timeline a completion point is on: none (already complete), the
 * session's compute context, its render context's 3D (fragment), or its
 * transfer context.
 */
#define POWERVR_POINT_NONE	0U
#define POWERVR_POINT_COMPUTE	1U
#define POWERVR_POINT_FRAGMENT	2U
#define POWERVR_POINT_TRANSFER	3U

/* The most finished transfers' memory a session keeps mapped for the next ones. */
#define POWERVR_TRANSFER_KEEP	4U

/*
 * How long a session's oldest pending callback may wait without its
 * point being reached before the driver reports the session lost
 * (ETIMEDOUT), well before the core's job deadline; the firmware's own
 * context reset usually comes first (about 2 s on the board).
 */
#define POWERVR_PROGRESS_MS	5000U

/* A render job whose 3D runs, kept until its point is reached (render-job.c). */
struct powervr_job_done;

/*
 * A render job's memory (render-job.c), and how many a session keeps
 * mapped for its next jobs: every map and unmap of a
 * buffer waits for the firmware to flush the core's MMU cache, six of each
 * a job, so the memory of finished jobs is reused instead.
 */
struct job_memory;
struct transfer_memory;
#define POWERVR_JOB_KEEP	4U

/*
 * One job marker of a session: a reservation libvulkan makes before it
 * submits a supervised job.  Free, reserved (held by the core, counted
 * against the ring), or committed (in the ring until delivered); the
 * completion is the one the reservation was made with.
 */
#define POWERVR_MARKER_FREE		0U
#define POWERVR_MARKER_RESERVED		1U
#define POWERVR_MARKER_COMMITTED	2U

struct powervr_marker {
	unsigned state;
	struct drv_gpu_completion *completion;
};

struct powervr_vk_session;
struct powervr_free_list;
struct powervr_targets;
struct powervr_target;

/*
 * The program windows of a session beyond the general one: the data
 * sequencer's, the shading cluster's and the transfer queue's fragment
 * streams.
 */
enum powervr_program_heap {
	POWERVR_PROGRAM_PDS,
	POWERVR_PROGRAM_USC,
	POWERVR_PROGRAM_TRANSFER
};

/*
 * Where the session's own programs start in the data sequencer's and the
 * shading cluster's windows: past the firmware's static programs, at the
 * place the Vulkan executor's programs have, which the first range of
 * each window holds for them.
 */
#define POWERVR_PROGRAM_HEAP_START	0x10000U
#define POWERVR_EXECUTOR_PDS_BYTES	0x4000U
#define POWERVR_EXECUTOR_USC_BYTES	0x20000U

/*
 * What the firmware reported of the render targets' free lists since the
 * node started: the grow requests it made (each answered with nothing
 * added), the rebuilds after a recovery, and the partial renders and
 * out-of-memory events its statistics counted.  A render compares them
 * before and after it runs to learn whether its free lists ran out.
 */
struct powervr_target_events {
	unsigned grow_requests;
	unsigned rebuilds;
	unsigned partial_renders;
	unsigned out_of_memory;
};

/*
 * One command buffer of a firmware context: its control block, the
 * buffer, the timeline word the firmware advances after the context's
 * jobs, all blocks of the firmware's heap; write is the host's write
 * offset into the buffer and submitted the last timeline value a job
 * was given.  All of it changes under the core's lock.
 */
struct powervr_client {
	struct powervr_object control;
	struct powervr_object buffer;
	struct powervr_object timeline;
	uint32_t write;
	uint32_t submitted;
};

/*
 * A session's render context: the firmware's record of it (the geometry
 * and fragment contexts' common parts and the static state its switch
 * uses), each context's own state block, command buffer and timeline,
 * all in the firmware's heap; the memory the switch uses in the
 * session's windows (the VDM call stack and the VDM and TA state in the
 * general one, each buffer's shared register and persistent temporary
 * state and its data sequencer programs in the data sequencer's, the
 * shading cluster programs in the cluster's), the commands of the render
 * being kicked, the background a fragment takes after a partial render
 * when its command names none (a stand-in program that only ends), the
 * identity the firmware names it by, whether it was kicked, and which of
 * its contexts the firmware cleaned up.  Everything
 * changes under the core's lock once the context is made.
 */
struct powervr_render_context {
	struct powervr_object record;
	struct powervr_object geometry_state;
	struct powervr_object fragment_state;
	struct powervr_client geometry;
	struct powervr_client fragment;
	struct powervr_resource *switch_memory;
	struct powervr_buffer *programs[2];
	uint64_t program_places[2];
	struct powervr_buffer *shaders;
	uint64_t shader_place;
	struct powervr_render_sequence sequence;
	uint64_t background[POWERVR_BACKGROUND_WORDS];
	uint32_t identity;
	unsigned kicked;
	unsigned geometry_cleaned;
	unsigned fragment_cleaned;
};

/*
 * A session's transfer context: the firmware's record of it (one common
 * context), its state block, its command buffer and timeline, the
 * identity the firmware names it by, whether it was kicked and whether
 * the firmware cleaned it up.  It changes under the core's lock once
 * made.
 */
struct powervr_transfer_context {
	struct powervr_object record;
	struct powervr_object state;
	struct powervr_client client;
	uint32_t identity;
	unsigned kicked;
	unsigned cleaned;
};

/*
 * Diagnostics: a compact note of the session's newest render job,
 * printed when a context reset names the session: the area, the colour
 * targets, and for each draw its pipeline, scissor and textures (the
 * image state words, and the sampler's first word).
 */
#define POWERVR_NOTE_COLORS	2U
#define POWERVR_NOTE_DRAWS	12U
#define POWERVR_NOTE_TEXTURES	4U

struct powervr_note_texture {
	uint64_t image[2];
	uint64_t sampler;
	uint32_t set;
	uint32_t binding;
};

struct powervr_note_draw {
	const void *pipeline;
	int32_t scissor_x;
	int32_t scissor_y;
	uint32_t scissor_width;
	uint32_t scissor_height;
	uint32_t texture_count;
	struct powervr_note_texture textures[POWERVR_NOTE_TEXTURES];
};

struct powervr_note_color {
	uint64_t address;
	uint32_t vk_format;
	uint32_t width;
	uint32_t height;
	uint32_t row_pitch;
	uint32_t padded_width;
	uint32_t linear;
	uint32_t load_op;
};

struct powervr_job_note {
	uint64_t serial;
	uint32_t area[4];
	uint32_t width;
	uint32_t height;
	uint32_t color_count;
	struct powervr_note_color colors[POWERVR_NOTE_COLORS];
	uint32_t draw_count;
	uint32_t prefix;
	struct powervr_note_draw draws[POWERVR_NOTE_DRAWS];
};

/*
 * One open of the render node.
 *
 * Mmu and space are the session's memory context, and fixed holds the
 * pages backing the context's fixed area at the start of the general,
 * data sequencer and shading cluster heaps (fixed-area.h), mapped while
 * the tables exist and returned with them; space hands out the general
 * heap after that area.  The firmware records
 * (its memory context, its compute context and state, and the compute
 * context's command buffer) are blocks of the firmware's heap, changed
 * under the core's lock.  The render context (its geometry and fragment
 * contexts) is made by the first render and changes under the core's
 * lock too.  Stop_context says which of the session's contexts the
 * cleanup in flight names.  The pending ring, its count of callbacks not yet delivered,
 * the job markers and their reserved count, and the stop flags are under
 * the render node's queue lock.  Blobs lists the session's blobs and
 * imports by identifier, and vk is the Vulkan executor's session (made
 * by the first command frame); both change only in the session's own
 * ioctls, which the GPU core serializes; so does deferred, the handles
 * destroyed while work was in flight, still mapped (render.c
 * resource_unmake).  So do targets, the render
 * targets and free lists made by the first render, and abandoned, set
 * once a submission ran out of time while the core may still run it.
 * The program windows' spaces change under the core's lock.
 */
struct powervr_session {
	struct powervr_session *next;
	struct powervr_render *render;
	struct powervr_gpu_mmu mmu;
	struct powervr_gpu_space space;
	struct powervr_gpu_space pds_space;
	struct powervr_gpu_space usc_space;
	struct powervr_gpu_space transfer_space;
	struct powervr_gpu_space region_space;
	struct powervr_buffer *fixed[POWERVR_FIXED_HEAPS];
	struct powervr_object memory_context;
	struct powervr_object context;
	struct powervr_object context_state;
	struct powervr_client compute;
	uint32_t identity;
	uint32_t submitted;
	uint32_t next_resource;
	unsigned resources;
	struct powervr_pending pending[POWERVR_RENDER_PENDING];
	unsigned pending_head;
	unsigned pending_count;
	unsigned outstanding;
	unsigned kicked;
	unsigned cleaned;
	unsigned stopping;
	unsigned stop_sent;
	unsigned stop_context;
	uint32_t stop_slot;
	uint64_t stop_since;
	unsigned stop_tries;
	unsigned lost;
	unsigned isolated;
	unsigned retained;
	struct powervr_resource *blobs;
	struct powervr_resource *deferred;
	struct powervr_resource *all;
	struct powervr_job_note note;
	struct powervr_resource *border_table;
	struct powervr_marker markers[POWERVR_RENDER_PENDING];
	unsigned reserved;
	struct powervr_vk_session *vk;
	struct powervr_targets *targets;
	struct powervr_render_context *render_context;
	struct powervr_transfer_context *transfer_context;
	struct powervr_buffer *compute_switch_pds;
	struct powervr_buffer *compute_switch_usc;
	uint64_t compute_switch_pds_place;
	uint64_t compute_switch_usc_place;
	unsigned abandoned;

	/*
	 * The last work kicked: its point (kind and value).  Callbacks and
	 * fences complete at it.  Changed only by the session's own
	 * submissions.
	 */
	unsigned last_kind;
	uint32_t last_value;

	/*
	 * The render jobs whose 3D may still run, oldest first, under the
	 * queue lock; their memory is released once their points are reached.
	 */
	struct powervr_job_done *jobs_done;
	struct powervr_job_done *jobs_done_tail;

	/*
	 * The submission timing's marks of the session's render jobs (only
	 * with the timing on): when the last reaped job's TA and 3D were seen
	 * done, and the 3D values and ends of the last two reaped jobs, one of
	 * which a later job's geometry waited for.  Under the queue lock.
	 */
	uint64_t timing_ta_done;
	uint64_t timing_fragment_done;
	uint32_t timing_fragment_values[2];
	uint64_t timing_fragment_ends[2];
	unsigned timing_fragment_next;

	/*
	 * The memory of finished render jobs kept mapped for the next ones
	 * (render-job.c), job_keep_count of them, under the queue lock; freed
	 * with the session (powervr_render_jobs_forget).
	 */
	struct job_memory *job_keep[POWERVR_JOB_KEEP];
	unsigned job_keep_count;

	/*
	 * The memory of finished transfers kept mapped for the next ones
	 * (transfer-job.c), transfer_keep_count of them, and the memory of
	 * transfers the core may still run, oldest first, each until its
	 * transfer's timeline value is reached; under the queue lock, freed
	 * with the session (powervr_transfer_forget).
	 */
	struct transfer_memory *transfer_keep[POWERVR_TRANSFER_KEEP];
	unsigned transfer_keep_count;
	struct transfer_memory *transfers_done;
	struct transfer_memory *transfers_done_tail;

	/*
	 * The render area's pixels of the job being kicked, which the job
	 * trace (render-queue.c, POWERVR_JOB_TRACE) sorts the jobs by.
	 */
	uint64_t trace_pixels;

	/* Pending callbacks that carry an error (a command may not overtake them), under the queue lock. */
	unsigned pending_errors;

	/* Set once the progress watchdog reported the session (under the queue lock). */
	unsigned progress_reported;

	/* The compute, 3D and transfer timeline values the service saw last, to tell the device's progress. */
	uint32_t seen_compute;
	uint32_t seen_fragment;
	uint32_t seen_transfer;

	/*
	 * The target the session's newest render job kicked with, for the
	 * description after a reset; targets live until the session closes.
	 */
	struct powervr_target *noted_target;

	/*
	 * The parameter manager's catalogues in the noted target's first
	 * half before the kick (VCE 0..3, TE 0..3, ALIST), to set beside
	 * those at a reset.
	 */
	uint64_t noted_catalogues[POWERVR_TARGET_CATALOGUES];
	unsigned noted_half;
};

/*
 * The shading cluster programs every render context's switch uses: the
 * shared register store and load, built once when the node starts, and
 * whether they were.
 */
struct powervr_switch_shaders {
	uint8_t store[512];
	uint8_t load[512];
	size_t store_bytes;
	size_t load_bytes;
	unsigned built;
};

/*
 * The render node.
 *
 * Sessions are listed under the core's lock.  Queue_lock guards every
 * session's pending ring and stop flags and the faulted flag, and is
 * taken from the interrupt handler, so it is held only briefly with
 * interrupts off; service wakes the service thread, retired wakes a
 * drain, and reached wakes a wait for a timeline value (the firmware's
 * interrupt and each pass of the service wake it).  Buffer_guard guards buffer references and the export list,
 * and the pool of freed buffers kept with their uncached views for reuse
 * (pool, newest first, pool_bytes in all).
 * Kept holds buffers and sessions that may still be reached by the core
 * after a fault or an isolation, until a reset frees them.  Lost, under
 * the core's lock, says a restart failed and the firmware is gone.
 * Table_memory is where sessions' page tables come from.  Running,
 * service_done, timing_done and spare_done, under the queue lock, tell
 * the service thread, the timing's thread and the spare's thread to end
 * and say they have.  Spare, under the queue lock, is a zeroed buffer
 * of a global free list's pages that the spare's thread makes ahead
 * (spare_wanted wakes it when the spare is taken).  Free_lists lists every session's free lists by the
 * identity the firmware names them by, next_free_list is the identity
 * the next one takes, and target_events counts what the firmware
 * reported of them; all three are under the core's lock.  Load_draws
 * keeps the load draw's pipelines (render-job.c), made on first use
 * under the core's lock.
 */
struct powervr_load_draws;
struct powervr_render {
	struct powervr_core *core;
	struct drv_gpu_device *device;
	struct drv_gpu_ops ops;
	struct powervr_gpu_memory table_memory;
	struct spinlock queue_lock;
	struct wait_queue service;
	struct wait_queue retired;
	struct wait_queue reached;
	struct wait_queue spare_wanted;
	struct spinlock buffer_guard;
	struct powervr_shared *exports;
	struct powervr_session *sessions;
	struct powervr_buffer *kept_buffers;
	struct powervr_buffer *pool;
	uint64_t pool_bytes;

	/* When the service last saw any session's timeline move (the watchdog's measure of progress). */
	uint64_t progress_ticks;

	/* Cached buffers whose last holder let go, under the queue lock; the service flushes the cache, then frees them. */
	struct powervr_buffer *slc_pending;
	struct powervr_session *kept_sessions;
	struct powervr_free_list *free_lists;
	struct powervr_target_events target_events;
	struct powervr_switch_shaders switch_shaders;
	struct powervr_load_draws *load_draws;
	struct powervr_buffer *spare;
	uint32_t next_free_list;
	unsigned faulted;
	unsigned lost;
	unsigned running;
	unsigned service_done;
	unsigned timing_done;
	unsigned spare_done;
	uint32_t next_identity;

	/*
	 * The busy time hw.gputelemetry reports, under the queue lock: the
	 * callbacks pending over every session (each from its kick until its
	 * delivery), when that count last rose from zero, and the nanoseconds
	 * it was above zero before then.  The time only grows.
	 */
	unsigned busy_pending;
	uint64_t busy_since;
	uint64_t busy_total;
};

/* Reads the fine monotonic counter in nanoseconds for the busy time; zero when there is none (render-queue.c). */
uint64_t powervr_busy_clock(void);

/* The bytes of the spare: a global free list's pages. */
#define POWERVR_SPARE_BYTES		((uint64_t)POWERVR_FREE_LIST_GLOBAL_PAGES * POWERVR_PM_PAGE_BYTES)

/* Takes the zeroed spare of a global free list's pages and has another made; ENOENT when none is ready (render-queue.c). */
int powervr_spare_take(struct powervr_render *render, struct powervr_buffer **result);

/* Makes a zeroed buffer of at least bytes bytes. */
int powervr_buffer_create(struct powervr_render *render, uint64_t bytes, struct powervr_buffer **result);

/*
 * The least bytes of a buffer the pool keeps; a storage of at least as
 * many is cleared by the core (vk-memory.c), a smaller one by the
 * processor as it is made.
 */
#define POWERVR_BUFFER_POOL_LEAST	(256ULL * 1024ULL)

/* Makes a buffer of at least bytes bytes whose bytes the caller clears before anything reads them. */
int powervr_buffer_create_uncleared(struct powervr_render *render, uint64_t bytes, struct powervr_buffer **result);

/* Takes a freed buffer that holds bytes bytes from the pool, its contents left as they were; ENOENT when none. */
int powervr_buffer_reuse(struct powervr_render *render, uint64_t bytes, struct powervr_buffer **result);

/* Takes another reference on a buffer. */
void powervr_buffer_retain(struct powervr_render *render, struct powervr_buffer *buffer);

/* Drops a reference; the last one frees the buffer, or keeps it after a fault. */
void powervr_buffer_put(struct powervr_render *render, struct powervr_buffer *buffer);

/* Frees a list of buffers kept for a reset, linked through next. */
void powervr_buffers_free(struct powervr_buffer *list);

/* Makes a session's handle on a buffer, mapped at a device address of its own; the handle takes a reference. */
int powervr_resource_make(struct powervr_session *session, struct powervr_buffer *buffer, int mappable, int shareable, struct powervr_resource **result);

/* Undoes a handle: unmaps it and drops its buffer, unless the core may still reach the session. */
void powervr_resource_unmake(struct powervr_session *session, struct powervr_resource *resource);
void powervr_session_describe_fault(struct powervr_session *session, uint64_t fault);
struct powervr_render_job;
struct powervr_render_draw;
void powervr_session_note_job(struct powervr_session *session, const struct powervr_render_job *job, const struct powervr_render_draw *prefix);
void powervr_session_describe_job(struct powervr_session *session);
uint64_t powervr_session_border_table(const struct powervr_session *session);
int powervr_resource_make_region_headers(struct powervr_session *session, struct powervr_buffer *buffer, struct powervr_resource **result);
int powervr_resource_make_free_list(struct powervr_session *session, struct powervr_buffer *buffer, struct powervr_resource **result);

/* Finds a blob or an import of a session by the identifier the renderer names it by, or NULL. */
struct powervr_resource *powervr_session_blob(struct powervr_session *session, uint32_t identifier);

/*
 * Puts a callback in a session's ring, retired once the timeline reaches
 * value, with an error and the job marker it frees; the caller checked
 * the ring has room.
 */
void powervr_session_pend(struct powervr_render *render, struct powervr_session *session, unsigned kind, uint32_t value, struct drv_gpu_completion *completion, int error, unsigned marker);

/* Reports whether a session's completion point was reached. */
int powervr_session_point_reached(const struct powervr_session *session, unsigned kind, uint32_t value);

/* Waits, bounded, for a session's completion point: 0, or ETIMEDOUT. */
int powervr_session_point_wait(const struct powervr_session *session, unsigned kind, uint32_t value, unsigned milliseconds);

/* Notes a session's last kicked work. */
void powervr_session_point_set(struct powervr_session *session, unsigned kind, uint32_t value);


/*
 * Releases the memory of a session's render jobs whose points were
 * reached (all of them when wait is set, waiting for each), in order
 * (render-job.c).  Nothing is released of a lost or isolated session.
 */
void powervr_render_jobs_reap(struct powervr_render *render, struct powervr_session *session, int wait);
void powervr_render_jobs_forget(struct powervr_render *render, struct powervr_session *session);

/*
 * Notes when the TA and the 3D of a session's running render jobs were
 * seen done, for the submission timing; the queue lock is held
 * (render-job.c).
 */
void powervr_render_jobs_stamp(struct powervr_session *session, uint64_t now);

/* Frees the transfer memory a session kept mapped, when the session ends (transfer-job.c). */
void powervr_transfer_forget(struct powervr_render *render, struct powervr_session *session);

/*
 * Takes one command frame of the Vulkan transport (or an empty marker
 * frame): runs it at once and puts its callback in the ring after the
 * session's work.
 */
int powervr_render_frame(struct powervr_render *render, struct powervr_session *session, const void *command, uint32_t bytes, uint32_t flags, uint32_t timeline, struct drv_gpu_completion *completion);

/* Binds the Vulkan transport, the capset and the supervised jobs to the node's table. */
void powervr_render_runtime_bind(struct powervr_render *render);

/*
 * Makes the pages of a session's fixed area with its programs written;
 * the core's lock is not held.
 */
int powervr_session_fixed_make(struct powervr_render *render, struct powervr_session *session);

/* Returns the pages of a session's fixed area that nothing maps; the core's lock is not held. */
void powervr_session_fixed_drop(struct powervr_render *render, struct powervr_session *session);

/*
 * Makes a session's memory context, with the fixed area's pages mapped,
 * and its firmware records; the core's lock is held.
 */
int powervr_session_firmware_create(struct powervr_render *render, struct powervr_session *session);

/*
 * Returns a session's firmware records, tables and fixed area's pages;
 * the firmware no longer uses them.
 */
void powervr_session_firmware_destroy(struct powervr_render *render, struct powervr_session *session);

/*
 * Returns a kept session's records and tables once the firmware that knew
 * them is gone, without touching their old places in its heap; the fixed
 * area's pages join the node's kept buffers, which the caller frees next.
 */
void powervr_session_firmware_discard(struct powervr_session *session);

/*
 * Has the firmware clean up a session's context and waits for it,
 * bounded; EBUSY when the context stays busy.
 */
int powervr_session_cleanup(struct powervr_render *render, struct powervr_session *session);

/*
 * Kicks one empty job on a session's context that advances its timeline
 * to value once done, after an optional wait for a sync word; the core's
 * lock is held.
 */
int powervr_session_kick(struct powervr_render *render, struct powervr_session *session, uint32_t value, uint32_t fence_address, uint32_t fence_value);

/*
 * Kicks one compute command of words 32-bit words on a session's context
 * that advances its timeline to value once done; the core's lock is held.
 */
int powervr_session_kick_compute(struct powervr_render *render, struct powervr_session *session, uint32_t value, const uint32_t *command, uint32_t words);

/* Waits, bounded, for a session's timeline to reach a value; ETIMEDOUT when it does not. */
int powervr_session_wait(struct powervr_session *session, uint32_t value, unsigned milliseconds);

/*
 * Runs the driver's own compute work on the core and reports, one line
 * each, whether it wrote what it should, what the processor's cached
 * view sees of it, and how the task issue counts temporaries.
 * Weak: only a test kernel built with CONFIG_POWERVR_COMPUTE_CHECK has
 * it, so the ordinary start does not spend the time.
 */
extern void powervr_compute_check(struct powervr_render *render) __attribute__((weak));

/*
 * Kicks a session's context again without new commands, so the firmware
 * looks again at a fence it waits on; the core's lock is held.
 */
int powervr_session_rekick(struct powervr_render *render, struct powervr_session *session);

/* Reads a session's timeline word as the firmware left it. */
uint32_t powervr_session_timeline(const struct powervr_session *session);

/* Starts the thread that retires native commands and reads the firmware's notices. */
int powervr_service_start(struct powervr_render *render);

/* Ends the service thread and waits until it has. */
void powervr_service_stop(struct powervr_render *render);

/*
 * Ends every pending callback of a session with an error; the work may
 * still be running, nothing is said about that.
 */
void powervr_session_fail_pending(struct powervr_render *render, struct powervr_session *session, int error);

/*
 * Arms the device-wide quarantine: no new kick, every destroy and close
 * keeps what the core may reach.  Idempotent.
 */
void powervr_render_quarantine(struct powervr_render *render, int error);
void powervr_render_refusal_log(struct powervr_render *render, const struct powervr_session *session, const char *where);

/* The queue and recovery operations the node's table points at. */
int powervr_render_submit(void *device, void *session, const void *command, uint32_t bytes, uint32_t flags, uint32_t timeline, struct drv_gpu_completion *completion);
void powervr_render_drain(void *device, void *session);
int powervr_render_stop_begin(void *device, void *session, int error);
int powervr_render_stop_poll(void *device, void *session);
void powervr_render_fault(void *device, int error);
int powervr_render_reset(void *device);
int powervr_render_isolate(void *device, void *session);

/*
 * The native command the queue takes before the Venus stream exists: one
 * empty job, retired through the timeline like any other.
 */
#define POWERVR_NATIVE_EMPTY_JOB	0x4e4a4250U
#define POWERVR_NATIVE_BYTES		8U

/* Sets up a session's program windows and holds the Vulkan executor's places in them. */
int powervr_session_heaps_init(struct powervr_session *session);

/*
 * Maps a buffer at a place of its own in one of a session's program
 * windows; the core's lock is not held.
 */
int powervr_session_program_map(struct powervr_session *session, enum powervr_program_heap heap, struct powervr_buffer *buffer, uint64_t *address);

/* Unmaps a buffer mapped by powervr_session_program_map() and gives its place back. */
void powervr_session_program_unmap(struct powervr_session *session, enum powervr_program_heap heap, struct powervr_buffer *buffer, uint64_t address);

/*
 * Finds or makes a session's render target for a frame shape, with the
 * session's global free list made first; the core's lock is not held.
 */
int powervr_session_target(struct powervr_render *render, struct powervr_session *session, const struct powervr_target_shape *shape, struct powervr_target **result);

/* Reports the firmware address of one half (0 or 1) of a render target's data set. */
uint32_t powervr_target_data_address(const struct powervr_target *target, unsigned half);

/* Reports the layout a render target was made with. */
const struct powervr_target_layout *powervr_target_layout_of(const struct powervr_target *target);

/* Takes the half of a target the next render uses; renders alternate between the two (render-target.c). */
unsigned powervr_target_take_half(struct powervr_target *target);

/* Reports the 3D value of the last render kicked on one half of a target, 0 for none (render-target.c). */
uint32_t powervr_target_half_fragment(const struct powervr_target *target, unsigned half);

/* Notes the 3D value of the render just kicked on one half of a target (render-target.c). */
void powervr_target_half_note(struct powervr_target *target, unsigned half, uint32_t fragment_value);

/* Reads the parameter manager's catalogues from one half of a target's record (render-target.c). */
void powervr_target_catalogues(const struct powervr_target *target, unsigned half, uint64_t catalogues[POWERVR_TARGET_CATALOGUES]);

/* Reports the firmware address of a target's render target data record (render-target.c). */
uint32_t powervr_target_record_address(const struct powervr_target *target);

void powervr_target_dump(const struct powervr_target *target);

/*
 * Prints what the render targets know of a recovery: the free lists a
 * faulted parameter manager catalogue page lies in or is listed by, and
 * the target half at the recovery's data set address; the core's lock is
 * held (render-target.c).
 */
void powervr_targets_describe_fault(struct powervr_render *render, uint32_t target_data, uint64_t catalogue);
void powervr_session_global_list_dump(const struct powervr_session *session);

/* Reports the firmware address of the cleanup count of one half of a render target, which a kick names. */
uint32_t powervr_target_cleanup_address(const struct powervr_target *target, unsigned half);

/*
 * Has the firmware clean up every render target and free list of a
 * session, bounded; EBUSY when one stays busy.
 */
int powervr_session_targets_cleanup(struct powervr_render *render, struct powervr_session *session);

/* Returns a session's render targets and free lists once the firmware no longer uses them. */
void powervr_session_targets_destroy(struct powervr_render *render, struct powervr_session *session);

/*
 * Returns a kept session's render targets and free lists once the
 * firmware that knew them is gone; the core's lock is held.
 */
void powervr_session_targets_discard(struct powervr_session *session);

/* Acts on a free list notice of the firmware; the core's lock is held. */
void powervr_targets_notice(struct powervr_render *render, uint32_t type, const struct powervr_object *buffer, uint32_t offset);

/* Copies what the firmware reported of the free lists; the core's lock is held. */
void powervr_render_target_events(const struct powervr_render *render, struct powervr_target_events *events);

/* Builds the shading cluster programs of a render context's switch; the node is starting. */
int powervr_render_switch_shaders(struct powervr_render *render);

/*
 * Finds or makes a session's render context: its geometry and fragment
 * contexts, their command buffers and timelines, the record with its
 * static state, and the switch's buffers and programs.  The core's lock
 * is not held.
 */
int powervr_session_render_context(struct powervr_render *render, struct powervr_session *session, struct powervr_render_context **result);

/*
 * Writes the compute context's static state with the programs and
 * buffers its switch uses, mapped in the session's windows; before the
 * context's first kick, the core's lock not held.
 */
int powervr_session_compute_switch(struct powervr_render *render, struct powervr_session *session);

/* Returns the compute switch's memory; the firmware no longer uses the context, or is gone (discard). */
void powervr_compute_switch_destroy(struct powervr_render *render, struct powervr_session *session, unsigned discard);

/* Returns a session's render context once the firmware no longer uses it. */
void powervr_render_context_destroy(struct powervr_render *render, struct powervr_session *session);

/*
 * Returns a kept session's render context once the firmware that knew it
 * is gone; the core's lock is held.
 */
void powervr_render_context_discard(struct powervr_session *session);

/*
 * Reports the firmware address of the next context of a session the
 * firmware still has to clean up, or zero when none is left: the compute
 * context, then the render context's geometry and fragment contexts.
 */
uint32_t powervr_session_next_cleanup(const struct powervr_session *session, unsigned *which);

/* Notes that the firmware cleaned up the context powervr_session_next_cleanup() named. */
void powervr_session_cleaned(struct powervr_session *session, unsigned which);

/*
 * What a render's kicks leave to wait on: the timeline words and the
 * values its geometry and fragment write, and what the firmware had
 * reported of the free lists before it ran.
 */
struct powervr_render_marks {
	uint32_t geometry_value;
	uint32_t fragment_value;
	struct powervr_list_marks lists;
};

/*
 * Kicks one render on a session's render context: the geometry and its
 * partial render in one combined kick, then the fragment, all on one
 * half of a render target, after the syncs it waits for; the core's lock
 * is held.
 */
int powervr_session_kick_render(struct powervr_render *render, struct powervr_session *session, struct powervr_target *target, unsigned half, const uint8_t *geometry, const uint8_t *partial, const uint8_t *fragment, const struct powervr_sync *waits, unsigned wait_count, struct powervr_render_marks *marks);

/*
 * Waits, bounded, for a render's fragment to finish and reports its
 * stages: ok for the TA and the 3D, fail(free list) for the TA when its
 * lists ran out, and fail(timeout) for the stage it stopped in;
 * ETIMEDOUT when it did not finish.
 */
int powervr_render_finish(struct powervr_render *render, struct powervr_session *session, const struct powervr_render_marks *marks, unsigned milliseconds);

/*
 * Says the TA and 3D stages of a render job whose point was reached:
 * ok, or fail(free list) for the TA when its lists ran out.
 */
void powervr_render_retired(struct powervr_render *render, struct powervr_session *session, const struct powervr_render_marks *marks);

/*
 * Runs the driver's own render on the core and reports how it went.
 * Weak: only a test kernel built with CONFIG_POWERVR_RENDER_CHECK has
 * it.
 */
extern void powervr_render_check(struct powervr_render *render) __attribute__((weak));

/*
 * Prints what the firmware knows of a context reset: its recovery
 * records, its trace's tail and the context's record.  Weak: only a
 * kernel built with CONFIG_POWERVR_HWR_DUMP has it (firmware-hwr.c).
 */
extern void powervr_hwr_dump(struct powervr_core *core, const struct powervr_object *context_record) __attribute__((weak));

/*
 * Finds or makes a session's transfer context; the core's lock is not
 * held.
 */
int powervr_session_transfer_context(struct powervr_render *render, struct powervr_session *session, struct powervr_transfer_context **result);

/* Returns a session's transfer context once the firmware no longer uses it. */
void powervr_transfer_context_destroy(struct powervr_render *render, struct powervr_session *session);

/* Returns a kept session's transfer context once the firmware that knew it is gone; the core's lock is held. */
void powervr_transfer_context_discard(struct powervr_session *session);

/*
 * Kicks one transfer command (POWERVR_TRANSFER_COMMAND_BYTES) on a
 * session's transfer context after the firmware sees each of the
 * wait_count waits reached, its timeline advancing to the next value
 * once done, which it reports; the core's lock is held.
 */
int powervr_session_kick_transfer(struct powervr_render *render, struct powervr_session *session, const uint8_t *command, const struct powervr_sync *waits, unsigned wait_count, uint32_t *value);

/* Reports the firmware address and value a firmware wait for a session's completion point names; 0 for a point with nothing to wait for. */
unsigned powervr_session_point_sync(const struct powervr_session *session, unsigned kind, uint32_t value, struct powervr_sync *sync);

/*
 * Waits, bounded, for a transfer to finish: transfer ok once done,
 * fail(timeout) and ETIMEDOUT when it does not.
 */
int powervr_transfer_finish(struct powervr_session *session, uint32_t value, unsigned milliseconds);

#endif
