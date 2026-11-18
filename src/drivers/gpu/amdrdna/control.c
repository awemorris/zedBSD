/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amdrdna driver's services on its GPU device (/dev/gpuN): the reset
 * root asks for (GPU_DEVICE_RESET) and the driver's reports
 * (GPU_DIAGNOSE, include/uapi/amdrdna.h).
 *
 * The reset is the one the bring-up thread runs for the GPU core's
 * recovery: a mode-1 reset, the video BIOS's post, the stages that ran set
 * up again and the display lit again.  The core asks for it only when no
 * session of the device is left.  The reports are read under the reset's
 * lock, so that none reads the hardware while a reset has it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/gpu/gpu.h>
#include <kern/kcrt.h>
#include <kern/lock.h>
#include <uapi/amdrdna.h>
#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-node.h"
#include "drivers/gpu/amdrdna/amdrdna-private.h"
#include "drivers/gpu/amdrdna/amdrdna-regs.h"
#include "drivers/gpu/amdrdna/amdrdna-reset.h"

static int control_reset(void *private_data);
static int control_diagnose(void *private_data, struct drv_gpu_report *report);
static int control_status(struct amdrdna_device *device, struct drv_gpu_report *report);
static int control_vbios(struct amdrdna_device *device, struct drv_gpu_report *report);
static int control_display(struct amdrdna_device *device, struct drv_gpu_report *report);
static int control_screen(struct amdrdna_device *device, struct drv_gpu_report *report);
static int control_hotplug(struct amdrdna_device *device, struct drv_gpu_report *report);
static int control_copy(struct drv_gpu_report *report, const void *source, uint64_t total);
static void control_fill(struct amdrdna_device *device, struct amdrdna_status *status);
static void control_fill_memory(struct amdrdna_device *device, struct amdrdna_status *status);
static uint32_t control_fill_power(struct amdrdna_device *device, struct amdrdna_status *status);
static void control_fill_engines(struct amdrdna_device *device, struct amdrdna_status *status);
static void control_fill_spaces(struct amdrdna_device *device, struct amdrdna_status *status);
static uint32_t ip_version(const struct amdrdna_ip *ip);

/* The services of every node; immutable. */
static const struct drv_gpu_service_ops control_service = {
	control_reset,
	control_diagnose
};

/* Gives the GPU device's operations the services, before the node is registered. */
void
amdrdna_control_prepare(
	struct amdrdna_node *node)
{
	/* The reset and the reports. */
	node->ops.service = &control_service;
}

/*
 * Resets the GPU at root's request, once the GPU core left no session:
 * the bring-up thread runs it; this waits.  Returns 0 when the GPU runs
 * again, or the reset's error.
 */
static int
control_reset(
	void *private_data)
{
	struct amdrdna_node *node;
	int error;

	/* The node the core passed in. */
	node = private_data;

	/* The reset, asked as root's request. */
	error = amdrdna_reset_request(node->device, AMDRDNA_RESET_REQUEST, NULL);
	if (error != 0)
		return error;

	/* Succeeded: the GPU is checked and runs again. */
	return 0;
}

/*
 * Fills one piece of a report, under the reset's lock (the video BIOS
 * image, kept in memory, needs none).  Returns 0, ENOTSUP for a kind the
 * driver does not have, or the report's error.
 */
static int
control_diagnose(
	void *private_data,
	struct drv_gpu_report *report)
{
	struct amdrdna_device *device;
	struct amdrdna_node *node;
	int error;

	/* The node the core passed in, and its GPU. */
	node = private_data;
	device = node->device;

	/* The image, read without the hardware. */
	if (report->kind == AMDRDNA_DIAGNOSE_VBIOS) {
		error = control_vbios(device, report);
		if (error != 0)
			return error;

		/* Succeeded: the piece of the image is filled. */
		return 0;
	}

	/* The others read the hardware: never while a reset runs. */
	mutex_lock(&device->reset.lock);

	switch (report->kind) {
	case AMDRDNA_DIAGNOSE_STATUS:
		error = control_status(device, report);
		break;
	case AMDRDNA_DIAGNOSE_DISPLAY:
		error = control_display(device, report);
		break;
	case AMDRDNA_DIAGNOSE_SCREEN:
		error = control_screen(device, report);
		break;
	case AMDRDNA_DIAGNOSE_HOTPLUG:
		error = control_hotplug(device, report);
		break;
	default:
		error = ENOTSUP;
		break;
	}

	mutex_unlock(&device->reset.lock);

	/* Reports a kind the driver does not have or a report that failed. */
	if (error != 0)
		return error;

	/* Succeeded: the piece is filled. */
	return 0;
}

/* Fills a piece of the driver's state and what its stages read: AMDRDNA_DIAGNOSE_STATUS. */
static int
control_status(
	struct amdrdna_device *device,
	struct drv_gpu_report *report)
{
	struct amdrdna_status status;
	int error;

	/* The state now. */
	control_fill(device, &status);

	/* The piece asked for. */
	error = control_copy(report, &status, sizeof(status));
	if (error != 0)
		return error;

	/* Succeeded: the piece of the state is filled. */
	return 0;
}

/*
 * Fills a piece of the video BIOS image: AMDRDNA_DIAGNOSE_VBIOS.  The image
 * exists once stage A1v has run and is never freed while the device
 * exists.  Returns 0, ENOENT without an image, or EINVAL for an offset
 * past its end.
 */
static int
control_vbios(
	struct amdrdna_device *device,
	struct drv_gpu_report *report)
{
	const uint8_t *image;
	uint32_t total;
	unsigned long irq;
	int error;

	/* The image and its length, as stage A1v left them. */
	irq = spin_lock_irqsave(&device->state_lock);

	image = NULL;
	total = 0U;
	if (device->stage >= AMDRDNA_STAGE_A1V) {
		image = device->vbios;
		total = device->vbios_bytes;
	}

	spin_unlock_irqrestore(&device->state_lock, irq);

	/* Without an image there is nothing to copy. */
	if (image == NULL)
		return ENOENT;

	/* The piece asked for. */
	error = control_copy(report, image, total);
	if (error != 0)
		return error;

	/* Succeeded: the piece of the image is filled. */
	return 0;
}

/*
 * Fills a piece of the display the driver took over, its registers and
 * its counts: AMDRDNA_DIAGNOSE_DISPLAY.  The registers are read once stage
 * A1 has read the firmware's chain; before, the answer is ENOENT.
 */
static int
control_display(
	struct amdrdna_device *device,
	struct drv_gpu_report *report)
{
	struct amdrdna_display_state state;
	struct amdrdna_node *node;
	unsigned long irq;
	unsigned stage;
	int error;

	/* The firmware's chain is known once stage A1 reported ok. */
	irq = spin_lock_irqsave(&device->state_lock);

	stage = device->stage;

	spin_unlock_irqrestore(&device->state_lock, irq);

	/* Without the firmware's chain there is no display to report. */
	if (stage < AMDRDNA_STAGE_A1 || !device->gop.lit)
		return ENOENT;

	/* The display's part, then the GPU device's. */
	kern_memset(&state, 0, sizeof(state));
	state.version = AMDRDNA_INTERFACE_VERSION;
	state.size = sizeof(state);
	amdrdna_display_report(device, &state);

	/* Registered with the display; leased while an open holds it. */
	node = device->node;
	if (node != NULL && node->display != 0) {
		mutex_lock(&node->display_lock);

		state.flags |= AMDRDNA_DISPLAY_REGISTERED;
		if (node->owner != NULL)
			state.flags |= AMDRDNA_DISPLAY_LEASED;
		if (node->broken != 0)
			state.flags |= AMDRDNA_DISPLAY_BROKEN;
		state.presents = node->presents;
		state.leases = node->leases;

		mutex_unlock(&node->display_lock);
	}

	/* The piece asked for. */
	error = control_copy(report, &state, sizeof(state));
	if (error != 0)
		return error;

	/* Succeeded: the piece of the display's state is filled. */
	return 0;
}

/*
 * Fills a piece of the picture the display shows: AMDRDNA_DIAGNOSE_SCREEN,
 * its head (struct amdrdna_screen) and then its rows.  Returns 0, ENOENT
 * before the takeover, ERANGE for a surface the driver cannot read,
 * ENOMEM, or EINVAL for an offset past the picture's end.
 */
static int
control_screen(
	struct amdrdna_device *device,
	struct drv_gpu_report *report)
{
	struct amdrdna_screen head;
	uint64_t total;
	uint32_t from;
	uint32_t part;
	uint32_t copied;
	uint8_t *buffer;
	int error;

	/* The head: the picture as the plane reads it now. */
	kern_memset(&head, 0, sizeof(head));
	head.version = AMDRDNA_INTERFACE_VERSION;
	head.size = sizeof(head);
	error = amdrdna_display_screen(device, &head, 0U, NULL, 0U, &copied);
	if (error != 0)
		return error;
	total = sizeof(head) + (uint64_t)head.bytes;
	report->total = total;
	if (report->offset > total)
		return EINVAL;

	/* The head's part of the piece. */
	report->bytes = 0U;
	buffer = report->buffer;
	if (report->offset < sizeof(head)) {
		from = (uint32_t)report->offset;
		part = (uint32_t)sizeof(head) - from;
		if (part > report->capacity)
			part = report->capacity;
		kern_memcpy(buffer, (const uint8_t *)&head + from, part);
		report->bytes = part;
	}

	/* The rows' part. */
	if (report->bytes < report->capacity) {
		error = amdrdna_display_screen(device, &head, report->offset + report->bytes - sizeof(head),
		    buffer + report->bytes, report->capacity - report->bytes, &copied);
		if (error != 0)
			return error;
		report->bytes += copied;
	}

	/* Succeeded: the piece of the picture is filled. */
	return 0;
}

/*
 * Acts as if the monitor went away or came (AMDRDNA_DIAGNOSE_HOTPLUG, the
 * level the argument) and fills the answer: the action's result, whether
 * the output is dark and the mode.  It acts for the piece at offset 0
 * only (EINVAL for another); the request fails when the action is refused.
 */
static int
control_hotplug(
	struct amdrdna_device *device,
	struct drv_gpu_report *report)
{
	struct amdrdna_hotplug answer;
	int result;
	int error;

	/* The action runs for the first piece alone. */
	if (report->offset != 0U)
		return EINVAL;

	/* The action, run by the bring-up thread; a refusal is the request's error. */
	result = amdrdna_hotplug_ask(device, report->argument);
	if (result == EINVAL ||
	    result == ENOENT ||
	    result == ENODEV)
		return result;

	/* The answer. */
	kern_memset(&answer, 0, sizeof(answer));
	answer.version = AMDRDNA_INTERFACE_VERSION;
	answer.size = sizeof(answer);
	answer.level = report->argument;
	answer.result = result;
	answer.dark = 0U;
	if (device->display.output_dark)
		answer.dark = 1U;
	answer.width = device->display.width;
	answer.height = device->display.height;
	answer.refresh_millihz = device->display.refresh_millihz;
	error = control_copy(report, &answer, sizeof(answer));
	if (error != 0)
		return error;

	/* Succeeded: the action ran; its result is in the answer. */
	return 0;
}

/*
 * Fills a piece of a report held whole in memory: the bytes from the
 * piece's offset, as many as fit.  Returns 0, or EINVAL for an offset past
 * the report's end.
 */
static int
control_copy(
	struct drv_gpu_report *report,
	const void *source,
	uint64_t total)
{
	uint64_t length;

	/* The report's length, and nothing past its end. */
	report->total = total;
	if (report->offset > total)
		return EINVAL;

	/* The bytes from the offset that fit. */
	length = total - report->offset;
	if (length > report->capacity)
		length = report->capacity;
	if (length != 0U)
		kern_memcpy(report->buffer, (const uint8_t *)source + report->offset, (size_t)length);
	report->bytes = (uint32_t)length;

	/* Succeeded: the piece is filled. */
	return 0;
}

/* Fills the status answer from the driver's record. */
static void
control_fill(
	struct amdrdna_device *device,
	struct amdrdna_status *status)
{
	const struct amdrdna_gop *gop;
	unsigned long irq;
	uint32_t flags;
	bool training;

	/* Starts from zero, keeping the caller's version and size. */
	kern_memset(status, 0, sizeof(*status));
	status->version = AMDRDNA_INTERFACE_VERSION;
	status->size = sizeof(*status);

	/* The stage machine, as one consistent view. */
	irq = spin_lock_irqsave(&device->state_lock);

	status->stage = device->stage;
	status->target = device->target;
	status->running = device->running;
	status->failed_stage = device->failed_stage;
	status->failed_error = device->failed_error;
	flags = 0U;
	if (device->thread_running)
		flags |= AMDRDNA_STATUS_THREAD_RUNNING;

	spin_unlock_irqrestore(&device->state_lock, irq);

	/* The identity and the windows, read by stage A0. */
	status->pci_id = ((uint32_t)AMDRDNA_PCI_VENDOR << 16) | device->product;
	status->pci_revision = device->revision;
	status->pci_subsystem = ((uint32_t)device->subvendor << 16) | device->subproduct;
	status->graphics_version = ip_version(&device->blocks.blocks[AMDRDNA_BLOCK_GC]);
	status->display_version = ip_version(&device->blocks.blocks[AMDRDNA_BLOCK_DCN]);
	status->security_version = ip_version(&device->blocks.blocks[AMDRDNA_BLOCK_PSP]);
	status->power_version = ip_version(&device->blocks.blocks[AMDRDNA_BLOCK_SMU]);
	status->vram_mib = device->vram_mib;
	status->bar0_mib = (uint32_t)(device->vram_window_bytes >> 20);
	status->rebar_capability = device->resize_sizes;
	status->rebar_control = device->resize_control;
	status->power_firmware_state = device->smu_state;
	status->secure_os_word = device->secure_os_word;
	status->boot_mailbox = device->boot_word;
	status->engine_status = device->engine_status;
	status->bios_scratch_7 = device->scratch_post;
	status->frame_buffer_base = device->frame_buffer_base;
	status->cpu_window_mib = (uint32_t)(device->layout.cpu_pool_end >> 20);

	/* The state flags of stage A0. */
	if (device->posted)
		flags |= AMDRDNA_STATUS_POSTED;
	if (device->secure_os_word != 0U)
		flags |= AMDRDNA_STATUS_SECURE_OS_ALIVE;
	if ((device->platform_ready_word & AMDRDNA_PSP_DONE) != 0U)
		flags |= AMDRDNA_STATUS_PLATFORM_READY;
	if (device->resize_capability != 0U)
		flags |= AMDRDNA_STATUS_RESIZABLE_BAR;
	if (device->boot_database.present)
		flags |= AMDRDNA_STATUS_BOOT_DATABASE;

	/* The firmware's picture, read by stage A1. */
	gop = &device->gop;
	if (gop->lit) {
		status->gop_vram_offset = gop->surface_offset;
		status->gop_timing = gop->timing;
		status->gop_plane = gop->plane;
		status->gop_output = gop->output;
		status->gop_output_kind = gop->output_kind;
		status->gop_width = gop->width;
		status->gop_height = gop->height;
		status->gop_pitch_word = gop->pitch_word;
		status->gop_format = gop->format;
		status->gop_source_width = gop->viewport_width;
		status->gop_source_height = gop->viewport_height;
		status->gop_shape = (uint32_t)gop->picture.shape;
	}

	/* Whether the loader's picture is this GPU's. */
	if (gop->loader_on_gpu)
		flags |= AMDRDNA_STATUS_GOP_ON_GPU;

	/* The video BIOS, read by stage A1v. */
	if (status->stage >= AMDRDNA_STAGE_A1V && device->vbios != NULL) {
		flags |= AMDRDNA_STATUS_VBIOS;
		status->vbios_source = device->vbios_source;
		status->vbios_length = device->vbios_bytes;
		status->firmware_capability_bits = device->vbios_info.capability_bits;
		status->firmware_table_version = ((uint32_t)device->vbios_info.firmware_format << 8) | device->vbios_info.firmware_content;
		status->power_table_id = device->vbios_info.power_table_id;
		status->shutdown_temperature = device->vbios_info.shutdown_temperature;

		/* Whether the board asks for the long memory training. */
		training = amdrdna_training_allowed(device);
		if (training)
			flags |= AMDRDNA_STATUS_TRAINING_ALLOWED;
	}

	/* The shared memory and the interrupts, set up by stage A2. */
	if (status->stage >= AMDRDNA_STAGE_A2)
		control_fill_memory(device, status);

	/* The training, the security processor and the power side, from stage A3t on. */
	if (status->stage >= AMDRDNA_STAGE_A2)
		flags |= control_fill_power(device, status);

	/* The engines, from stage A5 on (or as far as a failed A5 got). */
	if (status->stage >= AMDRDNA_STAGE_A4)
		control_fill_engines(device, status);

	/* The address spaces and the GPU device, from stage A5s on (or as far as a failed A5s got). */
	if (status->stage >= AMDRDNA_STAGE_A5)
		control_fill_spaces(device, status);

	/* The display hub's memory state and the underflow as they stand, once the driver took the display over. */
	if (status->stage >= AMDRDNA_STAGE_A6) {
		status->memory_state = amdrdna_display_memory_state(device);
		status->memory_handshake = amdrdna_display_memory_handshake(device);
		status->underflow_now = amdrdna_display_underflow(device, &status->timing_underflow_now);
		amdrdna_display_memory_marks(device, &status->switch_mark, &status->handshake_index);
	}

	/* Publishes the flags. */
	status->flags = flags;
}

/* Fills the status answer's part that stage A2 set up. */
static void
control_fill_memory(
	struct amdrdna_device *device,
	struct amdrdna_status *status)
{
	/* The system-memory table's reach. */
	status->gart_base = device->gart_start;
	status->gart_mib = (uint32_t)(AMDRDNA_GART_BYTES >> 20);

	/* The free VRAM of both pools, as one view. */
	mutex_lock(&device->memory_lock);

	status->cpu_pool_free_mib = (uint32_t)(device->cpu_pool.free_bytes >> 20);
	status->gpu_pool_free_mib = (uint32_t)(device->gpu_pool.free_bytes >> 20);

	mutex_unlock(&device->memory_lock);

	/* The interrupt's kind. */
	status->interrupt_kind = AMDRDNA_INTERRUPT_MSI;
	if (device->interrupts.irq.type == DRV_PCI_IRQ_MSIX)
		status->interrupt_kind = AMDRDNA_INTERRUPT_MSIX;

	/* The counts so far and the display's underflow at the end of the stage. */
	status->interrupt_signals = (uint32_t)device->interrupts.signals;
	status->vm_faults = (uint32_t)device->interrupts.faults;
	status->display_underflow = device->underflow_after;
}

/*
 * Fills the status answer's part that stages A3t, A3 and A4 set up, and
 * reports their state flags.
 */
static uint32_t
control_fill_power(
	struct amdrdna_device *device,
	struct amdrdna_status *status)
{
	struct amdrdna_thermal *thermal;
	unsigned long irq;
	uint32_t flags;

	/* The training's result. */
	status->training_result = device->training.result;
	status->training_reason = device->training.reason;
	status->training_ms = device->training.milliseconds;

	/* The security processor's side. */
	status->region_kib = device->psp.region_bytes >> 10;
	status->programs_loaded = device->psp.loaded;
	status->secure_os_version = device->psp.secure_os_version;
	status->security_warnings = device->psp.warnings;

	/* The interrupt entries read and those no block took. */
	status->interrupt_entries = (uint32_t)device->interrupts.entries;
	status->interrupt_unknown = (uint32_t)device->interrupts.unknown;

	/* The state flags of the three stages. */
	flags = 0U;
	if (device->interrupts.enabled)
		flags |= AMDRDNA_STATUS_INTERRUPTS_ON;
	if (device->psp.booted)
		flags |= AMDRDNA_STATUS_SECURE_OS_OURS;

	/* The power side, as one consistent view of what the watch publishes. */
	thermal = &device->thermal;
	irq = spin_lock_irqsave(&thermal->lock);

	status->smu_version = thermal->smu_version;
	status->smu_interface = thermal->interface_version;
	status->fan_mode = thermal->mode;
	status->fan_duty = thermal->duty_percent;
	status->fan_rpm = thermal->fan_speed;
	status->fan_driven = thermal->fan_driven;
	status->temperature = thermal->temperature;
	status->temperature_highest = thermal->highest;
	status->temperature_limit = thermal->limit;
	status->thermal_alerts = (uint32_t)(thermal->high_alerts + thermal->low_alerts + thermal->critical_alerts);
	status->memory_clock = thermal->memory_clock;
	status->return_clock = thermal->return_clock;
	if (thermal->faulted)
		flags |= AMDRDNA_STATUS_THERMAL_FAULT;
	if (thermal->hung)
		flags |= AMDRDNA_STATUS_SMU_HUNG;

	spin_unlock_irqrestore(&thermal->lock, irq);

	/* Succeeded: the flags of the three stages. */
	return flags;
}

/* Fills the status answer's part that stage A5 set up. */
static void
control_fill_engines(
	struct amdrdna_device *device,
	struct amdrdna_status *status)
{
	struct amdrdna_engines *engines;
	unsigned long irq;
	uint64_t interrupts;
	unsigned index;

	/* What runs. */
	engines = &device->engines;
	status->engine_flags = 0U;
	if (engines->scheduler_up)
		status->engine_flags |= AMDRDNA_RUNS_SCHEDULER;
	if (engines->graphics.up)
		status->engine_flags |= AMDRDNA_RUNS_GRAPHICS;
	if (engines->copy.up)
		status->engine_flags |= AMDRDNA_RUNS_COPY;
	if (engines->direct)
		status->engine_flags |= AMDRDNA_RUNS_DIRECT;
	status->compute_queues = engines->compute_count;

	/* The shape the stage read. */
	status->address_config = engines->address_config;
	status->backends = engines->backends;
	status->compute_units = engines->units;
	status->channels_off = engines->channels_off;

	/* The end-of-pipe interrupts of the graphics and compute rings, each read under its ring's lock. */
	irq = spin_lock_irqsave(&engines->graphics.lock);

	interrupts = engines->graphics.interrupts;

	spin_unlock_irqrestore(&engines->graphics.lock, irq);
	for (index = 0U; index < engines->compute_count; index++) {
		/* One compute ring's count. */
		irq = spin_lock_irqsave(&engines->compute[index].lock);

		interrupts += engines->compute[index].interrupts;

		spin_unlock_irqrestore(&engines->compute[index].lock, irq);
	}

	/* Publishes their sum. */
	status->end_of_pipe_interrupts = (uint32_t)interrupts;

	/* The copy engine's traps, under its ring's lock. */
	irq = spin_lock_irqsave(&engines->copy.lock);

	status->copy_traps = (uint32_t)engines->copy_traps;

	spin_unlock_irqrestore(&engines->copy.lock, irq);

	/* What the checks saw. */
	status->fences = engines->fences;
	status->copied_bytes = engines->copied_bytes;
	status->dispatch_words = engines->dispatch_words;
}

/* Fills the status answer's part that stage A5s set up. */
static void
control_fill_spaces(
	struct amdrdna_device *device,
	struct amdrdna_status *status)
{
	struct amdrdna_spaces *spaces;
	struct amdrdna_node *node;
	unsigned long irq;

	/* The spaces open and kept, and the drops, under the spaces' lock. */
	spaces = &device->spaces;
	irq = spin_lock_irqsave(&spaces->lock);

	status->spaces = (spaces->count & 0xffffU) | ((spaces->held & 0xffffU) << 16);
	status->context_flushes = (uint32_t)spaces->pool.flushes;

	spin_unlock_irqrestore(&spaces->lock, irq);

	/* The GPU device's state. */
	status->device_flags = 0U;
	node = device->node;
	if (node == NULL)
		return;
	if (node->gpu != NULL)
		status->device_flags |= AMDRDNA_DEVICE_REGISTERED;
	if (node->renderer != 0)
		status->device_flags |= AMDRDNA_DEVICE_RENDERER;
	if (node->faulted != 0 || spaces->faulted)
		status->device_flags |= AMDRDNA_DEVICE_FAULTED;
}

/* Packs an IP block's version as major << 16 | minor << 8 | step; 0 for an unlisted block. */
static uint32_t
ip_version(
	const struct amdrdna_ip *ip)
{
	uint32_t version;

	/* An unlisted block has no version. */
	if (ip->found == 0U)
		return 0U;

	/* Packs the three parts. */
	version = ((uint32_t)ip->major << 16) | ((uint32_t)ip->minor << 8) | ip->step;

	/* Succeeded: the packed version. */
	return version;
}
