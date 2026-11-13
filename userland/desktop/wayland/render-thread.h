/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The render thread (render-thread.c, ws213-p004; the design is
 * plan/ws213/phase001/phase.md 2 and 4, "the display list and the two
 * steps of the pull").
 *
 * The main thread says it may draw (a want: a frame, or a layer's picture
 * that presents nothing, with a number that moves only when the kind
 * changes or the want is raised again after it went down).  The render
 * thread acquires the images a frame needs (the output's, and the heads'
 * the want lists that it does not hold yet) and answers ready, with every
 * image it holds; from then until the main thread answers it calls no
 * Vulkan, so the main thread may walk its drawing (and use Vulkan as it
 * always has) and write the display list.  The answer is the list, which
 * takes the want down at once (a want raised after it is a new one), or a
 * cancel (the want changed meanwhile): the images stay held for a later
 * frame.  With the list the render thread records it into the command
 * buffers, submits, presents the output's and the heads' images, and tells
 * the main thread the job completed once it has seen the job's fence
 * signalled.  As many jobs may be in flight as there are slots.
 *
 * The thread waits for one thing at a time and never with a timeout: it
 * looks at the main thread's requests first and does what it can; with
 * nothing it can do and a job or a task in flight it waits for the oldest
 * one's fence, however long that takes, and tells its completion (so every
 * completion reaches the main thread before the thread sleeps, which the
 * main thread needs to draw again); with nothing in flight it waits for
 * the main thread's next change.  The output's image is acquired without
 * waiting while anything is in flight (the fence is waited for instead),
 * and only with nothing in flight does the acquire block until an image is
 * free.
 *
 * Every message to the main thread goes through one ordered queue: a
 * frame's completion always comes before the ready that follows it.  A
 * park stops the thread at a point where it calls no Vulkan (after the
 * frame in flight), for the swapchains to be changed; images the thread
 * holds stay held unless the main thread forgets them there.  The thread
 * answers a park only once every task given before it has completed too
 * (the park drains its work, so nothing it submitted still runs on the
 * GPU), and the main thread either waits for the answer or asks without
 * waiting and is told by a message in the queue, after the completions
 * that came before it.
 *
 * The thread is the only one that submits the main thread's work to the
 * queue: a task is a command buffer the main thread recorded (an image's
 * move to its layout), which the thread submits with the task's fence
 * before any list given after it, so the drawings that follow sample the
 * image after the move.  The thread watches the fence and tells the main
 * thread when the task completed, tasks in the order they were given.
 * Tasks are submitted and watched while the thread is parked too (they
 * touch no swapchain), and a stop waits for them.
 *
 * The thread knows nothing of the compositor: its Vulkan work is done by
 * the operations it is given (compose.c's, or a host test's fakes).
 */

#ifndef KWL_RENDER_THREAD_H
#define KWL_RENDER_THREAD_H

#include <pthread.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

/* The heads (other displays) a frame may draw, as compose.h's KWL_HEADS. */
#define KWL_RENDER_HEADS	7U

/*
 * How many jobs may be submitted before the oldest completes: each has its
 * own command buffers and fence, a slot of the owner's.
 */
#define KWL_RENDER_FLIGHTS	2U

/* What the main thread may draw. */
enum kwl_render_kind {
	/* Nothing: the thread acquires nothing. */
	KWL_RENDER_NONE = 0,
	/* A frame of the output (and the heads). */
	KWL_RENDER_FRAME,
	/* A layer's picture, which presents nothing and needs no image. */
	KWL_RENDER_LAYER
};

/* The main thread's answer to a ready. */
enum kwl_render_answer {
	KWL_RENDER_ANSWER_NONE = 0,
	/* The display list is written: run it. */
	KWL_RENDER_ANSWER_LIST,
	/* The want changed: no list for this ready. */
	KWL_RENDER_ANSWER_CANCEL
};

/* What one step of the render thread came to. */
enum kwl_render_step {
	/* Did some work (or waited for a fence and told its completion): step again. */
	KWL_RENDER_STEP_WORKED = 1,
	/* Waits for the main thread: a want, an answer, the end of a park. */
	KWL_RENDER_STEP_BLOCKED,
	/* Asked to stop, with no frame in flight. */
	KWL_RENDER_STEP_STOP
};

/* Where the render thread is in the pull. */
enum kwl_render_state {
	/* Looking for a want. */
	KWL_RENDER_STATE_IDLE = 0,
	/* A ready is out; waiting for the main thread's answer (jobs submitted earlier may be in flight meanwhile). */
	KWL_RENDER_STATE_ANSWER
};

/* What one message of the render thread to the main thread says. */
enum kwl_render_message_kind {
	/* The images are held: the main thread may walk. */
	KWL_RENDER_READY = 1,
	/* The output's image could not be acquired: the want went down. */
	KWL_RENDER_ACQUIRE_FAILED,
	/* A frame or a layer's picture completed (its fence seen signalled), or failed. */
	KWL_RENDER_COMPLETED,
	/* A task completed (or its submission or its fence failed). */
	KWL_RENDER_TASK_DONE,
	/* The thread parked, drained, for a park asked without waiting (the main thread's message given back). */
	KWL_RENDER_PARKED
};

/*
 * The want: its number, its kind, and for a frame the heads (slots of
 * compose.h's heads) whose images it needs.
 */
struct kwl_render_want {
	uint64_t number;
	enum kwl_render_kind kind;
	unsigned head_count;
	unsigned heads[KWL_RENDER_HEADS];
};

/*
 * The images the render thread holds (acquired, not presented yet): the
 * output's and each head's, by slot; and for a ready, the result of this
 * round's acquire of each wanted head it did not hold (VK_SUCCESS when it
 * got one or did not try).
 */
struct kwl_render_held {
	unsigned output;
	uint32_t output_image;
	unsigned heads[KWL_RENDER_HEADS];
	uint32_t head_images[KWL_RENDER_HEADS];
	VkResult head_results[KWL_RENDER_HEADS];
};

/*
 * What the main thread gives with its list: the want's number and kind,
 * the slot it was recorded for (its command buffers and fence), and for a
 * frame the output's image and the heads it drew (every held head, which
 * the thread presents).
 */
struct kwl_render_job {
	uint64_t number;
	enum kwl_render_kind kind;
	unsigned slot;
	uint32_t output_image;
	unsigned heads[KWL_RENDER_HEADS];
	uint32_t head_images[KWL_RENDER_HEADS];
};

/*
 * A frame's outcome, as the operations report it: the recording's and the
 * submission's result (any failure is the compositor's), the output's
 * present (a display gone is not), each head's present by slot (heads
 * says which were presented), the recording's time in cycles, and the
 * times of the end of the recording, of the submission and of the
 * present (kwl_microseconds).
 */
struct kwl_render_outcome {
	VkResult result;
	VkResult present_result;
	unsigned heads[KWL_RENDER_HEADS];
	VkResult head_results[KWL_RENDER_HEADS];
	uint64_t replay_cycles;
	uint64_t run_cycles;
	/* Of replay_cycles, the Begins and the drawing's End, and how long the submit and the presents took (ws211-p010). */
	uint64_t begin_cycles;
	uint64_t end_cycles;
	uint64_t submit_cycles;
	uint64_t present_cycles;
	/* The frame's host-image copies had a command buffer of their own. */
	unsigned copied;
	uint64_t recorded_us;
	uint64_t queued_us;
	uint64_t presented_us;
	uint64_t completed_us;
};

/*
 * One message of the ordered queue to the main thread; the thread
 * allocates it, the main thread frees it.  number is the want's (or the
 * job's); held is what a ready holds; result an acquire's failure;
 * outcome a completion's; acquired_us when a ready's output image was
 * acquired and acquire_cycles how long that took; serial, for a job or a
 * task the thread submitted, the order it was submitted in (from 1 up),
 * by which the oldest in flight is waited for first.
 *
 * A task's message is the main thread's instead: it allocates it with the
 * task (number, task_command, task_fence), the thread submits and watches
 * it and posts it back as its completion (result the submission's or the
 * fence's), and the main thread frees it with the command buffer and the
 * fence.
 */
struct kwl_render_message {
	struct kwl_render_message *next;
	enum kwl_render_message_kind kind;
	uint64_t number;
	enum kwl_render_kind want_kind;
	struct kwl_render_held held;
	VkResult result;
	struct kwl_render_job job;
	struct kwl_render_outcome outcome;
	uint64_t acquired_us;
	uint64_t acquire_cycles;
	uint64_t serial;
	VkCommandBuffer task_command;
	VkFence task_fence;
};

/*
 * What the render thread shares with the main thread now, for the line the
 * main thread writes when a want goes unanswered (KWL RENDER stalled): the
 * thread's want, the answer it has not taken yet (answer NONE when it took
 * it), the park, whether a job is in flight, whether it holds the output's
 * image, the messages not taken, and the numbers of the last ready and the
 * last completion it posted.
 */
struct kwl_render_status {
	enum kwl_render_kind want_kind;
	uint64_t want_number;
	enum kwl_render_answer answer;
	uint64_t answer_number;
	unsigned park;
	unsigned parked;
	unsigned busy;
	unsigned held_output;
	unsigned messages;
	uint64_t last_ready;
	uint64_t last_completed;
};

/*
 * What the render thread did, counted by the thread alone and read once
 * it has ended (the line the compositor writes when it stops it): the jobs
 * and the tasks told complete, the waits for a job's and for a task's
 * fence (one each for those submitted: no fence is looked at again and
 * again), the acquires of the output's image that waited until one was
 * free and those that did not wait (taken ahead included) and found none,
 * and the times the thread slept for the main thread's next change.
 */
struct kwl_render_counts {
	uint64_t jobs;
	uint64_t tasks;
	uint64_t fence_waits;
	uint64_t task_waits;
	uint64_t acquires_waiting;
	uint64_t acquires_now;
	uint64_t acquires_missed;
	uint64_t sleeps;
};

/*
 * The Vulkan work the thread does through its owner: acquire the output's
 * next image (timeout_ns is 0, not waiting, or UINT64_MAX, until one is
 * free; it gives how long it took in cycles and when it ended, for the
 * main thread's lines), acquire a head's next image without waiting,
 * record and submit and present a job, and wait for the job's fence (and
 * reset it for the slot's next job), and the clock of the main thread's
 * lines in microseconds (NULL for the monotonic clock); and submit a
 * task's command buffer with its fence and wait for that fence.  The
 * thread waits for a fence only with timeout_ns UINT64_MAX: any result but
 * VK_SUCCESS is the fence's failure.
 */
struct kwl_render_ops {
	VkResult (*acquire_output)(void *context, uint64_t timeout_ns, uint32_t *image, uint64_t *cycles, uint64_t *at_us);
	VkResult (*acquire_head)(void *context, unsigned slot, uint32_t *image);
	void (*run)(void *context, const struct kwl_render_job *job, struct kwl_render_outcome *outcome);
	VkResult (*wait)(void *context, unsigned slot, uint64_t timeout_ns);
	uint64_t (*now_us)(void);
	VkResult (*submit_task)(void *context, VkCommandBuffer command, VkFence fence);
	VkResult (*wait_task)(void *context, VkFence fence, uint64_t timeout_ns);
};

/*
 * The render thread and what it shares with the main thread.
 *
 * Under lock: the want; the answer to the ready outstanding (for which
 * number) and the job it carries; the park and stop requests and whether
 * the thread is parked (it answers on answered), and the message a park
 * asked without waiting is told with (park_message, the main thread's,
 * posted back once parked); whether a frame is in
 * flight, and whether a job is being recorded, submitted and presented
 * (running); the message queue and whether the main thread was woken for it;
 * the held images (the thread's, written by the main thread only while
 * parked) and whether the output's may be acquired ahead (a frame's
 * acquire succeeded since the output was last forgotten); the numbers of the last ready and the last completion posted
 * (for kwl_render_status); the tasks given and not submitted yet, oldest
 * first; and sequence, which counts every change the thread waits for.
 * The thread's own: the operations and their context, where it is in the
 * pull (state), the number of the ready it waits to have answered, the
 * completion message of the next job (made before the job is taken, so
 * that a completion is never lost), the completions of the jobs in flight,
 * oldest first (flight_count of them; busy says any is, or a job runs),
 * the tasks submitted and not seen complete, oldest first, the serial of
 * the last job or task submitted, what it did (counts), and the main
 * thread's wake pipe.
 */
struct kwl_render_thread {
	pthread_mutex_t lock;
	pthread_cond_t changed;
	pthread_cond_t answered;
	uint64_t sequence;
	struct kwl_render_want want;
	enum kwl_render_answer answer;
	uint64_t answer_number;
	struct kwl_render_job job;
	unsigned park;
	unsigned parked;
	struct kwl_render_message *park_message;
	unsigned stop;
	unsigned busy;
	unsigned running;
	struct kwl_render_message *messages;
	struct kwl_render_message *messages_tail;
	unsigned main_woken;
	unsigned dropped;
	struct kwl_render_held held;
	unsigned ahead;
	uint64_t last_ready;
	uint64_t last_completed;
	struct kwl_render_message *tasks;
	struct kwl_render_message *tasks_tail;
	struct kwl_render_ops ops;
	void *context;
	enum kwl_render_state state;
	uint64_t ready_number;
	struct kwl_render_message *completion;
	struct kwl_render_message *flights[KWL_RENDER_FLIGHTS];
	unsigned flight_count;
	struct kwl_render_message *task_flights;
	struct kwl_render_message *task_flights_tail;
	uint64_t submitted;
	struct kwl_render_counts counts;
	uint64_t seen_sequence;
	int wake_main;
	pthread_t thread;
	unsigned started;
};

int kwl_render_init(struct kwl_render_thread *render, const struct kwl_render_ops *ops, void *context, int wake_main);
void kwl_render_destroy(struct kwl_render_thread *render);
int kwl_render_start(struct kwl_render_thread *render);
void kwl_render_stop(struct kwl_render_thread *render);
enum kwl_render_step kwl_render_step(struct kwl_render_thread *render);
void kwl_render_set_want(struct kwl_render_thread *render, enum kwl_render_kind kind, const unsigned *heads, unsigned head_count, uint64_t *number);
void kwl_render_answer(struct kwl_render_thread *render, uint64_t number, enum kwl_render_answer answer, const struct kwl_render_job *job);
struct kwl_render_message *kwl_render_take(struct kwl_render_thread *render);
int kwl_render_wait_message(struct kwl_render_thread *render, unsigned wait_ms);
int kwl_render_park(struct kwl_render_thread *render, unsigned wait_ms);
void kwl_render_park_request(struct kwl_render_thread *render, struct kwl_render_message *message);
void kwl_render_unpark(struct kwl_render_thread *render);
void kwl_render_forget(struct kwl_render_thread *render, unsigned output, unsigned heads_mask);
void kwl_render_task(struct kwl_render_thread *render, struct kwl_render_message *task);
int kwl_render_running(struct kwl_render_thread *render);
void kwl_render_status(struct kwl_render_thread *render, struct kwl_render_status *status);

#endif
