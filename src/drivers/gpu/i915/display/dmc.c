/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Derived from the Linux kernel v6.8.12 (drivers/gpu/drm/i915/display/intel_dmc.c),
 * which carries the following notice.
 *
 * Copyright © 2014 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/*
 * The DMC firmware: parse, load and the asynchronous loader (see dmc.h).
 *
 * The parse is intel_dmc.c's parse_dmc_fw() with its CSS, package and
 * per-DMC header checks and dmc_set_fw_offset(): the entry matching the
 * display stepping is chosen for each DMC id, and its payload is copied
 * into storage the display owns, so it outlives the released firmware.
 * Header lengths are in dwords except the v1 DMC header's, which is in
 * bytes, and an entry's offset counts from the end of the CSS and package
 * headers, as in the reference.
 *
 * The load is intel_dmc_load_program() with the Alder Lake-P workarounds
 * around it; the loader (intel_dmc_init() and its work) takes the DMC's own
 * POWER_DOMAIN_INIT reference, loads on the DMC workqueue and gives the
 * reference back only after a complete load.
 *
 * The pipe DMC enable and disable of the modeset are the Linux text of
 * intel_dmc.c.
 */

#include "modeset-internal.h"
#include "dmc.h"
#include "power.h"
#include <kern/kcrt.h>

#include "../intel/mreg.h"

#include "../firmware.h"
#include "../mmio.h"
#include "../workqueue.h"

#include <kern/klog.h>
#include <kern/sched.h>

#include <uapi/errno.h>

/* The reference's firmware layout limits (intel_dmc.c). */
#define PACKAGE_MAX_FW_INFO_ENTRIES	20
#define PACKAGE_V2_MAX_FW_INFO_ENTRIES	32
#define DMC_V1_MAX_MMIO_COUNT		8
#define DMC_V3_MAX_MMIO_COUNT		20

/* The MMIO ranges a DMC header's registers must lie in (intel_dmc_regs.h). */
#define DMC_V1_MMIO_START_RANGE		0x80000u
#define DMC_MMIO_START_RANGE		0x80000u
#define DMC_MMIO_END_RANGE		0x8FFFFu
#define TGL_MAIN_MMIO_START		0x8F000u
#define TGL_MAIN_MMIO_END		0x8FFFFu
#define ADLP_PIPE_MMIO_START		0x5F000u
#define ADLP_PIPE_MMIO_END		0x5FFFFu

/* Display version 12 keeps one window per pipe payload (_PICK_EVEN(dmc_id - 1)). */
#define _TGL_PIPEA_MMIO_START		0x92000u
#define _TGL_PIPEA_MMIO_END		0x93FFFu
#define _TGL_PIPEB_MMIO_START		0x96000u
#define _TGL_PIPEB_MMIO_END		0x97FFFu

/* The per-platform payload ceilings (intel_dmc.c). */
#define DISPLAY_VER13_DMC_MAX_FW_SIZE	0x20000u
#define ICL_DMC_MAX_FW_SIZE		0x6000u
#define DISPLAY_VER12_DMC_MAX_FW_SIZE	ICL_DMC_MAX_FW_SIZE

/* The event handler registers: MAIN at 0x8f000, the pipe DMCs at 0x5f000 + 0x400 * (id - 1). */
#define DMC_MAIN_REG_BASE		0x8f000u
#define ADLP_PIPEDMC_BASE_A		0x5f000u
#define ADLP_PIPEDMC_STRIDE		0x400u
#define DMC_EVT_HTP_0			0x8f004u
#define DMC_EVT_CTL_0			0x8f034u
#define DMC_EVENT_HANDLER_COUNT		8u

/* The disabled event: TYPE_EDGE_0_1 (3 << 16) | EVENT_ID_FALSE (0x01 << 8). */
#define DMC_EVT_DISABLE_CTL		((3u << 16) | (0x01u << 8))

/* Wa_16015201720's clock gating and the DC state debug mask (i915_reg.h). */
#define CLKGATE_DIS_PSL_EXT_A		0x4654Cu
#define PIPEDMC_GATING_DIS		(1u << 12)
#define DC_STATE_DEBUG_REG		0x45520u
#define DC_STATE_DEBUG_MASK		0x3u	/* MASK_CORES | MASK_MEMORY_UP */

/* The firmware of Alder Lake-P when the configured one is missing (dmc_fallback_path()). */
#define ADLP_DMC_FALLBACK_PATH		"i915/adlp_dmc_ver2_16.bin"

/* The firmware of the platform when the caller names none. */
#define ADLP_DMC_DEFAULT_PATH		"i915/adlp_dmc.bin"

/*
 * The DMC firmware ids of the modeset text (intel_dmc.c's enum
 * intel_dmc_id), which PIPE_TO_DMC_ID() counts in.
 */
enum intel_dmc_id {
	DMC_FW_MAIN = 0,
	DMC_FW_PIPEA,
	DMC_FW_PIPEB,
	DMC_FW_PIPEC,
	DMC_FW_PIPED,
	DMC_FW_MAX
};

/*
 * The CSS header at the start of a DMC firmware image (intel_dmc.c).
 *
 * It is read in place from the firmware bytes and never stored.
 */
struct css_header {
	uint32_t module_type;
	uint32_t header_len;
	uint32_t header_ver;
	uint32_t module_id;
	uint32_t module_vendor;
	uint32_t date;
	uint32_t size;
	uint32_t key_size;
	uint32_t modulus_size;
	uint32_t exponent_size;
	uint32_t reserved1[12];
	uint32_t version;
	uint32_t reserved2[8];
	uint32_t kernel_header_info;
} __attribute__((packed));

/*
 * One firmware entry of the package header: which DMC and stepping a
 * payload is for, and where it starts (intel_dmc.c).  Read in place.
 */
struct fw_info {
	uint8_t reserved1;
	uint8_t dmc_id;
	char stepping;
	char substepping;
	uint32_t offset;
	uint32_t reserved2;
} __attribute__((packed));

/*
 * The package header that lists the firmware entries (intel_dmc.c).  Read
 * in place.
 */
struct package_header {
	uint8_t header_len;
	uint8_t header_ver;
	uint8_t reserved[10];
	uint32_t num_entries;
} __attribute__((packed));

/*
 * The part every DMC header version shares (intel_dmc.c).  Read in place.
 */
struct dmc_header_base {
	uint32_t signature;
	uint8_t header_len;
	uint8_t header_ver;
	uint16_t dmcc_ver;
	uint32_t project;
	uint32_t fw_size;
	uint32_t fw_version;
} __attribute__((packed));

/*
 * A version 1 DMC header: its register writes and the payload that
 * follows (intel_dmc.c).  Read in place.
 */
struct dmc_header_v1 {
	struct dmc_header_base base;
	uint32_t mmio_count;
	uint32_t mmioaddr[DMC_V1_MAX_MMIO_COUNT];
	uint32_t mmiodata[DMC_V1_MAX_MMIO_COUNT];
	char dfile[32];
	uint32_t reserved1[2];
} __attribute__((packed));

/*
 * A version 3 DMC header: its program start, its register writes and the
 * payload that follows (intel_dmc.c).  Read in place.
 */
struct dmc_header_v3 {
	struct dmc_header_base base;
	uint32_t start_mmioaddr;
	uint32_t reserved[9];
	char dfile[32];
	uint32_t mmio_count;
	uint32_t mmioaddr[DMC_V3_MAX_MMIO_COUNT];
	uint32_t mmiodata[DMC_V3_MAX_MMIO_COUNT];
} __attribute__((packed));

static struct i915_display *i915_dmc_display(struct i915_dmc *dmc);
static const uint8_t *i915_arena_copy(struct i915_dmc *dmc, const uint8_t *src, unsigned n);
static int i915_dmc_id_valid(int id);
static int i915_fw_info_matches_stepping(const struct fw_info *fi, char step, char sub);
static void i915_dmc_set_fw_offset(struct i915_dmc *dmc, const struct fw_info *fw_info, unsigned num_entries, char step, char sub, uint8_t package_ver);
static int i915_mmio_addr_ok(struct i915_dmc *dmc, const uint32_t *addr, uint32_t count, int header_ver, int id);
static uint32_t i915_parse_css(struct i915_dmc *dmc, const uint8_t *data, unsigned rem);
static uint32_t i915_parse_package(struct i915_dmc *dmc, const uint8_t *data, unsigned rem);
static void i915_dmc_truncated(struct i915_dmc *dmc);
static uint32_t i915_parse_header(struct i915_dmc *dmc, const uint8_t *data, unsigned rem, int id);
static uint32_t i915_dmc_reg_base(int id);
static uint32_t i915_dmc_reg(int id, uint32_t reg);
static uint32_t i915_dmc_evt_ctl(int id, unsigned handler);
static uint32_t i915_dmc_evt_htp(int id, unsigned handler);
static int i915_is_evt_ctl(int id, uint32_t addr);
static uint32_t i915_dmc_mmiodata(struct i915_dmc *dmc, int id, unsigned index);
static uint32_t i915_payload_dword(const uint8_t *payload, uint32_t index);
static void i915_dmc_rmw32(struct i915_mmio *mmio, uint32_t reg, uint32_t clear, uint32_t set);
static void i915_dmc_get_ref(struct i915_dmc_dev *dev);
static void i915_dmc_put_ref(struct i915_dmc_dev *dev);
static void i915_dmc_load_work_fn(void *ctx);
static bool i915_is_valid_dmc_id(enum intel_dmc_id dmc_id);

/*
 * Prepares a DMC parse for a display version and stepping, emptying the
 * display's payload storage.
 */
void
drv_i915_dmc_prepare(
	struct i915_dmc *dmc,
	int display_ver,
	char stepping,
	char substepping)
{
	struct i915_display *display;

	/* Starts from an empty parse. */
	kern_memset(dmc, 0, sizeof(*dmc));
	dmc->display_ver = display_ver;
	dmc->stepping = stepping;
	dmc->substepping = substepping;

	/* The payload ceiling is per display version too (intel_dmc_init()). */
	dmc->max_fw_size = DISPLAY_VER12_DMC_MAX_FW_SIZE;
	if (display_ver >= 13)
		dmc->max_fw_size = DISPLAY_VER13_DMC_MAX_FW_SIZE;

	/* The display's payload storage starts empty. */
	display = i915_dmc_display(dmc);
	display->g_dmc_arena_used = 0u;
}

/*
 * Parses a DMC firmware image (parse_dmc_fw()) and saves each chosen
 * payload.
 *
 * Returns 0 once the MAIN payload is saved, EINVAL for a missing image or
 * a bad CSS or package header, and ENOENT when no MAIN payload came out.
 */
int
drv_i915_parse_dmc_fw(
	struct i915_dmc *dmc,
	const uint8_t *data,
	unsigned size)
{
	uint32_t readcount;
	uint32_t parsed;
	uint32_t offset;
	int id;
	int has_payload;

	/* Refuses a missing image. */
	if (data == NULL || size == 0u)
		return EINVAL;

	/* Parses the CSS header. */
	readcount = 0;
	parsed = i915_parse_css(dmc, data, size);
	if (parsed == 0u)
		return EINVAL;
	readcount += parsed;

	/* Parses the package header and chooses the entries. */
	parsed = i915_parse_package(dmc, data + readcount, size - readcount);
	if (parsed == 0u)
		return EINVAL;
	readcount += parsed;

	/* Parses and saves the payload of each chosen entry. */
	for (id = I915_DMC_FW_MAIN; id < I915_DMC_FW_MAX; id++) {
		if (!dmc->dmc_info[id].present)
			continue;

		/* The entry's offset is in dwords after the CSS and package headers. */
		offset = readcount + dmc->dmc_info[id].dmc_offset * 4u;
		if (offset > size) {
			kern_logf("i915: dmc: reading beyond fw_size (id %d)\n", id);
			continue;
		}

		/* A header that fails is reported inside and leaves no payload. */
		(void)i915_parse_header(dmc, data + offset, size - offset, id);
	}

	/* The MAIN payload is what the load needs. */
	has_payload = drv_i915_dmc_has_payload(dmc);
	if (!has_payload)
		return ENOENT;

	/* Succeeded: the MAIN payload is saved. */
	return 0;
}

/*
 * Forgets every saved payload and empties the display's payload storage.
 */
void
drv_i915_dmc_parse_reset(
	struct i915_dmc *dmc)
{
	struct i915_display *display;
	unsigned id;

	/* Forgets each DMC id's payload. */
	for (id = 0u; id < I915_DMC_FW_MAX; id++) {
		dmc->dmc_info[id].payload = NULL;
		dmc->dmc_info[id].payload_size = 0u;
		dmc->dmc_info[id].present = 0;
	}

	/* Releases the display's payload storage. */
	display = i915_dmc_display(dmc);
	display->g_dmc_arena_used = 0u;
}

/*
 * Tells whether the MAIN payload is saved (intel_dmc_has_payload()).
 */
int
drv_i915_dmc_has_payload(
	const struct i915_dmc *dmc)
{
	/* Only a saved MAIN payload counts. */
	if (dmc->dmc_info[I915_DMC_FW_MAIN].payload == NULL)
		return 0;

	/* Succeeded: the MAIN payload is there. */
	return 1;
}

/*
 * Loads the saved payloads into the DMCs (intel_dmc_load_program()).
 *
 * The event handlers are disabled, the payload dwords written with
 * preemption off, each DMC's trailing registers written, the DC state debug
 * mask set, and Wa_16015201720 applied around it.  dc_state_out (optional)
 * receives the DC state the load leaves (0).
 */
void
drv_i915_dmc_load_program(
	struct i915_dmc *dmc,
	struct i915_mmio *mmio,
	uint32_t *dc_state_out)
{
	struct i915_dmc_info *info;
	int id;
	int has_payload;
	unsigned handler;
	unsigned index;
	uint32_t pipe;
	uint32_t addr;
	uint32_t value;

	/* Starts the load's diagnostics from zero. */
	dmc->payload_writes = 0u;
	dmc->aux_writes = 0u;
	dmc->evt_disable_writes = 0u;
	dmc->load_seq_completed = 0;
	dmc->psum = 0u;
	dmc->asum = 0u;

	/* Nothing to load without the MAIN payload. */
	has_payload = drv_i915_dmc_has_payload(dmc);
	if (!has_payload)
		return;

	/* Wa_16015201720:adl-p, before the load: pipes A..D. */
	for (pipe = 0u; pipe <= 3u; pipe++)
		i915_dmc_rmw32(mmio, CLKGATE_DIS_PSL_EXT_A + pipe * 4u, 0u, PIPEDMC_GATING_DIS);

	/* disable_all_event_handlers() (display version 12+): each CTL to FALSE, each HTP to 0. */
	for (id = I915_DMC_FW_MAIN; id < I915_DMC_FW_MAX; id++) {
		if (dmc->dmc_info[id].payload == NULL)
			continue;
		for (handler = 0u; handler < DMC_EVENT_HANDLER_COUNT; handler++) {
			drv_i915_raw_write32(mmio, i915_dmc_evt_ctl(id, handler), DMC_EVT_DISABLE_CTL);
			drv_i915_raw_write32(mmio, i915_dmc_evt_htp(id, handler), 0u);
			dmc->evt_disable_writes += 2u;
		}
	}

	/*
	 * Writes the payloads with preemption off (the reference's write_fw
	 * loop), each dword to DMC_PROGRAM(start, i), folding every write into
	 * an order-sensitive checksum.
	 */
	kern_preempt_disable();

	for (id = I915_DMC_FW_MAIN; id < I915_DMC_FW_MAX; id++) {
		info = &dmc->dmc_info[id];
		if (info->payload == NULL)
			continue;
		for (index = 0u; index < info->dmc_fw_size; index++) {
			addr = info->start_mmioaddr + index * 4u;
			value = i915_payload_dword(info->payload, index);
			drv_i915_raw_write32(mmio, addr, value);
			dmc->psum = dmc->psum * 1000003u + addr + (uint64_t)value * 7u;
			dmc->payload_writes++;
		}
	}

	kern_preempt_enable();

	/* Writes each DMC's trailing registers, as dmc_mmiodata() transforms them. */
	for (id = I915_DMC_FW_MAIN; id < I915_DMC_FW_MAX; id++) {
		info = &dmc->dmc_info[id];
		if (info->payload == NULL)
			continue;
		for (index = 0u; index < info->mmio_count; index++) {
			addr = info->mmioaddr[index];
			value = i915_dmc_mmiodata(dmc, id, index);
			drv_i915_raw_write32(mmio, addr, value);
			dmc->asum = dmc->asum * 1000003u + addr + (uint64_t)value * 7u;
			dmc->aux_writes++;
		}
	}

	/* power_domains->dc_state = 0. */
	if (dc_state_out != NULL)
		*dc_state_out = 0u;

	/* gen9_set_dc_state_debugmask(): the read-modify-write and its posting read. */
	i915_dmc_rmw32(mmio, DC_STATE_DEBUG_REG, 0u, DC_STATE_DEBUG_MASK);
	(void)drv_i915_raw_read32(mmio, DC_STATE_DEBUG_REG);

	/* Wa_16015201720:adl-p, after the load: pipes C..D only (not a symmetric undo). */
	for (pipe = 2u; pipe <= 3u; pipe++)
		i915_dmc_rmw32(mmio, CLKGATE_DIS_PSL_EXT_A + pipe * 4u, PIPEDMC_GATING_DIS, 0u);

	/* The whole sequence ran, the post workaround included. */
	dmc->load_seq_completed = 1;
}

/*
 * Starts the DMC loader (intel_dmc_init()).
 *
 * Takes the DMC's own POWER_DOMAIN_INIT reference (held on failure),
 * prepares the parse and queues the load work.
 */
void
drv_i915_dmc_init(
	struct i915_dmc_dev *dev,
	struct i915_workqueue *wq,
	struct i915_mmio *mmio,
	struct i915_power_domains *pd,
	struct i915_pw_ctx *pwc,
	int display_ver,
	int is_alderlake_p,
	char stepping,
	char substepping,
	const char *fw_path)
{
	/* Binds the loader and takes the DMC's own reference first. */
	dev->pd = pd;
	dev->pwc = pwc;
	dev->m = mmio;
	dev->wq = wq;
	dev->dmc_wakeref_held = 0;
	i915_dmc_get_ref(dev);

	/* Names the firmware and starts the diagnostics from zero. */
	dev->is_alderlake_p = is_alderlake_p;
	dev->fw_path = ADLP_DMC_DEFAULT_PATH;
	if (fw_path != NULL)
		dev->fw_path = fw_path;
	dev->dc_state = 0xffffffffu;
	dev->work_submitted = 0;
	dev->worker_started = 0;
	dev->firmware_acquired = 0;
	dev->fallback_requested = 0;
	dev->main_payload_present = 0;
	dev->load_seq_completed_flag = 0;
	dev->first_fault = 0;
	drv_i915_dmc_prepare(&dev->dmc, display_ver, stepping, substepping);

	/*
	 * Queues the work only after everything it reads is published: it may
	 * start on another CPU before this returns, and queueing orders the
	 * writes before it.
	 */
	drv_i915_work_init(&dev->work, i915_dmc_load_work_fn, dev);
	dev->work_submitted = 1;
	(void)drv_i915_queue_work(wq, &dev->work);
}

/*
 * Stops the DMC loader: waits for the work (a flush, not a cancel), gives
 * back a reference the work kept, and releases the payload storage.
 */
void
drv_i915_dmc_fini(
	struct i915_dmc_dev *dev,
	uint64_t deadline)
{
	/* Waits for the work to complete. */
	(void)drv_i915_flush_work(dev->wq, &dev->work, deadline);

	/* Gives back a reference the work left held (failure or fault). */
	i915_dmc_put_ref(dev);

	/* Releases the payload storage and the parse state. */
	drv_i915_dmc_parse_reset(&dev->dmc);
}

/*
 * Enables a pipe's DMC when its payload was loaded (intel_dmc_enable_pipe()).
 */
void
drv_i915_dmc_enable_pipe(
	struct drm_i915_private *i915,
	enum pipe pipe)
{
	enum intel_dmc_id dmc_id;
	bool valid;

	/* A pipe without a loaded DMC payload has nothing to enable. */
	dmc_id = PIPE_TO_DMC_ID(pipe);
	valid = i915_is_valid_dmc_id(dmc_id);
	if (!valid)
		return;
	if (!has_dmc_id_fw(i915, dmc_id))
		return;

	/* Display version 14+ has one control for every pipe. */
	if (I915_LCD_DISPLAY_VER(i915) >= 14)
		(void)i915_lcd_intel_de_rmw(i915, MTL_PIPEDMC_CONTROL, 0, PIPEDMC_ENABLE_MTL(pipe));
	else
		(void)i915_lcd_intel_de_rmw(i915, PIPEDMC_CONTROL(pipe), 0, PIPEDMC_ENABLE);
}

/*
 * Disables a pipe's DMC when its payload was loaded (intel_dmc_disable_pipe()).
 */
void
drv_i915_dmc_disable_pipe(
	struct drm_i915_private *i915,
	enum pipe pipe)
{
	enum intel_dmc_id dmc_id;
	bool valid;

	/* A pipe without a loaded DMC payload has nothing to disable. */
	dmc_id = PIPE_TO_DMC_ID(pipe);
	valid = i915_is_valid_dmc_id(dmc_id);
	if (!valid)
		return;
	if (!has_dmc_id_fw(i915, dmc_id))
		return;

	/* Display version 14+ has one control for every pipe. */
	if (I915_LCD_DISPLAY_VER(i915) >= 14)
		(void)i915_lcd_intel_de_rmw(i915, MTL_PIPEDMC_CONTROL, PIPEDMC_ENABLE_MTL(pipe), 0);
	else
		(void)i915_lcd_intel_de_rmw(i915, PIPEDMC_CONTROL(pipe), PIPEDMC_ENABLE, 0);
}

/*
 * The display a DMC parse belongs to: the parse is the dmc of the
 * display's DMC loader, and the payload storage is the display's.
 */
static struct i915_display *
i915_dmc_display(
	struct i915_dmc *dmc)
{
	struct i915_display *display;

	/* The parse is the dmc_dev.dmc member of its display. */
	display = container_of(dmc, struct i915_display, dmc_dev.dmc);

	/* Succeeded: reports the display. */
	return display;
}

/*
 * Copies a payload into the display's payload storage; NULL when it does
 * not fit (treated like an allocation failure).
 */
static const uint8_t *
i915_arena_copy(
	struct i915_dmc *dmc,
	const uint8_t *src,
	unsigned n)
{
	struct i915_display *display;
	uint8_t *dst;

	/* Refuses a payload the storage cannot hold. */
	display = i915_dmc_display(dmc);
	if (n > sizeof(display->g_dmc_arena) - display->g_dmc_arena_used)
		return NULL;

	/* Copies it after what is already stored. */
	dst = &display->g_dmc_arena[display->g_dmc_arena_used];
	kern_memcpy(dst, src, n);
	display->g_dmc_arena_used += n;

	/* Succeeded: the copy is the display's. */
	return dst;
}

/* Tells whether a DMC id is one of MAIN and the four pipes (1 or 0). */
static int
i915_dmc_id_valid(
	int id)
{
	/* The ids run from MAIN up to, not including, MAX. */
	if (id < I915_DMC_FW_MAIN)
		return 0;
	if (id >= I915_DMC_FW_MAX)
		return 0;

	/* Succeeded: the id names a DMC. */
	return 1;
}

/* Tells whether a firmware entry is for the display's stepping (fw_info_matches_stepping()). */
static int
i915_fw_info_matches_stepping(
	const struct fw_info *fi,
	char step,
	char sub)
{
	/* Any substepping of the stepping. */
	if (fi->substepping == '*' && step == fi->stepping)
		return 1;

	/* Exactly the stepping and substepping. */
	if (step == fi->stepping && sub == fi->substepping)
		return 1;

	/* Any stepping with the substepping. */
	if (step == '*' && sub == fi->substepping)
		return 1;

	/* Any stepping at all. */
	if (fi->stepping == '*' && fi->substepping == '*')
		return 1;

	/* Succeeded: the entry is for another stepping. */
	return 0;
}

/*
 * Chooses each DMC id's entry (dmc_set_fw_offset()): the first matching
 * entry wins, the more specific ones being listed first.
 */
static void
i915_dmc_set_fw_offset(
	struct i915_dmc *dmc,
	const struct fw_info *fw_info,
	unsigned num_entries,
	char step,
	char sub,
	uint8_t package_ver)
{
	unsigned index;
	int id;
	int valid;
	int matches;

	/* Walks the entries in order. */
	for (index = 0u; index < num_entries; index++) {
		/* A version 1 package has only the MAIN DMC. */
		id = I915_DMC_FW_MAIN;
		if (package_ver > 1)
			id = (int)fw_info[index].dmc_id;

		/* Skips an unknown id, and an id that already has a (more specific) entry. */
		valid = i915_dmc_id_valid(id);
		if (!valid)
			continue;
		if (dmc->dmc_info[id].present)
			continue;

		/* Takes the entry when it is for the stepping. */
		matches = i915_fw_info_matches_stepping(&fw_info[index], step, sub);
		if (matches) {
			dmc->dmc_info[id].present = 1;
			dmc->dmc_info[id].dmc_offset = fw_info[index].offset;
		}
	}
}

/*
 * Checks a DMC header's registers against the ranges its DMC may write
 * (dmc_mmio_addr_sanity_check()): 1 when every one is in range.
 */
static int
i915_mmio_addr_ok(
	struct i915_dmc *dmc,
	const uint32_t *addr,
	uint32_t count,
	int header_ver,
	int id)
{
	uint32_t lo;
	uint32_t hi;
	uint32_t index;

	/* Picks the range of the header version, the DMC and the platform. */
	if (header_ver == 1) {
		lo = DMC_MMIO_START_RANGE;
		hi = DMC_MMIO_END_RANGE;
	} else if (id == I915_DMC_FW_MAIN) {
		lo = TGL_MAIN_MMIO_START;
		hi = TGL_MAIN_MMIO_END;
	} else if (dmc->display_ver >= 13) {
		lo = ADLP_PIPE_MMIO_START;
		hi = ADLP_PIPE_MMIO_END;
	} else if (dmc->display_ver >= 12) {
		/* Tiger Lake: one window per pipe payload, not the single ADL-P one. */
		lo = _TGL_PIPEA_MMIO_START + (uint32_t)(id - 1) * (_TGL_PIPEB_MMIO_START - _TGL_PIPEA_MMIO_START);
		hi = _TGL_PIPEA_MMIO_END + (uint32_t)(id - 1) * (_TGL_PIPEB_MMIO_END - _TGL_PIPEA_MMIO_END);
	} else {
		/* XXX: unimplemented path -- displays before version 12 are not ported. */
		kern_logf("i915: dmc: XXX unknown mmio range for sanity check (display_ver=%d)\n",
			dmc->display_ver);
		return 0;
	}

	/* Every register must be in the range. */
	for (index = 0u; index < count; index++) {
		if (addr[index] < lo || addr[index] > hi)
			return 0;
	}

	/* Succeeded: every register is in range. */
	return 1;
}

/* Parses the CSS header (parse_dmc_fw_css()): its length, or 0 on a bad header. */
static uint32_t
i915_parse_css(
	struct i915_dmc *dmc,
	const uint8_t *data,
	unsigned rem)
{
	const struct css_header *css;

	/* A header that does not fit is refused. */
	css = (const struct css_header *)data;
	if (rem < sizeof(struct css_header))
		return 0;

	/* The header length is in dwords and must be the structure's. */
	if (sizeof(struct css_header) != (uint32_t)css->header_len * 4u) {
		kern_logf("i915: dmc: wrong CSS header length (%u bytes)\n",
			(unsigned)css->header_len * 4u);
		return 0;
	}

	/* Records the firmware version and the header length. */
	dmc->version = css->version;
	dmc->css_header_len_bytes = (uint32_t)css->header_len * 4u;

	/* Succeeded: reports the header's length. */
	return sizeof(struct css_header);
}

/*
 * Parses the package header and chooses the entries
 * (parse_dmc_fw_package()): its length, or 0 on a bad header.
 */
static uint32_t
i915_parse_package(
	struct i915_dmc *dmc,
	const uint8_t *data,
	unsigned rem)
{
	const struct package_header *ph;
	const struct fw_info *fi;
	uint32_t package_size;
	uint32_t max_entries;
	uint32_t num_entries;

	/* A header that does not fit is refused. */
	ph = (const struct package_header *)data;
	package_size = sizeof(struct package_header);
	if (rem < package_size)
		return 0;

	/* The entry count of the header version. */
	if (ph->header_ver == 1) {
		max_entries = PACKAGE_MAX_FW_INFO_ENTRIES;
	} else if (ph->header_ver == 2) {
		max_entries = PACKAGE_V2_MAX_FW_INFO_ENTRIES;
	} else {
		kern_logf("i915: dmc: unknown package header version %u\n", ph->header_ver);
		return 0;
	}

	/* The entries must fit, and the header length (dwords) must cover them. */
	package_size += max_entries * sizeof(struct fw_info);
	if (rem < package_size)
		return 0;
	if ((uint32_t)ph->header_len * 4u != package_size) {
		kern_logf("i915: dmc: wrong package header length (%u bytes)\n", package_size);
		return 0;
	}

	/* Takes at most the version's number of entries. */
	num_entries = ph->num_entries;
	if (num_entries > max_entries)
		num_entries = max_entries;

	/* Records the package. */
	dmc->package_header_ver = ph->header_ver;
	dmc->num_entries = num_entries;

	/* Chooses the entries for the display's stepping. */
	fi = (const struct fw_info *)(data + sizeof(struct package_header));
	i915_dmc_set_fw_offset(dmc, fi, num_entries, dmc->stepping, dmc->substepping, ph->header_ver);

	/* Succeeded: reports the header's length. */
	return package_size;
}

/* Records and logs a truncated firmware, which the parse refuses. */
static void
i915_dmc_truncated(
	struct i915_dmc *dmc)
{
	/* The truncation is recorded for the diagnostics and logged. */
	dmc->truncated = 1;
	kern_logf("i915: dmc: truncated DMC firmware, refusing\n");
}

/*
 * Parses one DMC id's header and saves its payload (parse_dmc_fw_header()):
 * the length of header and payload, or 0 when it was refused.
 */
static uint32_t
i915_parse_header(
	struct i915_dmc *dmc,
	const uint8_t *data,
	unsigned rem,
	int id)
{
	const struct dmc_header_base *base;
	const struct dmc_header_v3 *v3;
	const struct dmc_header_v1 *v1;
	struct i915_dmc_info *info;
	unsigned header_len_bytes;
	unsigned dmc_header_size;
	unsigned payload_size;
	unsigned index;
	const uint32_t *mmioaddr;
	const uint32_t *mmiodata;
	uint32_t mmio_count;
	uint32_t mmio_count_max;
	uint32_t start_mmioaddr;
	int addr_ok;

	/* The shared part must fit. */
	base = (const struct dmc_header_base *)data;
	info = &dmc->dmc_info[id];
	if (rem < sizeof(struct dmc_header_base)) {
		i915_dmc_truncated(dmc);
		return 0;
	}

	/* Reads the version's layout: v3 lengths in dwords, v1 in bytes. */
	if (base->header_ver == 3) {
		v3 = (const struct dmc_header_v3 *)data;
		if (rem < sizeof(struct dmc_header_v3)) {
			i915_dmc_truncated(dmc);
			return 0;
		}

		/* Takes the v3 layout (lengths in dwords). */
		mmioaddr = v3->mmioaddr;
		mmiodata = v3->mmiodata;
		mmio_count = v3->mmio_count;
		mmio_count_max = DMC_V3_MAX_MMIO_COUNT;
		header_len_bytes = (unsigned)base->header_len * 4u;
		start_mmioaddr = v3->start_mmioaddr;
		dmc_header_size = sizeof(struct dmc_header_v3);
	} else if (base->header_ver == 1) {
		v1 = (const struct dmc_header_v1 *)data;
		if (rem < sizeof(struct dmc_header_v1)) {
			i915_dmc_truncated(dmc);
			return 0;
		}

		/* Takes the v1 layout (header length in bytes). */
		mmioaddr = v1->mmioaddr;
		mmiodata = v1->mmiodata;
		mmio_count = v1->mmio_count;
		mmio_count_max = DMC_V1_MAX_MMIO_COUNT;
		header_len_bytes = base->header_len;
		start_mmioaddr = DMC_V1_MMIO_START_RANGE;
		dmc_header_size = sizeof(struct dmc_header_v1);
	} else {
		kern_logf("i915: dmc: unknown DMC fw header version %u\n", base->header_ver);
		return 0;
	}

	/* The header length must be the layout's, and the register count within it. */
	if (header_len_bytes != dmc_header_size) {
		kern_logf("i915: dmc: wrong dmc header length (%u bytes)\n", header_len_bytes);
		return 0;
	}

	/* The register count must be within the layout. */
	if (mmio_count > mmio_count_max) {
		kern_logf("i915: dmc: wrong mmio count %u\n", mmio_count);
		return 0;
	}

	/* Every register must be in the DMC's range; say which payload and which address. */
	addr_ok = i915_mmio_addr_ok(dmc, mmioaddr, mmio_count, base->header_ver, id);
	if (!addr_ok) {
		kern_logf("i915: dmc: wrong MMIO addresses (display_ver=%d id=%d header_ver=%d "
			"count=%u first=0x%05x last=0x%05x)\n", dmc->display_ver, id, base->header_ver,
			mmio_count, mmio_count != 0u ? mmioaddr[0] : 0u,
			mmio_count != 0u ? mmioaddr[mmio_count - 1u] : 0u);
		return 0;
	}

	/* Saves the registers and where the program starts. */
	for (index = 0u; index < mmio_count; index++) {
		info->mmioaddr[index] = mmioaddr[index];
		info->mmiodata[index] = mmiodata[index];
	}

	/* Records the count, the program start and the header version. */
	info->mmio_count = mmio_count;
	info->start_mmioaddr = start_mmioaddr;
	info->header_ver = base->header_ver;

	/* The payload follows the header; its size is in dwords and must fit and not exceed the ceiling. */
	rem -= header_len_bytes;
	payload_size = base->fw_size * 4u;
	if (rem < payload_size) {
		i915_dmc_truncated(dmc);
		return 0;
	}

	/* The payload must not exceed the platform ceiling. */
	if (payload_size > dmc->max_fw_size) {
		kern_logf("i915: dmc: FW too big (%u bytes)\n", payload_size);
		return 0;
	}

	/* Records the payload size in dwords. */
	info->dmc_fw_size = base->fw_size;

	/* Copies the payload into the display's storage (a full storage is an allocation failure). */
	info->payload = i915_arena_copy(dmc, data + header_len_bytes, payload_size);
	if (info->payload == NULL)
		return 0;
	info->payload_size = payload_size;

	/* Succeeded: reports the length of header and payload. */
	return header_len_bytes + payload_size;
}

/* The event register base of a DMC: MAIN, or the pipe DMC's block. */
static uint32_t
i915_dmc_reg_base(
	int id)
{
	/* MAIN has its own block. */
	if (id == I915_DMC_FW_MAIN)
		return DMC_MAIN_REG_BASE;

	/* Succeeded: the pipe DMC's block. */
	return ADLP_PIPEDMC_BASE_A + ADLP_PIPEDMC_STRIDE * (uint32_t)(id - 1);
}

/* A MAIN-relative register rebased to a DMC (_DMC_REG(id, reg)). */
static uint32_t
i915_dmc_reg(
	int id,
	uint32_t reg)
{
	/* The offset within MAIN's block, in the DMC's block. */
	return reg - DMC_MAIN_REG_BASE + i915_dmc_reg_base(id);
}

/* A DMC's event handler control register. */
static uint32_t
i915_dmc_evt_ctl(
	int id,
	unsigned handler)
{
	/* DMC_EVT_CTL(id, handler). */
	return i915_dmc_reg(id, DMC_EVT_CTL_0) + 4u * handler;
}

/* A DMC's event handler address register. */
static uint32_t
i915_dmc_evt_htp(
	int id,
	unsigned handler)
{
	/* DMC_EVT_HTP(id, handler). */
	return i915_dmc_reg(id, DMC_EVT_HTP_0) + 4u * handler;
}

/* Tells whether a register is one of a DMC's event handler controls (1 or 0). */
static int
i915_is_evt_ctl(
	int id,
	uint32_t addr)
{
	uint32_t start;
	uint32_t end;

	/* The controls run from handler 0 up to the handler count. */
	start = i915_dmc_evt_ctl(id, 0u);
	end = i915_dmc_evt_ctl(id, DMC_EVENT_HANDLER_COUNT);
	if (addr < start || addr >= end)
		return 0;

	/* Succeeded: the register is an event control. */
	return 1;
}

/*
 * The value a DMC's trailing register is written with (dmc_mmiodata()):
 * every pipe DMC event control stays disabled; MAIN's entries are written
 * as they are.
 */
static uint32_t
i915_dmc_mmiodata(
	struct i915_dmc *dmc,
	int id,
	unsigned index)
{
	int evt_ctl;

	/* A pipe DMC's event control is written disabled. */
	if (id != I915_DMC_FW_MAIN) {
		evt_ctl = i915_is_evt_ctl(id, dmc->dmc_info[id].mmioaddr[index]);
		if (evt_ctl)
			return DMC_EVT_DISABLE_CTL;
	}

	/* Succeeded: the header's value. */
	return dmc->dmc_info[id].mmiodata[index];
}

/* A little-endian dword of a payload. */
static uint32_t
i915_payload_dword(
	const uint8_t *payload,
	uint32_t index)
{
	const uint8_t *bytes;

	/* Assembles the dword's four bytes, lowest first. */
	bytes = payload + 4u * index;
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/* Clears and sets bits of a register with raw accesses. */
static void
i915_dmc_rmw32(
	struct i915_mmio *mmio,
	uint32_t reg,
	uint32_t clear,
	uint32_t set)
{
	uint32_t value;

	/* Reads, changes and writes the register back. */
	value = drv_i915_raw_read32(mmio, reg);
	drv_i915_raw_write32(mmio, reg, (value & ~clear) | set);
}

/* Takes the DMC's own POWER_DOMAIN_INIT reference (distinct from the display core's). */
static void
i915_dmc_get_ref(
	struct i915_dmc_dev *dev)
{
	/* Takes the reference; the flag says the loader holds it. */
	(void)drv_i915_display_power_get(dev->pd, I915_PW_DOMAIN_INIT, dev->pwc);
	dev->dmc_wakeref_held = 1;
}

/* Gives the DMC's own reference back, when it is held. */
static void
i915_dmc_put_ref(
	struct i915_dmc_dev *dev)
{
	/* Only a held reference is given back. */
	if (!dev->dmc_wakeref_held)
		return;

	/* Puts the domain; the flag says the loader no longer holds it. */
	drv_i915_display_power_put(dev->pd, I915_PW_DOMAIN_INIT, dev->pwc);
	dev->dmc_wakeref_held = 0;
}

/*
 * The load work (dmc_load_work_fn()): acquires the firmware, parses it,
 * loads it, and gives the DMC's reference back only after a complete load.
 */
static void
i915_dmc_load_work_fn(
	void *ctx)
{
	struct i915_dmc_dev *dev;
	struct i915_firmware fw;
	int request_result;

	/* Resolves the loader the work belongs to. */
	dev = ctx;
	dev->worker_started = 1;

	/* Acquires the configured firmware. */
	request_result = drv_i915_firmware_request(&fw, dev->fw_path);

	/*
	 * dmc_fallback_path(): the fallback firmware exists for Alder Lake-P
	 * and nothing else, so another display has nothing to fall back to.
	 */
	if (request_result == ENOENT && dev->is_alderlake_p) {
		dev->fallback_requested = 1;
		request_result = drv_i915_firmware_request(&fw, ADLP_DMC_FALLBACK_PATH);
		if (request_result == 0)
			dev->fw_path = ADLP_DMC_FALLBACK_PATH;
	}

	/* Parses what was acquired. */
	if (request_result == 0) {
		dev->firmware_acquired = 1;
		(void)drv_i915_parse_dmc_fw(&dev->dmc, fw.data, fw.size);
	}

	/* Records whether the MAIN payload came out. */
	dev->main_payload_present = drv_i915_dmc_has_payload(&dev->dmc);
	if (dev->main_payload_present)
		kern_logf("i915: dmc worker: proceeding to load\n");

	/*
	 * Loads the payload.  A complete load gives the DMC's reference back; a
	 * fault mid-load keeps it.  Without a payload (absent or refused) the
	 * reference is kept too, which blocks runtime suspend.
	 */
	if (dev->main_payload_present) {
		drv_i915_dmc_load_program(&dev->dmc, dev->m, &dev->dc_state);
		dev->load_seq_completed_flag = dev->dmc.load_seq_completed;
		if (dev->dmc.load_seq_completed)
			i915_dmc_put_ref(dev);
		else
			dev->first_fault = 1;
	}

	/* Releases the firmware bytes; the payload is the display's copy. */
	if (request_result == 0)
		drv_i915_firmware_release(&fw);
}

/* Tells whether a Linux DMC id names a DMC (is_valid_dmc_id()). */
static bool
i915_is_valid_dmc_id(
	enum intel_dmc_id dmc_id)
{
	/* The ids run from MAIN up to, not including, MAX. */
	if (dmc_id < DMC_FW_MAIN)
		return false;
	if (dmc_id >= DMC_FW_MAX)
		return false;

	/* Succeeded: the id names a DMC. */
	return true;
}
