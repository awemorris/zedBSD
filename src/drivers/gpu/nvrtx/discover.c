/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The read-only stages of the nvrtx driver.
 *
 * identify finds the GPU's memory windows, maps the first page of the register
 * window and reads the chip identity.  console records where the firmware's
 * picture lies: in this GPU's VRAM window or elsewhere.  Neither writes the
 * GPU; the only change is that memory decoding is switched on for the read
 * if the firmware left it off, and the command bits are restored afterwards.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/pci/pci.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/platform.h>
#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-private.h"

/* The name prefix of the boot handoff values that describe the firmware's framebuffer. */
#define DISCOVER_FRAMEBUFFER_HANDOFF	"pcat.framebuffer"

static void discover_windows(struct nvrtx_device *device);
static void discover_flr(struct nvrtx_device *device);
static const char *discover_bar_kind(const struct nvrtx_window *window);

/*
 * Runs the identify stage: finds the windows, maps the registers and reads the
 * chip.
 *
 * Returns 0 when the identity names a chip of a known generation, ENODEV
 * for a function the driver never drives, ENOTSUP for an unknown chip, or
 * the error of the PCI step that failed.  Whatever was taken is given back
 * by nvrtx_n0_release(), also after a failure.
 */
int
nvrtx_n0_discover(
	struct nvrtx_device *device)
{
	struct nvrtx_window *registers;
	const struct nvrtx_product *product;
	uint32_t ident0;
	uint32_t ident1;
	int error;

	/* Says which function the stage looks at. */
	nvrtx_stage_mark("identify begin %04x:%04x at %02x:%02x.%u",
	    (unsigned)NVRTX_PCI_VENDOR,
	    (unsigned)device->product,
	    (unsigned)device->address.bus,
	    (unsigned)device->address.device,
	    (unsigned)device->address.function);

	/* Records the memory windows and whether the function can reset itself. */
	discover_windows(device);
	discover_flr(device);

	/* The first memory window is the register window; without it there is nothing to read. */
	registers = &device->windows[0];
	if (registers->present == 0) {
		nvrtx_stage_mark("identify fail: no memory window");
		return ENODEV;
	}

	/* A window too small to hold the identity registers is not a GPU's register window. */
	if (registers->size < NVRTX_REGISTER_WINDOW_MIN) {
		nvrtx_stage_mark("identify fail: register window of %llu bytes",
		    (unsigned long long)registers->size);
		return ENODEV;
	}

	/* Saves the command bits, so the release gives the function back as the firmware left it. */
	error = drv_pci_device_save_enable_state(device->pci, &device->enable_state);
	if (error != 0) {
		nvrtx_stage_mark("identify fail: command bits not saved (%d)", error);
		return error;
	}

	device->enable_saved = 1;

	/* Lets the registers decode; the firmware's console has it on already. */
	error = drv_pci_device_enable_memory(device->pci);
	if (error != 0) {
		nvrtx_stage_mark("identify fail: memory decoding not enabled (%d)", error);
		return error;
	}

	/*
	 * Claims the register window, so no other driver maps it while identify
	 * reads.
	 */
	error = drv_pci_device_claim_bar(device->pci, registers->bar);
	if (error != 0) {
		nvrtx_stage_mark("identify fail: register window not claimed (%d)", error);
		return error;
	}

	device->registers_claimed = 1;

	/* Maps only the first page, which holds both identity registers, uncached and read-only. */
	error = drv_pci_device_map_bar_region(device->pci,
	    registers->bar,
	    0U,
	    NVRTX_N0_MAP_BYTES,
	    DRV_PCI_MAP_READ | DRV_PCI_MAP_NOCACHE,
	    &device->registers);
	if (error != 0) {
		nvrtx_stage_mark("identify fail: registers not mapped (%d)", error);
		return error;
	}

	/* Reads the two identity registers. */
	ident0 = kern_mmio_read32((const uint8_t *)device->registers.address + NVRTX_REG_MC_IDENT_0);
	ident1 = kern_mmio_read32((const uint8_t *)device->registers.address + NVRTX_REG_MC_IDENT_1);

	/* Marks the raw values, so a refused chip still shows what it answered. */
	nvrtx_stage_mark("identify ident0=%08x ident1=%08x flr=%s",
	    (unsigned)ident0,
	    (unsigned)ident1,
	    device->flr != 0 ? "yes" : "no");

	/* Decodes the chip, and says why a function is refused. */
	error = nvrtx_identify(ident0, ident1, &device->chip);
	if (error == ENOTSUP) {
		nvrtx_stage_mark("identify stop: unknown chip %03x (%s)",
		    (unsigned)device->chip.chipset,
		    nvrtx_family_name(device->chip.family));
		return error;
	}

	/* Any other refusal is a function this driver never drives. */
	if (error != 0) {
		nvrtx_stage_mark("identify stop: not a GPU this driver drives (chip %03x)",
		    (unsigned)device->chip.chipset);
		return error;
	}

	/* Marks the chip the later stages are chosen by. */
	nvrtx_stage_mark("identify ok chip=%03x %s rev %02x (%s)",
	    (unsigned)device->chip.chipset,
	    device->chip.name,
	    (unsigned)device->chip.revision,
	    nvrtx_family_name(device->chip.family));

	/*
	 * Names the board by its device ID when the table knows it.  The chip
	 * decides the path either way; a product whose table chip differs from
	 * the identity is shown, so a wrong table row is seen.
	 */
	product = nvrtx_product_find(device->product);
	if (product == NULL) {
		nvrtx_stage_mark("identify product %04x not in the table", (unsigned)device->product);
	} else if (product->chipset != device->chip.chipset) {
		nvrtx_stage_mark("identify product %04x is listed as chip %03x", (unsigned)device->product, (unsigned)product->chipset);
	} else if (product->name != NULL) {
		nvrtx_stage_mark("identify product %s%s", product->name, product->laptop != 0 ? " (laptop)" : "");
	}

	/* Succeeded: the chip is identified and the register page stays mapped until the release. */
	return 0;
}

/*
 * Runs the console stage: records where the firmware's picture lies.
 *
 * Only the loader's description of the framebuffer is read; the display
 * registers are left alone.  A picture inside this GPU's VRAM window is the
 * one the later stages must keep, a picture elsewhere belongs to another
 * display device (or to an emulated one under passthrough).
 */
void
nvrtx_n1_readout(
	struct nvrtx_device *device)
{
	struct kern_boot_framebuffer boot_framebuffer;
	const struct kern_boot_framebuffer *framebuffer;
	const struct nvrtx_window *vram;
	uint64_t offset;
	int error;

	/* A boot without a framebuffer has no picture to keep. */
	framebuffer = &boot_framebuffer;
	error = kern_boot_framebuffer(DISCOVER_FRAMEBUFFER_HANDOFF, &boot_framebuffer);
	if (error != 0) {
		nvrtx_stage_mark("console no firmware framebuffer");
		return;
	}

	/* The second memory window is the CPU's view of the VRAM. */
	vram = &device->windows[1];

	/* Reports a picture inside this GPU's VRAM window with its offset there. */
	if (vram->present != 0 &&
	    framebuffer->physical_base >= vram->bus_address &&
	    framebuffer->physical_base - vram->bus_address < vram->size) {
		offset = framebuffer->physical_base - vram->bus_address;
		device->gop_on_gpu = 1;
		device->gop_offset = offset;
		device->gop_width = framebuffer->width;
		device->gop_height = framebuffer->height;
		device->gop_stride = framebuffer->pitch;
		device->gop_format = framebuffer->format;
		nvrtx_stage_mark("console gop %ux%u stride %u in bar%u at +%llx",
		    (unsigned)framebuffer->width,
		    (unsigned)framebuffer->height,
		    (unsigned)framebuffer->pitch,
		    vram->bar,
		    (unsigned long long)offset);
		return;
	}

	/* Otherwise the picture is another device's. */
	nvrtx_stage_mark("console gop %ux%u at %llx is not on this GPU",
	    (unsigned)framebuffer->width,
	    (unsigned)framebuffer->height,
	    (unsigned long long)framebuffer->physical_base);

	/* Succeeded: the picture's place is in the log. */
	return;
}

/*
 * Gives back everything identify took, in the reverse order.
 */
void
nvrtx_n0_release(
	struct nvrtx_device *device)
{
	/* Unmaps the register page. */
	if (device->registers.address != NULL) {
		drv_pci_device_unmap_bar(device->pci, &device->registers);
		device->registers.address = NULL;
	}

	/* Releases the register window's claim. */
	if (device->registers_claimed != 0) {
		drv_pci_device_release_bar(device->pci, device->windows[0].bar);
		device->registers_claimed = 0;
	}

	/* Restores the command bits the firmware left. */
	if (device->enable_saved != 0) {
		(void)drv_pci_device_restore_enable_state(device->pci, &device->enable_state);
		device->enable_saved = 0;
	}
}

/*
 * Records the memory windows in BAR order and marks each one.
 *
 * The first is the register window, the second the VRAM window, the third
 * the instance-memory window; I/O BARs are skipped.
 */
static void
discover_windows(
	struct nvrtx_device *device)
{
	struct drv_pci_bar bar;
	struct nvrtx_window *window;
	unsigned count;
	unsigned found;
	unsigned index;
	int error;

	/* Walks every BAR slot; the upper half of a 64-bit BAR reads as no BAR. */
	count = drv_pci_device_bar_count(device->pci);
	found = 0U;
	for (index = 0U; index < count; index++) {
		/* Takes the next memory BAR while there is room for it. */
		if (found == NVRTX_WINDOW_COUNT)
			break;

		/* Reads the slot; an empty slot is skipped. */
		error = drv_pci_device_bar(device->pci, index, &bar);
		if (error != 0)
			continue;

		/* An I/O BAR is the legacy VGA port range, not one of the windows. */
		if (bar.type != DRV_PCI_BAR_MEMORY32 && bar.type != DRV_PCI_BAR_MEMORY64)
			continue;

		/* Records the window's place and size. */
		window = &device->windows[found];
		window->present = 1;
		window->bar = index;
		window->bus_address = bar.bus_address;
		window->size = bar.size;

		/* Records whether the window takes two slots. */
		window->is_64bit = 0;
		if (bar.type == DRV_PCI_BAR_MEMORY64)
			window->is_64bit = 1;

		/* Records whether the window may be mapped write-combined. */
		window->prefetchable = 0;
		if (bar.prefetchable)
			window->prefetchable = 1;

		/* Marks the window and counts it. */
		nvrtx_stage_mark("identify bar%u %s size %lluK at %llx",
		    index,
		    discover_bar_kind(window),
		    (unsigned long long)(window->size >> 10),
		    (unsigned long long)window->bus_address);
		found++;
	}
}

/* Records whether the PCI Express capability advertises a function-level reset. */
static void
discover_flr(
	struct nvrtx_device *device)
{
	unsigned capability;
	uint32_t device_capabilities;
	int error;

	/* Assumes no reset until the capability says otherwise. */
	device->flr = 0;

	/* Finds the PCI Express capability; a function without it has no FLR. */
	error = drv_pci_device_find_capability(device->pci, NVRTX_PCIE_CAPABILITY_ID, &capability);
	if (error != 0)
		return;

	/* Reads the device capabilities and their FLR bit. */
	error = drv_pci_device_config_read32(device->pci,
	    capability + NVRTX_PCIE_DEVICE_CAPABILITIES,
	    &device_capabilities);
	if (error != 0)
		return;

	/* Bit 28 says the function can reset itself alone. */
	if ((device_capabilities & NVRTX_PCIE_DEVICE_CAP_FLR) != 0U)
		device->flr = 1;
}

/* Names the kind of a memory window for its stage mark. */
static const char *
discover_bar_kind(
	const struct nvrtx_window *window)
{
	/* Tells a 64-bit window from a 32-bit one, and the prefetchable ones apart. */
	if (window->is_64bit != 0) {
		if (window->prefetchable != 0)
			return "mem64 pref";

		return "mem64";
	}

	/* A 32-bit window may be prefetchable too. */
	if (window->prefetchable != 0)
		return "mem32 pref";

	/* A plain 32-bit window. */
	return "mem32";
}
