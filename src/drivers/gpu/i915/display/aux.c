/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The DP AUX channel of the eDP port.
 *
 * The transfer is the Linux v6.8.12 drivers/gpu/drm/i915/display/
 * intel_dp_aux.c (sha256 f4a5dff0764804bdc7b5a57f79b11d3f10dfa8869f09f0df37f6fa999e777925)
 * rewritten in this tree's style: intel_dp_aux_xfer(), intel_dp_aux_transfer(),
 * the pack, unpack and header helpers, the SKL+ send-control word and the
 * TGL+ register selectors, with their register accesses, retries, waits and
 * power references unchanged.  The other platforms' clock dividers, control
 * words and register selectors are not ported; drv_i915_dp_aux_init() binds
 * the ones of this platform, as the Linux intel_dp_aux_init() picks them for
 * display versions 12 and 13.
 *
 * The file also holds the DPCD hooks the modeset reaches the live eDP's AUX
 * channel through (struct i915_lcd_emit).
 *
 * The messages of the Linux text show the format text only and never
 * evaluate their arguments (dp-internal.h); a transfer the channel does not
 * finish is reported with its values instead (i915_dp_aux_report()).
 *
 * The Linux original is under the MIT licence:
 *
 * Copyright (C) 2020-2021 Intel Corporation
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
 */

#include "dp-internal.h"
#include "aux.h"
#include "dp-sink.h"

#include <kern/klog.h>

/* The bytes of an AUX request header without, and with, the length byte. */
#define BARE_ADDRESS_SIZE	3
#define HEADER_SIZE		(BARE_ADDRESS_SIZE + 1)

/*
 * The AUX power wells' BIOS and driver request registers (the Linux
 * ICL_PWR_WELL_CTL_AUX1 and _AUX2), read into a failed transfer's report:
 * two bits a well, the state at 2n and the request at 2n + 1.
 */
#define I915_DP_AUX_WELLS_BIOS		0x45440u
#define I915_DP_AUX_WELLS_DRIVER	0x45444u

/*
 * The main power wells' driver requests (HSW_PWR_WELL_CTL2: PW_1 at bits 0
 * and 1, PW_2 at 2 and 3, PW_A to D after them) and the DC state enable
 * (DC_STATE_EN), also in the report (BUG-256: an AUX channel needs PW_2 up
 * and the display out of DC5 and DC6).
 */
#define I915_DP_MAIN_WELLS_DRIVER	0x45404u
#define I915_DP_DC_STATE_EN		0x45504u

/* The failed transfers reported in full: the first ones, then one of every so many. */
#define I915_DP_AUX_REPORTS_FIRST	8u
#define I915_DP_AUX_REPORTS_EVERY	64u

/*
 * How many failed AUX transfers were reported, of every port.
 *
 * It only grows, under the AUX mutex the transfers hold; it decides which
 * failures are logged in full, so a sink that never answers does not flood
 * the log.
 */
static unsigned i915_dp_aux_reports;

static u32 i915_dp_aux_pack(const u8 *src, int src_bytes);
static void i915_dp_aux_unpack(u32 src, u8 *dst, int dst_bytes);
static u32 i915_dp_aux_wait_done(struct intel_dp *intel_dp);
static u32 i915_skl_get_aux_clock_divider(struct intel_dp *intel_dp, int index);
static int i915_dp_aux_sync_len(void);
static int i915_dp_aux_fw_sync_len(struct intel_dp *intel_dp);
static u32 i915_skl_get_aux_send_ctl(struct intel_dp *intel_dp, int send_bytes, u32 unused);
static u32 i915_dp_aux_send(struct intel_dp *intel_dp, struct drm_i915_private *i915, i915_reg_t ch_ctl, const i915_reg_t *ch_data, const u8 *send, int send_bytes, u32 aux_send_ctl_flags, u32 status);
static int i915_dp_aux_xfer_locked(struct intel_dp *intel_dp, struct drm_i915_private *i915, i915_reg_t ch_ctl, const i915_reg_t *ch_data, const u8 *send, int send_bytes, u8 *recv, int recv_size, u32 aux_send_ctl_flags);
static int i915_dp_aux_xfer(struct intel_dp *intel_dp, const u8 *send, int send_bytes, u8 *recv, int recv_size, u32 aux_send_ctl_flags);
static void i915_dp_aux_header(u8 txbuf[HEADER_SIZE], const struct drm_dp_aux_msg *msg);
static u32 i915_dp_aux_xfer_flags(const struct drm_dp_aux_msg *msg);
static i915_dp_ssize_t i915_dp_aux_transfer(struct drm_dp_aux *aux, struct drm_dp_aux_msg *msg);
static i915_reg_t i915_tgl_aux_ctl_reg(struct intel_dp *intel_dp);
static i915_reg_t i915_tgl_aux_data_reg(struct intel_dp *intel_dp, int index);
static struct i915_dp_world *i915_aux_emit_world(void *ctx);
static void i915_dp_aux_report(struct intel_dp *intel_dp, struct drm_i915_private *i915, i915_reg_t ch_ctl, const char *what, u32 status);

/*
 * Binds the AUX channel of an eDP port to this platform's hardware (the
 * Linux intel_dp_aux_init() for display versions 12 and 13).
 *
 * The TGL register selectors and the SKL clock divider and send-control word
 * are chosen, the channel's mutex and DDC adapter are prepared, and the
 * transfer hook is set.
 */
void
drv_i915_dp_aux_init(
	struct intel_dp *intel_dp)
{
	/* Picks the TGL+ control and data registers. */
	intel_dp->aux_ch_ctl_reg = i915_tgl_aux_ctl_reg;
	intel_dp->aux_ch_data_reg = i915_tgl_aux_data_reg;

	/* Picks the SKL+ clock divider and send-control word. */
	intel_dp->get_aux_clock_divider = i915_skl_get_aux_clock_divider;
	intel_dp->get_aux_send_ctl = i915_skl_get_aux_send_ctl;

	/* Prepares the channel's mutex and DDC adapter, then sets the transfer hook. */
	drv_i915_drm_dp_aux_init(&intel_dp->aux);
	intel_dp->aux.transfer = i915_dp_aux_transfer;
}

/*
 * Reads DPCD bytes of the live eDP for the modeset (drm_dp_dpcd_read()).
 *
 * It is the dpcd_read hook of struct i915_lcd_emit; ctx is the panel run
 * (struct i915_lcd_kernel).  It returns the bytes transferred, or a negative
 * Linux errno (-I915_EDP_EINVAL without a live eDP).
 */
long
drv_i915_edp_emit_dpcd_read(
	void *ctx,
	unsigned offset,
	uint8_t *buf,
	size_t size)
{
	struct i915_dp_world *world;
	long transferred;

	/* Finds the eDP world of the run's panel. */
	world = i915_aux_emit_world(ctx);

	/* Reads the bytes over the live AUX channel. */
	transferred = drv_i915_edp_dpcd_read(world, offset, buf, size);
	if (transferred < 0)
		return transferred;

	/* Succeeded: reports the bytes read. */
	return transferred;
}

/*
 * Writes DPCD bytes of the live eDP for the modeset (drm_dp_dpcd_write()).
 *
 * It is the dpcd_write hook of struct i915_lcd_emit, as
 * drv_i915_edp_emit_dpcd_read().
 */
long
drv_i915_edp_emit_dpcd_write(
	void *ctx,
	unsigned offset,
	const uint8_t *buf,
	size_t size)
{
	struct i915_dp_world *world;
	long transferred;

	/* Finds the eDP world of the run's panel. */
	world = i915_aux_emit_world(ctx);

	/* Writes the bytes over the live AUX channel. */
	transferred = drv_i915_edp_dpcd_write(world, offset, buf, size);
	if (transferred < 0)
		return transferred;

	/* Succeeded: reports the bytes written. */
	return transferred;
}

/*
 * Reads the receiver capabilities of the live eDP for the modeset
 * (drm_dp_read_dpcd_caps()).
 *
 * It is the read_dpcd_caps hook of struct i915_lcd_emit; it returns 0, or a
 * negative Linux errno.
 */
int
drv_i915_edp_emit_read_dpcd_caps(
	void *ctx,
	uint8_t dpcd[15])
{
	struct i915_dp_world *world;
	int read;

	/* Finds the eDP world of the run's panel. */
	world = i915_aux_emit_world(ctx);

	/* Reads the capabilities, with the extended field, over the live AUX channel. */
	read = drv_i915_edp_read_dpcd_caps(world, dpcd);
	if (read != 0)
		return read;

	/* Succeeded: the capabilities are in dpcd. */
	return 0;
}

/* Packs up to four bytes into an AUX data register word, first byte highest. */
static u32
i915_dp_aux_pack(
	const u8 *src,
	int src_bytes)
{
	int i;
	u32 v;

	/* One register holds four bytes. */
	if (src_bytes > 4)
		src_bytes = 4;

	/* Places each byte from the top of the word down. */
	v = 0;
	for (i = 0; i < src_bytes; i++)
		v |= ((u32)src[i]) << ((3 - i) * 8);

	/* Succeeded: reports the packed word. */
	return v;
}

/* Unpacks up to four bytes from an AUX data register word, first byte highest. */
static void
i915_dp_aux_unpack(
	u32 src,
	u8 *dst,
	int dst_bytes)
{
	int i;

	/* One register holds four bytes. */
	if (dst_bytes > 4)
		dst_bytes = 4;

	/* Takes each byte from the top of the word down. */
	for (i = 0; i < dst_bytes; i++)
		dst[i] = src >> ((3 - i) * 8);
}

/*
 * Waits up to 10 ms for the channel to finish sending and reports its
 * status.
 */
static u32
i915_dp_aux_wait_done(
	struct intel_dp *intel_dp)
{
	struct drm_i915_private *i915;
	i915_reg_t ch_ctl;
	const unsigned int timeout_ms = 10;
	u32 status;
	int waited;

	/* Finds the device and the channel's control register. */
	i915 = i915_dp_dp_to_i915(intel_dp);
	ch_ctl = intel_dp->aux_ch_ctl_reg(intel_dp);

	/* Waits for SEND_BUSY to clear: 2 us spinning, then sleeping up to the timeout. */
	waited = __intel_de_wait_for_register(i915, ch_ctl,
					      DP_AUX_CH_CTL_SEND_BUSY, 0,
					      2, timeout_ms, &status);

	/* Reports a transfer that did not complete within the 10 ms. */
	if (waited == -I915_DP_ETIMEDOUT)
		i915_dp_aux_report(intel_dp, i915, ch_ctl, "did not complete within 10 ms", status);

	/* Succeeded: reports the last status read. */
	return status;
}

/*
 * Reports the AUX clock divider for a try (SKL+).
 *
 * The hardware derives the AUX clock from CDCLK itself; the divider only
 * makes the transfer loop run once.
 */
static u32
i915_skl_get_aux_clock_divider(
	struct intel_dp *intel_dp,
	int index)
{
	UNUSED_PARAMETER(intel_dp);

	/* One try with a nonzero divider, then the end of the list. */
	if (index != 0)
		return 0;

	/* Succeeded: the first and only divider. */
	return 1;
}

/* Reports the AUX SYNC length: the precharge (10-16) and the preamble. */
static int
i915_dp_aux_sync_len(void)
{
	int precharge;
	int preamble;

	/* The longest precharge and the preamble. */
	precharge = 16;
	preamble = 16;

	/* Succeeded: reports the length. */
	return precharge + preamble;
}

/*
 * Reports the Fast Wake SYNC length: the precharge (10-16) and the
 * preamble.
 *
 * One MTL laptop's panel needs two more precharge pulses; that quirk does
 * not apply here.
 */
static int
i915_dp_aux_fw_sync_len(
	struct intel_dp *intel_dp)
{
	int precharge;
	int preamble;
	bool quirk;

	UNUSED_PARAMETER(intel_dp);

	/* The hardware default precharge and the preamble. */
	precharge = 10;
	preamble = 8;

	/* The Dell Precision 5490 panel quirk lengthens the precharge. */
	quirk = intel_has_dpcd_quirk(intel_dp, QUIRK_FW_SYNC_LEN);
	if (quirk)
		precharge += 2;

	/* Succeeded: reports the length. */
	return precharge + preamble;
}

/* Builds the AUX control word that sends a message (SKL+). */
static u32
i915_skl_get_aux_send_ctl(
	struct intel_dp *intel_dp,
	int send_bytes,
	u32 unused)
{
	struct intel_digital_port *dig_port;
	struct drm_i915_private *i915;
	u32 ret;
	bool tbt;
	int display_ver;

	UNUSED_PARAMETER(unused);

	/* Finds the port and the device. */
	dig_port = i915_dp_dp_to_dig_port(intel_dp);
	i915 = i915_dp_to_i915(dig_port->base.base.dev);

	/* Sends, clears the status bits, and waits the longest timeout (4 ms on ICL+). */
	ret = DP_AUX_CH_CTL_SEND_BUSY |
		DP_AUX_CH_CTL_DONE |
		DP_AUX_CH_CTL_INTERRUPT |
		DP_AUX_CH_CTL_TIME_OUT_ERROR |
		DP_AUX_CH_CTL_TIME_OUT_MAX |
		DP_AUX_CH_CTL_RECEIVE_ERROR |
		DP_AUX_CH_CTL_MESSAGE_SIZE(send_bytes) |
		DP_AUX_CH_CTL_FW_SYNC_PULSE_SKL(i915_dp_aux_fw_sync_len(intel_dp)) |
		DP_AUX_CH_CTL_SYNC_PULSE_SKL(i915_dp_aux_sync_len());

	/* A Type-C port in TBT-alt mode sends through the TBT I/O. */
	tbt = i915_dp_intel_tc_port_in_tbt_alt_mode(dig_port);
	if (tbt)
		ret |= DP_AUX_CH_CTL_TBT_IO;

	/*
	 * Keeps the power request bit the AUX power well enable set (display 14
	 * and later).
	 *
	 * XXX: the display version is the DP environment's fixed 13.
	 */
	display_ver = i915_vbt_display_ver(i915);
	if (display_ver >= 14)
		ret |= XELPDP_DP_AUX_CH_CTL_POWER_REQUEST;

	/* Succeeded: reports the control word. */
	return ret;
}

/*
 * Sends a message until the channel reports it done.
 *
 * For each clock divider the message is sent up to five times (the DP
 * specification asks for at least three).  A timeout error goes straight to
 * the next try, because the hardware timeout already covers the 400 us the
 * DP CTS requires; a receive error waits 400 us first.  The last status is
 * reported, starting from the status the caller last read.
 */
static u32
i915_dp_aux_send(
	struct intel_dp *intel_dp,
	struct drm_i915_private *i915,
	i915_reg_t ch_ctl,
	const i915_reg_t *ch_data,
	const u8 *send,
	int send_bytes,
	u32 aux_send_ctl_flags,
	u32 status)
{
	u32 aux_clock_divider;
	u32 send_ctl;
	int clock;
	int try;
	int i;

	/* Goes through the clock dividers until the list ends. */
	clock = 0;
	for (;;) {
		aux_clock_divider = intel_dp->get_aux_clock_divider(intel_dp, clock);
		clock++;
		if (aux_clock_divider == 0)
			break;

		/* Builds the control word that sends the message. */
		send_ctl = intel_dp->get_aux_send_ctl(intel_dp, send_bytes, aux_clock_divider);
		send_ctl |= aux_send_ctl_flags;

		/* Sends the message up to five times. */
		for (try = 0; try < 5; try++) {
			/* Loads the send data into the channel's data registers. */
			for (i = 0; i < send_bytes; i += 4)
				i915_dp_intel_de_write(i915, ch_data[i >> 2], i915_dp_aux_pack(send + i, send_bytes - i));

			/* Sends the command and waits for it to complete. */
			i915_dp_intel_de_write(i915, ch_ctl, send_ctl);

			status = i915_dp_aux_wait_done(intel_dp);

			/* Clears the done status and any errors. */
			i915_dp_intel_de_write(i915,
					       ch_ctl,
					       status | DP_AUX_CH_CTL_DONE | DP_AUX_CH_CTL_TIME_OUT_ERROR | DP_AUX_CH_CTL_RECEIVE_ERROR);

			/* A timeout already waited long enough: tries again at once. */
			if (status & DP_AUX_CH_CTL_TIME_OUT_ERROR)
				continue;

			/* A receive error waits 400 us before the next try. */
			if (status & DP_AUX_CH_CTL_RECEIVE_ERROR) {
				i915_dp_usleep_range(400, 500);
				continue;
			}

			/* The channel is done with the message. */
			if (status & DP_AUX_CH_CTL_DONE)
				return status;
		}
	}

	/* Reports the status of the last try. */
	return status;
}

/*
 * Runs one AUX message with the channel's power, the PPS lock and VDD held.
 *
 * It returns the bytes received, or a negative Linux errno: -EBUSY when the
 * channel stays busy, does not finish or reports a forbidden size (the DRM
 * layer then retries), -E2BIG for a message larger than the five data
 * registers, -EIO for a receive error and -ETIMEDOUT when the sink did not
 * answer.
 */
static int
i915_dp_aux_xfer_locked(
	struct intel_dp *intel_dp,
	struct drm_i915_private *i915,
	i915_reg_t ch_ctl,
	const i915_reg_t *ch_data,
	const u8 *send,
	int send_bytes,
	u8 *recv,
	int recv_size,
	u32 aux_send_ctl_flags)
{
	u32 status;
	u32 last_status;
	int recv_bytes;
	int try;
	int i;
	int too_big;

	/* Waits for any previous AUX activity to end: three reads 1 ms apart. */
	status = 0;
	for (try = 0; try < 3; try++) {
		status = i915_dp_intel_de_read(i915, ch_ctl);
		if ((status & DP_AUX_CH_CTL_SEND_BUSY) == 0)
			break;

		i915_dp_msleep(1);
	}

	/* Reports a channel still busy, once per new status. */
	if (try == 3) {
		last_status = i915_dp_intel_de_read(i915, ch_ctl);

		if (last_status != intel_dp->aux_busy_last_status) {
			(void)I915_DP_DRM_WARN(&i915->drm, 1,
					       "%s: not started (status 0x%08x)\n",
					       intel_dp->aux.name, last_status);
			intel_dp->aux_busy_last_status = last_status;
		}

		return -I915_DP_EBUSY;
	}

	/* The channel has only five data registers. */
	too_big = I915_DP_DRM_WARN_ON(&i915->drm, send_bytes > 20 || recv_size > 20);
	if (too_big)
		return -I915_DP_E2BIG;

	/* Sends the message until the channel is done or the tries run out. */
	status = i915_dp_aux_send(intel_dp, i915, ch_ctl, ch_data, send, send_bytes, aux_send_ctl_flags, status);

	/* Reports a message the channel never finished. */
	if ((status & DP_AUX_CH_CTL_DONE) == 0) {
		i915_dp_aux_report(intel_dp, i915, ch_ctl, "not done", status);
		return -I915_DP_EBUSY;
	}

	/* Reports a receive error. */
	if (status & DP_AUX_CH_CTL_RECEIVE_ERROR) {
		i915_dp_aux_report(intel_dp, i915, ch_ctl, "receive error", status);
		return -I915_DP_EIO;
	}

	/* Reports a timeout, which is normal when nothing is connected, only at debug level. */
	if (status & DP_AUX_CH_CTL_TIME_OUT_ERROR) {
		I915_DP_DRM_DBG_KMS(&i915->drm, "%s: timeout (status 0x%08x)\n",
				    intel_dp->aux.name, status);
		return -I915_DP_ETIMEDOUT;
	}

	/*
	 * Refuses a received size of 0 or more than 20, which BSpec forbids;
	 * -EBUSY lets the DRM layer retry.
	 */
	recv_bytes = REG_FIELD_GET(DP_AUX_CH_CTL_MESSAGE_SIZE_MASK, status);
	if (recv_bytes == 0 || recv_bytes > 20) {
		I915_DP_DRM_DBG_KMS(&i915->drm,
				    "%s: Forbidden recv_bytes = %d on aux transaction\n",
				    intel_dp->aux.name, recv_bytes);
		return -I915_DP_EBUSY;
	}

	/* Keeps no more than the caller has room for. */
	if (recv_bytes > recv_size)
		recv_bytes = recv_size;

	/* Unloads the bytes the sink sent back. */
	for (i = 0; i < recv_bytes; i += 4)
		i915_dp_aux_unpack(i915_dp_intel_de_read(i915, ch_data[i >> 2]), recv + i, recv_bytes - i);

	/* Succeeded: reports the bytes received. */
	return recv_bytes;
}

/*
 * Runs one AUX message on the port (the Linux intel_dp_aux_xfer()).
 *
 * The AUX power domain is taken, then the PPS lock, and VDD is forced on
 * for the message when it was not on already; all three are given back in
 * reverse order, the power reference asynchronously.  It returns the bytes
 * received, or a negative Linux errno.
 */
static int
i915_dp_aux_xfer(
	struct intel_dp *intel_dp,
	const u8 *send,
	int send_bytes,
	u8 *recv,
	int recv_size,
	u32 aux_send_ctl_flags)
{
	struct intel_digital_port *dig_port;
	struct drm_i915_private *i915;
	enum phy phy;
	bool is_tc_port;
	bool connected;
	i915_reg_t ch_ctl;
	i915_reg_t ch_data[5];
	int aux_domain;
	intel_wakeref_t aux_wakeref;
	intel_wakeref_t pps_wakeref;
	int i;
	int transferred;
	bool vdd;

	/* Finds the port, the device, and whether the port is Type-C. */
	dig_port = i915_dp_dp_to_dig_port(intel_dp);
	i915 = i915_dp_to_i915(dig_port->base.base.dev);
	phy = i915_vbt_intel_port_to_phy(i915, dig_port->base.port);
	is_tc_port = i915_vbt_intel_phy_is_tc(i915, phy);

	/* Picks the channel's control and data registers. */
	ch_ctl = intel_dp->aux_ch_ctl_reg(intel_dp);
	for (i = 0; i < (int)ARRAY_SIZE(ch_data); i++)
		ch_data[i] = intel_dp->aux_ch_data_reg(intel_dp, i);

	/*
	 * Aborts a transfer on a disconnected Type-C port, as DP 1.4a link CTS
	 * 4.2.1.5 requires, which also avoids the long AUX timeouts.
	 */
	if (is_tc_port) {
		intel_tc_port_lock(dig_port);

		connected = intel_tc_port_connected_locked(&dig_port->base);
		if (!connected) {
			intel_tc_port_unlock(dig_port);
			return -I915_DP_ENXIO;
		}
	}

	/* Powers the channel and takes the PPS lock. */
	aux_domain = i915_aux_power_domain(dig_port);

	aux_wakeref = i915_dp_intel_display_power_get(i915, aux_domain);
	pps_wakeref = drv_i915_pps_lock(intel_dp);

	/*
	 * Forces VDD on unless it already is (DPCD, EDID and OUI reads come
	 * with VDD on and leave it to the upper layers to turn it off).
	 */
	vdd = drv_i915_pps_vdd_on_unlocked(intel_dp);

	/* Asks for the lowest wake-up latency: AUX is very sensitive to IRQ latency. */
	cpu_latency_qos_update_request(&intel_dp->pm_qos, 0);

	/* Warns when the panel has neither power nor VDD. */
	drv_i915_pps_check_power_unlocked(intel_dp);

	/* Runs the message. */
	transferred = i915_dp_aux_xfer_locked(intel_dp, i915, ch_ctl, ch_data, send, send_bytes, recv, recv_size, aux_send_ctl_flags);

	/* Gives back the latency request, VDD, the lock and the power, in reverse order. */
	cpu_latency_qos_update_request(&intel_dp->pm_qos, PM_QOS_DEFAULT_VALUE);

	if (vdd)
		drv_i915_pps_vdd_off_unlocked(intel_dp, false);

	(void)drv_i915_pps_unlock(intel_dp, pps_wakeref);
	i915_dp_intel_display_power_put_async(i915, aux_domain, aux_wakeref);

	/* Releases the Type-C port. */
	if (is_tc_port)
		intel_tc_port_unlock(dig_port);

	/* Reports a failed message. */
	if (transferred < 0)
		return transferred;

	/* Succeeded: reports the bytes received. */
	return transferred;
}

/* Builds the four-byte AUX request header: request, address and length. */
static void
i915_dp_aux_header(
	u8 txbuf[HEADER_SIZE],
	const struct drm_dp_aux_msg *msg)
{
	/* The request code and the top address bits. */
	txbuf[0] = (msg->request << 4) | ((msg->address >> 16) & 0xf);

	/* The middle and low address bytes. */
	txbuf[1] = (msg->address >> 8) & 0xff;
	txbuf[2] = msg->address & 0xff;

	/* The length, less one. */
	txbuf[3] = msg->size - 1;
}

/*
 * Reports the extra control bits of a message.
 *
 * Writing the HDCP Aksv needs the Aksv select bit, which makes the hardware
 * send the Aksv, which software cannot read, after the header.
 */
static u32
i915_dp_aux_xfer_flags(
	const struct drm_dp_aux_msg *msg)
{
	/* A native write to the Aksv address selects the Aksv. */
	if ((msg->request & ~DP_AUX_I2C_MOT) == DP_AUX_NATIVE_WRITE &&
	    msg->address == DP_AUX_HDCP_AKSV)
		return DP_AUX_CH_CTL_AUX_AKSV_SELECT;

	/* Succeeded: no extra bits. */
	return 0;
}

/*
 * Runs one AUX transaction for the DRM helpers (the Linux
 * intel_dp_aux_transfer()).
 *
 * It returns the payload bytes transferred with msg->reply set, or a
 * negative Linux errno.
 */
static i915_dp_ssize_t
i915_dp_aux_transfer(
	struct drm_dp_aux *aux,
	struct drm_dp_aux_msg *msg)
{
	struct intel_dp *intel_dp;
	u8 txbuf[20];
	u8 rxbuf[20];
	size_t txsize;
	size_t rxsize;
	u32 flags;
	int ret;
	int too_big;

	/*
	 * Finds the port and the message's extra bits.  The warnings below name
	 * no device: the message macros never read it.
	 */
	intel_dp = container_of(aux, struct intel_dp, aux);
	flags = i915_dp_aux_xfer_flags(msg);

	/* Builds the header. */
	i915_dp_aux_header(txbuf, msg);

	/* Sends a write with its data, or a read, as the request asks. */
	switch (msg->request & ~DP_AUX_I2C_MOT) {
	case DP_AUX_NATIVE_WRITE:
	case DP_AUX_I2C_WRITE:
	case DP_AUX_I2C_WRITE_STATUS_UPDATE:
		/* The header and the data, or a bare address; the reply has 0 or 1 data bytes. */
		if (msg->size != 0) {
			txsize = HEADER_SIZE + msg->size;
		} else {
			txsize = BARE_ADDRESS_SIZE;
		}
		rxsize = 2;

		too_big = I915_DP_DRM_WARN_ON(NULL, txsize > 20);
		if (too_big)
			return -I915_DP_E2BIG;

		(void)I915_DP_DRM_WARN_ON(NULL, !msg->buffer != !msg->size);

		/* Appends the data to the header. */
		if (msg->buffer)
			i915_dp_memcpy(txbuf + HEADER_SIZE, msg->buffer, msg->size);

		/* Sends the write; the reply says how much was taken. */
		ret = i915_dp_aux_xfer(intel_dp, txbuf, txsize, rxbuf, rxsize, flags);
		if (ret > 0) {
			msg->reply = rxbuf[0] >> 4;

			if (ret > 1) {
				/* The number of bytes written in a short write. */
				ret = I915_DP_CLAMP_T(int, rxbuf[1], 0, msg->size);
			} else {
				/* The whole payload was written. */
				ret = msg->size;
			}
		}
		break;

	case DP_AUX_NATIVE_READ:
	case DP_AUX_I2C_READ:
		/* The header, or a bare address; the reply has the data after one reply byte. */
		if (msg->size != 0) {
			txsize = HEADER_SIZE;
		} else {
			txsize = BARE_ADDRESS_SIZE;
		}
		rxsize = msg->size + 1;

		too_big = I915_DP_DRM_WARN_ON(NULL, rxsize > 20);
		if (too_big)
			return -I915_DP_E2BIG;

		/*
		 * Sends the read and copies the data, assuming a happy day: the
		 * caller checks msg->reply before touching it.
		 */
		ret = i915_dp_aux_xfer(intel_dp, txbuf, txsize, rxbuf, rxsize, flags);
		if (ret > 0) {
			msg->reply = rxbuf[0] >> 4;

			ret--;
			i915_dp_memcpy(msg->buffer, rxbuf + 1, ret);
		}
		break;

	default:
		ret = -I915_DP_EINVAL;
		break;
	}

	/* Reports a failed transaction. */
	if (ret < 0)
		return ret;

	/* Succeeded: reports the payload bytes. */
	return ret;
}

/* Reports the AUX control register of the port's channel (TGL+). */
static i915_reg_t
i915_tgl_aux_ctl_reg(
	struct intel_dp *intel_dp)
{
	struct intel_digital_port *dig_port;
	enum aux_ch aux_ch;

	/* Finds the port's channel. */
	dig_port = i915_dp_dp_to_dig_port(intel_dp);
	aux_ch = dig_port->aux_ch;

	/* Picks the channel's register; an unknown channel falls back to A. */
	switch (aux_ch) {
	case AUX_CH_A:
	case AUX_CH_B:
	case AUX_CH_C:
	case AUX_CH_USBC1:
	case AUX_CH_USBC2:
	case AUX_CH_USBC3:
	case AUX_CH_USBC4:
	case AUX_CH_USBC5:
	case AUX_CH_USBC6:
		/* USBC5 and USBC6 are also AUX_CH_D_XELPD and AUX_CH_E_XELPD. */
		return DP_AUX_CH_CTL(aux_ch);
	default:
		I915_DP_MISSING_CASE(aux_ch);
		return DP_AUX_CH_CTL(AUX_CH_A);
	}
}

/* Reports one AUX data register of the port's channel (TGL+). */
static i915_reg_t
i915_tgl_aux_data_reg(
	struct intel_dp *intel_dp,
	int index)
{
	struct intel_digital_port *dig_port;
	enum aux_ch aux_ch;

	/* Finds the port's channel. */
	dig_port = i915_dp_dp_to_dig_port(intel_dp);
	aux_ch = dig_port->aux_ch;

	/* Picks the channel's register; an unknown channel falls back to A. */
	switch (aux_ch) {
	case AUX_CH_A:
	case AUX_CH_B:
	case AUX_CH_C:
	case AUX_CH_USBC1:
	case AUX_CH_USBC2:
	case AUX_CH_USBC3:
	case AUX_CH_USBC4:
	case AUX_CH_USBC5:
	case AUX_CH_USBC6:
		/* USBC5 and USBC6 are also AUX_CH_D_XELPD and AUX_CH_E_XELPD. */
		return DP_AUX_CH_DATA(aux_ch, index);
	default:
		I915_DP_MISSING_CASE(aux_ch);
		return DP_AUX_CH_DATA(AUX_CH_A, index);
	}
}

/* Finds the eDP world of a panel run's panel, for the modeset hooks. */
static struct i915_dp_world *
i915_aux_emit_world(
	void *ctx)
{
	struct i915_lcd_kernel *kernel;
	struct i915_dp_world *world;

	/* The hook context is the panel run; its panel is the display's eDP device. */
	kernel = ctx;
	world = drv_i915_edp_world_of(kernel->d->edp);

	/* Succeeded: reports the world. */
	return world;
}

/*
 * Reports a failed AUX transfer with its values: the channel, what went
 * wrong, the last status, the control register and what it reads now, and
 * the AUX power wells' BIOS and driver requests.  The failure is counted as
 * a DP error; only the first ones, then one of every
 * I915_DP_AUX_REPORTS_EVERY, are logged.
 */
static void
i915_dp_aux_report(
	struct intel_dp *intel_dp,
	struct drm_i915_private *i915,
	i915_reg_t ch_ctl,
	const char *what,
	u32 status)
{
	u32 control;
	u32 wells_bios;
	u32 wells_driver;
	u32 main_wells;
	u32 dc_state;
	int shown;

	/* Counts the error; a level that is not shown ends here. */
	shown = drv_i915_dp_log_enabled(I915_VBT_LOG_ERR);
	if (!shown)
		return;

	/* Only the first failures, then one of every so many, are logged. */
	i915_dp_aux_reports++;
	if (i915_dp_aux_reports > I915_DP_AUX_REPORTS_FIRST && (i915_dp_aux_reports % I915_DP_AUX_REPORTS_EVERY) != 0u)
		return;

	/* The control register now, and the AUX wells' requests and states. */
	control = i915_dp_intel_de_read(i915, ch_ctl);
	wells_bios = i915_dp_intel_de_read(i915, _MMIO(I915_DP_AUX_WELLS_BIOS));
	wells_driver = i915_dp_intel_de_read(i915, _MMIO(I915_DP_AUX_WELLS_DRIVER));

	/* The main wells and the DC state the channel's registers live under. */
	main_wells = i915_dp_intel_de_read(i915, _MMIO(I915_DP_MAIN_WELLS_DRIVER));
	dc_state = i915_dp_intel_de_read(i915, _MMIO(I915_DP_DC_STATE_EN));

	/* One line with every value. */
	kern_logf("i915: aux %s: %s (status 0x%08x; control 0x%05x reads 0x%08x; AUX wells bios 0x%08x driver 0x%08x; main wells driver 0x%08x DC_STATE_EN 0x%08x; report %u)\n",
		  intel_dp->aux.name,
		  what,
		  status,
		  ch_ctl.reg,
		  control,
		  wells_bios,
		  wells_driver,
		  main_wells,
		  dc_state,
		  i915_dp_aux_reports);
}
