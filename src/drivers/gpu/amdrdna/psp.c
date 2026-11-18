/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Stage A3: the interrupt ring on, and the security processor's firmware.
 *
 * The security processor (PSP) starts every other engine's firmware: the
 * driver hands its boot loader the key database, the system driver and
 * the secure OS one at a time, then talks to the secure OS through a ring
 * of command frames.  Through the ring it reads the firmware's table of
 * contents (which says how large a trusted region of VRAM the programs
 * need), loads the system-management unit's program, sets up the trusted
 * region, loads the copy engines', the command processors' and the
 * run-list controller's programs, starts the run-list controller's load
 * of the graphics programs, and loads the display security application.
 *
 * The firmware files are read from the root file system first and checked
 * before anything is written to the processor.  Once the secure OS runs,
 * only a cold power cycle takes it away: the stop at shutdown unloads what
 * the driver loaded and destroys the ring, but the secure OS stays.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/generic/dma.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-private.h"
#include "drivers/gpu/amdrdna/amdrdna-regs.h"
#include "drivers/gpu/amdrdna/amdrdna-vcn.h"

/* The firmware files the stage reads, and the largest one it takes. */
#define PSP_FILES			11U
#define PSP_FILE_LIMIT			(1024U * 1024U)

/* The longest file name: the chip's name and a suffix. */
#define PSP_NAME_BYTES			48U

/*
 * The private buffer's size and alignment (1 MiB), the alignment of the
 * trusted region (1 MiB), and the page.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_psp.h (PSP_1_MEG, PSP_TMR_ALIGNMENT).
 */
#define PSP_PRIVATE_BYTES		0x100000U
#define PSP_REGION_ALIGNMENT		0x100000U
#define PSP_PAGE_BYTES			0x1000U

/* The three VRAM pages of the ring, the fence and the command buffer, from ring_vram. */
#define PSP_FENCE_PAGE			(1U * PSP_PAGE_BYTES)
#define PSP_COMMAND_PAGE		(2U * PSP_PAGE_BYTES)
#define PSP_PAGES_BYTES			(3U * PSP_PAGE_BYTES)

/*
 * The waits: the boot loader's ready bit, polled each millisecond for 2 s
 * (Linux waits 20 times 100 ms); the secure OS's sign of life and the
 * ring's handshake, polled for 1 s after a 20 ms pause; a command's fence,
 * polled 20000 times between 10 and 100 microseconds apart.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/psp_v11_0.c (psp_v11_0_wait_for_bootloader,
 * psp_v11_0_bootloader_load_sos, psp_v11_0_ring_create: the 20 ms delay)
 * and amdgpu_psp.c (psp_cmd_submit_buf: psp_timeout and the 10 to 100
 * microsecond sleep).
 */
#define PSP_BOOT_POLLS			2000U
#define PSP_POLL_US			1000U
#define PSP_HANDSHAKE_POLLS		1000U
#define PSP_HANDSHAKE_PAUSE_US		20000U
#define PSP_FENCE_POLLS			20000U
#define PSP_FENCE_SLEEP_MIN_US		10U
#define PSP_FENCE_SLEEP_MAX_US		100U

/* One firmware file the stage reads: its name after the chip's, and how it is cut. */
struct psp_file {
	const char *suffix;
	enum amdrdna_firmware_kind kind;
};

/* One program the secure OS loads: the file and piece it comes from, its type, and its name in the log. */
struct psp_program {
	unsigned file;
	unsigned piece;
	uint32_t type;
	const char *name;
};

/* The files the stage reads and what they hold, read once per stage. */
struct psp_files {
	struct amdrdna_blob blobs[PSP_FILES];
	struct amdrdna_firmware_layout layouts[PSP_FILES];
};

/* The files' indexes in psp_file_table. */
#define PSP_FILE_SECURE			0U
#define PSP_FILE_APPLICATION		1U

/* The files, in the order they are read. */
static const struct psp_file psp_file_table[PSP_FILES] = {
	{ "sos", AMDRDNA_FIRMWARE_SECURE },
	{ "asd", AMDRDNA_FIRMWARE_WHOLE },
	{ "smc", AMDRDNA_FIRMWARE_WHOLE },
	{ "sdma", AMDRDNA_FIRMWARE_WHOLE },
	{ "sdma1", AMDRDNA_FIRMWARE_WHOLE },
	{ "ce", AMDRDNA_FIRMWARE_WHOLE },
	{ "pfp", AMDRDNA_FIRMWARE_WHOLE },
	{ "me", AMDRDNA_FIRMWARE_WHOLE },
	{ "mec", AMDRDNA_FIRMWARE_COMPUTE },
	{ "mec2", AMDRDNA_FIRMWARE_COMPUTE },
	{ "rlc", AMDRDNA_FIRMWARE_RUN_LIST }
};

/*
 * The programs, in the order the secure OS loads them: the
 * system-management unit's first (before the trusted region is set up),
 * then the copy engines', the command processors' (the compute processors'
 * without their jump tables), the run-list controller's three lists and
 * its program last, after which the run-list controller loads the
 * graphics programs.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c (psp_hw_start, psp_load_fw,
 * psp_load_non_psp_fw, fw_load_skip_check: the order of the ucode IDs,
 * the jump tables skipped with the run-list autoload, and for MP0 11.0.0
 * the SMU program before the trusted region) and amdgpu_ucode.h (enum
 * AMDGPU_UCODE_ID).
 */
static const struct psp_program psp_program_table[AMDRDNA_PSP_PROGRAMS] = {
	{ 2U, 0U, AMDRDNA_PROGRAM_SMU, "smu" },
	{ 3U, 0U, AMDRDNA_PROGRAM_COPY0, "sdma0" },
	{ 4U, 0U, AMDRDNA_PROGRAM_COPY1, "sdma1" },
	{ 5U, 0U, AMDRDNA_PROGRAM_CE, "ce" },
	{ 6U, 0U, AMDRDNA_PROGRAM_PFP, "pfp" },
	{ 7U, 0U, AMDRDNA_PROGRAM_ME, "me" },
	{ 8U, AMDRDNA_COMPUTE_PROGRAM, AMDRDNA_PROGRAM_COMPUTE, "mec" },
	{ 9U, AMDRDNA_COMPUTE_PROGRAM, AMDRDNA_PROGRAM_COMPUTE, "mec2" },
	{ 10U, AMDRDNA_RUN_LIST_CONTROL, AMDRDNA_PROGRAM_LIST_CONTROL, "rlc-control" },
	{ 10U, AMDRDNA_RUN_LIST_GPM, AMDRDNA_PROGRAM_LIST_GPM, "rlc-gpm" },
	{ 10U, AMDRDNA_RUN_LIST_SRM, AMDRDNA_PROGRAM_LIST_SRM, "rlc-srm" },
	{ 10U, AMDRDNA_RUN_LIST_PROGRAM, AMDRDNA_PROGRAM_RUN_LIST, "rlc" }
};

static int psp_read_files(struct amdrdna_device *device, struct psp_files *files);
static void psp_video_read(struct amdrdna_device *device);
static void psp_video_load(struct amdrdna_device *device);
static void psp_free_files(struct psp_files *files);
static int psp_buffers(struct amdrdna_device *device, const struct psp_files *files);
static int psp_start(struct amdrdna_device *device, const struct psp_files *files);
static int psp_boot(struct amdrdna_device *device, const struct psp_files *files);
static int psp_boot_piece(struct amdrdna_device *device, const uint8_t *bytes, uint32_t length, uint32_t command, const char *name);
static int psp_open_ring(struct amdrdna_device *device);
static int psp_load_programs(struct amdrdna_device *device);
static int psp_load_one(struct amdrdna_device *device, unsigned index);
static int psp_region(struct amdrdna_device *device, const struct psp_files *files);
static int psp_application(struct amdrdna_device *device, const struct psp_files *files);
static int psp_submit(struct amdrdna_device *device, const uint8_t *command, const char *name, struct amdrdna_psp_reply *reply);
static void psp_copy_private(struct amdrdna_device *device, const uint8_t *bytes, uint32_t length);
static int psp_wait(struct amdrdna_device *device, uint32_t offset, uint32_t mask, uint32_t value, unsigned polls);
static uint32_t psp_read(struct amdrdna_device *device, uint32_t offset);
static void psp_write(struct amdrdna_device *device, uint32_t offset, uint32_t value);
static uint64_t psp_vram_address(struct amdrdna_device *device, uint64_t offset);

/*
 * Runs stage A3: reads and checks the firmware files, turns the interrupt
 * ring and the hubs' fault interrupts on, boots the secure OS, creates its
 * ring, and has it load every program.
 *
 * Returns 0 when every program is loaded, EBUSY when a secure OS already
 * runs, the error of the file or the buffer that failed, or ETIMEDOUT
 * for a handshake that never came.  A failure after the boot loader took
 * the first piece runs the stop once; the next boot needs a cold power
 * cycle.
 */
int
amdrdna_a3_psp(
	struct amdrdna_device *device)
{
	struct psp_files *files;
	struct amdrdna_psp *psp;
	uint32_t word;
	int error;

	/* Says the stage starts. */
	amdrdna_mark("A3 begin");
	psp = &device->psp;

	/* A secure OS left by an earlier driver takes no other; only a cold power cycle removes it. */
	word = psp_read(device, AMDRDNA_PSP_REG_SECURE_OS_ALIVE);
	if (word != 0U) {
		amdrdna_mark("A3 fail: sos alive (0x%08x), cold power cycle needed", (unsigned)word);
		return EBUSY;
	}

	/* The record of the files read. */
	files = kern_calloc(1U, sizeof(*files));
	if (files == NULL)
		return ENOMEM;

	/* Reads and checks every file before the processor is touched. */
	error = psp_read_files(device, files);
	if (error != 0) {
		psp_free_files(files);
		return error;
	}

	/* Reads the video engine's file when the boot asks for the engine; its absence is no failure. */
	amdrdna_vcn_parameters(device);
	psp_video_read(device);

	/* Takes the buffers and copies the programs into theirs. */
	error = psp_buffers(device, files);
	if (error != 0) {
		psp_free_files(files);
		return error;
	}

	/* Turns the interrupt ring on, and the hubs' fault interrupts with it. */
	amdrdna_ih_enable(device);
	amdrdna_fault_interrupts(device, true);
	amdrdna_mark("A3 ih on, fault interrupts on, signals so far %llu", (unsigned long long)device->interrupts.signals);

	/* Boots the secure OS and has it load every program. */
	error = psp_start(device, files);

	/* The files are copied where the processor reads them; the kernel copies go. */
	psp_free_files(files);

	/* A failure after the boot loader took a piece undoes what was loaded, once. */
	if (error != 0) {
		amdrdna_psp_stop(device);
		return error;
	}

	/* Marks the result. */
	amdrdna_mark("A3 ok ih=ring0 msi=%llu sos=0x%08x tmr=%uKiB fw=%u asd=%u warnings=%u",
	    (unsigned long long)device->interrupts.signals,
	    (unsigned)psp->secure_os_version,
	    (unsigned)(psp->region_bytes >> 10),
	    psp->loaded,
	    (unsigned)psp->application_session,
	    psp->warnings);

	/* Succeeded: every program is loaded and the run-list controller has started. */
	return 0;
}

/*
 * Undoes stage A3 as far as it got: unloads the display security
 * application, tears down the trusted region and destroys the secure OS's
 * ring.  The secure OS itself stays until a cold power cycle.  Running it
 * twice does nothing the second time.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c (psp_hw_fini, psp_asd_terminate,
 * psp_tmr_terminate) and psp_v11_0.c (psp_v11_0_ring_stop).
 */
void
amdrdna_psp_stop(
	struct amdrdna_device *device)
{
	uint8_t command[AMDRDNA_PSP_COMMAND_BYTES];
	struct amdrdna_psp_reply reply;
	struct amdrdna_psp *psp;
	const char *answer;
	int error;

	/* Nothing to undo without a ring. */
	psp = &device->psp;
	if (!psp->ring_up)
		return;

	/* Unloads the display security application. */
	if (psp->application_loaded) {
		amdrdna_psp_command_unload(command, psp->application_session);
		(void)psp_submit(device, command, "unload asd", &reply);
		psp->application_loaded = false;
	}

	/* Tears down the trusted region. */
	if (psp->region_set) {
		amdrdna_psp_command_plain(command, true);
		(void)psp_submit(device, command, "destroy tmr", &reply);
		psp->region_set = false;
	}

	/* Destroys the ring, and waits for the secure OS to say so. */
	psp_write(device, AMDRDNA_PSP_REG_RING_CONTROL, AMDRDNA_PSP_RING_DESTROY);
	kern_usleep_range(PSP_HANDSHAKE_PAUSE_US, PSP_HANDSHAKE_PAUSE_US + PSP_HANDSHAKE_PAUSE_US / 4U);
	error = psp_wait(device, AMDRDNA_PSP_REG_RING_CONTROL, AMDRDNA_PSP_READY_MASK, AMDRDNA_PSP_READY, PSP_HANDSHAKE_POLLS);
	psp->ring_up = false;

	/* Names how the ring went. */
	answer = "destroyed";
	if (error != 0)
		answer = "not acknowledged";

	/* Marks the undo, for the log. */
	amdrdna_mark("psp stopped: asd unloaded, tmr destroyed, ring %s; the secure OS stays until a cold power cycle", answer);
}

/* Reads every firmware file of the chip and cuts each into its pieces. */
static int
psp_read_files(
	struct amdrdna_device *device,
	struct psp_files *files)
{
	char name[PSP_NAME_BYTES];
	unsigned index;
	int error;

	/* Reads the files in order. */
	for (index = 0U; index < PSP_FILES; index++) {
		/* The file's name: the chip, an underscore, the suffix. */
		(void)kern_snprintf(name, sizeof(name), "%s_%s.bin", device->chip, psp_file_table[index].suffix);

		/* Reads it whole. */
		error = amdrdna_firmware_read(name, PSP_FILE_LIMIT, &files->blobs[index]);
		if (error != 0) {
			amdrdna_mark("A3 fail: firmware %s not read (%d); is amd-navi-firmware installed?", name, error);
			return error;
		}

		/* Cuts it, refusing a header that does not match the file. */
		error = amdrdna_firmware_layout(psp_file_table[index].kind, files->blobs[index].data, files->blobs[index].size, &files->layouts[index]);
		if (error != 0) {
			amdrdna_mark("A3 fail: firmware %s header does not match (%d)", name, error);
			return error;
		}
	}

	/* Records the secure OS's version, and says what was read. */
	device->psp.secure_os_version = files->layouts[PSP_FILE_SECURE].version;
	amdrdna_mark("A3 firmware %u files sos=0x%08x kdb=%uB sysdrv=%uB sos=%uB toc=%uB",
	    PSP_FILES,
	    (unsigned)files->layouts[PSP_FILE_SECURE].version,
	    (unsigned)files->layouts[PSP_FILE_SECURE].pieces[AMDRDNA_SECURE_KEYS].bytes,
	    (unsigned)files->layouts[PSP_FILE_SECURE].pieces[AMDRDNA_SECURE_SYSTEM_DRIVER].bytes,
	    (unsigned)files->layouts[PSP_FILE_SECURE].pieces[AMDRDNA_SECURE_OS].bytes,
	    (unsigned)files->layouts[PSP_FILE_SECURE].pieces[AMDRDNA_SECURE_CONTENTS].bytes);

	/* Succeeded: every file is read and its pieces lie inside it. */
	return 0;
}

/*
 * Reads the video engine's firmware file, when the boot asks for the
 * engine, and copies its program into a buffer of its own for the
 * security processor.  The file is optional: a missing or broken file, or
 * a buffer that cannot be had, leaves the engine off and the stage goes
 * on.  The buffer is taken once for the record's life.
 */
static void
psp_video_read(
	struct amdrdna_device *device)
{
	struct amdrdna_vcn *vcn;
	struct amdrdna_blob blob;
	char name[PSP_NAME_BYTES];
	size_t bytes;
	int error;

	/* Nothing to read unless the boot asks for the engine (amdrdna.vcn=1). */
	vcn = &device->vcn;
	vcn->loaded = false;
	if (!vcn->wanted)
		return;

	/* Reads the file whole; its absence only says the engine stays off. */
	(void)kern_snprintf(name, sizeof(name), "%s_vcn.bin", device->chip);
	error = amdrdna_firmware_read(name, PSP_FILE_LIMIT, &blob);
	if (error != 0) {
		amdrdna_mark("A3 vcn: firmware %s not read (%d); the video engine stays off", name, error);
		return;
	}

	/* Checks its header. */
	error = amdrdna_vcn_firmware_check(blob.data, blob.size, &vcn->firmware);
	if (error != 0) {
		amdrdna_mark("A3 vcn: firmware %s header does not match (%d); the video engine stays off", name, error);
		amdrdna_firmware_free(&blob);
		return;
	}

	/* Takes the program's buffer, a whole number of pages, once for the record's life. */
	if (!vcn->program.allocated) {
		bytes = ((size_t)vcn->firmware.program_bytes + PSP_PAGE_BYTES - 1U) & ~((size_t)PSP_PAGE_BYTES - 1U);
		error = drv_dma_alloc_coherent(device->dma, bytes, PSP_PAGE_BYTES, &vcn->program.dma);
		if (error != 0) {
			amdrdna_mark("A3 vcn: buffer not allocated (%d); the video engine stays off", error);
			amdrdna_firmware_free(&blob);
			return;
		}

		/* The buffer is the GPU's for the record's life. */
		vcn->program.allocated = true;

		/* Maps it for the processor. */
		error = amdrdna_gart_map(device, &vcn->program, PSP_PAGE_BYTES);
		if (error != 0) {
			amdrdna_mark("A3 vcn: buffer not mapped (%d); the video engine stays off", error);
			vcn->program.gpu_address = 0U;
			amdrdna_firmware_free(&blob);
			return;
		}
	}

	/* A buffer kept from an earlier run must still hold the program, and must be mapped. */
	if (vcn->program.dma.size < vcn->firmware.program_bytes || vcn->program.gpu_address == 0U) {
		amdrdna_mark("A3 vcn: buffer unusable for %uB; the video engine stays off", (unsigned)vcn->firmware.program_bytes);
		amdrdna_firmware_free(&blob);
		return;
	}

	/* Copies the program, zero after it. */
	kern_memset(vcn->program.dma.address, 0, vcn->program.dma.size);
	kern_memcpy(vcn->program.dma.address, blob.data + vcn->firmware.program_at, vcn->firmware.program_bytes);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* The kernel's copy of the file goes. */
	amdrdna_firmware_free(&blob);

	/* Marks what was read. */
	amdrdna_mark("A3 vcn firmware %s %uB version 0x%08x (enc %u.%u dec %u vep %u rev %u)",
	    name,
	    (unsigned)vcn->firmware.file_bytes,
	    (unsigned)vcn->firmware.version_word,
	    vcn->firmware.version.encode_major,
	    vcn->firmware.version.encode_minor,
	    vcn->firmware.version.decode,
	    vcn->firmware.version.vep,
	    vcn->firmware.version.revision);
}

/*
 * Asks the secure OS to load the video engine's program, after the
 * run-list controller's load has started, and keeps where it placed the
 * program in the trusted region: the engine runs it from there.  A
 * refusal, or a reply that names no place, leaves the engine off.
 *
 * Procedure referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c
 * (psp_load_non_psp_fw, psp_execute_ucode_load: the reply's firmware
 * address kept for the IP block) and amdgpu_ucode.h (enum
 * AMDGPU_UCODE_ID: the video engine after RLC_G).
 */
static void
psp_video_load(
	struct amdrdna_device *device)
{
	uint8_t command[AMDRDNA_PSP_COMMAND_BYTES];
	struct amdrdna_psp_reply reply;
	struct amdrdna_vcn *vcn;
	int error;

	/* Nothing to load without a program read. */
	vcn = &device->vcn;
	if (!vcn->wanted || vcn->program.gpu_address == 0U)
		return;
	if (vcn->firmware.program_bytes == 0U)
		return;

	/* Asks the secure OS to load it. */
	amdrdna_psp_command_program(command, vcn->program.gpu_address, vcn->firmware.program_bytes, AMDRDNA_PROGRAM_VIDEO);
	error = psp_submit(device, command, "load vcn", &reply);
	if (error != 0) {
		amdrdna_mark("A3 vcn: load not answered (%d); the video engine stays off", error);
		return;
	}

	/* A status other than success, or no place, is a refusal. */
	if (reply.status != 0U || reply.place == 0U) {
		amdrdna_mark("A3 vcn: load refused (status 0x%x, place 0x%llx); the video engine stays off",
		    (unsigned)reply.status,
		    (unsigned long long)reply.place);
		return;
	}

	/* Keeps the place; stage A5 points the engine at it. */
	vcn->place = reply.place;
	vcn->loaded = true;
	amdrdna_mark("A3 vcn loaded at 0x%llx", (unsigned long long)vcn->place);
}

/*
 * Asks the secure OS to apply the video engine's table of register writes
 * (bytes at the GPU address address): the start of the engine under its
 * dynamic power gating, which sets the registers the host may not write
 * once the secure OS loaded the engine's program.
 *
 * Returns 0, EIO when the secure OS did not answer, or EACCES when it
 * refused the table.
 *
 * Procedure referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/amdgpu_vcn.c
 * (amdgpu_vcn_psp_update_sram: a load of the table as the program type
 * VCN0_RAM).
 */
int
amdrdna_psp_video_table(
	struct amdrdna_device *device,
	uint64_t address,
	uint32_t bytes)
{
	uint8_t command[AMDRDNA_PSP_COMMAND_BYTES];
	struct amdrdna_psp_reply reply;
	int error;

	/* Asks the secure OS to apply the table. */
	amdrdna_psp_command_program(command, address, bytes, AMDRDNA_PROGRAM_VIDEO_TABLE);
	error = psp_submit(device, command, "vcn table", &reply);
	if (error != 0)
		return EIO;

	/* A status other than success is a refusal. */
	if (reply.status != 0U) {
		amdrdna_mark("vcn table: refused (status 0x%x)", (unsigned)reply.status);
		return EACCES;
	}

	/* Succeeded: the table is applied. */
	return 0;
}

/* Frees the kernel copies of the files and their record. */
static void
psp_free_files(
	struct psp_files *files)
{
	unsigned index;

	/* Frees each copy that was read. */
	for (index = 0U; index < PSP_FILES; index++)
		amdrdna_firmware_free(&files->blobs[index]);

	/* Frees the record. */
	kern_free(files);
}

/*
 * Takes the buffers the processor reads: the private buffer (system
 * memory, at a GPU address on a 1 MiB boundary), the ring, fence and
 * command pages (VRAM the CPU reaches), and one buffer of system memory
 * per program with the program copied in.  A buffer taken stays the
 * GPU's for the record's life.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c (psp_sw_init: the private
 * buffer in GTT, the fence and command buffers; psp_ring_init) and
 * amdgpu_ucode.c (amdgpu_ucode_init_bo: the programs in GTT).
 */
static int
psp_buffers(
	struct amdrdna_device *device,
	const struct psp_files *files)
{
	struct amdrdna_psp *psp;
	const struct psp_program *program;
	const struct amdrdna_firmware_piece *piece;
	struct amdrdna_system_buffer *buffer;
	volatile uint8_t *pages;
	size_t bytes;
	unsigned index;
	int error;

	/* The security processor's state. */
	psp = &device->psp;

	/* Allocates the private buffer, once for the record's life. */
	if (!psp->private_buffer.allocated) {
		error = drv_dma_alloc_coherent(device->dma, PSP_PRIVATE_BYTES, PSP_PAGE_BYTES, &psp->private_buffer.dma);
		if (error != 0) {
			amdrdna_mark("A3 fail: private buffer not allocated (%d)", error);
			return error;
		}

		/* The buffer is the GPU's for the record's life. */
		psp->private_buffer.allocated = true;

		/* Maps it at a GPU address on a 1 MiB boundary, as the boot loader takes it in 1 MiB units. */
		error = amdrdna_gart_map(device, &psp->private_buffer, PSP_PRIVATE_BYTES);
		if (error != 0) {
			amdrdna_mark("A3 fail: private buffer not mapped (%d)", error);
			return error;
		}
	}

	/* Takes the ring, fence and command pages from the CPU's VRAM pool, once. */
	if (!psp->pages_taken) {
		error = amdrdna_vram_take(device, true, PSP_PAGES_BYTES, PSP_PAGE_BYTES, &psp->ring_vram);
		if (error != 0) {
			amdrdna_mark("A3 fail: no VRAM for the ring (%d)", error);
			return error;
		}

		/* The pages are the processor's for the record's life. */
		psp->pages_taken = true;
	}

	/* Clears the three pages and makes the zeros visible to the processor. */
	pages = amdrdna_vram_cpu(device, psp->ring_vram);
	for (bytes = 0U; bytes < PSP_PAGES_BYTES; bytes += 4U)
		kern_mmio_write32(pages + bytes, 0U);
	amdrdna_host_path_flush(device);

	/* Copies every program into a buffer of its own. */
	for (index = 0U; index < AMDRDNA_PSP_PROGRAMS; index++) {
		program = &psp_program_table[index];
		piece = &files->layouts[program->file].pieces[program->piece];
		buffer = &psp->programs[index];

		/* Allocates the buffer, once for the record's life, a whole number of pages. */
		if (!buffer->allocated) {
			bytes = ((size_t)piece->bytes + PSP_PAGE_BYTES - 1U) & ~((size_t)PSP_PAGE_BYTES - 1U);
			error = drv_dma_alloc_coherent(device->dma, bytes, PSP_PAGE_BYTES, &buffer->dma);
			if (error != 0) {
				amdrdna_mark("A3 fail: buffer for %s not allocated (%d)", program->name, error);
				return error;
			}

			/* The buffer is the GPU's for the record's life. */
			buffer->allocated = true;

			/* Maps it for the processor. */
			error = amdrdna_gart_map(device, buffer, PSP_PAGE_BYTES);
			if (error != 0) {
				amdrdna_mark("A3 fail: buffer for %s not mapped (%d)", program->name, error);
				return error;
			}
		}

		/* A buffer kept from an earlier run must still hold the program. */
		if (buffer->dma.size < piece->bytes) {
			amdrdna_mark("A3 fail: buffer for %s too small", program->name);
			return EINVAL;
		}

		/* Copies the program, zero after it, and records its size. */
		kern_memset(buffer->dma.address, 0, buffer->dma.size);
		kern_memcpy(buffer->dma.address, files->blobs[program->file].data + piece->offset, piece->bytes);
		psp->program_bytes[index] = piece->bytes;
	}

	/* Orders the copies before the processor is told where they are. */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* Marks where the buffers are. */
	amdrdna_mark("A3 buffers private=0x%llx ring=0x%llx fence=0x%llx command=0x%llx programs=%u",
	    (unsigned long long)psp->private_buffer.gpu_address,
	    (unsigned long long)psp_vram_address(device, psp->ring_vram),
	    (unsigned long long)psp_vram_address(device, psp->ring_vram + PSP_FENCE_PAGE),
	    (unsigned long long)psp_vram_address(device, psp->ring_vram + PSP_COMMAND_PAGE),
	    AMDRDNA_PSP_PROGRAMS);

	/* Succeeded: the processor can read every buffer. */
	return 0;
}

/*
 * Boots the secure OS (the key database, the system driver, the secure
 * OS), creates its ring, sets up the trusted region with the
 * system-management unit's program, loads the other programs and the
 * display security application, stopping at the first step that fails.
 */
static int
psp_start(
	struct amdrdna_device *device,
	const struct psp_files *files)
{
	int error;

	/* Boots the secure OS through the boot loader. */
	error = psp_boot(device, files);
	if (error != 0)
		return error;

	/* Creates the secure OS's ring. */
	error = psp_open_ring(device);
	if (error != 0)
		return error;

	/* Sets up the trusted region, with the unit's program loaded first. */
	error = psp_region(device, files);
	if (error != 0)
		return error;

	/* Loads the other programs and starts the run-list controller. */
	error = psp_load_programs(device);
	if (error != 0)
		return error;

	/* Loads the video engine's program when it was read; a refusal leaves the engine off. */
	psp_video_load(device);

	/* Loads the display security application. */
	error = psp_application(device, files);
	if (error != 0)
		return error;

	/* Succeeded: the secure OS runs with every program loaded. */
	return 0;
}

/*
 * Boots the secure OS through the boot loader: the key database, the
 * system driver, then the secure OS, after which the secure OS's sign of
 * life must appear.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c (psp_hw_start) and psp_v11_0.c
 * (psp_v11_0_bootloader_load_kdb, psp_v11_0_bootloader_load_sysdrv,
 * psp_v11_0_bootloader_load_sos).
 */
static int
psp_boot(
	struct amdrdna_device *device,
	const struct psp_files *files)
{
	const struct amdrdna_firmware_layout *layout;
	const uint8_t *bytes;
	uint32_t word;
	unsigned poll;
	int error;

	/* The security processor's file and its pieces. */
	layout = &files->layouts[PSP_FILE_SECURE];
	bytes = files->blobs[PSP_FILE_SECURE].data;

	/* The key database. */
	error = psp_boot_piece(device,
	    bytes + layout->pieces[AMDRDNA_SECURE_KEYS].offset,
	    layout->pieces[AMDRDNA_SECURE_KEYS].bytes,
	    AMDRDNA_PSP_BOOT_KEY_DATABASE,
	    "kdb");
	if (error != 0)
		return error;

	/* The system driver. */
	error = psp_boot_piece(device,
	    bytes + layout->pieces[AMDRDNA_SECURE_SYSTEM_DRIVER].offset,
	    layout->pieces[AMDRDNA_SECURE_SYSTEM_DRIVER].bytes,
	    AMDRDNA_PSP_BOOT_SYSTEM_DRIVER,
	    "sysdrv");
	if (error != 0)
		return error;

	/*
	 * The secure OS.  From here on the secure OS may run, which only a
	 * cold power cycle undoes; booted tells the status so.
	 */
	device->psp.booted = true;
	error = psp_boot_piece(device,
	    bytes + layout->pieces[AMDRDNA_SECURE_OS].offset,
	    layout->pieces[AMDRDNA_SECURE_OS].bytes,
	    AMDRDNA_PSP_BOOT_SECURE_OS,
	    "sos");
	if (error != 0)
		return error;

	/* Waits for the secure OS's sign of life (the word leaves zero) after the pause the handshake needs. */
	kern_usleep_range(PSP_HANDSHAKE_PAUSE_US, PSP_HANDSHAKE_PAUSE_US + PSP_HANDSHAKE_PAUSE_US / 4U);
	word = 0U;
	for (poll = 0U; poll < PSP_HANDSHAKE_POLLS; poll++) {
		/* Stops at the first sign of life. */
		word = psp_read(device, AMDRDNA_PSP_REG_SECURE_OS_ALIVE);
		if (word != 0U)
			break;

		/* Sleeps one step. */
		kern_usleep_range(PSP_POLL_US, PSP_POLL_US + PSP_POLL_US / 4U);
	}

	/* Reports a secure OS that never came up. */
	if (word == 0U) {
		amdrdna_mark("A3 fail: the secure OS gave no sign of life");
		return ETIMEDOUT;
	}

	/* Marks the secure OS's sign of life. */
	amdrdna_mark("A3 sos alive 0x%08x", (unsigned)word);

	/* Succeeded: the secure OS runs. */
	return 0;
}

/*
 * Hands one boot piece to the boot loader: waits for it to be ready,
 * copies the piece into the private buffer, writes its address (in 1 MiB
 * units) and the command, and waits for the loader to be ready again.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/psp_v11_0.c
 * (psp_v11_0_bootloader_load_component, psp_v11_0_wait_for_bootloader).
 */
static int
psp_boot_piece(
	struct amdrdna_device *device,
	const uint8_t *bytes,
	uint32_t length,
	uint32_t command,
	const char *name)
{
	uint32_t word;
	int error;

	/* The boot loader must be ready for a piece. */
	error = psp_wait(device, AMDRDNA_PSP_REG_BOOT_COMMAND, AMDRDNA_PSP_READY_MASK, AMDRDNA_PSP_READY, PSP_BOOT_POLLS);
	if (error != 0) {
		word = psp_read(device, AMDRDNA_PSP_REG_BOOT_COMMAND);
		amdrdna_mark("A3 fail: boot loader not ready for %s (0x%08x)", name, (unsigned)word);
		return error;
	}

	/* Puts the piece where the loader reads it. */
	psp_copy_private(device, bytes, length);

	/* Tells the loader where the piece is and what it is. */
	psp_write(device, AMDRDNA_PSP_REG_BOOT_ARGUMENT, (uint32_t)(device->psp.private_buffer.gpu_address >> AMDRDNA_PSP_ADDRESS_SHIFT));
	psp_write(device, AMDRDNA_PSP_REG_BOOT_COMMAND, command);

	/* The loader is ready again once it took the piece. */
	error = psp_wait(device, AMDRDNA_PSP_REG_BOOT_COMMAND, AMDRDNA_PSP_READY_MASK, AMDRDNA_PSP_READY, PSP_BOOT_POLLS);
	if (error != 0) {
		word = psp_read(device, AMDRDNA_PSP_REG_BOOT_COMMAND);
		amdrdna_mark("A3 fail: boot loader did not take %s (0x%08x)", name, (unsigned)word);
		return error;
	}

	/* Marks the piece. */
	amdrdna_mark("A3 boot %s %uB ok", name, (unsigned)length);

	/* Succeeded: the loader took the piece. */
	return 0;
}

/*
 * Creates the secure OS's kernel-mode ring: waits for the secure OS to be
 * ready, writes the ring's address and size and the create command, and
 * waits for the answer after the pause the handshake needs.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/psp_v11_0.c (psp_v11_0_ring_create, the
 * path without virtualization).
 */
static int
psp_open_ring(
	struct amdrdna_device *device)
{
	struct amdrdna_psp *psp;
	uint64_t ring;
	uint32_t word;
	int error;

	/* The security processor's state. */
	psp = &device->psp;

	/* The secure OS must be ready for its ring. */
	error = psp_wait(device, AMDRDNA_PSP_REG_RING_CONTROL, AMDRDNA_PSP_READY_MASK, AMDRDNA_PSP_READY, PSP_HANDSHAKE_POLLS);
	if (error != 0) {
		word = psp_read(device, AMDRDNA_PSP_REG_RING_CONTROL);
		amdrdna_mark("A3 fail: the secure OS is not ready for its ring (0x%08x)", (unsigned)word);
		return error;
	}

	/* The ring's address and size, then the command. */
	ring = psp_vram_address(device, psp->ring_vram);
	psp_write(device, AMDRDNA_PSP_REG_RING_LOW, (uint32_t)ring);
	psp_write(device, AMDRDNA_PSP_REG_RING_HIGH, (uint32_t)(ring >> 32));
	psp_write(device, AMDRDNA_PSP_REG_RING_BYTES, AMDRDNA_PSP_RING_BYTES);
	psp_write(device, AMDRDNA_PSP_REG_RING_CONTROL, AMDRDNA_PSP_RING_CREATE);

	/* Waits for the answer after the pause. */
	kern_usleep_range(PSP_HANDSHAKE_PAUSE_US, PSP_HANDSHAKE_PAUSE_US + PSP_HANDSHAKE_PAUSE_US / 4U);
	error = psp_wait(device, AMDRDNA_PSP_REG_RING_CONTROL, AMDRDNA_PSP_READY_MASK, AMDRDNA_PSP_READY, PSP_HANDSHAKE_POLLS);
	if (error != 0) {
		word = psp_read(device, AMDRDNA_PSP_REG_RING_CONTROL);
		amdrdna_mark("A3 fail: the ring was not created (0x%08x)", (unsigned)word);
		return error;
	}

	/* The ring is up; the stop destroys it. */
	psp->ring_up = true;
	psp->fence_sequence = 0U;
	amdrdna_mark("A3 ring 0x%llx %uB", (unsigned long long)ring, (unsigned)AMDRDNA_PSP_RING_BYTES);

	/* Succeeded: commands go through the ring from now on. */
	return 0;
}

/*
 * Reads the table of contents (which answers with the trusted region's
 * size), takes the region from the VRAM only the GPU reaches, loads the
 * system-management unit's program, and sets the region up.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c (psp_hw_start, psp_tmr_init,
 * psp_load_toc, psp_load_smu_fw, psp_tmr_load: for MP0 11.0.0 the SMU
 * program comes before the region is set up).
 */
static int
psp_region(
	struct amdrdna_device *device,
	const struct psp_files *files)
{
	uint8_t command[AMDRDNA_PSP_COMMAND_BYTES];
	struct amdrdna_psp_reply reply;
	const struct amdrdna_firmware_piece *contents;
	struct amdrdna_psp *psp;
	uint64_t bytes;
	int error;

	/* The security processor's state. */
	psp = &device->psp;

	/* Reads the table of contents from the private buffer. */
	contents = &files->layouts[PSP_FILE_SECURE].pieces[AMDRDNA_SECURE_CONTENTS];
	psp_copy_private(device, files->blobs[PSP_FILE_SECURE].data + contents->offset, contents->bytes);
	amdrdna_psp_command_contents(command, psp->private_buffer.gpu_address, contents->bytes);
	error = psp_submit(device, command, "load toc", &reply);
	if (error != 0)
		return error;

	/* A table that asks for no region is not one the driver can go on with. */
	if (reply.region_bytes == 0U) {
		amdrdna_mark("A3 fail: the table of contents asks for no trusted region (status 0x%x)", (unsigned)reply.status);
		return EINVAL;
	}

	/* Takes the region, on a 1 MiB boundary, once for the record's life. */
	if (psp->region_bytes == 0U) {
		bytes = ((uint64_t)reply.region_bytes + PSP_PAGE_BYTES - 1U) & ~((uint64_t)PSP_PAGE_BYTES - 1U);
		error = amdrdna_vram_take(device, false, bytes, PSP_REGION_ALIGNMENT, &psp->region_vram);
		if (error != 0) {
			amdrdna_mark("A3 fail: no VRAM for a %uKiB trusted region (%d)", (unsigned)(reply.region_bytes >> 10), error);
			return error;
		}

		/* The region's size, as taken. */
		psp->region_bytes = (uint32_t)bytes;
	}

	/* Marks the region. */
	amdrdna_mark("A3 toc tmr=%uKiB at 0x%llx",
	    (unsigned)(psp->region_bytes >> 10),
	    (unsigned long long)psp_vram_address(device, psp->region_vram));

	/* Loads the system-management unit's program. */
	error = psp_load_one(device, 0U);
	if (error != 0)
		return error;

	/* Sets the region up with its GPU address and its physical VRAM address. */
	amdrdna_psp_command_region(command,
	    psp_vram_address(device, psp->region_vram),
	    psp->region_bytes,
	    device->frame_buffer_physical + psp->region_vram);
	error = psp_submit(device, command, "setup tmr", &reply);
	if (error != 0)
		return error;
	psp->region_set = true;

	/* Succeeded: the region is set up and the unit's program loaded. */
	return 0;
}

/*
 * Loads the remaining programs in order, then starts the run-list
 * controller's load of the graphics programs.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c (psp_load_non_psp_fw,
 * psp_rlc_autoload_start: after the run-list controller's program).
 */
static int
psp_load_programs(
	struct amdrdna_device *device)
{
	uint8_t command[AMDRDNA_PSP_COMMAND_BYTES];
	struct amdrdna_psp_reply reply;
	unsigned index;
	int error;

	/* Every program after the unit's. */
	for (index = 1U; index < AMDRDNA_PSP_PROGRAMS; index++) {
		/* Loads one; a load that fails stops the stage. */
		error = psp_load_one(device, index);
		if (error != 0)
			return error;
	}

	/* Starts the run-list controller's load. */
	amdrdna_psp_command_plain(command, false);
	error = psp_submit(device, command, "autoload rlc", &reply);
	if (error != 0)
		return error;

	/* Succeeded: every program is loaded and the run-list controller started. */
	return 0;
}

/* Loads one program of the table by its index. */
static int
psp_load_one(
	struct amdrdna_device *device,
	unsigned index)
{
	uint8_t command[AMDRDNA_PSP_COMMAND_BYTES];
	struct amdrdna_psp_reply reply;
	const struct psp_program *program;
	struct amdrdna_system_buffer *buffer;
	char name[PSP_NAME_BYTES];
	uint32_t bytes;
	int error;

	/* The program, its buffer and its size. */
	program = &psp_program_table[index];
	buffer = &device->psp.programs[index];
	bytes = device->psp.program_bytes[index];

	/* Asks the secure OS to load it. */
	(void)kern_snprintf(name, sizeof(name), "load %s", program->name);
	amdrdna_psp_command_program(command, buffer->gpu_address, bytes, program->type);
	error = psp_submit(device, command, name, &reply);
	if (error != 0)
		return error;

	/* Counts it. */
	device->psp.loaded++;

	/* Succeeded: the program is in the trusted region. */
	return 0;
}

/*
 * Loads the display security application from the private buffer and
 * records its session.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c (psp_asd_initialize,
 * psp_ta_load).
 */
static int
psp_application(
	struct amdrdna_device *device,
	const struct psp_files *files)
{
	uint8_t command[AMDRDNA_PSP_COMMAND_BYTES];
	struct amdrdna_psp_reply reply;
	const struct amdrdna_firmware_piece *piece;
	struct amdrdna_psp *psp;
	int error;

	/* The security processor's state. */
	psp = &device->psp;

	/* Puts the application in the private buffer and asks for it to be loaded. */
	piece = &files->layouts[PSP_FILE_APPLICATION].pieces[0];
	psp_copy_private(device, files->blobs[PSP_FILE_APPLICATION].data + piece->offset, piece->bytes);
	amdrdna_psp_command_application(command, psp->private_buffer.gpu_address, piece->bytes);
	error = psp_submit(device, command, "load asd", &reply);
	if (error != 0)
		return error;

	/* Records the session the stop unloads. */
	psp->application_session = reply.session;
	psp->application_loaded = true;

	/* Succeeded: the application runs in its session. */
	return 0;
}

/*
 * Submits one command through the ring and waits for its fence: copies
 * the command to the command page, writes the frame at the secure OS's
 * write pointer, flushes the host data path, moves the write pointer, and
 * waits until the fence page holds the frame's value; then reads the
 * reply.  A nonzero status is logged and counted, not refused (some
 * firmware leaves the status unwritten); a fence that never comes is
 * ETIMEDOUT.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c (psp_cmd_submit_buf,
 * psp_ring_cmd_submit) and psp_v11_0.c (psp_v11_0_ring_get_wptr,
 * psp_v11_0_ring_set_wptr).
 */
static int
psp_submit(
	struct amdrdna_device *device,
	const uint8_t *command,
	const char *name,
	struct amdrdna_psp_reply *reply)
{
	uint8_t frame[AMDRDNA_PSP_FRAME_BYTES];
	uint8_t answer[AMDRDNA_PSP_COMMAND_BYTES];
	struct amdrdna_psp *psp;
	volatile uint8_t *pages;
	uint32_t pointer;
	uint32_t index;
	uint32_t next;
	uint32_t value;
	uint32_t fence;
	uint32_t word;
	unsigned poll;
	size_t at;
	int error;

	/* The security processor's state. */
	psp = &device->psp;
	pages = amdrdna_vram_cpu(device, psp->ring_vram);

	/* One command on the ring at a time. */
	mutex_lock(&psp->lock);

	/* Copies the command over a zero command page. */
	for (at = 0U; at < PSP_PAGE_BYTES; at += 4U)
		kern_mmio_write32(pages + PSP_COMMAND_PAGE + at, 0U);
	for (at = 0U; at < AMDRDNA_PSP_COMMAND_BYTES; at += 4U) {
		kern_memcpy(&word, command + at, sizeof(word));
		kern_mmio_write32(pages + PSP_COMMAND_PAGE + at, word);
	}

	/* The frame's place at the secure OS's write pointer. */
	pointer = psp_read(device, AMDRDNA_PSP_REG_RING_WRITE);
	error = amdrdna_psp_ring_step(pointer, &index, &next);
	if (error != 0) {
		mutex_unlock(&psp->lock);
		amdrdna_mark("psp %s: write pointer 0x%x is not at a frame", name, (unsigned)pointer);
		return EIO;
	}

	/* Writes the frame with the next fence value. */
	psp->fence_sequence++;
	value = psp->fence_sequence;
	amdrdna_psp_frame(frame, psp_vram_address(device, psp->ring_vram + PSP_COMMAND_PAGE), psp_vram_address(device, psp->ring_vram + PSP_FENCE_PAGE), value);
	for (at = 0U; at < AMDRDNA_PSP_FRAME_BYTES; at += 4U) {
		kern_memcpy(&word, frame + at, sizeof(word));
		kern_mmio_write32(pages + (size_t)index * AMDRDNA_PSP_FRAME_BYTES + at, word);
	}

	/* Makes the command and the frame visible, then moves the write pointer. */
	amdrdna_host_path_flush(device);
	psp_write(device, AMDRDNA_PSP_REG_RING_WRITE, next);

	/* Waits for the fence, dropping the host path's read cache before each look. */
	fence = 0U;
	for (poll = 0U; poll < PSP_FENCE_POLLS; poll++) {
		/* Reads the fence as the processor wrote it. */
		amdrdna_host_path_drop(device);
		fence = kern_mmio_read32(pages + PSP_FENCE_PAGE);
		if (fence == value)
			break;

		/* Sleeps a little before looking again. */
		kern_usleep_range(PSP_FENCE_SLEEP_MIN_US, PSP_FENCE_SLEEP_MAX_US);
	}

	/* Reads the reply out of the command page. */
	for (at = 0U; at < AMDRDNA_PSP_COMMAND_BYTES; at += 4U) {
		word = kern_mmio_read32(pages + PSP_COMMAND_PAGE + at);
		kern_memcpy(answer + at, &word, sizeof(word));
	}

	mutex_unlock(&psp->lock);

	/* Decodes the reply. */
	amdrdna_psp_reply_read(answer, reply);

	/* Reports a command the secure OS never finished. */
	if (fence != value) {
		amdrdna_mark("psp %s: no fence (0x%x, waited for 0x%x)", name, (unsigned)fence, (unsigned)value);
		return ETIMEDOUT;
	}

	/* Logs and counts a status other than success. */
	if (reply->status != 0U) {
		psp->warnings++;
		amdrdna_mark("psp %s: status 0x%x", name, (unsigned)reply->status);
	} else {
		amdrdna_mark("psp %s ok", name);
	}

	/* Succeeded: the command ran and its reply is read. */
	return 0;
}

/* Puts a piece of firmware at the start of the zeroed private buffer. */
static void
psp_copy_private(
	struct amdrdna_device *device,
	const uint8_t *bytes,
	uint32_t length)
{
	struct amdrdna_system_buffer *buffer;

	/* Zero, then the piece; the caller checked it fits in 1 MiB. */
	buffer = &device->psp.private_buffer;
	kern_memset(buffer->dma.address, 0, PSP_PRIVATE_BYTES);
	kern_memcpy(buffer->dma.address, bytes, length);

	/* Orders the copy before the processor is told about it. */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/*
 * Waits until a mailbox word, masked, reads a value: polls each
 * millisecond, up to the count.  Returns 0, or ETIMEDOUT.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c (psp_wait_for).
 */
static int
psp_wait(
	struct amdrdna_device *device,
	uint32_t offset,
	uint32_t mask,
	uint32_t value,
	unsigned polls)
{
	uint32_t word;
	unsigned poll;

	/* Looks, then sleeps, until the word reads the value. */
	for (poll = 0U; poll < polls; poll++) {
		/* Stops once the word reads the value. */
		word = psp_read(device, offset);
		if ((word & mask) == value)
			return 0;

		/* Sleeps one step. */
		kern_usleep_range(PSP_POLL_US, PSP_POLL_US + PSP_POLL_US / 4U);
	}

	/* The word never read the value. */
	return ETIMEDOUT;
}

/* Reads one of the security processor's mailbox words. */
static uint32_t
psp_read(
	struct amdrdna_device *device,
	uint32_t offset)
{
	uint32_t value;

	/* The words are in the processor's first window. */
	value = amdrdna_block_read(device, AMDRDNA_BLOCK_PSP, AMDRDNA_PSP_WINDOW_MAILBOX, offset);

	/* Succeeded: the word's value. */
	return value;
}

/* Writes one of the security processor's mailbox words. */
static void
psp_write(
	struct amdrdna_device *device,
	uint32_t offset,
	uint32_t value)
{
	/* The words are in the processor's first window. */
	amdrdna_block_write(device, AMDRDNA_BLOCK_PSP, AMDRDNA_PSP_WINDOW_MAILBOX, offset, value);
}

/* The GPU's address of a VRAM offset: the frame buffer's base plus the offset. */
static uint64_t
psp_vram_address(
	struct amdrdna_device *device,
	uint64_t offset)
{
	/* The frame buffer starts at its base in the GPU's space. */
	return device->frame_buffer_base + offset;
}
