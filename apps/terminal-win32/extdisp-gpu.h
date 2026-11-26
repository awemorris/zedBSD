/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Win32 zero-copy GPU capture and encode pipeline: Virtual-Display-Driver
 * named pipe control, DXGI Output Duplication, D3D11-to-Vulkan external
 * memory import with KeyedMutex synchronization, Vulkan compute BGRA/RGBA
 * to NV12 conversion, and Vulkan Video H.264 hardware encoding.
 */

#ifndef MIKUTERM_WIN32_EXTDISP_GPU_H
#define MIKUTERM_WIN32_EXTDISP_GPU_H

#include <stddef.h>
#include <stdint.h>

struct extdisp_gpu;

/* Result codes returned by extdisp_gpu_capture_encode(). */
#define EXTDISP_GPU_OK		0
#define EXTDISP_GPU_IDLE	1
#define EXTDISP_GPU_REBUILD	2
#define EXTDISP_GPU_ERROR	(-1)

/* Sends a control command to \\.\pipe\MTTVirtualDisplayPipe if present. Returns 0 on success. */
int extdisp_vdd_pipe_command(const char *cmd, char *resp, size_t resp_cap);

/* Returns 1 if vdd\MttVDD.inf is present next to mikuterm.exe, 0 otherwise. */
int extdisp_vdd_package_present(void);

/* Elevated --install-vdd entry point: registers Root\MttVDD and installs vdd\MttVDD.inf. */
int extdisp_vdd_install(void);

/* Opens the DXGI capture pipeline (Vulkan Video zero-copy or DXGI staging fallback). Returns 0 on success. */
int extdisp_gpu_open(uint32_t pref_width, uint32_t pref_height, int wait_for_secondary, struct extdisp_gpu **out);

/* Returns 1 if the pipeline is using zero-copy GPU encode, 0 for DXGI staging CPU readback. */
int extdisp_gpu_is_zero_copy(const struct extdisp_gpu *gpu);

/* Returns the active pipeline mode name ("vulkan-video", "d3d12-video", or "dxgi-staging"). */
const char *extdisp_gpu_mode_name(const struct extdisp_gpu *gpu);

/* Rebuilds the capture and encode resources after a display mode or resolution change. */
int extdisp_gpu_rebuild(struct extdisp_gpu *gpu);

/* Returns the active capture width and height. */
void extdisp_gpu_dimensions(const struct extdisp_gpu *gpu, uint32_t *out_w, uint32_t *out_h);

/* Returns the captured output's virtual-desktop coordinates. Returns 0 on success. */
int extdisp_gpu_desktop_rect(const struct extdisp_gpu *gpu, int *left, int *top, int *right, int *bottom);

/* Captures one frame and encodes it to Annex-B H.264 (zero-copy mode). */
int extdisp_gpu_capture_encode(struct extdisp_gpu *gpu, int force_key, const uint8_t **out_data, size_t *out_size, int *out_is_key);

/* Captures one frame into dst_bgra via D3D11 staging readback (dxgi-staging mode). */
int extdisp_gpu_capture_bgra(struct extdisp_gpu *gpu, uint8_t *dst_bgra, uint32_t dst_stride, uint32_t dst_w, uint32_t dst_h);

/* Closes the GPU pipeline and releases all D3D11, DXGI, and Vulkan resources. */
void extdisp_gpu_close(struct extdisp_gpu *gpu);

#endif
