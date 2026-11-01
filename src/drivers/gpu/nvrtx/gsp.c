/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GSP boot of the nvrtx driver.
 *
 * Stage layout prepares everything the later stages need before anything is
 * written that cannot be taken back: the firmware files, the DMA memory the GSP
 * reads, the placement of the firmware in the VRAM, and the two FWSEC programs
 * from the VBIOS.  The only writes are the VBIOS windows, which are put back at
 * once.  A failure in layout leaves the GPU as the firmware left it.
 *
 * devinit waits for the GPU's own firmware to finish devinit.  frts runs
 * FWSEC-FRTS, which sets up the protected region at the top of the VRAM. The
 * region is torn down (unload's FWSEC-SB and booter_unload) only after the
 * firmware has run and unloaded; a region without a started firmware stays
 * until the host reboots.  Nothing is retried.  booter queues the system
 * information and the registry, puts the GSP's falcon into RISC-V mode and runs
 * booter_load, which places the firmware in the region and starts it.  gsp-init
 * waits for the firmware's init-done event, performing the CPU sequencer it
 * sends.
 *
 * static-info asks for the static information and allocates the first objects:
 * a client, the device and the subdevice.
 *
 * address-space and later make the address spaces, the channels, the display
 * and the GPU device (their own files).  Where the boot ends short of keeping
 * the GPU up, the worker stops the GSP (unload: the unload call, the GSP's
 * falcon reset, FWSEC-SB, booter_unload) and gives the preparation back, so the
 * GPU ends as the firmware left it.  A GSP kept up is stopped the same way at
 * the reboot or the shutdown of the machine (nvrtx_gsp_shutdown).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/pci/pci.h>
#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-gpu.h"
#include "drivers/gpu/nvrtx/nvrtx-nvdec.h"
#include "drivers/gpu/nvrtx/nvrtx-nvenc.h"

/* The largest small file read whole. */
#define GSP_SMALL_FILE_MAX		0x40000U

/*
 * How long devinit waits for devinit, in microseconds, and the step between its
 * reads.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/devinit/tu102.c (tu102_devinit_wait).
 */
#define GSP_DEVINIT_TIMEOUT_US		4100000U
#define GSP_DEVINIT_STEP_US		2000U

/*
 * How long gsp-init waits for the init-done event, and unload for the unload's
 * reply and the GSP's halt.
 */
#define GSP_INIT_DONE_TIMEOUT_US	10000000U
#define GSP_UNLOAD_TIMEOUT_US		2000000U
#define GSP_UNLOADED_TIMEOUT_US		2000000U

/*
 * The GSP falcon's mailbox 0 reads this once the firmware has unloaded.
 *
 * Value referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/gsp.c (r535_gsp_fini).
 */
#define GSP_UNLOADED			0x80000000U

/*
 * The GSP falcon's registers the start writes: the two mailboxes and the
 * application version.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/gsp.c
 * (r535_gsp_msg_run_cpu_sequencer, r535_gsp_init) and nvkm/subdev/gsp/tu102.c
 * (tu102_gsp_oneinit).
 */
#define GSP_REG_MAILBOX0		(NVRTX_FALCON_GSP_BASE + 0x040U)
#define GSP_REG_MAILBOX1		(NVRTX_FALCON_GSP_BASE + 0x044U)
#define GSP_REG_APP_VERSION		(NVRTX_FALCON_GSP_BASE + 0x080U)

/*
 * The secure scratch register whose bit 26 says the boot has handed over to the
 * firmware.
 *
 * Value referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/gsp.c
 * (r535_gsp_msg_run_cpu_sequencer).
 */
#define GSP_REG_BOOT_SCRATCH		0x1180f8U

/*
 * When gsp-init shows how far the firmware got, in milliseconds after it began
 * to wait.
 */
#define GSP_PROGRESS_POINTS		4U

/* How long the init runs before a test run gives it up on purpose (nvrtx.fail=gsp-init). */
#define GSP_FAIL_INIT_US		200000U

/* The highest user address the resource manager is told of (the 47-bit user half). */
#define GSP_USER_ADDRESS_LIMIT		0x00007ffffffff000ULL

/* The page of the queue memory that lists its pages, counted with the rest. */
#define GSP_QUEUE_PTE_COUNT		(NVRTX_QUEUE_SHARED_BYTES / NVRTX_GSP_PAGE_SIZE)

static int gsp_prepare(struct nvrtx_gpu *gpu);
static int gsp_load_files(struct nvrtx_gpu *gpu);
static int gsp_prepare_image(struct nvrtx_gpu *gpu);
static int gsp_prepare_placement(struct nvrtx_gpu *gpu);
static int gsp_prepare_start(struct nvrtx_gpu *gpu);
static int gsp_prepare_fwsec(struct nvrtx_gpu *gpu);
static int gsp_prepare_falcons(struct nvrtx_gpu *gpu);
static int gsp_devinit_wait(struct nvrtx_gpu *gpu);
static int gsp_region_set(struct nvrtx_gpu *gpu);
static void gsp_finish(struct nvrtx_gpu *gpu);
static int gsp_teardown(struct nvrtx_gpu *gpu);
static void gsp_hold(struct nvrtx_gpu *gpu, enum nvrtx_gsp_hold point, const char *what);
static void gsp_end(struct nvrtx_gpu *gpu);
static int gsp_queue_setup_calls(struct nvrtx_gpu *gpu);
static int gsp_boot_riscv(struct nvrtx_gpu *gpu);
static int gsp_init_done(struct nvrtx_gpu *gpu);
static void gsp_progress(struct nvrtx_gpu *gpu, uint32_t milliseconds);
static uint32_t gsp_queue_word(const struct nvrtx_gpu *gpu, size_t at);
static uint64_t gsp_log_put(const struct nvrtx_gpu *gpu, unsigned index);
static int gsp_stop(struct nvrtx_gpu *gpu);
static int gsp_static_info(struct nvrtx_gpu *gpu);
static int gsp_objects(struct nvrtx_gpu *gpu);

/*
 * Runs the GSP boot as far as this revision goes.
 *
 * Called on the worker thread.  Returns 0 when the boot stopped where it
 * was asked or built to, or the error of the stage that failed; either way
 * the GPU is as the firmware left it.
 */
int
nvrtx_gsp_start(
	struct nvrtx_gpu *gpu)
{
	bool allowed;
	bool wanted;
	bool forced;
	bool resident;
	bool work_wanted;
	int error;

	/* Stops before layout when the boot asks. */
	allowed = nvrtx_stage_allowed("layout");
	if (!allowed) {
		nvrtx_stage_mark("layout skipped (nvrtx.stop=layout)");
		return 0;
	}

	/*
	 * Keeps the firmware's console through the boot; a GPU whose console
	 * cannot be kept stays the firmware's unless the boot forces the GSP
	 * (nvrtx.gsp=1, a test run without a picture to keep).
	 */
	error = nvrtx_console_check(gpu);
	if (error != 0) {
		forced = nvrtx_stage_gsp_forced();
		if (!forced) {
			nvrtx_stage_mark("layout skipped: the console stays the firmware's");
			return 0;
		}
	}

	/* Prepares everything devinit and later need. */
	nvrtx_stage_mark("layout begin");
	error = gsp_prepare(gpu);
	if (error != 0) {
		nvrtx_stage_mark("layout fail (%d); the GPU is untouched", error);
		gpu->state = NVRTX_GSP_FAILED;
		nvrtx_gsp_release(gpu);
		return error;
	}

	gpu->state = NVRTX_GSP_PREPARED;
	nvrtx_stage_mark("layout ok vram %lluM wpr2 %llx+%lluM heap %lluM",
	    (unsigned long long)(gpu->layout.vram_size >> 20),
	    (unsigned long long)gpu->layout.region,
	    (unsigned long long)(gpu->layout.region_size >> 20),
	    (unsigned long long)(gpu->layout.heap_size >> 20));

	/* Stops before devinit when the boot asks. */
	allowed = nvrtx_stage_allowed("devinit");
	if (!allowed) {
		nvrtx_stage_mark("devinit skipped (nvrtx.stop=devinit)");
		gsp_finish(gpu);
		return 0;
	}

	/* Waits for the GPU's firmware to finish devinit. */
	error = gsp_devinit_wait(gpu);
	if (error != 0) {
		gpu->state = NVRTX_GSP_FAILED;
		gsp_finish(gpu);
		return error;
	}

	/* Stops before frts when the boot asks. */
	allowed = nvrtx_stage_allowed("frts");
	if (!allowed) {
		nvrtx_stage_mark("frts skipped (nvrtx.stop=frts)");
		gsp_finish(gpu);
		return 0;
	}

	/* Sets the protected region up with FWSEC-FRTS. */
	error = gsp_region_set(gpu);
	if (error != 0) {
		gsp_finish(gpu);
		return error;
	}

	/*
	 * Stops before booter when the boot asks; the finish reports the region
	 * it leaves.
	 */
	allowed = nvrtx_stage_allowed("booter");
	if (!allowed) {
		nvrtx_stage_mark("booter skipped (nvrtx.stop=booter)");
		gsp_finish(gpu);
		return 0;
	}

	/* Starts the GSP firmware. */
	error = gsp_boot_riscv(gpu);
	if (error != 0) {
		gsp_finish(gpu);
		return error;
	}

	/* Waits for the firmware to say it is up. */
	error = gsp_init_done(gpu);
	if (error != 0) {
		gsp_finish(gpu);
		return error;
	}

	/*
	 * Stops before static-info when the boot asks; the finish stops the GSP
	 * again.
	 */
	allowed = nvrtx_stage_allowed("static-info");
	if (!allowed) {
		nvrtx_stage_mark("static-info skipped (nvrtx.stop=static-info)");
		gsp_finish(gpu);
		return 0;
	}

	/* Reads the static information and allocates the first objects. */
	error = gsp_static_info(gpu);
	if (error == 0)
		error = gsp_objects(gpu);
	if (error != 0) {
		gsp_finish(gpu);
		return error;
	}

	/*
	 * Stops before address-space when the boot asks; the finish stops the
	 * GSP again.
	 */
	allowed = nvrtx_stage_allowed("address-space");
	if (!allowed) {
		nvrtx_stage_mark("address-space skipped (nvrtx.stop=address-space)");
		gsp_finish(gpu);
		return 0;
	}

	/* Makes the first address space. */
	error = nvrtx_space_start(gpu);
	if (error != 0) {
		gsp_finish(gpu);
		return error;
	}

	/* Sets the VRAM allocator and the CPU's window up unless the boot stops before them; neither stops the boot. */
	allowed = nvrtx_stage_allowed("vram-window");
	if (!allowed)
		nvrtx_stage_mark("vram-window skipped (nvrtx.stop=vram-window)");
	if (allowed)
		(void)nvrtx_memory_start(gpu);

	/* Gives a console the window no longer reaches back: the GSP stops and the window's mode is restored. */
	if (gpu->console.kept != 0 && gpu->console.mapped == 0) {
		nvrtx_stage_mark("vram-window: the console is not reached; stopping to give it back");
		gsp_finish(gpu);
		return EIO;
	}

	/* Stops before copy-channel when the boot asks. */
	allowed = nvrtx_stage_allowed("copy-channel");
	if (!allowed) {
		nvrtx_stage_mark("copy-channel skipped (nvrtx.stop=copy-channel)");
		gsp_end(gpu);
		return 0;
	}

	/* Makes the first channel and runs one fence through it. */
	error = nvrtx_channel_start(gpu);
	if (error != 0) {
		gsp_end(gpu);
		return error;
	}

	/* Builds the graphics golden context when the boot asks (nvrtx.gr=1, or the work channel that needs it) and does not stop before it. */
	wanted = nvrtx_stage_gr_wanted();
	work_wanted = nvrtx_stage_work_wanted();
	if (work_wanted)
		wanted = true;
	allowed = nvrtx_stage_allowed("golden-context");
	if (wanted && !allowed)
		nvrtx_stage_mark("golden-context skipped (nvrtx.stop=golden-context)");
	if (wanted && allowed) {
		error = nvrtx_gr_start(gpu);
		if (error != 0) {
			gsp_end(gpu);
			return error;
		}
	}

	/* Runs the first compute program when the boot asks (nvrtx.compute=1) and does not stop before it. */
	wanted = nvrtx_stage_compute_wanted();
	allowed = nvrtx_stage_allowed("compute-test");
	if (wanted && !allowed)
		nvrtx_stage_mark("compute-test skipped (nvrtx.stop=compute-test)");
	if (wanted && allowed) {
		error = nvrtx_compute_start(gpu);
		if (error != 0) {
			gsp_end(gpu);
			return error;
		}
	}

	/* Makes the work channel when the boot asks (nvrtx.work=1) and does not stop before it. */
	allowed = nvrtx_stage_allowed("work-channel");
	if (work_wanted && !allowed)
		nvrtx_stage_mark("work-channel skipped (nvrtx.stop=work-channel)");
	if (work_wanted && allowed) {
		error = nvrtx_work_start(gpu);
		if (error != 0) {
			gsp_end(gpu);
			return error;
		}
	}

	/* Asks whether the boot wants the video decoder's channel (nvrtx.video=1); without it the GPU decodes no video. */
	wanted = nvrtx_stage_video_wanted();
	allowed = nvrtx_stage_allowed("nvdec-channel");

	/* Says so when the boot stops before the decoder's channel. */
	if (wanted && !allowed)
		nvrtx_stage_mark("nvdec-channel skipped (nvrtx.stop=nvdec-channel)");

	/* Makes the decoder's channel once the work channel is ready; a failure leaves the boot going without video. */
	if (wanted &&
	    allowed &&
	    gpu->work.ready != 0) {
		error = nvrtx_nvdec_start(gpu);
		if (error != 0)
			nvrtx_stage_mark("nvdec-channel: no video decoder (%d); the boot goes on", error);
	}

	/* Says so when the boot stops before the encoder's channel. */
	allowed = nvrtx_stage_allowed("nvenc-channel");
	if (wanted && !allowed)
		nvrtx_stage_mark("nvenc-channel skipped (nvrtx.stop=nvenc-channel)");

	/* Makes the encoder's channel once the work channel is ready, with or without the decoder; a failure leaves the boot going without encoding. */
	if (wanted &&
	    allowed &&
	    gpu->work.ready != 0) {
		error = nvrtx_nvenc_start(gpu);
		if (error != 0)
			nvrtx_stage_mark("nvenc-channel: no video encoder (%d); the boot goes on", error);
	}

	/* Stops before display when the boot asks. */
	allowed = nvrtx_stage_allowed("display");
	if (!allowed) {
		nvrtx_stage_mark("display skipped (nvrtx.stop=display)");
		gsp_end(gpu);
		return 0;
	}

	/* Brings the display up under the GSP and reads what the firmware lit. */
	error = nvrtx_display_start(gpu);
	if (error != 0) {
		gsp_end(gpu);
		return error;
	}

	/* Takes the firmware's head over unless the boot says nvrtx.disp=0 or stops before it. */
	wanted = nvrtx_stage_disp_wanted();
	allowed = nvrtx_stage_allowed("takeover");
	if (wanted && !allowed)
		nvrtx_stage_mark("takeover skipped (nvrtx.stop=takeover)");
	if (!wanted)
		nvrtx_stage_mark("takeover off (nvrtx.disp=0); stopping after display-client");
	if (wanted && allowed) {
		error = nvrtx_scanout_start(gpu);
		if (error != 0) {
			gsp_end(gpu);
			return error;
		}
	}

	/* Keeps the GSP up as a GPU device unless the boot says nvrtx.resident=0, once the head was taken over. */
	resident = nvrtx_stage_resident_wanted();
	if (resident && gpu->display.taken != 0) {
		error = nvrtx_node_start(gpu);
		if (error == 0) {
			gpu->resident = 1;
			nvrtx_stage_mark("gpu-device: the GSP stays up until power-off");
			return 0;
		}
	}

	/*
	 * Says where the boot ends; the end keeps a console's GSP up, or runs
	 * display's and channel-stop's stops and unload.
	 */
	nvrtx_stage_mark("display done; this revision ends at gpu-device");
	gsp_end(gpu);

	/* Succeeded: the boot stopped where this revision ends. */
	return 0;
}

/*
 * Stops a GSP kept up until now, for the reboot or the shutdown of the machine:
 * the objects of channel-stop are freed, the firmware is unloaded (unload), the
 * protected region is torn down and the VRAM window is put back in the
 * firmware's mode, so nothing of this boot (the hub's state, the region) is
 * carried into the next.  Called from the PCI shutdown hook once the worker has
 * ended and the GPU device, if any, is stopped.  Returns 0 when the GSP stopped
 * and the region is gone, else the error; the memory is kept either way, since
 * the machine is about to be reset.
 */
int
nvrtx_gsp_shutdown(
	struct nvrtx_gpu *gpu)
{
	int error;

	/* A GSP that does not run has nothing to stop; its worker reported what it left. */
	if (gpu->state != NVRTX_GSP_RUNNING)
		return 0;

	/* Stops the firmware and tears the region down; a failure is reported and the reset follows anyway. */
	nvrtx_stage_mark("unload shutdown begin: stopping the GSP before the reset");
	error = gsp_teardown(gpu);
	if (error != 0) {
		nvrtx_stage_mark("unload shutdown fail (%d); the reset follows anyway", error);
		return error;
	}

	/* Succeeded: the GPU is as the firmware left it, as far as the driver can put it. */
	nvrtx_stage_mark("unload shutdown ok: the GSP is stopped");
	return 0;
}

/*
 * Gives back everything layout prepared.
 *
 * Only for a GPU whose GSP was never started: once the GSP may read this
 * memory it is kept until the GSP is stopped.
 */
void
nvrtx_gsp_release(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_boot_memory *memory;
	size_t chunk;
	unsigned index;

	memory = &gpu->memory;

	/* Frees the image's chunks and their bookkeeping. */
	if (memory->image_chunks != NULL) {
		for (chunk = 0U; chunk < memory->image.chunk_count; chunk++)
			nvrtx_buffer_free(gpu, &memory->image_chunks[chunk]);
		kern_free(memory->image_chunks);
	}

	if (memory->image.chunk_cpu != NULL)
		kern_free(memory->image.chunk_cpu);
	if (memory->image.chunk_bus != NULL)
		kern_free(memory->image.chunk_bus);

	/* Frees the single buffers. */
	nvrtx_buffer_free(gpu, &memory->table_buffer);
	nvrtx_buffer_free(gpu, &memory->signature);
	nvrtx_buffer_free(gpu, &memory->bootloader);
	nvrtx_buffer_free(gpu, &memory->record);
	nvrtx_buffer_free(gpu, &memory->libos);
	for (index = 0U; index < 3U; index++)
		nvrtx_buffer_free(gpu, &memory->logs[index]);
	nvrtx_buffer_free(gpu, &memory->rmargs);
	nvrtx_buffer_free(gpu, &memory->queues);
	nvrtx_buffer_free(gpu, &memory->fwsec_frts);
	nvrtx_buffer_free(gpu, &memory->fwsec_sb);

	/* Frees the buffers of the first channel, of the graphics context, of the compute launch, of the work channel and of the display's channels. */
	nvrtx_channel_release(gpu);
	nvrtx_gr_release(gpu);
	nvrtx_compute_release(gpu);
	nvrtx_nvenc_release(gpu);
	nvrtx_nvdec_release(gpu);
	nvrtx_work_release(gpu);
	nvrtx_scanout_release(gpu);
	nvrtx_memory_release(gpu);

	/* Frees the channel's staging buffer and the reply buffer. */
	if (memory->staging != NULL)
		kern_free(memory->staging);
	if (gpu->reply != NULL)
		kern_free(gpu->reply);
	gpu->reply = NULL;
	if (gpu->call != NULL)
		kern_free(gpu->call);
	gpu->call = NULL;
	if (gpu->static_info != NULL)
		kern_free(gpu->static_info);
	gpu->static_info = NULL;

	/* Frees the files and the VBIOS. */
	nvrtx_file_unload(&gpu->booter_load);
	nvrtx_file_unload(&gpu->booter_unload);
	nvrtx_file_unload(&gpu->bootloader_file);
	nvrtx_file_unload(&gpu->falcon_bootloader);
	if (gpu->vbios_data != NULL)
		kern_free(gpu->vbios_data);
	gpu->vbios_data = NULL;

	/* Forgets everything, so a second release finds nothing. */
	kern_memset(memory, 0, sizeof(*memory));
}

/* Runs the steps of layout in order; the first failure stops it. */
static int
gsp_prepare(
	struct nvrtx_gpu *gpu)
{
	int error;

	/* Reads the small firmware files and checks their containers. */
	error = gsp_load_files(gpu);
	if (error != 0)
		return error;

	/* Reads the GSP image into DMA memory and builds its three-level table. */
	error = gsp_prepare_image(gpu);
	if (error != 0)
		return error;

	/* Places the firmware in the VRAM and writes the placement record. */
	error = gsp_prepare_placement(gpu);
	if (error != 0)
		return error;

	/* Prepares the GSP's start page, logs, arguments and queues. */
	error = gsp_prepare_start(gpu);
	if (error != 0)
		return error;

	/* Reads the VBIOS and makes the two FWSEC programs. */
	error = gsp_prepare_fwsec(gpu);
	if (error != 0)
		return error;

	/* Finds the two falcons. */
	error = gsp_prepare_falcons(gpu);
	if (error != 0)
		return error;

	/* Succeeded: devinit has everything it needs. */
	return 0;
}

/* Reads the small firmware files and parses their containers. */
static int
gsp_load_files(
	struct nvrtx_gpu *gpu)
{
	const struct nvrtx_profile *profile;
	int error;

	profile = gpu->identity.chip.profile;

	/* Reads the GSP's RISC-V bootloader. */
	error = nvrtx_file_load(profile->bootloader_file, GSP_SMALL_FILE_MAX, &gpu->bootloader_file);
	if (error != 0) {
		nvrtx_stage_mark("layout fail: %s not read (%d); no firmware package?", profile->bootloader_file, error);
		return error;
	}

	error = nvrtx_bootloader_parse(gpu->bootloader_file.data, gpu->bootloader_file.size, &gpu->bootloader_desc);
	if (error != 0)
		return error;

	/* Reads the two booters the secure processor runs. */
	error = nvrtx_file_load(profile->booter_load_file, GSP_SMALL_FILE_MAX, &gpu->booter_load);
	if (error != 0)
		return error;

	error = nvrtx_booter_parse(gpu->booter_load.data, gpu->booter_load.size, &gpu->load);
	if (error != 0)
		return error;

	error = nvrtx_file_load(profile->booter_unload_file, GSP_SMALL_FILE_MAX, &gpu->booter_unload);
	if (error != 0)
		return error;

	error = nvrtx_booter_parse(gpu->booter_unload.data, gpu->booter_unload.size, &gpu->unload);
	if (error != 0)
		return error;

	/* Reads the falcon bootloader that loads FWSEC, where the generation has one. */
	if (profile->falcon_bootloader_file != NULL) {
		error = nvrtx_file_load(profile->falcon_bootloader_file, GSP_SMALL_FILE_MAX, &gpu->falcon_bootloader);
		if (error != 0)
			return error;

		error = nvrtx_falcon_bl_parse(gpu->falcon_bootloader.data, gpu->falcon_bootloader.size, &gpu->falcon_bl);
		if (error != 0)
			return error;
	}

	/* Succeeded: the small files are read. */
	nvrtx_stage_mark("layout files: booters %zu/%zu bytes, bootloader %zu bytes",
	    gpu->booter_load.size,
	    gpu->booter_unload.size,
	    gpu->bootloader_file.size);
	return 0;
}

/* Reads the GSP image into its chunks and builds the table that describes it. */
static int
gsp_prepare_image(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_boot_memory *memory;
	size_t table_pages;
	int error;

	memory = &gpu->memory;

	/* Reads the image and its signature into DMA memory. */
	error = nvrtx_file_load_image(gpu, gpu->identity.chip.profile->image_file);
	if (error != 0) {
		nvrtx_stage_mark("layout fail: %s not read (%d)", gpu->identity.chip.profile->image_file, error);
		return error;
	}

	/* Allocates the table's pages in one buffer. */
	table_pages = nvrtx_radix3_pages(memory->image.page_count);
	error = nvrtx_buffer_alloc(gpu, table_pages * NVRTX_GSP_PAGE_SIZE, &memory->table_buffer);
	if (error != 0)
		return error;

	memory->table.chunk_count = 1U;
	memory->table.chunk_bytes = table_pages * NVRTX_GSP_PAGE_SIZE;
	memory->table.page_count = table_pages;
	memory->table_cpu = (uint8_t *)memory->table_buffer.dma.address;
	memory->table_bus = memory->table_buffer.dma.device_address;
	memory->table.chunk_cpu = &memory->table_cpu;
	memory->table.chunk_bus = &memory->table_bus;

	/* Fills the table. */
	error = nvrtx_radix3_fill(&memory->image, &memory->table);
	if (error != 0)
		return error;

	/* Succeeded: the booter can find every page of the image. */
	nvrtx_stage_mark("layout image: %zu bytes in %zu chunks, signature %zu bytes",
	    memory->image_size,
	    memory->image.chunk_count,
	    memory->signature_size);
	return 0;
}

/*
 * Places the firmware in the VRAM and writes the record the booter reads.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/tu102.c (tu102_gsp_oneinit,
 * tu102_gsp_wpr_meta_init).
 */
static int
gsp_prepare_placement(
	struct nvrtx_gpu *gpu)
{
	const struct nvrtx_profile *profile;
	struct nvrtx_boot_memory *memory;
	struct nvrtx_wpr_sources sources;
	uint32_t vram_register;
	uint64_t vram_size;
	uint64_t vga;
	int error;

	memory = &gpu->memory;
	profile = gpu->identity.chip.profile;

	/* Copies the bootloader's payload into DMA memory. */
	error = nvrtx_buffer_alloc(gpu, gpu->bootloader_desc.payload_size, &memory->bootloader);
	if (error != 0)
		return error;

	kern_memcpy(memory->bootloader.dma.address,
	    gpu->bootloader_file.data + gpu->bootloader_desc.payload_at,
	    gpu->bootloader_desc.payload_size);

	/* Reads the VRAM size, in the form the generation reports it, and the VGA workspace. */
	vram_register = nvrtx_rd32(gpu, profile->vram_size_register);
	if (profile->vram_size_in_mib != 0) {
		vram_size = nvrtx_vram_size_mib(vram_register);
	} else {
		vram_size = nvrtx_vram_size(vram_register);
	}

	vga = nvrtx_vga_workspace(vram_size, nvrtx_rd32(gpu, NVRTX_REG_VGA_WORKSPACE));

	/* Computes the placement, with the generation's heap. */
	error = nvrtx_wpr_layout(vram_size,
	    vga,
	    gpu->bootloader_desc.payload_size,
	    memory->image_size,
	    profile->heap_os_bytes,
	    profile->heap_min_bytes,
	    &gpu->layout);
	if (error != 0) {
		nvrtx_stage_mark("layout fail: VRAM of %lluM too small", (unsigned long long)(vram_size >> 20));
		return error;
	}

	/* The reserved memory must lie in the end the firmware scrubbed. */
	error = nvrtx_wpr_scrubbed(&gpu->layout);
	if (error != 0) {
		nvrtx_stage_mark("layout fail: the reserved %lluM reach below the scrubbed end of the VRAM",
		    (unsigned long long)((vram_size - gpu->layout.outer_heap) >> 20));
		return error;
	}

	/* Writes the record. */
	error = nvrtx_buffer_alloc(gpu, NVRTX_WPRM_BYTES, &memory->record);
	if (error != 0)
		return error;

	sources.image_table = memory->table_bus;
	sources.boot_address = memory->bootloader.dma.device_address;
	sources.boot_size = gpu->bootloader_desc.payload_size;
	sources.signature = memory->signature.dma.device_address;
	sources.signature_size = memory->signature_size;
	sources.bootloader = &gpu->bootloader_desc;
	nvrtx_wpr_record((uint8_t *)memory->record.dma.address, &gpu->layout, &sources);

	/* Succeeded: the booter knows where everything goes. */
	return 0;
}

/*
 * Prepares the GSP's start page, its three logs, the start arguments and the
 * queues.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/gsp.c (r535_gsp_libos_init,
 * r535_gsp_shared_init, r535_gsp_rmargs_init).
 */
static int
gsp_prepare_start(
	struct nvrtx_gpu *gpu)
{
	static const char *const log_names[3] = { "LOGINIT", "LOGINTR", "LOGRM" };
	struct nvrtx_boot_memory *memory;
	unsigned index;
	int error;

	memory = &gpu->memory;

	/* Allocates the start page. */
	error = nvrtx_buffer_alloc(gpu, NVRTX_GSP_ARGS_BYTES, &memory->libos);
	if (error != 0)
		return error;

	/* Allocates each log, lists its pages, and names it in the start page. */
	for (index = 0U; index < 3U; index++) {
		error = nvrtx_buffer_alloc(gpu, NVRTX_GSP_LOG_BYTES, &memory->logs[index]);
		if (error != 0)
			return error;

		nvrtx_log_buffer((uint8_t *)memory->logs[index].dma.address,
		    memory->logs[index].dma.device_address,
		    NVRTX_GSP_LOG_BYTES);
		nvrtx_libos_region((uint8_t *)memory->libos.dma.address,
		    index,
		    log_names[index],
		    memory->logs[index].dma.device_address,
		    NVRTX_GSP_LOG_BYTES);
	}

	/* Allocates the queues and writes their page list and the command queue's header. */
	error = nvrtx_buffer_alloc(gpu, NVRTX_QUEUE_SHARED_BYTES, &memory->queues);
	if (error != 0)
		return error;

	memory->queue_cpu = (uint8_t *)memory->queues.dma.address;
	memory->queue_bus = memory->queues.dma.device_address;
	memory->queue_pages.chunk_count = 1U;
	memory->queue_pages.chunk_bytes = NVRTX_QUEUE_SHARED_BYTES;
	memory->queue_pages.page_count = GSP_QUEUE_PTE_COUNT;
	memory->queue_pages.chunk_cpu = &memory->queue_cpu;
	memory->queue_pages.chunk_bus = &memory->queue_bus;
	nvrtx_queue_shared_init(&memory->queue_pages);

	/* Allocates the channel's staging buffer and sets the channel up. */
	memory->staging = kern_malloc(NVRTX_STAGING_BYTES);
	if (memory->staging == NULL)
		return ENOMEM;

	nvrtx_channel_init(&gpu->channel, &memory->queue_pages, memory->staging);

	/* Writes the start arguments and names them in the start page. */
	error = nvrtx_buffer_alloc(gpu, NVRTX_RMARGS_REGION_BYTES, &memory->rmargs);
	if (error != 0)
		return error;

	nvrtx_rmargs((uint8_t *)memory->rmargs.dma.address, memory->queue_bus, GSP_QUEUE_PTE_COUNT);
	nvrtx_libos_region((uint8_t *)memory->libos.dma.address,
	    3U,
	    "RMARGS",
	    memory->rmargs.dma.device_address,
	    NVRTX_RMARGS_REGION_BYTES);

	/* Succeeded: the GSP's operating system has what it starts with. */
	return 0;
}

/* Reads the VBIOS, finds FWSEC and makes the FRTS and SB programs. */
static int
gsp_prepare_fwsec(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_boot_memory *memory;
	const uint8_t *program;
	uint32_t wanted_version;
	uint32_t fuse_register;
	uint32_t fuse;
	size_t buffer_size;
	unsigned index;
	int error;

	memory = &gpu->memory;

	/* Reads the VBIOS through one of its windows. */
	error = nvrtx_rom_read(gpu);
	if (error != 0) {
		nvrtx_stage_mark("layout fail: VBIOS not read (%d)", error);
		return error;
	}

	/* Finds the FWSEC program. */
	error = nvrtx_vbios_fwsec(&gpu->vbios, &gpu->fwsec);
	if (error != 0) {
		nvrtx_stage_mark("layout fail: FWSEC not found in the VBIOS (%d)", error);
		return error;
	}

	/* The descriptor's version must be the one the generation's falcon loads: 2 by the bootloader, 3 by DMA. */
	wanted_version = 2U;
	if (gpu->identity.chip.profile->falcon_style == NVRTX_FALCON_BOOT_ROM)
		wanted_version = 3U;
	if (gpu->fwsec.version != wanted_version) {
		nvrtx_stage_mark("layout fail: FWSEC v%u where v%u is loaded", (unsigned)gpu->fwsec.version, (unsigned)wanted_version);
		return ENOTSUP;
	}

	/* The copies hold the program and, for version 3, the data's last DMA block whole. */
	buffer_size = gpu->fwsec.program_size;
	if ((size_t)gpu->fwsec.dmem_at + gpu->fwsec.dmem_load_size > buffer_size)
		buffer_size = (size_t)gpu->fwsec.dmem_at + gpu->fwsec.dmem_load_size;

	/* Copies the program twice, once for each command; the buffers start zeroed. */
	program = gpu->vbios_data + gpu->fwsec.program_at;
	error = nvrtx_buffer_alloc(gpu, buffer_size, &memory->fwsec_frts);
	if (error != 0)
		return error;

	kern_memcpy(memory->fwsec_frts.dma.address, program, gpu->fwsec.program_size);
	error = nvrtx_buffer_alloc(gpu, buffer_size, &memory->fwsec_sb);
	if (error != 0)
		return error;

	kern_memcpy(memory->fwsec_sb.dma.address, program, gpu->fwsec.program_size);

	/* Version 3: both copies carry the signature the GPU's fuse asks for. */
	if (gpu->fwsec.version == 3U) {
		fuse = 0U;
		fuse_register = nvrtx_fwsec_fuse_register(&gpu->fwsec);
		if (fuse_register != 0U)
			fuse = nvrtx_rd32(gpu, fuse_register);

		error = nvrtx_fwsec_fuse_index(&gpu->fwsec, fuse, &index);
		if (error != 0) {
			nvrtx_stage_mark("layout fail: no FWSEC signature for fuse %08x (versions %04x)",
			    (unsigned)fuse,
			    (unsigned)gpu->fwsec.signature_versions);
			return error;
		}

		error = nvrtx_fwsec_sign((uint8_t *)memory->fwsec_frts.dma.address, &gpu->fwsec, &gpu->vbios, index);
		if (error != 0)
			return error;

		error = nvrtx_fwsec_sign((uint8_t *)memory->fwsec_sb.dma.address, &gpu->fwsec, &gpu->vbios, index);
		if (error != 0)
			return error;
	}

	/* Tells the first copy to set up the region and the second to tear it down. */
	error = nvrtx_fwsec_command((uint8_t *)memory->fwsec_frts.dma.address,
	    &gpu->fwsec,
	    NVRTX_FWSEC_FRTS,
	    gpu->layout.frts,
	    gpu->layout.frts_size);
	if (error != 0)
		return error;

	error = nvrtx_fwsec_command((uint8_t *)memory->fwsec_sb.dma.address, &gpu->fwsec, NVRTX_FWSEC_SB, 0U, 0U);
	if (error != 0)
		return error;

	/* Succeeded: both programs are ready. */
	nvrtx_stage_mark("layout vbios %zu bytes, fwsec v%u %u bytes",
	    gpu->vbios_size,
	    (unsigned)gpu->fwsec.version,
	    (unsigned)gpu->fwsec.program_size);
	return 0;
}

/* Finds the GSP's falcon and the secure processor with its PMC bit. */
static int
gsp_prepare_falcons(
	struct nvrtx_gpu *gpu)
{
	uint32_t words[NVRTX_DEVICE_INFO_WORDS];
	enum nvrtx_falcon_style style;
	uint32_t bit;
	uint32_t index;
	int error;

	/* The GSP's falcon has no PMC bit of its own. */
	style = gpu->identity.chip.profile->falcon_style;
	gpu->gsp_falcon.base = NVRTX_FALCON_GSP_BASE;
	gpu->gsp_falcon.pmc_mask = 0U;
	gpu->gsp_falcon.style = style;

	/*
	 * From Ampere on the secure processor is reset without its PMC bit (the
	 * device table moved and changed its form).
	 *
	 * Procedure referenced: Linux v6.19
	 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/ga102.c (ga102_gsp_reset,
	 * ga102_gsp_flcn).
	 */
	if (style == NVRTX_FALCON_BOOT_ROM) {
		gpu->sec2_falcon.base = NVRTX_FALCON_SEC2_BASE;
		gpu->sec2_falcon.pmc_mask = 0U;
		gpu->sec2_falcon.style = style;
		nvrtx_stage_mark("layout falcons: boot ROM loading (%s)", gpu->identity.chip.profile->name);
		return 0;
	}

	/* Reads the device information table and finds the secure processor's reset bit. */
	for (index = 0U; index < NVRTX_DEVICE_INFO_WORDS; index++)
		words[index] = nvrtx_rd32(gpu, NVRTX_REG_DEVICE_INFO + index * 4U);

	error = nvrtx_device_info_reset(words, NVRTX_ENGINE_SEC2, &bit);
	if (error != 0) {
		nvrtx_stage_mark("layout fail: no SEC2 in the device information (%d)", error);
		return error;
	}

	gpu->sec2_falcon.base = NVRTX_FALCON_SEC2_BASE;
	gpu->sec2_falcon.pmc_mask = 1U << bit;
	gpu->sec2_falcon.style = style;

	/* Succeeded: both falcons are known. */
	nvrtx_stage_mark("layout falcons: SEC2 reset bit %u", (unsigned)bit);
	return 0;
}

/*
 * Runs devinit: waits for the GPU's own firmware to finish devinit.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/devinit/tu102.c (tu102_devinit_wait).
 */
static int
gsp_devinit_wait(
	struct nvrtx_gpu *gpu)
{
	uint64_t start;
	uint64_t waited;
	uint32_t level;
	uint32_t progress;

	/* Polls both registers until devinit is complete or the time is spent. */
	nvrtx_stage_mark("devinit begin: devinit");
	start = nvrtx_now_us();
	for (;;) {
		level = nvrtx_rd32(gpu, NVRTX_REG_DEVINIT_LEVEL);
		progress = nvrtx_rd32(gpu, NVRTX_REG_DEVINIT_PROGRESS);
		waited = nvrtx_now_us() - start;
		if ((level & 0x1U) != 0U && (progress & 0xffU) == NVRTX_DEVINIT_COMPLETED) {
			nvrtx_stage_mark("devinit ok after %u ms", (unsigned)(waited / 1000U));
			return 0;
		}

		if (waited > GSP_DEVINIT_TIMEOUT_US)
			break;

		kern_usleep_range(GSP_DEVINIT_STEP_US, GSP_DEVINIT_STEP_US * 2U);
	}

	/* Devinit never completed: the GPU is not used. */
	nvrtx_stage_mark("devinit fail: devinit level %08x progress %08x", (unsigned)level, (unsigned)progress);
	return ETIMEDOUT;
}

/*
 * Runs frts: checks that no region is there, then sets it up with FWSEC-FRTS.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/fwsec.c (nvkm_gsp_fwsec_frts).
 */
static int
gsp_region_set(
	struct nvrtx_gpu *gpu)
{
	uint32_t low;
	uint32_t high;
	uint32_t result;
	bool fail;
	int error;

	/* A region left by an earlier boot cannot be set up again: stop without writing. */
	nvrtx_region_bounds(gpu, &low, &high);
	if (high != 0U) {
		nvrtx_stage_mark("frts stop: region %08x-%08x already set; power cycle needed", (unsigned)low, (unsigned)high);
		gpu->state = NVRTX_GSP_FAILED;
		return EBUSY;
	}

	/* Lets the falcons reach system memory. */
	error = drv_pci_device_set_bus_master(gpu->identity.pci, true);
	if (error != 0)
		return error;

	/* Runs FWSEC-FRTS; from here on the end of the boot reports or tears down the region. */
	gpu->region_written = 1;
	nvrtx_stage_mark("frts begin: FWSEC-FRTS at %llx", (unsigned long long)gpu->layout.frts);
	error = nvrtx_fwsec_run(gpu, &gpu->memory.fwsec_frts);
	if (error == ETIMEDOUT) {
		gsp_hold(gpu, NVRTX_GSP_HOLD_FWSEC, "frts fail: FWSEC did not halt");
		return error;
	}

	/* FWSEC's own result and the region's bounds decide the outcome. */
	result = nvrtx_frts_result(gpu);
	nvrtx_region_bounds(gpu, &low, &high);
	if (error != 0 || result != 0U || high == 0U) {
		nvrtx_stage_mark("frts fail: error %d result %04x region %08x-%08x", error, (unsigned)result, (unsigned)low, (unsigned)high);
		gpu->state = NVRTX_GSP_FAILED;
		return EIO;
	}

	/* The region is set up. */
	gpu->state = NVRTX_GSP_REGION_SET;
	nvrtx_stage_mark("frts ok: region %08x-%08x", (unsigned)low, (unsigned)high);

	/* A test run gives up here as if FWSEC had not halted. */
	fail = nvrtx_stage_fail_wanted("frts");
	if (fail) {
		gsp_hold(gpu, NVRTX_GSP_HOLD_FWSEC, "frts fail on purpose (nvrtx.fail=frts)");
		return ETIMEDOUT;
	}

	/* Succeeded: FWSEC-FRTS made the region. */
	return 0;
}

/*
 * Ends the boot after vram-window, on its success or a failure: a GSP whose
 * window holds the firmware's console again stays up (unless the boot says
 * nvrtx.resident=0), because stopping the resource manager takes the console's
 * mapping, and maybe the head, with it; the GPU is then kept until power-off as
 * for the GPU device.  Otherwise the finish stops it.
 */
static void
gsp_end(
	struct nvrtx_gpu *gpu)
{
	bool resident;

	/* Keeps a running GSP that holds the console. */
	resident = nvrtx_stage_resident_wanted();
	if (resident &&
	    gpu->console.mapped != 0 &&
	    gpu->state == NVRTX_GSP_RUNNING) {
		gpu->resident = 1;
		nvrtx_stage_mark("display: the GSP stays up to keep the console");
		return;
	}

	/* Stops the GSP and gives everything back. */
	gsp_finish(gpu);
}

/*
 * Ends the boot: stops a running GSP and tears its region down once, then
 * gives the preparation back.
 *
 * Memory a falcon may still read (one that did not halt) is kept.
 */
static void
gsp_finish(
	struct nvrtx_gpu *gpu)
{
	int error;

	/* Stops the GSP and tears the region down; memory a falcon may still read is kept. */
	error = gsp_teardown(gpu);
	if (error != 0)
		return;

	/* Gives the preparation back; a region that stayed is left for a power cycle. */
	nvrtx_gsp_release(gpu);
}

/*
 * Stops a running GSP (channel-stop's frees, then unload) and tears its region
 * down once.
 *
 * Returns 0 when the memory may be given back: the GSP is stopped, or it
 * never ran.  Returns EBUSY when a falcon may still read the memory (one
 * that did not halt, now or earlier), which is then kept until power-off.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/tu102.c (tu102_gsp_fini).
 */
static int
gsp_teardown(
	struct nvrtx_gpu *gpu)
{
	bool stopped;
	uint32_t low;
	uint32_t high;
	int error;

	/* Keeps everything when a falcon may still be reading the memory. */
	if (gpu->state == NVRTX_GSP_FAILED_HOLD)
		return EBUSY;

	/*
	 * Frees the graphics objects, the channel and the address space
	 * (channel-stop) before every unload: the firmware did not unload with
	 * a scheduled channel left.
	 */
	if (gpu->state == NVRTX_GSP_RUNNING) {
		nvrtx_nvenc_stop(gpu);
		nvrtx_nvdec_stop(gpu);
		nvrtx_work_stop(gpu);
		nvrtx_compute_stop(gpu);
		nvrtx_display_stop(gpu);
		nvrtx_gr_stop(gpu);
		nvrtx_channel_stop(gpu);
	}

	/* Stops a GSP that runs; one that does not stop keeps its memory. */
	stopped = false;
	if (gpu->state == NVRTX_GSP_RUNNING) {
		error = gsp_stop(gpu);
		if (error != 0) {
			nvrtx_stage_mark("unload fail: the GSP did not stop (%d)", error);
			gsp_hold(gpu, NVRTX_GSP_HOLD_UNLOAD, "unload fail");
			return EBUSY;
		}

		stopped = true;
	}

	/*
	 * Tears the region down only after a GSP that ran has unloaded:
	 * booter_unload undoes what booter_load and the firmware set up, and
	 * refuses a region that only FWSEC-FRTS made (it answers with mailbox
	 * 0x29).  A region without a started firmware stays until the GPU loses
	 * power or the host reboots, as the vendor's driver also leaves it.
	 */
	error = 0;
	if (stopped && gpu->region_written != 0)
		error = nvrtx_region_tear_down(gpu);
	if (error == ETIMEDOUT) {
		gsp_hold(gpu, NVRTX_GSP_HOLD_TEAR_DOWN, "unload fail: a program did not halt");
		return EBUSY;
	}

	if (!stopped && gpu->region_written != 0) {
		nvrtx_region_bounds(gpu, &low, &high);
		if (high != 0U)
			nvrtx_stage_mark("unload left: region %08x-%08x, no GSP started; reboot the host", (unsigned)low, (unsigned)high);
	}

	/* Puts the window back in the firmware's mode, so a kept console reaches its VRAM without the resource manager. */
	if (stopped)
		nvrtx_console_restore(gpu);

	/* Succeeded: nothing reads the memory any more; a region that stayed is left for a power cycle. */
	return 0;
}

/*
 * Leaves the boot at a point where a program may still run, giving the
 * firmware's picture back: the GPU is cut off the system memory (its bus
 * mastering off, so no falcon reads or writes it any more), the falcons that
 * may run are reset, and the VRAM window is put back in the firmware's mode.
 * The memory stays allocated (for a dump from outside the machine) and the
 * GPU is held until power-off; the protected region may stay, so the next
 * GSP boot needs a power cycle.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/tu102.c (tu102_gsp_fini: the GSP's
 * falcon reset once its firmware is stopped).
 */
static void
gsp_hold(
	struct nvrtx_gpu *gpu,
	enum nvrtx_gsp_hold point,
	const char *what)
{
	uint32_t falcons;
	int master_error;
	int gsp_error;
	int sec2_error;

	/* Held from here on: the memory is never given back. */
	gpu->state = NVRTX_GSP_FAILED_HOLD;

	/* Cuts the GPU off the system memory first, so nothing it still runs reaches the memory. */
	master_error = drv_pci_device_set_bus_master(gpu->identity.pci, false);

	/* Resets the falcons that may still run there. */
	falcons = nvrtx_gsp_hold_falcons(point);
	gsp_error = 0;
	sec2_error = 0;
	if ((falcons & NVRTX_GSP_HOLD_GSP_FALCON) != 0U)
		gsp_error = nvrtx_falcon_reset(gpu, &gpu->gsp_falcon);
	if ((falcons & NVRTX_GSP_HOLD_SEC2_FALCON) != 0U)
		sec2_error = nvrtx_falcon_reset(gpu, &gpu->sec2_falcon);

	/* Gives the firmware's picture its window back. */
	nvrtx_console_restore(gpu);

	/*
	 * Says what is left, one short mark at a time (a mark is cut at the
	 * console's width).
	 */
	nvrtx_stage_mark("%s", what);
	nvrtx_stage_mark("hold: bus master off (%d), falcon reset gsp %d sec2 %d",
	    master_error,
	    gsp_error,
	    sec2_error);
	nvrtx_stage_mark("hold: the firmware's picture is back, memory kept for a dump");
	nvrtx_stage_mark("hold: a region left stops the next GSP boot until power-off");
}

/*
 * Queues the two calls the firmware reads first: the system information and the
 * registry.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r570/gsp.c
 * (r570_gsp_set_system_info) and nvkm/subdev/gsp/rm/r535/gsp.c
 * (r535_gsp_rpc_set_registry).
 */
static int
gsp_queue_setup_calls(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_sysinfo_values values;
	struct nvrtx_device *identity;
	struct drv_pci_device *pci;
	uint8_t *record;
	size_t length;
	uint32_t sequence;
	int error;

	identity = &gpu->identity;
	pci = identity->pci;

	/* Describes the GPU's windows and PCI identity. */
	values.registers = identity->windows[0].bus_address;
	values.vram = identity->windows[1].bus_address;
	values.instance = identity->windows[2].bus_address;
	values.bus_device_function = ((uint64_t)identity->address.bus << 8) |
	    ((uint64_t)identity->address.device << 3) |
	    identity->address.function;
	values.user_address_limit = GSP_USER_ADDRESS_LIMIT;
	values.console_size = gpu->console.size;
	values.vendor = drv_pci_device_vendor(pci);
	values.device = drv_pci_device_product(pci);
	values.subvendor = drv_pci_device_subvendor(pci);
	values.subdevice = drv_pci_device_subproduct(pci);
	values.revision = drv_pci_device_revision(pci);
	values.primary = identity->gop_on_gpu;

	/* Builds both payloads in the reply buffer, one after the other, and queues them unnumbered. */
	record = gpu->reply;
	nvrtx_sysinfo(record, &values);
	error = nvrtx_rpc_send(gpu, NVRTX_CALL_SYSTEM_INFO, record, NVRTX_SYSINFO_BYTES, 0, &sequence);
	if (error != 0)
		return error;

	error = nvrtx_registry(record, NVRTX_STAGING_BYTES, &length);
	if (error != 0)
		return error;

	error = nvrtx_rpc_send(gpu, NVRTX_CALL_REGISTRY, record, length, 0, &sequence);
	if (error != 0)
		return error;

	/* Succeeded: the firmware will read both as soon as it runs. */
	return 0;
}

/*
 * Runs booter: queues the first calls, puts the GSP in RISC-V mode, and starts
 * it through booter_load.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/tu102.c (tu102_gsp_init,
 * tu102_gsp_booter_load) and nvkm/subdev/gsp/rm/r535/gsp.c (r535_gsp_init).
 */
static int
gsp_boot_riscv(
	struct nvrtx_gpu *gpu)
{
	uint64_t libos;
	uint64_t record;
	uint32_t status;
	bool running;
	bool fail;
	int error;

	nvrtx_stage_mark("booter begin: booter_load");

	/* Shows where the memory the firmware writes is, for a dump from outside the machine. */
	nvrtx_stage_mark("booter memory: libos %llx rmargs %llx queues %llx+%x",
	    (unsigned long long)gpu->memory.libos.dma.device_address,
	    (unsigned long long)gpu->memory.rmargs.dma.device_address,
	    (unsigned long long)gpu->memory.queues.dma.device_address,
	    (unsigned)NVRTX_QUEUE_SHARED_BYTES);
	nvrtx_stage_mark("booter memory: logs %llx %llx %llx, %x bytes each",
	    (unsigned long long)gpu->memory.logs[0].dma.device_address,
	    (unsigned long long)gpu->memory.logs[1].dma.device_address,
	    (unsigned long long)gpu->memory.logs[2].dma.device_address,
	    (unsigned)NVRTX_GSP_LOG_BYTES);

	/* Allocates the buffer each received message is copied into, and the one calls are built in. */
	gpu->reply = kern_malloc(NVRTX_STAGING_BYTES);
	if (gpu->reply == NULL)
		return ENOMEM;

	gpu->call = kern_malloc(NVRTX_STAGING_BYTES);
	if (gpu->call == NULL)
		return ENOMEM;

	/* Queues the system information and the registry. */
	error = gsp_queue_setup_calls(gpu);
	if (error != 0)
		return error;

	/* Puts the GSP's falcon into RISC-V mode and gives it the start page. */
	error = nvrtx_falcon_riscv_mode(gpu, &gpu->gsp_falcon);
	if (error != 0)
		return error;

	libos = gpu->memory.libos.dma.device_address;
	nvrtx_wr32(gpu, GSP_REG_MAILBOX0, (uint32_t)(libos & 0xffffffffU));
	nvrtx_wr32(gpu, GSP_REG_MAILBOX1, (uint32_t)(libos >> 32));

	/* Runs booter_load with the placement record's address. */
	record = gpu->memory.record.dma.device_address;
	error = nvrtx_booter_run(gpu, &gpu->load, (uint32_t)(record & 0xffffffffU), (uint32_t)(record >> 32));
	if (error == ETIMEDOUT) {
		gsp_hold(gpu, NVRTX_GSP_HOLD_BOOTER, "booter fail: booter_load did not halt");
		return error;
	}

	if (error != 0) {
		nvrtx_stage_mark("booter fail: booter_load (%d)", error);
		return error;
	}

	/* A test run gives up here as if booter_load had not halted. */
	fail = nvrtx_stage_fail_wanted("booter");
	if (fail) {
		gsp_hold(gpu, NVRTX_GSP_HOLD_BOOTER, "booter fail on purpose (nvrtx.fail=booter)");
		return ETIMEDOUT;
	}

	/* Tells the GSP the bootloader's version and checks that its RISC-V core runs. */
	nvrtx_wr32(gpu, GSP_REG_APP_VERSION, gpu->bootloader_desc.app_version);
	running = nvrtx_falcon_riscv_running(gpu, &gpu->gsp_falcon, &status);
	if (!running) {
		nvrtx_stage_mark("booter fail: the RISC-V core is not running (%08x)", (unsigned)status);
		return EIO;
	}

	/* Succeeded: the GSP firmware runs; from here the end of the boot must stop it. */
	gpu->state = NVRTX_GSP_RUNNING;
	nvrtx_stage_mark("booter ok: RISC-V running");
	return 0;
}

/*
 * Runs gsp-init: waits for the init-done event, performing the sequencer
 * meanwhile.
 *
 * The wait is cut at a few points to show how far the firmware got.  When
 * the event does not come, the GSP is stopped and the GPU cut off the
 * system memory, the firmware's picture gets its window back, and the
 * memory is kept, so the logs and the queues can be dumped from outside the
 * machine (gsp_hold()).
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/gsp.c (r535_gsp_init).
 */
static int
gsp_init_done(
	struct nvrtx_gpu *gpu)
{
	static const uint32_t points_ms[GSP_PROGRESS_POINTS] = { 200U, 1000U, 3000U, GSP_INIT_DONE_TIMEOUT_US / 1000U };
	uint64_t start;
	uint64_t elapsed;
	uint64_t target;
	uint32_t slice;
	size_t length;
	unsigned point;
	bool fail;
	int error;

	/* Waits for the event up to each point in turn, showing the progress at each. */
	nvrtx_stage_mark("gsp-init begin: waiting for init done");
	start = nvrtx_now_us();

	/* A test run gives the init up 200 ms in, as if the event had not come. */
	fail = nvrtx_stage_fail_wanted("gsp-init");
	if (fail) {
		(void)nvrtx_rpc_wait(gpu, NVRTX_EVENT_INIT_DONE, 0U, GSP_FAIL_INIT_US, &length);
		gsp_progress(gpu, GSP_FAIL_INIT_US / 1000U);
		gsp_hold(gpu, NVRTX_GSP_HOLD_INIT, "gsp-init fail on purpose (nvrtx.fail=gsp-init)");
		return ETIMEDOUT;
	}

	error = ETIMEDOUT;
	for (point = 0U; point < GSP_PROGRESS_POINTS; point++) {
		elapsed = nvrtx_now_us() - start;
		target = (uint64_t)points_ms[point] * 1000U;
		slice = 0U;
		if (target > elapsed)
			slice = (uint32_t)(target - elapsed);

		error = nvrtx_rpc_wait(gpu, NVRTX_EVENT_INIT_DONE, 0U, slice, &length);
		if (error != ETIMEDOUT)
			break;

		gsp_progress(gpu, points_ms[point]);
	}

	/* Measures how long the wait lasted. */
	elapsed = nvrtx_now_us() - start;

	/* The event did not come: shows the state once more unless a point just did, and keeps the memory for a dump. */
	if (error != 0) {
		if (error != ETIMEDOUT)
			gsp_progress(gpu, (uint32_t)(elapsed / 1000U));

		nvrtx_stage_mark("gsp-init fail (%d)", error);
		gsp_hold(gpu, NVRTX_GSP_HOLD_INIT, "gsp-init: the GSP did not say it is up");
		return error;
	}

	/* Succeeded: the firmware is up. */
	nvrtx_stage_mark("gsp-init ok: GSP-RM is up after %u ms", (unsigned)(elapsed / 1000U));
	return 0;
}

/*
 * Shows how far the firmware got: both queues' pointers, the firmware's
 * mailboxes and core state, and how much each log holds.
 */
static void
gsp_progress(
	struct nvrtx_gpu *gpu,
	uint32_t milliseconds)
{
	uint32_t status;

	/* Reads the RISC-V core's status register of the generation. */
	(void)nvrtx_falcon_riscv_running(gpu, &gpu->gsp_falcon, &status);

	/*
	 * The command queue's write pointer is the driver's and its read
	 * pointer the firmware's, kept in the status queue's header page; the
	 * status queue's header (its size is zero until the firmware writes it)
	 * and write pointer are the firmware's.
	 */
	nvrtx_stage_mark("gsp-init %u ms: cmd put %u got %u; msg size %x put %u got %u",
	    (unsigned)milliseconds,
	    (unsigned)gsp_queue_word(gpu, NVRTX_QUEUE_COMMAND_AT + NVRTX_QHDR_WRITE_AT),
	    (unsigned)gsp_queue_word(gpu, NVRTX_QUEUE_STATUS_AT + NVRTX_QHDR_READ_AT),
	    (unsigned)gsp_queue_word(gpu, NVRTX_QUEUE_STATUS_AT + NVRTX_QHDR_SIZE_AT),
	    (unsigned)gsp_queue_word(gpu, NVRTX_QUEUE_STATUS_AT + NVRTX_QHDR_WRITE_AT),
	    (unsigned)gsp_queue_word(gpu, NVRTX_QUEUE_COMMAND_AT + NVRTX_QHDR_READ_AT));

	/* The firmware's registers; a mark line holds 79 columns, so each group has its own. */
	nvrtx_stage_mark("gsp-init %u ms: mbox %08x %08x riscv %08x boot %08x",
	    (unsigned)milliseconds,
	    (unsigned)nvrtx_rd32(gpu, GSP_REG_MAILBOX0),
	    (unsigned)nvrtx_rd32(gpu, GSP_REG_MAILBOX1),
	    (unsigned)status,
	    (unsigned)nvrtx_rd32(gpu, GSP_REG_BOOT_SCRATCH));

	/* The three logs' put pointers: how many words the firmware has written in each. */
	nvrtx_stage_mark("gsp-init %u ms: log put %llx %llx %llx",
	    (unsigned)milliseconds,
	    (unsigned long long)gsp_log_put(gpu, 0U),
	    (unsigned long long)gsp_log_put(gpu, 1U),
	    (unsigned long long)gsp_log_put(gpu, 2U));
}

/* Reads one 32-bit word of the queue memory as the other side last wrote it. */
static uint32_t
gsp_queue_word(
	const struct nvrtx_gpu *gpu,
	size_t at)
{
	const volatile uint32_t *word;

	/* The queue memory is one contiguous buffer. */
	word = (const volatile uint32_t *)(const void *)(gpu->memory.queue_cpu + at);

	/* Succeeded: the word's current value. */
	return *word;
}

/* Reads the put pointer at the start of one log, as the firmware last wrote it. */
static uint64_t
gsp_log_put(
	const struct nvrtx_gpu *gpu,
	unsigned index)
{
	const volatile uint64_t *word;

	/* The pointer is the log's first word. */
	word = (const volatile uint64_t *)gpu->memory.logs[index].dma.address;

	/* Succeeded: the pointer's current value. */
	return *word;
}

/*
 * Stops a running GSP (unload): the unload call, the halt, and the falcon
 * reset.
 *
 * The region is torn down afterwards by the finish.  Returns ETIMEDOUT when
 * the firmware did not unload in time.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/gsp.c (r535_gsp_fini) and
 * nvkm/subdev/gsp/tu102.c (tu102_gsp_fini).
 */
static int
gsp_stop(
	struct nvrtx_gpu *gpu)
{
	uint8_t args[NVRTX_UNLOAD_ARGS_BYTES];
	uint32_t sequence;
	size_t length;
	int error;

	/* Asks the firmware to unload and waits for its reply; a lost reply still goes on to the halt check. */
	nvrtx_stage_mark("unload begin: unload");
	nvrtx_unload_args(args);
	error = nvrtx_rpc_send(gpu, NVRTX_CALL_UNLOAD, args, sizeof(args), 1, &sequence);
	if (error == 0)
		error = nvrtx_rpc_wait(gpu, NVRTX_CALL_UNLOAD, sequence, GSP_UNLOAD_TIMEOUT_US, &length);
	if (error != 0)
		nvrtx_stage_mark("unload unload call: %d", error);

	/* Waits for the firmware to say it has unloaded. */
	error = nvrtx_poll(gpu, GSP_REG_MAILBOX0, 0xffffffffU, GSP_UNLOADED, GSP_UNLOADED_TIMEOUT_US);
	if (error != 0)
		return error;

	/* Resets the GSP's falcon; the firmware no longer reads the memory. */
	error = nvrtx_falcon_reset(gpu, &gpu->gsp_falcon);
	if (error != 0)
		return error;

	/* Succeeded: the GSP is stopped. */
	gpu->state = NVRTX_GSP_REGION_SET;
	return 0;
}

/*
 * Asks the firmware for its static information and marks what it says about the
 * GPU.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r570/gsp.c
 * (r570_gsp_get_static_info).
 */
static int
gsp_static_info(
	struct nvrtx_gpu *gpu)
{
	char name[NVRTX_STATIC_NAME_BYTES + 1U];
	uint32_t sequence;
	size_t length;
	size_t index;
	int error;

	/* Sends a zeroed record of the information's size and waits for it to come back filled. */
	nvrtx_stage_mark("static-info begin: static info");
	kern_memset(gpu->reply, 0, NVRTX_STATIC_INFO_BYTES);
	error = nvrtx_rpc_send(gpu, NVRTX_CALL_STATIC_INFO, gpu->reply, NVRTX_STATIC_INFO_BYTES, 1, &sequence);
	if (error != 0)
		return error;

	error = nvrtx_rpc_wait(gpu, NVRTX_CALL_STATIC_INFO, sequence, NVRTX_CALL_TIMEOUT_US, &length);
	if (error != 0) {
		nvrtx_stage_mark("static-info fail: static info (%d)", error);
		return error;
	}

	/* A short reply is not the 570.144 layout. */
	if (length < NVRTX_STATIC_INFO_BYTES) {
		nvrtx_stage_mark("static-info fail: static info of %zu bytes", length);
		return EIO;
	}

	/*
	 * Keeps the information: address-space reads the FB regions and the
	 * internal handles from it.
	 */
	gpu->static_info = kern_malloc(NVRTX_STATIC_INFO_BYTES);
	if (gpu->static_info == NULL)
		return ENOMEM;

	kern_memcpy(gpu->static_info, gpu->reply, NVRTX_STATIC_INFO_BYTES);
	gpu->internal_client = nvrtx_get32(gpu->static_info, NVRTX_STATIC_INTERNAL_CLIENT_AT);
	gpu->internal_subdevice = nvrtx_get32(gpu->static_info, NVRTX_STATIC_INTERNAL_CLIENT_AT + 8U);

	/* Copies the GPU's short name, keeping printable characters only. */
	for (index = 0U; index < NVRTX_STATIC_NAME_BYTES; index++) {
		name[index] = (char)gpu->reply[NVRTX_STATIC_SHORT_NAME_AT + index];
		if (name[index] == '\0')
			break;
		if (name[index] < ' ' || name[index] > '~')
			name[index] = '?';
	}
	name[index] = '\0';

	/* Succeeded: the firmware knows the GPU. */
	nvrtx_stage_mark("static-info static: %s vram %lluM uefi %u", name,
	    (unsigned long long)(nvrtx_get64(gpu->reply, NVRTX_STATIC_FB_LENGTH_AT) >> 20),
	    (unsigned)gpu->reply[NVRTX_STATIC_UEFI_AT]);
	nvrtx_stage_mark("static-info static: bar1 root %llx bar2 root %llx",
	    (unsigned long long)nvrtx_get64(gpu->reply, NVRTX_STATIC_BAR1_ROOT_AT),
	    (unsigned long long)nvrtx_get64(gpu->reply, NVRTX_STATIC_BAR2_ROOT_AT));
	return 0;
}

/* Allocates the client, the device and the subdevice. */
static int
gsp_objects(
	struct nvrtx_gpu *gpu)
{
	uint8_t parameters[NVRTX_ROOT_PARAMS_BYTES];
	int error;

	/* The client is its own parent; it names itself and no process. */
	kern_memset(parameters, 0, sizeof(parameters));
	nvrtx_put32(parameters, 0U, NVRTX_HANDLE_CLIENT);
	nvrtx_put32(parameters, NVRTX_ROOT_PROCESS_AT, 0xffffffffU);
	error = nvrtx_rm_allocate(gpu, NVRTX_HANDLE_CLIENT, NVRTX_HANDLE_CLIENT, NVRTX_CLASS_ROOT, parameters, NVRTX_ROOT_PARAMS_BYTES);
	if (error != 0)
		return error;

	/* The device shares the client. */
	kern_memset(parameters, 0, sizeof(parameters));
	nvrtx_put32(parameters, NVRTX_DEVICE_SHARE_AT, NVRTX_HANDLE_CLIENT);
	error = nvrtx_rm_allocate(gpu, NVRTX_HANDLE_CLIENT, NVRTX_HANDLE_DEVICE, NVRTX_CLASS_DEVICE, parameters, NVRTX_DEVICE_PARAMS_BYTES);
	if (error != 0)
		return error;

	/* The subdevice is the first (and only) one of the device. */
	kern_memset(parameters, 0, sizeof(parameters));
	error = nvrtx_rm_allocate(gpu, NVRTX_HANDLE_DEVICE, NVRTX_HANDLE_SUBDEVICE, NVRTX_CLASS_SUBDEVICE, parameters, NVRTX_SUBDEVICE_PARAMS_BYTES);
	if (error != 0)
		return error;

	/* Succeeded: the driver has its client, device and subdevice. */
	nvrtx_stage_mark("static-info ok: client, device and subdevice");
	return 0;
}
