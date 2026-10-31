/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Internal interface of the nvrtx driver (NVIDIA Turing and later GPUs).
 *
 * The driver is brought up one stage at a time.  This revision has the stages
 * that only read: identify finds the GPU and reads its chip identity, console
 * records where the firmware's picture lives.  Nothing is written to the GPU
 * and the function is left to the firmware afterwards.
 */

#ifndef DRIVERS_GPU_NVRTX_PRIVATE_H
#define DRIVERS_GPU_NVRTX_PRIVATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/pci/pci.h>

/* Marks a parameter a callback receives but does not need. */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(parameter) ((void)(parameter))
#endif

/* The PCI vendor of every NVIDIA function. */
#define NVRTX_PCI_VENDOR		0x10deU

/* The PCI display class (VGA-compatible and 3D controllers), matched on its base class byte. */
#define NVRTX_PCI_CLASS_DISPLAY		0x030000U
#define NVRTX_PCI_CLASS_DISPLAY_MASK	0xff0000U

/*
 * The identity register at the start of the register window.
 *
 * Its bits 28:20 name the chip: bits 28:24 the architecture and bits 23:20
 * the implementation within it.  Bits 7:0 are the revision.
 *
 * Values referenced: NVIDIA open-gpu-doc
 * manuals/turing/tu104/dev_master.ref.txt (NV_PMC_BOOT_0,
 * NV_PMC_BOOT_0_ARCHITECTURE, NV_PMC_BOOT_0_IMPLEMENTATION) and Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/engine/device/base.c (nvkm_device_ctor).
 */
#define NVRTX_REG_MC_IDENT_0		0x000000U
#define NVRTX_IDENT_CHIP_SHIFT		20U
#define NVRTX_IDENT_CHIP_MASK		0x1ffU
#define NVRTX_IDENT_ARCH_SHIFT		24U
#define NVRTX_IDENT_ARCH_MASK		0x1fU
#define NVRTX_IDENT_REVISION_MASK	0xffU

/*
 * The second identity register.
 *
 * It reads 0 when the GPU answers in little-endian order and 0x01000001
 * when it was switched to big-endian.  On Turing and later, bits 17:16 are
 * set on a virtual function of a partitioned GPU.
 *
 * Values referenced: NVIDIA open-gpu-doc
 * manuals/turing/tu104/dev_master.ref.txt (NV_PMC_BOOT_1, NV_PMC_BOOT_1_VGPU)
 * and Linux v6.19 drivers/gpu/drm/nouveau/nvkm/engine/device/base.c
 * (nvkm_device_endianness, nvkm_device_ctor).
 */
#define NVRTX_REG_MC_IDENT_1		0x000004U
#define NVRTX_IDENT_1_BIG_ENDIAN	0x01000001U
#define NVRTX_IDENT_1_VIRTUAL_MASK	0x00030000U

/* The register window must reach past both identity registers; it is 16 MiB on Turing. */
#define NVRTX_REGISTER_WINDOW_MIN	0x1000U

/*
 * How much of the register window identify maps: the first page holds both
 * identity registers.
 */
#define NVRTX_N0_MAP_BYTES		0x1000U

/* The PCI Express capability and the function-level-reset bit of its device capabilities. */
#define NVRTX_PCIE_CAPABILITY_ID	0x10U
#define NVRTX_PCIE_DEVICE_CAPABILITIES	0x04U
#define NVRTX_PCIE_DEVICE_CAP_FLR	(1U << 28)

/* The widest line a stage mark writes, so a mark never wraps on the console. */
#define NVRTX_STAGE_LINE_COLUMNS	79U

/* The memory windows identify records: registers, VRAM, instance memory. */
#define NVRTX_WINDOW_COUNT		3U

/* The GPU generations, by the architecture field of the identity register. */
enum nvrtx_family {
	NVRTX_FAMILY_UNKNOWN = 0,
	NVRTX_FAMILY_TURING,
	NVRTX_FAMILY_AMPERE,
	NVRTX_FAMILY_HOPPER,
	NVRTX_FAMILY_ADA,
	NVRTX_FAMILY_BLACKWELL
};

/*
 * How a generation's falcons are given a signed program.
 *
 * On Turing the driver writes the falcon's memories through their ports and
 * FWSEC is copied in by the small falcon bootloader.  From Ampere on the
 * falcon copies the program itself by DMA, and its boot ROM checks the
 * signature the driver patched in against the fuses before it starts it.
 */
enum nvrtx_falcon_style {
	NVRTX_FALCON_PORTS = 0,
	NVRTX_FALCON_BOOT_ROM
};

/* The most shared-memory carve-outs (in KiB) a generation's SM offers. */
#define NVRTX_SHARED_CARVEOUTS_MAX	6U

/*
 * What one family of chips needs from the GSP boot path and the classes.
 *
 * One record stands for the chips that share a firmware set: TU102, TU104
 * and TU106; TU116 and TU117; GA102 to GA107; AD102 to AD107.  The chip
 * table points every known chip at its record, and every stage reads the
 * generation's differences from it instead of testing chip numbers.  The
 * records are constant and live as long as the kernel.
 *
 * verified records whether the family was seen working on real hardware;
 * a family not yet seen (verified zero) is booted all the same, and its
 * boot says so in the marks.
 */
struct nvrtx_profile {
	const char *name;
	int verified;

	/* The firmware files, below /lib/firmware/nvidia/, and the image's signature section. */
	const char *image_file;
	const char *bootloader_file;
	const char *booter_load_file;
	const char *booter_unload_file;
	/* The falcon bootloader FWSEC is copied in by; NULL where the falcon loads FWSEC itself. */
	const char *falcon_bootloader_file;
	const char *signature_section;
	enum nvrtx_falcon_style falcon_style;

	/* The VRAM-size register, and whether it holds whole MiB (else the magnitude and scale form). */
	uint32_t vram_size_register;
	int vram_size_in_mib;
	/* The fuse register whose bit 0 says the GPU has no display (and no VBIOS copy in the VRAM). */
	uint32_t display_fuse_register;

	/* The GSP heap: the part its operating system takes, and the least the heap may be. */
	uint64_t heap_os_bytes;
	uint64_t heap_min_bytes;

	/* Nonzero when the graphics context needs the scrubber channel (TU102, TU104, TU106 only). */
	int gr_scrubber;

	/* The classes of the channel, the copy engine, 3D and compute. */
	uint32_t class_channel;
	uint32_t class_copy;
	uint32_t class_threed;
	uint32_t class_compute;

	/* The display's classes: the display itself, the core, window, window-immediate and cursor channels. */
	uint32_t class_display;
	uint32_t class_core;
	uint32_t class_window;
	uint32_t class_wimm;
	uint32_t class_cursor;
	/* The window's storage word for a pitch-linear surface (Ampere's window has no layout field). */
	uint32_t window_storage_pitch;

	/* The QMD version compute launches are described in. */
	uint32_t qmd_major;
	uint32_t qmd_minor;
	/* The shader model the compiler targets (75, 86 or 89), and the warps one SM holds. */
	uint32_t sm;
	uint32_t warps_per_sm;
	/* The shared-memory carve-outs of one SM, smallest first, in KiB. */
	uint32_t shared_kib[NVRTX_SHARED_CARVEOUTS_MAX];
	uint32_t shared_count;
};

/*
 * What the identity registers say about one GPU.
 *
 * Filled by nvrtx_identify() from the two register values alone, so the
 * decision a stage takes on it can be tested on the host.
 */
struct nvrtx_chip {
	uint32_t chipset;
	uint32_t revision;
	enum nvrtx_family family;
	const char *name;
	/* The chip's family record; NULL for a known chip the GSP boot path does not cover. */
	const struct nvrtx_profile *profile;
};

/*
 * What the attach does with one GPU, and the reason its mark gives.
 *
 * Decided by nvrtx_boot_choose() from the family record, whether the
 * firmware's picture is on the GPU and the boot's nvrtx.gsp parameter, so
 * every combination can be checked on the host.
 */
enum nvrtx_boot_choice {
	/* Boots the GSP on a family seen working on real hardware. */
	NVRTX_BOOT_START = 0,
	/* Boots the GSP on a family not yet seen working on real hardware. */
	NVRTX_BOOT_START_UNSEEN,
	/* Leaves the GPU to the firmware because the boot says nvrtx.gsp=0. */
	NVRTX_BOOT_LEAVE_OFF,
	/* Leaves the GPU to the firmware because the chip has no GSP path. */
	NVRTX_BOOT_LEAVE_NO_PATH,
	/* Leaves a GPU the firmware showed no picture on (nvrtx.gsp=1 boots it). */
	NVRTX_BOOT_LEAVE_NO_PICTURE
};

/*
 * One product (PCI device ID) of the chips the driver knows.
 *
 * The product names a board, the chip names the silicon; the chip is
 * decided by the identity register, the product only names it in the marks.
 */
struct nvrtx_product {
	uint16_t device;
	uint16_t chipset;
	/* Nonzero for a laptop part. */
	uint8_t laptop;
	const char *name;
};

/*
 * One memory window of the GPU, as its base address register describes it.
 *
 * The windows are counted in order of the memory BARs, because a 64-bit BAR
 * takes two register slots and moves the next window's slot number.
 */
struct nvrtx_window {
	int present;
	unsigned bar;
	uint64_t bus_address;
	uint64_t size;
	int is_64bit;
	int prefetchable;
};

/*
 * One NVIDIA GPU function while the driver looks at it.
 *
 * Lives on the attach's stack for the read-only stages: the attach takes the
 * register window, runs identify and console, and gives everything back before
 * it returns, so no state outlives the attach.
 */
struct nvrtx_device {
	struct drv_pci_device *pci;
	struct drv_pci_address address;
	uint16_t product;
	/* The command bits from before the attach, restored when it gives the function back. */
	struct drv_pci_enable_state enable_state;
	int enable_saved;
	struct nvrtx_window windows[NVRTX_WINDOW_COUNT];
	/* Set while the register window's BAR is claimed, so the release gives it back once. */
	int registers_claimed;
	struct drv_pci_mapping registers;
	struct nvrtx_chip chip;
	/* Nonzero when the function advertises a function-level reset. */
	int flr;
	/*
	 * Nonzero when console found the firmware's picture in this GPU's VRAM
	 * window: the GPU is the primary display.
	 */
	int gop_on_gpu;
	/*
	 * That picture as the loader describes it: its offset in the VRAM
	 * window, its size, its stride in pixels and its format (a
	 * KERN_BOOT_FRAMEBUFFER_* code).
	 */
	uint64_t gop_offset;
	uint32_t gop_width;
	uint32_t gop_height;
	uint32_t gop_stride;
	uint32_t gop_format;
};

/*
 * Runs the identify stage: finds the windows, maps the registers and reads the
 * chip.
 */
int
nvrtx_n0_discover(
	struct nvrtx_device *device);

/*
 * Runs the console stage: records where the firmware's picture lies.
 */
void
nvrtx_n1_readout(
	struct nvrtx_device *device);

/*
 * Gives back everything identify took, in the reverse order.
 */
void
nvrtx_n0_release(
	struct nvrtx_device *device);

/*
 * Decodes the chip identity from the two identity registers.
 *
 * Returns 0 for a Turing or later GPU the identity names, ENODEV for a GPU
 * the driver never drives (older generations, a virtual function, a
 * big-endian or unreadable identity), and ENOTSUP for a newer chip number
 * it does not know.
 */
int
nvrtx_identify(
	uint32_t ident0,
	uint32_t ident1,
	struct nvrtx_chip *chip);

/*
 * Decides whether the attach boots the GSP on a GPU or leaves it to the firmware.
 */
enum nvrtx_boot_choice
nvrtx_boot_choose(
	const struct nvrtx_profile *profile,
	int gop_on_gpu,
	bool gsp_wanted,
	bool gsp_forced);

/*
 * Finds the product a PCI device ID names.
 *
 * Returns NULL for a device ID the table does not list.
 */
const struct nvrtx_product *
nvrtx_product_find(
	uint16_t device);

/*
 * Reports how many products the table lists.
 */
size_t
nvrtx_product_count(void);

/*
 * Reports one product of the table by its index.
 */
const struct nvrtx_product *
nvrtx_product_at(
	size_t index);

/*
 * The device information table: 64 words naming the engines, their
 * registers and their reset bits in the PMC enable register.
 *
 * Values referenced: NVIDIA open-gpu-doc manuals/turing/tu104/dev_top.ref.txt
 * (NV_PTOP_DEVICE_INFO, NV_PTOP_DEVICE_INFO_ENTRY, NV_PTOP_DEVICE_INFO_RESET,
 * NV_PTOP_DEVICE_INFO_RESET_ENUM, NV_PTOP_DEVICE_INFO_TYPE_ENUM,
 * NV_PTOP_DEVICE_INFO_CHAIN) and Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/top/gk104.c (gk104_top_parse).
 */
#define NVRTX_REG_DEVICE_INFO		0x022700U
#define NVRTX_DEVICE_INFO_WORDS		64U
#define NVRTX_DEVICE_INFO_KIND_MASK	0x3U
#define NVRTX_DEVICE_INFO_INVALID	0x0U
#define NVRTX_DEVICE_INFO_ENUM		0x2U
#define NVRTX_DEVICE_INFO_TYPE		0x3U
#define NVRTX_DEVICE_INFO_HAS_RESET	0x4U
#define NVRTX_DEVICE_INFO_RESET_SHIFT	9U
#define NVRTX_DEVICE_INFO_RESET_MASK	0x1fU
#define NVRTX_DEVICE_INFO_TYPE_SHIFT	2U
#define NVRTX_DEVICE_INFO_TYPE_MASK	0x1fffffffU
#define NVRTX_DEVICE_INFO_CHAINED	0x80000000U

/*
 * The engine type of the secure processor (SEC2) that runs the booters.
 *
 * Value referenced: NVIDIA open-gpu-doc manuals/turing/tu104/dev_top.ref.txt
 * (NV_PTOP_DEVICE_INFO_TYPE_ENUM_SEC) and Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/top/gk104.c (gk104_top_parse).
 */
#define NVRTX_ENGINE_SEC2		0xdU

/*
 * Finds the reset bit of one engine in the device information table.
 */
int
nvrtx_device_info_reset(
	const uint32_t *words,
	uint32_t engine_type,
	uint32_t *reset_bit);

/*
 * Names a GPU generation for the stage marks.
 */
const char *
nvrtx_family_name(
	enum nvrtx_family family);

/*
 * Reports whether the boot asked the driver to stay out entirely (nvrtx.off=1 or nvrtx=off).
 */
bool
nvrtx_stage_driver_off(void);

/*
 * Reports whether the GSP boot path runs: by default, unless the boot says nvrtx.gsp=0.
 */
bool
nvrtx_stage_gsp_wanted(void);

/*
 * Reports whether the boot forced the GSP boot path (nvrtx.gsp=1), even
 * where the firmware's picture on the GPU cannot be kept.
 */
bool
nvrtx_stage_gsp_forced(void);

/*
 * Reports whether the boot asked for the graphics golden context (nvrtx.gr=1).
 */
bool
nvrtx_stage_gr_wanted(void);

/*
 * Reports whether the display takeover runs: by default, unless the boot says nvrtx.disp=0.
 */
bool
nvrtx_stage_disp_wanted(void);

/*
 * Reports whether the takeover is the monitor test (nvrtx.disp.test=1).
 */
/*
 * Reports whether the takeover shows the driver's own surface at once
 * (nvrtx.disp.own=1) instead of the console.
 */
bool
nvrtx_stage_disp_own_wanted(void);

bool
nvrtx_stage_disp_test_wanted(void);

/*
 * Reports whether the display scans out block-linear images (nvrtx.disp.bl=1).
 */
bool
nvrtx_stage_disp_bl_wanted(void);

/*
 * Reports whether the boot asked for the first compute launch (nvrtx.compute=1).
 */
bool
nvrtx_stage_compute_wanted(void);

/*
 * Reports whether the boot asked for the first draw (nvrtx.draw=1).
 */
bool
nvrtx_stage_draw_wanted(void);

/*
 * Reports whether the boot asked for the work channel (nvrtx.work=1), or
 * wants the renderer that runs its work there.
 */
bool
nvrtx_stage_work_wanted(void);

/*
 * Reports whether the boot asked for the video decoder's channel
 * (nvrtx.video=1).
 */
bool
nvrtx_stage_video_wanted(void);

/*
 * Reports whether the boot asked for the first copy-engine clear of the
 * VRAM to be broken on purpose (nvrtx.clear.fault=1).
 */
bool
nvrtx_stage_clear_fault_wanted(void);

/*
 * Reports whether the boot asked for the VRAM to be cleared by the CPU only
 * (nvrtx.clear.cpu=1).
 */
bool
nvrtx_stage_clear_cpu_wanted(void);

/*
 * Reports whether the GPU device is also a renderer (the default; nvrtx.vk=0
 * turns it off).
 */
bool
nvrtx_stage_vk_wanted(void);

/*
 * Reports whether the boot asked for the renderer's and the display's counts
 * every 5 s (nvrtx.vk.perf=1).
 */
bool
nvrtx_stage_vk_perf_wanted(void);

/*
 * Reports whether each session gets a channel of its own (nvrtx.channels=0
 * shares the work channel instead).
 */
bool
nvrtx_stage_channels_wanted(void);

/*
 * Reports whether a session's channel whose work did not end is stopped
 * (nvrtx.kill=0 only reports the loss).
 */
bool
nvrtx_stage_kill_wanted(void);

/*
 * Reports whether the GPU stays up as a GPU device after the takeover: by
 * default, unless the boot says nvrtx.resident=0.
 */
bool
nvrtx_stage_resident_wanted(void);

/*
 * Reports whether a stage may run; nvrtx.stop=<stage> stops before it.
 */
bool
nvrtx_stage_allowed(
	const char *stage);

/*
 * Reports whether the boot asks the GSP boot to give up at a point on purpose
 * (nvrtx.fail=<point>, a test of the way back to the firmware's picture).
 */
bool
nvrtx_stage_fail_wanted(
	const char *point);

/*
 * Writes one stage-mark line to the console and the kernel log.
 */
void
nvrtx_stage_mark(
	const char *format,
	...) __attribute__((format(printf, 1, 2)));

#endif
