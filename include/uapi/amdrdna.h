/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amdrdna driver's reports, read through GPU_DIAGNOSE on its GPU
 * device (/dev/gpuN, the driver named "amdrdna").
 *
 * The driver brings an AMD RDNA GPU up one stage at a time, as far as the
 * boot line says.  Its reports tell how far it came and what each stage
 * read, hand out the GPU's video BIOS image, describe the display the
 * driver took over and its picture, and act as if the monitor went away
 * or came.
 *
 * Every structure starts with its version and its size, which the driver
 * writes, holds fixed-width integers only, and keeps its reserved words
 * zero.
 */

#ifndef KERN_UAPI_AMDRDNA_H
#define KERN_UAPI_AMDRDNA_H

#include <stddef.h>
#include <stdint.h>
#include <uapi/gpu.h>

/* The version every structure of this interface carries. */
#define AMDRDNA_INTERFACE_VERSION	2U

/*
 * The reports (gpu_diagnose.kind):
 *  - STATUS: struct amdrdna_status.
 *  - VBIOS: the video BIOS image's bytes, read on by offset (ENOENT
 *    before stage A1v read it).
 *  - DISPLAY: struct amdrdna_display_state (ENOENT before stage A1 found
 *    the firmware's display).
 *  - SCREEN (root): struct amdrdna_screen, then the picture's bytes.
 *  - HOTPLUG (root): argument is the level of struct amdrdna_hotplug; the
 *    report is that structure, read at offset 0 (the action runs then).
 */
#define AMDRDNA_DIAGNOSE_STATUS		1U
#define AMDRDNA_DIAGNOSE_VBIOS		2U
#define AMDRDNA_DIAGNOSE_DISPLAY	3U
#define AMDRDNA_DIAGNOSE_SCREEN		(GPU_DIAGNOSE_ROOT | 1U)
#define AMDRDNA_DIAGNOSE_HOTPLUG	(GPU_DIAGNOSE_ROOT | 2U)

/*
 * The stages in the order the driver runs them.  A stage reported as the
 * last one done means every earlier stage is done too.
 */
#define AMDRDNA_STAGE_NONE		0U
#define AMDRDNA_STAGE_A0		1U
#define AMDRDNA_STAGE_A1		2U
#define AMDRDNA_STAGE_A1V		3U
#define AMDRDNA_STAGE_A2		4U
#define AMDRDNA_STAGE_A3T		5U
#define AMDRDNA_STAGE_A3		6U
#define AMDRDNA_STAGE_A4		7U
#define AMDRDNA_STAGE_A5		8U
#define AMDRDNA_STAGE_A5S		9U
#define AMDRDNA_STAGE_A6		10U
#define AMDRDNA_STAGE_A7		11U
#define AMDRDNA_STAGE_A8		12U
#define AMDRDNA_STAGE_A9		13U
#define AMDRDNA_STAGE_A5C		14U
#define AMDRDNA_STAGE_A10		15U
#define AMDRDNA_STAGE_A11		16U
#define AMDRDNA_STAGE_A12		17U
#define AMDRDNA_STAGE_A13		18U
#define AMDRDNA_STAGE_COUNT		19U

/* What the status's flags word says about the GPU. */
#define AMDRDNA_STATUS_POSTED		0x00000001U
#define AMDRDNA_STATUS_SECURE_OS_ALIVE	0x00000002U
#define AMDRDNA_STATUS_PLATFORM_READY	0x00000004U
#define AMDRDNA_STATUS_RESIZABLE_BAR	0x00000008U
#define AMDRDNA_STATUS_VBIOS		0x00000010U
#define AMDRDNA_STATUS_BOOT_DATABASE	0x00000020U
#define AMDRDNA_STATUS_TRAINING_ALLOWED	0x00000040U
#define AMDRDNA_STATUS_GOP_ON_GPU	0x00000080U
#define AMDRDNA_STATUS_THREAD_RUNNING	0x00000100U
#define AMDRDNA_STATUS_INTERRUPTS_ON	0x00000200U
#define AMDRDNA_STATUS_SECURE_OS_OURS	0x00000400U
#define AMDRDNA_STATUS_THERMAL_FAULT	0x00000800U
#define AMDRDNA_STATUS_SMU_HUNG		0x00001000U

/* How memory training ended (training_result). */
#define AMDRDNA_TRAINING_NONE		0U
#define AMDRDNA_TRAINING_RAN		1U
#define AMDRDNA_TRAINING_SKIPPED	2U

/*
 * Why memory training was skipped (training_reason): the boot line said
 * so, there is no video BIOS image, the board does not ask for it, or a
 * secure OS already runs.
 */
#define AMDRDNA_TRAINING_SKIP_NONE	0U
#define AMDRDNA_TRAINING_SKIP_ASKED	1U
#define AMDRDNA_TRAINING_SKIP_NO_VBIOS	2U
#define AMDRDNA_TRAINING_SKIP_NOT_ASKED	3U
#define AMDRDNA_TRAINING_SKIP_SECURE_OS	4U

/*
 * How the fan is driven (fan_mode): not yet by the driver, at a static
 * duty through the thermal block (the static tier), or by the
 * system-management unit's fan control (the full tier).
 */
#define AMDRDNA_FAN_NONE		0U
#define AMDRDNA_FAN_STATIC		1U
#define AMDRDNA_FAN_FIRMWARE		2U

/* Where the video BIOS image came from. */
#define AMDRDNA_VBIOS_SOURCE_NONE	0U
#define AMDRDNA_VBIOS_SOURCE_ACPI	1U
#define AMDRDNA_VBIOS_SOURCE_ROM	2U

/*
 * How the firmware's surface reaches the screen: it fills the active area
 * (gop_width by gop_height) pixel for pixel, it sits unscaled inside it,
 * or the scaler stretches the fetched part (gop_source_width by
 * gop_source_height) over it.
 */
#define AMDRDNA_GOP_SHAPE_WHOLE		0U
#define AMDRDNA_GOP_SHAPE_PLACED	1U
#define AMDRDNA_GOP_SHAPE_SCALED	2U

/*
 * The interrupt the GPU signals the CPU with, once stage A2 set it up
 * (interrupt_kind).
 */
#define AMDRDNA_INTERRUPT_NONE		0U
#define AMDRDNA_INTERRUPT_MSI		1U
#define AMDRDNA_INTERRUPT_MSIX		2U

/*
 * What stage A5 left running (engine_flags): the scheduler's own queue,
 * the graphics queue, the first copy engine, and the graphics ring run
 * without the scheduler (amdrdna.kiq=0).
 */
#define AMDRDNA_RUNS_SCHEDULER	0x00000001U
#define AMDRDNA_RUNS_GRAPHICS		0x00000002U
#define AMDRDNA_RUNS_COPY		0x00000004U
#define AMDRDNA_RUNS_DIRECT		0x00000008U

/*
 * The GPU device's state (device_flags): registered with the GPU core
 * (/dev/gpuN published), a renderer libvulkan opens (an executor is
 * bound), and failed (it takes no new work).
 */
#define AMDRDNA_DEVICE_REGISTERED	0x00000001U
#define AMDRDNA_DEVICE_RENDERER		0x00000002U
#define AMDRDNA_DEVICE_FAULTED		0x00000004U

/* The output kind the firmware lit, as the display engine names it. */
#define AMDRDNA_OUTPUT_NONE		0U
#define AMDRDNA_OUTPUT_DP		1U
#define AMDRDNA_OUTPUT_DVI		2U
#define AMDRDNA_OUTPUT_HDMI		3U
#define AMDRDNA_OUTPUT_DP_MST		5U

/*
 * The driver's state and what its stages read (AMDRDNA_DIAGNOSE_STATUS).
 *
 * An IP
 * version is major << 16 | minor << 8 | step.  A value a stage has not
 * read yet is zero.  From stage A2 on: where the system-memory table's
 * reach starts in the GPU's address space and its size, the free VRAM of
 * the CPU's and the GPU's pools, the interrupt kind, the messages and the
 * page faults counted so far, and the display's underflow bits read at
 * the end of A2 (the plane's three bits, and 8 for the timing generator).
 * From stage A3t on: how memory training ended (AMDRDNA_TRAINING_*), why
 * it was skipped and how long it took.  From A3 on: the trusted region's
 * size, the programs the security processor loaded, the secure OS's
 * version and the replies that reported an error.  From A4 on: the
 * system-management unit's firmware and interface versions, the fan's
 * mode (AMDRDNA_FAN_*), its static duty and driven duty (percent) and its
 * speed (rpm), the temperature and the highest reading (degrees Celsius),
 * the limit past which the driver faults the GPU, and the thermal alerts
 * taken; and the interrupt entries read and those no block took.  From
 * A5 on: what runs (AMDRDNA_RUNS_*) and the compute queues attached,
 * the graphics engine's address configuration, the render back ends on
 * (one bit each), the compute units on, the level-2 cache channels off
 * (one bit each), the fences the checks waited for, the end-of-pipe
 * interrupts of the graphics and compute rings and the copy engine's
 * traps, the bytes the copy check moved, and the output words the
 * dispatch check compared.  From A5s on: the GPU address spaces open
 * (bits 15:0) and kept because their work never ended (bits 31:16), the
 * drops of the hub contexts' translations, and the GPU device's state
 * (AMDRDNA_DEVICE_*).  From A4 on: the memory clock and the display's
 * return clock the firmware last reported (MHz, 0 when it reports none).
 * From A6 on, read as the call is answered: the display hub's memory
 * state control word, its memory state handshake (its test debug word
 * 0xB: bit 30 the arbiter allows the change, bit 31 the firmware asks for
 * it), and the plane's underflow bits and the timing generator's
 * underflow as they stand, the memory clock change's watermark of set A
 * (reference clock cycles) and the test debug index as read back.
 */
struct amdrdna_status {
	uint32_t version;
	uint32_t size;
	uint32_t stage;
	uint32_t target;
	uint32_t running;
	uint32_t failed_stage;
	int32_t failed_error;
	uint32_t flags;
	uint32_t pci_id;
	uint32_t pci_revision;
	uint32_t pci_subsystem;
	uint32_t graphics_version;
	uint32_t display_version;
	uint32_t security_version;
	uint32_t power_version;
	uint32_t vram_mib;
	uint32_t bar0_mib;
	uint32_t rebar_capability;
	uint32_t rebar_control;
	uint32_t power_firmware_state;
	uint32_t secure_os_word;
	uint32_t boot_mailbox;
	uint32_t engine_status;
	uint32_t bios_scratch_7;
	uint64_t frame_buffer_base;
	uint64_t gop_vram_offset;
	uint32_t gop_timing;
	uint32_t gop_plane;
	uint32_t gop_output;
	uint32_t gop_output_kind;
	uint32_t gop_width;
	uint32_t gop_height;
	uint32_t gop_pitch_word;
	uint32_t gop_format;
	uint32_t vbios_source;
	uint32_t vbios_length;
	uint32_t firmware_capability_bits;
	uint32_t firmware_table_version;
	uint32_t power_table_id;
	uint32_t shutdown_temperature;
	uint32_t cpu_window_mib;
	uint32_t gop_source_width;
	uint32_t gop_source_height;
	uint32_t gop_shape;
	uint64_t gart_base;
	uint32_t gart_mib;
	uint32_t cpu_pool_free_mib;
	uint32_t gpu_pool_free_mib;
	uint32_t interrupt_kind;
	uint32_t interrupt_signals;
	uint32_t vm_faults;
	uint32_t display_underflow;
	uint32_t training_result;
	uint32_t training_reason;
	uint32_t training_ms;
	uint32_t region_kib;
	uint32_t programs_loaded;
	uint32_t secure_os_version;
	uint32_t security_warnings;
	uint32_t smu_version;
	uint32_t smu_interface;
	uint32_t fan_mode;
	uint32_t fan_duty;
	uint32_t fan_rpm;
	uint32_t fan_driven;
	uint32_t temperature;
	uint32_t temperature_highest;
	uint32_t temperature_limit;
	uint32_t thermal_alerts;
	uint32_t interrupt_entries;
	uint32_t interrupt_unknown;
	uint32_t engine_flags;
	uint32_t compute_queues;
	uint32_t address_config;
	uint32_t backends;
	uint32_t compute_units;
	uint32_t channels_off;
	uint32_t fences;
	uint32_t end_of_pipe_interrupts;
	uint32_t copy_traps;
	uint32_t copied_bytes;
	uint32_t dispatch_words;
	uint32_t spaces;
	uint32_t context_flushes;
	uint32_t device_flags;
	uint32_t memory_clock;
	uint32_t return_clock;
	uint32_t memory_state;
	uint32_t memory_handshake;
	uint32_t underflow_now;
	uint32_t timing_underflow_now;
	uint32_t switch_mark;
	uint32_t handshake_index;
};

/*
 * Acts as if the monitor went away (level 1) or came (level 2) on the
 * firmware's output (AMDRDNA_DIAGNOSE_HOTPLUG, the level its argument):
 * the output turned off, or the monitor's EDID read again and the output
 * lit again.  The driver answers when it has acted, with the result (0 or
 * an errno), whether the output is dark and the display's mode.  A level
 * the driver does not know, a GPU without the firmware's output and a
 * driver that stopped refuse the request itself (EINVAL, ENOENT, ENODEV).
 */
#define AMDRDNA_HOTPLUG_GONE		1U
#define AMDRDNA_HOTPLUG_THERE		2U

struct amdrdna_hotplug {
	uint32_t version;
	uint32_t size;
	uint32_t level;
	int32_t result;
	uint32_t dark;
	uint32_t width;
	uint32_t height;
	uint32_t refresh_millihz;
};

/* What the display's flags word says (amdrdna_display_state.flags). */
#define AMDRDNA_DISPLAY_TAKEN		0x00000001U
#define AMDRDNA_DISPLAY_CURSOR_READY	0x00000002U
#define AMDRDNA_DISPLAY_INTERRUPTS_ON	0x00000004U
#define AMDRDNA_DISPLAY_CONNECTED	0x00000008U
#define AMDRDNA_DISPLAY_BLANKED		0x00000010U
#define AMDRDNA_DISPLAY_REGISTERED	0x00000020U
#define AMDRDNA_DISPLAY_LEASED		0x00000040U
#define AMDRDNA_DISPLAY_BROKEN		0x00000080U
#define AMDRDNA_DISPLAY_CURSOR_SHOWN	0x00000100U
#define AMDRDNA_DISPLAY_HOTPLUG		0x00000200U

/*
 * The display the driver took over from the firmware, as its registers
 * and its counts are now (AMDRDNA_DIAGNOSE_DISPLAY).
 *
 * timing, plane and hotplug are the firmware's timing generator, plane
 * and hot-plug line; width, height, stride and refresh_millihz the
 * picture every frame has.  The addresses are the GPU's: the console's
 * surface, the one last asked for, the one the plane reads (the earliest
 * in use) and the one its address registers hold, and the cursor's.  The
 * register words follow as read; the counts are the interrupts taken,
 * the flips asked for, the refreshes counted, the topology sequence, the
 * presentations and the leases.
 */
struct amdrdna_display_state {
	uint32_t version;
	uint32_t size;
	uint32_t flags;
	uint32_t timing;
	uint32_t plane;
	uint32_t hotplug;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t refresh_millihz;
	uint64_t console_address;
	uint64_t requested_address;
	uint64_t shown_address;
	uint64_t plane_address;
	uint64_t cursor_address;
	uint32_t frame_count;
	uint32_t position;
	uint32_t flip_control;
	uint32_t flip_interrupt;
	uint32_t sync_status;
	uint32_t plane_control;
	uint32_t combiner_input;
	uint32_t cursor_control;
	uint32_t cursor_size;
	uint32_t cursor_position;
	uint32_t cursor_hot_spot;
	uint32_t pixel_cursor;
	uint32_t hotplug_status;
	uint32_t hotplug_control;
	uint64_t frame_signals;
	uint64_t flip_signals;
	uint64_t hotplug_signals;
	uint64_t flips;
	uint64_t refresh_count;
	uint64_t topology;
	uint64_t presents;
	uint64_t leases;
	uint32_t reserved[4];
};

/*
 * The head of the picture the display shows now, the surface the plane
 * reads (AMDRDNA_DIAGNOSE_SCREEN): its width, height and stride, its
 * bytes (stride by height) and the GPU address read.  The report goes on
 * with the picture's rows from the top, each stride bytes, 32-bit pixels
 * B, G, R, A in memory; a read by offset may meet a later picture.
 */
struct amdrdna_screen {
	uint32_t version;
	uint32_t size;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t bytes;
	uint64_t address;
	uint32_t reserved[8];
};

_Static_assert(sizeof(struct amdrdna_status) == 384U, "amdrdna status ABI size");
_Static_assert(offsetof(struct amdrdna_status, frame_buffer_base) == 96U, "amdrdna status ABI alignment");
_Static_assert(offsetof(struct amdrdna_status, gart_base) == 184U, "amdrdna status ABI alignment");
_Static_assert(sizeof(struct amdrdna_display_state) == 216U, "amdrdna display ABI size");
_Static_assert(offsetof(struct amdrdna_display_state, frame_signals) == 136U, "amdrdna display ABI alignment");
_Static_assert(sizeof(struct amdrdna_screen) == 64U, "amdrdna screen ABI size");
_Static_assert(sizeof(struct amdrdna_hotplug) == 32U, "amdrdna hotplug ABI size");

#endif
