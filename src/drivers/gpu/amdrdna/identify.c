/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Stage A0: finding and identifying an RDNA GPU.
 *
 * The stage records the function's BARs and its resizable-BAR capability,
 * maps the register window, and reads the VRAM size, the GPU's own table of
 * IP blocks, whether the video BIOS posted the board, whether a secure OS
 * already runs on the security processor (a driver ran since power-on),
 * the system-management unit's state, the graphics block's status, where
 * the frame buffer sits in the GPU's address space, and the boot database.
 * It only reads; the only registers written are the address registers of
 * the indirect windows.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/pci/pci.h>
#include <kern/clock.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-private.h"
#include "drivers/gpu/amdrdna/amdrdna-regs.h"

/*
 * How long the stage waits for the board firmware's ready word, and its
 * step: the platform has normally set it long before the driver looks.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c
 * (amdgpu_discovery_read_binary_from_mem: 2000 polls of 1 ms).
 */
#define IDENTIFY_READY_POLLS		2000U
#define IDENTIFY_READY_STEP_US		1000U

/*
 * The bytes of the boot database read at once: its header and a directory
 * of up to 64 entries fit with room to spare.
 */
#define IDENTIFY_DATABASE_BYTES		4096U

/* The bytes of one boot configuration entry. */
#define IDENTIFY_DATABASE_ENTRY_BYTES	8U

static int identify_bars(struct amdrdna_device *device);
static void identify_resize(struct amdrdna_device *device);
static int identify_map(struct amdrdna_device *device);
static int identify_platform_ready(struct amdrdna_device *device);
static int identify_blocks(struct amdrdna_device *device);
static int identify_state(struct amdrdna_device *device);
static void identify_boot_database(struct amdrdna_device *device);
static void identify_mark_blocks(struct amdrdna_device *device);

/*
 * Runs stage A0 on a function the driver is offered.
 *
 * Returns 0 when the function is a GPU of the first RDNA generation that
 * the firmware posted, with everything above recorded; ENODEV for a
 * function that is not an RDNA GPU; ENOTSUP for a later generation or an
 * unknown graphics version; or the error of the step that failed.
 * Whatever was taken is given back by amdrdna_a0_release(), also after a
 * failure.
 */
int
amdrdna_a0_identify(
	struct amdrdna_device *device)
{
	const struct amdrdna_product *product;
	bool allowed;
	int error;

	/* Says which function the stage looks at. */
	amdrdna_mark("A0 begin %04x:%04x rev %02x sub %04x:%04x at %02x:%02x.%u",
	    (unsigned)AMDRDNA_PCI_VENDOR,
	    (unsigned)device->product,
	    (unsigned)device->revision,
	    (unsigned)device->subvendor,
	    (unsigned)device->subproduct,
	    (unsigned)device->address.bus,
	    (unsigned)device->address.device,
	    (unsigned)device->address.function);

	/* A device ID the driver does not know is not an RDNA GPU. */
	product = amdrdna_product_find(device->product);
	if (product == NULL) {
		amdrdna_mark("A0 stop: %04x is not an RDNA GPU", (unsigned)device->product);
		return ENODEV;
	}

	/* Later generations are named and left to the firmware. */
	device->known = product;
	if (product->generation != AMDRDNA_GENERATION_RDNA1) {
		amdrdna_mark("A0 stop: %s (%s) is recognized but not supported yet",
		    product->chip,
		    amdrdna_generation_name(product->generation));
		return ENOTSUP;
	}

	/* Records the BARs; without the register and VRAM windows there is nothing to drive. */
	error = identify_bars(device);
	if (error != 0)
		return error;

	/* Records the resizable-BAR capability, for the log only. */
	identify_resize(device);

	/* Maps the register window. */
	error = identify_map(device);
	if (error != 0)
		return error;

	/* Waits for the board firmware, then reads the VRAM size and the block table. */
	error = identify_platform_ready(device);
	if (error != 0)
		return error;

	/* Reads the GPU's block table, which names the chip. */
	error = identify_blocks(device);
	if (error != 0)
		return error;

	/* Reads the post, security-processor, SMU and frame-buffer state. */
	error = identify_state(device);
	if (error != 0)
		return error;

	/* Reads the boot database, which may be absent. */
	identify_boot_database(device);

	/* Marks the result. */
	amdrdna_mark("A0 ok gc=%u.%u.%u %s vram=%uMiB bar0=%lluMiB rebar=0x%x/0x%x posted=%u sos=%u smu=0x%x grbm=0x%08x fb=0x%llx rtdb=%u",
	    (unsigned)device->blocks.blocks[AMDRDNA_BLOCK_GC].major,
	    (unsigned)device->blocks.blocks[AMDRDNA_BLOCK_GC].minor,
	    (unsigned)device->blocks.blocks[AMDRDNA_BLOCK_GC].step,
	    device->chip,
	    (unsigned)device->vram_mib,
	    (unsigned long long)(device->vram_window_bytes >> 20),
	    (unsigned)device->resize_sizes,
	    (unsigned)device->resize_control,
	    device->posted ? 1U : 0U,
	    device->secure_os_word != 0U ? 1U : 0U,
	    (unsigned)device->smu_state,
	    (unsigned)device->engine_status,
	    (unsigned long long)device->frame_buffer_base,
	    device->boot_database.present ? 1U : 0U);
	identify_mark_blocks(device);

	/*
	 * A secure OS left by an earlier driver keeps the stages that write
	 * from running, unless the boot lets the driver reset the GPU before
	 * stage A2 (amdrdna.reset=1).
	 */
	allowed = amdrdna_reset_allowed(device);
	if (device->secure_os_word != 0U && allowed)
		amdrdna_mark("A0 note: sos alive (0x%08x); the GPU is reset before A2 (amdrdna.reset=1)", (unsigned)device->secure_os_word);
	else if (device->secure_os_word != 0U)
		amdrdna_mark("A0 hold: sos alive (0x%08x), cold power cycle needed; only A1 and A1v run", (unsigned)device->secure_os_word);

	/* Succeeded: the GPU is identified and its registers stay mapped. */
	return 0;
}

/*
 * Gives back what stage A0 took, in the reverse order.
 */
void
amdrdna_a0_release(
	struct amdrdna_device *device)
{
	/* Unmaps the register window. */
	if (device->registers.address != NULL) {
		drv_pci_device_unmap_bar(device->pci, &device->registers);
		device->registers.address = NULL;
	}

	/* Releases the register window's claim. */
	if (device->registers_claimed) {
		drv_pci_device_release_bar(device->pci, AMDRDNA_BAR_REGISTERS);
		device->registers_claimed = false;
	}

	/* Restores the command bits the firmware left. */
	if (device->enable_saved) {
		(void)drv_pci_device_restore_enable_state(device->pci, &device->enable_state);
		device->enable_saved = false;
	}
}

/* Records the VRAM, doorbell and register BARs. */
static int
identify_bars(
	struct amdrdna_device *device)
{
	struct drv_pci_bar bar;
	int error;

	/* The VRAM window must be a memory BAR. */
	error = drv_pci_device_bar(device->pci, AMDRDNA_BAR_VRAM, &bar);
	if (error != 0 ||
	    bar.type == DRV_PCI_BAR_NONE ||
	    bar.type == DRV_PCI_BAR_IO) {
		amdrdna_mark("A0 fail: no VRAM window");
		return ENODEV;
	}

	/* Records where the VRAM window lies and how much it shows. */
	device->vram_bus = bar.bus_address;
	device->vram_window_bytes = bar.size;

	/* The doorbell BAR is recorded for the log; a missing one is not an error yet. */
	error = drv_pci_device_bar(device->pci, AMDRDNA_BAR_DOORBELL, &bar);
	if (error == 0)
		device->doorbell_bytes = bar.size;

	/* The register window must be a memory BAR large enough for the registers the driver uses. */
	error = drv_pci_device_bar(device->pci, AMDRDNA_BAR_REGISTERS, &bar);
	if (error != 0 ||
	    bar.type == DRV_PCI_BAR_NONE ||
	    bar.type == DRV_PCI_BAR_IO) {
		amdrdna_mark("A0 fail: no register window");
		return ENODEV;
	}

	/* A window smaller than the registers the driver uses is not this GPU's register window. */
	if (bar.size < AMDRDNA_REGISTER_WINDOW_BYTES) {
		amdrdna_mark("A0 fail: register window of %llu bytes", (unsigned long long)bar.size);
		return ENODEV;
	}

	/* Records the register window's size. */
	device->register_bytes = bar.size;

	/* Marks the windows. */
	amdrdna_mark("A0 bar0 %lluMiB at 0x%llx, bar2 %lluKiB, bar5 %lluKiB",
	    (unsigned long long)(device->vram_window_bytes >> 20),
	    (unsigned long long)device->vram_bus,
	    (unsigned long long)(device->doorbell_bytes >> 10),
	    (unsigned long long)(device->register_bytes >> 10));

	/* Succeeded: the windows are recorded. */
	return 0;
}

/*
 * Records the resizable-BAR capability of the VRAM window: the sizes it
 * offers and its control word (the size the firmware chose).
 */
static void
identify_resize(
	struct amdrdna_device *device)
{
	uint32_t control;
	uint32_t sizes;
	unsigned capability;
	unsigned count;
	unsigned entry;
	int error;

	/* A function without the capability keeps its window's size. */
	error = drv_pci_device_find_extended_capability(device->pci, AMDRDNA_PCI_RESIZE_CAPABILITY, 0U, &capability);
	if (error != 0)
		return;

	/* The first entry's control word says how many entries there are. */
	device->resize_capability = capability;
	error = drv_pci_device_config_read32(device->pci, capability + AMDRDNA_PCI_RESIZE_CONTROL, &control);
	if (error != 0)
		return;

	/* The number of resizable BARs the capability lists. */
	count = (control >> AMDRDNA_PCI_RESIZE_CONTROL_COUNT_SHIFT) & AMDRDNA_PCI_RESIZE_CONTROL_COUNT_MASK;

	/* Finds the entry of the VRAM window. */
	for (entry = 0U; entry < count; entry++) {
		/* Reads the entry's control word. */
		error = drv_pci_device_config_read32(device->pci,
		    capability + AMDRDNA_PCI_RESIZE_CONTROL + entry * AMDRDNA_PCI_RESIZE_ENTRY_BYTES,
		    &control);
		if (error != 0)
			return;

		/* Skips the entry of another BAR. */
		if ((control & AMDRDNA_PCI_RESIZE_CONTROL_INDEX) != AMDRDNA_BAR_VRAM)
			continue;

		/* Records the sizes the window offers and its control word. */
		error = drv_pci_device_config_read32(device->pci,
		    capability + AMDRDNA_PCI_RESIZE_SIZES + entry * AMDRDNA_PCI_RESIZE_ENTRY_BYTES,
		    &sizes);
		if (error != 0)
			return;

		/* Records the window's offered sizes and its control word. */
		device->resize_sizes = sizes;
		device->resize_control = control;
		break;
	}
}

/* Lets the function decode memory, claims the register window and maps it. */
static int
identify_map(
	struct amdrdna_device *device)
{
	int error;

	/* Saves the command bits, so the release gives the function back as the firmware left it. */
	error = drv_pci_device_save_enable_state(device->pci, &device->enable_state);
	if (error != 0) {
		amdrdna_mark("A0 fail: command bits not saved (%d)", error);
		return error;
	}

	/* The release now restores the saved bits. */
	device->enable_saved = true;

	/* Lets the registers decode; the firmware's console has it on already. */
	error = drv_pci_device_enable_memory(device->pci);
	if (error != 0) {
		amdrdna_mark("A0 fail: memory decoding not enabled (%d)", error);
		return error;
	}

	/* Claims the register window, so no other driver maps it. */
	error = drv_pci_device_claim_bar(device->pci, AMDRDNA_BAR_REGISTERS);
	if (error != 0) {
		amdrdna_mark("A0 fail: register window not claimed (%d)", error);
		return error;
	}

	/* The release now gives the claim back. */
	device->registers_claimed = true;

	/* Maps the part of the window the registers live in, uncached. */
	error = drv_pci_device_map_bar_region(device->pci,
	    AMDRDNA_BAR_REGISTERS,
	    0U,
	    AMDRDNA_REGISTER_WINDOW_BYTES,
	    DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE | DRV_PCI_MAP_NOCACHE,
	    &device->registers);
	if (error != 0) {
		amdrdna_mark("A0 fail: registers not mapped (%d)", error);
		return error;
	}

	/* Succeeded: the registers are mapped. */
	return 0;
}

/* Waits for the board firmware's ready word, then reads the VRAM size. */
static int
identify_platform_ready(
	struct amdrdna_device *device)
{
	unsigned poll;
	uint32_t size;

	/* Polls the ready word; the platform has normally set it already. */
	for (poll = 0U; poll < IDENTIFY_READY_POLLS; poll++) {
		/* Stops once the done bit is set. */
		device->platform_ready_word = amdrdna_read(device, AMDRDNA_PSP_REG_EARLY_INIT_DONE);
		if ((device->platform_ready_word & AMDRDNA_PSP_DONE) != 0U)
			break;

		/* Waits one step before the next poll. */
		kern_usleep_range(IDENTIFY_READY_STEP_US, IDENTIFY_READY_STEP_US * 2U);
	}

	/* Reports a board whose firmware never came up. */
	if ((device->platform_ready_word & AMDRDNA_PSP_DONE) == 0U) {
		amdrdna_mark("A0 fail: board firmware not ready (0x%08x)", (unsigned)device->platform_ready_word);
		return EIO;
	}

	/* Reads the VRAM size; zero or all ones means the GPU does not answer. */
	size = amdrdna_read(device, AMDRDNA_NBIO_REG_VRAM_SIZE_MIB);
	if (size == 0U || size == 0xffffffffU) {
		amdrdna_mark("A0 fail: VRAM size unreadable (0x%08x)", (unsigned)size);
		return EIO;
	}

	/* Records the VRAM size in MiB and in bytes. */
	device->vram_mib = size;
	device->vram_bytes = (uint64_t)size << 20;

	/* Succeeded: the board is up and its VRAM size is known. */
	return 0;
}

/* Reads the block table near the top of VRAM and names the chip. */
static int
identify_blocks(
	struct amdrdna_device *device)
{
	const struct amdrdna_ip *graphics;
	uint8_t *table;
	int error;

	/* A buffer for the table. */
	table = kern_malloc(AMDRDNA_BLOCK_TABLE_BYTES);
	if (table == NULL)
		return ENOMEM;

	/* Reads the table through the indirect VRAM window and parses it. */
	amdrdna_vram_read(device, device->vram_bytes - AMDRDNA_BLOCK_TABLE_FROM_TOP, table, AMDRDNA_BLOCK_TABLE_BYTES);
	error = amdrdna_block_table_parse(table, AMDRDNA_BLOCK_TABLE_BYTES, &device->blocks);
	kern_free(table);
	if (error != 0) {
		amdrdna_mark("A0 fail: block table unreadable (%d)", error);
		return error;
	}

	/* The graphics block's version names the chip. */
	graphics = &device->blocks.blocks[AMDRDNA_BLOCK_GC];
	device->chip = amdrdna_graphics_chip_name(graphics);
	if (device->chip == NULL) {
		amdrdna_mark("A0 stop: graphics %u.%u.%u is not a chip the driver knows",
		    (unsigned)graphics->major,
		    (unsigned)graphics->minor,
		    (unsigned)graphics->step);
		return ENOTSUP;
	}

	/* Only the first RDNA generation (graphics 10.1) is driven. */
	if (graphics->major != 10U || graphics->minor != 1U) {
		amdrdna_mark("A0 stop: %s (graphics %u.%u.%u) is not supported yet",
		    device->chip,
		    (unsigned)graphics->major,
		    (unsigned)graphics->minor,
		    (unsigned)graphics->step);
		return ENOTSUP;
	}

	/* Succeeded: the blocks are known and the chip is named. */
	return 0;
}

/* Reads the post, security-processor, SMU, graphics and frame-buffer state. */
static int
identify_state(
	struct amdrdna_device *device)
{
	uint32_t index;
	uint32_t word;
	int error;

	/* The video BIOS's scratch words: the post bit and the driver-owns-display bit. */
	error = amdrdna_block_index(device, AMDRDNA_BLOCK_NBIO, AMDRDNA_NBIO_WINDOW_SCRATCH,
	    AMDRDNA_NBIO_REG_BIOS_SCRATCH_FIRST + AMDRDNA_BIOS_SCRATCH_POST, &index);
	if (error != 0) {
		amdrdna_mark("A0 fail: no NBIO scratch window");
		return error;
	}

	/* Reads both scratch words and decodes the post bit. */
	device->scratch_post = amdrdna_read(device, index);
	device->scratch_accelerated = amdrdna_read(device, index - AMDRDNA_BIOS_SCRATCH_POST + AMDRDNA_BIOS_SCRATCH_ACCELERATED);
	device->posted = false;
	if ((device->scratch_post & AMDRDNA_BIOS_SCRATCH_7_POSTED) != 0U)
		device->posted = true;

	/* A board the video BIOS did not post would need the driver to post it, which it cannot yet. */
	if (!device->posted) {
		amdrdna_mark("A0 fail: board not posted (scratch 0x%08x)", (unsigned)device->scratch_post);
		return ENOTSUP;
	}

	/* The security processor's boot word and the secure OS's sign of life. */
	error = amdrdna_block_index(device, AMDRDNA_BLOCK_PSP, AMDRDNA_PSP_WINDOW_MAILBOX, AMDRDNA_PSP_REG_BOOT_COMMAND, &index);
	if (error != 0) {
		amdrdna_mark("A0 fail: no PSP mailbox window");
		return error;
	}

	/* Reads the boot loader's command word. */
	device->boot_word = amdrdna_read(device, index);

	/* The word a running secure OS sets. */
	error = amdrdna_block_index(device, AMDRDNA_BLOCK_PSP, AMDRDNA_PSP_WINDOW_MAILBOX, AMDRDNA_PSP_REG_SECURE_OS_ALIVE, &index);
	if (error != 0)
		return error;

	/* Reads the secure OS's word; non-zero means a driver loaded it since power-on. */
	device->secure_os_word = amdrdna_read(device, index);

	/* The SMU's firmware state, through the indirect window. */
	device->smu_state = amdrdna_far_read(device, AMDRDNA_SMU_FAR_FIRMWARE_STATE);

	/* The graphics block's status word. */
	error = amdrdna_block_index(device, AMDRDNA_BLOCK_GC, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_BLOCK_STATUS, &index);
	if (error != 0) {
		amdrdna_mark("A0 fail: no graphics window");
		return error;
	}

	/* Reads the graphics block's status word. */
	device->engine_status = amdrdna_read(device, index);

	/* Where the frame buffer sits in the GPU's address space, in 16 MiB units. */
	error = amdrdna_block_index(device, AMDRDNA_BLOCK_GC, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_FRAME_BUFFER_BOTTOM, &index);
	if (error != 0)
		return error;

	/* Reads the 16 MiB unit and turns it into an address. */
	word = amdrdna_read(device, index) & AMDRDNA_GC_FRAME_BUFFER_MASK;
	device->frame_buffer_base = (uint64_t)word << AMDRDNA_GC_FRAME_BUFFER_SHIFT;

	/* The last 16 MiB unit the frame buffer covers. */
	error = amdrdna_block_index(device, AMDRDNA_BLOCK_GC, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_FRAME_BUFFER_TOP, &index);
	if (error != 0)
		return error;

	/* Reads the 16 MiB unit and turns it into an address. */
	word = amdrdna_read(device, index) & AMDRDNA_GC_FRAME_BUFFER_MASK;
	device->frame_buffer_top = ((uint64_t)word << AMDRDNA_GC_FRAME_BUFFER_SHIFT) | ((1ULL << AMDRDNA_GC_FRAME_BUFFER_SHIFT) - 1U);

	/* Where the frame buffer starts in the VRAM itself. */
	error = amdrdna_block_index(device, AMDRDNA_BLOCK_GC, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_FRAME_BUFFER_PHYSICAL, &index);
	if (error != 0)
		return error;

	/* Reads the 16 MiB unit and turns it into an address. */
	word = amdrdna_read(device, index) & AMDRDNA_GC_FRAME_BUFFER_MASK;
	device->frame_buffer_physical = (uint64_t)word << AMDRDNA_GC_FRAME_BUFFER_SHIFT;

	/* Succeeded: the state is recorded. */
	return 0;
}

/* Reads the boot database below the top of VRAM; an absent or damaged one is only noted. */
static void
identify_boot_database(
	struct amdrdna_device *device)
{
	uint8_t *bytes;
	uint8_t entry[IDENTIFY_DATABASE_ENTRY_BYTES];
	uint64_t start;
	int error;

	/* A buffer for the header and the directory. */
	bytes = kern_malloc(IDENTIFY_DATABASE_BYTES);
	if (bytes == NULL)
		return;

	/* Reads the header and the directory and parses them. */
	start = device->vram_bytes - AMDRDNA_BOOT_DATABASE_FROM_TOP;
	amdrdna_vram_read(device, start, bytes, IDENTIFY_DATABASE_BYTES);
	error = amdrdna_boot_database_parse(bytes, IDENTIFY_DATABASE_BYTES, &device->boot_database);
	kern_free(bytes);
	if (error != 0) {
		amdrdna_mark("A0 boot database damaged (%d)", error);
		return;
	}

	/* Reads a boot configuration entry that lies past the first read on its own. */
	if (device->boot_database.has_config && !device->boot_database.config_read) {
		amdrdna_vram_read(device, start + device->boot_database.config_offset, entry, sizeof(entry));
		amdrdna_boot_database_set_config(&device->boot_database, entry);
	}

	/* Marks what the database says, when there is one. */
	if (device->boot_database.present) {
		amdrdna_mark("A0 boot database v0x%04x entries=%u config=%u bits=0x%08x",
		    (unsigned)device->boot_database.version,
		    (unsigned)device->boot_database.entries,
		    device->boot_database.has_config ? 1U : 0U,
		    (unsigned)device->boot_database.config_bits);
	}
}

/* Marks the versions of the blocks the driver uses and the graphics shape. */
static void
identify_mark_blocks(
	struct amdrdna_device *device)
{
	const struct amdrdna_ip *blocks;
	const struct amdrdna_shader_shape *shape;

	/* The blocks the later stages start. */
	blocks = device->blocks.blocks;
	amdrdna_mark("A0 blocks %u: psp %u.%u.%u smu %u.%u.%u sdma %u.%u.%u ih %u.%u.%u nbio %u.%u.%u hdp %u.%u.%u mmhub %u.%u.%u dcn %u.%u.%u",
	    device->blocks.listed,
	    (unsigned)blocks[AMDRDNA_BLOCK_PSP].major, (unsigned)blocks[AMDRDNA_BLOCK_PSP].minor, (unsigned)blocks[AMDRDNA_BLOCK_PSP].step,
	    (unsigned)blocks[AMDRDNA_BLOCK_SMU].major, (unsigned)blocks[AMDRDNA_BLOCK_SMU].minor, (unsigned)blocks[AMDRDNA_BLOCK_SMU].step,
	    (unsigned)blocks[AMDRDNA_BLOCK_SDMA0].major, (unsigned)blocks[AMDRDNA_BLOCK_SDMA0].minor, (unsigned)blocks[AMDRDNA_BLOCK_SDMA0].step,
	    (unsigned)blocks[AMDRDNA_BLOCK_IH].major, (unsigned)blocks[AMDRDNA_BLOCK_IH].minor, (unsigned)blocks[AMDRDNA_BLOCK_IH].step,
	    (unsigned)blocks[AMDRDNA_BLOCK_NBIO].major, (unsigned)blocks[AMDRDNA_BLOCK_NBIO].minor, (unsigned)blocks[AMDRDNA_BLOCK_NBIO].step,
	    (unsigned)blocks[AMDRDNA_BLOCK_HDP].major, (unsigned)blocks[AMDRDNA_BLOCK_HDP].minor, (unsigned)blocks[AMDRDNA_BLOCK_HDP].step,
	    (unsigned)blocks[AMDRDNA_BLOCK_MMHUB].major, (unsigned)blocks[AMDRDNA_BLOCK_MMHUB].minor, (unsigned)blocks[AMDRDNA_BLOCK_MMHUB].step,
	    (unsigned)blocks[AMDRDNA_BLOCK_DCN].major, (unsigned)blocks[AMDRDNA_BLOCK_DCN].minor, (unsigned)blocks[AMDRDNA_BLOCK_DCN].step);

	/* The graphics shape, when the table carries one. */
	if (device->blocks.has_shape) {
		shape = &device->blocks.shape;
		amdrdna_mark("A0 shape se=%u sa=%u cu/sa=%u rb/se=%u l2=%u wave=%u waves/simd=%u",
		    (unsigned)shape->engines,
		    (unsigned)shape->arrays_per_engine,
		    (unsigned)(shape->pairs_per_array * 2U),
		    (unsigned)shape->backends_per_engine,
		    (unsigned)shape->cache_channels,
		    (unsigned)shape->wave_lanes,
		    (unsigned)shape->waves_per_simd);
	}
}
