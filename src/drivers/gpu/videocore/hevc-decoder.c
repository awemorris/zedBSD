/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 5's HEVC decoder block (see hevc-decoder.h).
 *
 * The block has two register windows: the decoder's (both phases'
 * registers) and an interrupt controller whose one word latches the end
 * of each phase.  It reaches memory by bus address, which on this board
 * is the physical address; the IOMMU in front of it is only read here, to
 * say how the firmware left it.  Its clock is the firmware's.
 */

#include <drivers/generic/fdt.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <uapi/errno.h>

#include "drivers/gpu/videocore/bcm2711-private.h"
#include "drivers/gpu/videocore/hevc-decoder.h"
#include "drivers/gpu/videocore/v3d71-private.h"

/*
 * The block's device tree binding: its compatible string and the names of
 * its two register windows.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y (7821b6a1)
 * arch/arm64/boot/dts/broadcom/bcm2712-ds.dtsi, node hevc_dec (codec@800000).
 */
#define HEVC_COMPATIBLE			"brcm,bcm2712-hevc-dec"
#define HEVC_WINDOW_DECODER		"hevc"
#define HEVC_WINDOW_INTERRUPT		"intc"

/*
 * The firmware's clock of the block and the property tags that ask its
 * highest rate, set its rate and turn it on.
 *
 * Values referenced: bcm2712-ds.dtsi above (clocks = <&firmware_clocks 11>);
 * Raspberry Pi firmware wiki, "Mailbox property interface" (Get max clock
 * rate 0x00030004, Set clock rate 0x00038002, Set clock state 0x00038001,
 * state bit 0 on).
 */
#define HEVC_CLOCK_ID			11U
#define FIRMWARE_TAG_MAX_RATE		0x00030004U
#define FIRMWARE_TAG_SET_RATE		0x00038002U
#define FIRMWARE_TAG_SET_STATE		0x00038001U
#define FIRMWARE_CLOCK_ON		1U
#define FIRMWARE_CLOCK_OFF		0U

/*
 * The decoder's registers this file writes and reads: phase 1's lists,
 * status, version and command list; phase 2's lists, outputs, size,
 * motion vectors and the reference table at 0x9000 (sixteen entries of
 * four words).
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y (7821b6a1)
 * drivers/media/platform/raspberrypi/hevc_dec/hevc_d_hw.h (Phase 1 and
 * Phase 2 register offsets); hevc_d_h265.c phase2_claimed (0x9000 + 16 i).
 */
#define REG_STATUS			0x38U
#define REG_VERSION			0x3cU
#define REG_PUWBASE			0x50U
#define REG_PUWSTRIDE			0x54U
#define REG_COEFFWBASE			0x58U
#define REG_COEFFWSTRIDE		0x5cU
#define REG_CFBASE			0x6cU
#define REG_CFNUM			0x70U
#define REG_CFSTATUS			0x74U
#define REG_PURBASE			0x8000U
#define REG_PURSTRIDE			0x8004U
#define REG_COEFFRBASE			0x8008U
#define REG_COEFFRSTRIDE		0x800cU
#define REG_NUMROWS			0x8010U
#define REG_CONFIG2			0x8014U
#define REG_OUTYBASE			0x8018U
#define REG_OUTYSTRIDE			0x801cU
#define REG_OUTCBASE			0x8020U
#define REG_OUTCSTRIDE			0x8024U
#define REG_FRAMESIZE			0x802cU
#define REG_MVBASE			0x8030U
#define REG_MVSTRIDE			0x8034U
#define REG_COLBASE			0x8038U
#define REG_COLSTRIDE			0x803cU
#define REG_CURRPOC			0x8040U
#define REG_REFERENCES			0x9000U
#define REFERENCE_STRIDE		16U

/*
 * Phase 1's status bits: the coefficient list and the PU list ran out.
 *
 * Values referenced: hevc_d_h265.c (rpi-6.12.y, 7821b6a1)
 * STATUS_COEFF_EXHAUSTED and STATUS_PU_EXHAUSTED, check_status (the
 * phase succeeded when CFSTATUS equals CFNUM).
 */
#define STATUS_COEFFICIENTS_FULL	8U
#define STATUS_PU_FULL			16U

/*
 * The interrupt control word (offset 0 of the interrupt window): each
 * phase's latched end (write 1 to clear) and its enable, and the bits that
 * must be written 0.
 *
 * Values referenced: hevc_d_hw.h (rpi-6.12.y, 7821b6a1) ARG_IC_ICTRL_*
 * (ACTIVE1_INT bit 0, ACTIVE1_EN bit 2, ACTIVE2_INT bit 4, ACTIVE2_EN bit
 * 6, SET_ZERO_MASK bits 19:12 and 11); hevc_d_hw.c hevc_d_hw_probe (the
 * enables set and the latched bits cleared at the start).
 */
#define ICTRL				0x00U
#define ICTRL_PHASE1_DONE		(1U << 0)
#define ICTRL_PHASE1_ENABLE		(1U << 2)
#define ICTRL_PHASE2_DONE		(1U << 4)
#define ICTRL_PHASE2_ENABLE		(1U << 6)
#define ICTRL_ZERO_MASK			((0xffU << 12) | (1U << 11))

/* The IOMMU's control word and its enable bit, read to report the firmware's setting. */
#define IOMMU_CTRL			0x00U
#define IOMMU_ENABLE			1U
#define IOMMU_WINDOW_BYTES		0x40U

/*
 * The block, once found: its two windows, the IOMMU's (if the tree names
 * one), its clock's rate, and how many users hold the clock.  Written at
 * discovery and by hold and release under the controller mutex.
 */
static struct bcm2711_window hevc_decoder;
static struct bcm2711_window hevc_interrupt;
static bool hevc_present;
static uint32_t hevc_rate;
static uint32_t hevc_holders;

static void write_decoder(uint32_t reg, uint32_t value);
static uint32_t read_decoder(uint32_t reg);
static uint32_t bus_units(uint64_t address);
static uint32_t length_units(uint64_t bytes);
static uint32_t list_stride(uint64_t bytes, uint32_t rows);
static void report_iommu(const struct drv_fdt *fdt, uint32_t node);

/*
 * Finds the block in the device tree and maps its registers.
 */
int
bcm2712_hevc_decoder_discover(
	const struct drv_fdt *fdt)
{
	uint32_t node;
	uint32_t status;
	int error;

	/* The node, and its two windows by name. */
	error = bcm2711_fdt_find(fdt, HEVC_COMPATIBLE, &node);
	if (error != 0)
		return ENODEV;
	error = bcm2711_fdt_named_window(fdt, node, HEVC_WINDOW_DECODER, &hevc_decoder);
	if (error == 0)
		error = bcm2711_fdt_named_window(fdt, node, HEVC_WINDOW_INTERRUPT, &hevc_interrupt);
	if (error != 0) {
		kern_logf("v3d: hevc: the decoder's windows are not in the tree (%d)\n", error);
		return ENODEV;
	}

	/* Maps both. */
	error = bcm2711_map_window(&hevc_decoder);
	if (error == 0)
		error = bcm2711_map_window(&hevc_interrupt);
	if (error != 0) {
		kern_logf("v3d: hevc: the decoder's windows do not map (%d)\n", error);
		return error;
	}

	/* The completions enabled, and any latched ones cleared. */
	kern_mmio_write32(hevc_interrupt.mapped + ICTRL, ICTRL_PHASE1_ENABLE | ICTRL_PHASE2_ENABLE);
	status = kern_mmio_read32(hevc_interrupt.mapped + ICTRL);
	kern_mmio_write32(hevc_interrupt.mapped + ICTRL, status & ~ICTRL_ZERO_MASK);

	/* How the firmware left the IOMMU, for the log. */
	report_iommu(fdt, node);
	hevc_present = true;
	kern_logf("v3d: hevc: decoder %llx interrupt %llx\n",
		  (unsigned long long)hevc_decoder.physical, (unsigned long long)hevc_interrupt.physical);

	/* Succeeded: the block may decode once its clock runs. */
	return 0;
}

/*
 * Reports whether the block was found.
 */
bool
bcm2712_hevc_decoder_present(void)
{
	/* Set once by discovery. */
	return hevc_present;
}

/*
 * Takes the block's clock: the first user turns it on at its highest rate.
 */
int
bcm2712_hevc_decoder_hold(void)
{
	uint32_t rate;
	int error;

	/* A later user finds it running. */
	if (!hevc_present)
		return ENODEV;
	if (hevc_holders != 0U) {
		hevc_holders++;
		return 0;
	}

	/* The highest rate, set, then the clock on. */
	error = bcm2712_clock_get(FIRMWARE_TAG_MAX_RATE, HEVC_CLOCK_ID, &rate);
	if (error == 0)
		error = bcm2712_clock_set(FIRMWARE_TAG_SET_RATE, HEVC_CLOCK_ID, rate);
	if (error == 0)
		error = bcm2712_clock_set(FIRMWARE_TAG_SET_STATE, HEVC_CLOCK_ID, FIRMWARE_CLOCK_ON);
	if (error != 0) {
		kern_logf("v3d: hevc: the firmware did not start the decoder's clock (%d)\n", error);
		return error;
	}

	/* The block's version, once the clock runs, with the rate. */
	if (hevc_rate == 0U)
		kern_logf("v3d: hevc: clock %u Hz, version %08x\n", (unsigned)rate, (unsigned)read_decoder(REG_VERSION));
	hevc_rate = rate;
	hevc_holders = 1U;

	/* Succeeded: the block runs. */
	return 0;
}

/*
 * Gives back the block's clock: the last user stops it.
 */
void
bcm2712_hevc_decoder_release(void)
{
	int error;

	/* Only the last user stops it. */
	if (hevc_holders == 0U)
		return;
	hevc_holders--;
	if (hevc_holders != 0U)
		return;

	/* The clock off; a failure leaves it running, which costs only power. */
	error = bcm2712_clock_set(FIRMWARE_TAG_SET_STATE, HEVC_CLOCK_ID, FIRMWARE_CLOCK_OFF);
	if (error != 0)
		kern_logf("v3d: hevc: the firmware did not stop the decoder's clock (%d)\n", error);
}

/*
 * Starts phase 1 of a picture.
 *
 * Procedure referenced: hevc_d_h265.c (rpi-6.12.y, 7821b6a1)
 * phase1_claimed (the two lists' bases and strides, the command count,
 * then the command list's base, which starts the phase).
 */
void
bcm2712_hevc_phase1_start(
	const struct bcm2712_hevc_phase1_buffers *buffers)
{
	/* The lists the phase writes, each row of CTBs its share of the list. */
	kern_mmio_write32(hevc_interrupt.mapped + ICTRL, (kern_mmio_read32(hevc_interrupt.mapped + ICTRL) & ~ICTRL_ZERO_MASK) | ICTRL_PHASE1_ENABLE | ICTRL_PHASE2_ENABLE);
	write_decoder(REG_PUWBASE, bus_units(buffers->pu));
	write_decoder(REG_PUWSTRIDE, length_units(list_stride(buffers->pu_bytes, buffers->ctb_rows)));
	write_decoder(REG_COEFFWBASE, bus_units(buffers->coefficients));
	write_decoder(REG_COEFFWSTRIDE, length_units(list_stride(buffers->coefficient_bytes, buffers->ctb_rows)));

	/* The command count, then the list's base, which starts the phase. */
	write_decoder(REG_CFNUM, buffers->command_count);
	kern_io_write_barrier();
	write_decoder(REG_CFBASE, bus_units(buffers->commands));
}

/*
 * Looks at phase 1.
 *
 * Procedure referenced: hevc_d_h265.c (rpi-6.12.y, 7821b6a1) check_status.
 */
enum bcm2712_hevc_result
bcm2712_hevc_phase1_poll(void)
{
	uint32_t latched;
	uint32_t done;
	uint32_t count;
	uint32_t status;

	/* Not latched yet: still running. */
	latched = kern_mmio_read32(hevc_interrupt.mapped + ICTRL);
	if ((latched & ICTRL_PHASE1_DONE) == 0U)
		return BCM2712_HEVC_RUNNING;
	kern_mmio_write32(hevc_interrupt.mapped + ICTRL, (latched & ~ICTRL_ZERO_MASK & ~ICTRL_PHASE2_DONE) | ICTRL_PHASE1_DONE);

	/* Every command ran: done. */
	done = read_decoder(REG_CFSTATUS);
	count = read_decoder(REG_CFNUM);
	if (done == count)
		return BCM2712_HEVC_DONE;

	/* Stopped short: a list that ran out, else a failure. */
	status = read_decoder(REG_STATUS);
	if ((status & STATUS_PU_FULL) != 0U)
		return BCM2712_HEVC_PU_FULL;
	if ((status & STATUS_COEFFICIENTS_FULL) != 0U)
		return BCM2712_HEVC_COEFFICIENTS_FULL;

	/* Succeeded: the phase failed on the bitstream. */
	return BCM2712_HEVC_FAILED;
}

/*
 * Starts phase 2 of a picture.
 *
 * Procedure referenced: hevc_d_h265.c (rpi-6.12.y, 7821b6a1)
 * phase2_claimed (the lists, the outputs, the sixteen references, the
 * words, the motion vector stores, then NUMROWS, which starts the phase).
 */
void
bcm2712_hevc_phase2_start(
	const struct bcm2712_hevc_phase2_buffers *buffers)
{
	uint32_t index;
	uint32_t entry;

	/* The lists phase 1 wrote, with the strides it used. */
	write_decoder(REG_PURBASE, bus_units(buffers->pu));
	write_decoder(REG_PURSTRIDE, length_units(list_stride(buffers->pu_bytes, buffers->ctb_rows)));
	write_decoder(REG_COEFFRBASE, bus_units(buffers->coefficients));
	write_decoder(REG_COEFFRSTRIDE, length_units(list_stride(buffers->coefficient_bytes, buffers->ctb_rows)));

	/* The output planes and their column strides. */
	write_decoder(REG_OUTYBASE, bus_units(buffers->luma));
	write_decoder(REG_OUTCBASE, bus_units(buffers->chroma));
	write_decoder(REG_OUTYSTRIDE, length_units(buffers->luma_stride));
	write_decoder(REG_OUTCSTRIDE, length_units(buffers->chroma_stride));

	/* The references: each entry's planes and strides. */
	for (index = 0; index < 16U; index++) {
		entry = REG_REFERENCES + REFERENCE_STRIDE * index;
		write_decoder(entry, bus_units(buffers->reference_luma[index]));
		write_decoder(entry + 4U, length_units(buffers->luma_stride));
		write_decoder(entry + 8U, bus_units(buffers->reference_chroma[index]));
		write_decoder(entry + 12U, length_units(buffers->chroma_stride));
	}

	/* The picture's words. */
	write_decoder(REG_CONFIG2, buffers->config2);
	write_decoder(REG_FRAMESIZE, buffers->frame_size);
	write_decoder(REG_CURRPOC, buffers->current_poc);

	/* The motion vectors written and read. */
	write_decoder(REG_COLSTRIDE, length_units(buffers->motion_stride));
	write_decoder(REG_MVSTRIDE, length_units(buffers->motion_stride));
	write_decoder(REG_MVBASE, bus_units(buffers->motion_write));
	write_decoder(REG_COLBASE, bus_units(buffers->motion_read));

	/* The rows, which start the phase. */
	kern_io_write_barrier();
	write_decoder(REG_NUMROWS, buffers->ctb_rows);
}

/*
 * Looks at phase 2.
 */
enum bcm2712_hevc_result
bcm2712_hevc_phase2_poll(void)
{
	uint32_t latched;

	/* Not latched yet: still running. */
	latched = kern_mmio_read32(hevc_interrupt.mapped + ICTRL);
	if ((latched & ICTRL_PHASE2_DONE) == 0U)
		return BCM2712_HEVC_RUNNING;

	/* Clears the latch. */
	kern_mmio_write32(hevc_interrupt.mapped + ICTRL, (latched & ~ICTRL_ZERO_MASK & ~ICTRL_PHASE1_DONE) | ICTRL_PHASE2_DONE);

	/* Succeeded: the picture is written. */
	return BCM2712_HEVC_DONE;
}

/*
 * Clears the block's latched completions after a phase was given up.
 */
void
bcm2712_hevc_decoder_clear(void)
{
	uint32_t latched;

	/* Writes back whatever is latched. */
	if (!hevc_present)
		return;
	latched = kern_mmio_read32(hevc_interrupt.mapped + ICTRL);
	kern_mmio_write32(hevc_interrupt.mapped + ICTRL, latched & ~ICTRL_ZERO_MASK);
}

/* Writes one decoder register. */
static void
write_decoder(
	uint32_t reg,
	uint32_t value)
{
	/* The decoder window's word. */
	kern_mmio_write32(hevc_decoder.mapped + reg, value);
}

/* Reads one decoder register. */
static uint32_t
read_decoder(
	uint32_t reg)
{
	/* The decoder window's word. */
	return kern_mmio_read32(hevc_decoder.mapped + reg);
}

/*
 * Reports a bus address in the decoder's 64-byte units.
 *
 * Values referenced: hevc_d_hw.h (rpi-6.12.y, 7821b6a1) apb_write_vc_addr.
 */
static uint32_t
bus_units(
	uint64_t address)
{
	/* The address over 64. */
	return (uint32_t)(address >> 6);
}

/*
 * Reports a length or stride in 64-byte units, rounded up.
 *
 * Values referenced: hevc_d_hw.h (rpi-6.12.y, 7821b6a1) apb_write_vc_len.
 */
static uint32_t
length_units(
	uint64_t bytes)
{
	/* Whole units. */
	return (uint32_t)((bytes + 63U) >> 6);
}

/*
 * Reports a phase 1 list's stride: its share for each CTB row, rounded
 * down to 64 bytes.
 *
 * Values referenced: hevc_d_h265.c (rpi-6.12.y, 7821b6a1) phase1_claimed
 * (pu_stride and coeff_stride).
 */
static uint32_t
list_stride(
	uint64_t bytes,
	uint32_t rows)
{
	/* No rows: the whole list. */
	if (rows == 0U)
		return (uint32_t)(bytes & ~(uint64_t)63U);

	/* Succeeded: the row's share. */
	return (uint32_t)((bytes / rows) & ~(uint64_t)63U);
}

/* Logs how the firmware left the IOMMU in front of the block, when the tree names one. */
static void
report_iommu(
	const struct drv_fdt *fdt,
	uint32_t node)
{
	struct bcm2711_window iommu;
	const uint8_t *value;
	uint32_t length;
	uint32_t phandle;
	uint32_t iommu_node;
	uint32_t control;
	const char *setting;
	int error;

	/* Finds the IOMMU the node names; a node without one has nothing to report. */
	iommu.mapped = NULL;
	error = drv_fdt_property(fdt, node, "iommus", &value, &length);
	if (error != 0 || length < 4U)
		return;

	/* Resolves the phandle to the IOMMU's own node. */
	phandle = (uint32_t)drv_fdt_cells_value(value, 0, 1);
	error = drv_fdt_find_phandle(fdt, phandle, &iommu_node);
	if (error != 0)
		return;

	/* Reads the IOMMU's register window from its node. */
	error = bcm2711_fdt_window(fdt, iommu_node, 0, &iommu);
	if (error != 0)
		return;

	/* Refuses a window too small to hold the control word. */
	if (iommu.size < IOMMU_WINDOW_BYTES)
		return;

	/* Maps the window, only to be read. */
	error = bcm2711_map_window(&iommu);
	if (error != 0 || iommu.mapped == NULL)
		return;

	/* Its control word: off means the block's addresses go to memory as they are. */
	control = kern_mmio_read32(iommu.mapped + IOMMU_CTRL);
	setting = "off";
	if ((control & IOMMU_ENABLE) != 0U)
		setting = "on";

	/* Logs the setting. */
	kern_logf("v3d: hevc: iommu %llx ctrl %08x (%s)\n", (unsigned long long)iommu.physical, (unsigned)control, setting);
}
