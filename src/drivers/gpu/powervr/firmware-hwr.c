/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the firmware knows of a context reset: its hardware
 * recovery records, the last words of its trace, and the reset context's
 * firmware record.  Built only into a kernel with CONFIG_POWERVR_HWR_DUMP
 * (the desktop test kernels); the reset path calls it through a weak
 * symbol.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_fwif.h and
 * pvr_rogue_fwif_check.h (struct rogue_hwrinfo, 136 bytes: hwr_data 0
 * (four 64-bit words: BIF, MMU, poll or ECC information), cr_timer 32,
 * os_timer 40, frame_num 48, pid 52, active_hwrt_data 56, hwr_number 60,
 * event_status 64, hwr_recovery_flags 68, hwr_type 72 (enum
 * rogue_hwrtype: 0 unknown, 1 overrun, 2 poll failure, 3 and 4 BIF0 and
 * BIF1 fault, 5 TEXAS BIF0 fault, 6 MMU fault, 7 MMU META fault, 8 MIPS
 * TLB fault, 9 ECC fault, 10 MMU RISC-V fault), dm 76, core_id 80,
 * cr_time_of_kick 88, cr_time_hw_reset_start 96, cr_time_hw_reset_finish
 * 104, cr_time_freelist_ready 112; struct rogue_fwif_hwrinfobuf, 2336
 * bytes: sixteen records, hwr_counter 2176, write_index 2180,
 * dd_req_count 2184, hwr_info_buf_flags 2188, and per data master (nine)
 * the locked-up counts at 2192, overran 2228, recovered 2264 and false
 * detections 2300; struct rogue_fwif_tracebuf_space: trace_pointer 0 of
 * the control's first space at 8).
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/klog.h>

#include "drivers/gpu/powervr/interface.h"
#include "drivers/gpu/powervr/render-private.h"

#define HWR_RECORDS		16U
#define HWR_RECORD_BYTES	136U
#define HWR_COUNTER		2176U
#define HWR_WRITE_INDEX		2180U
#define HWR_DM_COUNT		9U
#define HWR_LOCKED_UP		2192U
#define HWR_OVERRAN		2228U
#define HWR_RECOVERED		2264U
#define HWR_FALSE_DETECT	2300U
#define TRACE_POINTER		8U
/*
 * The newest words of the ring, oldest of them first: the kicks just
 * before the lockup.  The whole ring (1500 lines) pushed the lines before
 * it out of the kernel log's 32 KB.
 */
#define TRACE_TAIL_WORDS	1024U
#define RECORD_WORDS_MAX	96U

/*
 * A BIF fault's two status registers on this core (XE memory):
 * REQ_STATUS address 4..39, tag ID 40..45, tag side band 46..51, read not
 * write 52; MMU_STATUS fault 0, PM/META read only 2, read only 4, data type
 * 5..6, page size 8..10, catalogue base 12..15.  And the requesters' tag
 * IDs.
 *
 * Values referenced: StarFive Linux JH7110_VisionFive2_6.12.y_devel
 * drivers/gpu/drm/img/img-rogue/hwdefs/rogue/km/rgx_cr_defs_km.h
 * (RGX_CR_BIF_FAULT_BANK0_REQ_STATUS__XE_MEM__*, RGX_CR_BIF_FAULT_BANK0_
 * MMU_STATUS_*), rgx_mh_defs_km.h (RGX_MH_TAG_ENCODING_MH_TAG_*) and
 * services/server/devices/rogue/rgxdebug.c (_RGXDecodeBIFReqTagsXE).
 */
#define HWR_TYPE_BIF0		3U
#define HWR_TYPE_TEXAS_BIF0	5U

/*
 * The first catalogue base the parameter manager's own catalogues take
 * (8 to 15: VCE, TE, ZLS and ALIST of its two sets).
 *
 * Values referenced: StarFive Linux JH7110_VisionFive2_6.12.y_devel
 * drivers/gpu/drm/img/img-rogue/services/server/devices/rogue/rgxdebug.c
 * (_RGXDecodePMPC: 0x8 PM-VCE0 to 0xF PM-ALIST1; _RGXDecodeMMULevel: data
 * type 0 page table, 1 directory, 2 catalogue, 3 catalogue base).
 */
#define HWR_PM_CATALOGUE_FIRST	8U

/* The MMU level a fault stopped at (data type 0..3), and the parameter manager's catalogue bases 8..15. */
static const char *const level_names[] = {
	"page table", "page directory", "page catalogue", "catalogue base"
};
static const char *const pm_catalogue_names[] = {
	"PM VCE0", "PM TE0", "PM ZLS0", "PM ALIST0", "PM VCE1", "PM TE1", "PM ZLS1", "PM ALIST1"
};

/* The requesters by tag ID. */
static const char *const tag_names[] = {
	"MMU", "CPU MMU", "CPU IFU", "CPU LSU", "MIPS", "CDM 0", "CDM 1", "CDM 2",
	"CDM 3", "VDM 0", "VDM 1", "VDM 2", "VDM 3", "VDM 4", "PDS 0", "PDS 1",
	"MCU USCA", "MCU USCB", "MCU USCC", "MCU USCD", "MCU PDS USCA", "MCU PDS USCB", "MCU PDS USCC", "MCU PDS USCD",
	"MCU PDSRW", "TCU 0", "TCU 1", "FBCDC 0", "FBCDC 1", "FBCDC 2", "FBCDC 3", "USC",
	"ISP ZLS", "ISP DS", "TPF", "TPF DBIAS", "TPF SPF", "IPF CREQ", "IPF OTHERS", "VDM 5",
	"TA PPP", "TA TPWRTC", "TA TEACRTC", "TA PSGRTC", "TA PSGREGION", "TA PSGSTREAM", "TA TPW", "TA TPC",
	"PM ALLOC", "PM DEALLOC", "TDM DMA", "TDM CTL", "PBE 0", "PBE 1", "PBE 2", "PBE 3"
};

static uint64_t get64(const struct powervr_object *object, uint32_t offset);
static void bif_decode(uint32_t record, uint64_t request, uint64_t status, uint64_t catalogue);

/* Prints the recovery records, the trace's tail and the reset context's record (a release image only the counters). */
void
powervr_hwr_dump(
	struct powervr_core *core,
	const struct powervr_object *context_record)
{
	const struct powervr_object *info;
	const struct powervr_object *trace;
	uint32_t record;
	uint32_t base;
	uint32_t dm;
	uint32_t pointer;
	uint32_t first;
	uint32_t index;
	uint32_t words[8];
	uint32_t done;
	uint32_t count;
	uint32_t type;
	uint64_t catalogue;

	/* The buffer's counters, and each data master's counts. */
	info = &core->firmware.hwr_info;
	kern_logf("powervr: hwr: counter %u write index %u\n",
		  (unsigned)powervr_object_get32(info, HWR_COUNTER), (unsigned)powervr_object_get32(info, HWR_WRITE_INDEX));

	/* A release image says only that: the reset's own notice names the context, its reason and its data master. */
	if (!POWERVR_DEBUG)
		return;
	for (dm = 0; dm < HWR_DM_COUNT; dm++) {
		if (powervr_object_get32(info, HWR_LOCKED_UP + dm * 4U) == 0 && powervr_object_get32(info, HWR_OVERRAN + dm * 4U) == 0 &&
		    powervr_object_get32(info, HWR_RECOVERED + dm * 4U) == 0 && powervr_object_get32(info, HWR_FALSE_DETECT + dm * 4U) == 0)
			continue;
		kern_logf("powervr: hwr: dm %u locked up %u overran %u recovered %u false %u\n", (unsigned)dm,
			  (unsigned)powervr_object_get32(info, HWR_LOCKED_UP + dm * 4U), (unsigned)powervr_object_get32(info, HWR_OVERRAN + dm * 4U),
			  (unsigned)powervr_object_get32(info, HWR_RECOVERED + dm * 4U), (unsigned)powervr_object_get32(info, HWR_FALSE_DETECT + dm * 4U));
	}

	/* Each record the firmware filled. */
	for (record = 0; record < HWR_RECORDS; record++) {
		base = record * HWR_RECORD_BYTES;
		if (powervr_object_get32(info, base + 60U) == 0)
			continue;
		kern_logf("powervr: hwr: record %u: number %u dm %u type %u flags 0x%x event 0x%x frame %u hwrt 0x%x core %u\n",
			  (unsigned)record, (unsigned)powervr_object_get32(info, base + 60U), (unsigned)powervr_object_get32(info, base + 76U),
			  (unsigned)powervr_object_get32(info, base + 72U), (unsigned)powervr_object_get32(info, base + 68U),
			  (unsigned)powervr_object_get32(info, base + 64U), (unsigned)powervr_object_get32(info, base + 48U),
			  (unsigned)powervr_object_get32(info, base + 56U), (unsigned)powervr_object_get32(info, base + 80U));
		kern_logf("powervr: hwr: record %u: data %llx %llx %llx %llx; kick %llx reset %llx..%llx free list %llx timer %llx\n",
			  (unsigned)record, (unsigned long long)get64(info, base), (unsigned long long)get64(info, base + 8U),
			  (unsigned long long)get64(info, base + 16U), (unsigned long long)get64(info, base + 24U),
			  (unsigned long long)get64(info, base + 88U), (unsigned long long)get64(info, base + 96U),
			  (unsigned long long)get64(info, base + 104U), (unsigned long long)get64(info, base + 112U),
			  (unsigned long long)get64(info, base + 32U));
		catalogue = 0;
		type = powervr_object_get32(info, base + 72U);
		if (type >= HWR_TYPE_BIF0 && type <= HWR_TYPE_TEXAS_BIF0) {
			bif_decode(record, get64(info, base), get64(info, base + 8U), get64(info, base + 16U));

			/* A fault through a parameter manager catalogue base (8 and up) names that catalogue's page. */
			if (((get64(info, base + 8U) >> 12) & 0xfU) >= HWR_PM_CATALOGUE_FIRST)
				catalogue = get64(info, base + 16U) & ~0xfffULL;
		}

		/* What the render targets know of the record's data set and the catalogue. */
		powervr_targets_describe_fault(core->render, powervr_object_get32(info, base + 56U), catalogue);
	}

	/*
	 * The trace's newest TRACE_TAIL_WORDS words, for decoding off line: each
	 * entry is its id, a 64-bit timestamp and its arguments, the id as
	 * Linux's pvr_rogue_fwif_sf.h (ROGUE_FW_LOG_CREATESFID) forms it.
	 */
	trace = &core->firmware.trace_buffer;
	pointer = powervr_object_get32(&core->firmware.trace_control, TRACE_POINTER) % POWERVR_TRACE_WORDS;
	first = (pointer + POWERVR_TRACE_WORDS - TRACE_TAIL_WORDS) % POWERVR_TRACE_WORDS;
	kern_logf("powervr: hwr: trace pointer %u, %u words from %u\n", (unsigned)pointer, (unsigned)TRACE_TAIL_WORDS, (unsigned)first);
	for (done = 0; done < TRACE_TAIL_WORDS; done += 8U) {
		for (index = 0; index < 8U; index++)
			words[index] = powervr_object_get32(trace, ((first + done + index) % POWERVR_TRACE_WORDS) * 4U);
		kern_logf("powervr: dump: trace %5u: %08x %08x %08x %08x %08x %08x %08x %08x\n",
			  (unsigned)((first + done) % POWERVR_TRACE_WORDS),
			  (unsigned)words[0], (unsigned)words[1], (unsigned)words[2], (unsigned)words[3],
			  (unsigned)words[4], (unsigned)words[5], (unsigned)words[6], (unsigned)words[7]);
	}

	/* The reset context's firmware record, raw, eight words a line. */
	if (context_record == NULL || context_record->size == 0)
		return;
	count = context_record->size / 4U;
	if (count > RECORD_WORDS_MAX)
		count = RECORD_WORDS_MAX;
	kern_logf("powervr: hwr: context record at 0x%x, %u bytes\n",
		  (unsigned)powervr_object_address(context_record, 0), (unsigned)context_record->size);
	for (done = 0; done < count; done += 8U) {
		for (index = 0; index < 8U; index++) {
			words[index] = 0;
			if (done + index < count)
				words[index] = powervr_object_get32(context_record, (done + index) * 4U);
		}
		kern_logf("powervr: hwr: record +%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n", (unsigned)(done * 4U),
			  (unsigned)words[0], (unsigned)words[1], (unsigned)words[2], (unsigned)words[3],
			  (unsigned)words[4], (unsigned)words[5], (unsigned)words[6], (unsigned)words[7]);
	}
}

/* Reads a 64-bit field as two 32-bit words, low first. */
static uint64_t
get64(
	const struct powervr_object *object,
	uint32_t offset)
{
	/* The low word, then the high. */
	return (uint64_t)powervr_object_get32(object, offset) | ((uint64_t)powervr_object_get32(object, offset + 4U) << 32);
}

/* Prints a BIF fault's status registers decoded: the address, the requester, the access, the catalogue. */
static void
bif_decode(
	uint32_t record,
	uint64_t request,
	uint64_t status,
	uint64_t catalogue)
{
	uint32_t tag;
	uint32_t base;
	const char *name;
	const char *base_name;

	/* The requester by its tag. */
	tag = (uint32_t)((request >> 40) & 0x3fU);
	name = "?";
	if (tag < sizeof(tag_names) / sizeof(tag_names[0]))
		name = tag_names[tag];

	/* The catalogue base: a context's (0 to 7) or one of the parameter manager's. */
	base = (uint32_t)((status >> 12) & 0xfU);
	base_name = "context";
	if (base >= HWR_PM_CATALOGUE_FIRST)
		base_name = pm_catalogue_names[base - HWR_PM_CATALOGUE_FIRST];

	/* The fault, decoded. */
	kern_logf("powervr: hwr: record %u: bif fault at 0x%llx, %s by tag %u (%s) side band %u; mmu fault %u read only %u pm/meta read only %u level %s page size %u catalogue base %u (%s); page catalogue 0x%llx\n",
		  (unsigned)record, (unsigned long long)(request & 0xfffffffff0ULL), ((request >> 52) & 1U) != 0 ? "read" : "write",
		  (unsigned)tag, name, (unsigned)((request >> 46) & 0x3fU),
		  (unsigned)(status & 1U), (unsigned)((status >> 4) & 1U), (unsigned)((status >> 2) & 1U),
		  level_names[(status >> 5) & 3U], (unsigned)((status >> 8) & 7U), (unsigned)base, base_name,
		  (unsigned long long)catalogue);
}
