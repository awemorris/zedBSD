/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The VRAM allocator and the CPU's window onto the VRAM (the vram-window
 * stage).
 *
 * The allocator hands out runs of the last usable VRAM region below the
 * fixed arenas the earlier stages place at its top (the first block, the
 * display's instance memory, the graphics and compute arenas), from the top
 * down.  Those stages keep their fixed places; new users (the display's
 * surfaces, the buffer objects of the GPU device) take their VRAM here.
 *
 * The window (BAR1): the resource manager owns the window's root page table,
 * whose address the static information gives; the driver builds the lower
 * levels for the first part of the window in its own VRAM, writes the root's
 * first entry to point at them, and invalidates the hub's TLB with the root
 * named.  A buffer object is then mapped by writing its pages' entries and
 * invalidating again.  The CPU maps the window uncached.
 *
 * vram-window does not fail the boot: when the window cannot be set up the
 * later stages run as before (they reach the VRAM through the register window,
 * PRAMIN) and only the users of the window find it missing.
 *
 * On the GPU the machine boots on, the firmware's console lies at the start of
 * the window and of the VRAM (the GOP's mapping in the window's physical mode).
 * The resource manager makes the whole window virtual, so vram-window maps the
 * window's first pages back onto the console before anything else takes the
 * window (the console's size, and its mapping kept at offset 0).  The console's
 * pages are in the tables before the root's entry is switched to them, so the
 * console's writes (the boot splash turns on every kernel line) never meet an
 * unmapped window.  The CPU's own mapping of the window starts after the
 * console, so the console's memory is never mapped twice.
 *
 * Every write of page entries is read back through PRAMIN, and the self-test
 * that ends vram-window recovers in rounds: when the window does not reach its
 * run, vram-window reports what the root's entry, the run's page entry, the
 * window's mode and the MMU's fault record hold, then clears the fault record,
 * writes the tables again, binds the window to its instance block again (under
 * the root that block names, should it differ from the static information's),
 * and waits longer between the later readings.  The hub keeps its MMU state
 * over a warm reboot (the GSP stays up until power-off, the function is never
 * reset), so a record of an earlier boot's fault is shown before the tables are
 * written, for telling a later failure's cause.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/boot.h>
#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-gpu.h"

/*
 * The VRAM the fixed arenas take at the top of the region (the compute
 * arena's reserve is the deepest), with 2 MiB of slack for their alignment.
 */
#define VRAM_FIXED_RESERVE		(NVRTX_COMPUTE_ARENA_RESERVE + 0x200000ULL)

/* The allocator's top is on this boundary. */
#define VRAM_HEAP_ALIGN			0x200000ULL

/* The largest part of the window the driver's tables map: one first-level directory entry (512 MiB). */
#define VRAM_BAR1_WINDOW_MAX		0x20000000ULL

/* How many times a full window is told in the log (the first ones say why an allocation failed). */
#define VRAM_BAR1_FULL_MARKS		8U

/* The objects from this size on are cleared by the copy engine when it can (smaller ones take the CPU a few ms at most). */
#define VRAM_CLEAR_COPY_BYTES		0x40000ULL

/* The window's page tables are on a 64 KiB boundary, so one window position of PRAMIN starts them. */
#define VRAM_TABLES_ALIGN		0x10000ULL

/* Where the page tables start in the tables' block (after the root and the three directories). */
#define VRAM_TABLES_PTE_AT		0x4000ULL

/*
 * The window's mode register (Turing's BAR1 block): bit 31 set is the
 * virtual mode.  The bind's pending bits are in the status register, polled
 * when the mode is put back.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/bar/tu102.c (tu102_bar_bar1_init,
 * tu102_bar_bar1_fini, tu102_bar_bar1_wait) and NVIDIA
 * open-gpu-kernel-modules 570.144
 * src/common/inc/swref/published/turing/tu102/dev_vm.h
 * (NV_VIRTUAL_FUNCTION_PRIV_BAR1_BLOCK, NV_VIRTUAL_FUNCTION_PRIV_BIND_STATUS).
 */
#define VRAM_REG_BAR1_BLOCK		0xb80f40U
#define VRAM_BAR1_BLOCK_VIRTUAL		0x80000000U
#define VRAM_REG_BAR_STATUS		0xb80f50U
#define VRAM_BAR1_PENDING		0x00000003U
#define VRAM_BAR1_POLLS			1000U
#define VRAM_BAR1_POLL_US		10U

/*
 * The console's VRAM is reserved in whole 64 KiB.
 *
 * Value referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/nvidia/arch/nvalloc/unix/src/osinit.c (RmSetConsolePreservationParams).
 */
#define VRAM_CONSOLE_ALIGN		0x10000ULL

/* The self-test's run, and the values written each way. */
#define VRAM_TEST_BYTES			0x10000U
#define VRAM_TEST_FILL			0x5a5a1234U
#define VRAM_TEST_WORD			0xc3c3abcdU
#define VRAM_TEST_WORD_AT		20U

/* How many recovery rounds the self-test runs, and the wait of the first; each later round waits twice as long. */
#define VRAM_RETRY_ROUNDS		6U
#define VRAM_RETRY_WAIT_US		1000U

/* The rounds that do more than clear the fault record and invalidate: the tables written again, the window bound again. */
#define VRAM_RETRY_ROUND_TABLES		2U
#define VRAM_RETRY_ROUND_BIND		3U

/*
 * The MMU's record of the last fault no fault buffer took (Turing's "other"
 * fault), where a BAR's faulted access lands: the address, the instance word
 * whose low bits name the engine (4 is BAR1), and the information word (bits
 * 4:0 the fault's type, 2 an invalid page entry; bits 14:8 the client, 0x07 the
 * CPU's read and 0x08 its posted write through a BAR; bits 19:16 the access, 0
 * a read and 1 a write; bit 31 the record's validity).  The status word's bits
 * 7:0 say which window's accesses were dropped (bit 1 BAR1 in its virtual mode)
 * and bit 31 that a record waits; each clears when written as 1.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/fault/tu102.c (tu102_fault_info_fault)
 * and NVIDIA open-gpu-doc manuals/turing/tu104/dev_vm.ref.txt
 * (NV_VIRTUAL_FUNCTION_PRIV_MMU_FAULT_INST_LO_ENGINE_ID,
 * NV_VIRTUAL_FUNCTION_PRIV_MMU_FAULT_INFO,
 * NV_VIRTUAL_FUNCTION_PRIV_MMU_FAULT_STATUS).
 */
#define VRAM_REG_FAULT_ADDR_LO		0xb83080U
#define VRAM_REG_FAULT_ADDR_HI		0xb83084U
#define VRAM_REG_FAULT_INST_LO		0xb83088U
#define VRAM_REG_FAULT_INFO		0xb83090U
#define VRAM_REG_FAULT_STATUS		0xb83094U
#define VRAM_FAULT_STATUS_CLEAR		0x800000ffU
#define VRAM_FAULT_ENGINE_MASK		0x1ffU
#define VRAM_FAULT_TYPE_MASK		0x1fU
#define VRAM_FAULT_CLIENT_SHIFT		8U
#define VRAM_FAULT_CLIENT_MASK		0x7fU
#define VRAM_FAULT_ACCESS_SHIFT		16U
#define VRAM_FAULT_ACCESS_MASK		0xfU

/*
 * The window's mode register names the instance block the window is
 * bound to: its page number in bits 27:0 and its target in bits 29:28 (0
 * the VRAM).  The block's field at 0x200 names the root, with the tables'
 * format in its low bits.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/inc/swref/published/turing/tu102/dev_vm.h
 * (NV_VIRTUAL_FUNCTION_PRIV_BAR1_BLOCK_PTR,
 * NV_VIRTUAL_FUNCTION_PRIV_BAR1_BLOCK_TARGET) and NVIDIA open-gpu-doc
 * manuals/turing/tu104/dev_ram.ref.txt (NV_RAMIN_PAGE_DIR_BASE_LO).
 */
#define VRAM_BAR1_BLOCK_PTR_MASK	0x0fffffffU
#define VRAM_BAR1_BLOCK_TARGET_MASK	0x30000000U
#define VRAM_INSTANCE_ROOT_AT		0x200U
#define VRAM_INSTANCE_ROOT_MASK		(~0xfffULL)

/*
 * What one self-test of the window read: the run's first and last words
 * through the window, and the word written through the window as PRAMIN
 * reads it back.  Lives on the self-test's stack.
 */
struct vram_probe {
	uint32_t first;
	uint32_t last;
	uint32_t back;
};

static int vram_bar1_tables(struct nvrtx_gpu *gpu);
static int vram_bar1_write_tables(struct nvrtx_gpu *gpu);
static int vram_console_map(struct nvrtx_gpu *gpu);
static int vram_bar1_cpu(struct nvrtx_gpu *gpu);
static int vram_bar1_test(struct nvrtx_gpu *gpu);
static int vram_bar1_probe(struct nvrtx_gpu *gpu, const struct nvrtx_bo *bo, struct vram_probe *probe);
static void vram_bar1_report(struct nvrtx_gpu *gpu, const struct nvrtx_bo *bo, const struct vram_probe *probe);
static void vram_fault_show(struct nvrtx_gpu *gpu, const char *when);
static int vram_bar1_recover(struct nvrtx_gpu *gpu, const struct nvrtx_bo *bo, struct vram_probe *probe, const char **how);
static int vram_bar1_revive(struct nvrtx_gpu *gpu, const struct nvrtx_bo *bo, unsigned round, const char **how);
static int vram_bar1_rebind(struct nvrtx_gpu *gpu, const struct nvrtx_bo *bo);
static int vram_bar1_repair(struct nvrtx_gpu *gpu, const struct nvrtx_bo *bo);
static int vram_write_ptes(struct nvrtx_gpu *gpu, uint64_t bar1, uint64_t vram, uint64_t size, int clear);
static int vram_place_ptes(struct nvrtx_gpu *gpu, uint64_t at, const uint8_t *entries, uint64_t pages);
static int vram_read64(struct nvrtx_gpu *gpu, uint64_t address, uint64_t *value);

/*
 * Runs the vram-window stage: the VRAM allocator, then the window and its
 * self-test.
 *
 * Returns 0 even when the window could not be set up (the stage's marks say
 * why); an error only when the allocator itself cannot be placed.
 */
int
nvrtx_memory_start(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_memory_state *memory;
	uint64_t base;
	uint64_t top;
	uint64_t heap_top;
	uint32_t region;
	int error;

	memory = &gpu->memory_state;

	/* Finds the last usable region; the fixed arenas take its top. */
	nvrtx_stage_mark("vram-window begin");
	error = nvrtx_mmu_vram_region(gpu->static_info, &base, &top, &region);
	if (error != 0) {
		nvrtx_stage_mark("vram-window fail: no usable VRAM region (%d)", error);
		return error;
	}

	/* A kept console holds the VRAM's start; the allocator stays above it. */
	if (gpu->console.kept != 0 && base < gpu->console.size)
		base = gpu->console.size;

	/* The allocator covers the rest of the region, below the fixed arenas. */
	if (top <= base || top - base <= VRAM_FIXED_RESERVE) {
		nvrtx_stage_mark("vram-window fail: region %u is too small (%llx..%llx)", (unsigned)region, (unsigned long long)base, (unsigned long long)top);
		return ENOSPC;
	}

	/* Starts the allocator over the region below them. */
	heap_top = (top - VRAM_FIXED_RESERVE) & ~(VRAM_HEAP_ALIGN - 1U);
	mutex_lock(&memory->lock);

	nvrtx_heap_init(&memory->vram, base, heap_top);
	memory->ready = 1;

	mutex_unlock(&memory->lock);

	/* Shows where the allocator lies. */
	nvrtx_stage_mark("vram-window vram: region %u, heap %llx..%llx (%llu MiB)",
	    (unsigned)region,
	    (unsigned long long)base,
	    (unsigned long long)heap_top,
	    (unsigned long long)((heap_top - base) >> 20));

	/* Builds the window's tables under the resource manager's root. */
	error = vram_bar1_tables(gpu);
	if (error != 0) {
		nvrtx_stage_mark("vram-window: no bar1 window (%d); the later stages use PRAMIN", error);
		return 0;
	}

	/* Records the console's place in the window, mapped with the tables; the caller decides what a failure means. */
	error = vram_console_map(gpu);
	if (error != 0)
		return 0;

	/* Maps the window for the CPU. */
	error = vram_bar1_cpu(gpu);
	if (error != 0) {
		nvrtx_stage_mark("vram-window: the CPU cannot map bar1 (%d)", error);
		return 0;
	}

	/* Checks the window both ways before anyone relies on it. */
	error = vram_bar1_test(gpu);
	if (error != 0) {
		mutex_lock(&memory->lock);

		memory->bar1_ready = 0;

		mutex_unlock(&memory->lock);

		return 0;
	}

	/* Succeeded: the allocator and the window are ready. */
	return 0;
}

/*
 * Gives back the CPU's mapping of the window and its claim.
 *
 * The VRAM itself needs no release: the GSP's unload and the end of the
 * resource manager leave it unused.
 */
void
nvrtx_memory_release(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_memory_state *memory;

	memory = &gpu->memory_state;

	/* Unmaps the window. */
	if (memory->bar1_mapped != 0) {
		drv_pci_device_unmap_bar(gpu->identity.pci, &memory->bar1_mapping);
		memory->bar1_mapped = 0;
	}

	/* Releases its claim. */
	if (memory->bar1_claimed != 0) {
		drv_pci_device_release_bar(gpu->identity.pci, gpu->identity.windows[1].bar);
		memory->bar1_claimed = 0;
	}

	/* Nobody may take VRAM or map it any more. */
	memory->bar1_ready = 0;
	memory->ready = 0;
}

/*
 * Takes a run of VRAM of whole pages for a buffer object, unmapped.
 *
 * align is a power of two of at least a page (smaller is raised to a page).
 * Returns ENODEV before vram-window, or the allocator's ENOSPC.
 */
int
nvrtx_bo_alloc(
	struct nvrtx_gpu *gpu,
	uint64_t size,
	uint64_t align,
	struct nvrtx_bo *bo)
{
	struct nvrtx_memory_state *memory;
	uint64_t address;
	int error;

	memory = &gpu->memory_state;

	/* Whole pages, on at least a page boundary. */
	if (size == 0U)
		return EINVAL;
	size = (size + NVRTX_GSP_PAGE_SIZE - 1U) & ~((uint64_t)NVRTX_GSP_PAGE_SIZE - 1U);
	if (align < NVRTX_GSP_PAGE_SIZE)
		align = NVRTX_GSP_PAGE_SIZE;

	/* Takes the run from the allocator. */
	mutex_lock(&memory->lock);

	error = ENODEV;
	if (memory->ready != 0)
		error = nvrtx_heap_alloc(&memory->vram, size, align, &address);

	mutex_unlock(&memory->lock);

	/* Reports a missing allocator or a full one (a full one is told, the first few times). */
	if (error == ENOSPC && memory->vram_full_marks < VRAM_BAR1_FULL_MARKS) {
		memory->vram_full_marks++;
		nvrtx_stage_mark("vram full: %llu KiB asked, %llu KiB free in %u runs",
		    (unsigned long long)(size >> 10),
		    (unsigned long long)(memory->vram.free_bytes >> 10),
		    (unsigned)memory->vram.count);
	}
	if (error != 0)
		return error;

	/* Succeeded: the run is the object's, not yet in the window. */
	bo->vram = address;
	bo->size = size;
	bo->bar1 = 0U;
	bo->bar1_mapped = 0;
	return 0;
}

/*
 * Maps a buffer object into the CPU's window.
 *
 * Two callers may map the same object at once (an object is mapped when the
 * CPU first needs it), so the check and the mapping are under the lock.
 * Returns ENODEV without a working window, or ENOSPC when the window has
 * no room.
 */
int
nvrtx_bo_map(
	struct nvrtx_gpu *gpu,
	struct nvrtx_bo *bo)
{
	struct nvrtx_memory_state *memory;
	uint64_t address;
	uint64_t used;
	int full;
	int error;

	memory = &gpu->memory_state;

	/* Takes a run of the window and points its pages at the object's VRAM; a mapped object stays where it is. */
	mutex_lock(&memory->lock);

	if (bo->bar1_mapped != 0) {
		mutex_unlock(&memory->lock);
		return 0;
	}

	error = ENODEV;
	if (memory->bar1_ready != 0)
		error = nvrtx_heap_alloc(&memory->bar1, bo->size, NVRTX_GSP_PAGE_SIZE, &address);
	if (error == 0) {
		error = vram_write_ptes(gpu, address, bo->vram, bo->size, 0);
		if (error != 0)
			(void)nvrtx_heap_free(&memory->bar1, address, bo->size);
	}
	if (error == 0) {
		bo->bar1 = address;
		bo->bar1_mapped = 1;
	}

	/* The window's use, its highest, and the first few times it was full. */
	used = (memory->bar1.top - memory->bar1.base) - memory->bar1.free_bytes;
	if (used > memory->bar1_peak)
		memory->bar1_peak = used;
	full = 0;
	if (error == ENOSPC && memory->bar1_full_marks < VRAM_BAR1_FULL_MARKS) {
		memory->bar1_full_marks++;
		full = 1;
	}

	mutex_unlock(&memory->lock);

	if (full != 0)
		nvrtx_stage_mark("window full: %llu KiB asked, %llu of %llu KiB used",
		    (unsigned long long)(bo->size >> 10),
		    (unsigned long long)(used >> 10),
		    (unsigned long long)((memory->bar1.top - memory->bar1.base) >> 10));

	/* Reports a missing window, a full one or a write that failed. */
	if (error != 0)
		return error;

	/* Succeeded: the CPU reaches the object at its place in the window. */
	return 0;
}

/*
 * Clears a buffer object's VRAM.  A large one goes to the copy engine when
 * the driver's work channel can take it (the CPU's windows write about
 * 32 MB/s, 250 ms for one 1920x1080 picture); otherwise the CPU writes it,
 * through the window when the object is mapped, else through the register
 * window (PRAMIN), so an object the CPU never reads takes no part of the
 * window.
 *
 * Returns 0, the error of the register window, or EBUSY when the copy
 * engine's fill did not end: the GPU may still write the object's VRAM,
 * which must then never be given back.
 */
int
nvrtx_bo_clear(
	struct nvrtx_gpu *gpu,
	struct nvrtx_bo *bo)
{
	volatile uint8_t *cpu;
	uint64_t at;
	int error;

	/* A large object by the copy engine, when it can; a fill that did not end is told. */
	if (bo->size >= VRAM_CLEAR_COPY_BYTES) {
		error = nvrtx_work_clear(gpu, bo->vram, bo->size);
		if (error == 0 || error == EBUSY)
			return error;
	}

	/* An unmapped object: the register window in 64 KiB pieces. */
	if (bo->bar1_mapped == 0)
		return nvrtx_vram_fill(gpu, bo->vram, 0U, (size_t)bo->size);

	/* A mapped one: word by word through the uncached window. */
	cpu = nvrtx_bo_cpu(gpu, bo);
	for (at = 0U; at < bo->size; at += 4U)
		*(volatile uint32_t *)(volatile void *)(cpu + at) = 0U;

	/* Succeeded: the object reads as zero. */
	return 0;
}

/*
 * Reports the window's bytes in use now, the most it has held, and its size
 * (the part after the console), all 0 before the window is set up.
 */
void
nvrtx_bo_window_use(
	struct nvrtx_gpu *gpu,
	uint64_t *used,
	uint64_t *peak,
	uint64_t *size)
{
	struct nvrtx_memory_state *memory;

	memory = &gpu->memory_state;

	/* Read under the lock, so the three agree. */
	mutex_lock(&memory->lock);

	*used = 0U;
	*peak = 0U;
	*size = 0U;
	if (memory->bar1_ready != 0) {
		*size = memory->bar1.top - memory->bar1.base;
		*used = *size - memory->bar1.free_bytes;
		*peak = memory->bar1_peak;
	}

	mutex_unlock(&memory->lock);
}

/*
 * Frees a buffer object: unmaps it from the window, then gives its VRAM back.
 *
 * The caller has made sure the GPU no longer reads it.  A page entry that
 * cannot be cleared keeps the window's run taken, so it is never reused.
 */
void
nvrtx_bo_free(
	struct nvrtx_gpu *gpu,
	struct nvrtx_bo *bo)
{
	struct nvrtx_memory_state *memory;
	int error;

	memory = &gpu->memory_state;

	/* An object never allocated has nothing to give back. */
	if (bo->size == 0U)
		return;

	/* Clears the window's entries and returns both runs. */
	mutex_lock(&memory->lock);

	if (bo->bar1_mapped != 0) {
		error = vram_write_ptes(gpu, bo->bar1, bo->vram, bo->size, 1);
		if (error == 0)
			(void)nvrtx_heap_free(&memory->bar1, bo->bar1, bo->size);
	}

	if (memory->ready != 0)
		(void)nvrtx_heap_free(&memory->vram, bo->vram, bo->size);

	mutex_unlock(&memory->lock);

	/* Forgets the object, so a second free finds nothing. */
	bo->vram = 0U;
	bo->size = 0U;
	bo->bar1 = 0U;
	bo->bar1_mapped = 0;
}

/*
 * Takes a buffer object out of the CPU's window, leaving its VRAM as it is
 * (the object names a part of a run another object owns).
 */
void
nvrtx_bo_unmap(
	struct nvrtx_gpu *gpu,
	struct nvrtx_bo *bo)
{
	struct nvrtx_memory_state *memory;
	int error;

	memory = &gpu->memory_state;

	/* Clears the window's entries and gives the window's run back. */
	mutex_lock(&memory->lock);

	if (bo->bar1_mapped != 0) {
		error = vram_write_ptes(gpu, bo->bar1, bo->vram, bo->size, 1);
		if (error == 0)
			(void)nvrtx_heap_free(&memory->bar1, bo->bar1, bo->size);
	}

	mutex_unlock(&memory->lock);

	/* Succeeded: the object is not in the window. */
	bo->bar1 = 0U;
	bo->bar1_mapped = 0;
}

/*
 * Reports where the CPU reads and writes a mapped buffer object.
 */
volatile uint8_t *
nvrtx_bo_cpu(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_bo *bo)
{
	volatile uint8_t *window;

	/* The object's place in the uncached mapping of the window, which starts after the console. */
	window = (volatile uint8_t *)gpu->memory_state.bar1_mapping.address;

	/* Succeeded: the object's first byte. */
	return window + (bo->bar1 - gpu->memory_state.bar1_cpu_from);
}

/*
 * Reports the CPU physical address of a mapped buffer object, for a user mapping.
 */
uint64_t
nvrtx_bo_bus(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_bo *bo)
{
	uint64_t physical;

	/* The object's place in the window's physical range. */
	physical = gpu->memory_state.bar1_mapping.physical_address + (bo->bar1 - gpu->memory_state.bar1_cpu_from);

	/* Succeeded: the address. */
	return physical;
}

/*
 * Decides, before the GSP boot (layout), whether the firmware's console on this
 * GPU is kept through it.
 *
 * The console is kept when the loader's picture starts the VRAM window, the
 * window is in its physical mode (so the picture is at the VRAM's start
 * too) and the picture is in the 32-bit format the display shows (B, G, R,
 * X in memory).  Returns 0 when the boot may go on (the console is kept, or
 * the GPU has none: the attach kept it only for nvrtx.gsp=1), EBUSY when
 * the console is on the GPU but cannot be kept.
 */
int
nvrtx_console_check(
	struct nvrtx_gpu *gpu)
{
	const struct nvrtx_device *identity;
	struct nvrtx_console *console;
	uint64_t bytes;
	uint32_t block;

	identity = &gpu->identity;
	console = &gpu->console;

	/* A GPU the firmware's picture is not on has no console to keep. */
	if (identity->gop_on_gpu == 0)
		return 0;

	/* Reads the window's mode as the firmware left it. */
	block = nvrtx_rd32(gpu, VRAM_REG_BAR1_BLOCK);
	console->bar1_block = block;

	/* Refuses a picture away from the window's start, which the resource manager does not keep. */
	if (identity->gop_offset != 0U) {
		nvrtx_stage_mark("layout console at bar1 +%llx, not its start: cannot be kept", (unsigned long long)identity->gop_offset);
		return EBUSY;
	}

	/* Refuses a window the firmware put in its virtual mode: the picture's VRAM is unknown. */
	if ((block & VRAM_BAR1_BLOCK_VIRTUAL) != 0U) {
		nvrtx_stage_mark("layout console: bar1 is virtual (%08x): cannot be kept", (unsigned)block);
		return EBUSY;
	}

	/* Refuses a format the display's window is not given and a picture of no size. */
	if (identity->gop_format != KERN_BOOT_FRAMEBUFFER_BGRX8888 ||
	    identity->gop_width == 0U ||
	    identity->gop_height == 0U ||
	    identity->gop_stride < identity->gop_width) {
		nvrtx_stage_mark("layout console %ux%u stride %u format %u: cannot be kept",
		    (unsigned)identity->gop_width,
		    (unsigned)identity->gop_height,
		    (unsigned)identity->gop_stride,
		    (unsigned)identity->gop_format);
		return EBUSY;
	}

	/* Describes the console and the VRAM it holds, in whole 64 KiB. */
	bytes = (uint64_t)identity->gop_stride * 4U * identity->gop_height;
	console->size = (bytes + VRAM_CONSOLE_ALIGN - 1U) & ~(VRAM_CONSOLE_ALIGN - 1U);
	console->width = identity->gop_width;
	console->height = identity->gop_height;
	console->pitch = identity->gop_stride * 4U;
	console->kept = 1;

	/* Succeeded: the boot keeps the console. */
	nvrtx_stage_mark("layout console %ux%u pitch %u kept: %llu KiB at the VRAM's start",
	    (unsigned)console->width,
	    (unsigned)console->height,
	    (unsigned)console->pitch,
	    (unsigned long long)(console->size >> 10));
	return 0;
}

/*
 * Puts the window back in the mode the firmware left it in, once the GSP
 * has stopped, so the console reaches its VRAM again without the resource
 * manager's tables.  Nothing is done for a console not kept or a mode that
 * did not change.
 */
void
nvrtx_console_restore(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_console *console;
	uint32_t block;
	uint32_t restored;
	uint32_t status;
	uint32_t poll;

	console = &gpu->console;

	/* A console not kept has nothing to restore. */
	if (console->kept == 0)
		return;

	/* A window still in the firmware's mode needs nothing. */
	block = nvrtx_rd32(gpu, VRAM_REG_BAR1_BLOCK);
	if (block == console->bar1_block)
		return;

	/* Writes the firmware's mode and waits for the bind to settle. */
	nvrtx_wr32(gpu, VRAM_REG_BAR1_BLOCK, console->bar1_block);
	status = 0U;
	for (poll = 0U; poll < VRAM_BAR1_POLLS; poll++) {
		status = nvrtx_rd32(gpu, VRAM_REG_BAR_STATUS);
		if ((status & VRAM_BAR1_PENDING) == 0U)
			break;

		kern_usleep_range(VRAM_BAR1_POLL_US, VRAM_BAR1_POLL_US);
	}

	/* Says what the window was put back to; the resource manager's mapping of the console is gone. */
	restored = nvrtx_rd32(gpu, VRAM_REG_BAR1_BLOCK);
	console->mapped = 0;
	nvrtx_stage_mark("unload console: bar1 mode %08x again (was %08x, status %08x)",
	    (unsigned)restored,
	    (unsigned)block,
	    (unsigned)status);
}

/*
 * Builds the window's directories and page tables in VRAM of the allocator
 * and points the resource manager's root at them.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/bar.c (r535_bar_bar1_init,
 * r535_bar_flush: the window's root from the static information, the lower
 * levels and the flush).
 */
static int
vram_bar1_tables(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_memory_state *memory;
	const struct nvrtx_window *window;
	uint64_t tables;
	size_t length;
	uint32_t status;
	uint32_t info;
	int error;

	memory = &gpu->memory_state;
	window = &gpu->identity.windows[1];

	/* The window must exist and hold at least one page table's reach. */
	if (window->present == 0 || window->size < NVRTX_MMU_RANGE_BYTES)
		return ENODEV;

	/* The resource manager's root must be a page of the VRAM. */
	memory->bar1_root = nvrtx_get64(gpu->static_info, NVRTX_STATIC_BAR1_ROOT_AT);
	if (memory->bar1_root == 0U || (memory->bar1_root & (NVRTX_GSP_PAGE_SIZE - 1U)) != 0U)
		return EINVAL;

	/* The tables map the window's first 512 MiB at most, in whole page tables. */
	memory->bar1_window = window->size;
	if (memory->bar1_window > VRAM_BAR1_WINDOW_MAX)
		memory->bar1_window = VRAM_BAR1_WINDOW_MAX;
	memory->bar1_window &= ~(NVRTX_MMU_RANGE_BYTES - 1U);

	/* Takes the tables' VRAM from the allocator. */
	length = nvrtx_mmu_gr_tables_bytes(memory->bar1_window);
	mutex_lock(&memory->lock);

	error = nvrtx_heap_alloc(&memory->vram, length, VRAM_TABLES_ALIGN, &tables);

	mutex_unlock(&memory->lock);

	/* Reports an allocator without room for them. */
	if (error != 0)
		return error;

	memory->bar1_tables = tables;

	/* Reads the root's first entry as the resource manager left it, to show it. */
	error = vram_read64(gpu, memory->bar1_root, &memory->bar1_root_entry);
	if (error != 0)
		return error;

	nvrtx_stage_mark("vram-window bar1: root %llx entry %llx, window %llu MiB, tables at %llx",
	    (unsigned long long)memory->bar1_root,
	    (unsigned long long)memory->bar1_root_entry,
	    (unsigned long long)(memory->bar1_window >> 20),
	    (unsigned long long)tables);

	/* Shows a fault record the MMU still holds (one of an earlier boot survives a warm reboot), to tell a later failure's cause from. */
	status = nvrtx_rd32(gpu, VRAM_REG_FAULT_STATUS);
	info = nvrtx_rd32(gpu, VRAM_REG_FAULT_INFO);
	if ((status & VRAM_FAULT_STATUS_CLEAR) != 0U || info != 0U)
		vram_fault_show(gpu, "bar1 before the tables");

	/* Writes the tables and switches the root's entry to them. */
	error = vram_bar1_write_tables(gpu);
	if (error != 0)
		return error;

	/* Succeeded: the window's tables are in place, every page unmapped but the console's. */
	return 0;
}

/*
 * Writes the window's tables over the driver's VRAM and points the
 * resource manager's root at them: every page unmapped but a kept
 * console's, which map the VRAM's start page for page.
 *
 * The console's entries are in the tables before the root's entry changes,
 * so no write of the console meets an unmapped window.  A run mapped in
 * the window before is unmapped by it.  The root's entry is read back, so
 * a write that went elsewhere is not taken for a window.
 */
static int
vram_bar1_write_tables(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_memory_state *memory;
	struct nvrtx_console *console;
	uint8_t entry[8];
	uint8_t *image;
	uint64_t found;
	size_t length;
	int error;

	memory = &gpu->memory_state;
	console = &gpu->console;
	length = nvrtx_mmu_gr_tables_bytes(memory->bar1_window);

	/* Allocates the tables' image in memory. */
	image = kern_malloc(length);
	if (image == NULL)
		return ENOMEM;

	/* Builds the tables: every page entry invalid. */
	error = nvrtx_mmu_gr_tables(image, memory->bar1_tables, 0U, memory->bar1_window);

	/* Maps a kept console that fits the window onto the VRAM's start (vram_console_map() refuses one that does not). */
	if (error == 0 &&
	    console->kept != 0 &&
	    console->size < memory->bar1_window)
		error = nvrtx_mmu_gr_map_vram(image, 0U, memory->bar1_window, 0U, 0U, console->size);

	/* Writes the tables into the VRAM. */
	if (error == 0)
		error = nvrtx_vram_write(gpu, memory->bar1_tables, image, length);

	/* Keeps the root's first entry, as the builder made it for its own copy of the root. */
	kern_memcpy(entry, image, sizeof(entry));
	kern_free(image);
	if (error != 0)
		return error;

	/* Remembers the entry the root holds from now on, for the self-test's report. */
	memory->bar1_root_ours = nvrtx_get64(entry, 0U);

	/* Points the resource manager's root at the driver's second-level directory. */
	error = nvrtx_vram_write(gpu, memory->bar1_root, entry, sizeof(entry));
	if (error != 0)
		return error;

	/* Reads the root's entry back through PRAMIN. */
	error = vram_read64(gpu, memory->bar1_root, &found);
	if (error != 0)
		return error;

	/* Refuses a root whose entry did not take the driver's directories. */
	if (found != memory->bar1_root_ours) {
		nvrtx_stage_mark("vram-window: the root's entry reads %llx, %llx written", (unsigned long long)found, (unsigned long long)memory->bar1_root_ours);
		return EIO;
	}

	/* Drops any stale translation of the window. */
	error = nvrtx_flush_tlb_bar(gpu, memory->bar1_root);
	if (error != 0)
		return error;

	/* Succeeded: the root points at the tables, the hub's TLB holds nothing older. */
	return 0;
}

/*
 * Records that the window's first pages point back at the console the
 * firmware left at the VRAM's start, page for page: the tables were
 * written with them, before anything else takes the window.
 *
 * Procedure referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/nvidia/arch/nvalloc/unix/src/osinit.c
 * (RmSetConsolePreservationParams, the console's size) and
 * src/nvidia/src/kernel/gpu/bus/arch/maxwell/kern_bus_gm107.c
 * (kbusInitBar1_GM107, its mapping kept at offset 0).
 */
static int
vram_console_map(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_memory_state *memory;
	struct nvrtx_console *console;

	memory = &gpu->memory_state;
	console = &gpu->console;

	/* A GPU without a kept console has nothing to map back. */
	if (console->kept == 0)
		return 0;

	/* The console must lie in the part of the window the tables cover; the tables left it out otherwise. */
	if (console->size >= memory->bar1_window) {
		nvrtx_stage_mark("vram-window fail: the console's %llu KiB do not fit the window", (unsigned long long)(console->size >> 10));
		return ENOSPC;
	}

	/* Succeeded: the console's writes reach its VRAM again. */
	console->mapped = 1;
	nvrtx_stage_mark("vram-window console: bar1 0..%llx on the VRAM's start again", (unsigned long long)console->size);
	return 0;
}

/*
 * Claims the window and maps it uncached for the CPU, after the console's
 * part; the allocator of the window starts empty after the console.
 */
static int
vram_bar1_cpu(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_memory_state *memory;
	uint64_t from;
	unsigned bar;
	int error;

	memory = &gpu->memory_state;
	bar = gpu->identity.windows[1].bar;

	/* The console's part of the window stays the console's. */
	from = 0U;
	if (gpu->console.mapped != 0)
		from = gpu->console.size;

	/* Claims the window's BAR. */
	error = drv_pci_device_claim_bar(gpu->identity.pci, bar);
	if (error != 0)
		return error;

	memory->bar1_claimed = 1;

	/* Maps the part the tables cover. */
	error = drv_pci_device_map_bar_region(gpu->identity.pci,
	    bar,
	    from,
	    (size_t)(memory->bar1_window - from),
	    DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE | DRV_PCI_MAP_NOCACHE,
	    &memory->bar1_mapping);
	if (error != 0)
		return error;

	memory->bar1_mapped = 1;
	memory->bar1_cpu_from = from;
	memory->bar1_bus = memory->bar1_mapping.physical_address;

	/* The window's runs are handed out from now on. */
	mutex_lock(&memory->lock);

	nvrtx_heap_init(&memory->bar1, from, memory->bar1_window);
	memory->bar1_ready = 1;

	mutex_unlock(&memory->lock);

	/* Succeeded: the CPU reaches the window. */
	return 0;
}

/*
 * Checks the window both ways on a run of its own: words written through
 * PRAMIN read back through the window, and a word written through the window
 * reads back through PRAMIN.  A window that does not reach the run is
 * reported and recovered in rounds, the run read again after each.
 */
static int
vram_bar1_test(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_bo bo;
	struct vram_probe probe;
	const char *how;
	int error;

	/* Takes a run and maps it. */
	kern_memset(&bo, 0, sizeof(bo));
	error = nvrtx_bo_alloc(gpu, VRAM_TEST_BYTES, 0U, &bo);
	if (error == 0)
		error = nvrtx_bo_map(gpu, &bo);
	if (error != 0) {
		nvrtx_stage_mark("vram-window fail: the test run (%d)", error);
		nvrtx_bo_free(gpu, &bo);
		return error;
	}

	/* Reads the run both ways. */
	how = NULL;
	error = vram_bar1_probe(gpu, &bo, &probe);

	/* Reports a window that does not reach the run, then recovers it in rounds. */
	if (error == EIO) {
		vram_bar1_report(gpu, &bo, &probe);
		error = vram_bar1_recover(gpu, &bo, &probe, &how);
	}

	/* The run goes back either way. */
	nvrtx_bo_free(gpu, &bo);

	/* Reports a window that does not reach the VRAM. */
	if (error != 0) {
		nvrtx_stage_mark("vram-window fail: bar1 reads %08x %08x, PRAMIN reads %08x (%d)", (unsigned)probe.first, (unsigned)probe.last, (unsigned)probe.back, error);
		return EIO;
	}

	/* Says that the window works only since a recovery round. */
	if (how != NULL)
		nvrtx_stage_mark("vram-window retry: the window reads back after %s", how);

	/* Succeeded: the window reaches the VRAM both ways. */
	nvrtx_stage_mark("vram-window ok: bar1 %llu MiB at %llx, read back both ways",
	    (unsigned long long)(gpu->memory_state.bar1_window >> 20),
	    (unsigned long long)gpu->memory_state.bar1_bus);
	return 0;
}

/*
 * Reads a mapped run both ways: EIO when the window does not reach the
 * run's VRAM, another error when PRAMIN refuses.
 */
static int
vram_bar1_probe(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_bo *bo,
	struct vram_probe *probe)
{
	volatile uint32_t *words;
	int error;

	/* Starts with nothing read. */
	probe->first = 0U;
	probe->last = 0U;
	probe->back = 0U;

	/* Fills the run through PRAMIN. */
	error = nvrtx_vram_fill(gpu, bo->vram, VRAM_TEST_FILL, VRAM_TEST_BYTES);
	if (error != 0)
		return error;

	/* Reads its first and last words through the window. */
	words = (volatile uint32_t *)(volatile void *)nvrtx_bo_cpu(gpu, bo);
	probe->first = words[0];
	probe->last = words[VRAM_TEST_BYTES / 4U - 1U];

	/* Writes one word through the window (the read makes sure it left the CPU) and reads it back through PRAMIN. */
	words[VRAM_TEST_WORD_AT] = VRAM_TEST_WORD;
	(void)words[VRAM_TEST_WORD_AT];
	error = nvrtx_vram_read32(gpu, bo->vram + VRAM_TEST_WORD_AT * 4U, &probe->back);
	if (error != 0)
		return error;

	/* Refuses a window whose reads did not come from the run. */
	if (probe->first != VRAM_TEST_FILL)
		return EIO;
	if (probe->last != VRAM_TEST_FILL)
		return EIO;

	/* Refuses a window whose write did not reach the run. */
	if (probe->back != VRAM_TEST_WORD)
		return EIO;

	/* Succeeded: the window reaches the run both ways. */
	return 0;
}

/*
 * Reports a window that did not reach the self-test's run, with what the
 * hardware holds now; each names a different cause: the root's entry taken
 * back from the driver's directories, a page entry that did not land, the
 * window's mode changed, or the access faulted in the MMU.
 */
static void
vram_bar1_report(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_bo *bo,
	const struct vram_probe *probe)
{
	struct nvrtx_memory_state *memory;
	uint64_t root;
	uint64_t entry;
	uint64_t wanted;
	uint32_t block;
	int error;

	memory = &gpu->memory_state;
	wanted = nvrtx_mmu_vram_pte(bo->vram);

	/* Reads the root's first entry and the run's first page entry back through PRAMIN. */
	root = 0U;
	entry = 0U;
	error = vram_read64(gpu, memory->bar1_root, &root);
	if (error == 0)
		error = vram_read64(gpu, memory->bar1_tables + VRAM_TABLES_PTE_AT + (bo->bar1 / NVRTX_GSP_PAGE_SIZE) * 8U, &entry);

	/* Reads the window's mode. */
	block = nvrtx_rd32(gpu, VRAM_REG_BAR1_BLOCK);

	/* Says what was found, before anything is changed. */
	nvrtx_stage_mark("vram-window retry: bar1 reads %08x %08x, PRAMIN reads %08x", (unsigned)probe->first, (unsigned)probe->last, (unsigned)probe->back);
	nvrtx_stage_mark("vram-window retry: root entry %llx, ours %llx (%d)", (unsigned long long)root, (unsigned long long)memory->bar1_root_ours, error);
	nvrtx_stage_mark("vram-window retry: page entry %llx, wants %llx, mode %08x", (unsigned long long)entry, (unsigned long long)wanted, (unsigned)block);
	vram_fault_show(gpu, "retry");
}

/*
 * Shows the MMU's record of the last unbuffered fault and the fault status,
 * decoded: which engine's access it was (4 is this window), the fault's
 * type (2 an invalid page entry), the client (the CPU's read 07 or posted
 * write 08) and the access (0 read, 1 write).  Nothing is acknowledged.
 */
static void
vram_fault_show(
	struct nvrtx_gpu *gpu,
	const char *when)
{
	uint32_t status;
	uint32_t low;
	uint32_t high;
	uint32_t instance;
	uint32_t info;

	/* Reads the status and the record. */
	status = nvrtx_rd32(gpu, VRAM_REG_FAULT_STATUS);
	low = nvrtx_rd32(gpu, VRAM_REG_FAULT_ADDR_LO);
	high = nvrtx_rd32(gpu, VRAM_REG_FAULT_ADDR_HI);
	instance = nvrtx_rd32(gpu, VRAM_REG_FAULT_INST_LO);
	info = nvrtx_rd32(gpu, VRAM_REG_FAULT_INFO);

	/* Says what the MMU holds, raw and decoded, in lines short enough for the console and dmesg (80 columns). */
	nvrtx_stage_mark("vram-window %s: fault status %08x, record %08x",
	    when,
	    (unsigned)status,
	    (unsigned)info);
	nvrtx_stage_mark("vram-window %s: fault at %08x%08x, engine %x",
	    when,
	    (unsigned)high,
	    (unsigned)low,
	    (unsigned)(instance & VRAM_FAULT_ENGINE_MASK));
	nvrtx_stage_mark("vram-window %s: fault type %u client %02x access %u",
	    when,
	    (unsigned)(info & VRAM_FAULT_TYPE_MASK),
	    (unsigned)((info >> VRAM_FAULT_CLIENT_SHIFT) & VRAM_FAULT_CLIENT_MASK),
	    (unsigned)((info >> VRAM_FAULT_ACCESS_SHIFT) & VRAM_FAULT_ACCESS_MASK));
}

/*
 * Recovers a window that did not reach the self-test's run, in rounds:
 * each clears the fault record and invalidates the hub's TLB, the second
 * writes the tables again first, the third binds the window to its
 * instance block again, and every round waits twice as long as the one
 * before (1 ms to 32 ms) before the run is read again.  On success *how
 * names the round that worked; EIO reports a window that never read back,
 * another error one the recovery could not touch.
 */
static int
vram_bar1_recover(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_bo *bo,
	struct vram_probe *probe,
	const char **how)
{
	unsigned round;
	unsigned wait;
	int error;

	/* Tries each round until the run reads back. */
	for (round = 1U; round <= VRAM_RETRY_ROUNDS; round++) {
		/* Does the round's recovery. */
		error = vram_bar1_revive(gpu, bo, round, how);
		if (error != 0)
			return error;

		/* Gives the hub the round's moment before the reading. */
		wait = VRAM_RETRY_WAIT_US << (round - 1U);
		kern_usleep_range(wait, wait);

		/* Reads the run both ways; a run that reads back ends the recovery. */
		error = vram_bar1_probe(gpu, bo, probe);
		if (error == 0)
			return 0;
		if (error != EIO)
			return error;

		/* Says what this round read, so the log tells which rounds did nothing. */
		nvrtx_stage_mark("vram-window retry %u (%s): bar1 reads %08x %08x, PRAMIN reads %08x",
		    round,
		    *how,
		    (unsigned)probe->first,
		    (unsigned)probe->last,
		    (unsigned)probe->back);
	}

	/* The window never read back. */
	return EIO;
}

/*
 * Does one recovery round's work before the run is read again, and names
 * it in *how.  Every round clears the fault record and invalidates the
 * hub's TLB for the window's root; the second round writes the tables
 * again first and the third binds the window again.
 */
static int
vram_bar1_revive(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_bo *bo,
	unsigned round,
	const char **how)
{
	struct nvrtx_memory_state *memory;
	int error;

	memory = &gpu->memory_state;

	/* Does what the round is for. */
	if (round == VRAM_RETRY_ROUND_TABLES) {
		/* The tables, the root's entry and the run's entries written again. */
		*how = "the tables were written again";
		error = vram_bar1_repair(gpu, bo);
		if (error != 0)
			return error;
	} else if (round == VRAM_RETRY_ROUND_BIND) {
		/* The window bound to its instance block again, under the root that block names. */
		*how = "the window was bound again";
		error = vram_bar1_rebind(gpu, bo);
		if (error != 0)
			return error;
	} else if (round == 1U) {
		/* The fault record cleared and the TLB invalidated once more. */
		*how = "the fault record was cleared";
	} else {
		/* Only a longer wait, for a hub busy with the resource manager's own work. */
		*how = "a longer wait";
	}

	/* Clears the dropped-access bits and the record's validity, and drops any stale translation of the window. */
	nvrtx_wr32(gpu, VRAM_REG_FAULT_STATUS, VRAM_FAULT_STATUS_CLEAR);
	error = nvrtx_flush_tlb_bar(gpu, memory->bar1_root);
	if (error != 0)
		return error;

	/* Succeeded: the round's work is done. */
	return 0;
}

/*
 * Binds the window to its instance block again: reads the root the block
 * names and, when it is not the static information's, adopts it and writes
 * the tables under it; then writes the window's mode register with the
 * value it holds, which makes the hub load the block again, and waits for
 * the bind to settle.
 */
static int
vram_bar1_rebind(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_bo *bo)
{
	struct nvrtx_memory_state *memory;
	uint64_t instance;
	uint64_t named;
	uint32_t block;
	uint32_t status;
	int error;

	memory = &gpu->memory_state;

	/* Only a window in its virtual mode on an instance block in the VRAM can be bound again. */
	block = nvrtx_rd32(gpu, VRAM_REG_BAR1_BLOCK);
	if ((block & VRAM_BAR1_BLOCK_VIRTUAL) == 0U || (block & VRAM_BAR1_BLOCK_TARGET_MASK) != 0U) {
		nvrtx_stage_mark("vram-window retry: bar1 mode %08x is not virtual in the VRAM; not bound again", (unsigned)block);
		return EIO;
	}

	/* Reads the root the instance block names. */
	instance = (uint64_t)(block & VRAM_BAR1_BLOCK_PTR_MASK) << 12;
	error = vram_read64(gpu, instance + VRAM_INSTANCE_ROOT_AT, &named);
	if (error != 0)
		return error;

	named &= VRAM_INSTANCE_ROOT_MASK;
	nvrtx_stage_mark("vram-window retry: instance block %llx names root %llx",
	    (unsigned long long)instance,
	    (unsigned long long)named);
	nvrtx_stage_mark("vram-window retry: the static information names root %llx",
	    (unsigned long long)memory->bar1_root);

	/* Adopts a root named elsewhere and writes the tables under it. */
	if (named != 0U && named != memory->bar1_root) {
		memory->bar1_root = named;
		error = vram_bar1_repair(gpu, bo);
		if (error != 0)
			return error;
	}

	/* Binds the window again and waits for the bind to settle. */
	nvrtx_wr32(gpu, VRAM_REG_BAR1_BLOCK, block);
	error = nvrtx_poll(gpu, VRAM_REG_BAR_STATUS, VRAM_BAR1_PENDING, 0U, VRAM_BAR1_POLLS * VRAM_BAR1_POLL_US);
	if (error != 0) {
		status = nvrtx_rd32(gpu, VRAM_REG_BAR_STATUS);
		nvrtx_stage_mark("vram-window retry: the bind of bar1 did not settle (status %08x)", (unsigned)status);
		return error;
	}

	/* Succeeded: the hub holds the instance block again. */
	return 0;
}

/*
 * Writes the window's tables again in full (the console's pages with
 * them), then the self-test's run's entries, each read back.  Nothing else
 * is in the window yet.
 */
static int
vram_bar1_repair(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_bo *bo)
{
	struct nvrtx_memory_state *memory;
	int error;

	memory = &gpu->memory_state;

	/* Writes the tables and switches the root's entry to them again. */
	error = vram_bar1_write_tables(gpu);
	if (error != 0)
		return error;

	/* Maps the run again; the new tables left it out. */
	mutex_lock(&memory->lock);

	error = vram_write_ptes(gpu, bo->bar1, bo->vram, bo->size, 0);

	mutex_unlock(&memory->lock);

	/* Reports a run that could not be mapped again. */
	if (error != 0)
		return error;

	/* Succeeded: the tables and the run's entries are written again. */
	return 0;
}

/*
 * Writes the window's page entries for a run: each page of the window from
 * bar1 on points at the VRAM page of the same index from vram on, or is
 * cleared.  Then invalidates the hub's TLB.  The caller holds the memory lock.
 */
static int
vram_write_ptes(
	struct nvrtx_gpu *gpu,
	uint64_t bar1,
	uint64_t vram,
	uint64_t size,
	int clear)
{
	struct nvrtx_memory_state *memory;
	uint8_t *entries;
	uint64_t pages;
	uint64_t page;
	uint64_t entry;
	uint64_t at;
	int error;

	memory = &gpu->memory_state;
	pages = size / NVRTX_GSP_PAGE_SIZE;

	/* The page tables are one array of entries after the directories, a page table per 2 MiB in order. */
	at = memory->bar1_tables + VRAM_TABLES_PTE_AT + (bar1 / NVRTX_GSP_PAGE_SIZE) * 8U;

	/* Builds the entries in memory. */
	entries = kern_malloc((size_t)pages * 8U);
	if (entries == NULL)
		return ENOMEM;

	for (page = 0U; page < pages; page++) {
		entry = 0U;
		if (clear == 0)
			entry = nvrtx_mmu_vram_pte(vram + page * NVRTX_GSP_PAGE_SIZE);
		nvrtx_put64(entries, (size_t)page * 8U, entry);
	}

	/* Writes them through PRAMIN and reads them back. */
	error = vram_place_ptes(gpu, at, entries, pages);
	kern_free(entries);
	if (error != 0)
		return error;

	/* Drops the hub's stale translations. */
	error = nvrtx_flush_tlb_bar(gpu, memory->bar1_root);
	if (error != 0)
		return error;

	/* Succeeded: the run is mapped (or unmapped). */
	return 0;
}

/*
 * Writes a run of page entries through PRAMIN and reads the first and the
 * last back; a run that did not land is written once more, and EIO
 * reports one that still did not.
 */
static int
vram_place_ptes(
	struct nvrtx_gpu *gpu,
	uint64_t at,
	const uint8_t *entries,
	uint64_t pages)
{
	uint64_t first;
	uint64_t last;
	uint64_t first_wanted;
	uint64_t last_wanted;
	uint64_t last_at;
	unsigned attempt;
	int error;

	/* An empty run has nothing to write. */
	if (pages == 0U)
		return 0;

	/* The first and the last entry, and where the last lies in the run. */
	last_at = (pages - 1U) * 8U;
	first_wanted = nvrtx_get64(entries, 0U);
	last_wanted = nvrtx_get64(entries, (size_t)last_at);

	/* Writes the entries and checks them, twice at most. */
	error = EIO;
	for (attempt = 0U; attempt < 2U; attempt++) {
		/* Writes the entries through PRAMIN. */
		error = nvrtx_vram_write(gpu, at, entries, (size_t)pages * 8U);
		if (error != 0)
			return error;

		/* Reads the first and the last entry back. */
		error = vram_read64(gpu, at, &first);
		if (error == 0)
			error = vram_read64(gpu, at + last_at, &last);
		if (error != 0)
			return error;

		/* Stops once both read as written. */
		if (first == first_wanted && last == last_wanted) {
			error = 0;
			break;
		}

		/* Says that the entries did not land, so a second write is seen in the log. */
		nvrtx_stage_mark("bar1: entries at %llx read %llx, %llx written", (unsigned long long)at, (unsigned long long)first, (unsigned long long)first_wanted);
		error = EIO;
	}

	/* Reports entries that did not land after the second write. */
	if (error != 0)
		return error;

	/* Succeeded: the entries are in the VRAM. */
	return 0;
}

/*
 * Reads one 64-bit word of the VRAM through PRAMIN, low word first.
 */
static int
vram_read64(
	struct nvrtx_gpu *gpu,
	uint64_t address,
	uint64_t *value)
{
	uint32_t low;
	uint32_t high;
	int error;

	/* Reads the two halves. */
	error = nvrtx_vram_read32(gpu, address, &low);
	if (error != 0)
		return error;

	error = nvrtx_vram_read32(gpu, address + 4U, &high);
	if (error != 0)
		return error;

	/* Succeeded: the word in *value. */
	*value = ((uint64_t)high << 32) | low;
	return 0;
}
