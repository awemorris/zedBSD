/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * One GPU the nvrtx driver keeps, and the parts of the GSP boot path that
 * touch the kernel: the register window, DMA memory, firmware files and the
 * worker thread.
 */

#ifndef DRIVERS_GPU_NVRTX_GPU_H
#define DRIVERS_GPU_NVRTX_GPU_H

#include <stddef.h>
#include <stdint.h>

#include <drivers/generic/dma.h>
#include <drivers/pci/pci.h>
#include <kern/lock.h>

#include "drivers/gpu/nvrtx/display/nvrtx-disp.h"
#include "drivers/gpu/nvrtx/nvrtx-gsp.h"
#include "drivers/gpu/nvrtx/nvrtx-mem.h"
#include "drivers/gpu/nvrtx/nvrtx-private.h"
#include "drivers/gpu/nvrtx/nvrtx-work.h"

struct nvrtx_nvdec_state;

struct nvrtx_node;

/* The firmware directory the GSP files are read from; a chip's files are named by its profile below it. */
#define NVRTX_FIRMWARE_DIRECTORY	"/lib/firmware/nvidia/"

/*
 * The registers devinit reads: GFW's devinit is done when bit 0 of the first is
 * set and the second's low byte is 0xFF.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/devinit/tu102.c (tu102_devinit_wait).
 */
#define NVRTX_REG_DEVINIT_LEVEL		0x118128U
#define NVRTX_REG_DEVINIT_PROGRESS	0x118234U
#define NVRTX_DEVINIT_COMPLETED		0xffU

/*
 * The register layout reads for the display's VGA workspace (the VRAM size's
 * register is the profile's).
 *
 * Value referenced: Linux v6.19 drivers/gpu/drm/nouveau/nvkm/subdev/gsp/tu102.c
 * (tu102_gsp_vga_workspace_addr).
 */
#define NVRTX_REG_VGA_WORKSPACE		0x625f04U

/* The largest VBIOS the driver reads. */
#define NVRTX_VBIOS_MAX_BYTES		0x100000U

/* The chunk the firmware image is held in: whole pages, under the bus's segment limit. */
#define NVRTX_IMAGE_CHUNK_BYTES		0x100000U

/* The largest GSP image the three-level table describes with one middle-level page (512 x 512 pages). */
#define NVRTX_IMAGE_MAX_BYTES		0x40000000U

/*
 * The handles the driver gives its objects of the resource manager, the values
 * below (the space's root is refused under other handles).  A client handle is
 * in the resource server's client range, 0xc1d0 in the high half and the
 * client's number in the low half; the resource server re-encodes a client
 * handle outside it.  The channel's handle carries its channel number.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/handles.h (NVKM_RM_CLIENT,
 * NVKM_RM_DEVICE, NVKM_RM_SUBDEVICE, NVKM_RM_VASPACE, NVKM_RM_CHAN).
 */
#define NVRTX_HANDLE_CLIENT		0xc1d00000U
#define NVRTX_HANDLE_DEVICE		0xde1d0000U
#define NVRTX_HANDLE_SUBDEVICE		0x5d1d0000U
#define NVRTX_HANDLE_SPACE		0x90f10000U
#define NVRTX_HANDLE_CHANNEL		0xf1f00008U
#define NVRTX_HANDLE_COPY		0xc5b50008U

/*
 * The graphics context's objects (5f).  The golden context's space, channel
 * (chid 1) and 3D object take the next handles of the same families; the
 * scrubber's are the resource manager's fixed ones, by which it finds the
 * scrubber channel of the client.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r570/nvrm/gr.h
 * (KGRAPHICS_SCRUBBER_HANDLE_VAS, KGRAPHICS_SCRUBBER_HANDLE_CHANNEL,
 * KGRAPHICS_SCRUBBER_HANDLE_3DOBJ) and nvkm/subdev/gsp/rm/handles.h
 * (NVKM_RM_THREED).
 */
#define NVRTX_HANDLE_GOLDEN_SPACE	0x90f10001U
#define NVRTX_HANDLE_GOLDEN_CHANNEL	0xf1f00001U
#define NVRTX_HANDLE_GOLDEN_THREED	0x97000001U
#define NVRTX_HANDLE_SCRUBBER_SPACE	0xdada0042U
#define NVRTX_HANDLE_SCRUBBER_CHANNEL	0xdada0045U
#define NVRTX_HANDLE_SCRUBBER_THREED	0xdada0046U

/*
 * The first compute launch's objects (compute-test): its address space
 * (owned by the driver), its graphics channel (chid 3) and the compute
 * object on it, in the same families.
 */
#define NVRTX_HANDLE_COMPUTE_SPACE	0x90f10003U
#define NVRTX_HANDLE_COMPUTE_CHANNEL	0xf1f00003U
#define NVRTX_HANDLE_COMPUTE		0xc5c00003U

/*
 * The first draw's 3D object (draw-test), on the compute launch's channel, in
 * the same family.
 */
#define NVRTX_HANDLE_DRAW		0x97000003U

/*
 * The work channel's objects (work-channel): its address space
 * (owned by the driver), its graphics channel (chid 4) and the 3D, compute
 * and copy objects on it, in the same families.
 */
#define NVRTX_HANDLE_WORK_SPACE		0x90f10004U
#define NVRTX_HANDLE_WORK_CHANNEL	0xf1f00004U
#define NVRTX_HANDLE_WORK_THREED	0x97000004U
#define NVRTX_HANDLE_WORK_COMPUTE	0xc5c00004U
#define NVRTX_HANDLE_WORK_COPY		0xc5b50004U

/*
 * The first draw's place in the compute launch's range: its system-memory
 * pages from NVRTX_DRAW_VA_PAGES (after the launch's own), and its VRAM
 * colour target at NVRTX_DRAW_VA_VRAM.
 */
#define NVRTX_DRAW_VA_PAGES		0x28010000ULL
#define NVRTX_DRAW_PAGES		78U
#define NVRTX_DRAW_VA_VRAM		0x28100000ULL
#define NVRTX_DRAW_VRAM_BYTES		0x40000ULL

/* The compute launch's semaphore page in its range, which the draw's fences release too. */
#define NVRTX_COMPUTE_SEMAPHORE_VA	0x28005000ULL

/*
 * The VRAM the graphics context's arena reserves from the top of the last
 * usable region (golden-context), and the compute arena's (compute-test,
 * below the graphics arena).  The display's surfaces (takeover) are taken
 * below both.
 */
#define NVRTX_GR_ARENA_RESERVE		0x8800000ULL
#define NVRTX_COMPUTE_ARENA_RESERVE	0xa800000ULL

/*
 * The hash-table entries the display's channels add (takeover): two objects for
 * each of two channels.
 */
#define NVRTX_DISP_RAMHT_USED_MAX	24U

/* How long a call to the resource manager waits for its reply. */
#define NVRTX_CALL_TIMEOUT_US		5000000U

/*
 * The two falcons the boot drives: the GSP's own and the secure processor's.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/nvkm/subdev/gsp/base.c
 * (nvkm_gsp_new_) and nvkm/engine/sec2/tu102.c (tu102_sec2_new).
 */
#define NVRTX_FALCON_GSP_BASE		0x110000U
#define NVRTX_FALCON_SEC2_BASE		0x840000U

/*
 * One falcon: its register base, how its generation loads and starts a
 * program, and, for the secure processor on Turing, its bit in the PMC
 * enable register (from the device information table).
 */
struct nvrtx_falcon {
	uint32_t base;
	uint32_t pmc_mask;
	enum nvrtx_falcon_style style;
};

/* How far the GSP boot got; each later state includes the earlier ones. */
enum nvrtx_gsp_state {
	NVRTX_GSP_NONE = 0,
	NVRTX_GSP_PREPARED,
	NVRTX_GSP_REGION_SET,
	NVRTX_GSP_RUNNING,
	NVRTX_GSP_FAILED,
	/* A program did not end: the GPU is cut off the system memory, its falcons reset, its memory kept until power-off. */
	NVRTX_GSP_FAILED_HOLD
};

/*
 * One coherent DMA buffer the GSP reads, with the device it came from.
 */
struct nvrtx_buffer {
	struct drv_dma_buffer dma;
	int allocated;
};

/*
 * A firmware file the boot reads, small enough to hold in kernel memory.
 */
struct nvrtx_blob {
	uint8_t *data;
	size_t size;
};

/*
 * The memory the GSP boot prepares in layout and keeps while the GSP may read
 * it.
 *
 * Every buffer stays allocated until the GSP is stopped (unload) or, when the
 * boot never started the GSP, until the worker gives the preparation back.
 */
struct nvrtx_boot_memory {
	/* The GSP firmware image (.fwimage) in 1 MiB chunks, and its three-level table. */
	struct nvrtx_pages image;
	struct nvrtx_buffer *image_chunks;
	struct nvrtx_pages table;
	struct nvrtx_buffer table_buffer;
	uint8_t *table_cpu;
	uint64_t table_bus;
	size_t image_size;

	/* The image's signature, the RISC-V bootloader's payload and the placement record. */
	struct nvrtx_buffer signature;
	size_t signature_size;
	struct nvrtx_buffer bootloader;
	struct nvrtx_buffer record;

	/* The start page of the GSP's operating system, its three logs and the start arguments. */
	struct nvrtx_buffer libos;
	struct nvrtx_buffer logs[3];
	struct nvrtx_buffer rmargs;

	/* The shared queue memory and the channel's staging buffer. */
	struct nvrtx_buffer queues;
	struct nvrtx_pages queue_pages;
	uint8_t *queue_cpu;
	uint64_t queue_bus;
	uint8_t *staging;

	/* The two FWSEC programs, each with its command written. */
	struct nvrtx_buffer fwsec_frts;
	struct nvrtx_buffer fwsec_sb;
};

/*
 * The first address space and channel (stages address-space to channel-stop).
 *
 * The VRAM block holds the page tables, the instance block and USERD; the
 * system-memory buffers are the ring, the push buffer, the semaphore page and
 * the method buffer.  The flags say which resource-manager objects exist, so
 * the stop frees only those.
 */
struct nvrtx_engine_state {
	uint64_t vram_block;
	struct nvrtx_buffer ring;
	struct nvrtx_buffer push;
	struct nvrtx_buffer semaphore;
	struct nvrtx_buffer methods;
	uint32_t methods_size;
	struct nvrtx_engine_pick pick;
	int space_made;
	int root_set;
	int channel_made;
	int scheduled;
	int copy_made;
};

/*
 * The graphics context (the golden-context stage, nvrtx.gr=1).
 *
 * The arena is a run of VRAM below the first block that holds two sets of
 * page tables (the golden context's and the scrubber's), the two channels'
 * instance blocks and USERD, and the context buffers.  The global buffers
 * made for the golden context stay for every later graphics channel; each
 * buffer has the same address in both spaces.  The flags say which
 * resource-manager objects exist, so the stop frees only those.
 */
struct nvrtx_gr_state {
	uint64_t arena;
	struct nvrtx_gr_buffer buffers[NVRTX_GR_BUFFER_MAX];
	size_t buffer_count;
	uint64_t golden_physical[NVRTX_GR_BUFFER_MAX];
	uint64_t scrubber_physical[NVRTX_GR_BUFFER_MAX];
	uint64_t virtual_address[NVRTX_GR_BUFFER_MAX];
	struct nvrtx_buffer golden_methods;
	struct nvrtx_buffer scrubber_methods;
	int golden_space_made;
	int golden_channel_made;
	int golden_threed_made;
	int scrubber_space_made;
	int scrubber_channel_made;
	int scrubber_threed_made;
	int scrubber_enabled;
};

/*
 * The first compute launch (the compute-test stage, nvrtx.compute=1).
 *
 * The arena is a run of VRAM below the graphics arena that holds the
 * space's page tables, the channel's instance block and USERD, and the
 * channel's own context buffers (the global ones are the golden
 * context's).  The system-memory buffers are the ring, the push buffer,
 * the semaphore, the program, the QMD, constant buffer 0, the result and
 * the method buffer.  The flags say which resource-manager objects exist,
 * so the stop frees only those.
 */
struct nvrtx_compute_state {
	uint64_t arena;
	uint64_t physical[NVRTX_GR_BUFFER_MAX];
	struct nvrtx_buffer ring;
	struct nvrtx_buffer push;
	struct nvrtx_buffer semaphore;
	struct nvrtx_buffer program;
	struct nvrtx_buffer qmd;
	struct nvrtx_buffer constants;
	struct nvrtx_buffer result;
	struct nvrtx_buffer methods;
	int space_made;
	int root_set;
	int channel_made;
	int compute_made;
	int scheduled;
};

/*
 * One run of the VRAM the driver handed out (a buffer object).
 *
 * The VRAM run is whole 4 KiB pages; while bar1_mapped is set the run is
 * also mapped at bar1 in the CPU's window, so the CPU reads and writes it
 * there.  The owner frees it with nvrtx_bo_free(), which unmaps it first.
 */
struct nvrtx_bo {
	uint64_t vram;
	uint64_t size;
	uint64_t bar1;
	int bar1_mapped;
};

/*
 * One graphics channel of the work space (nvrtx-work.h has its layout):
 * the driver's own work channel, or a channel of one session's.
 *
 * chid is its number on the graphics runlist and the handles name the
 * channel and its 3D, compute and copy objects for the resource manager
 * (the copy object on the driver's channel always, on a session's only
 * with the video decoder, nvrtx.video=1).
 * instance and userd are its instance block and USERD in the VRAM (in
 * the space's arena for the driver's channel, in the channel's own arena
 * for a session's, which also holds the channel's own context buffers).
 * physical and virtual_address are where each context buffer lies and
 * where the channel's promote names it: the golden context's addresses for
 * the driver's channel, the channel's region of the space for a session's.
 * region is that 2 MiB of the space (0 for the driver's channel), and the
 * ring, the semaphore and the fences' push buffers are reached at ring_va,
 * semaphore_va and fences_va.  The system-memory buffers are the ring, the
 * semaphore, the fences' push buffers (one slot per ring entry) and the
 * method buffer.  The made flags say which resources exist, so the stop and
 * the close free only those.
 *
 * lock serializes the submissions: put is the next ring entry, fence the
 * last value a submission asked the semaphore to reach (also read without
 * the lock, by nvrtx_work_channel_passed()), entry_fence the fence each ring
 * entry's submission ended with (so an entry and its fence slot are reused
 * only after that work is done), submitted_us the CPU's time each of the
 * last runs was submitted (by the fence's low bits, read without the lock
 * by the recovery).  ready is set once the channel ran its first fence;
 * nothing is submitted before.  Every run gets a start stamp with the GPU's
 * timer before its segment (so the recovery sees which run began, and
 * when); timed, set when the boot asks for the counts (nvrtx.vk.perf=1),
 * adds an end stamp after it, so the GPU's own time for a run can be read
 * after later runs.
 *
 * state, rc_type and lost_reason are the renderer's (under its node's
 * queue_lock, read without it where a racy look is harmless): the channel's
 * state, the GSP's error report on it (its kind plus one, 0 before any) and
 * why it was lost.
 */
struct nvrtx_work_channel {
	uint32_t chid;
	uint32_t handle_channel;
	uint32_t handle_threed;
	uint32_t handle_compute;
	uint32_t handle_copy;
	struct nvrtx_bo arena;
	uint64_t instance;
	uint64_t userd;
	uint64_t physical[NVRTX_GR_BUFFER_MAX];
	uint64_t virtual_address[NVRTX_GR_BUFFER_MAX];
	uint64_t region;
	uint64_t ring_va;
	uint64_t semaphore_va;
	uint64_t fences_va;
	struct nvrtx_buffer ring;
	struct nvrtx_buffer semaphore;
	struct nvrtx_buffer fences;
	struct nvrtx_buffer methods;
	struct mutex lock;
	uint32_t put;
	uint32_t fence;
	uint32_t entry_fence[NVRTX_WORK_RING_ENTRIES];
	uint64_t submitted_us[NVRTX_WORK_RUN_RECORDS];
	int arena_made;
	int region_made;
	int channel_made;
	int threed_made;
	int compute_made;
	int copy_made;
	int scheduled;
	int ready;
	int timed;
	uint32_t state;
	uint32_t rc_type;
	int lost_reason;
};

/*
 * The work space and the driver's work channel (work-channel, nvrtx.work=1):
 * the address space the GPU device keeps for the work it is given, and the
 * graphics channel it made first (nvrtx-work.h has the layout).
 *
 * The arena is a buffer object of the VRAM allocator holding the space's
 * page tables, the driver's channel's instance block and USERD, and its own
 * context buffers (the global ones are the golden context's).  lock
 * serializes the space's mappings: va hands out the addresses of the runs
 * mapped for users and of the sessions' channels' regions.  ready is set
 * once the driver's channel ran its first fence (the space takes mappings
 * and channels from then on, even after that channel was stopped), and
 * cleared by the stop.  timed is set at the start when the boot asks for the
 * counts (nvrtx.vk.perf=1); every channel made afterwards times its runs as
 * well.
 */
struct nvrtx_work_state {
	struct nvrtx_bo arena;
	/* How many times a full space for the users' runs was told. */
	uint32_t va_full_marks;
	struct nvrtx_heap va;
	struct mutex lock;
	int arena_made;
	int space_made;
	int root_set;
	int ready;
	int timed;
	/*
	 * Whether the driver's channel had its copy object at the start (the
	 * sessions' channels are made the same way), whether the copy engine's
	 * clears of the VRAM are off for good (one failed, or the boot asked
	 * for the CPU's clears, nvrtx.clear.cpu=1), and whether the
	 * next one is to be broken on purpose (nvrtx.clear.fault=1, a test of
	 * the way back to the CPU's clears).
	 */
	int copy_ready;
	int clear_off;
	int clear_fault;
	struct nvrtx_work_channel driver;
};

/*
 * The first draw (the draw-test stage, nvrtx.draw=1), on the compute launch's
 * channel.
 *
 * The system-memory buffers are the push buffer, the four programs (a
 * vertex and a fragment program, each in one colour and textured),
 * constant buffer 0, the vertices, the texture with its header and
 * sampler, a zero page and the colour target.  The VRAM colour target
 * is a buffer object when the VRAM allocator is up (read back through
 * the CPU's window when it maps), else the compute arena's last MiB (read
 * back word by word).  The flags say what exists, so the stop and the
 * release free only that.
 */
struct nvrtx_draw_state {
	struct nvrtx_buffer push;
	struct nvrtx_buffer programs;
	struct nvrtx_buffer constants;
	struct nvrtx_buffer vertices;
	struct nvrtx_buffer texture;
	struct nvrtx_buffer headers;
	struct nvrtx_buffer samplers;
	struct nvrtx_buffer zero;
	struct nvrtx_buffer target;
	struct nvrtx_bo vram_bo;
	int vram_bo_made;
	uint64_t vram_target;
	int threed_made;
};

/*
 * The VRAM the driver hands out and the CPU's window onto it (the vram-window
 * stage).
 *
 * The VRAM allocator covers the last usable region below the fixed arenas of
 * address-space to compute-test; the BAR1 allocator covers the window the
 * driver's page tables map.  The BAR1 root (top level) is the resource
 * manager's, from the static information; the driver writes its own lower
 * levels and the root's first entry.  lock serializes both allocators and the
 * page entries; it is taken before the GPU's window_lock.  The flags say what
 * exists, so the release gives back only that.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/bar.c (the BAR1 root from
 * the static information).
 */
struct nvrtx_memory_state {
	struct mutex lock;
	int ready;
	struct nvrtx_heap vram;
	int bar1_ready;
	struct nvrtx_heap bar1;
	uint64_t bar1_root;
	uint64_t bar1_root_entry;
	/*
	 * The root's first entry as vram-window writes it (at the driver's
	 * directories), checked when the window misbehaves.
	 */
	uint64_t bar1_root_ours;
	uint64_t bar1_tables;
	uint64_t bar1_window;
	uint64_t bar1_bus;
	/* Where the CPU's mapping of the window starts: after the console's part, else 0. */
	uint64_t bar1_cpu_from;
	/* The most of the window in use at once, and how many times a full window was told. */
	uint64_t bar1_peak;
	uint32_t bar1_full_marks;
	/* How many times a full VRAM allocator was told. */
	uint32_t vram_full_marks;
	int bar1_claimed;
	struct drv_pci_mapping bar1_mapping;
	int bar1_mapped;
};

/*
 * The display under the GSP (stages display-client and takeover).
 *
 * The instance memory is 64 KiB of VRAM the resource manager keeps the
 * display's objects in.  The masks say which heads, windows and outputs the
 * hardware has and which heads the firmware (GOP) lit; the first lit head,
 * its output (SOR) and protocol are what takeover takes over.  The flags say
 * which resource-manager objects exist, so the stop frees only those.
 *
 * takeover adds the core channel and the head's first window channel (their
 * push buffers in system memory, kept until the GSP has stopped), the
 * notifiers' 4 KiB of VRAM after the instance memory, the hash-table slots
 * and context DMA objects it wrote into the instance memory, the head's
 * armed state it copied, and the two test surfaces.
 *
 * gpu-device adds the head's cursor channel (a PIO channel, made before the GPU
 * device is registered) and core_pending: set while a core update the
 * driver sent without waiting (a cursor's) may not have finished, cleared
 * once its notifier says it has.  Both are the GPU device's, under its lock.
 */
struct nvrtx_display_state {
	uint64_t instance;
	uint32_t head_count;
	uint32_t head_mask;
	uint32_t window_mask;
	uint32_t output_mask;
	uint32_t lit_mask;
	int gop_head_found;
	uint32_t gop_head;
	uint32_t gop_display_id;
	uint32_t gop_sor;
	uint32_t gop_protocol;
	int common_made;
	int root_made;

	/*
	 * display-client: the display's own client and objects, and each
	 * output's resource, type and protocol by id bit.
	 */
	int client_made;
	int device_made;
	int subdevice_made;
	int hotplug_made;
	int dp_irq_made;
	uint32_t output_or[32];
	uint32_t output_type[32];
	uint32_t output_protocol[32];

	/*
	 * takeover: the outputs connected, what the lit output's EDID says, and
	 * the LUTs' VRAM.
	 */
	uint32_t connected_mask;
	struct nvrtx_disp_sink sink;
	struct nvrtx_disp_timing timing;
	int timing_from_edid;
	uint64_t olut;
	uint64_t ilut;
	/* The window's notifier the next flip uses (its slot's two halves in turn). */
	uint32_t window_ntfy;

	/*
	 * takeover: the channels (the core, every window, window-immediate and
	 * cursor), their push buffers and what the takeover wrote.
	 */
	struct nvrtx_buffer core_buffer;
	struct nvrtx_buffer window_buffers[NVRTX_DISP_WINDOWS];
	struct nvrtx_buffer wimm_buffers[NVRTX_DISP_WINDOWS];
	uint32_t windows_made;
	uint32_t wimms_made;
	uint32_t cursors_made;
	struct nvrtx_push core_push;
	struct nvrtx_push window_push;
	uint32_t window;
	uint64_t sync;
	uint32_t object_next;
	uint32_t ramht_slots[NVRTX_DISP_RAMHT_USED_MAX];
	uint32_t ramht_count;
	uint32_t armed[NVRTX_DISP_HEAD_COPIED];
	uint32_t sor_control;
	struct nvrtx_disp_mode mode;
	/*
	 * The head's viewport input size as last released (width in bits 15:0,
	 * height in 31:16): the size of the picture the window shows.  The
	 * console may be smaller than the mode, which the head's scaler then
	 * enlarges; a flip to a picture of another size changes it with the
	 * flip.  Zero until the first commit.
	 */
	uint32_t view_in;
	uint32_t assigned_sor;
	struct nvrtx_disp_surface surfaces[2];
	struct nvrtx_bo surface_objects[2];
	int core_made;
	int window_made;
	int cursor_made;
	int core_pending;
	/*
	 * Set once takeover shows its surface on the head: the GPU device
	 * (gpu-device) may take the display.
	 */
	int taken;
};

/*
 * The firmware's console on the GPU the machine boots on (the native boot): the
 * picture the GOP left at the start of the VRAM window, which the window
 * reaches in its physical mode until the resource manager makes it virtual.
 * When the console is kept, the resource manager is told its size (it keeps
 * that much VRAM from offset 0, as for the vendor's driver), vram-window maps
 * the window's first pages back onto it, takeover shows it on the taken-over
 * head, and a GSP that stops leaves the window in physical mode again.
 *
 * kept says the GSP boot may go on with the console; mapped says the window
 * reaches it again (vram-window).  size is the VRAM it takes in whole 64 KiB,
 * pitch its row in bytes; bar1_block is the window's mode register as the
 * firmware left it.
 */
struct nvrtx_console {
	int kept;
	int mapped;
	uint64_t size;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t bar1_block;
};

/*
 * One GPU the driver keeps after console, when the boot asked for the GSP path.
 *
 * Created by the attach and owned by the PCI binding; the worker thread runs
 * the GSP boot and is the only writer of the state while it runs.  The
 * register window stays mapped for as long as the binding lasts.
 */
struct nvrtx_gpu {
	struct nvrtx_device identity;
	struct drv_pci_mapping registers;
	int registers_claimed;
	struct drv_dma_device *dma;

	/* The firmware files read in layout. */
	struct nvrtx_blob booter_load;
	struct nvrtx_blob booter_unload;
	struct nvrtx_blob bootloader_file;
	struct nvrtx_blob falcon_bootloader;
	struct nvrtx_booter load;
	struct nvrtx_booter unload;
	struct nvrtx_bootloader bootloader_desc;
	struct nvrtx_falcon_bl falcon_bl;

	/* The VBIOS read in layout and its FWSEC program. */
	uint8_t *vbios_data;
	size_t vbios_size;
	struct nvrtx_vbios vbios;
	struct nvrtx_fwsec fwsec;

	/* The two falcons. */
	struct nvrtx_falcon gsp_falcon;
	struct nvrtx_falcon sec2_falcon;

	/* The placement in the VRAM and the memory the GSP reads. */
	struct nvrtx_wpr_layout layout;
	struct nvrtx_boot_memory memory;
	struct nvrtx_channel channel;
	/* The last call's sequence number, and the buffer each received message is copied into. */
	uint32_t call_sequence;
	uint8_t *reply;
	uint8_t *call;
	/*
	 * Set once a call to the resource manager got no reply in time: the
	 * firmware is taken as not answering, and every later call fails at
	 * once instead of waiting its whole time, so a teardown of many
	 * objects (the reboot's unload) stays short.
	 */
	int rpc_unanswered;
	/*
	 * Serializes the calls to the resource manager and the reading of the
	 * GSP's messages once more than one thread reaches the GSP (the
	 * message drain, a session's channel, the recovery): gpu->call and
	 * gpu->reply are one pair, so a caller that reads a reply holds the
	 * lock from the call to the read (nvrtx_rpc_lock()); a call alone takes
	 * it itself.  rpc_wanted counts the threads waiting for it (atomically):
	 * the drain gives the lock up between its turns while it is not zero.
	 */
	struct mutex rpc_lock;
	uint32_t rpc_wanted;
	/*
	 * Told of every channel error the GSP reports (its channel number and
	 * its kind), by whichever thread reads the message under rpc_lock; set
	 * once by the renderer's start, NULL before.  It takes only spinlocks.
	 */
	void (*channel_error)(void *argument, uint32_t chid, uint32_t type);
	void *channel_error_argument;

	/* The first address space and channel. */
	struct nvrtx_engine_state engine;

	/* The graphics golden context and the scrubber. */
	struct nvrtx_gr_state gr;

	/* The display. */
	struct nvrtx_display_state display;

	/* The first compute launch, and the first draw on its channel. */
	struct nvrtx_compute_state compute;
	struct nvrtx_draw_state draw;

	/* The work channel the GPU device keeps (work-channel). */
	struct nvrtx_work_state work;

	/* The video decoder's channel (nvdec-channel, nvdec.c), NULL until it is begun. */
	struct nvrtx_nvdec_state *nvdec;

	/* The VRAM allocator and the BAR1 window. */
	struct nvrtx_memory_state memory_state;

	/*
	 * The firmware's console kept through the boot (written by the worker
	 * before layout, read after).
	 */
	struct nvrtx_console console;

	/*
	 * Serializes the register window onto the VRAM (PRAMIN) and the TLB
	 * invalidate, which every thread that writes the VRAM or the page
	 * tables shares; set up when the function is kept.  window_wanted
	 * counts the users other than a fill that wait for it (atomic): a
	 * fill steps aside between its pieces while it is not zero.
	 */
	struct mutex window_lock;
	uint32_t window_wanted;

	/*
	 * The static information static-info read, and the resource manager's
	 * own client and subdevice.
	 */
	uint8_t *static_info;
	uint32_t internal_client;
	uint32_t internal_subdevice;

	/*
	 * The registered GPU device (the gpu-device stage), and whether the GSP
	 * stays up for it: then detach refuses, and the GPU is kept until
	 * power-off.
	 */
	struct nvrtx_node *node;
	int resident;

	/*
	 * Set while the GPU counts in the kernel's hw.gpu.attaching (the
	 * graphical login waits for its display while it does): from the start
	 * of the worker that may register the device to the worker's end.
	 */
	int attach_counted;

	/* Set by the worker; read by detach only after the worker has ended. */
	enum nvrtx_gsp_state state;
	/* Set just before FWSEC-FRTS runs: from then on the end of the boot tears the region down or reports it. */
	int region_written;
	volatile int worker_running;
	/*
	 * Counts the submissions to any of the GPU's engines (every one may
	 * write a blob) and the channels' stops and resets, atomically;
	 * nvrtx_gpu_submitted() adds one.
	 */
	uint64_t submissions;
};

/* Counts one more submission to an engine of the GPU and gives the count after it. */
static inline uint64_t
nvrtx_gpu_submitted(
	struct nvrtx_gpu *gpu)
{
	return __atomic_add_fetch(&gpu->submissions, 1U, __ATOMIC_SEQ_CST);
}

uint32_t nvrtx_rd32(const struct nvrtx_gpu *gpu, uint32_t offset);
void nvrtx_wr32(const struct nvrtx_gpu *gpu, uint32_t offset, uint32_t value);

int nvrtx_buffer_alloc(struct nvrtx_gpu *gpu, size_t size, struct nvrtx_buffer *buffer);
void nvrtx_buffer_free(struct nvrtx_gpu *gpu, struct nvrtx_buffer *buffer);

int nvrtx_file_load(const char *name, size_t limit, struct nvrtx_blob *blob);
void nvrtx_file_unload(struct nvrtx_blob *blob);
int nvrtx_file_load_image(struct nvrtx_gpu *gpu, const char *name);

int nvrtx_rom_read(struct nvrtx_gpu *gpu);

int nvrtx_poll(struct nvrtx_gpu *gpu, uint32_t offset, uint32_t mask, uint32_t value, uint32_t timeout_us);
uint64_t nvrtx_now_us(void);
int nvrtx_falcon_reset(struct nvrtx_gpu *gpu, const struct nvrtx_falcon *falcon);
int nvrtx_falcon_reset_engine(struct nvrtx_gpu *gpu, const struct nvrtx_falcon *falcon);
void nvrtx_falcon_start(struct nvrtx_gpu *gpu, const struct nvrtx_falcon *falcon);
void nvrtx_falcon_load_physical(struct nvrtx_gpu *gpu, const struct nvrtx_falcon *falcon);
void nvrtx_falcon_dma_system(struct nvrtx_gpu *gpu, const struct nvrtx_falcon *falcon, uint32_t context);
uint32_t nvrtx_falcon_code_limit(struct nvrtx_gpu *gpu, const struct nvrtx_falcon *falcon);
int nvrtx_falcon_imem(struct nvrtx_gpu *gpu, const struct nvrtx_falcon *falcon, const uint8_t *code, uint32_t length, uint32_t address, uint32_t tag, int secure);
int nvrtx_falcon_dmem(struct nvrtx_gpu *gpu, const struct nvrtx_falcon *falcon, const uint8_t *data, uint32_t length, uint32_t address);
int nvrtx_falcon_boot(struct nvrtx_gpu *gpu, const struct nvrtx_falcon *falcon, uint32_t boot_address, uint32_t *mailbox0, uint32_t *mailbox1);
int nvrtx_falcon_riscv_mode(struct nvrtx_gpu *gpu, const struct nvrtx_falcon *falcon);
bool nvrtx_falcon_riscv_running(struct nvrtx_gpu *gpu, const struct nvrtx_falcon *falcon, uint32_t *status);
int nvrtx_falcon_dma_load(struct nvrtx_gpu *gpu, const struct nvrtx_falcon *falcon, uint64_t bus, uint32_t source, uint32_t length, uint32_t target, int code, int secure);
void nvrtx_falcon_boot_rom(struct nvrtx_gpu *gpu, const struct nvrtx_falcon *falcon, uint32_t signature_at, uint32_t engine_id, uint32_t ucode_id);

int nvrtx_fwsec_run(struct nvrtx_gpu *gpu, const struct nvrtx_buffer *program);
int nvrtx_booter_run(struct nvrtx_gpu *gpu, const struct nvrtx_booter *booter, uint32_t mailbox0, uint32_t mailbox1);
int nvrtx_region_tear_down(struct nvrtx_gpu *gpu);
void nvrtx_region_bounds(struct nvrtx_gpu *gpu, uint32_t *low, uint32_t *high);
uint32_t nvrtx_frts_result(struct nvrtx_gpu *gpu);

int nvrtx_rpc_send(struct nvrtx_gpu *gpu, uint32_t function, const uint8_t *payload, size_t length, int with_sequence, uint32_t *sequence);
int nvrtx_rpc_wait(struct nvrtx_gpu *gpu, uint32_t function, uint32_t sequence, uint32_t timeout_us, size_t *length);
void nvrtx_rpc_lock(struct nvrtx_gpu *gpu);
void nvrtx_rpc_unlock(struct nvrtx_gpu *gpu);

int nvrtx_rm_allocate(struct nvrtx_gpu *gpu, uint32_t parent, uint32_t handle, uint32_t class_number, const uint8_t *parameters, size_t length);
int nvrtx_rm_control(struct nvrtx_gpu *gpu, uint32_t client, uint32_t object, uint32_t command, const uint8_t *parameters, size_t length, const uint8_t **reply, size_t *reply_length);
int nvrtx_rm_free(struct nvrtx_gpu *gpu, uint32_t object);
int nvrtx_rm_allocate_in(struct nvrtx_gpu *gpu, uint32_t client, uint32_t parent, uint32_t handle, uint32_t class_number, const uint8_t *parameters, size_t length);
int nvrtx_rm_free_in(struct nvrtx_gpu *gpu, uint32_t client, uint32_t object);

int nvrtx_space_start(struct nvrtx_gpu *gpu);
int nvrtx_vram_write(struct nvrtx_gpu *gpu, uint64_t address, const uint8_t *data, size_t length);
int nvrtx_vram_zero(struct nvrtx_gpu *gpu, uint64_t address, size_t length);
int nvrtx_vram_fill(struct nvrtx_gpu *gpu, uint64_t address, uint32_t value, size_t length);
int nvrtx_vram_read32(struct nvrtx_gpu *gpu, uint64_t address, uint32_t *value);
int nvrtx_flush_tlb(struct nvrtx_gpu *gpu, uint64_t root);
int nvrtx_flush_tlb_bar(struct nvrtx_gpu *gpu, uint64_t root);
int nvrtx_channel_start(struct nvrtx_gpu *gpu);
void nvrtx_channel_stop(struct nvrtx_gpu *gpu);
void nvrtx_channel_release(struct nvrtx_gpu *gpu);

int nvrtx_memory_start(struct nvrtx_gpu *gpu);
void nvrtx_memory_release(struct nvrtx_gpu *gpu);
int nvrtx_console_check(struct nvrtx_gpu *gpu);
void nvrtx_console_restore(struct nvrtx_gpu *gpu);
int nvrtx_bo_alloc(struct nvrtx_gpu *gpu, uint64_t size, uint64_t align, struct nvrtx_bo *bo);
int nvrtx_bo_map(struct nvrtx_gpu *gpu, struct nvrtx_bo *bo);
int nvrtx_bo_clear(struct nvrtx_gpu *gpu, struct nvrtx_bo *bo);
void nvrtx_bo_window_use(struct nvrtx_gpu *gpu, uint64_t *used, uint64_t *peak, uint64_t *size);
void nvrtx_bo_free(struct nvrtx_gpu *gpu, struct nvrtx_bo *bo);
volatile uint8_t *nvrtx_bo_cpu(struct nvrtx_gpu *gpu, const struct nvrtx_bo *bo);
uint64_t nvrtx_bo_bus(struct nvrtx_gpu *gpu, const struct nvrtx_bo *bo);

int nvrtx_gr_start(struct nvrtx_gpu *gpu);
void nvrtx_gr_stop(struct nvrtx_gpu *gpu);
void nvrtx_gr_release(struct nvrtx_gpu *gpu);
int nvrtx_gr_promote(struct nvrtx_gpu *gpu, uint32_t channel, const uint64_t *physical, const uint64_t *virtual_address, bool golden);

int nvrtx_compute_start(struct nvrtx_gpu *gpu);
void nvrtx_compute_stop(struct nvrtx_gpu *gpu);
void nvrtx_compute_release(struct nvrtx_gpu *gpu);
int nvrtx_compute_submit(struct nvrtx_gpu *gpu, uint32_t entry, uint64_t push_va, size_t length);
int nvrtx_compute_wait(struct nvrtx_gpu *gpu, uint32_t value, uint32_t *waited);

int nvrtx_work_start(struct nvrtx_gpu *gpu);
void nvrtx_work_stop(struct nvrtx_gpu *gpu);
void nvrtx_work_release(struct nvrtx_gpu *gpu);
int nvrtx_work_map_vram(struct nvrtx_gpu *gpu, uint64_t physical, uint64_t size, uint64_t *va);
int nvrtx_work_map_system(struct nvrtx_gpu *gpu, const uint64_t *pages, size_t count, uint64_t *va);
int nvrtx_work_unmap(struct nvrtx_gpu *gpu, uint64_t va, uint64_t size);
int nvrtx_work_clear(struct nvrtx_gpu *gpu, uint64_t physical, uint64_t size);
int nvrtx_work_remap_vram(struct nvrtx_gpu *gpu, uint64_t va, uint64_t physical, uint64_t size, uint32_t kind);
uint64_t nvrtx_work_root(const struct nvrtx_gpu *gpu);
int nvrtx_work_channel_open(struct nvrtx_gpu *gpu, struct nvrtx_work_channel *channel, uint32_t chid);
int nvrtx_work_channel_close(struct nvrtx_gpu *gpu, struct nvrtx_work_channel *channel);
void nvrtx_work_channel_look(struct nvrtx_work_channel *channel, struct nvrtx_recover_view *view);
bool nvrtx_work_channel_resident(struct nvrtx_gpu *gpu, const struct nvrtx_work_channel *channel);
int nvrtx_work_channel_stop(struct nvrtx_gpu *gpu, struct nvrtx_work_channel *channel);
int nvrtx_work_channel_reset(struct nvrtx_gpu *gpu, struct nvrtx_work_channel *channel, int reported);
int nvrtx_work_channel_free(struct nvrtx_gpu *gpu, struct nvrtx_work_channel *channel);
int nvrtx_work_channel_submit(struct nvrtx_gpu *gpu, struct nvrtx_work_channel *channel, uint64_t push_va, size_t length, uint32_t *fence);
int nvrtx_work_channel_wait(struct nvrtx_work_channel *channel, uint32_t fence, uint32_t timeout_us);
int nvrtx_work_channel_wait_late(struct nvrtx_work_channel *channel, uint32_t fence, uint32_t timeout_us, uint32_t quiet_us);
bool nvrtx_work_channel_passed(struct nvrtx_work_channel *channel, uint32_t fence);
int nvrtx_work_channel_run_time(struct nvrtx_work_channel *channel, uint32_t fence, uint64_t *gpu_ns);

void nvrtx_perf_start(struct nvrtx_gpu *gpu);

int nvrtx_draw_buffers(struct nvrtx_gpu *gpu, uint64_t arena_target);
void nvrtx_draw_pages(const struct nvrtx_gpu *gpu, uint64_t *pages);
int nvrtx_draw_start(struct nvrtx_gpu *gpu);
int nvrtx_draw_stop(struct nvrtx_gpu *gpu);
void nvrtx_draw_release(struct nvrtx_gpu *gpu);

int nvrtx_display_start(struct nvrtx_gpu *gpu);
int nvrtx_display_inherit(struct nvrtx_gpu *gpu);
int nvrtx_display_detect(struct nvrtx_gpu *gpu);
int nvrtx_display_hdmi(struct nvrtx_gpu *gpu);
void nvrtx_display_stop(struct nvrtx_gpu *gpu);

int nvrtx_scanout_start(struct nvrtx_gpu *gpu);
int nvrtx_scanout_frame(struct nvrtx_gpu *gpu, const struct nvrtx_disp_surface *surface);
int nvrtx_scanout_frame_send(struct nvrtx_gpu *gpu, const struct nvrtx_disp_surface *surface, uint32_t *notifier);
int nvrtx_scanout_frame_wait(struct nvrtx_gpu *gpu, uint32_t notifier);
void nvrtx_scanout_stop(struct nvrtx_gpu *gpu);
void nvrtx_scanout_release(struct nvrtx_gpu *gpu);
int nvrtx_scanout_cursor_start(struct nvrtx_gpu *gpu);
int nvrtx_scanout_cursor_show(struct nvrtx_gpu *gpu, uint64_t offset, uint32_t point);
int nvrtx_scanout_cursor_point(struct nvrtx_gpu *gpu, uint32_t point);
int nvrtx_scanout_cursor_clear(struct nvrtx_gpu *gpu);
int nvrtx_scanout_core_settled(struct nvrtx_gpu *gpu, uint32_t wait_us);
uint32_t nvrtx_scanout_frame_counter(struct nvrtx_gpu *gpu);

int nvrtx_node_start(struct nvrtx_gpu *gpu);
void nvrtx_node_stop(struct nvrtx_node *node);

int nvrtx_gsp_start(struct nvrtx_gpu *gpu);
int nvrtx_gsp_shutdown(struct nvrtx_gpu *gpu);
void nvrtx_gsp_release(struct nvrtx_gpu *gpu);

#endif
