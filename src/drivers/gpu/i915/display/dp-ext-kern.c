/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display's external DP ports (see dp-ext-kern.h).
 *
 * Each Type-C port the display declares is an external DP port with an
 * AUX channel of its own (xd-aux.c).  Every access of a sink holds the
 * Type-C port in the mode it is in (a port no output holds is brought up
 * to what is plugged in, its PHY taken for the access and given back
 * after), refuses a port that is not held for DisplayPort or whose partner
 * went, powers the channel and the display core, and takes the lock all
 * the external ports' accesses share.
 *
 * A probe holds a link on the Type-C port for its whole length, so the
 * port's PHY stays with the display between the accesses, and gives the
 * link back at the end: the port's last link gives the PHY back at once,
 * so a port the display does not drive is released as soon as it has been
 * looked at (the firmware updates the other Type-C ports' hot plug only
 * after that).  The sink's probe itself is xd-dp.c.
 */

#include "internal.h"
#include "dp-ext-kern.h"
#include "display-power.h"
#include "tc-port.h"
#include "vbt-parse.h"
#include "xd-aux.h"
#include "xd-edid.h"

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>

#include <stdarg.h>

#include <uapi/errno.h>

/* The longest line the probe logs. */
#define I915_DP_EXT_KERN_LOG_LINE	256U

/* The highest link rate of a display version 13 Type-C port (HBR3), and the lanes its DDI drives. */
#define I915_DP_EXT_KERN_MAX_RATE	810000
#define I915_DP_EXT_KERN_MAX_LANES	4

/* The lanes a probe asks the Type-C port for: AUX needs the PHY, not a lane count. */
#define I915_DP_EXT_KERN_PROBE_LANES	1U

/*
 * The receiver capability fields the capability read uses: the revision,
 * the training interval's bit that says extended capabilities exist, and
 * where they are.
 *
 * Values referenced: Linux v6.8.12 include/drm/display/drm_dp.h
 * (DP_DPCD_REV, DP_TRAINING_AUX_RD_INTERVAL,
 * DP_EXTENDED_RECEIVER_CAP_FIELD_PRESENT, DP_DP13_DPCD_REV).
 */
#define I915_DP_EXT_KERN_DPCD_REV		0x000U
#define I915_DP_EXT_KERN_TRAINING_INTERVAL	0x00eU
#define I915_DP_EXT_KERN_EXTENDED_PRESENT	0x80U
#define I915_DP_EXT_KERN_EXTENDED_CAPS		0x2200U

/*
 * The I2C addresses of the EDID on the DDC: the EEPROM, and the segment
 * pointer that selects which 256 bytes of it the EEPROM shows.
 *
 * Values referenced: VESA E-DDC Standard, version 1.2 (the 0xA0/0xA1
 * EDID address and the 0x60 segment pointer, as 7-bit addresses).
 */
#define I915_DP_EXT_KERN_DDC_EDID	0x50U
#define I915_DP_EXT_KERN_DDC_SEGMENT	0x30U

struct i915_dp_ext_world;

/*
 * One external DP port: its Type-C port and DDI, its AUX channel, what
 * the source offers, what the last probe found, and the link limits a
 * failed training left.
 *
 * It lives in the external ports for the display's lifetime.  lock
 * serializes the port's probes and guards the sink and the limits.
 */
struct i915_dp_ext_port {
	int declared;
	char name[8];
	unsigned tc_port;
	int ddi;
	struct i915_xd_aux aux;
	struct i915_xd_dp_probe_env env;
	struct i915_xd_dp_probe_source source;
	struct i915_xd_dp_probe sink;
	struct mutex lock;
	unsigned probes;
	unsigned refused;
	struct i915_lcd_aux_emit aux_emit;
	struct i915_dp_ext_world *ext;
	int max_link_rate;
	int max_link_lanes;
};

/*
 * The display's external DP ports.
 *
 * The display owns it from the start of its outputs to its stop.  live is
 * set once the ports are bound and cleared by the stop; an access refuses
 * while it is clear.  aux_lock lets one access of a sink run at a time on
 * all the external ports' channels.
 */
struct i915_dp_ext_world {
	struct i915_display *display;
	int live;
	struct i915_xd_tc_ports *tc;
	struct mutex aux_lock;
	struct i915_dp_ext_port port[I915_XD_TC_PORTS];
};

/* What an access of a sink holds: the Type-C port's lock, the AUX channel's power, and the channels' lock. */
struct i915_dp_ext_access {
	int tc_locked;
	uint64_t aux_functions;
	int aux_powered;
	int aux_locked;
};

static void i915_dp_ext_bind_port(struct i915_dp_ext_world *ext, unsigned tc_port);
static void i915_dp_ext_probe_locked(struct i915_dp_ext_world *ext, struct i915_dp_ext_port *p);
static int i915_dp_ext_access_begin(struct i915_dp_ext_port *p, struct i915_dp_ext_access *access);
static void i915_dp_ext_access_end(struct i915_dp_ext_port *p, struct i915_dp_ext_access *access);
static long i915_dp_ext_read(struct i915_dp_ext_port *p, unsigned offset, uint8_t *buffer, size_t size);
static long i915_dp_ext_write(struct i915_dp_ext_port *p, unsigned offset, const uint8_t *buffer, size_t size);
static int i915_dp_ext_caps(struct i915_dp_ext_port *p, uint8_t dpcd[I915_XD_DP_PROBE_DPCD_SIZE]);
static long i915_dp_ext_dpcd_read(void *ctx, unsigned offset, uint8_t *buffer, size_t size);
static long i915_dp_ext_dpcd_write(void *ctx, unsigned offset, const uint8_t *buffer, size_t size);
static int i915_dp_ext_dpcd_probe(void *ctx, unsigned offset);
static int i915_dp_ext_read_caps(void *ctx, uint8_t dpcd[I915_XD_DP_PROBE_DPCD_SIZE]);
static int i915_dp_ext_ddc_probe(void *ctx);
static int i915_dp_ext_edid_read(void *ctx, uint8_t *buffer, unsigned max_blocks, unsigned *extensions);
static int i915_dp_ext_edid_block(void *context, unsigned segment, unsigned offset, uint8_t *buffer, unsigned size);
static void i915_dp_ext_log(void *ctx, const char *format, ...) __attribute__((format(printf, 2, 3)));
static long i915_dp_ext_emit_dpcd_read(void *ctx, unsigned offset, uint8_t *buffer, size_t size);
static long i915_dp_ext_emit_dpcd_write(void *ctx, unsigned offset, const uint8_t *buffer, size_t size);
static int i915_dp_ext_emit_read_caps(void *ctx, uint8_t dpcd[15]);
static int i915_dp_ext_emit_configure_converter(void *ctx);
static struct i915_dp_ext_port *i915_dp_ext_declared(struct i915_display *display, int port);

/*
 * Binds the display's Type-C ports as external DP ports.
 *
 * It runs once, after the Type-C ports are declared and read out and
 * before the hotplug starts; a display without bound Type-C ports has no
 * external DP port.  Nothing is probed here.
 */
void
drv_i915_dp_ext_start(
	struct i915_display *display)
{
	struct i915_dp_ext_world *ext;
	struct i915_xd_tc_ports *tc;
	unsigned tc_port;
	unsigned bound;
	int error;

	/* A display that started its ports already has nothing more to bind. */
	if (display->dp_ext != NULL)
		return;

	/* Without bound Type-C ports there is no external DP port. */
	tc = drv_i915_tc_port_ports(display);
	if (tc == NULL) {
		kern_logf("i915: DP-ext: no Type-C ports: no external DP port\n");
		return;
	}

	/* The ports, zeroed, and the lock their accesses share. */
	ext = kern_calloc(1U, sizeof(*ext));
	if (ext == NULL) {
		kern_logf("i915: DP-ext: no memory for the external DP ports\n");
		return;
	}

	/* The lock that keeps one access at a time on the channels. */
	error = mutex_init(&ext->aux_lock, LOCK_RANK_DEVICE, "i915 dp-ext aux");
	if (error != 0) {
		kern_free(ext);
		kern_logf("i915: DP-ext: the AUX lock could not be made: no external DP port\n");
		return;
	}

	/* The display and its Type-C ports. */
	ext->display = display;
	ext->tc = tc;

	/* Binds each declared Type-C port. */
	bound = 0U;
	for (tc_port = 0U; tc_port < I915_XD_TC_PORTS; tc_port++) {
		/* A port the VBT does not declare is never touched. */
		if (!tc->port[tc_port].present)
			continue;

		/* Binds the port and counts it. */
		i915_dp_ext_bind_port(ext, tc_port);
		bound++;
	}

	/* The ports can be probed from here on. */
	ext->live = 1;
	display->dp_ext = ext;
	kern_logf("i915: DP-ext: %u external DP port(s) bound\n", bound);
}

/*
 * Stops the external DP ports: no access starts after this, and a probe
 * running is waited for.  The ports stay allocated until
 * drv_i915_dp_ext_destroy().
 */
void
drv_i915_dp_ext_stop(
	struct i915_display *display)
{
	struct i915_dp_ext_world *ext;
	struct i915_dp_ext_port *p;
	unsigned tc_port;

	/* Nothing to stop without started external ports. */
	ext = display->dp_ext;
	if (ext == NULL || !ext->live)
		return;

	/* An access that starts from here on finds the ports stopped. */
	ext->live = 0;

	/* Waits out a probe that is running, port by port. */
	for (tc_port = 0U; tc_port < I915_XD_TC_PORTS; tc_port++) {
		/* A port never bound has no lock. */
		p = &ext->port[tc_port];
		if (!p->declared)
			continue;

		/* Takes the port's lock, which a running probe holds, and logs the port's tally. */
		mutex_lock(&p->lock);

		kern_logf("i915: DP-ext %s: stopped after %u probe(s), last %s (AUX tries %u, no answers %u, refused %u)\n",
		    p->name,
		    p->probes,
		    drv_i915_xd_dp_probe_status_name(p->sink.status),
		    p->aux.tries,
		    p->aux.no_answers,
		    p->refused);

		mutex_unlock(&p->lock);
	}
}

/*
 * Frees the display's external DP ports, which must have been stopped.
 */
void
drv_i915_dp_ext_destroy(
	struct i915_display *display)
{
	/* A display without them has nothing to free. */
	if (display->dp_ext == NULL)
		return;

	/* Frees them and forgets them. */
	kern_free(display->dp_ext);
	display->dp_ext = NULL;
}

/*
 * Probes the sink of an external DP port (the Type-C port of a DDI port)
 * and copies its EDID.
 *
 * The caller has found the port's live status connected.  Returns what
 * the probe concluded; unknown when the external ports are not bound or
 * the port is not a Type-C port, disconnected for a port the VBT does not
 * declare.  *edid_bytes receives the EDID bytes copied (whole blocks, 0
 * without an EDID).
 */
enum i915_xd_dp_probe_status
drv_i915_dp_ext_probe(
	struct i915_display *display,
	int port,
	uint8_t *edid,
	size_t edid_size,
	unsigned *edid_bytes)
{
	struct i915_dp_ext_world *ext;
	struct i915_dp_ext_port *p;
	enum i915_xd_dp_probe_status status;
	unsigned bytes;
	int tc_port;

	/* Nothing is copied unless the probe reads an EDID. */
	*edid_bytes = 0U;

	/* Without bound external ports the sink cannot be asked. */
	ext = display->dp_ext;
	if (ext == NULL || !ext->live)
		return I915_XD_DP_PROBE_UNKNOWN;

	/* Only a Type-C port is an external DP port here. */
	tc_port = drv_i915_tc_port_of(display, port);
	if (tc_port < 0)
		return I915_XD_DP_PROBE_UNKNOWN;

	/* A port the VBT does not declare has no sink. */
	p = &ext->port[tc_port];
	if (!p->declared)
		return I915_XD_DP_PROBE_DISCONNECTED;

	/* Probes under the port's lock, unless the ports stopped meanwhile. */
	mutex_lock(&p->lock);

	if (!ext->live) {
		mutex_unlock(&p->lock);
		return I915_XD_DP_PROBE_UNKNOWN;
	}

	i915_dp_ext_probe_locked(ext, p);
	status = p->sink.status;

	/* Copies as many of the EDID's whole blocks as the caller has room for. */
	bytes = p->sink.edid_blocks * I915_XD_DP_PROBE_EDID_BLOCK_SIZE;
	if (bytes > edid_size)
		bytes = (unsigned)(edid_size / I915_XD_DP_PROBE_EDID_BLOCK_SIZE) * I915_XD_DP_PROBE_EDID_BLOCK_SIZE;

	/* Copies the EDID the probe read. */
	if (bytes != 0U)
		kern_memcpy(edid, p->sink.edid, bytes);

	mutex_unlock(&p->lock);

	/* Succeeded: reports the conclusion and the EDID copied. */
	*edid_bytes = bytes;
	return status;
}

/*
 * Handles a short pulse (IRQ_HPD) of an external DP port.
 *
 * The port is held for the accesses as for a probe, and its sink is
 * checked against what the last probe found (xd-dp.c).  Returns 1 when
 * the pulse is handled, 0 when the port must be detected again (the
 * hotplug then probes it); a port that cannot be asked returns 0.
 */
int
drv_i915_dp_ext_pulse(
	struct i915_display *display,
	int port)
{
	struct i915_dp_ext_world *ext;
	struct i915_dp_ext_port *p;
	int handled;
	int retrain;

	/* Only a declared Type-C port of bound external ports has a sink to check. */
	p = i915_dp_ext_declared(display, port);
	if (p == NULL)
		return 0;

	/* Checks the sink with the port held for the accesses, unless the ports stopped meanwhile. */
	ext = p->ext;
	mutex_lock(&p->lock);

	if (!ext->live) {
		mutex_unlock(&p->lock);
		return 0;
	}

	/* Handles the pulse with the port's link held. */
	drv_i915_xd_tc_ports_get_link(ext->tc, p->tc_port, I915_DP_EXT_KERN_PROBE_LANES);
	handled = drv_i915_xd_dp_probe_short_pulse(&p->env, &p->sink);
	drv_i915_xd_tc_ports_put_link(ext->tc, p->tc_port);
	kern_logf("i915: DP-ext %s: short pulse %s\n", p->name, handled ? "handled" : "asks for a detection");

	mutex_unlock(&p->lock);

	/* A sink that changed is detected again. */
	if (!handled)
		return 0;

	/* A lit link that lost its lock goes to the detection, whose link check trains it again. */
	retrain = drv_i915_dp_ext_link_check(display, port);
	if (retrain)
		return 0;

	/* Succeeded: the pulse is handled. */
	return 1;
}

/*
 * Tells whether the resident output's trained link on an external DP port
 * must be trained again: 1 when the port's DisplayPort display is the lit
 * resident output and its sink's link status lost alignment or a lane's
 * lock, 0 otherwise.  The status is read with the port held (the output's
 * own link keeps the PHY).
 */
int
drv_i915_dp_ext_link_check(
	struct i915_display *display,
	int port)
{
	struct i915_dp_ext_world *ext;
	struct i915_dp_ext_port *p;
	int lanes;
	int needs;

	/* Only the lit resident output's port has a trained link. */
	if (display->output.none || display->output.kind != I915_OUTPUT_KIND_DP_EXT)
		return 0;
	if (display->output.port != port || !display->window.display_up)
		return 0;

	/* The lanes the output's link runs on. */
	lanes = (int)display->output.link.lanes;

	/* Only a declared Type-C port of bound external ports. */
	p = i915_dp_ext_declared(display, port);
	if (p == NULL)
		return 0;

	/* Reads the link status with the port held, unless the ports stopped meanwhile. */
	ext = p->ext;
	mutex_lock(&p->lock);

	if (!ext->live) {
		mutex_unlock(&p->lock);
		return 0;
	}

	/* Asks the sink with the port's link held. */
	drv_i915_xd_tc_ports_get_link(ext->tc, p->tc_port, (unsigned)lanes);
	needs = drv_i915_xd_dp_probe_link_needs_retrain(&p->env, lanes);
	drv_i915_xd_tc_ports_put_link(ext->tc, p->tc_port);

	mutex_unlock(&p->lock);

	/* Logs a link that lost its lock. */
	if (needs)
		kern_logf("i915: DP-ext %s: the lit link (x%d) lost its alignment or a lane's lock: it is trained again\n", p->name, lanes);

	/* Succeeded: the answer. */
	return needs;
}

/*
 * Copies what the last probe of an external DP port found, for the
 * output a claim prepares: one probe's whole result, taken under the
 * port's lock.  Returns 0, or ENXIO when the ports are not bound or the
 * port is not a declared Type-C port.
 */
int
drv_i915_dp_ext_sink_copy(
	struct i915_display *display,
	int port,
	struct i915_xd_dp_probe *sink)
{
	struct i915_dp_ext_port *p;

	/* Only a declared Type-C port has a sink. */
	p = i915_dp_ext_declared(display, port);
	if (p == NULL)
		return ENXIO;

	/* Copies the last probe's result whole. */
	mutex_lock(&p->lock);

	*sink = p->sink;

	mutex_unlock(&p->lock);

	/* Succeeded: the copy is the caller's. */
	return 0;
}

/*
 * Reports the link limits a failed link training left on an external DP
 * port: 0 means the sink's own.  Returns 0, or ENXIO when the ports are
 * not bound or the port is not a declared Type-C port.
 */
int
drv_i915_dp_ext_link_limits(
	struct i915_display *display,
	int port,
	int *max_rate,
	int *max_lanes)
{
	struct i915_dp_ext_port *p;

	/* Only a declared Type-C port has limits. */
	p = i915_dp_ext_declared(display, port);
	if (p == NULL)
		return ENXIO;

	/* Reads them under the port's lock. */
	mutex_lock(&p->lock);

	*max_rate = p->max_link_rate;
	*max_lanes = p->max_link_lanes;

	mutex_unlock(&p->lock);

	/* Succeeded: the limits are the caller's. */
	return 0;
}

/*
 * Lowers an external DP port's link limits after a link training failed
 * at a rate and lane count (drv_i915_xd_dp_probe_fallback_values()).  Returns
 * 0, ENOSPC when no lower link is left, or ENXIO when the port is not a
 * declared Type-C port.
 */
int
drv_i915_dp_ext_link_fallback(
	struct i915_display *display,
	int port,
	int rate,
	int lanes)
{
	struct i915_dp_ext_port *p;
	const char *outcome;
	int max_rate;
	int max_lanes;
	int lowered;
	int error;

	/* Only a declared Type-C port has limits. */
	p = i915_dp_ext_declared(display, port);
	if (p == NULL)
		return ENXIO;

	/* Lowers the limits under the port's lock, from the sink's shared rates. */
	mutex_lock(&p->lock);

	error = ENOSPC;
	lowered = drv_i915_xd_dp_probe_fallback_values(&p->sink, rate, lanes, &max_rate, &max_lanes);
	if (lowered) {
		p->max_link_rate = max_rate;
		p->max_link_lanes = max_lanes;
		error = 0;
	}

	mutex_unlock(&p->lock);

	/* Logs the step. */
	outcome = "no lower link";
	if (error == 0)
		outcome = "lowered";

	kern_logf("i915: DP-ext %s: link training failed at %d kHz x%d: %s (max rate %d, max lanes %d)\n",
	    p->name,
	    rate,
	    lanes,
	    outcome,
	    p->max_link_rate,
	    p->max_link_lanes);

	/* Reports that no lower link is left. */
	if (error != 0)
		return error;

	/* Succeeded: the limits are lowered. */
	return 0;
}

/*
 * Gives an external DP port its sink's own link limits back (after a long
 * hot plug pulse).
 */
void
drv_i915_dp_ext_link_reset(
	struct i915_display *display,
	int port)
{
	struct i915_dp_ext_port *p;

	/* Only a declared Type-C port has limits. */
	p = i915_dp_ext_declared(display, port);
	if (p == NULL)
		return;

	/* Clears them under the port's lock. */
	mutex_lock(&p->lock);

	p->max_link_rate = 0;
	p->max_link_lanes = 0;

	mutex_unlock(&p->lock);
}

/*
 * Gives the DPCD access of an external DP port's sink for a screen of
 * that sink.
 *
 * The access goes over the port's AUX channel, holding the Type-C port
 * for each access; it does not take the port's probe lock, so a probe and
 * a screen may interleave their accesses (the accesses' own lock orders
 * them).  NULL when the ports are not bound or the port is not a declared
 * Type-C port.
 */
const struct i915_lcd_aux_emit *
drv_i915_dp_ext_aux_emit(
	struct i915_display *display,
	int port)
{
	struct i915_dp_ext_port *p;

	/* Only a declared Type-C port has a channel of its own. */
	p = i915_dp_ext_declared(display, port);
	if (p == NULL)
		return NULL;

	/* Succeeded: the port's access, bound with the port. */
	return &p->aux_emit;
}

/*
 * Binds one declared Type-C port as an external DP port: its DDI, its AUX
 * channel, what the source offers there (the VBT's limits), the probe's
 * access, the screen's access and the lock.
 */
static void
i915_dp_ext_bind_port(
	struct i915_dp_ext_world *ext,
	unsigned tc_port)
{
	const struct i915_vbt_encoder *child;
	struct i915_dp_ext_port *p;
	struct i915_display *display;
	int port;

	/* The port, its DDI, and the VBT's child of that DDI. */
	display = ext->display;
	p = &ext->port[tc_port];
	port = (int)ext->tc->port[tc_port].desc->ddi;
	child = NULL;
	if (display->vbt_state.parsed_live)
		child = drv_i915_vbt_encoder_for_port(&display->vbt_state.parsed, port);

	/* Its name, its DDI and its AUX channel. */
	(void)kern_snprintf(p->name, sizeof(p->name), "TC%u", tc_port + 1U);
	p->ext = ext;
	p->tc_port = tc_port;
	p->ddi = port;
	(void)drv_i915_xd_aux_init(&p->aux, drv_i915_tc_port_aux(display, tc_port));

	/* What the source offers: HBR3 and 4 lanes, within the VBT's limits; the FIA's lanes are read at each probe. */
	p->source.max_rate = I915_DP_EXT_KERN_MAX_RATE;
	p->source.max_lanes = I915_DP_EXT_KERN_MAX_LANES;
	p->source.vbt_max_rate = 0;
	p->source.vbt_max_lanes = 0;
	p->source.fia_lanes = 0;
	if (child != NULL) {
		p->source.vbt_max_rate = child->dp_max_link_rate;
		p->source.vbt_max_lanes = child->dp_max_lane_count;
	}

	/* The probe's access to the sink. */
	p->env.ctx = p;
	p->env.dpcd_read = i915_dp_ext_dpcd_read;
	p->env.dpcd_write = i915_dp_ext_dpcd_write;
	p->env.dpcd_probe = i915_dp_ext_dpcd_probe;
	p->env.read_caps = i915_dp_ext_read_caps;
	p->env.ddc_probe = i915_dp_ext_ddc_probe;
	p->env.edid_read = i915_dp_ext_edid_read;
	p->env.log = i915_dp_ext_log;

	/* The access a screen of the sink takes, refused once the ports stop. */
	p->aux_emit.ctx = p;
	p->aux_emit.dpcd_read = i915_dp_ext_emit_dpcd_read;
	p->aux_emit.dpcd_write = i915_dp_ext_emit_dpcd_write;
	p->aux_emit.read_dpcd_caps = i915_dp_ext_emit_read_caps;
	p->aux_emit.configure_converter = i915_dp_ext_emit_configure_converter;

	/* The lock, and a sink not yet probed. */
	(void)mutex_init(&p->lock, LOCK_RANK_DEVICE, "i915 dp-ext port");
	drv_i915_xd_dp_probe_forget(&p->sink);
	p->declared = 1;

	/* Logs the binding. */
	kern_logf("i915: DP-ext %s: bound (DDI %c, AUX channel %u, VBT max rate %d lanes %d)\n",
	    p->name,
	    (char)('A' + port),
	    p->aux.channel,
	    p->source.vbt_max_rate,
	    p->source.vbt_max_lanes);
}

/*
 * Probes one port's sink with the port's lock held: takes a link on the
 * Type-C port for the probe, reads the lanes the FIA assigned, probes a
 * port held in DP-alt or legacy mode, gives the link back (the PHY goes
 * back at once when no output holds the port) and logs what it found.
 */
static void
i915_dp_ext_probe_locked(
	struct i915_dp_ext_world *ext,
	struct i915_dp_ext_port *p)
{
	unsigned mode;
	unsigned after;
	unsigned tc_port;

	/* Counts the probe. */
	tc_port = p->tc_port;
	p->probes++;

	/* Holds the port for the whole probe: its PHY stays with the display between the accesses. */
	drv_i915_xd_tc_ports_get_link(ext->tc, tc_port, I915_DP_EXT_KERN_PROBE_LANES);

	/* The mode the link fixed, and the lanes the FIA gave DisplayPort (read with the port held). */
	mode = drv_i915_xd_tc_ports_lock(ext->tc, tc_port, I915_DP_EXT_KERN_PROBE_LANES);

	p->source.fia_lanes = (int)drv_i915_xd_tc_ports_max_lanes(ext->tc, tc_port);

	drv_i915_xd_tc_ports_unlock(ext->tc, tc_port);

	/* Only a port the display holds for DP has a sink to probe; Thunderbolt is outside the driver's scope. */
	if (mode != I915_XD_TC_MODE_DP_ALT && mode != I915_XD_TC_MODE_LEGACY) {
		drv_i915_xd_dp_probe_forget(&p->sink);
		p->sink.step = I915_XD_DP_PROBE_STEP_PHY;
		kern_logf("i915: DP-ext %s: held in %s mode, not DP: no sink probed\n", p->name, drv_i915_xd_tc_mode_name(mode));
	} else {
		/* Probes the sink; one the AUX channel could not reach has the port's Type-C state logged while it is still held. */
		drv_i915_xd_dp_probe_detect(&p->env, &p->source, &p->sink);
		if (p->sink.status == I915_XD_DP_PROBE_DISCONNECTED && p->sink.error != 0)
			drv_i915_xd_tc_ports_log_state(ext->tc, tc_port, "AUX failed");
	}

	/* Gives the link back: the port's last link gives the PHY back at once. */
	drv_i915_xd_tc_ports_put_link(ext->tc, tc_port);
	after = drv_i915_xd_tc_ports_mode(ext->tc, tc_port);

	/* Logs what the probe found and how the port was left. */
	drv_i915_xd_dp_probe_log(&p->env, &p->sink, p->name);
	kern_logf("i915: DP-ext %s: probe %u held the port in %s mode (FIA lanes %d), %s after it | AUX tries %u no answers %u\n",
	    p->name,
	    p->probes,
	    drv_i915_xd_tc_mode_name(mode),
	    p->source.fia_lanes,
	    drv_i915_xd_tc_mode_name(after),
	    p->aux.tries,
	    p->aux.no_answers);
}

/*
 * Makes a port's AUX channel usable for one access: the Type-C port held
 * in the mode it is in (refused unless it carries DisplayPort and its
 * partner is there), the channel's power and the display core's, and the
 * lock the external ports' accesses share.  Returns 0, or a negative
 * error with nothing held.
 */
static int
i915_dp_ext_access_begin(
	struct i915_dp_ext_port *p,
	struct i915_dp_ext_access *access)
{
	struct i915_display *display;
	unsigned mode;
	int connected;
	int error;

	/* Nothing held yet. */
	display = p->ext->display;
	access->tc_locked = 0;
	access->aux_powered = 0;
	access->aux_locked = 0;

	/* The Type-C port, brought up to what is plugged in when no output holds it. */
	mode = drv_i915_xd_tc_ports_lock(p->ext->tc, p->tc_port, I915_DP_EXT_KERN_PROBE_LANES);
	access->tc_locked = 1;

	/* Only DP-alt and legacy carry DisplayPort AUX here, and only with the partner there. */
	if (mode != I915_XD_TC_MODE_DP_ALT && mode != I915_XD_TC_MODE_LEGACY) {
		p->refused++;
		i915_dp_ext_access_end(p, access);
		return -ENXIO;
	}

	/* Refuses a port with no partner plugged in. */
	connected = drv_i915_xd_tc_ports_connected_locked(p->ext->tc, p->tc_port);
	if (!connected) {
		p->refused++;
		i915_dp_ext_access_end(p, access);
		return -ENXIO;
	}

	/* The channel's power. */
	access->aux_functions = drv_i915_display_power_aux(display->xd.choice.platform->display_ver, (int)p->aux.channel, 0);
	error = drv_i915_display_power_get(&display->power, access->aux_functions);
	if (error != 0) {
		i915_dp_ext_access_end(p, access);
		return -EIO;
	}

	/* The channel's power is held. */
	access->aux_powered = 1;

	/* One access at a time on the external ports' channels. */
	mutex_lock(&p->ext->aux_lock);
	access->aux_locked = 1;

	/* Succeeded: the channel is usable until the end of the access. */
	return 0;
}

/* Gives back what an access held, in reverse: the lock, the power (the channel's later), the Type-C port. */
static void
i915_dp_ext_access_end(
	struct i915_dp_ext_port *p,
	struct i915_dp_ext_access *access)
{
	struct i915_display *display;

	/* The display the access powered. */
	display = p->ext->display;

	/* The lock is held only once the channel's power was taken. */
	if (access->aux_locked)
		mutex_unlock(&p->ext->aux_lock);

	/* The channel's power goes after a while, for the next access. */
	if (access->aux_powered)
		drv_i915_display_power_put_later(&display->power, access->aux_functions, -1);

	/* The Type-C port; a port no link holds gives its PHY back at once. */
	if (access->tc_locked)
		drv_i915_xd_tc_ports_unlock(p->ext->tc, p->tc_port);
}

/* Reads DPCD bytes of a port's sink: the bytes read, or a negative error. */
static long
i915_dp_ext_read(
	struct i915_dp_ext_port *p,
	unsigned offset,
	uint8_t *buffer,
	size_t size)
{
	struct i915_dp_ext_access access;
	int error;

	/* Makes the channel usable. */
	error = i915_dp_ext_access_begin(p, &access);
	if (error != 0)
		return error;

	/* Reads over the channel. */
	error = drv_i915_xd_dpcd_read(&p->aux, &p->ext->display->xd.io, offset, buffer, (unsigned)size);

	i915_dp_ext_access_end(p, &access);

	/* Reports a read that failed. */
	if (error != 0)
		return -(long)error;

	/* Succeeded: every byte was read. */
	return (long)size;
}

/* Writes DPCD bytes of a port's sink: the bytes written, or a negative error. */
static long
i915_dp_ext_write(
	struct i915_dp_ext_port *p,
	unsigned offset,
	const uint8_t *buffer,
	size_t size)
{
	struct i915_dp_ext_access access;
	int error;

	/* Makes the channel usable. */
	error = i915_dp_ext_access_begin(p, &access);
	if (error != 0)
		return error;

	/* Writes over the channel. */
	error = drv_i915_xd_dpcd_write(&p->aux, &p->ext->display->xd.io, offset, buffer, (unsigned)size);

	i915_dp_ext_access_end(p, &access);

	/* Reports a write that failed. */
	if (error != 0)
		return -(long)error;

	/* Succeeded: every byte was written. */
	return (long)size;
}

/*
 * Reads a port's sink's receiver capabilities, the extended ones in their
 * place where the sink has them and their revision is not lower.  Returns
 * 0, or a negative error.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/display/drm_dp_helper.c
 * (drm_dp_read_dpcd_caps).
 */
static int
i915_dp_ext_caps(
	struct i915_dp_ext_port *p,
	uint8_t dpcd[I915_XD_DP_PROBE_DPCD_SIZE])
{
	uint8_t extended[I915_XD_DP_PROBE_DPCD_SIZE];
	long transferred;

	/* The receiver's own capabilities. */
	transferred = i915_dp_ext_read(p, I915_DP_EXT_KERN_DPCD_REV, dpcd, I915_XD_DP_PROBE_DPCD_SIZE);
	if (transferred < 0)
		return (int)transferred;

	/* A receiver without extended capabilities has said everything. */
	if ((dpcd[I915_DP_EXT_KERN_TRAINING_INTERVAL] & I915_DP_EXT_KERN_EXTENDED_PRESENT) == 0U)
		return 0;

	/* The extended ones, which replace the receiver's unless their revision is lower. */
	transferred = i915_dp_ext_read(p, I915_DP_EXT_KERN_EXTENDED_CAPS, extended, I915_XD_DP_PROBE_DPCD_SIZE);
	if (transferred < 0)
		return (int)transferred;
	if (extended[I915_DP_EXT_KERN_DPCD_REV] < dpcd[I915_DP_EXT_KERN_DPCD_REV])
		return 0;

	/* Takes the extended capabilities in place of the first ones. */
	kern_memcpy(dpcd, extended, I915_XD_DP_PROBE_DPCD_SIZE);

	/* Succeeded: the extended capabilities are in their place. */
	return 0;
}

/* Reads DPCD bytes over the port's AUX channel for the probe. */
static long
i915_dp_ext_dpcd_read(
	void *ctx,
	unsigned offset,
	uint8_t *buffer,
	size_t size)
{
	long transferred;

	/* Reads over the port's channel. */
	transferred = i915_dp_ext_read(ctx, offset, buffer, size);
	if (transferred < 0)
		return transferred;

	/* Succeeded: reports the bytes read. */
	return transferred;
}

/* Writes DPCD bytes over the port's AUX channel for the probe. */
static long
i915_dp_ext_dpcd_write(
	void *ctx,
	unsigned offset,
	const uint8_t *buffer,
	size_t size)
{
	long transferred;

	/* Writes over the port's channel. */
	transferred = i915_dp_ext_write(ctx, offset, buffer, size);
	if (transferred < 0)
		return transferred;

	/* Succeeded: reports the bytes written. */
	return transferred;
}

/* Wakes the port's sink with a throw-away read of one byte. */
static int
i915_dp_ext_dpcd_probe(
	void *ctx,
	unsigned offset)
{
	uint8_t byte;
	long transferred;

	/* Reads one byte over the port's channel. */
	transferred = i915_dp_ext_read(ctx, offset, &byte, 1U);
	if (transferred < 0)
		return (int)transferred;

	/* Succeeded: the sink answered. */
	return 0;
}

/* Reads the port's receiver capabilities, the extended ones where the sink has them. */
static int
i915_dp_ext_read_caps(
	void *ctx,
	uint8_t dpcd[I915_XD_DP_PROBE_DPCD_SIZE])
{
	int error;

	/* Reads over the port's channel. */
	error = i915_dp_ext_caps(ctx, dpcd);
	if (error != 0)
		return error;

	/* Succeeded: the capabilities are in dpcd. */
	return 0;
}

/* Asks whether a device answers at the EDID address behind the port's channel: one byte read from offset 0. */
static int
i915_dp_ext_ddc_probe(
	void *ctx)
{
	uint8_t byte;
	int error;

	/* Reads the first byte of the first block. */
	error = i915_dp_ext_edid_block(ctx, 0U, 0U, &byte, 1U);
	if (error != 0)
		return 0;

	/* Succeeded: a device answered. */
	return 1;
}

/* Reads the EDID behind the port's channel: the valid blocks, or a negative error. */
static int
i915_dp_ext_edid_read(
	void *ctx,
	uint8_t *buffer,
	unsigned max_blocks,
	unsigned *extensions)
{
	struct i915_xd_edid_ddc ddc;
	unsigned blocks;
	int error;

	/* Reads the blocks over the channel's I2C. */
	ddc.read_block = i915_dp_ext_edid_block;
	ddc.context = ctx;
	error = drv_i915_xd_edid_read(&ddc, buffer, max_blocks, &blocks, extensions);
	if (error != 0)
		return -error;

	/* Succeeded: reports the valid blocks. */
	return (int)blocks;
}

/*
 * Reads size bytes of an EDID block over the channel's I2C in one access
 * of the sink: the segment pointer for a segment past the first, the
 * offset within the segment, the read.  Returns 0 or a positive error
 * (the DDC of struct i915_xd_edid_ddc).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/drm_edid.c
 * (drm_do_probe_ddc_edid).
 */
static int
i915_dp_ext_edid_block(
	void *context,
	unsigned segment,
	unsigned offset,
	uint8_t *buffer,
	unsigned size)
{
	struct i915_xd_aux_i2c_message messages[3];
	struct i915_dp_ext_access access;
	struct i915_dp_ext_port *p;
	uint8_t segment_byte;
	uint8_t offset_byte;
	unsigned first;
	int error;

	/* The port, and the bytes of the segment and the offset. */
	p = context;
	segment_byte = (uint8_t)segment;
	offset_byte = (uint8_t)offset;

	/* The segment pointer, which only a segment past the first needs. */
	messages[0].address = I915_DP_EXT_KERN_DDC_SEGMENT;
	messages[0].read = 0;
	messages[0].bytes = &segment_byte;
	messages[0].length = 1U;
	first = 1U;
	if (segment != 0U)
		first = 0U;

	/* The offset, then the bytes. */
	messages[1].address = I915_DP_EXT_KERN_DDC_EDID;
	messages[1].read = 0;
	messages[1].bytes = &offset_byte;
	messages[1].length = 1U;
	messages[2].address = I915_DP_EXT_KERN_DDC_EDID;
	messages[2].read = 1;
	messages[2].bytes = buffer;
	messages[2].length = size;

	/* The transaction in one access. */
	error = i915_dp_ext_access_begin(p, &access);
	if (error != 0)
		return -error;

	error = drv_i915_xd_aux_i2c(&p->aux, &p->ext->display->xd.io, &messages[first], 3U - first);

	i915_dp_ext_access_end(p, &access);

	/* Reports a transaction that failed. */
	if (error != 0)
		return error;

	/* Succeeded: the bytes are read. */
	return 0;
}

/* Writes a line of the probe's log to the kernel log. */
static void
i915_dp_ext_log(
	void *ctx,
	const char *format,
	...)
{
	char line[I915_DP_EXT_KERN_LOG_LINE];
	va_list arguments;

	UNUSED_PARAMETER(ctx);

	/* Formats the line and writes it. */
	va_start(arguments, format);
	(void)kern_vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	kern_logf("%s", line);
}

/* Reads DPCD bytes of the port's sink for a screen: refused once the ports stopped. */
static long
i915_dp_ext_emit_dpcd_read(
	void *ctx,
	unsigned offset,
	uint8_t *buffer,
	size_t size)
{
	struct i915_dp_ext_port *p;
	long transferred;

	/* The ports' accesses end with their stop. */
	p = ctx;
	if (!p->ext->live)
		return -(long)EIO;

	/* Reads over the port's channel. */
	transferred = i915_dp_ext_read(p, offset, buffer, size);
	if (transferred < 0)
		return transferred;

	/* Succeeded: reports the bytes read. */
	return transferred;
}

/* Writes DPCD bytes of the port's sink for a screen: refused once the ports stopped. */
static long
i915_dp_ext_emit_dpcd_write(
	void *ctx,
	unsigned offset,
	const uint8_t *buffer,
	size_t size)
{
	struct i915_dp_ext_port *p;
	long transferred;

	/* The ports' accesses end with their stop. */
	p = ctx;
	if (!p->ext->live)
		return -(long)EIO;

	/* Writes over the port's channel. */
	transferred = i915_dp_ext_write(p, offset, buffer, size);
	if (transferred < 0)
		return transferred;

	/* Succeeded: reports the bytes written. */
	return transferred;
}

/* Reads the receiver capabilities of the port's sink for a screen: refused once the ports stopped. */
static int
i915_dp_ext_emit_read_caps(
	void *ctx,
	uint8_t dpcd[15])
{
	struct i915_dp_ext_port *p;
	int error;

	/* The ports' accesses end with their stop. */
	p = ctx;
	if (!p->ext->live)
		return -EIO;

	/* Reads over the port's channel. */
	error = i915_dp_ext_caps(p, dpcd);
	if (error != 0)
		return error;

	/* Succeeded: the capabilities are in dpcd. */
	return 0;
}

/*
 * Sets up the protocol converter of the port's sink for the RGB stream
 * (xd-dp.c, from what the last probe found), with the port's lock held;
 * refused once the ports stopped.
 */
static int
i915_dp_ext_emit_configure_converter(
	void *ctx)
{
	struct i915_dp_ext_port *p;
	int error;

	/* The ports' accesses end with their stop. */
	p = ctx;
	if (!p->ext->live)
		return -EIO;

	/* Sets the converter up from the probe's result. */
	mutex_lock(&p->lock);

	error = drv_i915_xd_dp_probe_configure_converter(&p->env, &p->sink);

	mutex_unlock(&p->lock);

	/* Reports a write that failed. */
	if (error != 0)
		return error;

	/* Succeeded: the converter is set up, or there is none. */
	return 0;
}

/* Finds a declared external DP port of a DDI port: NULL when the ports are not bound or the port is not one. */
static struct i915_dp_ext_port *
i915_dp_ext_declared(
	struct i915_display *display,
	int port)
{
	struct i915_dp_ext_world *ext;
	struct i915_dp_ext_port *p;
	int tc_port;

	/* Without bound external ports there is none. */
	ext = display->dp_ext;
	if (ext == NULL || !ext->live)
		return NULL;

	/* Only a declared Type-C port is one. */
	tc_port = drv_i915_tc_port_of(display, port);
	if (tc_port < 0)
		return NULL;

	p = &ext->port[tc_port];
	if (!p->declared)
		return NULL;

	/* Succeeded: the port. */
	return p;
}
