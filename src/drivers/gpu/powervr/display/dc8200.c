/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The VisionFive 2's display controller as the loader left it.
 *
 * The loader lights the HDMI output and scans its framebuffer through one
 * of the controller's primary planes.  At attach the driver reads the
 * controller's identity and both primary planes, decides whether the
 * plane on the HDMI panel shows exactly the loader's framebuffer, and
 * keeps what it read as the place to return the screen to.  It also
 * watches the panel's scan position and the plane's commit bit for a
 * moment, to learn how they behave.  Nothing is written at attach: the
 * screen stays the loader's, drawn by the copying display node.
 *
 * When the screen can be taken over, the display node then turns on the
 * panel's vertical-blank interrupt, which counts frames for the rest of
 * the system's life, and commits a program's buffer to the same plane at
 * the loader's extent and row pitch, or the loader's framebuffer back.
 * Every commit holds the shadowed registers back while it writes them,
 * under a lock that also keeps a fatal stop's return of the plane from
 * being overwritten.
 *
 * The outcome of the attach is one line of the boot log, which the screen
 * shows.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/powervr/display/dc8200-adopt.h"
#include "drivers/gpu/powervr/display/dc8200-hdmi.h"
#include "drivers/gpu/powervr/display/dc8200-mode.h"
#include "drivers/gpu/powervr/powervr.h"
#include <kern/atomic.h>
#include <kern/boot.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/platform.h>
#include <kern/pmem.h>
#include <kern/sched.h>
#include <kern/waitq.h>
#include <uapi/errno.h>

/*
 * The controller's identity registers, from the control window's base.
 *
 * Values referenced:
 * Linux torvalds/linux master drivers/gpu/drm/verisilicon/vs_dc_top_regs.h
 * (the chip model at 0x20, the revision at 0x24, the customer id at 0x30).
 */
#define DC_MODEL		0x20U
#define DC_REVISION		0x24U
#define DC_CUSTOMER		0x30U

/*
 * The interrupt registers in the control window: the pending panels,
 * cleared by reading them, and the enabled ones, one bit per panel.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/verisilicon/vs_dc_hw.h (AQ_INTR_ACKNOWLEDGE 0x10,
 * AQ_INTR_ENBL 0x14), vs_dc_hw.c (dc_hw_enable_interrupt: the enable bits
 * are the CRTC mask) and vs_crtc.c (vs_crtc_enable_vblank: one bit per
 * CRTC in its order).
 */
#define DC_INTERRUPT_PENDING	0x10U
#define DC_INTERRUPT_ENABLE	0x14U

/*
 * Where the display window starts among the controller's offsets, and the
 * registers of primary plane 0 and panel 0; plane and panel 1 are 4 bytes
 * further.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/verisilicon/vs_dc_hw.h (the register base 0x800, the
 * framebuffer's address 0x1400, stride 0x1408, configuration 0x1518 and
 * its extension 0x1cc0, size 0x1810, corners 0x24d8 and 0x24e0,
 * background 0x1528, blending 0x2510; the panel's configuration 0x1418,
 * its extension 0x2518 and the current location 0x1450) and vs_dc_hw.c
 * (the second plane's registers 4 bytes on).
 */
#define DC_DISPLAY_BASE		0x800U
#define DC_PLANE_ADDRESS	0x1400U
#define DC_PLANE_STRIDE		0x1408U
#define DC_PLANE_CONFIG		0x1518U
#define DC_PLANE_EXTRA		0x1cc0U
#define DC_PLANE_SIZE		0x1810U
#define DC_PLANE_TOP_LEFT	0x24d8U
#define DC_PLANE_BOTTOM_RIGHT	0x24e0U
#define DC_PLANE_BACKGROUND	0x1528U
#define DC_PLANE_BLEND		0x2510U
#define DC_PANEL_CONFIG		0x1418U
#define DC_PANEL_EXTRA		0x2518U
#define DC_PANEL_LOCATION	0x1450U
#define DC_UNIT_STEP		4U

/* The JH7110 PLIC, which the vblank report reads one line of. */
#define DC_PLIC_BASE		0x0c000000ULL
#define DC_PLIC_SIZE		0x400000U
#define DC_PLIC_CONTEXTS	10U

/*
 * The cursor plane of the first panel, and the second panel's offset from
 * it; the configuration's fields: the format (2, ARGB8888) in bits 0 to
 * 2, bits 2 and 3 set when shown, the size at bit 5 (1, 64 x 64) and the
 * hotspot's y at 8 and x at 16.  Hiding clears the format, keeps bit 3.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel
 * drivers/gpu/drm/verisilicon/vs_dc_hw.h (DC_CURSOR_CONFIG 0x1468,
 * DC_CURSOR_ADDRESS 0x146C, DC_CURSOR_LOCATION 0x1470, DC_CURSOR_OFFSET
 * 0x1080, CURSOR_SIZE_64X64 1) and vs_dc_hw.c (dc_hw_commit: the address,
 * the location x | y << 16, then the configuration).
 */
#define DC_CURSOR_CONFIG	0x1468U
#define DC_CURSOR_ADDRESS	0x146cU
#define DC_CURSOR_LOCATION	0x1470U
#define DC_CURSOR_PANEL_STEP	0x1080U
#define DC_CURSOR_SHOWN		((1U << 3) | (1U << 2) | 0x2U | (1U << 5))
#define DC_CURSOR_CLEAR		((0xffU << 16) | (0xffU << 8) | (0x7U << 5) | 0x1fU)
#define DC_CURSOR_HIDDEN_SET	(1U << 3)
#define DC_CURSOR_HIDDEN_CLEAR	0x3U
#define DC_CURSOR_HOT_MAX	63

/*
 * The panel registers the driver sets when it lights the output itself:
 * the dither, the DPI and DP output formats, the four timing words, and
 * the word that starts the panels.
 *
 * Values referenced:
 * Linux torvalds/linux master drivers/gpu/drm/verisilicon/vs_crtc_regs.h
 * (VSDC_DISP_DITHER_CONFIG 0x1410, VSDC_DISP_HSIZE 0x1430,
 * VSDC_DISP_HSYNC 0x1438, VSDC_DISP_VSIZE 0x1440, VSDC_DISP_VSYNC 0x1448)
 * and vs_bridge_regs.h (VSDC_DISP_DPI_CONFIG 0x14b8, its RGB888 5;
 * VSDC_DISP_PANEL_START 0x1ccc, a panel's running bit n and the
 * multi-display sync bit 3; VSDC_DISP_DP_CONFIG 0x1cd0, its RGB888 2 and
 * enable bit 3; the panel configuration's data-enable bit 0 and its
 * polarity bit 1, data bit 4 and polarity bit 5, clock bit 8 and polarity
 * bit 9, running bit 12, YUV bit 16).
 */
#define DC_PANEL_DITHER		0x1410U
#define DC_PANEL_HORIZONTAL	0x1430U
#define DC_PANEL_HSYNC		0x1438U
#define DC_PANEL_VERTICAL	0x1440U
#define DC_PANEL_VSYNC		0x1448U
#define DC_PANEL_DPI		0x14b8U
#define DC_PANEL_DPI_RGB888	5U
#define DC_PANEL_START		0x1cccU
#define DC_PANEL_START_SYNC	(1U << 3)
#define DC_PANEL_DP		0x1cd0U
#define DC_PANEL_DP_RGB888	2U
#define DC_PANEL_DP_ENABLE	(1U << 3)
#define DC_PANEL_CONFIG_DE		(1U << 0)
#define DC_PANEL_CONFIG_DE_LOW		(1U << 1)
#define DC_PANEL_CONFIG_DATA		(1U << 4)
#define DC_PANEL_CONFIG_DATA_LOW	(1U << 5)
#define DC_PANEL_CONFIG_CLOCK		(1U << 8)
#define DC_PANEL_CONFIG_CLOCK_LOW	(1U << 9)
#define DC_PANEL_CONFIG_RUNNING		(1U << 12)
#define DC_PANEL_CONFIG_YUV		(1U << 16)

/*
 * The plane bits the driver clears when it sets a plane up itself: the
 * clear colour, the rotation, the YUV space, the tile mode, the scaler,
 * the UV swizzle, the region of interest, the compression and the depth
 * order; and the blend word that turns blending off.
 *
 * Values referenced:
 * Linux torvalds/linux master
 * drivers/gpu/drm/verisilicon/vs_primary_plane_regs.h
 * (VSDC_FB_CONFIG_CLEAR_EN bit 8, ROT bits 11-13, YUV_SPACE bits 14-16,
 * TILE_MODE bits 17-21, SCALE_EN bit 22, UV_SWIZZLE_EN bit 25;
 * VSDC_FB_CONFIG_EX_ZPOS bits 16-18; VSDC_FB_BLEND_CONFIG_BLEND_DISABLE
 * bit 1); StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/verisilicon/vs_dc_hw.c (plane_commit: the region of
 * interest at bit 0 of the extension).
 */
#define DC_CONFIG_OWN_CLEAR	((1U << 8) | (0x7U << 11) | (0x7U << 14) | (0x1fU << 17) | (1U << 22) | (1U << 25))
#define DC_EXTRA_OWN_CLEAR	((1U << 0) | (1U << 1) | (0x7U << 16) | (1U << 19))
#define DC_BLEND_DISABLE	(1U << 1)

/*
 * The display subsystem's control block and the two bits the vendor's
 * driver sets before it starts a panel; their meaning is not documented
 * to the driver.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * arch/riscv/boot/dts/starfive/jh7110.dtsi (dssctrl@295b0000, 0x90
 * bytes) and drivers/gpu/drm/verisilicon/vs_dc.c (vs_dc_enable: bit 20
 * of 0x4 and bit 3 of 0x8).
 */
#define DC_SUBSYSTEM_FIRST	0x4U
#define DC_SUBSYSTEM_FIRST_BIT	(1U << 20)
#define DC_SUBSYSTEM_SECOND	0x8U
#define DC_SUBSYSTEM_SECOND_BIT	(1U << 3)

/* The bytes of one pixel, and the alignment of a framebuffer the driver allocates. */
#define DC_PIXEL_BYTES		4U
#define DC_OWN_ALIGNMENT	0x10000U

/* The highest address the controller can scan: its address registers are 32 bits. */
#define DC_ADDRESS_LIMIT	0xffffffffULL

/*
 * The panel bit that holds back the shadowed registers while it is set.
 *
 * Value referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/verisilicon/vs_dc_hw.c (dc_hw_enable_shadow_register:
 * holding sets bit 0 of the panel's extension and clears the planes'
 * commit bit; releasing does the reverse).
 */
#define DC_PANEL_EXTRA_HOLD	(1U << 0)

/*
 * The pixel layout of a program's frame on the plane: 32 bits a pixel
 * with the alpha byte ignored, and the order of its colour bytes.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/verisilicon/vs_dc_hw.h (FORMAT_X8R8G8B8 5; the swizzles
 * ARGB 0 and ABGR 2) and vs_dc.c (XRGB8888, bytes B G R X, takes ARGB;
 * XBGR8888, bytes R G B X, takes ABGR).
 */
#define DC_CONFIG_FORMAT_SHIFT	26U
#define DC_CONFIG_FORMAT_MASK	0x3fU
#define DC_CONFIG_FORMAT_XRGB	5U
#define DC_CONFIG_SWIZZLE_SHIFT	23U
#define DC_CONFIG_SWIZZLE_MASK	0x3U
#define DC_CONFIG_SWIZZLE_ARGB	0U
#define DC_CONFIG_SWIZZLE_ABGR	2U

/*
 * How long the vertical blanks may stop before a wait for them gives up
 * and the output counts as lost: six frames at 60 Hz.
 */
#define DC_STALL_TICKS		KERN_MS_TO_TICKS(100U)

/*
 * How far before the next blank a commit is still taken at that blank, in
 * microseconds, and the shortest and longest frames the measured period
 * may be.  A commit written earlier in the frame counts from the blank
 * before (drv_powervr_dc_commit), so it is taken one blank sooner: the
 * presents of the desktop's frames were held to one every two blanks.
 */
#define DC_EARLY_MARGIN_US	3000U
#define DC_PERIOD_LEAST_US	8000U
#define DC_PERIOD_MOST_US	50000U

/* How many vertical blanks' scan positions the interrupt keeps for a test. */
#define DC_BLANK_RING		64U

/*
 * How many times a fatal stop tries the commit lock before it writes
 * without it.
 */
#define DC_FATAL_LOCK_TRIES	4096U

/*
 * The panel the HDMI output is driven from.
 *
 * The vendor's driver feeds display 0's pixel clock from the HDMI
 * transmitter, so the HDMI panel is taken to be panel 0; the attach's
 * record shows whether the lit plane agrees.
 *
 * Value referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/verisilicon/vs_dc.c (display 0's pixel clock parent).
 */
#define DC_HDMI_PANEL		0U

/* How long the scan position is watched: a little over two frames at 60 Hz. */
#define DC_WATCH_MICROSECONDS	40000U

/* How many reads stand in for the watch when the kernel's counter is not running. */
#define DC_WATCH_READS		200000U

/*
 * What the panel's scan position and the plane's commit bit did while they
 * were watched.
 *
 * Wraps counts the times the position went down, which a counter of the
 * scan line does once a frame.
 */
struct dc_watch {
	uint32_t first;
	uint32_t lowest;
	uint32_t highest;
	uint32_t wraps;
	uint32_t reads;
	uint32_t commit_seen;
};

/*
 * The controller as the attach found it.
 *
 * Filled once by drv_powervr_dc_probe() on the boot CPU and not changed
 * afterwards; the plane registers it holds are the loader's screen, the
 * place a later takeover returns the screen to.  Probed is zero until
 * then.
 */
struct dc_adoption {
	int probed;
	volatile uint8_t *control;
	volatile uint8_t *display;
	uint32_t interrupt;
	struct powervr_dc_state state;
	struct powervr_dc_screen screen;
	enum powervr_dc_verdict verdict;
	unsigned plane;
	uint32_t panel_config;
	uint32_t panel_extra;
	struct dc_watch watch;
	int own;
};

/*
 * The vertical-blank count of the HDMI panel.
 *
 * Started by drv_powervr_dc_vblank_start() and kept for the life of the
 * system.  The interrupt alone advances sequence and fills the ring; both
 * change under lock, and sequence is also read without it through an
 * acquire load.  Last_tick is the scheduler tick of the newest blank,
 * last_counter the fine counter's reading at it and period_counts the
 * counter's counts between the last two blanks (0 until two came in a
 * row), so a commit can tell how far into the frame it is written.  The
 * ring keeps the panel's raw scan position as each interrupt read it, so
 * a test sees where in the frame the interrupt comes.  Others counts
 * interrupts for the other panel only, which do not advance the count.
 */
struct dc_blanks {
	struct spinlock lock;
	struct wait_queue wake;
	int started;
	int irq;
	uint64_t sequence;
	uint64_t last_tick;
	uint64_t last_counter;
	uint64_t period_counts;
	uint64_t others;
	uint32_t lowest;
	uint32_t highest;
	uint32_t next;
	uint32_t ring[DC_BLANK_RING];
};

/* The board's one controller, as drv_powervr_dc_probe() found it. */
static struct dc_adoption board_adoption;

/* The HDMI panel's vertical blanks, counted once started. */
static struct dc_blanks board_blanks;

/*
 * Serializes every write of the planes' registers: a commit's whole
 * sequence and a fatal stop's.  A leaf lock, held for a few register
 * writes with interrupts disabled.
 */
static struct spinlock dc_commit_lock = {
	{ 0 }, LOCK_RANK_DEVICE, "dc8200 commit", 0, 0
};

/*
 * Set once by a fatal stop and never cleared: from then on no commit
 * writes the planes.  Read under dc_commit_lock by a commit.
 */
static volatile unsigned dc_fatal;

/*
 * The last commit's place in its frame, for the display's flip report: how
 * far after the newest blank it was written (microseconds, the fine
 * counter), the period the blanks measured then, and whether it counted as
 * early (taken at the next blank).  Written under dc_commit_lock by a
 * commit, read by its caller under the display's mutex after it.
 */
static struct drv_powervr_dc_commit_note dc_last_commit;

static uint32_t read_control(const struct dc_adoption *adoption, uint32_t offset);
static uint32_t read_display(const struct dc_adoption *adoption, uint32_t offset);
static void write_display(const struct dc_adoption *adoption, uint32_t offset, uint32_t value);
static int commit_held_here(void);
static void read_state(struct dc_adoption *adoption);
static void watch_scan(struct dc_adoption *adoption, uint32_t extra_offset);
static void log_outcome(const struct dc_adoption *adoption);
static void write_control(const struct dc_adoption *adoption, uint32_t offset, uint32_t value);
static void write_plane(const struct dc_adoption *adoption, const struct powervr_dc_plane *plane);
static void blank_interrupt(int irq, kern_irq_ack_t acknowledge, void *argument);
static int commit_early(const struct dc_blanks *blanks, uint64_t sequence);
static void dc_phase(const struct dc_blanks *blanks, uint64_t *since_us, uint64_t *period_us);
static const struct powervr_dc_mode *choose_mode(void);
static int own_screen(const struct powervr_dc_mode *mode, struct powervr_dc_screen *screen, uint32_t *format);
static void draw_bars(volatile uint32_t *pixels, const struct powervr_dc_screen *screen);
static void plic_report(int irq, int complete);
static void start_panel(const struct dc_adoption *adoption, const struct powervr_dc_mode *mode);

/*
 * Reads the display controller the loader left on and decides whether its
 * screen can be taken over, without changing any of its registers.
 */
int
drv_powervr_dc_probe(
	const struct drv_powervr_dc_place *place)
{
	struct dc_adoption *adoption;
	struct kern_boot_framebuffer framebuffer;
	void *mapped;
	unsigned plane;
	int error;

	/* Refuses a missing place, a second probe, or a display window too small for the planes. */
	adoption = &board_adoption;
	if (place == NULL)
		return EINVAL;
	if (adoption->probed)
		return EALREADY;
	if (place->display_size < DC_PANEL_EXTRA + DC_UNIT_STEP * 2U - DC_DISPLAY_BASE)
		return EINVAL;
	if (place->control_size < DC_CUSTOMER + DC_UNIT_STEP)
		return EINVAL;

	/* Reads the loader's screen; without one the controller is not lit and is not touched. */
	error = kern_boot_framebuffer("riscv64.framebuffer", &framebuffer);
	if (error != 0) {
		kern_logf("dc8200: no-go (no boot framebuffer, %d)\n", error);
		return ENOENT;
	}

	adoption->screen.physical = framebuffer.physical_base;
	adoption->screen.pitch = framebuffer.pitch;
	adoption->screen.width = framebuffer.width;
	adoption->screen.height = framebuffer.height;

	/* Maps the identity registers. */
	error = kern_device_map(place->control, (size_t)place->control_size, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0) {
		kern_logf("dc8200: no-go (control map failed, %d)\n", error);
		return error;
	}

	adoption->control = mapped;

	/* Maps the planes' and panels' registers. */
	error = kern_device_map(place->display, (size_t)place->display_size, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0) {
		(void)kern_device_unmap((void *)adoption->control, (size_t)place->control_size);
		adoption->control = NULL;
		kern_logf("dc8200: no-go (display map failed, %d)\n", error);
		return error;
	}

	adoption->display = mapped;
	adoption->interrupt = place->interrupt;

	/* Reads the identity and both planes as the loader left them. */
	read_state(adoption);

	/* Decides whether the HDMI panel's plane is exactly the loader's framebuffer. */
	adoption->verdict = powervr_dc_judge(&adoption->state, &adoption->screen, DC_HDMI_PANEL, &plane);
	adoption->plane = plane;

	/* Watches the scan position, and the commit bit of the lit plane (plane 0 when none is lit). */
	if (plane >= POWERVR_DC_PLANES)
		plane = 0;
	watch_scan(adoption, DC_PLANE_EXTRA + DC_UNIT_STEP * plane);

	/* Keeps the record for the takeover and writes it to the boot log. */
	adoption->probed = 1;
	log_outcome(adoption);

	/* Succeeded: the controller is read and the decision recorded, go or not. */
	return 0;
}

/*
 * Lights the HDMI output at a mode the driver chooses and shows a
 * framebuffer on it, for a boot whose loader left no screen or whose line
 * names display.mode=.
 */
int
drv_powervr_dc_own_mode(
	const struct drv_powervr_dc_own_place *place)
{
	struct dc_adoption *adoption;
	const struct powervr_dc_mode *mode;
	struct powervr_dc_plane *plane;
	struct powervr_dc_screen screen;
	volatile uint8_t *hdmi;
	volatile uint8_t *subsystem;
	void *mapped;
	unsigned long enabled;
	uint32_t format;
	uint32_t swizzle;
	uint32_t value;
	int error;

	/* Refuses a missing place, a controller already read, or windows too small. */
	adoption = &board_adoption;
	if (place == NULL)
		return EINVAL;
	if (adoption->probed)
		return EALREADY;
	if (place->controller.display_size < DC_PANEL_EXTRA + DC_UNIT_STEP * 2U - DC_DISPLAY_BASE)
		return EINVAL;
	if (place->controller.control_size < DC_CUSTOMER + DC_UNIT_STEP)
		return EINVAL;

	/* Picks the mode and the memory the plane will show. */
	mode = choose_mode();
	error = own_screen(mode, &screen, &format);
	if (error != 0) {
		kern_logf("dc8200: own mode: no framebuffer (%d)\n", error);
		return error;
	}

	/* Maps the identity, the display and the HDMI transmitter's registers. */
	error = kern_device_map(place->controller.control, (size_t)place->controller.control_size, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0) {
		kern_logf("dc8200: own mode: control map failed (%d)\n", error);
		return error;
	}
	adoption->control = mapped;

	error = kern_device_map(place->controller.display, (size_t)place->controller.display_size, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0) {
		kern_logf("dc8200: own mode: display map failed (%d)\n", error);
		return error;
	}
	adoption->display = mapped;
	adoption->interrupt = place->controller.interrupt;

	error = kern_device_map(place->hdmi, (size_t)place->hdmi_size, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0) {
		kern_logf("dc8200: own mode: HDMI map failed (%d)\n", error);
		return error;
	}
	hdmi = mapped;

	/* Refuses a controller whose registers the driver does not know. */
	read_state(adoption);
	if (adoption->state.model != POWERVR_DC_MODEL) {
		kern_logf("dc8200: own mode: unknown model %x\n", adoption->state.model);
		return ENODEV;
	}

	/* Sets the two subsystem bits the vendor's driver sets, when the board powered the output up itself. */
	if (place->subsystem != 0) {
		error = kern_device_map(place->subsystem, (size_t)place->subsystem_size, KERN_DEVICE_UNCACHED, &mapped);
		if (error != 0) {
			kern_logf("dc8200: own mode: subsystem map failed (%d)\n", error);
			return error;
		}
		subsystem = mapped;
		value = kern_mmio_read32(subsystem + DC_SUBSYSTEM_FIRST);
		kern_mmio_write32(subsystem + DC_SUBSYSTEM_FIRST, value | DC_SUBSYSTEM_FIRST_BIT);
		value = kern_mmio_read32(subsystem + DC_SUBSYSTEM_SECOND);
		kern_mmio_write32(subsystem + DC_SUBSYSTEM_SECOND, value | DC_SUBSYSTEM_SECOND_BIT);
	}

	/* Starts the HDMI transmitter first: its PHY makes the panel's pixel clock. */
	error = powervr_dc_hdmi_start(hdmi, mode);
	if (error != 0 && error != ETIMEDOUT) {
		kern_logf("dc8200: own mode: HDMI not set (%d)\n", error);
		return error;
	}
	if (error == ETIMEDOUT)
		kern_logf("dc8200: own mode: HDMI PLL not locked\n");

	/* Sets the HDMI panel's output and timing and starts it. */
	start_panel(adoption, mode);

	/* The plane: the framebuffer's first pixel and row pitch, 32-bit pixels without alpha, at the mode's size. */
	swizzle = DC_CONFIG_SWIZZLE_ARGB;
	if (format == KERN_BOOT_FRAMEBUFFER_RGBX8888)
		swizzle = DC_CONFIG_SWIZZLE_ABGR;
	adoption->plane = 0;
	plane = &adoption->state.planes[0];
	plane->address = (uint32_t)screen.physical;
	plane->stride = screen.pitch;
	plane->config = plane->config & ~DC_CONFIG_OWN_CLEAR;
	plane->config &= ~((DC_CONFIG_FORMAT_MASK << DC_CONFIG_FORMAT_SHIFT) | (DC_CONFIG_SWIZZLE_MASK << DC_CONFIG_SWIZZLE_SHIFT));
	plane->config |= (DC_CONFIG_FORMAT_XRGB << DC_CONFIG_FORMAT_SHIFT) | (swizzle << DC_CONFIG_SWIZZLE_SHIFT);
	plane->extra = plane->extra & ~(DC_EXTRA_OWN_CLEAR | POWERVR_DC_EXTRA_COMMIT);
	plane->extra |= POWERVR_DC_EXTRA_ENABLED | (DC_HDMI_PANEL << POWERVR_DC_EXTRA_PANEL_SHIFT);
	plane->size = mode->width | (mode->height << POWERVR_DC_SIZE_HEIGHT_SHIFT);
	plane->top_left = 0;
	plane->bottom_right = mode->width | (mode->height << POWERVR_DC_SIZE_HEIGHT_SHIFT);
	plane->background = 0;
	plane->blend = DC_BLEND_DISABLE;

	/* The place a takeover and a fatal stop return the screen to, for the life of the system. */
	adoption->screen = screen;
	adoption->screen.width = mode->width;
	adoption->screen.height = mode->height;
	adoption->verdict = POWERVR_DC_GO;
	adoption->own = 1;

	/* Commits the plane with the shadowed registers held back. */
	enabled = spin_lock_irqsave(&dc_commit_lock);
	write_plane(adoption, plane);
	spin_unlock_irqrestore(&dc_commit_lock, enabled);

	/* Records the outcome, which the screen shows when the loader's console draws on it. */
	adoption->probed = 1;
	if (POWERVR_DEBUG)
		kern_logf("dc8200: own mode %ux%u@%u clock %u kHz model %x rev %x cid %x fb %llx/%u format %u\n",
			  mode->width,
			  mode->height,
			  mode->refresh,
			  mode->clock_khz,
			  adoption->state.model,
			  adoption->state.revision,
			  adoption->state.customer,
			  (unsigned long long)screen.physical,
			  screen.pitch,
			  format);

	/* Succeeded: the output runs at the mode and shows the framebuffer. */
	return 0;
}

/*
 * Reports whether the driver lights the output itself rather than take
 * the loader's screen over.
 */
int
drv_powervr_dc_own_wanted(
	void)
{
	struct kern_boot_framebuffer framebuffer;
	const char *line;
	uint32_t width;
	uint32_t height;
	uint32_t refresh;
	int error;

	/* A loader that left no screen leaves the output to the driver. */
	error = kern_boot_framebuffer("riscv64.framebuffer", &framebuffer);
	if (error != 0)
		return 1;

	/* A boot line that names a mode, readable or not, asks for the driver's own. */
	line = kern_boot_handoff("boot.command-line");
	error = powervr_dc_mode_from_line(line, &width, &height, &refresh);
	if (error != ENOENT)
		return 1;

	/* Otherwise the loader's screen is taken over. */
	return 0;
}

/*
 * Returns the screen to the loader's plane for a fatal stop.
 */
void
drv_powervr_dc_restore_fatal(
	void)
{
	const struct dc_adoption *adoption;
	const struct powervr_dc_plane *plane;
	unsigned tries;
	int locked;

	/* Stops every later commit before anything is written. */
	atomic_raw_store_release(&dc_fatal, 1U);

	/* Leaves a controller the probe did not find showing the loader's screen alone. */
	adoption = &board_adoption;
	if (!adoption->probed || adoption->verdict != POWERVR_DC_GO)
		return;
	if (adoption->plane >= POWERVR_DC_PLANES)
		return;

	/*
	 * Waits a bounded time for a commit on another CPU to finish.  A
	 * commit this CPU was in when it stopped is not waited for: trying a
	 * lock this CPU holds would trap.
	 */
	locked = 0;
	tries = 0;
	if (commit_held_here())
		tries = DC_FATAL_LOCK_TRIES;
	for (; tries < DC_FATAL_LOCK_TRIES; tries++) {
		/* Stops at the first try that takes the lock. */
		if (spin_trylock(&dc_commit_lock)) {
			locked = 1;
			break;
		}
	}

	/* Writes the plane as the probe read it and lets the panel take it at its next frame. */
	plane = &adoption->state.planes[adoption->plane];
	write_plane(adoption, plane);

	/* Releases the lock when it was taken. */
	if (locked)
		spin_unlock(&dc_commit_lock);
}

/*
 * Reports whether a fatal stop has taken the screen back.
 */
int
drv_powervr_dc_fatal(
	void)
{
	/* Pairs with the release store of the fatal stop. */
	return atomic_raw_load_acquire(&dc_fatal) != 0U;
}

/*
 * Turns on the HDMI panel's vertical-blank interrupt and starts counting
 * the panel's frames.
 */
int
drv_powervr_dc_vblank_start(
	void)
{
	struct dc_adoption *adoption;
	struct dc_blanks *blanks;
	uint32_t enabled_panels;
	int error;

	/* Only a controller whose screen can be taken over, and only once. */
	adoption = &board_adoption;
	blanks = &board_blanks;
	if (!adoption->probed || adoption->verdict != POWERVR_DC_GO)
		return ENODEV;
	if (blanks->started)
		return EALREADY;

	/* The count starts at zero, with no scan position seen yet. */
	spin_init(&blanks->lock, LOCK_RANK_DEVICE, "dc8200 blanks");
	waitq_init(&blanks->wake, "dc8200 vblank");
	blanks->irq = (int)adoption->interrupt;
	blanks->sequence = 0;
	blanks->last_tick = 0;
	blanks->others = 0;
	blanks->lowest = 0xffffffffU;
	blanks->highest = 0;
	blanks->next = 0;

	/* Drops whatever the loader left pending before the handler can see it. */
	(void)read_control(adoption, DC_INTERRUPT_PENDING);

	/* Installs the handler before the panel may raise the interrupt. */
	error = kern_irq_register(blanks->irq, blank_interrupt, blanks);
	if (error != 0) {
		kern_logf("dc8200: vblank interrupt %d not registered (%d)\n", blanks->irq, error);
		return error;
	}

	blanks->started = 1;

	/*
	 * Gives the source its priority: registering only routes it, and a
	 * source of priority 0 never reaches the hart (the controller's
	 * pending bit is set and the handler never runs).
	 */
	kern_irq_unmask(blanks->irq);
	if (POWERVR_DEBUG)
		kern_logf("dc8200: vblank interrupt %d unmasked\n", blanks->irq);
	plic_report(blanks->irq, 1);

	/* Lets the HDMI panel raise it, leaving the other panel's bit as it was. */
	enabled_panels = read_control(adoption, DC_INTERRUPT_ENABLE);
	enabled_panels |= 1U << DC_HDMI_PANEL;
	write_control(adoption, DC_INTERRUPT_ENABLE, enabled_panels);
	if (POWERVR_DEBUG)
		kern_logf("dc8200: vblank interrupt %d on, panel %u\n", blanks->irq, DC_HDMI_PANEL);

	/* Succeeded: the panel's frames are counted from now on. */
	return 0;
}

/*
 * Reports whether a program's buffer may be committed to the plane: the
 * screen could be taken over, frames are being counted and one was seen,
 * and no fatal stop took the screen back.
 */
int
drv_powervr_dc_scanout_ready(
	void)
{
	const struct dc_adoption *adoption;
	uint64_t sequence;
	int fatal;

	/* Only the controller whose screen can be taken over. */
	adoption = &board_adoption;
	if (!adoption->probed || adoption->verdict != POWERVR_DC_GO)
		return 0;

	/* A commit is judged by blanks, so the interrupt must have come at least once. */
	if (!board_blanks.started)
		return 0;

	sequence = atomic_u64_load_acquire(&board_blanks.sequence);
	if (sequence == 0)
		return 0;

	/* A fatal stop keeps the loader's plane for good. */
	fatal = drv_powervr_dc_fatal();
	if (fatal)
		return 0;

	return 1;
}

/*
 * Reports whether a frame of this extent and row pitch is scanned by the
 * plane as the loader set it up: only the address and the byte order of a
 * commit change, never the extent or the pitch.
 */
int
drv_powervr_dc_frame_fits(
	uint32_t width,
	uint32_t height,
	uint32_t stride)
{
	const struct dc_adoption *adoption;
	const struct powervr_dc_plane *plane;
	uint32_t plane_width;
	uint32_t plane_height;

	/* Only the controller whose screen can be taken over has a plane to match. */
	adoption = &board_adoption;
	if (!adoption->probed || adoption->verdict != POWERVR_DC_GO)
		return 0;

	plane = &adoption->state.planes[adoption->plane];

	/* The row pitch the plane reads with. */
	if (stride != plane->stride)
		return 0;

	/* The extent the plane shows. */
	plane_width = plane->size & POWERVR_DC_SIZE_WIDTH_MASK;
	plane_height = (plane->size >> POWERVR_DC_SIZE_HEIGHT_SHIFT) & POWERVR_DC_SIZE_WIDTH_MASK;
	if (width != plane_width)
		return 0;
	if (height != plane_height)
		return 0;

	return 1;
}

/*
 * Commits a program's frame, or the loader's framebuffer when frame is
 * NULL, to the plane, and reports the vertical-blank sequence read right
 * after the commit was written.
 */
int
drv_powervr_dc_commit(
	const struct drv_powervr_dc_frame *frame,
	uint64_t *base)
{
	const struct dc_adoption *adoption;
	struct powervr_dc_plane plane;
	unsigned long enabled;
	uint32_t format;
	uint32_t swizzle;
	int early;

	/* Only the controller whose screen can be taken over. */
	adoption = &board_adoption;
	if (!adoption->probed || adoption->verdict != POWERVR_DC_GO)
		return ENODEV;

	/* Starts from the plane as the loader set it up; that is the framebuffer's commit. */
	plane = adoption->state.planes[adoption->plane];

	/*
	 * A frame changes only the address and the byte order: the extent and
	 * the pitch are the loader's, which the caller checked the frame has.
	 * The alpha byte is ignored, as the loader's blending is kept.
	 */
	if (frame != NULL) {
		if (frame->physical > 0xffffffffULL)
			return EINVAL;

		swizzle = DC_CONFIG_SWIZZLE_ARGB;
		if (frame->rgba)
			swizzle = DC_CONFIG_SWIZZLE_ABGR;

		format = DC_CONFIG_FORMAT_XRGB;
		plane.address = (uint32_t)frame->physical;
		plane.config &= ~(DC_CONFIG_FORMAT_MASK << DC_CONFIG_FORMAT_SHIFT);
		plane.config &= ~(DC_CONFIG_SWIZZLE_MASK << DC_CONFIG_SWIZZLE_SHIFT);
		plane.config |= format << DC_CONFIG_FORMAT_SHIFT;
		plane.config |= swizzle << DC_CONFIG_SWIZZLE_SHIFT;
	}

	/*
	 * Writes the plane under the commit lock with interrupts off, so a
	 * fatal stop on another CPU either comes first and is seen here, or
	 * waits for this commit and writes after it.  The sequence is read
	 * after the panel's hold is let go: a blank that came while the
	 * registers were written does not count for the commit.
	 */
	enabled = spin_lock_irqsave(&dc_commit_lock);

	if (drv_powervr_dc_fatal()) {
		spin_unlock_irqrestore(&dc_commit_lock, enabled);
		return ENXIO;
	}

	write_plane(adoption, &plane);
	*base = atomic_u64_load_acquire(&board_blanks.sequence);
	early = commit_early(&board_blanks, *base);
	dc_phase(&board_blanks, &dc_last_commit.since_us, &dc_last_commit.period_us);
	dc_last_commit.early = early;
	dc_last_commit.sequence = *base;
	if (early)
		*base -= 1U;

	spin_unlock_irqrestore(&dc_commit_lock, enabled);

	/* Succeeded: the plane takes the commit at one of the next blanks. */
	return 0;
}

/* Gives the last commit's place in its frame (dc_last_commit). */
void
drv_powervr_dc_commit_noted(
	struct drv_powervr_dc_commit_note *note)
{
	/* A copy; the caller's mutex keeps the next commit out. */
	*note = dc_last_commit;
}

/*
 * Gives how far after the newest blank it is now and the period the blanks
 * measured, in microseconds (both 0 without the fine counter or a period).
 */
void
drv_powervr_dc_phase(
	uint64_t *since_us,
	uint64_t *period_us)
{
	/* The board's blanks. */
	dc_phase(&board_blanks, since_us, period_us);
}

/*
 * Reports the HDMI panel's vertical-blank count.
 */
uint64_t
drv_powervr_dc_sequence(
	void)
{
	/* Pairs with the interrupt's release store. */
	return atomic_u64_load_acquire(&board_blanks.sequence);
}

/*
 * Waits until the vertical-blank count reaches target, or until no blank
 * has come for the stall limit (ETIMEDOUT).  The count reached is stored
 * through reached either way.
 */
int
drv_powervr_dc_wait(
	uint64_t target,
	uint64_t *reached)
{
	struct dc_blanks *blanks;
	unsigned long enabled;
	uint64_t start;
	uint64_t since;
	uint64_t deadline;
	uint64_t observed;
	uint64_t now;
	int error;

	/* Without the interrupt no blank ever comes. */
	blanks = &board_blanks;
	*reached = 0;
	if (!blanks->started)
		return ENODEV;

	/*
	 * Sleeps on the interrupt's wakeup.  The stall limit runs from the
	 * newest blank, or from the start of the wait when that is later, so
	 * a panel that keeps counting is waited for however long the target
	 * is away.
	 */
	error = 0;
	start = sched_ticks();
	enabled = spin_lock_irqsave(&blanks->lock);

	for (;;) {
		/* Stops once the count is there. */
		*reached = blanks->sequence;
		if (blanks->sequence >= target)
			break;

		/* Gives up once the blanks stopped for the limit. */
		since = blanks->last_tick;
		if (since < start)
			since = start;
		deadline = since + DC_STALL_TICKS;
		now = sched_ticks();
		if (now >= deadline) {
			error = ETIMEDOUT;
			break;
		}

		/* Sleeps until the next blank or the deadline; EAGAIN means one came meanwhile. */
		observed = waitq_sequence(&blanks->wake);
		(void)waitq_sleep(&blanks->wake, &blanks->lock, observed, deadline, 0);
	}

	spin_unlock_irqrestore(&blanks->lock, enabled);

	return error;
}

/*
 * Writes what the vertical-blank interrupt saw to the boot log: how many
 * blanks came, and the range of scan positions it read them at.
 */
void
drv_powervr_dc_vblank_report(
	void)
{
	struct dc_blanks *blanks;
	unsigned long enabled;
	uint64_t sequence;
	uint64_t others;
	uint32_t lowest;
	uint32_t highest;
	uint32_t newest;

	/* Nothing to say before the interrupt was started, nor in a release image. */
	blanks = &board_blanks;
	if (!blanks->started || !POWERVR_DEBUG)
		return;

	/* Samples the counters together. */
	enabled = spin_lock_irqsave(&blanks->lock);

	sequence = blanks->sequence;
	others = blanks->others;
	lowest = blanks->lowest;
	highest = blanks->highest;
	newest = blanks->ring[(blanks->next + DC_BLANK_RING - 1U) % DC_BLANK_RING];

	spin_unlock_irqrestore(&blanks->lock, enabled);

	kern_logf("dc8200: vblank %llu other %llu loc %x-%x newest %x\n",
		  (unsigned long long)sequence,
		  (unsigned long long)others,
		  lowest,
		  highest,
		  newest);

	/*
	 * The PLIC's view of the source, the controller's interrupt enable and
	 * the panel's configuration.  The pending register (0x10) is not read:
	 * the read clears it, and only the handler may (a read here eats the
	 * interrupt the handler waits for).
	 */
	plic_report(board_blanks.irq, 0);
	kern_logf("dc8200: interrupt enable %x, panel config %x\n",
		  read_control(&board_adoption, DC_INTERRUPT_ENABLE),
		  read_display(&board_adoption, DC_PANEL_CONFIG + DC_UNIT_STEP * DC_HDMI_PANEL));
}

/*
 * The JH7110 PLIC's view of the controller's source, in one line: its
 * priority, whether it is pending, and the contexts it is enabled in
 * (the S-mode ones of harts 1 to 4 are 2, 4, 6 and 8).  With complete set
 * (once, when the count starts) each enabled context completes the source,
 * which frees a claim left uncompleted and is ignored otherwise.
 *
 * Values referenced: the RISC-V PLIC specification (priority 4 bytes a
 * source from 0, pending bits from 0x1000, enable blocks of 0x80 a context
 * from 0x2000, threshold 0x1000 a context from 0x200000); the JH7110
 * device tree (plic at 0x0c000000).
 */
static void
plic_report(
	int irq,
	int complete)
{
	static volatile uint8_t *plic;
	void *mapped;
	uint32_t source;
	uint32_t enabled;
	unsigned context;
	int error;

	/* The registers, through one view of their own made at the first call. */
	if (irq < 0)
		return;
	if (plic == NULL) {
		error = kern_device_map(DC_PLIC_BASE, DC_PLIC_SIZE, KERN_DEVICE_UNCACHED, &mapped);
		if (error != 0) {
			kern_logf("dc8200: plic unread (%d)\n", error);
			return;
		}
		plic = mapped;
	}

	/* The contexts the source is enabled in; at the start each completes a claim it may hold. */
	source = (uint32_t)irq;
	enabled = 0;
	for (context = 0; context < DC_PLIC_CONTEXTS; context++) {
		if (((kern_mmio_read32(plic + 0x2000U + context * 0x80U + (source / 32U) * 4U) >> (source % 32U)) & 1U) == 0)
			continue;
		enabled |= 1U << context;
		if (complete)
			kern_mmio_write32(plic + 0x200004U + context * 0x1000U, source);
	}

	/* One line: the priority, whether it is pending, and where it is enabled. */
	if (POWERVR_DEBUG)
		kern_logf("dc8200: plic source %u priority %u pending %u contexts %x\n", source,
			  kern_mmio_read32(plic + source * 4U),
			  (kern_mmio_read32(plic + 0x1000U + (source / 32U) * 4U) >> (source % 32U)) & 1U,
			  enabled);
}

/* Reads one of the identity registers. */
static uint32_t
read_control(
	const struct dc_adoption *adoption,
	uint32_t offset)
{
	/* The registers are 32 bits wide at their byte offsets. */
	return kern_mmio_read32(adoption->control + offset);
}

/* Reads one of the planes' or panels' registers by its controller offset. */
static uint32_t
read_display(
	const struct dc_adoption *adoption,
	uint32_t offset)
{
	/* The display window starts at the controller's offset of its base. */
	return kern_mmio_read32(adoption->display + (offset - DC_DISPLAY_BASE));
}

/*
 * Reports whether this CPU holds the commit lock.
 *
 * Reads the lock's owner the way spin_trylock() does before it traps on a
 * recursive acquisition: the owner's CPU is valid once owner_valid is
 * published with a release.
 */
static int
commit_held_here(
	void)
{
	/* A free lock is nobody's. */
	if (atomic_load_acquire(&dc_commit_lock.held) == 0U)
		return 0;

	/* A held lock whose owner is not yet published is another CPU's. */
	if (atomic_raw_load_acquire(&dc_commit_lock.owner_valid) == 0U)
		return 0;

	/* The lock is this CPU's when the published owner is this CPU. */
	return dc_commit_lock.owner_cpu == kern_cpu_current();
}

/* Writes one of the planes' or panels' registers by its controller offset. */
static void
write_display(
	const struct dc_adoption *adoption,
	uint32_t offset,
	uint32_t value)
{
	/* The display window starts at the controller's offset of its base. */
	kern_mmio_write32(adoption->display + (offset - DC_DISPLAY_BASE), value);
}

/* Reads the controller's identity, both primary planes and the HDMI panel's settings. */
static void
read_state(
	struct dc_adoption *adoption)
{
	struct powervr_dc_plane *plane;
	uint32_t step;
	unsigned index;

	/* Reads which controller this is. */
	adoption->state.model = read_control(adoption, DC_MODEL);
	adoption->state.revision = read_control(adoption, DC_REVISION);
	adoption->state.customer = read_control(adoption, DC_CUSTOMER);

	/* Reads every register a plane is set up by, for each primary plane. */
	for (index = 0; index < POWERVR_DC_PLANES; index++) {
		plane = &adoption->state.planes[index];
		step = DC_UNIT_STEP * index;
		plane->address = read_display(adoption, DC_PLANE_ADDRESS + step);
		plane->stride = read_display(adoption, DC_PLANE_STRIDE + step);
		plane->config = read_display(adoption, DC_PLANE_CONFIG + step);
		plane->extra = read_display(adoption, DC_PLANE_EXTRA + step);
		plane->size = read_display(adoption, DC_PLANE_SIZE + step);
		plane->top_left = read_display(adoption, DC_PLANE_TOP_LEFT + step);
		plane->bottom_right = read_display(adoption, DC_PLANE_BOTTOM_RIGHT + step);
		plane->background = read_display(adoption, DC_PLANE_BACKGROUND + step);
		plane->blend = read_display(adoption, DC_PLANE_BLEND + step);
	}

	/* Reads the HDMI panel's settings, including the word that holds back shadowed updates. */
	adoption->panel_config = read_display(adoption, DC_PANEL_CONFIG + DC_UNIT_STEP * DC_HDMI_PANEL);
	adoption->panel_extra = read_display(adoption, DC_PANEL_EXTRA + DC_UNIT_STEP * DC_HDMI_PANEL);
}

/*
 * Reads the HDMI panel's scan position and a plane's commit bit over a
 * little more than two frames, keeping the range the position covered,
 * how often it went down, and whether the commit bit was ever set.
 *
 * Only reads: the loader's commit has long been taken, so a set bit means
 * the bit stays set rather than clearing itself.
 */
static void
watch_scan(
	struct dc_adoption *adoption,
	uint32_t extra_offset)
{
	struct dc_watch *watch;
	uint64_t start;
	uint64_t now;
	uint64_t frequency;
	uint64_t span;
	uint32_t location;
	uint32_t previous;
	uint32_t extra;
	bool counting;

	/* Starts the record from the first reading. */
	watch = &adoption->watch;
	location = read_display(adoption, DC_PANEL_LOCATION + DC_UNIT_STEP * DC_HDMI_PANEL);
	extra = read_display(adoption, extra_offset);
	watch->first = location;
	watch->lowest = location;
	watch->highest = location;
	watch->wraps = 0;
	watch->reads = 1;
	watch->commit_seen = 0;
	if ((extra & POWERVR_DC_EXTRA_COMMIT) != 0)
		watch->commit_seen = 1;

	/* Times the watch by the kernel's counter, or by a count of reads when it is not running. */
	counting = kern_rtc_read_counter(&start, &frequency);
	span = 0;
	if (counting && frequency != 0)
		span = (frequency * DC_WATCH_MICROSECONDS) / 1000000U;

	/* Reads the position and the bit until the watch is over. */
	previous = location;
	for (;;) {
		/* Takes one reading of the position and the bit. */
		location = read_display(adoption, DC_PANEL_LOCATION + DC_UNIT_STEP * DC_HDMI_PANEL);
		extra = read_display(adoption, extra_offset);
		watch->reads++;

		/* Widens the range the position covered. */
		if (location < watch->lowest)
			watch->lowest = location;
		if (location > watch->highest)
			watch->highest = location;

		/* Counts a fall of the position, the start of a new frame for a line counter. */
		if (location < previous)
			watch->wraps++;
		previous = location;

		/* Notes a commit bit that is set, which a self-clearing bit never shows here. */
		if ((extra & POWERVR_DC_EXTRA_COMMIT) != 0)
			watch->commit_seen = 1;

		/* Ends by the counter when it runs, otherwise by the count of reads. */
		if (span != 0) {
			counting = kern_rtc_read_counter(&now, &frequency);
			if (!counting || now - start >= span)
				break;
		} else if (watch->reads >= DC_WATCH_READS) {
			break;
		}
	}
}

/* Writes the decision and what it rests on as one line of the boot log. */
static void
log_outcome(
	const struct dc_adoption *adoption)
{
	const struct powervr_dc_plane *plane;
	const char *outcome;
	unsigned shown;

	/* Shows the lit plane, or plane 0 when none is lit. */
	shown = adoption->plane;
	if (shown >= POWERVR_DC_PLANES)
		shown = 0;
	plane = &adoption->state.planes[shown];

	/* Names the outcome the takeover's next step depends on. */
	outcome = "no-go";
	if (adoption->verdict == POWERVR_DC_GO)
		outcome = "go";

	if (POWERVR_DEBUG)
		kern_logf("dc8200: %s (%s) model %x rev %x cid %x plane %u addr %x stride %u size %x cfg %x ex %x panel %x/%x loc %x %x-%x wraps %u reads %u commit %u fb %llx/%u\n",
			  outcome,
			  powervr_dc_verdict_name(adoption->verdict),
			  adoption->state.model,
			  adoption->state.revision,
			  adoption->state.customer,
			  shown,
			  plane->address,
			  plane->stride,
			  plane->size,
			  plane->config,
			  plane->extra,
			  adoption->panel_config,
			  adoption->panel_extra,
			  adoption->watch.first,
			  adoption->watch.lowest,
			  adoption->watch.highest,
			  adoption->watch.wraps,
			  adoption->watch.reads,
			  adoption->watch.commit_seen,
			  (unsigned long long)adoption->screen.physical,
			  adoption->screen.pitch);
}

/* Writes one of the control window's registers. */
static void
write_control(
	const struct dc_adoption *adoption,
	uint32_t offset,
	uint32_t value)
{
	/* The registers are 32 bits wide at their byte offsets. */
	kern_mmio_write32(adoption->control + offset, value);
}

/*
 * Writes a plane's registers to the chosen plane and commits them: holds
 * back the HDMI panel's shadowed registers and drops the commit bit,
 * writes the plane, sets the commit bit and lets the panel take the
 * registers at its next frame.  The caller holds the commit lock, or is
 * a fatal stop that could not take it.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/verisilicon/vs_dc_hw.c (dc_hw_enable_shadow_register
 * around the plane writes); Linux torvalds/linux master
 * drivers/gpu/drm/verisilicon/vs_primary_plane.c (atomic_update: the
 * commit bit set after the plane's registers).
 */
static void
write_plane(
	const struct dc_adoption *adoption,
	const struct powervr_dc_plane *plane)
{
	uint32_t step;
	uint32_t panel;

	/* The chosen plane's registers and the HDMI panel's extension. */
	step = DC_UNIT_STEP * adoption->plane;
	panel = DC_PANEL_EXTRA + DC_UNIT_STEP * DC_HDMI_PANEL;

	/* Holds the shadowed registers back and drops the commit bit, so setting it later is an edge. */
	write_display(adoption, panel, adoption->panel_extra | DC_PANEL_EXTRA_HOLD);
	write_display(adoption, DC_PLANE_EXTRA + step, plane->extra & ~POWERVR_DC_EXTRA_COMMIT);

	/* Writes every register the plane is set up by. */
	write_display(adoption, DC_PLANE_ADDRESS + step, plane->address);
	write_display(adoption, DC_PLANE_STRIDE + step, plane->stride);
	write_display(adoption, DC_PLANE_CONFIG + step, plane->config);
	write_display(adoption, DC_PLANE_SIZE + step, plane->size);
	write_display(adoption, DC_PLANE_TOP_LEFT + step, plane->top_left);
	write_display(adoption, DC_PLANE_BOTTOM_RIGHT + step, plane->bottom_right);
	write_display(adoption, DC_PLANE_BACKGROUND + step, plane->background);
	write_display(adoption, DC_PLANE_BLEND + step, plane->blend);

	/* Commits the plane and lets the panel take it. */
	write_display(adoption, DC_PLANE_EXTRA + step, plane->extra | POWERVR_DC_EXTRA_COMMIT);
	write_display(adoption, panel, adoption->panel_extra & ~DC_PANEL_EXTRA_HOLD);
}

/*
 * Handles the controller's interrupt: reading the pending panels clears
 * them; a blank of the HDMI panel advances the count, notes the time and
 * the scan position, and wakes the waiters.  Nothing else is done here --
 * buffers are let go by the threads that wait.
 */
static void
blank_interrupt(
	int irq,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	struct dc_blanks *blanks;
	uint64_t counter;
	uint64_t frequency;
	uint32_t pending;
	uint32_t location;
	unsigned long enabled;
	bool counting;

	(void)irq;
	blanks = argument;

	/* Reads and clears the pending panels, and where the HDMI panel's scan is. */
	pending = read_control(&board_adoption, DC_INTERRUPT_PENDING);
	location = read_display(&board_adoption, DC_PANEL_LOCATION + DC_UNIT_STEP * DC_HDMI_PANEL);

	/* Counts the blank of the HDMI panel only; the other panel's does not advance it. */
	enabled = spin_lock_irqsave(&blanks->lock);

	if ((pending & (1U << DC_HDMI_PANEL)) != 0) {
		atomic_u64_store_release(&blanks->sequence, blanks->sequence + 1U);
		blanks->last_tick = sched_ticks();
		counting = kern_rtc_read_counter(&counter, &frequency);
		blanks->period_counts = 0;
		if (counting && blanks->last_counter != 0U && counter > blanks->last_counter)
			blanks->period_counts = counter - blanks->last_counter;
		blanks->last_counter = 0;
		if (counting)
			blanks->last_counter = counter;
		blanks->ring[blanks->next] = location;
		blanks->next = (blanks->next + 1U) % DC_BLANK_RING;
		if (location < blanks->lowest)
			blanks->lowest = location;
		if (location > blanks->highest)
			blanks->highest = location;
		waitq_wake_all(&blanks->wake);
	} else {
		blanks->others++;
	}

	spin_unlock_irqrestore(&blanks->lock, enabled);

	/* Hands the interrupt back to the controller of interrupts. */
	kern_irq_send_eoi(acknowledge);
}

/*
 * Gives how far after the newest blank it is now and the period between
 * the last two blanks, in microseconds, from the fine counter; 0 for what
 * cannot be told.  Read without the blanks' lock, as commit_early reads it.
 */
static void
dc_phase(
	const struct dc_blanks *blanks,
	uint64_t *since_us,
	uint64_t *period_us)
{
	uint64_t counter;
	uint64_t frequency;
	uint64_t last;
	bool counting;

	/* Nothing without the counter or a blank. */
	*since_us = 0U;
	*period_us = 0U;
	counting = kern_rtc_read_counter(&counter, &frequency);
	if (!counting || frequency == 0U)
		return;
	last = blanks->last_counter;
	if (last == 0U || counter < last)
		return;

	/* The counts turned into microseconds. */
	*since_us = (counter - last) * 1000000U / frequency;
	*period_us = blanks->period_counts * 1000000U / frequency;
}

/*
 * Tells whether a commit written now, with the blank count at sequence,
 * comes early enough in the frame to be taken at the blank that ends it:
 * the newest blank is the one counted, the frames before it measured a
 * period of a plausible length, and the next blank is at least
 * DC_EARLY_MARGIN_US away.  Read without the blanks' lock: an interrupt
 * meanwhile changes the count, which is then not the one counted, and
 * the commit is not early.
 */
static int
commit_early(
	const struct dc_blanks *blanks,
	uint64_t sequence)
{
	uint64_t counter;
	uint64_t frequency;
	uint64_t last;
	uint64_t period;
	uint64_t since;
	bool counting;

	/* The fine counter, and the newest blank's reading and period. */
	counting = kern_rtc_read_counter(&counter, &frequency);
	if (!counting || frequency == 0U)
		return 0;
	last = blanks->last_counter;
	period = blanks->period_counts;
	if (atomic_u64_load_acquire(&blanks->sequence) != sequence || last == 0U || counter < last)
		return 0;

	/* A period of a plausible frame, in the counter's counts. */
	if (period < frequency * DC_PERIOD_LEAST_US / 1000000U || period > frequency * DC_PERIOD_MOST_US / 1000000U)
		return 0;

	/* Early: the next blank, a period after the newest, at least the margin away. */
	since = counter - last;
	if (since + frequency * DC_EARLY_MARGIN_US / 1000000U >= period)
		return 0;

	/* Succeeded: the blank ending this frame takes it. */
	return 1;
}

/*
 * Chooses the mode the boot line names with display.mode=, or the default
 * when it names none the driver knows.
 */
static const struct powervr_dc_mode *
choose_mode(
	void)
{
	const struct powervr_dc_mode *mode;
	const char *line;
	uint32_t width;
	uint32_t height;
	uint32_t refresh;
	int error;

	/* The boot parameters are not parsed yet, so the line is read here. */
	line = kern_boot_handoff("boot.command-line");
	error = powervr_dc_mode_from_line(line, &width, &height, &refresh);
	if (error == ENOENT)
		return powervr_dc_mode_default();
	if (error != 0) {
		kern_logf("dc8200: display.mode= unreadable (%d); default mode\n", error);
		return powervr_dc_mode_default();
	}

	/* Takes the named mode when the table has it. */
	error = powervr_dc_mode_find(width, height, refresh, &mode);
	if (error != 0) {
		kern_logf("dc8200: no mode %ux%u@%u; default mode\n", width, height, refresh);
		return powervr_dc_mode_default();
	}

	return mode;
}

/*
 * Finds the memory the plane shows at a mode: the loader's framebuffer
 * when it holds the mode, otherwise one the driver allocates below 4 GiB
 * and paints with colour bars.
 */
static int
own_screen(
	const struct powervr_dc_mode *mode,
	struct powervr_dc_screen *screen,
	uint32_t *format)
{
	struct kern_boot_framebuffer framebuffer;
	struct kern_pmem run;
	uint64_t bytes;
	void *view;
	int fits;
	int error;

	/* Uses the loader's framebuffer when it is in reach and holds the mode, so its console stays visible. */
	error = kern_boot_framebuffer("riscv64.framebuffer", &framebuffer);
	fits = 0;
	if (error == 0)
		fits = 1;
	if (fits && framebuffer.width < mode->width)
		fits = 0;
	if (fits && framebuffer.height < mode->height)
		fits = 0;
	if (fits && framebuffer.physical_base + framebuffer.size - 1U > DC_ADDRESS_LIMIT)
		fits = 0;
	if (fits && framebuffer.format != KERN_BOOT_FRAMEBUFFER_RGBX8888 && framebuffer.format != KERN_BOOT_FRAMEBUFFER_BGRX8888)
		fits = 0;
	if (fits) {
		screen->physical = framebuffer.physical_base;
		screen->pitch = framebuffer.pitch;
		screen->width = framebuffer.width;
		screen->height = framebuffer.height;
		*format = framebuffer.format;
		return 0;
	}

	/* Allocates one contiguous run the controller can scan. */
	bytes = (uint64_t)mode->width * DC_PIXEL_BYTES * mode->height;
	error = kern_pmem_alloc_limited((size_t)bytes, DC_OWN_ALIGNMENT, DC_ADDRESS_LIMIT, 0, &run);
	if (error != 0)
		return error;

	/* Paints it through its uncached alias, which the controller reads without a flush. */
	error = kern_pmem_map_uncached(&run, &view);
	if (error != 0) {
		(void)kern_pmem_free(&run);
		return error;
	}

	screen->physical = run.paddr;
	screen->pitch = mode->width * DC_PIXEL_BYTES;
	screen->width = mode->width;
	screen->height = mode->height;
	*format = KERN_BOOT_FRAMEBUFFER_BGRX8888;
	draw_bars(view, screen);

	/* Succeeded: the run is the screen for the life of the system. */
	return 0;
}

/*
 * Paints red, green, blue and white bars over a framebuffer the driver
 * allocated, blue bytes first, so the output shows the colours in order.
 */
static void
draw_bars(
	volatile uint32_t *pixels,
	const struct powervr_dc_screen *screen)
{
	static const uint32_t colours[4] = { 0x00ff0000U, 0x0000ff00U, 0x000000ffU, 0x00ffffffU };
	uint32_t row;
	uint32_t column;
	uint32_t bar;

	/* Four upright bars, each a quarter of the width. */
	for (row = 0; row < screen->height; row++) {
		for (column = 0; column < screen->width; column++) {
			bar = (column * 4U) / screen->width;
			pixels[row * (screen->pitch / DC_PIXEL_BYTES) + column] = colours[bar];
		}
	}
}

/*
 * Sets the HDMI panel's output to 24-bit RGB on the display port, its
 * timing to the mode's, and starts it.
 *
 * Procedure referenced:
 * Linux torvalds/linux master drivers/gpu/drm/verisilicon/vs_bridge.c
 * (vs_bridge_atomic_enable_dp and vs_bridge_enable_common: the format
 * with the port enabled, the panel's data, enable and clock signals and
 * their polarities, the running bit, the panel started without the
 * multi-display sync) and vs_crtc.c (vs_crtc_mode_set_nofb: the timing
 * words); StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/verisilicon/vs_dc_hw.c (setup_display: the DPI format
 * and the dither off).
 */
static void
start_panel(
	const struct dc_adoption *adoption,
	const struct powervr_dc_mode *mode)
{
	struct powervr_dc_panel_timing timing;
	uint32_t step;
	uint32_t value;

	step = DC_UNIT_STEP * DC_HDMI_PANEL;

	/* 24-bit RGB on the display port and on the parallel output. */
	write_display(adoption, DC_PANEL_DP + step, DC_PANEL_DP_RGB888 | DC_PANEL_DP_ENABLE);
	write_display(adoption, DC_PANEL_DPI + step, DC_PANEL_DPI_RGB888);
	write_display(adoption, DC_PANEL_DITHER + step, 0U);

	/* The mode's timing. */
	powervr_dc_panel_timing(mode, &timing);
	write_display(adoption, DC_PANEL_HORIZONTAL + step, timing.horizontal);
	write_display(adoption, DC_PANEL_HSYNC + step, timing.horizontal_sync);
	write_display(adoption, DC_PANEL_VERTICAL + step, timing.vertical);
	write_display(adoption, DC_PANEL_VSYNC + step, timing.vertical_sync);

	/* The panel's signals enabled with their plain polarities, RGB, running. */
	value = read_display(adoption, DC_PANEL_CONFIG + step);
	value &= ~(DC_PANEL_CONFIG_DE_LOW | DC_PANEL_CONFIG_DATA_LOW | DC_PANEL_CONFIG_CLOCK_LOW | DC_PANEL_CONFIG_YUV);
	value |= DC_PANEL_CONFIG_DE | DC_PANEL_CONFIG_DATA | DC_PANEL_CONFIG_CLOCK | DC_PANEL_CONFIG_RUNNING;
	write_display(adoption, DC_PANEL_CONFIG + step, value);

	/* Starts the panel on its own, without the other panel's sync. */
	value = read_display(adoption, DC_PANEL_START);
	value &= ~DC_PANEL_START_SYNC;
	value |= 1U << DC_HDMI_PANEL;
	write_display(adoption, DC_PANEL_START, value);
}

/*
 * Shows the 64 x 64 ARGB cursor image at physical with its top-left
 * corner at (x, y) on the HDMI panel, or hides it.  A corner left of or
 * above the screen is shown through the hotspot, as Linux does.  The
 * registers are written with the panel's shadowed registers held back and
 * the plane's commit bit set again, the order Linux's commit uses, so the
 * cursor changes at a frame boundary with the plane.
 */
int
drv_powervr_dc_cursor(
	int shown,
	uint64_t physical,
	int32_t x,
	int32_t y)
{
	const struct dc_adoption *adoption;
	const struct powervr_dc_plane *plane;
	unsigned long enabled;
	uint32_t base;
	uint32_t step;
	uint32_t panel;
	uint32_t config;
	uint32_t hot_x;
	uint32_t hot_y;

	/* Only the controller whose screen is the driver's, and an image it can name. */
	adoption = &board_adoption;
	if (!adoption->probed || adoption->verdict != POWERVR_DC_GO || adoption->plane >= POWERVR_DC_PLANES)
		return ENODEV;
	if (shown && (physical > DC_ADDRESS_LIMIT || (physical & 0xffU) != 0))
		return EINVAL;

	/* The hotspot takes what lies left of or above the screen; beyond 63 the cursor is off the screen. */
	hot_x = 0;
	hot_y = 0;
	if (x < 0) {
		if (x < -DC_CURSOR_HOT_MAX)
			shown = 0;
		else
			hot_x = (uint32_t)-x;
		x = 0;
	}
	if (y < 0) {
		if (y < -DC_CURSOR_HOT_MAX)
			shown = 0;
		else
			hot_y = (uint32_t)-y;
		y = 0;
	}
	if (x > 0xffff)
		x = 0xffff;
	if (y > 0xffff)
		y = 0xffff;

	/* The registers of the HDMI panel's cursor, and the plane's. */
	base = DC_HDMI_PANEL != 0U ? DC_CURSOR_PANEL_STEP : 0U;
	step = DC_UNIT_STEP * adoption->plane;
	panel = DC_PANEL_EXTRA + DC_UNIT_STEP * DC_HDMI_PANEL;
	plane = &adoption->state.planes[adoption->plane];

	enabled = spin_lock_irqsave(&dc_commit_lock);

	if (drv_powervr_dc_fatal()) {
		spin_unlock_irqrestore(&dc_commit_lock, enabled);
		return ENXIO;
	}

	/* Holds the shadowed registers back, then the cursor, then the commit edge and the release. */
	write_display(adoption, panel, adoption->panel_extra | DC_PANEL_EXTRA_HOLD);
	write_display(adoption, DC_PLANE_EXTRA + step, plane->extra & ~POWERVR_DC_EXTRA_COMMIT);
	config = read_display(adoption, DC_CURSOR_CONFIG + base);
	if (shown) {
		write_display(adoption, DC_CURSOR_ADDRESS + base, (uint32_t)physical);
		write_display(adoption, DC_CURSOR_LOCATION + base, (uint32_t)x | ((uint32_t)y << 16));
		config = (config & ~DC_CURSOR_CLEAR) | (hot_x << 16) | (hot_y << 8) | DC_CURSOR_SHOWN;
	} else {
		config = (config & ~DC_CURSOR_HIDDEN_CLEAR) | DC_CURSOR_HIDDEN_SET;
	}
	write_display(adoption, DC_CURSOR_CONFIG + base, config);
	write_display(adoption, DC_PLANE_EXTRA + step, plane->extra | POWERVR_DC_EXTRA_COMMIT);
	write_display(adoption, panel, adoption->panel_extra & ~DC_PANEL_EXTRA_HOLD);

	spin_unlock_irqrestore(&dc_commit_lock, enabled);

	/* Succeeded: the cursor changes at one of the next blanks. */
	return 0;
}
