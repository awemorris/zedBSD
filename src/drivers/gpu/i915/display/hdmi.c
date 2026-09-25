/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The HDMI detection of the hotplug path (see hdmi.h).
 *
 * The forced detect of an HDMI connector follows Linux v6.8.12
 * intel_hdmi.c (intel_hdmi_detect(), intel_hdmi_set_edid(),
 * intel_hdmi_unset_edid()).  What it calls outside i915 is provided here:
 *
 *   drm_edid_read_ddc()          the base block and its extensions over the
 *                                connector's DDC adapter, each block's
 *                                header and checksum checked, through the
 *                                EDID reader of the DP environment (the
 *                                Linux drm_do_probe_ddc_edid()); the EDID
 *                                is kept in the connector's slot of the
 *                                world, where Linux allocates it;
 *   drm_edid_connector_update()  the part the hotplug path depends on: the
 *                                epoch counter moves when a previously
 *                                stored EDID changes; the display info and
 *                                the property are not ported;
 *   drm_edid_is_digital()        EDID byte 20 bit 7 (EDID_INPUT_DIGITAL);
 *   the DP dual-mode probe       (I2C address 0x40) a recorded step finding
 *                                no adaptor.
 *
 * The Linux text this file follows carries this notice:
 *
 * Copyright 2006 Dave Airlie <airlied@linux.ie>
 * Copyright (c) 2006-2009 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *	Eric Anholt <eric@anholt.net>
 *	Jesse Barnes <jesse.barnes@intel.com>
 */

#include "hotplug-internal.h"
#include "hdmi.h"
#include <kern/kcrt.h>

#include <kern/klog.h>

#include <stddef.h>

/* The EDID input byte whose bit 7 marks a digital sink (EDID_INPUT_DIGITAL). */
#define I915_EDID_INPUT_BYTE 20u
#define I915_EDID_INPUT_DIGITAL 0x80u

static unsigned i915_hpd_conn_index(struct drm_connector *connector);
static struct i915_hpd_world *i915_hpd_connector_world(struct drm_connector *connector);
static void i915_hpd_edid_note(struct i915_hpd_world *world, const u8 *block, int blocks, unsigned extensions);
static void i915_hdmi_dp_dual_mode_detect(struct drm_connector *connector);
static void i915_hdmi_unset_edid(struct drm_connector *connector);
static bool i915_hdmi_set_edid(struct drm_connector *connector);
static enum connector_status i915_hdmi_detect(struct drm_connector *connector, bool force);

/*
 * The connector callbacks of an HDMI connector: the forced detect.
 *
 * They never change, so every HDMI connector of every world shares them.
 */
static const struct drm_connector_funcs i915_hpd_hdmi_connector_funcs = {
	i915_hdmi_detect
};

/*
 * Reads a connector's EDID over its DDC adapter into the connector's slot.
 *
 * Returns the EDID, or NULL when there is no adapter or the read failed.
 */
const struct drm_edid *
i915_hpd_drm_edid_read_ddc(
	struct drm_connector *connector,
	struct i2c_adapter *adapter)
{
	struct i915_hpd_world *world;
	struct i915_hpd_edid_slot *slot;
	unsigned idx;
	unsigned ext;
	int rc;

	/* Finds the connector's slot. */
	world = i915_hpd_connector_world(connector);
	idx = i915_hpd_conn_index(connector);
	slot = &world->hpd_edid[idx];
	ext = 0u;

	/* Counts the attempt; a connector without a DDC bus reads nothing. */
	world->hpd_edid_reads++;
	if (adapter == NULL)
		return NULL;

	/* Reads the base block and up to three extensions. */
	rc = drv_i915_drm_edid_read(adapter, slot->buf, I915_HPD_EDID_MAX_BLOCKS, &ext);

	/* Starts the record of this read with what the reader answered. */
	kern_memset(&world->hpd_edid_last, 0, sizeof(world->hpd_edid_last));
	world->hpd_edid_last.rc = rc;

	/* A read with no valid block fails. */
	if (rc < 1) {
		world->hpd_edid_fails++;
		kern_logf("i915: hpd EDID read over %s failed: rc %d\n", adapter->name, rc);
		return NULL;
	}

	/* Hands the blocks read to the Linux text. */
	slot->e.edid = slot->buf;
	slot->e.size = (size_t)rc * 128u;

	/* Records what the base block says and logs it. */
	i915_hpd_edid_note(world, slot->buf, rc, ext);
	kern_logf("i915: hpd EDID %s: %u block(s) (extensions %u) header ok, checksum ok | %s product 0x%04x serial 0x%08x EDID %u.%u %s | DTD1 %ux%u pixel clock %u kHz\n",
		  connector->name,
		  (unsigned)rc,
		  ext,
		  world->hpd_edid_last.mfg,
		  world->hpd_edid_last.product,
		  world->hpd_edid_last.serial,
		  world->hpd_edid_last.version,
		  world->hpd_edid_last.revision,
		  world->hpd_edid_last.digital ? "digital" : "analog",
		  world->hpd_edid_last.hactive,
		  world->hpd_edid_last.vactive,
		  world->hpd_edid_last.pixel_clock_khz);

	/* Succeeded: the EDID lives in the connector's slot. */
	return &slot->e;
}

/*
 * Stores a connector's EDID as its property, moving the epoch counter when
 * a previously stored EDID changed (the Linux
 * _drm_edid_connector_property_update()).
 */
void
i915_hpd_drm_edid_connector_update(
	struct drm_connector *connector,
	const struct drm_edid *drm_edid)
{
	struct i915_hpd_world *world;
	struct i915_hpd_edid_slot *slot;
	unsigned idx;
	size_t size;
	int changed;
	int compared;

	/* Finds the connector's slot and the size of the new EDID. */
	world = i915_hpd_connector_world(connector);
	idx = i915_hpd_conn_index(connector);
	slot = &world->hpd_edid[idx];
	size = 0u;
	if (drm_edid != NULL)
		size = drm_edid->size;

	/* An EDID differs from the stored one in its size or in its bytes. */
	changed = 0;
	if (slot->have_stored) {
		if (size != slot->stored_size) {
			changed = 1;
		} else if (size != 0u) {
			compared = kern_memcmp(drm_edid->edid, slot->stored, size);
			if (compared != 0)
				changed = 1;
		}
	}

	/* A changed EDID is a new epoch of the connector. */
	if (changed) {
		connector->epoch_counter++;
		I915_HPD_DRM_DBG_KMS(connector->dev, "[CONNECTOR:%d:%s] EDID changed, epoch counter %llu\n", connector->base.id, connector->name, (unsigned long long)connector->epoch_counter);
	}

	/* Stores the new EDID as the property. */
	slot->stored_size = size;
	if (size != 0u)
		kern_memcpy(slot->stored, drm_edid->edid, size);
	slot->have_stored = 1;
}

/*
 * Tells whether an EDID describes a digital sink.
 */
bool
i915_hpd_drm_edid_is_digital(
	const struct drm_edid *drm_edid)
{
	/* No EDID is no digital sink. */
	if (drm_edid == NULL)
		return false;
	if (drm_edid->edid == NULL)
		return false;

	/* The input byte says whether the sink is digital. */
	if ((drm_edid->edid[I915_EDID_INPUT_BYTE] & I915_EDID_INPUT_DIGITAL) == 0u)
		return false;

	/* Succeeded: the sink is digital. */
	return true;
}

/*
 * Releases an EDID; the bytes belong to the connector's slot, so nothing is
 * freed.
 */
void
i915_hpd_drm_edid_free(
	const struct drm_edid *drm_edid)
{
	UNUSED_PARAMETER(drm_edid);
}

/*
 * Returns the connector callbacks of an HDMI connector.
 */
const struct drm_connector_funcs *
drv_i915_hpd_hdmi_connector_funcs(void)
{
	/* Succeeded: the callbacks are static and shared. */
	return &i915_hpd_hdmi_connector_funcs;
}

/*
 * Copies out what the last EDID read found, with the read counters.
 */
void
drv_i915_hpd_edid_info(
	struct i915_hpd_world *world,
	struct i915_hpd_edid_info *out)
{
	/* Copies the record, then the counters since the start. */
	*out = world->hpd_edid_last;
	out->reads = world->hpd_edid_reads;
	out->fails = world->hpd_edid_fails;
}

/*
 * Returns the EDID bytes a connector read last and their size.
 *
 * Returns NULL with a zero size when the connector has read none.
 */
const uint8_t *
drv_i915_hpd_edid_bytes(
	struct i915_hpd_world *world,
	unsigned idx,
	unsigned *size)
{
	/* A connector outside the world has no EDID. */
	if (idx >= I915_HPD_MAX_CONNECTORS) {
		*size = 0u;
		return NULL;
	}

	/* A connector that has read nothing has no EDID. */
	if (world->hpd_edid[idx].e.edid == NULL) {
		*size = 0u;
		return NULL;
	}

	/* Succeeded: reports the bytes of the last read. */
	*size = (unsigned)world->hpd_edid[idx].e.size;
	return world->hpd_edid[idx].e.edid;
}

/*
 * Forgets every connector's EDID and the read records, for a new start.
 */
void
drv_i915_hpd_edid_forget(
	struct i915_hpd_world *world)
{
	/* Clears the slots, the last record and the counters. */
	kern_memset(world->hpd_edid, 0, sizeof(world->hpd_edid));
	kern_memset(&world->hpd_edid_last, 0, sizeof(world->hpd_edid_last));
	world->hpd_edid_reads = 0u;
	world->hpd_edid_fails = 0u;
}

/* Returns the slot index of a connector: its id less one. */
static unsigned
i915_hpd_conn_index(
	struct drm_connector *connector)
{
	/* Connector ids are their index plus one; the modulo keeps a stray id inside. */
	return (unsigned)(connector->base.id - 1) % I915_HPD_MAX_CONNECTORS;
}

/* Returns the world a connector's device belongs to. */
static struct i915_hpd_world *
i915_hpd_connector_world(
	struct drm_connector *connector)
{
	struct drm_i915_private *i915;
	struct i915_hpd_world *world;

	/* The connector's drm device is the device the world holds. */
	i915 = i915_hpd_to_i915(connector->dev);
	world = i915_hpd_world_of(i915);

	/* Succeeded: reports the connector's world. */
	return world;
}

/* Records what a base EDID block says in the world's last-read record. */
static void
i915_hpd_edid_note(
	struct i915_hpd_world *world,
	const u8 *block,
	int blocks,
	unsigned extensions)
{
	struct i915_hpd_edid_info *last;

	/* The record every field goes into. */
	last = &world->hpd_edid_last;

	/* The number of blocks read and of extensions the base block announced. */
	last->blocks = (unsigned)blocks;
	last->extensions = extensions;

	/* The manufacturer: three five-bit letters in bytes 8 and 9. */
	last->mfg[0] = (char)('A' - 1 + ((block[8] >> 2) & 0x1f));
	last->mfg[1] = (char)('A' - 1 + (((block[8] & 3) << 3) | (block[9] >> 5)));
	last->mfg[2] = (char)('A' - 1 + (block[9] & 0x1f));

	/* The product code, bytes 10 and 11, little endian. */
	last->product = (unsigned)block[10] | ((unsigned)block[11] << 8);

	/* The serial number, bytes 12 to 15, little endian. */
	last->serial = (unsigned)block[12] | ((unsigned)block[13] << 8) | ((unsigned)block[14] << 16) | ((unsigned)block[15] << 24);

	/* The EDID version and revision. */
	last->version = block[18];
	last->revision = block[19];

	/* Whether the sink is digital (the input byte). */
	last->digital = 0u;
	if ((block[I915_EDID_INPUT_BYTE] & I915_EDID_INPUT_DIGITAL) != 0u)
		last->digital = 1u;

	/* The checksum byte. */
	last->checksum = block[127];

	/* The first detailed timing descriptor (the preferred mode): clock in 10 kHz units. */
	last->pixel_clock_khz = ((unsigned)block[54] | ((unsigned)block[55] << 8)) * 10u;

	/* Its active width and height, low eight bits plus the high nibble. */
	last->hactive = (unsigned)block[56] | (((unsigned)block[58] & 0xf0u) << 4);
	last->vactive = (unsigned)block[59] | (((unsigned)block[61] & 0xf0u) << 4);
}

/* Records the unported DP dual-mode adaptor probe: no adaptor. */
static void
i915_hdmi_dp_dual_mode_detect(
	struct drm_connector *connector)
{
	/* Names the step; the adaptor type stays DRM_DP_DUAL_MODE_NONE. */
	kern_logf("i915: hpd step intel_hdmi_dp_dual_mode_detect (DP dual-mode probe not ported): %s -> none\n", connector->name);
}

/* Forgets the EDID and the dual-mode adaptor of an HDMI connector. */
static void
i915_hdmi_unset_edid(
	struct drm_connector *connector)
{
	struct intel_connector *intel_connector;
	struct intel_hdmi *intel_hdmi;

	/* Resolves the HDMI port the connector is attached to. */
	intel_connector = i915_hpd_to_intel_connector(connector);
	intel_hdmi = intel_attached_hdmi(intel_connector);

	/* No dual-mode adaptor is known any more. */
	intel_hdmi->dp_dual_mode.type = DRM_DP_DUAL_MODE_NONE;
	intel_hdmi->dp_dual_mode.max_tmds_clock = 0;

	/* Releases and forgets the EDID of the last detection. */
	i915_hpd_drm_edid_free(intel_connector->detect_edid);
	intel_connector->detect_edid = NULL;
}

/*
 * Reads an HDMI connector's EDID over GMBUS, retrying over bit-banging,
 * and reports whether a digital sink answered.
 */
static bool
i915_hdmi_set_edid(
	struct drm_connector *connector)
{
	struct drm_i915_private *dev_priv;
	struct intel_connector *intel_connector;
	struct intel_hdmi *intel_hdmi;
	struct i2c_adapter *ddc;
	intel_wakeref_t wakeref;
	const struct drm_edid *drm_edid;
	bool connected;
	bool forced;
	bool digital;

	/* Resolves the device, the HDMI port and the DDC bus. */
	dev_priv = i915_hpd_to_i915(connector->dev);
	intel_connector = i915_hpd_to_intel_connector(connector);
	intel_hdmi = intel_attached_hdmi(intel_connector);
	ddc = connector->ddc;
	connected = false;

	/* Holds the GMBUS power domain across the reads. */
	wakeref = drv_i915_hpd_intel_display_power_get(dev_priv, POWER_DOMAIN_GMBUS);

	/* Reads the EDID over GMBUS. */
	drm_edid = i915_hpd_drm_edid_read_ddc(connector, ddc);

	/* A failed GMBUS read is retried once over bit-banging. */
	if (drm_edid == NULL) {
		forced = i915_hpd_intel_gmbus_is_forced_bit(ddc);
		if (!forced) {
			I915_HPD_DRM_DBG_KMS(&dev_priv->drm, "HDMI GMBUS EDID read failed, retry using GPIO bit-banging\n");
			i915_hpd_intel_gmbus_force_bit(ddc, true);
			drm_edid = i915_hpd_drm_edid_read_ddc(connector, ddc);
			i915_hpd_intel_gmbus_force_bit(ddc, false);
		}
	}

	/* Below we depend on display info having been updated */
	i915_hpd_drm_edid_connector_update(connector, drm_edid);

	/* The EDID is the one of this detection. */
	intel_connector->detect_edid = drm_edid;

	/* A digital sink is connected; its dual-mode adaptor is probed. */
	digital = i915_hpd_drm_edid_is_digital(drm_edid);
	if (digital) {
		i915_hdmi_dp_dual_mode_detect(connector);

		/* The sink is connected. */
		connected = true;
	}

	/* Drops the power reference. */
	drv_i915_hpd_intel_display_power_put(dev_priv, POWER_DOMAIN_GMBUS, wakeref);

	/* Publishes the sink's CEC physical address (not ported: nothing is published). */
	cec_notifier_set_phys_addr(intel_hdmi->cec_notifier, connector->display_info.source_physical_address);

	/* Reports whether a digital sink answered. */
	return connected;
}

/*
 * Detects an HDMI sink: the live status of the port, then the EDID.
 *
 * It is the forced detect of the HDMI connector callbacks.
 */
static enum connector_status
i915_hdmi_detect(
	struct drm_connector *connector,
	bool force)
{
	enum connector_status status;
	struct drm_i915_private *dev_priv;
	struct intel_connector *intel_connector;
	struct intel_hdmi *intel_hdmi;
	struct intel_encoder *encoder;
	intel_wakeref_t wakeref;
	int version;
	bool live;
	bool connected;

	UNUSED_PARAMETER(force);

	/* Resolves the device, the HDMI port and its encoder. */
	status = connector_status_disconnected;
	dev_priv = i915_hpd_to_i915(connector->dev);
	intel_connector = i915_hpd_to_intel_connector(connector);
	intel_hdmi = intel_attached_hdmi(intel_connector);
	encoder = &hdmi_to_dig_port(intel_hdmi)->base;

	/* Logs the connector being detected. */
	I915_HPD_DRM_DBG_KMS(&dev_priv->drm, "[CONNECTOR:%d:%s]\n", connector->base.id, connector->name);

	/* A disabled display detects nothing. */
	if (!intel_display_device_enabled(dev_priv))
		return connector_status_disconnected;

	/* Holds the GMBUS power domain across the detection. */
	wakeref = drv_i915_hpd_intel_display_power_get(dev_priv, POWER_DOMAIN_GMBUS);

	/*
	 * From display version 11 the port's live status decides first: a
	 * port with nothing plugged in is disconnected without an EDID read.
	 */
	live = true;
	version = i915_hpd_display_ver(dev_priv);
	if (version >= 11)
		live = drv_i915_hpd_intel_digital_port_connected(encoder);

	/* A live port is connected when a digital sink answers with its EDID. */
	if (live) {
		i915_hdmi_unset_edid(connector);

		/* Reads the EDID. */
		connected = i915_hdmi_set_edid(connector);
		if (connected)
			status = connector_status_connected;
	}

	/* Drops the power reference. */
	drv_i915_hpd_intel_display_power_put(dev_priv, POWER_DOMAIN_GMBUS, wakeref);

	/* A sink that is not connected has no CEC physical address (not ported). */
	if (status != connector_status_connected)
		cec_notifier_phys_addr_invalidate(intel_hdmi->cec_notifier);

	/* Succeeded: reports the connector status. */
	return status;
}
