/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * D3D12 Video Encode zero-copy fallback: shares the D3D11 capture texture
 * into D3D12 via NT handle + shared fence, converts BGRA/R10G10B10A2 to NV12
 * using ID3D12VideoProcessor, encodes H.264 with ID3D12VideoEncoder, and
 * prepends Annex-B SPS/PPS NAL units on IDR frames.
 */

#ifndef MIKUTERM_WIN32_EXTDISP_D3D12_H
#define MIKUTERM_WIN32_EXTDISP_D3D12_H

#include <dxgi1_2.h>
#include <d3d11_1.h>
#include <stddef.h>
#include <stdint.h>

struct extdisp_d3d12;

/* Opens the D3D12 Video Encode pipeline on the same adapter as d3d11_dev. Returns 0 on success. */
int extdisp_d3d12_open(IDXGIAdapter *adapter, ID3D11Device *d3d11_dev, ID3D11DeviceContext *d3d11_ctx, uint32_t width, uint32_t height, DXGI_FORMAT src_format, struct extdisp_d3d12 **out);

/* Copies desktop_tex to the shared texture, composites optional cursor, converts to NV12, and encodes H.264 Annex-B. */
int extdisp_d3d12_encode_texture(struct extdisp_d3d12 *d12, ID3D11Texture2D *desktop_tex, int cursor_visible, int cursor_x, int cursor_y, uint32_t cursor_type, uint32_t cursor_w, uint32_t cursor_h, uint32_t cursor_pitch, const uint8_t *cursor_shape, int force_key, const uint8_t **out_data, size_t *out_size, int *out_is_key);

/* Releases all D3D12 resources. */
void extdisp_d3d12_close(struct extdisp_d3d12 *d12);

#endif
