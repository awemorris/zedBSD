/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display under the GSP (the display-client stage: bring-up).
 *
 * The resource manager owns the display hardware's state machine.  The driver
 * brings the display up in this order: the display's instance memory (64 KiB of
 * VRAM) is given to the resource manager; the display gets a client, device and
 * subdevice of its own with the common object the display controls go to; the
 * windows, heads and outputs are read, each output's resource and connector;
 * the hotplug and DisplayPort interrupt notifications are asked for; the hash
 * table at the start of the instance memory is cleared; and the Turing display
 * root is made.  Nothing here changes what is on the screen; the channels and
 * the first commit on the head the firmware lit are stage takeover (scanout.c).
 * When takeover does not run, the heads the firmware lit are read for the log.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-gpu.h"

/*
 * The display's instance memory: 64 KiB of VRAM, 1 MiB below the first
 * block, write-combined in the VRAM's space.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/disp.c
 * (r535_disp_oneinit, the instance memory's size) and r535/nvrm/disp.h
 * (ADDR_FBMEM, NV_MEMORY_WRITECOMBINED).
 */
#define DISPLAY_INSTANCE_BELOW		0x100000ULL
#define DISPLAY_INSTANCE_BYTES		0x10000U
#define DISPLAY_SPACE_VRAM		2U
#define DISPLAY_CACHE_WRITE_COMBINED	2U

/*
 * The controls of the bring-up and the readout.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080internal.h
 * (NV2080_CTRL_CMD_INTERNAL_DISPLAY_WRITE_INST_MEM,
 * NV2080_CTRL_CMD_INTERNAL_DISPLAY_GET_STATIC_INFO,
 * NV2080_CTRL_CMD_INTERNAL_INIT_BRIGHTC_STATE_LOAD), ctrl2080/ctrl2080event.h
 * (NV2080_CTRL_CMD_EVENT_SET_NOTIFICATION), ctrl0073/ctrl0073system.h,
 * ctrl0073/ctrl0073specific.h, ctrl0073/ctrl0073dp.h and
 * ctrl0073/ctrl0073dfp.h (the NV0073_CTRL_CMD_ controls of the same names).
 */
#define DISPLAY_CTL_WRITE_INSTANCE	0x20800a49U
#define DISPLAY_CTL_STATIC_INFO		0x20800a01U
#define DISPLAY_CTL_BRIGHTNESS_LOAD	0x20800ac6U
#define DISPLAY_CTL_MANUAL_DP		0x00731365U
#define DISPLAY_CTL_HEAD_COUNT		0x00730102U
#define DISPLAY_CTL_HEAD_MASK		0x00730287U
#define DISPLAY_CTL_SUPPORTED		0x00730107U
#define DISPLAY_CTL_ACTIVE		0x0073010cU
#define DISPLAY_CTL_OUTPUT_INFO		0x0073028bU
#define DISPLAY_CTL_CONNECTOR_DATA	0x00730250U
#define DISPLAY_CTL_DP_CAPS		0x00731369U
#define DISPLAY_CTL_SET_NOTIFICATION	0x20800301U
#define DISPLAY_CTL_CONNECT_STATE	0x00730108U
#define DISPLAY_CTL_DFP_INFO		0x00731140U
#define DISPLAY_CTL_EDID		0x00730245U
#define DISPLAY_CTL_HDMI_ENABLE		0x00730273U
#define DISPLAY_CTL_HDMI_SINK_CAPS	0x00730293U

/*
 * Their parameter lengths.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080internal.h,
 * ctrl2080/ctrl2080event.h and ctrl0073/ctrl0073system.h, ctrl0073specific.h,
 * ctrl0073dp.h, ctrl0073dfp.h (the controls' _PARAMS structures).
 */
#define DISPLAY_WRITE_INSTANCE_BYTES	24U
#define DISPLAY_STATIC_INFO_BYTES	36U
#define DISPLAY_BRIGHTNESS_LOAD_BYTES	4104U
#define DISPLAY_MANUAL_DP_BYTES		4U
#define DISPLAY_HEAD_COUNT_BYTES	12U
#define DISPLAY_HEAD_MASK_BYTES		8U
#define DISPLAY_SUPPORTED_BYTES		12U
#define DISPLAY_ACTIVE_BYTES		16U
#define DISPLAY_OUTPUT_INFO_BYTES	56U
#define DISPLAY_CONNECTOR_DATA_BYTES	72U
#define DISPLAY_CONNECTOR_COUNT_AT	16U
#define DISPLAY_CONNECTOR_TYPE_AT	24U
#define DISPLAY_DP_CAPS_BYTES		64U
#define DISPLAY_DP_CAPS_SOR_AT		4U
#define DISPLAY_CONNECT_STATE_BYTES	16U
#define DISPLAY_CONNECT_MASK_AT		8U
#define DISPLAY_DFP_INFO_BYTES		16U
#define DISPLAY_EDID_BYTES		2064U
#define DISPLAY_EDID_SIZE_AT		8U
#define DISPLAY_EDID_DATA_AT		16U
#define DISPLAY_EDID_DATA_MAX		2048U
#define DISPLAY_HDMI_BYTES		12U
#define DISPLAY_HDMI_VALUE_AT		8U
#define DISPLAY_TYPE_NONE		0U
#define DISPLAY_OUTPUT_MAX		32U

/*
 * The notifications asked for: hotplug and the DisplayPort interrupt, with
 * their event objects.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/class/cl2080_notification.h
 * (NV2080_NOTIFIERS_HOTPLUG, NV2080_NOTIFIERS_DP_IRQ).
 */
#define DISPLAY_NOTIFY_HOTPLUG		1U
#define DISPLAY_NOTIFY_DP_IRQ		7U
#define DISPLAY_HANDLE_HOTPLUG		0x007e0000U
#define DISPLAY_HANDLE_DP_IRQ		0x007e0001U

/*
 * The fields the driver reads or writes.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080internal.h
 * (NV2080_CTRL_INTERNAL_DISPLAY_GET_STATIC_INFO_PARAMS) and
 * ctrl0073/ctrl0073system.h, ctrl0073specific.h
 * (NV0073_CTRL_SYSTEM_GET_NUM_HEADS_PARAMS,
 * NV0073_CTRL_SPECIFIC_GET_ALL_HEAD_MASK_PARAMS,
 * NV0073_CTRL_SYSTEM_GET_SUPPORTED_PARAMS,
 * NV0073_CTRL_SYSTEM_GET_ACTIVE_PARAMS,
 * NV0073_CTRL_SPECIFIC_OR_GET_INFO_PARAMS).
 */
#define DISPLAY_STATIC_WINDOWS_AT	4U
#define DISPLAY_HEAD_COUNT_AT		8U
#define DISPLAY_HEAD_MASK_AT		4U
#define DISPLAY_SUPPORTED_MASK_AT	4U
#define DISPLAY_ACTIVE_HEAD_AT		4U
#define DISPLAY_ACTIVE_ID_AT		12U
#define DISPLAY_OUTPUT_ID_AT		4U
#define DISPLAY_OUTPUT_INDEX_AT		8U
#define DISPLAY_OUTPUT_TYPE_AT		12U
#define DISPLAY_OUTPUT_PROTOCOL_AT	16U

/*
 * "Not supported": the brightness state the driver has no ACPI method to
 * load.
 *
 * Value referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/nvstatuscodes.h (NV_ERR_NOT_SUPPORTED).
 */
#define DISPLAY_STATUS_NOT_SUPPORTED	0x56U

/*
 * The objects: the common object and the display root (its class is the
 * generation's, the profile's class_display; the handle keeps Turing's
 * number on every generation, as a handle is only the driver's name).
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/class/cl0073.h (NV04_DISPLAY_COMMON) and
 * class/clc570.h (NVC570_DISPLAY).
 */
#define DISPLAY_CLASS_COMMON		0x0073U
#define DISPLAY_HANDLE_COMMON		0x00730000U
#define DISPLAY_HANDLE_ROOT		0xc5700000U

/*
 * The most heads the readout looks at.
 *
 * Value referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/inc/swref/published/disp/v03_00/dev_disp.h
 * (NV_PDISP_FE_HW_SYS_CAP_HEAD_EXISTS__SIZE_1).
 */
#define DISPLAY_HEAD_MAX		8U

/*
 * A head's armed state: the raster, sync and blank words, and the pixel
 * clock (the core's armed copy, from its first head method on).
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/inc/swref/published/disp/v03_00/dev_disp.h
 * (NV_UDISP_FE_CHN_ARMED_BASEADR_CORE) and NVIDIA open-gpu-doc
 * classes/display/clc57d.h (the NVC57D_HEAD_SET_ methods).
 */
#define DISPLAY_REG_ARMED_HEAD		0x68a000U
#define DISPLAY_HEAD_STRIDE		0x400U
#define DISPLAY_ARMED_CONTROL_AT	0x004U
#define DISPLAY_ARMED_CLOCK_AT		0x00cU
#define DISPLAY_ARMED_TOTAL_AT		0x064U
#define DISPLAY_ARMED_SYNC_END_AT	0x068U
#define DISPLAY_ARMED_BLANK_END_AT	0x06cU
#define DISPLAY_ARMED_BLANK_START_AT	0x070U

/*
 * The output kinds and protocols the readout names.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/ctrl/ctrl0073/ctrl0073specific.h
 * (NV0073_CTRL_SPECIFIC_OR_TYPE_SOR, NV0073_CTRL_SPECIFIC_OR_PROTOCOL_SOR_*).
 */
#define DISPLAY_TYPE_SOR		2U
#define DISPLAY_PROTOCOL_TMDS_A		1U
#define DISPLAY_PROTOCOL_TMDS_B		2U
#define DISPLAY_PROTOCOL_TMDS_DUAL	5U
#define DISPLAY_PROTOCOL_DP_A		8U
#define DISPLAY_PROTOCOL_DP_B		9U

static int display_control(struct nvrtx_gpu *gpu, uint32_t client, uint32_t object, uint32_t command, uint8_t *params, size_t length);
static int display_instance(struct nvrtx_gpu *gpu);
static int display_client(struct nvrtx_gpu *gpu);
static int display_counts(struct nvrtx_gpu *gpu);
static int display_outputs(struct nvrtx_gpu *gpu);
static int display_event(struct nvrtx_gpu *gpu, uint32_t handle, uint32_t notify, int *made);
static int display_edid(struct nvrtx_gpu *gpu, uint32_t index);
static void display_readout(struct nvrtx_gpu *gpu);
static void display_head(struct nvrtx_gpu *gpu, uint32_t head, uint32_t display_id);
static const char *display_protocol_name(uint32_t protocol);

/*
 * Runs the display-client stage: the display's instance memory, its objects and
 * the readout of the lit heads.
 *
 * A failure is returned; the finish's stop frees the objects that were made.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/disp.c
 * (r535_disp_oneinit), with the system controls' numbers of NVIDIA
 * open-gpu-kernel-modules 570.144.
 */
int
nvrtx_display_start(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_display_state *display;
	bool takeover;
	bool allowed;
	int error;

	display = &gpu->display;

	/* Gives the resource manager the display's instance memory. */
	nvrtx_stage_mark("display begin");
	error = display_instance(gpu);
	if (error != 0)
		return error;

	/* Makes the display's own client, device and subdevice, and the common object under them. */
	error = display_client(gpu);
	if (error != 0)
		return error;

	error = nvrtx_rm_allocate_in(gpu, NVRTX_DISP_CLIENT, NVRTX_DISP_DEVICE, DISPLAY_HANDLE_COMMON, DISPLAY_CLASS_COMMON, NULL, 0U);
	if (error != 0)
		return error;

	display->common_made = 1;

	/*
	 * Reads the windows and heads, and declares DisplayPort handled by
	 * hand.
	 */
	error = display_counts(gpu);
	if (error != 0)
		return error;

	/* Reads each output's resource and connector. */
	error = display_outputs(gpu);
	if (error != 0)
		return error;

	/* Asks for the hotplug and DisplayPort interrupt notifications. */
	error = display_event(gpu, DISPLAY_HANDLE_HOTPLUG, DISPLAY_NOTIFY_HOTPLUG, &display->hotplug_made);
	if (error != 0)
		return error;

	error = display_event(gpu, DISPLAY_HANDLE_DP_IRQ, DISPLAY_NOTIFY_DP_IRQ, &display->dp_irq_made);
	if (error != 0)
		return error;

	/* Clears the hash table at the start of the instance memory. */
	error = nvrtx_vram_zero(gpu, display->instance, NVRTX_DISP_RAMHT_BYTES);
	if (error != 0)
		return error;

	/* Makes the Turing display root under the display's device. */
	error = nvrtx_rm_allocate_in(gpu, NVRTX_DISP_CLIENT, NVRTX_DISP_DEVICE, DISPLAY_HANDLE_ROOT, gpu->identity.chip.profile->class_display, NULL, 0U);
	if (error != 0)
		return error;

	display->root_made = 1;

	/*
	 * Without the takeover the heads the firmware lit are read here for the
	 * log; with it, takeover reads them in its order.
	 */
	takeover = nvrtx_stage_disp_wanted();
	allowed = nvrtx_stage_allowed("takeover");
	if (!takeover || !allowed)
		display_readout(gpu);

	/* Succeeded: the display is the driver's to drive from here. */
	nvrtx_stage_mark("display ok: display, heads %x, windows %x, outputs %x",
	    (unsigned)display->head_mask,
	    (unsigned)display->window_mask,
	    (unsigned)display->output_mask);
	return 0;
}

/*
 * Frees the display's objects before the unload; failures are only shown.
 */
void
nvrtx_display_stop(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_display_state *display;
	int failures;
	int error;

	display = &gpu->display;
	failures = 0;

	/* Frees takeover's channels first: they are children of the root. */
	nvrtx_scanout_stop(gpu);

	/* Frees the root, then the two event objects, then the common object. */
	if (display->root_made != 0) {
		error = nvrtx_rm_free_in(gpu, NVRTX_DISP_CLIENT, DISPLAY_HANDLE_ROOT);
		if (error != 0)
			failures++;
		display->root_made = 0;
	}

	if (display->dp_irq_made != 0) {
		error = nvrtx_rm_free_in(gpu, NVRTX_DISP_CLIENT, DISPLAY_HANDLE_DP_IRQ);
		if (error != 0)
			failures++;
		display->dp_irq_made = 0;
	}

	if (display->hotplug_made != 0) {
		error = nvrtx_rm_free_in(gpu, NVRTX_DISP_CLIENT, DISPLAY_HANDLE_HOTPLUG);
		if (error != 0)
			failures++;
		display->hotplug_made = 0;
	}

	if (display->common_made != 0) {
		error = nvrtx_rm_free_in(gpu, NVRTX_DISP_CLIENT, DISPLAY_HANDLE_COMMON);
		if (error != 0)
			failures++;
		display->common_made = 0;
	}

	/* Frees the subdevice, the device and the client. */
	if (display->subdevice_made != 0) {
		error = nvrtx_rm_free_in(gpu, NVRTX_DISP_CLIENT, NVRTX_DISP_SUBDEVICE);
		if (error != 0)
			failures++;
		display->subdevice_made = 0;
	}

	if (display->device_made != 0) {
		error = nvrtx_rm_free_in(gpu, NVRTX_DISP_CLIENT, NVRTX_DISP_DEVICE);
		if (error != 0)
			failures++;
		display->device_made = 0;
	}

	if (display->client_made != 0) {
		error = nvrtx_rm_free_in(gpu, NVRTX_DISP_CLIENT, NVRTX_DISP_CLIENT);
		if (error != 0)
			failures++;
		display->client_made = 0;
	}

	/* Says how the stop went, when anything failed. */
	if (failures != 0)
		nvrtx_stage_mark("channel-stop: %d display frees failed; the unload removes the rest", failures);
}

/* Sends a control whose reply the caller reads in place: the reply is copied back over the parameters. */
static int
display_control(
	struct nvrtx_gpu *gpu,
	uint32_t client,
	uint32_t object,
	uint32_t command,
	uint8_t *params,
	size_t length)
{
	const uint8_t *reply;
	size_t reply_length;
	int error;

	/* Holds the GSP's lock from the call to the copy of its reply (the reply lies in the one reply buffer). */
	nvrtx_rpc_lock(gpu);

	/* Sends the control and waits for the reply. */
	error = nvrtx_rm_control(gpu, client, object, command, params, length, &reply, &reply_length);

	/* A short reply is not this layout. */
	if (error == 0 && reply_length < length)
		error = EIO;

	/* Copies the reply back into the parameters while it is still the GSP's last. */
	if (error == 0)
		kern_memcpy(params, reply, length);

	nvrtx_rpc_unlock(gpu);

	/* A call that failed, or a short reply that is not this layout. */
	if (error != 0)
		return error;

	/* Succeeded: the reply is in the parameters. */
	return 0;
}

/* Hands the display's instance memory to the resource manager. */
static int
display_instance(
	struct nvrtx_gpu *gpu)
{
	uint8_t params[DISPLAY_WRITE_INSTANCE_BYTES];
	int error;

	/* Takes 64 KiB 1 MiB below the first block (between it and the graphics arena); its hash table is cleared later. */
	gpu->display.instance = gpu->engine.vram_block - DISPLAY_INSTANCE_BELOW;

	/* Names it: its address, size, the VRAM and write-combined. */
	kern_memset(params, 0, sizeof(params));
	nvrtx_put64(params, 0U, gpu->display.instance);
	nvrtx_put64(params, 8U, DISPLAY_INSTANCE_BYTES);
	nvrtx_put32(params, 16U, DISPLAY_SPACE_VRAM);
	nvrtx_put32(params, 20U, DISPLAY_CACHE_WRITE_COMBINED);
	error = display_control(gpu, gpu->internal_client, gpu->internal_subdevice, DISPLAY_CTL_WRITE_INSTANCE, params, sizeof(params));
	if (error != 0)
		return error;

	/* Succeeded: the resource manager uses the memory. */
	return 0;
}

/*
 * Finds the output the firmware lit and its head (the takeover stage): for each
 * output with a resource, in the order of its id, asks each head which display
 * it drives until one drives this output, then asks the output's resource and
 * protocol.  The first lit output is the one taken over.  Returns 0 (with
 * gop_head_found clear when nothing is lit) or the error of a control.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/disp.c
 * (r535_outp_inherit).
 */
int
nvrtx_display_inherit(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_display_state *display;
	uint8_t params[DISPLAY_OUTPUT_INFO_BYTES];
	uint32_t index;
	uint32_t display_id;
	uint32_t head;
	uint32_t active;
	int error;

	display = &gpu->display;

	/* Each output with a resource, in the order of its id. */
	for (index = 0U; index < DISPLAY_OUTPUT_MAX; index++) {
		display_id = 1U << index;
		if ((display->output_mask & display_id) == 0U)
			continue;
		if (display->output_type[index] == DISPLAY_TYPE_NONE)
			continue;

		/* Each head until one drives this output. */
		for (head = 0U; head < display->head_count; head++) {
			kern_memset(params, 0, DISPLAY_ACTIVE_BYTES);
			nvrtx_put32(params, DISPLAY_ACTIVE_HEAD_AT, head);
			error = display_control(gpu, NVRTX_DISP_CLIENT, DISPLAY_HANDLE_COMMON, DISPLAY_CTL_ACTIVE, params, DISPLAY_ACTIVE_BYTES);
			if (error != 0)
				return error;

			active = nvrtx_get32(params, DISPLAY_ACTIVE_ID_AT);
			if (active != display_id)
				continue;

			/* The output's resource and protocol as armed. */
			nvrtx_disp_first_display_params(params, DISPLAY_OUTPUT_INFO_BYTES, display_id);
			error = display_control(gpu, NVRTX_DISP_CLIENT, DISPLAY_HANDLE_COMMON, DISPLAY_CTL_OUTPUT_INFO, params, DISPLAY_OUTPUT_INFO_BYTES);
			if (error != 0)
				return error;

			display->lit_mask |= 1U << head;
			nvrtx_stage_mark("takeover output %x lit on head %u: SOR %u %s",
			    (unsigned)display_id,
			    (unsigned)head,
			    (unsigned)nvrtx_get32(params, DISPLAY_OUTPUT_INDEX_AT),
			    display_protocol_name(nvrtx_get32(params, DISPLAY_OUTPUT_PROTOCOL_AT)));

			/* The first lit output is the one taken over. */
			if (display->gop_head_found == 0) {
				display->gop_head_found = 1;
				display->gop_head = head;
				display->gop_display_id = display_id;
				display->gop_sor = nvrtx_get32(params, DISPLAY_OUTPUT_INDEX_AT);
				display->gop_protocol = nvrtx_get32(params, DISPLAY_OUTPUT_PROTOCOL_AT);
			}

			break;
		}
	}

	/* Succeeded: the lit outputs are known. */
	return 0;
}

/*
 * Detects the connected outputs (the takeover stage): each output with a
 * resource is asked whether a sink is connected; a connected one is asked its
 * flat-panel information, and a connected TMDS output its EDID, which for the
 * lit output gives the preferred timing and the HDMI facts.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/disp.c
 * (r535_outp_detect, r535_outp_dfp_get_info, r535_tmds_edid_get).
 */
int
nvrtx_display_detect(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_display_state *display;
	uint8_t params[DISPLAY_CONNECT_STATE_BYTES];
	uint32_t index;
	uint32_t display_id;
	uint32_t protocol;
	int error;

	display = &gpu->display;

	/* Each output with a resource, in the order of its id. */
	for (index = 0U; index < DISPLAY_OUTPUT_MAX; index++) {
		display_id = 1U << index;
		if ((display->output_mask & display_id) == 0U)
			continue;
		if (display->output_type[index] == DISPLAY_TYPE_NONE)
			continue;

		/* Whether a sink is connected. */
		kern_memset(params, 0, sizeof(params));
		nvrtx_put32(params, DISPLAY_CONNECT_MASK_AT, display_id);
		error = display_control(gpu, NVRTX_DISP_CLIENT, DISPLAY_HANDLE_COMMON, DISPLAY_CTL_CONNECT_STATE, params, DISPLAY_CONNECT_STATE_BYTES);
		if (error != 0)
			return error;

		if ((nvrtx_get32(params, DISPLAY_CONNECT_MASK_AT) & display_id) == 0U)
			continue;

		display->connected_mask |= display_id;

		/* The flat panel's information. */
		nvrtx_disp_first_display_params(params, DISPLAY_DFP_INFO_BYTES, display_id);
		error = display_control(gpu, NVRTX_DISP_CLIENT, DISPLAY_HANDLE_COMMON, DISPLAY_CTL_DFP_INFO, params, DISPLAY_DFP_INFO_BYTES);
		if (error != 0)
			return error;

		/* A TMDS sink's EDID. */
		protocol = display->output_protocol[index];
		if (protocol == DISPLAY_PROTOCOL_TMDS_A ||
		    protocol == DISPLAY_PROTOCOL_TMDS_B ||
		    protocol == DISPLAY_PROTOCOL_TMDS_DUAL) {
			error = display_edid(gpu, index);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: the connected outputs are known. */
	nvrtx_stage_mark("takeover connected %x", (unsigned)display->connected_mask);
	return 0;
}

/*
 * Tells the resource manager the lit output drives an HDMI sink, and the
 * sink's capabilities, before the first commit.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/disp.c
 * (r535_sor_hdmi_ctrl, r535_sor_hdmi_scdc).
 */
int
nvrtx_display_hdmi(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_display_state *display;
	uint8_t params[DISPLAY_HDMI_BYTES];
	int error;

	display = &gpu->display;

	/* HDMI on for the display. */
	nvrtx_disp_first_display_params(params, sizeof(params), display->gop_display_id);
	nvrtx_put32(params, DISPLAY_HDMI_VALUE_AT, 1U);
	error = display_control(gpu, NVRTX_DISP_CLIENT, DISPLAY_HANDLE_COMMON, DISPLAY_CTL_HDMI_ENABLE, params, sizeof(params));
	if (error != 0)
		return error;

	/* The sink's capabilities. */
	nvrtx_disp_first_display_params(params, sizeof(params), display->gop_display_id);
	nvrtx_put32(params, DISPLAY_HDMI_VALUE_AT, display->sink.caps);
	error = display_control(gpu, NVRTX_DISP_CLIENT, DISPLAY_HANDLE_COMMON, DISPLAY_CTL_HDMI_SINK_CAPS, params, sizeof(params));
	if (error != 0)
		return error;

	/* Succeeded: the output sends HDMI. */
	return 0;
}

/*
 * Makes the lit output's mode list from its EDID (the takeover stage, once
 * the armed state is read): every timing the sink names that its range,
 * the link from the output's connector (nvrtx_disp_first_link_caps(), the
 * firmware's armed clock counting as carried) and the head's size take, the
 * default first.  Without an EDID, or when it cannot be decoded, the list
 * stays empty and the head keeps its one mode.
 */
void
nvrtx_display_modes(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_display_state *display;
	struct gpu_link_caps caps;
	struct gpu_edid *edid;
	uint32_t index;
	uint32_t connector;
	uint32_t reason;
	int dual_link;
	int error;

	display = &gpu->display;
	display->modes.count = 0U;

	/* No EDID, no list. */
	if (display->edid_length == 0U) {
		nvrtx_stage_mark("takeover modes: no EDID; the head keeps its mode");
		return;
	}

	/* The decode's storage (a few kilobytes, too large for this stack). */
	edid = kern_calloc(1U, sizeof(*edid));
	if (edid == NULL) {
		nvrtx_stage_mark("takeover modes: no memory for the EDID's decode; the head keeps its mode");
		return;
	}

	error = gpu_edid_decode(display->edid, display->edid_length, edid);
	if (error != 0) {
		kern_free(edid);
		nvrtx_stage_mark("takeover modes: the EDID does not decode (%d); the head keeps its mode", error);
		return;
	}

	/* The link from the output's connector, with the clock the firmware drives it at. */
	index = 0U;
	while (index < 32U && (display->gop_display_id >> index) != 1U)
		index++;
	connector = NVRTX_DISP_CONNECTOR_NONE;
	if (index < 32U)
		connector = display->output_connector[index];
	dual_link = 0;
	if (display->gop_protocol == DISPLAY_PROTOCOL_TMDS_DUAL)
		dual_link = 1;
	nvrtx_disp_first_link_caps(connector, dual_link, display->armed[NVRTX_DISP_ARMED_PIXEL_CLOCK] / 1000U, &caps);

	/* The list, the default first. */
	(void)gpu_modes_build(edid, &caps, NULL, NULL, &display->modes);
	display->width_mm = edid->width_mm;
	display->height_mm = edid->height_mm;
	nvrtx_stage_mark("takeover sink %s %s, connector %x, TMDS to %u kHz",
	    edid->vendor,
	    edid->name,
	    (unsigned)connector,
	    (unsigned)caps.tmds_max_khz);

	/* Each timing the output cannot show, and why (the GPU_MODES_ code). */
	for (index = 0U; index < edid->timing_count; index++) {
		reason = gpu_modes_check(edid, &caps, NULL, NULL, &edid->timings[index]);
		if (reason != GPU_MODES_FITS)
			nvrtx_stage_mark("takeover modes: no %ux%u at %u mHz, %u kHz (%u)",
			    (unsigned)edid->timings[index].h_active,
			    (unsigned)edid->timings[index].v_active,
			    (unsigned)edid->timings[index].refresh_mhz,
			    (unsigned)edid->timings[index].pixel_khz,
			    (unsigned)reason);
	}

	nvrtx_stage_mark("takeover modes: %u of %u timings, default %ux%u at %u mHz",
	    (unsigned)display->modes.count,
	    (unsigned)edid->timing_count,
	    display->modes.count != 0U ? (unsigned)display->modes.modes[0].h_active : 0U,
	    display->modes.count != 0U ? (unsigned)display->modes.modes[0].v_active : 0U,
	    display->modes.count != 0U ? (unsigned)display->modes.modes[0].refresh_mhz : 0U);
	kern_free(edid);
}

/* Makes the display's own client, its device sharing it, and the device's first subdevice. */
static int
display_client(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_display_state *display;
	uint8_t params[NVRTX_ROOT_PARAMS_BYTES];
	int error;

	display = &gpu->display;

	/* The client is its own parent; it names itself and no process. */
	kern_memset(params, 0, sizeof(params));
	nvrtx_put32(params, 0U, NVRTX_DISP_CLIENT);
	nvrtx_put32(params, NVRTX_ROOT_PROCESS_AT, 0xffffffffU);
	error = nvrtx_rm_allocate_in(gpu, NVRTX_DISP_CLIENT, NVRTX_DISP_CLIENT, NVRTX_DISP_CLIENT, NVRTX_CLASS_ROOT, params, NVRTX_ROOT_PARAMS_BYTES);
	if (error != 0)
		return error;

	display->client_made = 1;

	/* The device shares the client. */
	kern_memset(params, 0, sizeof(params));
	nvrtx_put32(params, NVRTX_DEVICE_SHARE_AT, NVRTX_DISP_CLIENT);
	error = nvrtx_rm_allocate_in(gpu, NVRTX_DISP_CLIENT, NVRTX_DISP_CLIENT, NVRTX_DISP_DEVICE, NVRTX_CLASS_DEVICE, params, NVRTX_DEVICE_PARAMS_BYTES);
	if (error != 0)
		return error;

	display->device_made = 1;

	/* The subdevice is the device's first. */
	kern_memset(params, 0, sizeof(params));
	error = nvrtx_rm_allocate_in(gpu, NVRTX_DISP_CLIENT, NVRTX_DISP_DEVICE, NVRTX_DISP_SUBDEVICE, NVRTX_CLASS_SUBDEVICE, params, NVRTX_SUBDEVICE_PARAMS_BYTES);
	if (error != 0)
		return error;

	/* Succeeded: the display's objects have their own client. */
	display->subdevice_made = 1;
	return 0;
}

/*
 * Reads, for each output (display id) the board has, its resource, type
 * and protocol, then its connector (and a DisplayPort output's
 * capabilities); an output with no resource is passed over.
 */
static int
display_outputs(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_display_state *display;
	uint8_t params[DISPLAY_CONNECTOR_DATA_BYTES];
	uint32_t index;
	uint32_t display_id;
	uint32_t type;
	uint32_t protocol;
	int error;

	display = &gpu->display;

	/* Each output in the order of its id. */
	for (index = 0U; index < DISPLAY_OUTPUT_MAX; index++) {
		display_id = 1U << index;
		if ((display->output_mask & display_id) == 0U)
			continue;

		/* Its output resource, type and protocol. */
		nvrtx_disp_first_display_params(params, DISPLAY_OUTPUT_INFO_BYTES, display_id);
		error = display_control(gpu, NVRTX_DISP_CLIENT, DISPLAY_HANDLE_COMMON, DISPLAY_CTL_OUTPUT_INFO, params, DISPLAY_OUTPUT_INFO_BYTES);
		if (error != 0)
			return error;

		type = nvrtx_get32(params, DISPLAY_OUTPUT_TYPE_AT);
		protocol = nvrtx_get32(params, DISPLAY_OUTPUT_PROTOCOL_AT);
		display->output_or[index] = nvrtx_get32(params, DISPLAY_OUTPUT_INDEX_AT);
		display->output_type[index] = type;
		display->output_protocol[index] = protocol;
		if (type == DISPLAY_TYPE_NONE)
			continue;

		/* Its connector: the type of the first one, which bounds what the link carries. */
		nvrtx_disp_first_display_params(params, DISPLAY_CONNECTOR_DATA_BYTES, display_id);
		error = display_control(gpu, NVRTX_DISP_CLIENT, DISPLAY_HANDLE_COMMON, DISPLAY_CTL_CONNECTOR_DATA, params, DISPLAY_CONNECTOR_DATA_BYTES);
		if (error != 0)
			return error;

		display->output_connector[index] = NVRTX_DISP_CONNECTOR_NONE;
		if (nvrtx_get32(params, DISPLAY_CONNECTOR_COUNT_AT) != 0U)
			display->output_connector[index] = nvrtx_get32(params, DISPLAY_CONNECTOR_TYPE_AT);

		/* A DisplayPort output's capabilities, for any SOR. */
		if (protocol == DISPLAY_PROTOCOL_DP_A || protocol == DISPLAY_PROTOCOL_DP_B) {
			kern_memset(params, 0, sizeof(params));
			nvrtx_put32(params, DISPLAY_DP_CAPS_SOR_AT, 0xffffffffU);
			error = display_control(gpu, NVRTX_DISP_CLIENT, DISPLAY_HANDLE_COMMON, DISPLAY_CTL_DP_CAPS, params, DISPLAY_DP_CAPS_BYTES);
			if (error != 0)
				return error;
		}

		nvrtx_stage_mark("display output %x: %s %u %s, connector %x",
		    (unsigned)display_id,
		    type == DISPLAY_TYPE_SOR ? "SOR" : "OR",
		    (unsigned)display->output_or[index],
		    display_protocol_name(protocol),
		    (unsigned)display->output_connector[index]);
	}

	/* Succeeded: every output is known. */
	return 0;
}

/*
 * Reads one connected TMDS output's EDID; for the output whose id takeover
 * takes over, keeps its bytes (whole blocks, at most GPU_DISPLAY_EDID_MAX),
 * its preferred timing and its sink's facts.
 */
static int
display_edid(
	struct nvrtx_gpu *gpu,
	uint32_t index)
{
	struct nvrtx_display_state *display;
	uint8_t *params;
	uint32_t size;
	int error;

	display = &gpu->display;

	/* The parameters are 2 KiB of EDID and more. */
	params = kern_calloc(1U, DISPLAY_EDID_BYTES);
	if (params == NULL)
		return ENOMEM;

	/* Asks for the EDID. */
	nvrtx_put32(params, 4U, 1U << index);
	error = display_control(gpu, NVRTX_DISP_CLIENT, DISPLAY_HANDLE_COMMON, DISPLAY_CTL_EDID, params, DISPLAY_EDID_BYTES);
	if (error != 0) {
		kern_free(params);
		return error;
	}

	/* The lit output's EDID gives the mode and the sink. */
	size = nvrtx_get32(params, DISPLAY_EDID_SIZE_AT);
	if (size > DISPLAY_EDID_DATA_MAX)
		size = DISPLAY_EDID_DATA_MAX;
	if (display->gop_head_found != 0 && display->gop_display_id == (1U << index)) {
		/* The bytes GPU_DISPLAY_EDID returns and the mode list is made from. */
		display->edid_total = size;
		display->edid_length = size / GPU_EDID_BLOCK_BYTES * GPU_EDID_BLOCK_BYTES;
		if (display->edid_length > GPU_DISPLAY_EDID_MAX)
			display->edid_length = GPU_DISPLAY_EDID_MAX;
		kern_memcpy(display->edid, params + DISPLAY_EDID_DATA_AT, display->edid_length);

		error = nvrtx_disp_first_edid_timing(params + DISPLAY_EDID_DATA_AT, size, &display->timing);
		if (error == 0)
			display->timing_from_edid = 1;
		nvrtx_disp_first_edid_sink(params + DISPLAY_EDID_DATA_AT, size, &display->sink);
		nvrtx_stage_mark("takeover EDID %x: %u bytes, preferred %ux%u at %u kHz, %s%s",
		    (unsigned)(1U << index),
		    (unsigned)size,
		    (unsigned)display->timing.hdisplay,
		    (unsigned)display->timing.vdisplay,
		    (unsigned)display->timing.clock_khz,
		    display->sink.hdmi != 0 ? "HDMI" : "DVI",
		    display->sink.audio != 0 ? " with audio" : "");
	}

	kern_free(params);

	/* Succeeded: the EDID was read. */
	return 0;
}

/* Makes an event object for one notification of the display's subdevice and asks for it each time. */
static int
display_event(
	struct nvrtx_gpu *gpu,
	uint32_t handle,
	uint32_t notify,
	int *made)
{
	uint8_t params[NVRTX_DISP_EVENT_BYTES];
	int error;

	/* The event object under the subdevice. */
	nvrtx_disp_first_event_params(params, NVRTX_DISP_CLIENT, notify);
	error = nvrtx_rm_allocate_in(gpu, NVRTX_DISP_CLIENT, NVRTX_DISP_SUBDEVICE, handle, NVRTX_DISP_CLASS_EVENT, params, sizeof(params));
	if (error != 0)
		return error;

	*made = 1;

	/* The notification, repeated. */
	nvrtx_disp_first_notification_params(params, notify);
	error = display_control(gpu, NVRTX_DISP_CLIENT, NVRTX_DISP_SUBDEVICE, DISPLAY_CTL_SET_NOTIFICATION, params, NVRTX_DISP_NOTIFICATION_BYTES);
	if (error != 0)
		return error;

	/* Succeeded: the notification comes as a message. */
	return 0;
}

/*
 * Reads the windows and heads, loads the brightness state and takes DisplayPort
 * by hand.
 *
 * The brightness state is "not supported" (the driver evaluates no ACPI
 * method for it).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/disp.c
 * (r535_disp_oneinit, the brightness state sent when the method is
 * absent).
 */
static int
display_counts(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_display_state *display;
	uint8_t params[DISPLAY_HEAD_COUNT_BYTES + DISPLAY_STATIC_INFO_BYTES];
	uint8_t *brightness;
	int error;

	display = &gpu->display;

	/* The windows the hardware has. */
	kern_memset(params, 0, sizeof(params));
	error = display_control(gpu, gpu->internal_client, gpu->internal_subdevice, DISPLAY_CTL_STATIC_INFO, params, DISPLAY_STATIC_INFO_BYTES);
	if (error != 0)
		return error;

	display->window_mask = nvrtx_get32(params, DISPLAY_STATIC_WINDOWS_AT);

	/* The brightness state. */
	brightness = kern_calloc(1U, DISPLAY_BRIGHTNESS_LOAD_BYTES);
	if (brightness == NULL)
		return ENOMEM;

	nvrtx_put32(brightness, 0U, DISPLAY_STATUS_NOT_SUPPORTED);
	error = display_control(gpu, gpu->internal_client, gpu->internal_subdevice, DISPLAY_CTL_BRIGHTNESS_LOAD, brightness, DISPLAY_BRIGHTNESS_LOAD_BYTES);
	kern_free(brightness);
	if (error != 0)
		return error;

	/* DisplayPort is handled by the driver's calls, not by the resource manager on its own. */
	kern_memset(params, 0, sizeof(params));
	error = display_control(gpu, NVRTX_DISP_CLIENT, DISPLAY_HANDLE_COMMON, DISPLAY_CTL_MANUAL_DP, params, DISPLAY_MANUAL_DP_BYTES);
	if (error != 0)
		return error;

	/* The number of heads, and which exist. */
	kern_memset(params, 0, sizeof(params));
	error = display_control(gpu, NVRTX_DISP_CLIENT, DISPLAY_HANDLE_COMMON, DISPLAY_CTL_HEAD_COUNT, params, DISPLAY_HEAD_COUNT_BYTES);
	if (error != 0)
		return error;

	display->head_count = nvrtx_get32(params, DISPLAY_HEAD_COUNT_AT);
	kern_memset(params, 0, sizeof(params));
	error = display_control(gpu, NVRTX_DISP_CLIENT, DISPLAY_HANDLE_COMMON, DISPLAY_CTL_HEAD_MASK, params, DISPLAY_HEAD_MASK_BYTES);
	if (error != 0)
		return error;

	display->head_mask = nvrtx_get32(params, DISPLAY_HEAD_MASK_AT);

	/* The outputs (display ids) the board has. */
	kern_memset(params, 0, sizeof(params));
	error = display_control(gpu, NVRTX_DISP_CLIENT, DISPLAY_HANDLE_COMMON, DISPLAY_CTL_SUPPORTED, params, DISPLAY_SUPPORTED_BYTES);
	if (error != 0)
		return error;

	/* Succeeded: the display's shape is known. */
	display->output_mask = nvrtx_get32(params, DISPLAY_SUPPORTED_MASK_AT);
	nvrtx_stage_mark("display heads %u (%x), windows %x, outputs %x",
	    (unsigned)display->head_count,
	    (unsigned)display->head_mask,
	    (unsigned)display->window_mask,
	    (unsigned)display->output_mask);
	return 0;
}

/* Asks, for every head, which output the firmware lit on it, and reads its timing. */
static void
display_readout(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_display_state *display;
	uint8_t params[DISPLAY_ACTIVE_BYTES];
	uint32_t head;
	uint32_t display_id;
	int error;

	display = &gpu->display;

	/* Asks for each head that exists. */
	for (head = 0U; head < DISPLAY_HEAD_MAX; head++) {
		if ((display->head_mask & (1U << head)) == 0U)
			continue;

		kern_memset(params, 0, sizeof(params));
		nvrtx_put32(params, DISPLAY_ACTIVE_HEAD_AT, head);
		error = display_control(gpu, NVRTX_DISP_CLIENT, DISPLAY_HANDLE_COMMON, DISPLAY_CTL_ACTIVE, params, sizeof(params));
		if (error != 0) {
			nvrtx_stage_mark("display head %u: active query failed (%d)", (unsigned)head, error);
			continue;
		}

		/* A head with no output is dark. */
		display_id = nvrtx_get32(params, DISPLAY_ACTIVE_ID_AT);
		if (display_id == 0U)
			continue;

		display->lit_mask |= 1U << head;
		display_head(gpu, head, display_id);
	}
}

/* Shows one lit head: its output (SOR and protocol) and the timing it was armed with. */
static void
display_head(
	struct nvrtx_gpu *gpu,
	uint32_t head,
	uint32_t display_id)
{
	uint8_t params[DISPLAY_OUTPUT_INFO_BYTES];
	uint32_t base;
	uint32_t total;
	uint32_t blank_end;
	uint32_t blank_start;
	uint32_t clock;
	uint32_t output;
	uint32_t type;
	uint32_t protocol;
	int error;

	/* Asks which output resource drives the display. */
	kern_memset(params, 0, sizeof(params));
	nvrtx_put32(params, DISPLAY_OUTPUT_ID_AT, display_id);
	error = display_control(gpu, NVRTX_DISP_CLIENT, DISPLAY_HANDLE_COMMON, DISPLAY_CTL_OUTPUT_INFO, params, sizeof(params));
	output = 0xffffffffU;
	type = 0U;
	protocol = 0xffffffffU;
	if (error == 0) {
		output = nvrtx_get32(params, DISPLAY_OUTPUT_INDEX_AT);
		type = nvrtx_get32(params, DISPLAY_OUTPUT_TYPE_AT);
		protocol = nvrtx_get32(params, DISPLAY_OUTPUT_PROTOCOL_AT);
	}

	/* Keeps the first lit head for takeover. */
	if (gpu->display.gop_head_found == 0) {
		gpu->display.gop_head_found = 1;
		gpu->display.gop_head = head;
		gpu->display.gop_display_id = display_id;
		gpu->display.gop_sor = output;
		gpu->display.gop_protocol = protocol;
	}

	/* Reads the armed raster: active size is blank start less blank end. */
	base = DISPLAY_REG_ARMED_HEAD + head * DISPLAY_HEAD_STRIDE;
	total = nvrtx_rd32(gpu, base + DISPLAY_ARMED_TOTAL_AT);
	blank_end = nvrtx_rd32(gpu, base + DISPLAY_ARMED_BLANK_END_AT);
	blank_start = nvrtx_rd32(gpu, base + DISPLAY_ARMED_BLANK_START_AT);
	clock = nvrtx_rd32(gpu, base + DISPLAY_ARMED_CLOCK_AT);
	nvrtx_stage_mark("display head %u: display %x %s %u %s", (unsigned)head, (unsigned)display_id,
	    type == DISPLAY_TYPE_SOR ? "SOR" : "OR",
	    (unsigned)output,
	    display_protocol_name(protocol));
	nvrtx_stage_mark("display head %u: %ux%u of %ux%u, clock %u",
	    (unsigned)head,
	    (unsigned)((blank_start & 0xffffU) - (blank_end & 0xffffU)),
	    (unsigned)((blank_start >> 16) - (blank_end >> 16)),
	    (unsigned)(total & 0xffffU),
	    (unsigned)(total >> 16),
	    (unsigned)clock);
}

/* Names an output protocol for the marks. */
static const char *
display_protocol_name(
	uint32_t protocol)
{
	/* The protocols a Turing board's connectors use. */
	switch (protocol) {
	case DISPLAY_PROTOCOL_TMDS_A:
		return "TMDS-A";
	case DISPLAY_PROTOCOL_TMDS_B:
		return "TMDS-B";
	case DISPLAY_PROTOCOL_TMDS_DUAL:
		return "TMDS-dual";
	case DISPLAY_PROTOCOL_DP_A:
		return "DP-A";
	case DISPLAY_PROTOCOL_DP_B:
		return "DP-B";
	default:
		break;
	}

	/* Anything else is shown by number elsewhere. */
	return "other";
}
