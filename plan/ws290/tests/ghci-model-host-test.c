/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The self-test of the GHCI 1.0 model and the kernel stubs
 * (ghci-model.c, ghci-stubs.c): hand-built packet sequences in the ring,
 * the reset table, the interrupt line, the three levels of the VM (shared
 * table, per-session tables, flat), session and device faults, MMU_OOM,
 * Bus Master, and the stubs' checks of lock order and waits.
 *
 * The expected values are written from docs/ghci.md (amaterrace-gpu
 * 20bf888), not taken from the model.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "drivers/gpu/gpu.h"
#include "drivers/gpu/ghci/ghci-cmd.h"
#include "drivers/gpu/ghci/ghci-regs.h"
#include "kern/device-io.h"
#include "kern/lock.h"
#include "kern/pmem.h"
#include "kern/waitq.h"
#include "uapi/errno.h"
#include "ghci-model.h"
#include "ghci-stubs.h"

/* The ring of the scenes: 4 KiB, 1024 dwords. */
#define TEST_RING_BYTES			4096U
#define TEST_RING_DWORDS		(TEST_RING_BYTES / 4U)

/* The CP_HEAP of the scenes: 256 KiB, unless a scene gives its own. */
#define TEST_HEAP_BYTES			(256U * 1024U)

/* A packet's largest dwords in the scenes. */
#define TEST_PACKET_DWORDS		64U

/* The GPU VA the VM scenes map: 1 MiB, inside the first 2 MiB chunk. */
#define TEST_VA				0x00100000ULL

/* An address where nothing answers. */
#define TEST_NOWHERE			0xDEAD0000ULL

/*
 * The counts of the checks that held and that failed; main reports both
 * and fails when any check failed.
 */
static unsigned checks_passed;
static unsigned checks_failed;

/*
 * The ring, fence word and heap of the scene that runs: their bus
 * addresses in the model's host memory and the ring's write index the
 * test keeps.  scene_start sets them.
 */
static uint64_t ring_bus;
static uint64_t fence_bus;
static uint64_t heap_bus;
static uint32_t ring_wptr;

/* The interrupts the scene's handler took, and whether it clears what it reads. */
static unsigned handler_calls;
static int handler_clears;

static void expect(int held, const char *name);
static void scene_start(enum model_profile profile, const struct model_switches *switches, uint32_t heap_bytes);
static void handler(void *argument);
static void push(const uint32_t *packet, uint32_t dwords);
static void push_raw(const uint32_t *words, uint32_t dwords, uint32_t doorbell);
static void push_nop(uint32_t dwords);
static void push_fence(uint64_t sequence);
static uint64_t fence_value(void);
static uint32_t fault_kind(void);
static uint32_t fault_session(void);
static int device_faulted(void);
static void scene_identify(void);
static void scene_ring_fence(void);
static void scene_wrap(void);
static void scene_framing(void);
static void scene_reset(void);
static void scene_vm_shared(void);
static void scene_vm_protected(void);
static void scene_oom(void);
static void scene_flat(void);
static void scene_watchdog(void);
static void scene_halt(void);
static void scene_irq_line(void);
static void scene_bus_master(void);
static void scene_locked_and_bus(void);
static void scene_wait_fence(void);
static void scene_stubs(void);
static void scene_display(void);
static void sleep_hook(unsigned sleep, void *argument);
static int test_open(void *device, void **session);
static void test_close(void *device, void *session);
static int test_get_info(void *device, void *session, struct gpu_info *info);

/*
 * Runs every scene and reports the counts.
 */
int
main(void)
{
	/* Keeps the driver's log out of the report. */
	stub.quiet = 1;

	/* Runs the scenes. */
	scene_identify();
	scene_ring_fence();
	scene_wrap();
	scene_framing();
	scene_reset();
	scene_vm_shared();
	scene_vm_protected();
	scene_oom();
	scene_flat();
	scene_watchdog();
	scene_halt();
	scene_irq_line();
	scene_bus_master();
	scene_locked_and_bus();
	scene_wait_fence();
	scene_display();
	scene_stubs();
	model_fini();

	/* Reports the counts; any failure fails the test. */
	printf("ghci-model-host-test: %u passed, %u failed\n", checks_passed, checks_failed);
	if (checks_failed != 0U)
		return 1;

	/* Succeeded: every check held. */
	return 0;
}

/* Counts one check and reports a failed one. */
static void
expect(
	int held,
	const char *name)
{
	/* A check that held is only counted. */
	if (held) {
		checks_passed++;
		return;
	}

	/* A failed check is reported by name. */
	checks_failed++;
	printf("FAIL %s\n", name);
}

/*
 * Builds a running model of a profile: Bus Master on, a 4 KiB ring, a
 * fence word, a CP_HEAP of heap_bytes (none when 0), the interrupt
 * handler, and RUN with IRQ_GLOBAL.
 */
static void
scene_start(
	enum model_profile profile,
	const struct model_switches *switches,
	uint32_t heap_bytes)
{
	int error;

	/* A fresh model and stubs, Bus Master on. */
	model_init(profile, switches);
	stub_reset();
	stub.quiet = 1;
	model_set_bus_master(1);
	handler_calls = 0U;
	handler_clears = 1;
	ring_wptr = 0U;

	/* The ring and the fence word in host memory. */
	error = model_host_alloc(TEST_RING_BYTES, TEST_RING_BYTES, UINT32_MAX, &ring_bus);
	expect(error == 0, "scene: ring allocated");
	error = model_host_alloc(8U, 8U, UINT32_MAX, &fence_bus);
	expect(error == 0, "scene: fence allocated");

	/* The ring's registers, written while IDLE. */
	model_write(GHCI_REG_RING_BASE_LO, (uint32_t)ring_bus);
	model_write(GHCI_REG_RING_BASE_HI, (uint32_t)(ring_bus >> 32));
	model_write(GHCI_REG_RING_SIZE, TEST_RING_BYTES);

	/* The heap, when the scene has one. */
	heap_bus = 0U;
	if (heap_bytes != 0U) {
		error = model_host_alloc(heap_bytes, 4096U, UINT32_MAX, &heap_bus);
		expect(error == 0, "scene: heap allocated");
		model_write(GHCI_REG_CP_HEAP_BASE, (uint32_t)heap_bus);
		model_write(GHCI_REG_CP_HEAP_LIMIT, (uint32_t)heap_bus + heap_bytes);
	}

	/* The interrupts the driver takes, then RUN. */
	model_set_irq_handler(handler, NULL);
	model_write(GHCI_REG_IRQ_ENABLE, GHCI_IRQ_FENCE | GHCI_IRQ_FAULT | GHCI_IRQ_WATCHDOG | GHCI_IRQ_HALTED);
	model_write(GHCI_REG_CTRL, GHCI_CTRL_RUN | GHCI_CTRL_IRQ_GLOBAL | GHCI_CTRL_WDT_EN);
}

/* The scene's interrupt handler: counts, and clears what it reads unless told not to. */
static void
handler(
	void *argument)
{
	uint32_t status;

	/* Reads the events and clears them as the driver does. */
	(void)argument;
	handler_calls++;
	status = model_read(GHCI_REG_IRQ_STATUS);
	if (handler_clears)
		model_write(GHCI_REG_IRQ_STATUS, status);
}

/* Puts one packet in the ring, padding to the end first when it does not fit, and rings the doorbell. */
static void
push(
	const uint32_t *packet,
	uint32_t dwords)
{
	uint32_t *ring;
	uint32_t pad;

	/* Pads the rest of the ring with one NOP when the packet does not fit before the end (section 6.2). */
	ring = (uint32_t *)(void *)model_dma(ring_bus, TEST_RING_BYTES);
	if (ring_wptr + dwords > TEST_RING_DWORDS) {
		pad = TEST_RING_DWORDS - ring_wptr;
		ghci_cmd_nop(&ring[ring_wptr], pad);
		ring_wptr = 0U;
	}

	/* Writes the packet and rings. */
	memcpy(&ring[ring_wptr], packet, dwords * 4U);
	ring_wptr = (ring_wptr + dwords) % TEST_RING_DWORDS;
	model_write(GHCI_REG_RING_WPTR, ring_wptr);
}

/* Puts words at the write index without padding, and rings the doorbell at write index + doorbell. */
static void
push_raw(
	const uint32_t *words,
	uint32_t dwords,
	uint32_t doorbell)
{
	uint32_t *ring;
	uint32_t index;

	/* Writes the words as they are, wrapping only the index. */
	ring = (uint32_t *)(void *)model_dma(ring_bus, TEST_RING_BYTES);
	for (index = 0U; index < dwords; index++)
		ring[(ring_wptr + index) % TEST_RING_DWORDS] = words[index];

	/* Rings at the index the test chose. */
	ring_wptr = (ring_wptr + doorbell) % TEST_RING_DWORDS;
	model_write(GHCI_REG_RING_WPTR, ring_wptr);
}

/* Puts a NOP of any length: only its header is written, the CP skips the rest. */
static void
push_nop(
	uint32_t dwords)
{
	uint32_t header;

	/* The header alone, rung at the end of the NOP. */
	header = ghci_cmd_header(GHCI_OP_NOP, dwords, 0U);
	push_raw(&header, 1U, dwords);
}

/* Puts a fence of a sequence that writes the word and raises the interrupt. */
static void
push_fence(
	uint64_t sequence)
{
	uint32_t packet[TEST_PACKET_DWORDS];
	uint32_t dwords;

	/* Encodes the fence and puts it in the ring. */
	dwords = ghci_cmd_fence(packet, fence_bus, sequence, GHCI_FENCE_FLAG_MEM_WRITE | GHCI_FENCE_FLAG_IRQ);
	push(packet, dwords);
}

/* Reads the fence word. */
static uint64_t
fence_value(void)
{
	uint64_t value;

	/* The 64-bit word the fences write. */
	memcpy(&value, model_dma(fence_bus, 8U), 8U);
	return value;
}

/* The kind of the latched fault. */
static uint32_t
fault_kind(void)
{
	/* FAULT_INFO[3:0]. */
	return model_read(GHCI_REG_FAULT_INFO) & GHCI_FAULT_KIND_MASK;
}

/* The session of the latched fault. */
static uint32_t
fault_session(void)
{
	/* FAULT_INFO[15:8]. */
	return (model_read(GHCI_REG_FAULT_INFO) >> GHCI_FAULT_SESSION_SHIFT) & GHCI_FAULT_SESSION_MASK;
}

/* Reports whether STATUS shows a device fault. */
static int
device_faulted(void)
{
	uint32_t status;

	/* STATUS.FAULTED. */
	status = model_read(GHCI_REG_STATUS);
	if ((status & GHCI_STATUS_FAULTED) != 0U)
		return 1;

	/* Not faulted. */
	return 0;
}

/* Each profile identifies and decodes as design section 14 describes it. */
static void
scene_identify(void)
{
	uint32_t window[GHCI_WINDOW_WORDS];
	struct ghci_caps caps;
	uint32_t index;
	int profile;
	int error;
	int has;

	/* Decodes every profile from a copy of its window. */
	for (profile = 0; profile < MODEL_PROFILES; profile++) {
		/* Copies the window as the driver does, by MMIO through the stubs. */
		model_init((enum model_profile)profile, NULL);
		for (index = 0U; index < GHCI_WINDOW_WORDS; index++)
			window[index] = kern_mmio_read32((const volatile uint8_t *)stub_bar0() + index * 4U);

		/* Every profile is a GHCI 1.0 GPU with a good list. */
		error = ghci_caps_decode(window, &caps);
		expect(error == 0, "identify: decodes");
		if (profile == MODEL_KC705_MIN || profile == MODEL_VA30)
			expect(caps.va_bits == 30U, "identify: 30-bit VA");
		else
			expect(caps.va_bits == 32U, "identify: 32-bit VA");

		/* Picks the profile's own checks. */
		switch (profile) {
		case MODEL_KC705_UMA:
			has = ghci_caps_has(&caps, GHCI_HAS_DISCRETE);
			expect(!has, "identify: kc705-uma is unified");
			has = ghci_caps_has(&caps, GHCI_HAS_DISPLAY);
			expect(has, "identify: kc705-uma has DISP2");
			break;
		case MODEL_KC705_DISCRETE:
		case MODEL_DISCRETE:
			has = ghci_caps_has(&caps, GHCI_HAS_DISCRETE);
			expect(has, "identify: discrete profile has local memory");
			expect(caps.local_base == MODEL_LOCAL_BASE, "identify: LOCAL_BASE");
			break;
		case MODEL_MINIMAL:
			expect(caps.blocks == 0U, "identify: minimal has no blocks");
			break;
		case MODEL_FULL:
			has = ghci_caps_has(&caps, GHCI_HAS_PROT);
			expect(has, "identify: full has PROT");
			expect(caps.prot_caps == 15U, "identify: full PROT_CAPS");
			expect(caps.num_su == 2U, "identify: full has two SUs");
			break;
		case MODEL_FLAT:
			has = ghci_caps_has(&caps, GHCI_HAS_HW_WALKER);
			expect(!has, "identify: flat has no walker");
			break;
		case MODEL_NO_BLT:
			has = ghci_caps_has(&caps, GHCI_HAS_BLT);
			expect(!has, "identify: no-blt has no BLT");
			break;
		case MODEL_NO_SU:
			has = ghci_caps_has(&caps, GHCI_HAS_SHADER_UNITS);
			expect(!has, "identify: no-su has no SU");
			break;
		case MODEL_MISMATCH:
			expect(caps.mismatches == (GHCI_CAP_FP16 | GHCI_CAP_DUAL_SOURCE), "identify: mismatched mirrors");
			break;
		case MODEL_KC705_MIN:
			has = ghci_caps_has(&caps, GHCI_HAS_HW_WALKER) || ghci_caps_has(&caps, GHCI_HAS_UMA) ||
			    ghci_caps_has(&caps, GHCI_HAS_BLT) || ghci_caps_has(&caps, GHCI_HAS_CLIP_POLY) ||
			    ghci_caps_has(&caps, GHCI_HAS_TU_BILINEAR);
			expect(!has, "identify: kc705-min is discrete and flat, without the blitter, the clipper and the filter");
			expect(caps.max_mrt == 1U && caps.max_color == 1U && caps.lds_bytes == 1024U && caps.pa_bits == 40U &&
			       caps.mismatches == 0U, "identify: kc705-min has one attachment and 1 KiB");
			break;
		case MODEL_BLT_MIN:
			has = ghci_caps_has(&caps, GHCI_HAS_BLT_SCALE) || ghci_caps_has(&caps, GHCI_HAS_BLT_BLEND) ||
			    ghci_caps_has(&caps, GHCI_HAS_BLT_TILE) || ghci_caps_has(&caps, GHCI_HAS_TU_BILINEAR);
			expect(ghci_caps_has(&caps, GHCI_HAS_BLT) && !has, "identify: blt-min has a plain blitter");
			break;
		default:
			break;
		}
	}
}

/* A NOP and a fence run: the word, the interrupt, the record, session 0's root. */
static void
scene_ring_fence(void)
{
	uint32_t packet[TEST_PACKET_DWORDS];
	const struct model_command *command;
	uint32_t dwords;

	/* RUN built session 0's root (16 KiB) and opened session 0. */
	scene_start(MODEL_KC705_UMA, NULL, TEST_HEAP_BYTES);
	expect(model_heap_bytes() == GHCI_ROOT_BYTES_VA32, "fence: session 0 root in CP_HEAP");
	expect(model_read(GHCI_REG_SESSION_ACTIVE) == 1U, "fence: session 0 active");

	/* A NOP of three dwords, then a fence. */
	dwords = ghci_cmd_nop(packet, 3U);
	push(packet, dwords);
	push_fence(1U);

	/* The word, the interrupt, the indices and the record. */
	expect(fence_value() == 1U, "fence: word written");
	expect(handler_calls == 1U, "fence: one interrupt");
	expect(model_read(GHCI_REG_RING_RPTR) == ring_wptr, "fence: ring read to the doorbell");
	expect((model_read(GHCI_REG_STATUS) & GHCI_STATUS_RING_EMPTY) != 0U, "fence: ring empty");
	command = model_last_command(GHCI_OP_FENCE);
	expect(command != NULL && command->session == 0U, "fence: recorded in session 0");
	expect(model_count_commands(GHCI_OP_NOP) == 0U, "fence: NOP not recorded");
}

/* A packet that does not fit before the end follows a NOP to the end; one that crosses it faults. */
static void
scene_wrap(void)
{
	uint32_t packet[TEST_PACKET_DWORDS];
	uint32_t index;

	/* Fills the ring with fences until 3 dwords remain, then a fence of 6. */
	scene_start(MODEL_KC705_UMA, NULL, TEST_HEAP_BYTES);
	for (index = 1U; ring_wptr < TEST_RING_DWORDS - 3U - GHCI_DW_FENCE; index++)
		push_fence(index);
	push_nop(TEST_RING_DWORDS - 3U - ring_wptr);
	push_fence(1000U);
	expect(ring_wptr == GHCI_DW_FENCE, "wrap: the fence went to index 0");
	expect(fence_value() == 1000U, "wrap: the fence after the wrap ran");
	expect(!device_faulted(), "wrap: no fault");

	/* A fence that crosses the end is a framing error (kind 3). */
	scene_start(MODEL_KC705_UMA, NULL, TEST_HEAP_BYTES);
	push_nop(255U);
	push_nop(255U);
	push_nop(255U);
	push_nop(TEST_RING_DWORDS - 3U - 3U * 255U);
	ghci_cmd_fence(packet, fence_bus, 7U, GHCI_FENCE_FLAG_MEM_WRITE);
	push_raw(packet, GHCI_DW_FENCE, GHCI_DW_FENCE);
	expect(device_faulted(), "wrap: crossing the end faults the device");
	expect(fault_kind() == GHCI_FAULT_KIND_BAD_REG, "wrap: kind 3");
	expect(fence_value() == 0U, "wrap: the crossing fence did not run");
}

/* Bad headers, opcodes, lengths and guards. */
static void
scene_framing(void)
{
	uint32_t packet[TEST_PACKET_DWORDS];
	uint32_t data[4];

	/* A header of 0 dwords: kind 3. */
	scene_start(MODEL_KC705_UMA, NULL, TEST_HEAP_BYTES);
	packet[0] = ghci_cmd_header(GHCI_OP_NOP, 0U, 0U);
	push_raw(packet, 1U, 1U);
	expect(device_faulted() && fault_kind() == GHCI_FAULT_KIND_BAD_REG, "framing: 0 dwords is kind 3");
	expect(handler_calls == 1U, "framing: one interrupt");

	/* A packet longer than the doorbell made ready: kind 3. */
	scene_start(MODEL_KC705_UMA, NULL, TEST_HEAP_BYTES);
	ghci_cmd_fence(packet, fence_bus, 1U, GHCI_FENCE_FLAG_MEM_WRITE);
	push_raw(packet, GHCI_DW_FENCE, 3U);
	expect(device_faulted() && fault_kind() == GHCI_FAULT_KIND_BAD_REG, "framing: past the write index is kind 3");

	/* An opcode GHCI 1.0 does not define: kind 1. */
	scene_start(MODEL_KC705_UMA, NULL, TEST_HEAP_BYTES);
	packet[0] = ghci_cmd_header(0x7FU, 1U, 0U);
	push_raw(packet, 1U, 1U);
	expect(device_faulted() && fault_kind() == GHCI_FAULT_KIND_INVALID_OP, "framing: unknown opcode is kind 1");

	/* A length the opcode does not allow: kind 3. */
	scene_start(MODEL_KC705_UMA, NULL, TEST_HEAP_BYTES);
	packet[0] = ghci_cmd_header(GHCI_OP_FENCE, 5U, 0U);
	push_raw(packet, 5U, 5U);
	expect(device_faulted() && fault_kind() == GHCI_FAULT_KIND_BAD_REG, "framing: wrong fixed length is kind 3");

	/* A transfer without CAP_BLT: kind 2. */
	scene_start(MODEL_NO_BLT, NULL, TEST_HEAP_BYTES);
	ghci_cmd_copy_buffer(packet, 0x1000U, 0x2000U, 64U, 0U, 0U);
	push(packet, GHCI_DW_COPY_BUFFER);
	expect(device_faulted() && fault_kind() == GHCI_FAULT_KIND_KICK_ABSENT, "framing: COPY_BUFFER without BLT is kind 2");

	/* LOAD_CONST of 4 constants is 3 + 4 dwords and runs. */
	scene_start(MODEL_KC705_UMA, NULL, TEST_HEAP_BYTES);
	memset(data, 0, sizeof(data));
	ghci_cmd_load_const(packet, 0U, 4U, data);
	push(packet, 7U);
	expect(!device_faulted(), "framing: LOAD_CONST 3 + N runs");
	expect(model_count_commands(GHCI_OP_LOAD_CONST) == 1U, "framing: LOAD_CONST recorded");
}

/* CTRL.RESET keeps and clears what section 5.2 says; RUN builds the root again. */
static void
scene_reset(void)
{
	struct model_switches switches;
	uint32_t packet[TEST_PACKET_DWORDS];
	uint32_t ctrl;
	unsigned reads;

	/* Sets the preserved registers, opens a session and faults the device. */
	scene_start(MODEL_KC705_UMA, NULL, TEST_HEAP_BYTES);
	model_write(GHCI_REG_SCRATCH, 0x5A5A5A5AU);
	model_write(GHCI_REG_WDT_CYCLES, 12345U);
	ghci_cmd_session_open(packet, 3U, 0U);
	push(packet, GHCI_DW_SESSION_OPEN);
	push_fence(1U);
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0U, 0);
	expect(device_faulted(), "reset: device faulted before the reset");

	/* The reset. */
	model_write(GHCI_REG_CTRL, GHCI_CTRL_RESET);

	/* Kept: ring, heap, watchdog, interrupt enable, scratch. */
	expect(model_read(GHCI_REG_RING_BASE_LO) == (uint32_t)ring_bus, "reset: RING_BASE kept");
	expect(model_read(GHCI_REG_RING_SIZE) == TEST_RING_BYTES, "reset: RING_SIZE kept");
	expect(model_read(GHCI_REG_CP_HEAP_BASE) == (uint32_t)heap_bus, "reset: CP_HEAP_BASE kept");
	expect(model_read(GHCI_REG_WDT_CYCLES) == 12345U, "reset: WDT_CYCLES kept");
	expect(model_read(GHCI_REG_SCRATCH) == 0x5A5A5A5AU, "reset: SCRATCH kept");
	expect(model_read(GHCI_REG_IRQ_ENABLE) != 0U, "reset: IRQ_ENABLE kept");

	/* Cleared: indices, interrupts, fault, sessions, execution bits, the heap. */
	expect(model_read(GHCI_REG_RING_WPTR) == 0U && model_read(GHCI_REG_RING_RPTR) == 0U, "reset: indices cleared");
	expect(model_read(GHCI_REG_IRQ_STATUS) == 0U, "reset: IRQ_STATUS cleared");
	expect(model_read(GHCI_REG_FAULT_INFO) == 0U, "reset: FAULT_INFO cleared");
	expect(model_read(GHCI_REG_SESSION_ACTIVE) == 0U, "reset: SESSION_ACTIVE cleared");
	expect(model_read(GHCI_REG_CTRL) == 0U, "reset: CTRL cleared");
	expect(model_heap_bytes() == 0U, "reset: CP_HEAP empty");
	expect(!device_faulted(), "reset: FAULTED cleared");

	/* RUN builds session 0's root again, and the ring works from index 0. */
	ring_wptr = 0U;
	model_write(GHCI_REG_CTRL, GHCI_CTRL_RUN | GHCI_CTRL_IRQ_GLOBAL);
	expect(model_heap_bytes() == GHCI_ROOT_BYTES_VA32, "reset: root again after RUN");
	push_fence(2U);
	expect(fence_value() == 2U, "reset: fence after the reset");

	/* RESET reads 1 for the reads the switch gives, then 0. */
	memset(&switches, 0, sizeof(switches));
	switches.reset_reads = 3U;
	switches.mem_ready_reads = 2U;
	scene_start(MODEL_KC705_UMA, &switches, TEST_HEAP_BYTES);
	model_write(GHCI_REG_CTRL, GHCI_CTRL_RESET);
	for (reads = 0U; reads < 10U; reads++) {
		/* Stops when RESET clears. */
		ctrl = model_read(GHCI_REG_CTRL);
		if ((ctrl & GHCI_CTRL_RESET) == 0U)
			break;
	}
	expect(reads == 3U, "reset: RESET clears after the given reads");
	expect((model_read(GHCI_REG_STATUS) & GHCI_STATUS_MEM_READY) == 0U, "reset: MEM_READY late (1)");
	expect((model_read(GHCI_REG_STATUS) & GHCI_STATUS_MEM_READY) == 0U, "reset: MEM_READY late (2)");
	expect((model_read(GHCI_REG_STATUS) & GHCI_STATUS_MEM_READY) != 0U, "reset: MEM_READY shows");

	/* A stuck reset never clears and reports AXI_STUCK. */
	memset(&switches, 0, sizeof(switches));
	switches.reset_stuck = 1;
	scene_start(MODEL_KC705_UMA, &switches, TEST_HEAP_BYTES);
	model_write(GHCI_REG_CTRL, GHCI_CTRL_RESET);
	expect((model_read(GHCI_REG_CTRL) & GHCI_CTRL_RESET) != 0U, "reset: stuck reset stays");
	expect((model_read(GHCI_REG_STATUS) & GHCI_STATUS_AXI_STUCK) != 0U, "reset: AXI_STUCK");
}

/* Without PROT: one shared table, collisions, CLOSE keeps the PTEs, the closed-session cases. */
static void
scene_vm_shared(void)
{
	struct model_switches switches;
	uint32_t packet[TEST_PACKET_DWORDS];
	uint64_t heap_after_map;

	/* Opens sessions 1 and 2; without PROT no root is added. */
	scene_start(MODEL_KC705_UMA, NULL, TEST_HEAP_BYTES);
	ghci_cmd_session_open(packet, 1U, 0U);
	push(packet, GHCI_DW_SESSION_OPEN);
	ghci_cmd_session_open(packet, 2U, 0U);
	push(packet, GHCI_DW_SESSION_OPEN);
	expect(model_read(GHCI_REG_SESSION_ACTIVE) == 0x7U, "vm: sessions 0, 1 and 2 active");
	expect(model_heap_bytes() == GHCI_ROOT_BYTES_VA32, "vm: no root for a session without PROT");

	/* Session 1 maps 2 MiB from 1 MiB: two L0 tables. */
	ghci_cmd_vm_map(packet, 1U, GHCI_VM_FLAG_READ | GHCI_VM_FLAG_WRITE, TEST_VA, MODEL_HOST_BASE, 0x200000U);
	push(packet, GHCI_DW_VM_MAP);
	heap_after_map = model_heap_bytes();
	expect(heap_after_map == GHCI_ROOT_BYTES_VA32 + 2U * GHCI_TABLE_BYTES, "vm: two L0 tables");
	expect(model_lookup(1U, TEST_VA + 0x1000U) != NULL, "vm: session 1 translates");
	expect(model_lookup(2U, TEST_VA + 0x1000U) != NULL, "vm: the shared table translates for session 2 too");

	/* Session 2 maps elsewhere; an unmap of part of session 1 keeps the rest. */
	ghci_cmd_vm_map(packet, 2U, GHCI_VM_FLAG_READ, 0x10000000ULL, MODEL_HOST_BASE, 0x1000U);
	push(packet, GHCI_DW_VM_MAP);
	ghci_cmd_vm_unmap(packet, 1U, TEST_VA, 0x100000U);
	push(packet, GHCI_DW_VM_UNMAP);
	expect(model_lookup(1U, TEST_VA) == NULL, "vm: unmapped part gone");
	expect(model_lookup(1U, TEST_VA + 0x100000U) != NULL, "vm: the rest stays");
	expect(model_heap_bytes() == GHCI_ROOT_BYTES_VA32 + 3U * GHCI_TABLE_BYTES, "vm: VM_UNMAP keeps the L0 tables (d9252e4 section 11.6)");

	/* CLOSE of session 1 keeps the shared PTEs (N49); the session is gone. */
	ghci_cmd_session_close(packet, 1U);
	push(packet, GHCI_DW_SESSION_CLOSE);
	expect((model_read(GHCI_REG_SESSION_ACTIVE) & 0x2U) == 0U, "vm: session 1 closed");
	expect(model_lookup(1U, TEST_VA + 0x100000U) != NULL, "vm: CLOSE kept the shared PTE");

	/* VM_UNMAP and CLOSE of a closed session are no-ops (N49). */
	ghci_cmd_vm_unmap(packet, 1U, TEST_VA + 0x100000U, 0x100000U);
	push(packet, GHCI_DW_VM_UNMAP);
	ghci_cmd_session_close(packet, 1U);
	push(packet, GHCI_DW_SESSION_CLOSE);
	expect(!device_faulted(), "vm: closed-session UNMAP and CLOSE are no-ops");

	/* A map that collides in the shared table is kind 10, here a device fault. */
	ghci_cmd_vm_map(packet, 2U, GHCI_VM_FLAG_READ, TEST_VA + 0x100000U, MODEL_HOST_BASE, 0x1000U);
	push(packet, GHCI_DW_VM_MAP);
	expect(device_faulted() && fault_kind() == GHCI_FAULT_KIND_PERM_FAULT, "vm: collision is kind 10");

	/* VM_MAP to a session that is not open: kind 1. */
	scene_start(MODEL_KC705_UMA, NULL, TEST_HEAP_BYTES);
	ghci_cmd_vm_map(packet, 4U, GHCI_VM_FLAG_READ, TEST_VA, MODEL_HOST_BASE, 0x1000U);
	push(packet, GHCI_DW_VM_MAP);
	expect(device_faulted() && fault_kind() == GHCI_FAULT_KIND_INVALID_OP, "vm: map to a closed session is kind 1");

	/* The pessimistic switches: CLOSE clears the PTEs, UNMAP of a closed session faults. */
	memset(&switches, 0, sizeof(switches));
	switches.prot0_close_clears = 1;
	switches.unmap_closed_faults = 1;
	scene_start(MODEL_KC705_UMA, &switches, TEST_HEAP_BYTES);
	ghci_cmd_session_open(packet, 1U, 0U);
	push(packet, GHCI_DW_SESSION_OPEN);
	ghci_cmd_vm_map(packet, 1U, GHCI_VM_FLAG_READ, TEST_VA, MODEL_HOST_BASE, 0x1000U);
	push(packet, GHCI_DW_VM_MAP);
	ghci_cmd_session_close(packet, 1U);
	push(packet, GHCI_DW_SESSION_CLOSE);
	expect(model_lookup(1U, TEST_VA) == NULL, "vm switch: CLOSE cleared the PTEs");
	ghci_cmd_vm_unmap(packet, 1U, TEST_VA, 0x1000U);
	push(packet, GHCI_DW_VM_UNMAP);
	expect(device_faulted() && fault_kind() == GHCI_FAULT_KIND_INVALID_OP, "vm switch: UNMAP of a closed session is kind 1");
}

/* With PROT: a table and root per session, CLOSE returns them, a client fault stays in its session. */
static void
scene_vm_protected(void)
{
	struct model_switches switches;
	uint32_t packet[TEST_PACKET_DWORDS];
	const struct model_command *command;
	uint32_t index;

	/* Two sessions, each with its root, may map the same VA. */
	scene_start(MODEL_PROT, NULL, TEST_HEAP_BYTES);
	for (index = 1U; index <= 2U; index++) {
		/* Opens the session and maps the same VA. */
		ghci_cmd_session_open(packet, index, 0U);
		push(packet, GHCI_DW_SESSION_OPEN);
		ghci_cmd_vm_map(packet, index, GHCI_VM_FLAG_READ, TEST_VA, MODEL_HOST_BASE + index * 0x1000U, 0x1000U);
		push(packet, GHCI_DW_VM_MAP);
	}
	expect(!device_faulted(), "prot: the same VA in two sessions");
	expect(model_heap_bytes() == 3U * GHCI_ROOT_BYTES_VA32 + 2U * GHCI_TABLE_BYTES, "prot: three roots and two L0");
	expect(model_lookup(2U, TEST_VA)->phys == MODEL_HOST_BASE + 0x2000U, "prot: session 2 has its own table");

	/* VM_UNMAP keeps the tables; CLOSE returns the session's root and tables (N48). */
	ghci_cmd_vm_unmap(packet, 1U, TEST_VA, 0x1000U);
	push(packet, GHCI_DW_VM_UNMAP);
	expect(model_heap_bytes() == 3U * GHCI_ROOT_BYTES_VA32 + 2U * GHCI_TABLE_BYTES, "prot: VM_UNMAP keeps the L0 table");
	ghci_cmd_session_close(packet, 1U);
	push(packet, GHCI_DW_SESSION_CLOSE);
	expect(model_heap_bytes() == 2U * GHCI_ROOT_BYTES_VA32 + GHCI_TABLE_BYTES, "prot: CLOSE returned the tables");

	/* A fault at session 2's draw stops only session 2; its next draw is skipped, the fence is written. */
	model_inject(GHCI_FAULT_KIND_PAGE_FAULT, GHCI_UNIT_SU, 2U, TEST_VA, 1);
	ghci_cmd_bind_session(packet, 2U);
	push(packet, GHCI_DW_BIND_SESSION);
	ghci_cmd_draw(packet, 3U, 1U, 0U, 0U);
	push(packet, GHCI_DW_DRAW);
	push(packet, GHCI_DW_DRAW);
	ghci_cmd_bind_session(packet, 0U);
	push(packet, GHCI_DW_BIND_SESSION);
	push_fence(9U);
	expect(!device_faulted(), "prot: a client fault does not fault the device");
	expect(fault_kind() == GHCI_FAULT_KIND_PAGE_FAULT && fault_session() == 2U, "prot: FAULT_INFO names session 2");
	expect((model_read(GHCI_REG_SESSION_ACTIVE) & 0x4U) == 0U, "prot: session 2 left SESSION_ACTIVE");
	command = model_last_command(GHCI_OP_DRAW);
	expect(command != NULL && command->skipped == 1U, "prot: the faulted session's draw is skipped");
	expect(fence_value() == 9U, "prot: the fence after the fault is written");

	/* The switch keeps a closed session's tables in CP_HEAP. */
	memset(&switches, 0, sizeof(switches));
	switches.close_keeps_heap = 1;
	scene_start(MODEL_PROT, &switches, TEST_HEAP_BYTES);
	ghci_cmd_session_open(packet, 1U, 0U);
	push(packet, GHCI_DW_SESSION_OPEN);
	ghci_cmd_session_close(packet, 1U);
	push(packet, GHCI_DW_SESSION_CLOSE);
	expect(model_heap_bytes() == 2U * GHCI_ROOT_BYTES_VA32, "prot switch: CLOSE kept the root");
}

/* MMU_OOM: a client session faults alone, session 0 faults the device, RUN without a heap faults. */
static void
scene_oom(void)
{
	uint32_t packet[TEST_PACKET_DWORDS];

	/* A heap of the root and one L0 table: a map of two chunks does not fit. */
	scene_start(MODEL_KC705_UMA, NULL, GHCI_ROOT_BYTES_VA32 + GHCI_TABLE_BYTES);
	ghci_cmd_session_open(packet, 1U, 0U);
	push(packet, GHCI_DW_SESSION_OPEN);
	ghci_cmd_vm_map(packet, 1U, GHCI_VM_FLAG_READ, TEST_VA, MODEL_HOST_BASE, 0x200000U);
	push(packet, GHCI_DW_VM_MAP);
	push_fence(5U);
	expect(fault_kind() == GHCI_FAULT_KIND_MMU_OOM && fault_session() == 1U, "oom: session 1 MMU_OOM");
	expect(!device_faulted(), "oom: the device keeps running");
	expect((model_read(GHCI_REG_SESSION_ACTIVE) & 0x2U) == 0U, "oom: session 1 left SESSION_ACTIVE");
	expect(model_lookup(1U, TEST_VA) == NULL, "oom: the map was rolled back");
	expect(model_heap_bytes() == GHCI_ROOT_BYTES_VA32, "oom: CP_HEAP as before");
	expect(fence_value() == 5U, "oom: the fence after it is written");

	/* The same for session 0 faults the device. */
	scene_start(MODEL_KC705_UMA, NULL, GHCI_ROOT_BYTES_VA32 + GHCI_TABLE_BYTES);
	ghci_cmd_vm_map(packet, 0U, GHCI_VM_FLAG_READ, TEST_VA, MODEL_HOST_BASE, 0x200000U);
	push(packet, GHCI_DW_VM_MAP);
	expect(device_faulted() && fault_kind() == GHCI_FAULT_KIND_MMU_OOM, "oom: session 0 MMU_OOM faults the device");

	/* RUN without a heap cannot build session 0's root. */
	scene_start(MODEL_KC705_UMA, NULL, 0U);
	expect(device_faulted() && fault_kind() == GHCI_FAULT_KIND_MMU_OOM, "oom: RUN without CP_HEAP faults");
}

/* The flat device has no walker: SESSION_OPEN and VM_MAP are kind 2. */
static void
scene_flat(void)
{
	uint32_t packet[TEST_PACKET_DWORDS];

	/* SESSION_OPEN of a client session. */
	scene_start(MODEL_FLAT, NULL, GHCI_ROOT_BYTES_VA32);
	ghci_cmd_session_open(packet, 1U, 0U);
	push(packet, GHCI_DW_SESSION_OPEN);
	expect(device_faulted() && fault_kind() == GHCI_FAULT_KIND_KICK_ABSENT, "flat: SESSION_OPEN is kind 2");

	/* VM_MAP of session 0. */
	scene_start(MODEL_FLAT, NULL, GHCI_ROOT_BYTES_VA32);
	ghci_cmd_vm_map(packet, 0U, GHCI_VM_FLAG_READ, TEST_VA, MODEL_HOST_BASE, 0x1000U);
	push(packet, GHCI_DW_VM_MAP);
	expect(device_faulted() && fault_kind() == GHCI_FAULT_KIND_KICK_ABSENT, "flat: VM_MAP is kind 2");

	/* A fence works. */
	scene_start(MODEL_FLAT, NULL, GHCI_ROOT_BYTES_VA32);
	push_fence(4U);
	expect(fence_value() == 4U, "flat: fence");
}

/* The watchdog: both bits; session 0 faults the device, a PROT client only its session. */
static void
scene_watchdog(void)
{
	uint32_t status;

	/* Session 0. */
	scene_start(MODEL_KC705_UMA, NULL, TEST_HEAP_BYTES);
	handler_clears = 0;
	model_inject_watchdog(0U);
	status = model_read(GHCI_REG_IRQ_STATUS);
	expect((status & (GHCI_IRQ_WATCHDOG | GHCI_IRQ_FAULT)) == (GHCI_IRQ_WATCHDOG | GHCI_IRQ_FAULT), "watchdog: both bits");
	expect(device_faulted() && fault_kind() == GHCI_FAULT_KIND_WATCHDOG, "watchdog: session 0 faults the device");
	expect(model.irq_left_high == 1U, "watchdog: a handler that does not clear leaves the line high");

	/* A client session with PROT. */
	scene_start(MODEL_PROT, NULL, TEST_HEAP_BYTES);
	model_inject_watchdog(1U);
	expect(!device_faulted() && fault_session() == 1U, "watchdog: a PROT client stays in its session");
}

/* HALT: STATUS shows HALTED after the given reads, IRQ_HALTED; the ring waits until HALT clears. */
static void
scene_halt(void)
{
	struct model_switches switches;
	uint32_t status;

	/* HALTED shows on the third read. */
	memset(&switches, 0, sizeof(switches));
	switches.halt_reads = 2U;
	scene_start(MODEL_KC705_UMA, &switches, TEST_HEAP_BYTES);
	handler_clears = 0;
	model_write(GHCI_REG_CTRL, GHCI_CTRL_RUN | GHCI_CTRL_IRQ_GLOBAL | GHCI_CTRL_HALT);
	status = model_read(GHCI_REG_STATUS);
	expect((status & GHCI_STATUS_HALTED) == 0U, "halt: not yet (1)");
	status = model_read(GHCI_REG_STATUS);
	expect((status & GHCI_STATUS_HALTED) == 0U, "halt: not yet (2)");
	status = model_read(GHCI_REG_STATUS);
	expect((status & GHCI_STATUS_HALTED) != 0U, "halt: HALTED");
	expect((model_read(GHCI_REG_IRQ_STATUS) & GHCI_IRQ_HALTED) != 0U, "halt: IRQ_HALTED");

	/* The ring waits while halted and runs after. */
	push_fence(3U);
	expect(fence_value() == 0U, "halt: no packet while halted");
	model_write(GHCI_REG_CTRL, GHCI_CTRL_RUN | GHCI_CTRL_IRQ_GLOBAL);
	expect(fence_value() == 3U, "halt: the packet runs after HALT clears");
}

/* The line: IRQ_GLOBAL and IRQ_ENABLE gate it; enabling a pending event raises it. */
static void
scene_irq_line(void)
{
	/* Without IRQ_GLOBAL the fence's event stays pending. */
	scene_start(MODEL_KC705_UMA, NULL, TEST_HEAP_BYTES);
	model_write(GHCI_REG_CTRL, GHCI_CTRL_RUN);
	push_fence(1U);
	expect(handler_calls == 0U, "irq: no call without IRQ_GLOBAL");
	expect((model_read(GHCI_REG_IRQ_STATUS) & GHCI_IRQ_FENCE) != 0U, "irq: FENCE pending");

	/* IRQ_GLOBAL raises it. */
	model_write(GHCI_REG_CTRL, GHCI_CTRL_RUN | GHCI_CTRL_IRQ_GLOBAL);
	expect(handler_calls == 1U, "irq: IRQ_GLOBAL raises the pending event");

	/* SOFT through IRQ_SET, gated by IRQ_ENABLE. */
	model_write(GHCI_REG_IRQ_SET, GHCI_IRQ_SOFT);
	expect(handler_calls == 1U, "irq: SOFT not enabled");
	model_write(GHCI_REG_IRQ_ENABLE, GHCI_IRQ_SOFT);
	expect(handler_calls == 2U, "irq: enabling SOFT raises it");
	expect(model.irq_left_high == 0U, "irq: the handler cleared the line");
}

/* Without Bus Master the processor fetches nothing. */
static void
scene_bus_master(void)
{
	/* The doorbell is counted as a DMA without Bus Master. */
	scene_start(MODEL_KC705_UMA, NULL, TEST_HEAP_BYTES);
	model_set_bus_master(0);
	push_fence(1U);
	expect(model.dma_without_master == 1U, "bus master: fetch refused");
	expect(model_read(GHCI_REG_RING_RPTR) == 0U, "bus master: nothing read");

	/* Setting it lets the ring run. */
	model_set_bus_master(1);
	expect(fence_value() == 1U, "bus master: the ring runs after");
}

/* A locked register written while running is kind 7; a fence to nowhere is a bus error; a held processor waits. */
static void
scene_locked_and_bus(void)
{
	uint32_t packet[TEST_PACKET_DWORDS];

	/* RING_BASE while running. */
	scene_start(MODEL_KC705_UMA, NULL, TEST_HEAP_BYTES);
	model_write(GHCI_REG_CP_HEAP_LIMIT, (uint32_t)heap_bus + TEST_HEAP_BYTES + 4096U);
	expect(!device_faulted(), "locked: CP_HEAP_LIMIT may grow while running");
	model_write(GHCI_REG_RING_BASE_LO, 0U);
	expect(device_faulted() && fault_kind() == GHCI_FAULT_KIND_LOCKED_REG, "locked: RING_BASE while running is kind 7");
	expect(model_read(GHCI_REG_RING_BASE_LO) == (uint32_t)ring_bus, "locked: the write was refused");

	/* A fence to an address nothing answers. */
	scene_start(MODEL_KC705_UMA, NULL, TEST_HEAP_BYTES);
	ghci_cmd_fence(packet, TEST_NOWHERE, 1U, GHCI_FENCE_FLAG_MEM_WRITE);
	push(packet, GHCI_DW_FENCE);
	expect(device_faulted() && fault_kind() == GHCI_FAULT_KIND_AXI_ERROR, "bus: fence to nowhere is kind 5");

	/* A held processor: a hang without a fault. */
	scene_start(MODEL_KC705_UMA, NULL, TEST_HEAP_BYTES);
	model_hold(1);
	push_fence(6U);
	expect(fence_value() == 0U && !device_faulted(), "hold: nothing runs, nothing faults");
	model_hold(0);
	expect(fence_value() == 6U, "hold: released, the fence runs");
}

/* WAIT_FENCE stops the processor at the packet until the word reaches the value. */
static void
scene_wait_fence(void)
{
	uint32_t packet[TEST_PACKET_DWORDS];
	uint64_t wait_bus;
	uint64_t value;
	uint32_t status;
	int error;

	/* Waits for 5 at a word of its own, then a fence. */
	scene_start(MODEL_KC705_UMA, NULL, TEST_HEAP_BYTES);
	error = model_host_alloc(8U, 8U, UINT32_MAX, &wait_bus);
	expect(error == 0, "wait: word allocated");
	ghci_cmd_wait_fence(packet, wait_bus, 5U);
	push(packet, GHCI_DW_WAIT_FENCE);
	push_fence(1U);
	status = model_read(GHCI_REG_STATUS);
	expect(((status >> GHCI_STATUS_CP_STATE_SHIFT) & GHCI_STATUS_CP_STATE_MASK) == 4U, "wait: CP_STATE WAIT_FENCE");
	expect(fence_value() == 0U, "wait: the fence waits");

	/* The word reaches 5; the next tick lets it go. */
	value = 5U;
	memcpy(model_dma(wait_bus, 8U), &value, 8U);
	model_tick();
	expect(fence_value() == 1U, "wait: the fence ran after the word passed");
}

/*
 * The stubs: MMIO into the model, the lock order, the forbidden waits,
 * the sleep hook and clock, RAM from the model, and the registration
 * checks of gpu.c.
 */
static void
scene_stubs(void)
{
	static const char *const order[] = { "device", "display", "cp", "irq" };
	static const struct drv_gpu_ops good_ops = {
		.version = DRV_GPU_INTERFACE_VERSION,
		.size = sizeof(struct drv_gpu_ops),
		.open = test_open,
		.close = test_close,
		.get_info = test_get_info
	};
	struct drv_gpu_ops bad_ops;
	struct drv_gpu_device *device;
	struct kern_pmem run;
	struct wait_queue gate;
	struct mutex device_lock;
	struct mutex cp_lock;
	struct spinlock irq_lock;
	unsigned long saved;
	unsigned hook_calls;
	void *view;
	int error;

	/* MMIO writes reach the model. */
	scene_start(MODEL_KC705_UMA, NULL, TEST_HEAP_BYTES);
	kern_mmio_write32((volatile uint8_t *)stub_bar0() + GHCI_REG_SCRATCH, 0xA5A5A5A5U);
	expect(model_read(GHCI_REG_SCRATCH) == 0xA5A5A5A5U, "stubs: MMIO write");

	/* The order device, cp, irq holds; cp then device does not. */
	stub_lock_order(order, 4U);
	mutex_init(&device_lock, LOCK_RANK_DEVICE, "device");
	mutex_init(&cp_lock, LOCK_RANK_DEVICE, "cp");
	spin_init(&irq_lock, LOCK_RANK_DEVICE, "irq");
	mutex_lock(&device_lock);
	mutex_lock(&cp_lock);
	saved = spin_lock_irqsave(&irq_lock);
	spin_unlock_irqrestore(&irq_lock, saved);
	mutex_unlock(&cp_lock);
	mutex_unlock(&device_lock);
	expect(stub.order_violations == 0U, "stubs: the order holds");
	stub.quiet = 1;
	mutex_lock(&cp_lock);
	mutex_lock(&device_lock);
	mutex_unlock(&device_lock);
	mutex_unlock(&cp_lock);
	expect(stub.order_violations == 1U, "stubs: cp before device is a violation");

	/* A mutex locked twice. */
	mutex_lock(&cp_lock);
	mutex_lock(&cp_lock);
	mutex_unlock(&cp_lock);
	expect(stub.double_locks == 1U, "stubs: a double lock is counted");
	expect(stub.held_count == 1U, "stubs: one entry is still held");

	/* Starts again from no locks held. */
	stub_reset();
	stub_lock_order(order, 4U);

	/* Waiting at the gate while holding device is forbidden; the hook runs in the wait. */
	stub_forbid_wait("gate", "device");
	waitq_init(&gate, "gate");
	hook_calls = 0U;
	stub_set_sleep_hook(sleep_hook, &hook_calls);
	waitq_sleep(&gate, NULL, 0U, 0U, 0U);
	expect(stub.wait_violations == 0U, "stubs: a wait without the lock");
	mutex_lock(&device_lock);
	waitq_sleep(&gate, NULL, 0U, 0U, 0U);
	mutex_unlock(&device_lock);
	expect(stub.wait_violations == 1U, "stubs: a wait holding device is a violation");
	expect(hook_calls == 2U && stub.ticks == 2U, "stubs: the hook and the clock follow each sleep");

	/* RAM comes from the model and an uncached view is its bytes. */
	error = kern_pmem_alloc_limited(8192U, 4096U, UINT32_MAX, 0U, &run);
	expect(error == 0 && run.paddr >= MODEL_HOST_BASE, "stubs: RAM from the model");
	error = kern_pmem_map_uncached(&run, &view);
	expect(error == 0 && view == model_dma(run.paddr, 1U), "stubs: uncached view");

	/* gpu.c's checks: a table without open is refused, a complete one is taken. */
	bad_ops = good_ops;
	bad_ops.open = NULL;
	error = drv_gpu_register(&bad_ops, NULL, &device);
	expect(error == EINVAL && stub.register_errors == 1U, "stubs: registration without open refused");
	error = drv_gpu_register(&good_ops, NULL, &device);
	expect(error == 0 && device != NULL && stub.registrations == 1U, "stubs: registration taken");
}

/* Counts the hook's calls. */
static void
sleep_hook(
	unsigned sleep,
	void *argument)
{
	unsigned *calls;

	/* One more call. */
	(void)sleep;
	calls = argument;
	(*calls)++;
}

/* The operations of the registration check do nothing. */
static int
test_open(
	void *device,
	void **session)
{
	/* No session state. */
	(void)device;
	*session = NULL;
	return 0;
}

static void
test_close(
	void *device,
	void *session)
{
	/* Nothing to free. */
	(void)device;
	(void)session;
}

static int
test_get_info(
	void *device,
	void *session,
	struct gpu_info *info)
{
	/* No information. */
	(void)device;
	(void)session;
	(void)info;
	return 0;
}

/*
 * The display controller of DISP2 (section 13.2): the mode table, the
 * ring's lighting (a flip taken at once while dark, its record and
 * TAKEN0), a flip pending until the vsync, the display event and its
 * summary in IRQ_STATUS[3], the host's scan-out writes refused while the
 * processor runs and taken while it is halted, a record due while halted
 * and written by the reset, FB_NEXT refused to DISP_SET, and the stop
 * cancelling a pending flip.
 */
static void
scene_display(void)
{
	uint32_t packet[TEST_PACKET_DWORDS];
	uint32_t offsets[4];
	uint32_t values[4];
	uint32_t dwords;
	uint32_t record[4];
	uint64_t record_bus;
	uint32_t block;
	int error;

	/* A device with the controller, its events enabled, and a record. */
	scene_start(MODEL_KC705_UMA, NULL, TEST_HEAP_BYTES);
	block = MODEL_DISP2_BLOCK;
	model_write(GHCI_REG_IRQ_ENABLE, GHCI_IRQ_FENCE | GHCI_IRQ_DISPLAY);
	model_write(block + GHCI_DC_IRQ_ENABLE, GHCI_DC_IRQ_FLIP_DONE | GHCI_DC_IRQ_CURSOR_DONE);
	error = model_host_alloc(16U, 16U, UINT32_MAX, &record_bus);
	expect(error == 0, "display: record allocated");

	/* The mode table. */
	model_write(block + GHCI_DC_MODE_INDEX, 1U);
	expect(model_read(block + GHCI_DC_MODE_H0) == (640U | (96U << 16)) && model_read(block + GHCI_DC_MODE_CLOCK) == 25175U &&
	    ((model_read(block + GHCI_DC_CAPS) >> GHCI_DC_CAPS_MODES_SHIFT) & GHCI_DC_CAPS_MODES_MASK) == 2U, "display: modes");

	/* The ring lights it: CTRL off waiting for the idle, the stride, a flip taken at once, CTRL on. */
	offsets[0] = GHCI_DC_CTRL;
	values[0] = 0U;
	dwords = ghci_cmd_disp_set(packet, 1U, 1U, offsets, values);
	push(packet, dwords);
	offsets[0] = GHCI_DC_FB_STRIDE;
	values[0] = 2560U;
	dwords = ghci_cmd_disp_set(packet, 0U, 1U, offsets, values);
	push(packet, dwords);
	dwords = ghci_cmd_flip(packet, 0U, 0x10400000U, GHCI_FLIP_VSYNC_WAIT | GHCI_FLIP_RECORD_ENABLE, 5U, (uint32_t)record_bus);
	push(packet, dwords);
	memcpy(record, model_dma(record_bus, 16U), 16U);
	expect(model_read(block + GHCI_DC_FB_BASE) == 0x10400000U && model_read(block + GHCI_DC_TAKEN0) == 5U && record[0] == 5U &&
	    model_read(block + GHCI_DC_FB_STRIDE) == 2560U, "display: a flip taken at once while dark");
	expect((model_read(GHCI_REG_IRQ_STATUS) & GHCI_IRQ_DISPLAY) != 0U && handler_calls != 0U &&
	    model.display.irq_status == GHCI_DC_IRQ_FLIP_DONE, "display: FLIP_DONE summed up in IRQ_STATUS[3]");
	model_write(block + GHCI_DC_IRQ_STATUS, GHCI_DC_IRQ_ALL);
	model_write(GHCI_REG_IRQ_STATUS, GHCI_IRQ_DISPLAY);
	expect((model_read(GHCI_REG_IRQ_STATUS) & GHCI_IRQ_DISPLAY) == 0U, "display: IRQ_STATUS[3] clears after DISP_IRQ_STATUS");
	offsets[0] = GHCI_DC_CTRL;
	values[0] = GHCI_DC_CTRL_ENABLE;
	dwords = ghci_cmd_disp_set(packet, 0U, 1U, offsets, values);
	push(packet, dwords);
	expect((model_read(block + GHCI_DC_STATUS) & GHCI_DC_ST_RUNNING) != 0U && model.display.idle_waits_lit == 0U, "display: lit");

	/* A flip while lit waits for the vsync. */
	dwords = ghci_cmd_flip(packet, 0U, 0x10800000U, GHCI_FLIP_VSYNC_WAIT | GHCI_FLIP_RECORD_ENABLE, 6U, (uint32_t)record_bus);
	push(packet, dwords);
	expect((model_read(block + GHCI_DC_STATUS) & GHCI_DC_ST_PENDING0) != 0U && model_read(block + GHCI_DC_FB_BASE) == 0x10400000U,
	    "display: pending until the vsync");
	model_display_vsync();
	memcpy(record, model_dma(record_bus, 16U), 16U);
	expect(model_read(block + GHCI_DC_FB_BASE) == 0x10800000U && record[0] == 6U && record[1] == 1U, "display: taken at the vsync");

	/* The host's scan-out write while the processor runs is refused; halted, it is taken. */
	model_write(block + GHCI_DC_FB_STRIDE, 4096U);
	expect(model.display.bad_writes == 1U && (model_read(block + GHCI_DC_STATUS) & GHCI_DC_ST_BAD_WRITE) != 0U,
	    "display: BAD_WRITE while running");
	model_write(block + GHCI_DC_STATUS, GHCI_DC_ST_BAD_WRITE);
	model_write(GHCI_REG_CTRL, GHCI_CTRL_RUN | GHCI_CTRL_IRQ_GLOBAL | GHCI_CTRL_HALT);
	model_write(block + GHCI_DC_CURSOR_CTRL, 1U);
	expect(model.display.bad_writes == 1U && model_read(block + GHCI_DC_CURSOR_CTRL) == 1U, "display: the cursor is the host's in every state");

	/* A flip pending at the halt is taken at the vsync; its record waits for the reset. */
	model_write(GHCI_REG_CTRL, GHCI_CTRL_RUN | GHCI_CTRL_IRQ_GLOBAL);
	dwords = ghci_cmd_flip(packet, 0U, 0x10C00000U, GHCI_FLIP_VSYNC_WAIT | GHCI_FLIP_RECORD_ENABLE, 7U, (uint32_t)record_bus);
	push(packet, dwords);
	model_write(GHCI_REG_CTRL, GHCI_CTRL_RUN | GHCI_CTRL_IRQ_GLOBAL | GHCI_CTRL_HALT);
	model_display_vsync();
	memcpy(record, model_dma(record_bus, 16U), 16U);
	expect(record[0] == 6U && (model_read(block + GHCI_DC_STATUS) & GHCI_DC_ST_RECORD) != 0U &&
	    model_read(block + GHCI_DC_TAKEN0) == 7U, "display: taken while halted, record due");
	model_write(GHCI_REG_CTRL, GHCI_CTRL_RESET);
	memcpy(record, model_dma(record_bus, 16U), 16U);
	expect(record[0] == 7U && (model_read(block + GHCI_DC_STATUS) & GHCI_DC_ST_RECORD) == 0U &&
	    (model_read(block + GHCI_DC_CTRL) & GHCI_DC_CTRL_ENABLE) != 0U, "display: record written by the reset, scan-out kept");

	/* FB_NEXT is not DISP_SET's; a host stop cancels a pending flip. */
	model_write(GHCI_REG_RING_WPTR, 0U);
	ring_wptr = 0U;
	model_write(GHCI_REG_CTRL, GHCI_CTRL_RUN | GHCI_CTRL_IRQ_GLOBAL);
	offsets[0] = GHCI_DC_FB_NEXT;
	values[0] = 0x11000000U;
	dwords = ghci_cmd_disp_set(packet, 0U, 1U, offsets, values);
	push(packet, dwords);
	expect(model.display.bad_writes == 2U, "display: FB_NEXT refused to DISP_SET");
	dwords = ghci_cmd_flip(packet, 0U, 0x11400000U, GHCI_FLIP_VSYNC_WAIT | GHCI_FLIP_RECORD_ENABLE, 8U, (uint32_t)record_bus);
	push(packet, dwords);
	model_write(GHCI_REG_CTRL, GHCI_CTRL_RUN | GHCI_CTRL_IRQ_GLOBAL | GHCI_CTRL_HALT);
	model_write(block + GHCI_DC_CTRL, 0U);
	memcpy(record, model_dma(record_bus, 16U), 16U);
	expect(model_read(block + GHCI_DC_TAKEN0) == (8U | GHCI_DC_TAKEN_CANCELLED) && model_read(block + GHCI_DC_FB_BASE) == 0x10C00000U &&
	    (model_read(block + GHCI_DC_STATUS) & GHCI_DC_ST_IDLE) != 0U, "display: the stop cancels the pending flip");
}
