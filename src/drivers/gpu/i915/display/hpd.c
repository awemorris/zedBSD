/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display's connectors and their hotplug (see hpd.h).
 *
 * The pins, the pulses and the connectors' detection are xd-hpd.c's; this
 * file gives them the kernel's side: the interrupt entry and its gate, the
 * locks, the works that take the pulses, detect the connectors, look
 * again at a Type-C link and arm the pins again after a storm, the hooks
 * through which the detection reaches the power domains and the Type-C
 * and external DisplayPort code, and the topology the display sessions
 * read.
 */

#include "internal.h"
#include "hpd.h"
#include "xd-dp.h"
#include "dp-ext-kern.h"
#include "output.h"
#include "present.h"
#include "tc-port.h"
#include "xd-hpd.h"

#include "../workqueue.h"

#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/poll.h>
#include <kern/sched.h>

#include <uapi/errno.h>
#include <stddef.h>

/*
 * The VBT's device type bit of a connector inside the machine: a
 * DisplayPort output on port A with it is the built-in panel.
 */
#define I915_HPD_DEVICE_TYPE_INTERNAL	0x1000U

/* How long a Type-C link may wait for its DisplayPort partner before its output stops, in milliseconds. */
#define I915_HPD_TC_RESET_MS		2000U

/* How long the stop waits for a running work, in seconds, and how often it looks for interrupts still inside. */
#define I915_HPD_SYNC_SECONDS		5U
#define I915_HPD_DRAIN_SPINS		1000000U

/* A connector's preferred mode as the topology took it from its EDID (0 when none). */
struct i915_hpd_mode {
	uint32_t width;
	uint32_t height;
	uint32_t refresh_millihz;
	uint32_t width_mm;
	uint32_t height_mm;
};

struct i915_hpd_world;

/* The look at one Type-C port's link two seconds after it lost its DisplayPort partner. */
struct i915_hpd_tc_reset {
	struct i915_hpd_world *world;
	unsigned tc;
	struct i915_delayed_work work;
};

/*
 * The hotplug of a display.
 *
 * lock protects the pins, the pulses and the events (xh's), which the
 * interrupt and the works share; probe_lock serializes the detection of
 * the connectors (the works and the start's probes); topology_lock
 * protects the topology the sessions read.  The world lives as long as
 * the display; the queues and the works exist between the start and the
 * stop.
 */
struct i915_hpd_world {
	struct i915_display *display;
	struct i915_xd_hpd xh;
	struct i915_xd_hpd_env env;
	struct i915_xd_tc_ports *tc;
	struct spinlock lock;
	struct mutex probe_lock;
	struct spinlock topology_lock;
	uint64_t topology_sequence;
	uint64_t generation[I915_XD_HPD_CONNECTORS];
	int connected[I915_XD_HPD_CONNECTORS];
	struct i915_hpd_mode mode[I915_XD_HPD_CONNECTORS];
	struct i915_workqueue wq;
	struct i915_timer_queue tq;
	struct i915_work pulse_work;
	struct i915_delayed_work detect_work;
	struct i915_delayed_work rearm_work;
	struct i915_hpd_tc_reset tc_reset[I915_XD_TC_PORTS];
	int armed;
	int started;
	int live;
	unsigned inflight;
	unsigned irq_entries;
	unsigned irq_dropped;
	unsigned pulse_works;
	unsigned detections;
	unsigned rearms;
};

static void i915_hpd_add_connectors(struct i915_hpd_world *world);
static void i915_hpd_irq(struct i915_hpd_world *world, uint32_t pins, uint32_t longs);
static void i915_hpd_queue(struct i915_hpd_world *world, unsigned queue);
static void i915_hpd_pulse_work(void *context);
static void i915_hpd_detect_work(void *context);
static void i915_hpd_rearm_work(void *context);
static void i915_hpd_tc_reset_work(void *context);
static void i915_hpd_detect_one(struct i915_hpd_world *world, unsigned index, int event, uint32_t *changed, uint32_t *retry);
static void i915_hpd_topology_update(struct i915_hpd_world *world, unsigned idx, int publish);
static void i915_hpd_requeue(struct i915_hpd_world *world, struct i915_delayed_work *work, unsigned delay_ms);
static uint64_t i915_hpd_now_ms(void);
static uint64_t i915_hpd_deadline(void);
static int i915_hpd_power(void *ctx, unsigned what, int on);
static int i915_hpd_tc_live(void *ctx, unsigned ddi);
static int i915_hpd_dp_probe(void *ctx, unsigned ddi, uint8_t *edid, size_t size, unsigned *bytes);
static int i915_hpd_dp_pulse(void *ctx, unsigned ddi);
static void i915_hpd_dp_reset(void *ctx, unsigned ddi);
static void i915_hpd_link(void *ctx, unsigned ddi, int hdmi);

/*
 * Allocates the display's hotplug, with no connector and the topology's
 * first snapshot.  Returns 0, or ENOMEM.
 */
int
drv_i915_hpd_world_create(
	struct i915_display *display)
{
	struct i915_hpd_world *world;
	unsigned index;
	int error;

	/* The world, zeroed. */
	world = kern_calloc(1U, sizeof(*world));
	if (world == NULL)
		return ENOMEM;

	/* Its locks. */
	world->display = display;
	spin_init(&world->lock, LOCK_RANK_DEVICE, "i915 hotplug");
	spin_init(&world->topology_lock, LOCK_RANK_DEVICE, "i915 display topology");
	error = mutex_init(&world->probe_lock, LOCK_RANK_DEVICE, "i915 hotplug probe");
	if (error != 0) {
		kern_free(world);
		return ENOMEM;
	}

	/* The topology's first snapshot, and every connector's first generation. */
	world->topology_sequence = 1U;
	for (index = 0U; index < I915_XD_HPD_CONNECTORS; index++)
		world->generation[index] = 1U;

	/* The hooks of the detection. */
	world->env.ctx = world;
	world->env.power = i915_hpd_power;
	world->env.tc_live = i915_hpd_tc_live;
	world->env.dp_probe = i915_hpd_dp_probe;
	world->env.dp_pulse = i915_hpd_dp_pulse;
	world->env.dp_reset = i915_hpd_dp_reset;
	world->env.link = i915_hpd_link;

	/* Succeeded: the display has its hotplug. */
	display->hpd_world = world;
	return 0;
}

/*
 * Frees the display's hotplug, which must have been stopped.
 */
void
drv_i915_hpd_world_destroy(
	struct i915_display *display)
{
	/* A display that never had one has nothing to free. */
	if (display->hpd_world == NULL)
		return;

	/* Frees it and forgets it. */
	kern_free(display->hpd_world);
	display->hpd_world = NULL;
}

/*
 * Makes the connectors of the display's ports (once) and arms their pins.
 *
 * interrupts says whether the device takes interrupts yet: the south
 * mask is written only then.  A display the xd code does not describe has
 * no connectors.
 */
void
drv_i915_hpd_arm(
	struct i915_display *display,
	int interrupts)
{
	struct i915_hpd_world *world;
	struct i915_xd_hpd_words *words;
	unsigned long irq;

	/* Nothing without the hotplug or a description of the display. */
	world = display->hpd_world;
	if (world == NULL || !display->xd.chosen)
		return;

	/* The connectors, made once from the ports. */
	if (!world->armed) {
		drv_i915_xd_hpd_init(&world->xh, display->xd.choice.platform, display->pch.type);
		i915_hpd_add_connectors(world);
		world->armed = 1;
	}

	/* Arms the pins under the lock the interrupt takes. */
	irq = spin_lock_irqsave(&world->lock);

	drv_i915_xd_hpd_arm(&world->xh, &display->xd.io, interrupts);

	spin_unlock_irqrestore(&world->lock, irq);

	/* Logs what the hotplug registers hold. */
	words = &world->xh.words;
	kern_logf("i915: hpd arm: connectors=%u | north used=0x%x enabled=0x%x IMR=0x%08x TC_CTL=0x%08x TBT_CTL=0x%08x | south used=0x%x enabled=0x%x filter=0x%x SDEIMR=0x%08x%s DDI_CTL=0x%08x TC_CTL=0x%08x\n",
	    world->xh.count,
	    words->north_used,
	    words->north_enabled,
	    words->de_imr,
	    words->tc_ctl,
	    words->tbt_ctl,
	    words->south_used,
	    words->south_enabled,
	    words->filter,
	    words->sde_imr,
	    words->sde_imr_written ? "" : " (not written: interrupts off)",
	    words->ddi_ctl,
	    words->tc_south_ctl);
}

/*
 * Starts the hotplug: its queues and works, then the interrupt entry.
 * Returns 0, EINVAL for a hotplug that is not armed or runs already, or
 * ENOMEM.
 */
int
drv_i915_hpd_start(
	struct i915_display *display)
{
	struct i915_hpd_world *world;
	const char *hdmi;
	unsigned index;
	unsigned tc;
	int error;

	/* Refuses a hotplug without connectors, and a second start. */
	world = display->hpd_world;
	if (world == NULL ||
	    !world->armed ||
	    world->started)
		return EINVAL;

	/* The Type-C ports the detection asks the live state of. */
	world->tc = drv_i915_tc_port_ports(display);

	/* The work queue and the timer queue its delayed works are armed on. */
	error = drv_i915_workqueue_create(&world->wq, "i915-hotplug");
	if (error != 0)
		return ENOMEM;

	/* The timer queue of the delayed works. */
	error = drv_i915_timer_queue_create(&world->tq, &world->wq, "i915-hotplug-timer");
	if (error != 0) {
		drv_i915_workqueue_destroy(&world->wq);
		return ENOMEM;
	}

	/* The works. */
	drv_i915_work_init(&world->pulse_work, i915_hpd_pulse_work, world);
	drv_i915_delayed_work_init(&world->detect_work, i915_hpd_detect_work, world);
	drv_i915_delayed_work_init(&world->rearm_work, i915_hpd_rearm_work, world);
	for (tc = 0U; tc < I915_XD_TC_PORTS; tc++) {
		world->tc_reset[tc].world = world;
		world->tc_reset[tc].tc = tc;
		drv_i915_delayed_work_init(&world->tc_reset[tc].work, i915_hpd_tc_reset_work, &world->tc_reset[tc]);
	}

	/* Opens the interrupt entry: a hotplug interrupt is taken from now on. */
	world->started = 1;
	__atomic_store_n(&world->live, 1, __ATOMIC_SEQ_CST);

	/* Logs the connectors. */
	hdmi = "-";
	for (index = 0U; index < world->xh.count; index++) {
		if (world->xh.connector[index].kind == I915_XD_HPD_HDMI) {
			hdmi = world->xh.connector[index].name;
			break;
		}
	}

	/* Logs the start. */
	kern_logf("i915: hpd start: connectors=%u hdmi=%s storm limit=%d\n", world->xh.count, hdmi, I915_XD_HPD_STORM_LIMIT);

	/* Succeeded: the hotplug runs. */
	return 0;
}

/*
 * Takes the hotplug part of a south display interrupt: acknowledges the
 * pins' pulses and sorts them.  Runs in the interrupt.
 */
void
drv_i915_hpd_pch_irq(
	struct i915_display *display,
	uint32_t sde_iir)
{
	struct i915_hpd_world *world;
	uint32_t pins;
	uint32_t longs;
	unsigned long irq;
	int live;

	/* A display without a hotplug takes nothing. */
	world = display->hpd_world;
	if (world == NULL)
		return;

	/* Counts the entry before looking at the gate, so the stop can wait for it. */
	__atomic_add_fetch(&world->inflight, 1U, __ATOMIC_SEQ_CST);
	live = __atomic_load_n(&world->live, __ATOMIC_SEQ_CST);
	if (!live) {
		world->irq_dropped++;
		__atomic_sub_fetch(&world->inflight, 1U, __ATOMIC_SEQ_CST);
		return;
	}

	/* Acknowledges the pulses and sorts them. */
	irq = spin_lock_irqsave(&world->lock);

	world->irq_entries++;
	drv_i915_xd_hpd_south_irq(&world->xh, &display->xd.io, sde_iir, &pins, &longs);

	spin_unlock_irqrestore(&world->lock, irq);

	/* Hands the pulses to the hotplug. */
	i915_hpd_irq(world, pins, longs);

	/* The entry has left. */
	__atomic_sub_fetch(&world->inflight, 1U, __ATOMIC_SEQ_CST);
}

/*
 * Takes a north display hotplug interrupt (the Type-C and Thunderbolt
 * pins): acknowledges the pins' pulses and sorts them.  Runs in the
 * interrupt.
 */
void
drv_i915_hpd_de_irq(
	struct i915_display *display,
	uint32_t de_hpd_iir)
{
	struct i915_hpd_world *world;
	uint32_t pins;
	uint32_t longs;
	unsigned long irq;
	int live;

	/* A display without a hotplug takes nothing. */
	world = display->hpd_world;
	if (world == NULL)
		return;

	/* Counts the entry before looking at the gate, so the stop can wait for it. */
	__atomic_add_fetch(&world->inflight, 1U, __ATOMIC_SEQ_CST);
	live = __atomic_load_n(&world->live, __ATOMIC_SEQ_CST);
	if (!live) {
		world->irq_dropped++;
		__atomic_sub_fetch(&world->inflight, 1U, __ATOMIC_SEQ_CST);
		return;
	}

	/* Acknowledges the pulses and sorts them. */
	irq = spin_lock_irqsave(&world->lock);

	world->irq_entries++;
	drv_i915_xd_hpd_north_irq(&world->xh, &display->xd.io, de_hpd_iir, &pins, &longs);

	spin_unlock_irqrestore(&world->lock, irq);

	/* Logs the pins and hands them to the hotplug. */
	kern_logf("i915: hpd north iir=0x%08x pins=0x%08x long=0x%08x\n", de_hpd_iir, pins, longs);
	i915_hpd_irq(world, pins, longs);

	/* The entry has left. */
	__atomic_sub_fetch(&world->inflight, 1U, __ATOMIC_SEQ_CST);
}

/*
 * Stops the hotplug: closes the interrupt entry, waits for the entries
 * inside, cancels the works and destroys their queues.
 */
void
drv_i915_hpd_stop(
	struct i915_display *display)
{
	struct i915_hpd_world *world;
	unsigned long irq;
	uint64_t deadline;
	unsigned inflight;
	unsigned spins;
	unsigned tc;

	/* A hotplug that does not run has nothing to stop. */
	world = display->hpd_world;
	if (world == NULL || !world->started)
		return;

	/* Closes the interrupt entry: a new interrupt is dropped. */
	__atomic_store_n(&world->live, 0, __ATOMIC_SEQ_CST);

	/* Waits, boundedly, for the entries already inside. */
	for (spins = 0U; spins < I915_HPD_DRAIN_SPINS; spins++) {
		inflight = __atomic_load_n(&world->inflight, __ATOMIC_SEQ_CST);
		if (inflight == 0U)
			break;

		/* Waits a moment before looking again. */
		__asm__ volatile("pause");
	}

	/* Forgets every pending pulse, event and retry. */
	irq = spin_lock_irqsave(&world->lock);

	world->xh.long_ports = 0U;
	world->xh.short_ports = 0U;
	world->xh.events = 0U;
	world->xh.retries = 0U;

	spin_unlock_irqrestore(&world->lock, irq);

	/* Cancels every work and waits for a running one. */
	deadline = i915_hpd_deadline();
	(void)drv_i915_cancel_work_sync(&world->wq, &world->pulse_work, deadline);
	(void)drv_i915_delayed_cancel_sync(&world->tq, &world->detect_work, deadline);
	(void)drv_i915_delayed_cancel_sync(&world->tq, &world->rearm_work, deadline);
	for (tc = 0U; tc < I915_XD_TC_PORTS; tc++)
		(void)drv_i915_delayed_cancel_sync(&world->tq, &world->tc_reset[tc].work, deadline);

	/* Destroys the queues. */
	drv_i915_timer_queue_destroy(&world->tq);
	drv_i915_workqueue_destroy(&world->wq);
	world->started = 0;

	/* Logs what the hotplug handled. */
	kern_logf("i915: hpd stop: interrupts=%u dropped=%u pulse works=%u detections=%u storms=%u rearms=%u\n",
	    world->irq_entries,
	    world->irq_dropped,
	    world->pulse_works,
	    world->detections,
	    world->xh.storms,
	    world->rearms);
}

/*
 * Quiesces the hotplug for a suspend: closes the interrupt entry, waits
 * for the entries inside, forgets every pending pulse, event and retry,
 * and cancels the works (a running detection is waited for).  The queues
 * stay for the resume.
 */
void
drv_i915_hpd_suspend(
	struct i915_display *display)
{
	struct i915_hpd_world *world;
	unsigned long irq;
	uint64_t deadline;
	unsigned inflight;
	unsigned spins;
	unsigned tc;

	/* A hotplug that does not run has nothing to quiesce. */
	world = display->hpd_world;
	if (world == NULL || !world->started)
		return;

	/* Closes the interrupt entry: a new interrupt is dropped. */
	__atomic_store_n(&world->live, 0, __ATOMIC_SEQ_CST);

	/* Waits, boundedly, for the entries already inside. */
	for (spins = 0U; spins < I915_HPD_DRAIN_SPINS; spins++) {
		inflight = __atomic_load_n(&world->inflight, __ATOMIC_SEQ_CST);
		if (inflight == 0U)
			break;

		/* Waits a moment before looking again. */
		__asm__ volatile("pause");
	}

	/* Forgets every pending pulse, event and retry. */
	irq = spin_lock_irqsave(&world->lock);

	world->xh.long_ports = 0U;
	world->xh.short_ports = 0U;
	world->xh.events = 0U;
	world->xh.retries = 0U;

	spin_unlock_irqrestore(&world->lock, irq);

	/* Cancels every work and waits for a running one. */
	deadline = i915_hpd_deadline();
	(void)drv_i915_cancel_work_sync(&world->wq, &world->pulse_work, deadline);
	(void)drv_i915_delayed_cancel_sync(&world->tq, &world->detect_work, deadline);
	(void)drv_i915_delayed_cancel_sync(&world->tq, &world->rearm_work, deadline);
	for (tc = 0U; tc < I915_XD_TC_PORTS; tc++)
		(void)drv_i915_delayed_cancel_sync(&world->tq, &world->tc_reset[tc].work, deadline);

	/* Logs what the hotplug handled before the sleep. */
	kern_logf("i915: hpd suspend: interrupts=%u detections=%u\n", world->irq_entries, world->detections);
}

/*
 * Brings the hotplug back after a resume, once the interrupts are back:
 * arms the pins again (the hotplug registers were lost with the display
 * core), opens the interrupt entry, and asks for a detection of every
 * connector, so one plugged or unplugged while the machine slept is seen.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_driver.c
 * (intel_display_driver_resume), intel_hotplug.c (intel_hpd_init,
 * intel_hpd_poll_disable).
 */
void
drv_i915_hpd_resume(
	struct i915_display *display)
{
	struct i915_hpd_world *world;
	unsigned long irq;
	unsigned index;
	uint32_t pins;

	/* A hotplug that does not run has nothing to bring back. */
	world = display->hpd_world;
	if (world == NULL || !world->started)
		return;

	/* Arms the pins again, with the interrupts on. */
	drv_i915_hpd_arm(display, 1);

	/* Every connector's pin, which the detection looks at once. */
	pins = 0U;
	for (index = 0U; index < world->xh.count; index++) {
		if (world->xh.connector[index].pin != I915_XD_HPD_PIN_NONE)
			pins |= 1U << world->xh.connector[index].pin;
	}

	/* Gives the detection an event on every pin. */
	irq = spin_lock_irqsave(&world->lock);

	world->xh.events |= pins;

	spin_unlock_irqrestore(&world->lock, irq);

	/* Opens the interrupt entry: a hotplug interrupt is taken from now on. */
	__atomic_store_n(&world->live, 1, __ATOMIC_SEQ_CST);

	/* Looks at every connector at once. */
	(void)drv_i915_delayed_queue(&world->tq, &world->detect_work, 0U);
	kern_logf("i915: hpd resume: pins=0x%x looked at again\n", pins);
}

/*
 * Detects a connector once, as its first probe does (it reads the sink's
 * EDID).  Returns its state (1 connected, 2 disconnected, 3 unknown), or
 * EINVAL for a hotplug that does not run or a connector it does not have.
 */
int
drv_i915_hpd_probe_connector(
	struct i915_display *display,
	unsigned idx)
{
	struct i915_hpd_world *world;
	struct i915_xd_hpd_connector *connector;
	int status;

	/* Refuses a hotplug that does not run and a connector it does not have. */
	world = display->hpd_world;
	if (world == NULL ||
	    !world->started ||
	    idx >= world->xh.count)
		return EINVAL;

	/* Detects the connector, serialized with the works. */
	mutex_lock(&world->probe_lock);

	status = drv_i915_xd_hpd_probe(&world->xh, &display->xd.io, &world->env, idx);
	connector = &world->xh.connector[idx];
	kern_logf("i915: hpd probe %s: %s (live %d, EDID %u bytes)\n",
	    connector->name,
	    drv_i915_xd_hpd_status_name(status),
	    connector->live,
	    connector->edid_size);

	mutex_unlock(&world->probe_lock);

	/* Succeeded: reports the connector's state. */
	return status;
}

/*
 * Reports whether the hotplug runs, how many connectors it has and which
 * is the first HDMI one.
 */
void
drv_i915_hpd_summary(
	struct i915_display *display,
	struct i915_hpd_summary *s)
{
	struct i915_hpd_world *world;
	unsigned index;

	/* A display without a hotplug has nothing. */
	s->started = 0;
	s->num_connectors = 0U;
	s->hdmi_connector = -1;
	world = display->hpd_world;
	if (world == NULL)
		return;

	/* Whether it runs, its connectors, and the first HDMI one. */
	s->started = world->started;
	s->num_connectors = world->xh.count;
	for (index = 0U; index < world->xh.count; index++) {
		if (world->xh.connector[index].kind == I915_XD_HPD_HDMI) {
			s->hdmi_connector = (int)index;
			break;
		}
	}
}

/*
 * Reports the display topology's sequence: 1, the first snapshot, until a
 * connector is connected or disconnected.  No hardware access and no
 * wait: the display events operation reads it in any context.
 */
uint64_t
drv_i915_hpd_topology_sequence(
	struct i915_display *display)
{
	struct i915_hpd_world *world;
	unsigned long irq;
	uint64_t sequence;

	/* A display without a hotplug has only its first snapshot. */
	world = display->hpd_world;
	if (world == NULL)
		return 1U;

	/* The sequence now. */
	irq = spin_lock_irqsave(&world->topology_lock);

	sequence = world->topology_sequence;

	spin_unlock_irqrestore(&world->topology_lock, irq);

	/* Succeeded: the sequence. */
	return sequence;
}

/*
 * Tells the display sessions that the inventory changed without a
 * connector's change: the resident output moved back to the firmware's,
 * so index 0 names another display.
 */
void
drv_i915_hpd_topology_touch(
	struct i915_display *display)
{
	struct i915_hpd_world *world;
	unsigned long irq;

	/* A display without a hotplug has no sessions to tell. */
	world = display->hpd_world;
	if (world == NULL)
		return;

	/* A new sequence. */
	irq = spin_lock_irqsave(&world->topology_lock);

	world->topology_sequence++;

	spin_unlock_irqrestore(&world->topology_lock, irq);

	/* A display session waiting for the change hears of it. */
	poll_notify();
}

/*
 * Describes one connector for the display inventory: its kind, port,
 * name, and, as the topology took them, whether a sink is connected, its
 * generation and its sink's preferred mode.  0, or ENOENT for a connector
 * the hotplug does not have.
 */
int
drv_i915_hpd_output(
	struct i915_display *display,
	unsigned idx,
	struct i915_hpd_output *output)
{
	struct i915_hpd_world *world;
	const struct i915_xd_hpd_connector *connector;
	unsigned long irq;

	/* A connector of the hotplug. */
	world = display->hpd_world;
	if (world == NULL || idx >= world->xh.count)
		return ENOENT;

	/* Its kind, port and name. */
	connector = &world->xh.connector[idx];
	output->kind = I915_HPD_OUTPUT_OTHER;
	if (connector->kind == I915_XD_HPD_EDP) {
		output->kind = I915_HPD_OUTPUT_EDP;
	} else if (connector->kind == I915_XD_HPD_HDMI) {
		output->kind = I915_HPD_OUTPUT_HDMI;
	} else if (connector->kind == I915_XD_HPD_DP) {
		output->kind = I915_HPD_OUTPUT_DP;
	}

	/* The output's port and name. */
	output->port = (int)connector->ddi;
	output->name = connector->name;

	/* Its connection, generation and preferred mode as the topology took them. */
	irq = spin_lock_irqsave(&world->topology_lock);

	output->connected = world->connected[idx];
	output->generation = world->generation[idx];
	output->width = world->mode[idx].width;
	output->height = world->mode[idx].height;
	output->refresh_millihz = world->mode[idx].refresh_millihz;
	output->width_mm = world->mode[idx].width_mm;
	output->height_mm = world->mode[idx].height_mm;

	spin_unlock_irqrestore(&world->topology_lock, irq);

	/* Succeeded: the connector is described. */
	return 0;
}

/*
 * Reports how many connectors the hotplug has (0 before they are made).
 */
unsigned
drv_i915_hpd_output_count(
	struct i915_display *display)
{
	struct i915_hpd_world *world;

	/* A display without a hotplug has none. */
	world = display->hpd_world;
	if (world == NULL)
		return 0U;

	/* Succeeded: the connectors made. */
	return world->xh.count;
}

/*
 * Takes a connector's connection and preferred mode into the topology
 * once, without telling the sessions: the baseline the first snapshot
 * reads.
 */
void
drv_i915_hpd_output_take(
	struct i915_display *display,
	unsigned idx)
{
	struct i915_hpd_world *world;

	/* A connector of the hotplug. */
	world = display->hpd_world;
	if (world == NULL || idx >= world->xh.count)
		return;

	/* Its state, with no change told. */
	i915_hpd_topology_update(world, idx, 0);
}

/*
 * Reports the EDID a connector read last and its size; NULL with a zero
 * size when it read none.
 */
const uint8_t *
drv_i915_hpd_edid_bytes(
	struct i915_display *display,
	unsigned idx,
	unsigned *size)
{
	struct i915_hpd_world *world;
	const struct i915_xd_hpd_connector *connector;

	/* A connector of the hotplug, with an EDID. */
	*size = 0U;
	world = display->hpd_world;
	if (world == NULL || idx >= world->xh.count)
		return NULL;

	/* The connector; one without an EDID has none to give. */
	connector = &world->xh.connector[idx];
	if (connector->edid_size == 0U)
		return NULL;

	/* Succeeded: the bytes of the last read. */
	*size = connector->edid_size;
	return connector->edid;
}

/*
 * Makes one connector for each of the display's ports: HDMI for an HDMI
 * port that is not DisplayPort, the built-in panel for a DisplayPort port
 * A inside the machine, DisplayPort for the others.
 */
static void
i915_hpd_add_connectors(
	struct i915_hpd_world *world)
{
	const struct i915_xd_ports *ports;
	const struct i915_xd_port *port;
	unsigned kind;
	unsigned index;
	int added;

	/* Each port, while there is room. */
	ports = &world->display->xd.ports;
	for (index = 0U; index < ports->n; index++) {
		port = &ports->port[index];

		/* Its connector's kind. */
		kind = I915_XD_HPD_DP;
		if (port->hdmi && !port->dp) {
			kind = I915_XD_HPD_HDMI;
		} else if (port->dp &&
		           (port->device_type & I915_HPD_DEVICE_TYPE_INTERNAL) != 0U &&
		           port->ddi == I915_PORT_A) {
			kind = I915_XD_HPD_EDP;
		}

		/* A port past the last place has no connector. */
		added = drv_i915_xd_hpd_add(&world->xh, kind, port->ddi);
		if (added < 0)
			break;
	}
}

/* Sorts an interrupt's pulses and queues the works it asks for (interrupt context). */
static void
i915_hpd_irq(
	struct i915_hpd_world *world,
	uint32_t pins,
	uint32_t longs)
{
	unsigned long irq;
	unsigned queue;
	uint64_t now_ms;

	/* An interrupt that raised no pin has nothing to sort. */
	if (pins == 0U)
		return;

	/* Sorts the pulses into the works' share. */
	now_ms = i915_hpd_now_ms();
	irq = spin_lock_irqsave(&world->lock);

	queue = drv_i915_xd_hpd_sort(&world->xh, &world->display->xd.io, pins, longs, now_ms);

	spin_unlock_irqrestore(&world->lock, irq);

	/* Queues what it asks for. */
	i915_hpd_queue(world, queue);
}

/* Queues the pulse work and the detection the sort asked for. */
static void
i915_hpd_queue(
	struct i915_hpd_world *world,
	unsigned queue)
{
	/* The pulses first; the detection right away. */
	if ((queue & I915_XD_HPD_QUEUE_PULSES) != 0U)
		(void)drv_i915_queue_work(&world->wq, &world->pulse_work);
	if ((queue & I915_XD_HPD_QUEUE_DETECT) != 0U)
		(void)drv_i915_delayed_queue(&world->tq, &world->detect_work, 0U);
}

/*
 * Hands the pulses the interrupts collected to their connectors; a
 * pulse a connector leaves to the detection becomes an event of its pin.
 */
static void
i915_hpd_pulse_work(
	void *context)
{
	struct i915_hpd_world *world;
	const struct i915_xd_hpd_connector *connector;
	uint32_t long_ports;
	uint32_t short_ports;
	uint32_t events;
	unsigned long irq;
	unsigned index;
	int long_pulse;
	int taken;

	/* Counts the work, and takes the pulses. */
	world = context;
	world->pulse_works++;

	/* Takes the pulses the interrupt left, under the lock. */
	irq = spin_lock_irqsave(&world->lock);

	long_ports = world->xh.long_ports;
	short_ports = world->xh.short_ports;
	world->xh.long_ports = 0U;
	world->xh.short_ports = 0U;

	spin_unlock_irqrestore(&world->lock, irq);

	/* Each connector that takes its own pulses and has one. */
	events = 0U;
	for (index = 0U; index < world->xh.count; index++) {
		/* An HDMI connector, or one whose port had no pulse, has nothing to take. */
		connector = &world->xh.connector[index];
		if (connector->kind == I915_XD_HPD_HDMI)
			continue;
		if (((long_ports | short_ports) & (1U << connector->ddi)) == 0U)
			continue;

		/* The pulse is long when the port had a long one. */
		long_pulse = 0;
		if ((long_ports & (1U << connector->ddi)) != 0U)
			long_pulse = 1;

		/* Hands it the pulse; one it leaves goes to the detection. */
		kern_logf("i915: hpd %s pulse on %s\n", long_pulse ? "long" : "short", connector->name);
		taken = drv_i915_xd_hpd_pulse(&world->xh, &world->env, index, long_pulse);
		if (!taken && connector->pin != I915_XD_HPD_PIN_NONE)
			events |= 1U << connector->pin;
	}

	/* Nothing left for the detection. */
	if (events == 0U)
		return;

	/* The detection looks at the pins the connectors left. */
	irq = spin_lock_irqsave(&world->lock);

	world->xh.events |= events;

	spin_unlock_irqrestore(&world->lock, irq);

	/* Runs the detection now. */
	(void)drv_i915_delayed_queue(&world->tq, &world->detect_work, 0U);
}

/*
 * Detects the connectors whose pins had an event or a retry; masks the
 * pins a storm marked and arms them again two minutes later; asks for
 * another look at the connectors that found nothing new.
 */
static void
i915_hpd_detect_work(
	void *context)
{
	struct i915_hpd_world *world;
	const struct i915_xd_hpd_connector *connector;
	uint32_t events;
	uint32_t retries;
	uint32_t changed;
	uint32_t retry;
	unsigned long irq;
	unsigned index;
	uint32_t bit;
	int masked;

	/* The hotplug state the work belongs to. */
	world = context;

	/* Takes the events and the retries, and masks the storming pins. */
	irq = spin_lock_irqsave(&world->lock);

	events = world->xh.events;
	retries = world->xh.retries;
	world->xh.events = 0U;
	world->xh.retries = 0U;
	masked = drv_i915_xd_hpd_storms(&world->xh);

	spin_unlock_irqrestore(&world->lock, irq);

	/* A storm's pins come back after two minutes. */
	if (masked) {
		kern_logf("i915: hpd: an interrupt storm masked a pin for %u ms\n", I915_XD_HPD_STORM_REARM_MS);
		i915_hpd_requeue(world, &world->rearm_work, I915_XD_HPD_STORM_REARM_MS);
	}

	/* Detects each connector whose pin had an event or a retry. */
	changed = 0U;
	retry = 0U;
	mutex_lock(&world->probe_lock);

	for (index = 0U; index < world->xh.count; index++) {
		/* A connector without a pin, or whose pin had neither, is not looked at. */
		connector = &world->xh.connector[index];
		if (connector->pin == I915_XD_HPD_PIN_NONE)
			continue;

		/* A connector whose pin has neither events nor retries is passed over. */
		bit = 1U << connector->pin;
		if (((events | retries) & bit) == 0U)
			continue;

		/* Detects it, for its pin's event or its retry. */
		i915_hpd_detect_one(world, index, (events & bit) != 0U, &changed, &retry);
	}

	mutex_unlock(&world->probe_lock);

	/* A pin shared with a changed connector needs no other look. */
	retry &= ~changed;
	if (retry == 0U)
		return;

	/* Another look in a second. */
	irq = spin_lock_irqsave(&world->lock);

	world->xh.retries |= retry;

	spin_unlock_irqrestore(&world->lock, irq);

	/* Comes back for the retries after a while. */
	i915_hpd_requeue(world, &world->detect_work, I915_XD_HPD_RETRY_MS);
}

/* Enables and arms the pins a storm masked. */
static void
i915_hpd_rearm_work(
	void *context)
{
	struct i915_hpd_world *world;
	unsigned long irq;

	/* Counts the work, and enables the pins under the lock the interrupt takes. */
	world = context;
	world->rearms++;

	/* Arms the masked pins again, under the lock. */
	irq = spin_lock_irqsave(&world->lock);

	drv_i915_xd_hpd_rearm(&world->xh, &world->display->xd.io);

	spin_unlock_irqrestore(&world->lock, irq);

	/* Logs the rearm. */
	kern_logf("i915: hpd: the pins a storm masked are armed again\n");
}

/*
 * Looks at a Type-C port two seconds after its link lost its DisplayPort
 * partner: a partner that came back keeps the link; otherwise the output
 * on the port stops as an unplugged display's does.
 */
static void
i915_hpd_tc_reset_work(
	void *context)
{
	struct i915_hpd_tc_reset *reset;
	struct i915_hpd_world *world;
	int needs;

	/* The reset and its hotplug state. */
	reset = context;
	world = reset->world;

	/* Without the Type-C ports there is nothing to look at. */
	if (world->tc == NULL)
		return;

	/* A partner that came back keeps the link. */
	needs = drv_i915_xd_tc_ports_link_needs_reset(world->tc, reset->tc);
	if (!needs) {
		kern_logf("i915: hpd TC%u: the DisplayPort partner is back: the link is kept\n", reset->tc + 1U);
		return;
	}

	/* The link must go: the output on the port stops. */
	kern_logf("i915: hpd TC%u: the link has had no DisplayPort partner for %u ms: its output stops\n", reset->tc + 1U, I915_HPD_TC_RESET_MS);
	drv_i915_present_tc_partner_gone(world->display, reset->tc);
}

/*
 * Detects one connector and takes what changed: a connector connected or
 * disconnected, or connected with another EDID, is a topology change; a
 * connector that went is an unplugged output.  Adds its pin to changed
 * or to retry.
 */
static void
i915_hpd_detect_one(
	struct i915_hpd_world *world,
	unsigned index,
	int event,
	uint32_t *changed,
	uint32_t *retry)
{
	const struct i915_xd_hpd_connector *connector;
	const char *outcome;
	uint64_t old_epoch;
	int old_status;
	int topology;
	int state;

	/* The connector before the detection. */
	connector = &world->xh.connector[index];
	old_status = connector->status;
	old_epoch = connector->epoch;

	/* Detects it. */
	world->detections++;
	state = drv_i915_xd_hpd_detect(&world->xh, &world->display->xd.io, &world->env, index, event);

	/* Its pin changed, or is to be looked at again. */
	outcome = "unchanged";
	if (state == I915_XD_HPD_CHANGED) {
		*changed |= 1U << connector->pin;
		outcome = "changed";
	} else if (state == I915_XD_HPD_RETRY) {
		*retry |= 1U << connector->pin;
		outcome = "retry";
	}

	/* A connection or a disconnection, or a connected sink with another EDID, changes the topology. */
	topology = 0;
	if (old_status != connector->status &&
	    (connector->status == I915_XD_HPD_CONNECTED ||
	     old_status == I915_XD_HPD_CONNECTED)) {
		topology = 1;
	} else if (connector->status == I915_XD_HPD_CONNECTED && connector->epoch != old_epoch) {
		topology = 1;
	}

	/* Publishes a change of the topology. */
	if (topology)
		i915_hpd_topology_update(world, index, 1);

	/* The display the resident output was moved to went: its output stops at once. */
	if (old_status == I915_XD_HPD_CONNECTED && connector->status != I915_XD_HPD_CONNECTED)
		drv_i915_present_unplugged(world->display, index);

	/* Logs the state it took and what it was decided from. */
	kern_logf("i915: hpd event %s retry=%u %s -> %s (%s) live=%d EDID %u bytes epoch=%llu\n",
	    connector->name,
	    connector->retries,
	    drv_i915_xd_hpd_status_name(old_status),
	    drv_i915_xd_hpd_status_name(connector->status),
	    outcome,
	    connector->live,
	    connector->edid_size,
	    (unsigned long long)connector->epoch);
}

/*
 * Takes a connector's connection and preferred mode (from its EDID) into
 * the topology; with publish, its generation and the topology's sequence
 * move too and the sessions are woken.  Never in the interrupt.
 */
static void
i915_hpd_topology_update(
	struct i915_hpd_world *world,
	unsigned idx,
	int publish)
{
	const struct i915_xd_hpd_connector *connector;
	struct i915_display_mode mode;
	struct i915_hpd_mode taken;
	const char *source;
	unsigned long irq;
	uint64_t pixels;
	int connected;
	int error;

	/* The connection, and the preferred mode of a connected sink with an EDID. */
	connector = &world->xh.connector[idx];
	kern_memset(&taken, 0, sizeof(taken));
	connected = 0;
	if (connector->status == I915_XD_HPD_CONNECTED)
		connected = 1;

	/* Picks the mode from the EDID of a connected sink. */
	error = EINVAL;
	if (connected && connector->edid_size != 0U)
		error = drv_i915_output_pick_mode(0U, 0U, 0U, connector->edid, connector->edid_size, &mode, &source);

	/* The mode's pixels, when one was picked. */
	pixels = 0U;
	if (error == 0)
		pixels = (uint64_t)mode.htotal * mode.vtotal;

	/* Records the size of a sink that has a mode. */
	if (pixels != 0U) {
		taken.width = mode.hdisplay;
		taken.height = mode.vdisplay;
		taken.refresh_millihz = (uint32_t)(((uint64_t)mode.clock_khz * 1000000ULL + pixels / 2U) / pixels);
		taken.width_mm = mode.width_mm;
		taken.height_mm = mode.height_mm;
	}

	/* The topology's copy, and a change when asked. */
	irq = spin_lock_irqsave(&world->topology_lock);

	world->connected[idx] = connected;
	world->mode[idx] = taken;
	if (publish) {
		world->generation[idx]++;
		world->topology_sequence++;
	}

	spin_unlock_irqrestore(&world->topology_lock, irq);

	/* A display session waiting for the change hears of it. */
	if (publish)
		poll_notify();
}

/* Arms a delayed work for a new delay, replacing an arming not yet run. */
static void
i915_hpd_requeue(
	struct i915_hpd_world *world,
	struct i915_delayed_work *work,
	unsigned delay_ms)
{
	/* A pending arming gives way to the new one. */
	(void)drv_i915_delayed_cancel(&world->tq, work);
	(void)drv_i915_delayed_queue(&world->tq, work, delay_ms);
}

/* Reports the time in milliseconds the storm counts are kept against. */
static uint64_t
i915_hpd_now_ms(void)
{
	uint64_t ticks;
	uint64_t now_ms;

	/* The scheduler's ticks, in milliseconds. */
	ticks = sched_ticks();
	now_ms = kern_ticks_to_ms(ticks);

	/* Succeeded: the time now. */
	return now_ms;
}

/* Reports the deadline of a synchronous cancel: five seconds from now. */
static uint64_t
i915_hpd_deadline(void)
{
	uint64_t deadline;
	uint64_t now;

	/* Five seconds of scheduler ticks from now. */
	deadline = 0U;
	now = sched_ticks();
	(void)kern_deadline_after(now, kern_ms_to_ticks((uint64_t)I915_HPD_SYNC_SECONDS * 1000U), &deadline);

	/* Succeeded: the deadline. */
	return deadline;
}

/*
 * Takes or gives back the power a detection needs (a hook): the display
 * core's or the GMBUS controller's.  No switched well powers either on
 * display 12 and 13: they stay powered while the display core is up, so
 * no well is counted.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power_map.c
 * (tgl_power_wells, xelpd_power_wells: no well's domains name the GMBUS
 * or the display core).
 */
static int
i915_hpd_power(
	void *ctx,
	unsigned what,
	int on)
{
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(what);
	UNUSED_PARAMETER(on);

	/* Succeeded: nothing to take or give back. */
	return 0;
}

/* Reports whether a Type-C port has something the display uses plugged in, and tells the Type-C layer (a hook). */
static int
i915_hpd_tc_live(
	void *ctx,
	unsigned ddi)
{
	struct i915_hpd_world *world;
	int connected;
	int tc;

	/* The hotplug state the hook belongs to. */
	world = ctx;

	/* Without the display's Type-C ports, or for a port they do not have, nothing is connected. */
	tc = drv_i915_tc_port_of(world->display, (int)ddi);
	if (world->tc == NULL || tc < 0)
		return 0;

	/* The port's live state in the mode it is held in. */
	connected = drv_i915_xd_tc_ports_connected(world->tc, (unsigned)tc);
	kern_logf("i915: hpd TC%d connected=%d\n", tc + 1, connected);

	/* Tells the Type-C layer what the port has of DisplayPort now. */
	drv_i915_tc_port_report(world->tc, (unsigned)tc);

	/* Nothing the display uses is plugged in. */
	if (!connected)
		return 0;

	/* Succeeded: something is plugged in. */
	return 1;
}

/* Probes the sink of an external DisplayPort port and reads its EDID (a hook). */
static int
i915_hpd_dp_probe(
	void *ctx,
	unsigned ddi,
	uint8_t *edid,
	size_t size,
	unsigned *bytes)
{
	struct i915_hpd_world *world;
	enum i915_xd_dp_probe_status probed;

	/* The hotplug state the hook belongs to. */
	world = ctx;

	/* Probes the sink. */
	probed = drv_i915_dp_ext_probe(world->display, (int)ddi, edid, size, bytes);
	kern_logf("i915: hpd DP probe DDI %u: %s, EDID %u bytes\n", ddi, drv_i915_xd_dp_probe_status_name(probed), *bytes);

	/* A display the probe found is connected; a sink without one is not. */
	if (probed == I915_XD_DP_PROBE_CONNECTED)
		return I915_XD_HPD_CONNECTED;
	if (probed == I915_XD_DP_PROBE_DISCONNECTED)
		return I915_XD_HPD_DISCONNECTED;

	/* Succeeded: the probe could not tell. */
	return I915_XD_HPD_UNKNOWN;
}

/* Checks an external DisplayPort sink after a short pulse: nonzero when it is as it was (a hook). */
static int
i915_hpd_dp_pulse(
	void *ctx,
	unsigned ddi)
{
	struct i915_hpd_world *world;
	int handled;

	/* The hotplug state the hook belongs to. */
	world = ctx;

	/* Asks the external port whether the sink is as it was after the short pulse. */
	handled = drv_i915_dp_ext_pulse(world->display, (int)ddi);
	if (!handled)
		return 0;

	/* Succeeded: the sink is as it was. */
	return 1;
}

/* Gives an external DisplayPort sink its own link limits back after a long pulse (a hook). */
static void
i915_hpd_dp_reset(
	void *ctx,
	unsigned ddi)
{
	struct i915_hpd_world *world;

	/* The hotplug state the hook belongs to. */
	world = ctx;

	/* The sink's limits, before the detection looks. */
	drv_i915_dp_ext_link_reset(world->display, (int)ddi);
}

/*
 * Looks at a port's link after a detection (a hook): a Type-C link that
 * lost its DisplayPort partner is looked at again two seconds later
 * instead of being retrained; a lit external DisplayPort link that lost
 * its lock is trained again at the next frame.  An HDMI link is not
 * looked at.
 */
static void
i915_hpd_link(
	void *ctx,
	unsigned ddi,
	int hdmi)
{
	struct i915_hpd_world *world;
	int needs;
	int tc;

	/* The hotplug state, and the port's Type-C number. */
	world = ctx;
	tc = drv_i915_tc_port_of(world->display, (int)ddi);

	/* A Type-C link without its partner: looked at again later. */
	if (world->tc != NULL &&
	    tc >= 0 &&
	    tc < (int)I915_XD_TC_PORTS) {
		needs = drv_i915_xd_tc_ports_link_needs_reset(world->tc, (unsigned)tc);
		if (needs) {
			(void)drv_i915_delayed_queue(&world->tq, &world->tc_reset[tc].work, I915_HPD_TC_RESET_MS);
			kern_logf("i915: hpd TC%d: the link lost its DisplayPort partner: looking again in %u ms\n", tc + 1, I915_HPD_TC_RESET_MS);
			return;
		}
	}

	/* An HDMI link, or a link off the external DisplayPort ports, is not looked at. */
	if (hdmi || tc < 0)
		return;

	/* A lit link that lost its lock is trained again at the next frame. */
	needs = drv_i915_dp_ext_link_check(world->display, (int)ddi);
	if (needs)
		drv_i915_present_retrain_request(world->display);
}
