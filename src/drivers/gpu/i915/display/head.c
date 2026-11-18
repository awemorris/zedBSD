/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The second output, the head.
 *
 * While Keiland holds the resident output's lease, a claim of another
 * connected connector makes it the head (i915_head_claim_way() decides): an HDMI
 * or an external DisplayPort display on pipe B beside the built-in panel
 * on pipe A.  The claim writes no hardware; it prepares the output (its
 * mode, link and PLL) and gives the head a lease of its own.
 *
 * The head's first frame lights it, on the worker, from inside the display
 * window the resident output keeps: modeset screen 1, a run context of its
 * own (its hooks, its vblank event and frame counter on its pipe, its
 * power references), two buffers at its mode's size.  Its DBUF share is
 * computed for both pipes, and so must the resident output's: when the
 * resident output was lit for one pipe, the window is left and the
 * resident output lit again for two first (present.c).  Each frame is
 * copied into the head's back buffer and flipped to, as the resident
 * output's are.  The head's release, and the window's end, stop it and
 * the output goes dark (D-RELEASE, no hold).
 *
 * A head that cannot be lit beside the resident output latches its
 * connector limited until the topology moves (D-LIMIT): its claims are
 * the limit meanwhile, so the compositor does not light the resident
 * output again and again for a head that will not come up.
 *
 * Every modeset call on the head's screen is made with screen 1 selected,
 * and the worker leaves with screen 0 selected again: the resident
 * output's flips and light assume it.
 */

#include "internal.h"
#include "head.h"
#include "output.h"
#include "present.h"
#include "resident.h"
#include "scanout.h"
#include <kern/kcrt.h>

#include "../ggtt.h"
#include "../i915.h"
#include "../memory.h"
#include "../mmio.h"
#include "../ppgtt.h"
#include "../worker.h"

#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/sched.h>

#include <uapi/errno.h>
#include <stddef.h>

/* How many times a head's DP link is lowered and tried again before the head is given up. */
#define I915_HEAD_LINK_TRIES		4U

/* TRANSCONF of pipe A, the pipes 0x1000 apart, and its enable and state bits (both clear once the transcoder stopped). */
#define I915_HEAD_TRANSCONF_A		0x70008U
#define I915_HEAD_PIPE_STRIDE		0x1000U
#define I915_HEAD_TRANSCONF_ON		0xc0000000U

/* What a head's claim is decided on: the kinds of output. */
enum i915_head_kind {
	I915_HEAD_KIND_PANEL = 0,
	I915_HEAD_KIND_HDMI,
	I915_HEAD_KIND_DP_EXT,
	I915_HEAD_KIND_OTHER
};

/*
 * The facts of one claim of a connector that is not the resident output,
 * gathered under the locks that keep them still.
 *
 * It lives on the caller's stack for the one decision.
 */
struct i915_head_claim_facts {
	/* Nonzero while a session holds the resident output's lease. */
	int resident_leased;

	/* The resident output's kind. */
	enum i915_head_kind resident_kind;

	/* Nonzero while a head is claimed (by any lease). */
	int head_claimed;

	/* Nonzero once a head's stop could not be confirmed: no head is lit again. */
	int head_broken;

	/* Nonzero while this connector is latched limited for the current topology. */
	int limited;

	/* The claimed connector's kind. */
	enum i915_head_kind kind;
};

/*
 * What a head's claim comes to: a move of the resident output (no lease
 * held), the head, or a refusal for the limit of the outputs shown at
 * once.
 */
enum i915_head_claim_way {
	I915_HEAD_CLAIM_MOVE = 0,
	I915_HEAD_CLAIM_HEAD,
	I915_HEAD_CLAIM_LIMIT
};

static int i915_head_light(struct i915_display *display);
static int i915_head_light_once(struct i915_display *display, int *enable_rc);
static int i915_head_buffers(struct i915_display *display);
static void i915_head_buffers_release(struct i915_display *display, int confirmed);
static int i915_head_commit_stop(struct i915_display *display);
static int i915_head_map(struct i915_display *display, struct i915_ppgtt *vm);
static void i915_head_unmap(struct i915_display *display);
static struct i915_scanout *i915_head_target(struct i915_display *display, int *flip);
static int i915_head_flip(struct i915_display *display, struct i915_scanout *to);
static void i915_head_copy(struct i915_scanout *back, const struct i915_worker_present *frame);
static enum i915_head_kind i915_head_kind_of(unsigned hpd_kind);
static const char *i915_head_kind_name(enum i915_output_kind kind);
static int i915_head_resident_leased(struct i915_display *display);
static unsigned i915_head_held_refs(const struct i915_lcd_kernel *k);
static int i915_head_may_light_locked(struct i915_display *display);
static void i915_head_log_dbuf(struct i915_display *display, const char *when);
static enum i915_head_claim_way i915_head_claim_way(const struct i915_head_claim_facts *facts, const char **reason);

/*
 * Prepares the head's lease once: its mutex.
 */
void
drv_i915_head_init(
	struct i915_display *display)
{
	struct i915_display_head *head;

	head = &display->head;

	/* The mutex is prepared already. */
	if (head->inited)
		return;

	/* The head's own mutex; the lease numbers are the resident output's. */
	(void)mutex_init(&head->mutex, LOCK_RANK_DEVICE, "i915-display-head");
	head->inited = 1;
}

/*
 * Makes a connected connector that is not the resident output the head,
 * under the resident output's lease (the display claim operation, any
 * thread).
 *
 * No hardware is written: the output is prepared and the head given a
 * lease; its first frame lights it.  Returns 0 with the lease, ENOSPC for
 * the limit of the outputs shown at once (D-LIMIT), or the preparation's
 * error (ENXIO, EOPNOTSUPP, ...).  A claim with no resident lease held is
 * not this function's: the resident output moves instead (display.c).
 */
int
drv_i915_head_claim(
	struct i915_device *device,
	void *session,
	unsigned connector,
	uint64_t generation,
	uint64_t *lease)
{
	struct i915_display *display;
	struct i915_display_head *head;
	struct i915_head_claim_facts facts;
	struct i915_display_output next;
	struct i915_hpd_output found;
	enum i915_head_claim_way way;
	const char *reason;
	unsigned long irq;
	unsigned resident_pipe;
	unsigned head_pipe;
	int error;

	display = device->display;
	head = &display->head;
	drv_i915_head_init(display);

	/* The connector's kind, as the hotplug path last took it. */
	error = drv_i915_display_output_connector(display, connector, &found);
	if (error != 0)
		return ENXIO;

	/* The facts the rule decides on: the resident lease, the head, the latch, the kinds. */
	kern_memset(&facts, 0, sizeof(facts));
	facts.resident_leased = i915_head_resident_leased(display);
	facts.kind = i915_head_kind_of(found.kind);
	facts.resident_kind = I915_HEAD_KIND_OTHER;
	if (display->output.kind == I915_OUTPUT_KIND_PANEL && !display->output.none)
		facts.resident_kind = I915_HEAD_KIND_PANEL;

	/* The head's state and the latch of this connector's generation, as the worker sees them. */
	irq = spin_lock_irqsave(&device->irq_lock);

	facts.head_claimed = head->claimed;
	facts.head_broken = head->broken;
	if (head->limited &&
	    head->limited_connector == connector &&
	    head->limited_generation == found.generation)
		facts.limited = 1;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* What the claim comes to; a move is the caller's. */
	way = i915_head_claim_way(&facts, &reason);
	if (way == I915_HEAD_CLAIM_MOVE)
		return EAGAIN;
	if (way == I915_HEAD_CLAIM_LIMIT) {
		kern_logf("i915: display head: claim of connector %u refused: %s (the limit of outputs shown at once)\n", connector, reason);
		return ENOSPC;
	}

	/* The connector's own preparation (no hardware is written). */
	reason = "";
	error = drv_i915_display_output_prepare(display, connector, &next, &reason);
	if (error != 0) {
		kern_logf("i915: display head: connector %u cannot be shown beside the resident output: %s (%d)\n", connector, reason, error);
		return error;
	}

	/* One pipe drives one output. */
	resident_pipe = drv_i915_display_output_pipe(&display->output);
	head_pipe = drv_i915_display_output_pipe(&next);
	if (resident_pipe == head_pipe) {
		kern_logf("i915: display head: claim of connector %u refused: its pipe %u is the resident output's (the limit of outputs shown at once)\n", connector, head_pipe);
		return ENOSPC;
	}

	/* The lease number, from the resident output's numbering so the two never meet. */
	drv_i915_present_lease_init(display);
	mutex_lock(&display->rd.mutex);

	*lease = display->rd.next_lease;
	display->rd.next_lease++;

	mutex_unlock(&display->rd.mutex);

	/* The head's lease, under its mutex; a claim that raced in first keeps it. */
	mutex_lock(&head->mutex);

	if (head->owner != NULL) {
		mutex_unlock(&head->mutex);
		kern_logf("i915: display head: claim of connector %u refused: a second output is shown already (the limit of outputs shown at once)\n", connector);
		return ENOSPC;
	}

	head->owner = session;
	head->lease = *lease;
	head->sequence = 0U;
	head->present_tick = 0U;
	head->connector = connector;
	head->generation = generation;
	head->output = next;

	/* The worker reads the claim and the pipe when it decides how the resident output is lit. */
	irq = spin_lock_irqsave(&device->irq_lock);

	head->claimed = 1;
	head->pipe = head_pipe;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	mutex_unlock(&head->mutex);

	/* Succeeded: the session holds the head; its first frame lights it. */
	kern_logf("i915: display head: lease %llu claimed (connector %u, %s, pipe %c)\n",
	    (unsigned long long)*lease,
	    connector,
	    i915_head_kind_name(next.kind),
	    (char)('A' + head_pipe));
	return 0;
}

/*
 * Tells whether a session's lease is the head's.
 */
int
drv_i915_head_owns(
	struct i915_display *display,
	void *session,
	uint64_t lease)
{
	struct i915_display_head *head;
	int owns;

	head = &display->head;

	/* A head never prepared was never claimed. */
	if (!head->inited)
		return 0;

	/* The owner and the lease, under the head's mutex. */
	mutex_lock(&head->mutex);

	owns = 0;
	if (head->owner == session && head->lease == lease && lease != 0U)
		owns = 1;

	mutex_unlock(&head->mutex);

	/* Succeeded: whether it is the head's. */
	return owns;
}

/*
 * Ends the head's lease (the display release operation on the head's
 * lease): the worker stops the head when it is lit, and the output goes
 * dark (D-RELEASE).  The lease ends whatever the stop did (a
 * failed release would leave the core's claim count and the compositor's
 * device behind); a stop that was not confirmed breaks the head and is
 * logged.  Returns 0, or EINVAL for a session that does not hold the
 * lease.
 */
int
drv_i915_head_release(
	struct i915_device *device,
	void *session,
	uint64_t lease)
{
	struct i915_display *display;
	struct i915_display_head *head;
	struct i915_worker_present item;
	unsigned long irq;
	uint64_t frames;
	int error;

	display = device->display;
	head = &display->head;
	drv_i915_head_init(display);

	/* Ends the lease under the head's mutex. */
	mutex_lock(&head->mutex);

	if (head->owner != session || head->lease != lease) {
		mutex_unlock(&head->mutex);
		return EINVAL;
	}

	/* The worker stops a lit head; an unlit one has nothing to stop. */
	kern_memset(&item, 0, sizeof(item));
	item.head = 1;
	error = drv_i915_worker_sync_display(device, I915_WORKER_SYNC_RELEASE, &item);

	/* The head is free, whatever the stop did (a stop that failed left the head broken). */
	frames = head->sequence;
	head->owner = NULL;
	head->lease = 0U;
	head->sequence = 0U;

	irq = spin_lock_irqsave(&device->irq_lock);

	head->claimed = 0;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	mutex_unlock(&head->mutex);

	kern_logf("i915: display head: lease %llu released after %llu frame(s) (connector %u)\n",
	    (unsigned long long)lease,
	    (unsigned long long)frames,
	    head->connector);

	/* A worker that did not serve the release (it is not serving: nothing is lit). */
	if (error != 0)
		kern_logf("i915: display head: the release was not served by the worker (%d); the lease ends anyway\n", error);

	/* Succeeded: the head is free. */
	return 0;
}

/*
 * Ends the head's lease a closing session still holds; the head is
 * stopped first.
 */
void
drv_i915_head_lease_close(
	struct i915_device *device,
	void *session)
{
	struct i915_display_head *head;
	uint64_t lease;
	void *owner;

	/* A device without a display has no head. */
	if (device->display == NULL)
		return;

	head = &device->display->head;

	/* A head never prepared was never claimed. */
	if (!head->inited)
		return;

	/* The lease the session holds, if any. */
	mutex_lock(&head->mutex);

	owner = head->owner;
	lease = head->lease;

	mutex_unlock(&head->mutex);

	/* Another session's head, or none. */
	if (owner != session)
		return;

	/* Ends it as a release does. */
	kern_logf("i915: display head: the lease owner closed without releasing it\n");
	(void)drv_i915_head_release(device, session, lease);
}

/*
 * Tells the query's state of a connector that is not the resident output:
 * whether it is the head, whether the head is lit, and whether a claim of
 * it now is the limit of the outputs shown at once (GPU_DISPLAY_LIMITED).
 */
void
drv_i915_head_connector_state(
	struct i915_display *display,
	unsigned connector,
	int *is_head,
	int *lit,
	int *limited)
{
	struct i915_display_head *head;
	struct i915_head_claim_facts facts;
	struct i915_hpd_output found;
	enum i915_head_claim_way way;
	const char *reason;
	unsigned long irq;
	int known;
	int error;

	head = &display->head;
	*is_head = 0;
	*lit = 0;
	*limited = 0;

	/* The facts a claim would be decided on now. */
	kern_memset(&facts, 0, sizeof(facts));
	facts.resident_leased = i915_head_resident_leased(display);
	facts.resident_kind = I915_HEAD_KIND_OTHER;
	if (display->output.kind == I915_OUTPUT_KIND_PANEL && !display->output.none)
		facts.resident_kind = I915_HEAD_KIND_PANEL;
	facts.kind = I915_HEAD_KIND_OTHER;
	known = 0;
	error = drv_i915_display_output_connector(display, connector, &found);
	if (error == 0) {
		facts.kind = i915_head_kind_of(found.kind);
		known = 1;
	}

	/* The head's state and the latch of this connector's generation. */
	irq = spin_lock_irqsave(&display->device->irq_lock);

	if (head->claimed && head->connector == connector) {
		*is_head = 1;
		*lit = head->up;
	}

	facts.head_claimed = head->claimed;
	facts.head_broken = head->broken;
	if (known &&
	    head->limited &&
	    head->limited_connector == connector &&
	    head->limited_generation == found.generation)
		facts.limited = 1;

	spin_unlock_irqrestore(&display->device->irq_lock, irq);

	/* The head itself is shown, not the limit. */
	if (*is_head)
		return;

	/* Succeeded: whether a claim now is the limit. */
	way = i915_head_claim_way(&facts, &reason);
	if (way == I915_HEAD_CLAIM_LIMIT)
		*limited = 1;
}

/*
 * Gives the pipes besides the resident output's that the resident run is
 * to leave DBUF room for: BIT(the claimed head's pipe) while the head may
 * be lit (i915_head_may_light_locked), else 0.  Runs on the worker as the
 * run begins.
 */
unsigned
drv_i915_head_run_pipes(
	struct i915_display *display)
{
	struct i915_display_head *head;
	unsigned long irq;
	unsigned pipes;
	int may;

	head = &display->head;

	/* The claim and its pipe, as the claim left them. */
	irq = spin_lock_irqsave(&display->device->irq_lock);

	pipes = 0U;
	may = i915_head_may_light_locked(display);
	if (may)
		pipes = 1U << head->pipe;

	spin_unlock_irqrestore(&display->device->irq_lock, irq);

	/* Succeeded: the pipes. */
	return pipes;
}

/*
 * Latches the claimed head's connector limited for its generation
 * (D-LIMIT): its claims are the limit until the connector is
 * plugged again, and no resident run leaves room for it meanwhile.
 */
void
drv_i915_head_limit(
	struct i915_display *display,
	const char *why)
{
	struct i915_display_head *head;
	unsigned long irq;

	head = &display->head;

	/* The connector and the generation it was claimed with. */
	irq = spin_lock_irqsave(&display->device->irq_lock);

	head->limited = 1;
	head->limited_connector = head->connector;
	head->limited_generation = head->generation;

	spin_unlock_irqrestore(&display->device->irq_lock, irq);

	/* Says why, once. */
	kern_logf("i915: display head: connector %u is the limit until it is plugged again: %s\n", head->connector, why);
}

/*
 * Tells whether a frame of the head needs the resident output lit again,
 * for two pipes, first: the head is not lit, it may be lit, and the
 * resident run left no DBUF room for its pipe.  A head that may not be lit
 * never asks for it (otherwise the frame, which stays at the
 * head of the queue, would light the resident output again and again).
 * Runs on the worker inside the window; the caller holds the device IRQ
 * lock.
 */
int
drv_i915_head_needs_relight(
	struct i915_device *device)
{
	struct i915_display *display;
	struct i915_display_head *head;
	unsigned pipe;
	int may;

	display = device->display;
	head = &display->head;

	/* A lit head needs nothing, nor does one that may not be lit. */
	if (head->up)
		return 0;
	may = i915_head_may_light_locked(display);
	if (!may)
		return 0;

	/* The run left room for the head's pipe already. */
	pipe = 1U << head->pipe;
	if ((display->window.run_pipes & pipe) != 0U)
		return 0;

	/* Succeeded: the resident output is to be lit again for two pipes. */
	return 1;
}

/*
 * Shows one frame of the head from inside the window, lighting the head
 * first when it is not lit: a CPU copy, or the GPU copy the frame's
 * builder makes, into the back buffer, then a flip on the head's screen.
 * Returns 0, ENXIO when the head cannot be lit (it is latched limited) or
 * is not claimed, or EIO when the copy or the flip failed.
 */
int
drv_i915_head_frame(
	struct i915_device *device,
	const struct i915_worker_present *frame)
{
	struct i915_display *display;
	struct i915_display_head *head;
	struct i915_scanout *back;
	uint64_t batch_va;
	unsigned index;
	int flip;
	int error;

	display = device->display;
	head = &display->head;

	/* A head released meanwhile shows nothing. */
	if (!head->claimed)
		return ENXIO;

	/* The head's first frame lights it. */
	if (!head->up) {
		error = i915_head_light(display);
		if (error != 0)
			return ENXIO;
	}

	/* The buffer the frame goes into, and whether the head must flip to it. */
	back = i915_head_target(display, &flip);
	index = 0U;
	if (back == &head->buf[1])
		index = 1U;

	/* An empty frame, or one larger than the head, is refused (the claim's mode is the head's size). */
	if (frame->width == 0U || frame->height == 0U)
		return EINVAL;
	if (frame->width > back->width || frame->height > back->height)
		return EINVAL;

	/* The GPU copy into the back buffer, in the presenting session's space, or the CPU copy. */
	if (frame->build != NULL) {
		error = i915_head_map(display, frame->vm);
		if (error != 0)
			return EIO;

		batch_va = 0U;
		error = frame->build(frame->build_ctx, head->map_va[index], back->width, back->height, back->pitch, &batch_va);
		if (error == 0)
			error = drv_i915_worker_run_batch(device, frame->context, batch_va);
		if (error != 0) {
			kern_logf("i915: display head: the GPU copy into the head's buffer failed: %d\n", error);
			return EIO;
		}
	} else {
		i915_head_copy(back, frame);
		drv_i915_scanout_publish(back);
	}

	/* Shows the buffer. */
	if (flip) {
		error = i915_head_flip(display, back);
		if (error != 0)
			return EIO;
	}

	/* Succeeded: the frame is on the head, or armed to be at its next vblank. */
	return 0;
}

/*
 * Stops the head when it is lit, the plane and the crtc off on the head's
 * screen, and the output goes dark; the resident output keeps running.
 *
 * keep (the window's end: a sleep, a hold that ran out, the shutdown)
 * keeps the head's buffers and its last picture, so that the next window
 * lights it again at its start (the compositor of the extended
 * mode presents a head only when it opens it or the wallpaper changes).
 * Without keep (the release) the buffers are given back, dormant ones too.
 * A stop that was not confirmed abandons the buffers and breaks the head.
 * Runs on the worker: inside the window for a lit head.
 */
void
drv_i915_head_stop(
	struct i915_display *display,
	int keep)
{
	struct i915_display_head *head;
	unsigned held;
	int error;

	head = &display->head;

	/* Nothing lit: only dormant buffers of a release to give back. */
	if (!head->up) {
		if (!keep && head->dormant) {
			i915_head_buffers_release(display, 1);
			head->dormant = 0;
		}

		return;
	}

	/* The plane and the crtc off; the GPU's mappings go before the buffers; the DBUF keeps the head's pipe reserved. */
	error = i915_head_commit_stop(display);
	i915_head_unmap(display);
	i915_head_log_dbuf(display, "stopped");

	/* The head is dark; its run's parameters are gone. */
	head->up = 0;
	head->k.p = NULL;

	/* Everything the head's run took must be given back, or the display may still read its buffers. */
	held = i915_head_held_refs(&head->k);
	if (error != 0 || held != 0U) {
		i915_head_buffers_release(display, 0);
		head->broken = 1;
		head->dormant = 0;
		kern_logf("i915: display head: XXX the stop was not confirmed (rc=%d, power refs held %u): its buffers are kept for ever and no second output is lit again\n", error, held);
		return;
	}

	/* Kept for the next window: buffer A, which the next lighting shows first, takes the picture B shows. */
	if (keep) {
		if (head->front != 0U) {
			drv_i915_gt_clflush(head->buf[1].cpu, head->buf[1].size);
			kern_memcpy(head->buf[0].cpu, head->buf[1].cpu, head->buf[0].size);
			drv_i915_scanout_publish(&head->buf[0]);
		}

		head->front = 0U;
		head->dormant = 1;
		kern_logf("i915: display head: stopped with the window (connector %u); its picture is kept for the next window\n", head->connector);
		return;
	}

	/* Succeeded: the buffers are given back and the output is dark. */
	i915_head_buffers_release(display, 1);
	kern_logf("i915: display head: stopped (connector %u); the output is dark\n", head->connector);
}

/*
 * Lights a head kept over the last window's end again at the start of a
 * new window, with its last picture, when it may still be lit
 * and its connector is still connected with the generation it was claimed
 * with.  Runs on the worker inside the window, the resident output lit.
 */
void
drv_i915_head_resume(
	struct i915_display *display)
{
	struct i915_display_head *head;
	struct i915_hpd_output found;
	unsigned long irq;
	int may;
	int error;

	head = &display->head;

	/* Only a dormant head. */
	if (!head->dormant || head->up)
		return;

	/* It may still be lit beside the resident output. */
	irq = spin_lock_irqsave(&display->device->irq_lock);

	may = i915_head_may_light_locked(display);

	spin_unlock_irqrestore(&display->device->irq_lock, irq);

	/* A head that may not be lit waits for its release, which gives its buffers back. */
	if (!may)
		return;

	/* A run that left no DBUF room for the head's pipe: the head's next frame lights the resident output again first. */
	if ((display->window.run_pipes & (1U << head->pipe)) == 0U)
		return;

	/* Its connector, still connected, of the claim's generation (else the compositor opens it again). */
	error = drv_i915_display_output_connector(display, head->connector, &found);
	if (error != 0)
		return;
	if (!found.connected || found.generation != head->generation)
		return;

	/* Lit again with the kept buffers; a failure latches it limited (logged). */
	(void)i915_head_light(display);
}

/*
 * Lights the head: its run once, and at a lower DP link while the link
 * does not train (a bounded number of times).  A head that does not come
 * up is latched limited.  Returns 0, or ENXIO.
 */
static int
i915_head_light(
	struct i915_display *display)
{
	struct i915_display_head *head;
	unsigned long irq;
	unsigned tries;
	int enable_rc;
	int fallback_error;
	int may;
	int error;

	head = &display->head;

	/* A head that may not be lit beside the resident output now (broken, latched, the resident output moved). */
	irq = spin_lock_irqsave(&display->device->irq_lock);

	may = i915_head_may_light_locked(display);

	spin_unlock_irqrestore(&display->device->irq_lock, irq);

	if (!may)
		return ENXIO;

	/* Lights it; a DP link that did not train is lowered and lit again. */
	error = ENXIO;
	for (tries = 0U; tries < I915_HEAD_LINK_TRIES; tries++) {
		enable_rc = I915_XD_PICTURE_OK;
		error = i915_head_light_once(display, &enable_rc);
		if (error == 0)
			break;

		/* Only a link that did not train, on a head stopped cleanly, is tried again. */
		if (enable_rc != I915_XD_PICTURE_LINK_NOT_TRAINED || head->broken)
			break;
		fallback_error = drv_i915_resident_link_fallback(display, &head->output, enable_rc);
		if (fallback_error != 0)
			break;
	}

	/* A head that did not come up is the limit until the topology moves. */
	if (error != 0) {
		drv_i915_head_limit(display, "it could not be lit");
		return ENXIO;
	}

	/* Succeeded: the head is lit; the DBUF state it left shows that both pipes have their share. */
	i915_head_log_dbuf(display, "lit");
	kern_logf("i915: display head: lit (connector %u, %s, %ux%u, pipe %c)\n",
	    head->connector,
	    i915_head_kind_name(head->output.kind),
	    head->buf[0].width,
	    head->buf[0].height,
	    (char)('A' + head->pipe));
	return 0;
}

/*
 * Lights the head once: its run context, its preflight, its configuration
 * beside the resident output's pipe, its buffers, then the check phase and
 * the enable commit on screen 1.  Whatever came up is stopped again when
 * the enable fails.  Returns 0, or ENXIO with the enable's result in
 * enable_rc.
 */
static int
i915_head_light_once(
	struct i915_display *display,
	int *enable_rc)
{
	struct i915_display_head *head;
	const struct i915_lcd_kernel_deps *d;
	struct i915_xd_picture_status status;
	unsigned resident_pipe;
	int params_error;
	int preflight_error;
	int config_error;
	const char *first_error;
	unsigned held;
	int buffers_error;
	int prepare_error;
	int begin_error;
	int stop_error;

	head = &display->head;
	d = display->rctx.lcd;

	/* The output's port and pipe. */
	params_error = drv_i915_resident_output_params(&head->output, &head->params);
	if (params_error != 0) {
		kern_logf("i915: display head: this driver does not light a %s beside the resident output\n", i915_head_kind_name(head->output.kind));
		return ENXIO;
	}

	/* The head's own run: its hooks, events, frame counter and power references. */
	drv_i915_resident_bind(display, &head->k, d, &head->params);

	/* The head's pipe and port must be idle, and the inputs complete. */
	preflight_error = drv_i915_resident_preflight(&head->k);
	config_error = 0;
	if (preflight_error == 0)
		config_error = drv_i915_resident_config(&head->k, &head->output, &head->cfg);
	if (preflight_error != 0 || config_error != 0) {
		kern_logf("i915: display head: not lit (preflight %d, inputs %d: nothing was written)\n", preflight_error, config_error);
		head->k.p = NULL;
		return ENXIO;
	}

	/* The display buffer's share is computed beside the resident output's pipe. */
	resident_pipe = drv_i915_display_output_pipe(&display->output);
	head->cfg.also_pipes = (uint8_t)(1U << resident_pipe);

	/* The two buffers, or the ones kept with the last picture over a window's end; the plane reads A first. */
	if (head->dormant) {
		head->dormant = 0;
	} else {
		buffers_error = i915_head_buffers(display);
		if (buffers_error != 0) {
			head->k.p = NULL;
			return ENXIO;
		}
	}

	head->front = 0U;
	head->cfg.fb_fourcc = head->buf[0].format;
	head->cfg.fb_modifier = head->buf[0].modifier;
	head->cfg.fb_width = head->buf[0].width;
	head->cfg.fb_height = head->buf[0].height;
	head->cfg.fb_pitch = head->buf[0].pitch;
	head->cfg.fb_surf = (uint32_t)head->buf[0].surf;

	/* The plan of the head's screen: nothing is written if it refuses (CDCLK, bandwidth, DBUF). */
	prepare_error = drv_i915_xd_picture_prepare(&display->xd.pictures, I915_XD_PICTURE_HEAD, &head->cfg, &head->k.ops);
	if (prepare_error != 0) {
		kern_logf("i915: display head: the configuration beside the resident output was refused (%d): nothing was written\n", prepare_error);
		i915_head_buffers_release(display, 1);
		head->k.p = NULL;
		return ENXIO;
	}

	/* The display takes buffer A. */
	begin_error = drv_i915_scanout_begin(&head->buf[0]);
	if (begin_error != 0) {
		i915_head_buffers_release(display, 1);
		head->k.p = NULL;
		return ENXIO;
	}

	/* Lights the head's screen. */
	*enable_rc = drv_i915_xd_picture_light(&display->xd.pictures, I915_XD_PICTURE_HEAD);
	if (*enable_rc == I915_XD_PICTURE_OK) {
		head->up = 1;
		return 0;
	}

	/* What the failed light left running is stopped; a pipe that never ran only gives its buffer back. */
	drv_i915_xd_picture_status(&display->xd.pictures, I915_XD_PICTURE_HEAD, &status);
	first_error = "-";
	if (status.first_error != NULL)
		first_error = status.first_error;
	kern_logf("i915: display head: the enable failed (rc=%d, crtc active %d, first error %s)\n",
	    *enable_rc,
	    status.crtc_active,
	    first_error);
	stop_error = 0;
	if (status.crtc_active) {
		head->up = 1;
		stop_error = i915_head_commit_stop(display);
		head->up = 0;
	} else {
		drv_i915_xd_picture_released(&display->xd.pictures, I915_XD_PICTURE_HEAD);
		drv_i915_scanout_end(&head->buf[0]);
	}

	/* The buffers back, or kept for ever when the stop was not confirmed. */
	i915_head_buffers_release(display, stop_error == 0);
	held = i915_head_held_refs(&head->k);
	if (stop_error != 0 || held != 0U)
		head->broken = 1;

	/* The head is not lit. */
	head->k.p = NULL;
	return ENXIO;
}

/* Creates, pins, clears and publishes the head's two buffers at its mode's size; 0, or ENOMEM / the scanout's error. */
static int
i915_head_buffers(
	struct i915_display *display)
{
	struct i915_display_head *head;
	const struct i915_lcd_kernel_deps *d;
	int window_error;
	int error;
	int i;

	head = &display->head;
	d = display->rctx.lcd;

	/* The display window of the GGTT; claimed earlier is fine. */
	window_error = drv_i915_gt_display_window_init(d->gm, I915_GT_DISPLAY_PAGES);
	if (window_error != 0 && window_error != EBUSY)
		return window_error;

	/* A at the head's mode's size, then B at A's. */
	error = drv_i915_scanout_create(d->gm, (uint32_t)head->output.mode.hdisplay, (uint32_t)head->output.mode.vdisplay, I915_FOURCC_XRGB8888, I915_MOD_LINEAR, &head->buf[0]);
	if (error == 0)
		error = drv_i915_scanout_pin(&head->buf[0], "head A");
	if (error == 0)
		error = drv_i915_scanout_create(d->gm, head->buf[0].width, head->buf[0].height, I915_FOURCC_XRGB8888, I915_MOD_LINEAR, &head->buf[1]);
	if (error == 0)
		error = drv_i915_scanout_pin(&head->buf[1], "head B");

	/* Gives back whatever was made, B first. */
	if (error != 0) {
		kern_logf("i915: display head: not lit (buffers rc=%d)\n", error);
		for (i = 1; i >= 0; i--) {
			(void)drv_i915_scanout_unpin(&head->buf[i]);
			(void)drv_i915_scanout_destroy(&head->buf[i]);
		}

		return error;
	}

	/* Black until the first frame, visible to the display. */
	for (i = 0; i < 2; i++) {
		kern_memset(head->buf[i].cpu, 0, head->buf[i].size);
		drv_i915_scanout_publish(&head->buf[i]);
	}

	/* Succeeded: both buffers are pinned and black. */
	return 0;
}

/*
 * Gives the head's buffers back once the display provably reads neither
 * (confirmed), or abandons them for ever when it may.
 */
static void
i915_head_buffers_release(
	struct i915_display *display,
	int confirmed)
{
	struct i915_display_head *head;
	struct i915_scanout *so;
	int error;
	int i;

	head = &display->head;

	/* Each buffer, B last as it was made last. */
	for (i = 0; i < 2; i++) {
		so = &head->buf[i];

		/* A buffer never made. */
		if (so->state == I915_SCANOUT_NONE)
			continue;

		/* The display may still read it: kept for ever. */
		if (!confirmed) {
			if (so->state >= I915_SCANOUT_PINNED && so->state != I915_SCANOUT_ABANDONED)
				drv_i915_scanout_abandon(so);
			continue;
		}

		/* Unpinned and destroyed. */
		error = drv_i915_scanout_unpin(so);
		if (error == 0)
			error = drv_i915_scanout_destroy(so);
		if (error != 0)
			kern_logf("i915: display head: XXX buffer %c could not be given back (%d)\n", (char)('A' + i), error);
	}
}

/*
 * Stops the lit head on its screen: an armed flip settles, the plane and
 * the crtc go off (the disable commit), and once that is confirmed the
 * display reads neither buffer any more.  Returns 0, or EIO when the stop
 * was not confirmed.
 */
static int
i915_head_commit_stop(
	struct i915_display *display)
{
	struct i915_display_head *head;
	uint32_t transconf;
	int plane_rc;
	int disable_rc;

	head = &display->head;

	/* An armed flip latches before the buffers change hands (it logs a failure). */
	(void)drv_i915_xd_picture_flip_settle(&display->xd.pictures, I915_XD_PICTURE_HEAD);

	/* The plane, then the dark. */
	plane_rc = drv_i915_xd_picture_plane_off(&display->xd.pictures, I915_XD_PICTURE_HEAD);
	disable_rc = drv_i915_xd_picture_dark(&display->xd.pictures, I915_XD_PICTURE_HEAD);

	/*
	 * The hardware's word, besides the commits': the head's
	 * transcoder is neither enabled nor running any more.
	 */
	transconf = drv_i915_read32(display->rctx.lcd->mmio, I915_HEAD_TRANSCONF_A + I915_HEAD_PIPE_STRIDE * head->pipe);

	/* A confirmed stop: the plane reads neither buffer any more. */
	if (plane_rc == I915_XD_PICTURE_OK &&
	    disable_rc == I915_XD_PICTURE_OK &&
	    (transconf & I915_HEAD_TRANSCONF_ON) == 0U) {
		drv_i915_xd_picture_released(&display->xd.pictures, I915_XD_PICTURE_HEAD);
		drv_i915_scanout_end(&head->buf[0]);
		return 0;
	}

	/* Not confirmed: everything stays as it is. */
	kern_logf("i915: display head: the stop was not confirmed (plane rc=%d, disable rc=%d, TRANSCONF 0x%08x)\n", plane_rc, disable_rc, transconf);
	return EIO;
}

/*
 * Maps the head's buffers into the presenting session's address space for
 * the GPU copy, once per address space (another one takes them over).
 * Returns 0, EIO for a buffer without backing, or the mapping's error.
 */
static int
i915_head_map(
	struct i915_display *display,
	struct i915_ppgtt *vm)
{
	struct i915_device *device;
	struct i915_display_head *head;
	struct i915_scanout *so;
	uint64_t va;
	uint64_t dma;
	unsigned i;
	unsigned page;
	int error;

	device = display->device;
	head = &display->head;

	/* The buffers are in this address space already. */
	if (head->map_vm == vm)
		return 0;

	/* Another address space gives them up first. */
	i915_head_unmap(display);

	/* Maps each buffer under the device mutex, which guards the address spaces. */
	mutex_lock(&device->mutex);

	for (i = 0U; i < 2U; i++) {
		/* The buffer must be backed. */
		so = &head->buf[i];
		if (so->obj == NULL) {
			mutex_unlock(&device->mutex);
			return EIO;
		}

		/* A range as large as the buffer's pages, every page uncached, in order. */
		error = drv_i915_ppgtt_va_alloc(vm, (uint64_t)so->obj->pages * I915_GT_PAGE_BYTES, &va);
		for (page = 0U; error == 0 && page < so->obj->pages; page++) {
			error = drv_i915_gt_object_page_dma(so->obj, page, &dma);
			if (error != 0) {
				error = EIO;
				break;
			}

			error = drv_i915_ppgtt_insert_uncached(vm, va + (uint64_t)page * I915_GT_PAGE_BYTES, dma, 1U);
		}

		if (error != 0) {
			mutex_unlock(&device->mutex);
			return error;
		}

		head->map_va[i] = va;
		head->map_pages[i] = so->obj->pages;
	}

	/* The address space holds the buffers from here on. */
	head->map_vm = vm;

	mutex_unlock(&device->mutex);

	/* Succeeded: the GPU can write either buffer. */
	return 0;
}

/* Unmaps the head's buffers from the address space that holds them. */
static void
i915_head_unmap(
	struct i915_display *display)
{
	struct i915_device *device;
	struct i915_display_head *head;
	unsigned i;

	device = display->device;
	head = &display->head;

	/* No address space holds them. */
	if (head->map_vm == NULL)
		return;

	/* Clears both ranges under the device mutex. */
	mutex_lock(&device->mutex);

	for (i = 0U; i < 2U; i++)
		drv_i915_ppgtt_clear(head->map_vm, head->map_va[i], head->map_pages[i]);

	mutex_unlock(&device->mutex);

	/* No address space holds them from here on. */
	head->map_vm = NULL;
}

/*
 * Chooses the head buffer a frame is drawn into, and whether the head must
 * then flip to it: the one it does not show, or, while an armed flip has
 * not latched, the one that flip shows (the newest frame wins).
 */
static struct i915_scanout *
i915_head_target(
	struct i915_display *display,
	int *flip)
{
	struct i915_display_head *head;
	int idle;

	head = &display->head;

	/* Whether a flip is still armed on the head's screen. */
	idle = drv_i915_xd_picture_flip_poll(&display->xd.pictures, I915_XD_PICTURE_HEAD);

	/* An armed flip keeps the frame in the buffer it shows next. */
	*flip = 1;
	if (!idle) {
		*flip = 0;
		return &head->buf[head->front];
	}

	/* Succeeded: the buffer the head does not show. */
	return &head->buf[head->front ^ 1U];
}

/* Arms the head's flip to a buffer; it latches at the head's next vblank.  0, or EIO. */
static int
i915_head_flip(
	struct i915_display *display,
	struct i915_scanout *to)
{
	struct i915_display_head *head;
	struct i915_xd_picture_flip result;
	int error;

	head = &display->head;

	/* Arms the flip on the head's screen. */
	kern_memset(&result, 0, sizeof(result));
	error = drv_i915_xd_picture_flip_nowait(&display->xd.pictures, I915_XD_PICTURE_HEAD, (uint32_t)to->surf, &result);

	/* Says why the flip failed. */
	if (error != 0 || result.result != I915_XD_PICTURE_FLIP_ARMED) {
		kern_logf("i915: display head: flip to 0x%08x failed: rc=%d result=%d\n", (uint32_t)to->surf, error, result.result);
		return EIO;
	}

	/* Succeeded: the buffer is the front one from the next vblank. */
	head->front ^= 1U;
	return 0;
}

/*
 * Copies a CPU frame into a head buffer: scaled by the largest whole
 * factor that fits, centred, the rest left as it was (black on a fresh
 * head).  The scanout is XRGB8888; an RGBA frame swaps R and B.
 */
static void
i915_head_copy(
	struct i915_scanout *back,
	const struct i915_worker_present *frame)
{
	const uint32_t *src;
	uint32_t *row;
	uint32_t scale;
	uint32_t x0;
	uint32_t y0;
	uint32_t x;
	uint32_t y;
	uint32_t pixel;

	/* The largest whole factor that fits both directions (the caller checked the frame fits). */
	scale = back->width / frame->width;
	if (back->height / frame->height < scale)
		scale = back->height / frame->height;

	/* Centres the scaled frame. */
	x0 = (back->width - frame->width * scale) / 2U;
	y0 = (back->height - frame->height * scale) / 2U;

	/* Copies every scaled row. */
	for (y = 0U; y < frame->height * scale; y++) {
		src = (const uint32_t *)(const void *)(frame->pixels + (uint64_t)(y / scale) * frame->stride);
		row = back->cpu + (uint64_t)(y0 + y) * (back->pitch / 4U) + x0;

		/* Copies every scaled pixel of the row. */
		for (x = 0U; x < frame->width * scale; x++) {
			pixel = src[x / scale];

			/* RGBA swaps R and B; BGRA is the scanout's order. */
			if (!frame->bgra)
				pixel = ((pixel & 0xffU) << 16) | (pixel & 0xff00U) | ((pixel >> 16) & 0xffU);

			row[x] = pixel & 0x00ffffffU;
		}
	}
}

/* Gives the head's kind of a hotplug connector's kind. */
static enum i915_head_kind
i915_head_kind_of(
	unsigned hpd_kind)
{
	/* Each kind the hotplug path tells. */
	switch (hpd_kind) {
	case I915_HPD_OUTPUT_EDP:
		return I915_HEAD_KIND_PANEL;
	case I915_HPD_OUTPUT_HDMI:
		return I915_HEAD_KIND_HDMI;
	case I915_HPD_OUTPUT_DP:
		return I915_HEAD_KIND_DP_EXT;
	default:
		break;
	}

	/* Succeeded: a kind the head does not light. */
	return I915_HEAD_KIND_OTHER;
}

/* Names an output's kind for the log. */
static const char *
i915_head_kind_name(
	enum i915_output_kind kind)
{
	/* Each kind. */
	switch (kind) {
	case I915_OUTPUT_KIND_PANEL:
		return "eDP panel";
	case I915_OUTPUT_KIND_HDMI:
		return "HDMI";
	case I915_OUTPUT_KIND_DP_EXT:
		return "DP";
	default:
		break;
	}

	/* Succeeded: another kind. */
	return "other";
}

/* Tells whether a session holds the resident output's lease. */
static int
i915_head_resident_leased(
	struct i915_display *display)
{
	int leased;

	/* The resident lease's holder, under its mutex. */
	drv_i915_present_lease_init(display);
	mutex_lock(&display->rd.mutex);

	leased = 0;
	if (display->rd.owner != NULL)
		leased = 1;

	mutex_unlock(&display->rd.mutex);

	/* Succeeded: whether it is held. */
	return leased;
}

/* Counts the power references a run still holds. */
static unsigned
i915_head_held_refs(
	const struct i915_lcd_kernel *k)
{
	unsigned held;
	unsigned domain;

	/* Every domain's count. */
	held = 0U;
	for (domain = 0U; domain < I915_LCD_POWER_SLOTS; domain++)
		held += (unsigned)k->power_refs[domain];

	/* Succeeded: the references held. */
	return held;
}

/*
 * Tells whether the claimed head may be lit beside the resident output:
 * claimed, not broken, not latched limited for its generation, beside the
 * built-in panel on another pipe (the resident output may have
 * moved since the claim).  The caller holds the device IRQ lock.
 */
static int
i915_head_may_light_locked(
	struct i915_display *display)
{
	struct i915_display_head *head;
	unsigned resident_pipe;

	head = &display->head;

	/* A head that is not claimed, or whose earlier stop was not confirmed. */
	if (!head->claimed || head->broken)
		return 0;

	/* A head latched limited for the generation it was claimed with. */
	if (head->limited &&
	    head->limited_connector == head->connector &&
	    head->limited_generation == head->generation)
		return 0;

	/* Only beside the built-in panel. */
	if (display->output.kind != I915_OUTPUT_KIND_PANEL || display->output.none)
		return 0;

	/* On another pipe than the resident output's. */
	resident_pipe = drv_i915_display_output_pipe(&display->output);
	if (resident_pipe == head->pipe)
		return 0;

	/* Succeeded: the head may be lit. */
	return 1;
}

/*
 * Logs the device's DBUF state as the head's screen last committed it:
 * the active pipes (the reserved one included), the slices and the MBUS
 * joining, so that a run shows the head's stop changed neither under the
 * resident output.
 */
static void
i915_head_log_dbuf(
	struct i915_display *display,
	const char *when)
{
	struct i915_xd_picture_status status;

	/* The head's screen's view of the DBUF. */
	drv_i915_xd_picture_status(&display->xd.pictures, I915_XD_PICTURE_HEAD, &status);

	kern_logf("i915: display head: DBUF %s: pipes 0x%x slices 0x%x MBUS joined %d\n",
	    when,
	    status.dbuf_active_pipes_now,
	    (unsigned)status.dbuf_slices_now,
	    status.mbus_joined_now);
}

/*
 * Decides what a claim of a connector that is not the resident output
 * comes to, with the reason of a refusal for the log (an empty string
 * otherwise).  Without the resident output's lease the resident output
 * moves to the connector; with it, the connector becomes the head beside
 * the built-in panel, one head at a time; anything else is the limit of
 * the outputs shown at once (a limit, not a failure).
 */
static enum i915_head_claim_way
i915_head_claim_way(
	const struct i915_head_claim_facts *facts,
	const char **reason)
{
	/* Nothing to say yet. */
	*reason = "";

	/*
	 * No resident lease held: the resident output moves to the connector,
	 * unless a head is still claimed, whose pipe or connector the move
	 * could take.
	 */
	if (!facts->resident_leased) {
		if (facts->head_claimed) {
			*reason = "the resident output does not move while a second output is shown";
			return I915_HEAD_CLAIM_LIMIT;
		}

		/* Succeeded: a move. */
		return I915_HEAD_CLAIM_MOVE;
	}

	/* One head at a time. */
	if (facts->head_claimed) {
		*reason = "a second output is shown already";
		return I915_HEAD_CLAIM_LIMIT;
	}

	/* A head whose stop was not confirmed may still be read by the display: none is lit again. */
	if (facts->head_broken) {
		*reason = "an earlier second output was not stopped cleanly";
		return I915_HEAD_CLAIM_LIMIT;
	}

	/* A connector that could not be lit stays limited until the topology changes. */
	if (facts->limited) {
		*reason = "the connector could not be lit beside the resident output (until the next hotplug)";
		return I915_HEAD_CLAIM_LIMIT;
	}

	/* The head is lit beside the built-in panel only. */
	if (facts->resident_kind != I915_HEAD_KIND_PANEL) {
		*reason = "a second output is lit beside the built-in panel only";
		return I915_HEAD_CLAIM_LIMIT;
	}

	/* The head is an HDMI or an external DisplayPort display. */
	if (facts->kind != I915_HEAD_KIND_HDMI && facts->kind != I915_HEAD_KIND_DP_EXT) {
		*reason = "a second output of this kind is not lit";
		return I915_HEAD_CLAIM_LIMIT;
	}

	/* Succeeded: the connector becomes the head. */
	return I915_HEAD_CLAIM_HEAD;
}
