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

#include "apps/mikuterm-common/mikuterm.h"

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <dxgi1_2.h>
#include <d3d11_1.h>
#include <initguid.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3d12video.h>

#include "extdisp-d3d12.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXTDISP_D12_DPB_SLOTS		2U
#define EXTDISP_D12_BITSTREAM_BYTES	(4U * 1024U * 1024U)
#define EXTDISP_D12_DEFAULT_QP		26U
#define EXTDISP_D12_KEY_QP_DELTA	3U
#define EXTDISP_D12_KEY_INTERVAL	120U
#define EXTDISP_D12_PS_MAX		256U
#define EXTDISP_D12_CURSOR_MAX		256U

typedef HRESULT (WINAPI *pfn_d3d12_create_device)(
	IUnknown *pAdapter,
	D3D_FEATURE_LEVEL MinimumFeatureLevel,
	REFIID riid,
	void **ppDevice);

/*
 * Tiny H.264 Annex-B RBSP bitwriter for generating SPS and PPS headers.
 */
struct extdisp_rbsp {
	uint8_t buf[EXTDISP_D12_PS_MAX];
	size_t bytes;
	uint32_t cur;
	int bits;
};

/*
 * Active D3D12 Video Processor and Video Encoder pipeline bound to a D3D11
 * capture device on the same adapter LUID.
 */
struct extdisp_d3d12 {
	HMODULE d3d12_dll;
	uint32_t width;
	uint32_t height;
	uint32_t aligned_w;
	uint32_t aligned_h;
	DXGI_FORMAT src_format;

	ID3D11Device *d3d11_dev;
	ID3D11DeviceContext *d3d11_ctx;
	ID3D11Device5 *d3d11_dev5;
	ID3D11DeviceContext4 *d3d11_ctx4;
	ID3D11Texture2D *d3d11_shared_tex;
	ID3D11Texture2D *d3d11_cursor_tex;
	ID3D11Fence *d3d11_fence;
	HANDLE tex_handle;
	HANDLE fence_handle;

	ID3D12Device *dev;
	ID3D12VideoDevice3 *vdev;
	ID3D12Fence *shared_fence;
	ID3D12Fence *done_fence;
	HANDLE fence_event;
	uint64_t fence_val;

	ID3D12CommandQueue *vp_queue;
	ID3D12CommandAllocator *vp_alloc;
	ID3D12VideoProcessCommandList *vp_list;
	ID3D12VideoProcessor *vp;

	ID3D12CommandQueue *enc_queue;
	ID3D12CommandAllocator *enc_alloc;
	ID3D12VideoEncodeCommandList2 *enc_list;
	ID3D12CommandQueue *copy_queue;
	ID3D12CommandAllocator *copy_alloc;
	ID3D12GraphicsCommandList *copy_list;
	ID3D12VideoEncoder *encoder;
	ID3D12VideoEncoderHeap *heap;

	ID3D12Resource *d3d12_src_tex;
	ID3D12Resource *nv12_tex;
	ID3D12Resource *dpb_tex[EXTDISP_D12_DPB_SLOTS];
	ID3D12Resource *bitstream_buf;
	ID3D12Resource *meta_buf;
	ID3D12Resource *resolved_meta_buf;
	ID3D12Resource *readback_buf;

	uint8_t ps_buf[EXTDISP_D12_PS_MAX];
	size_t ps_len;
	uint8_t *pkt_buf;
	size_t pkt_cap;

	uint32_t cur_dpb;
	uint32_t prev_dpb;
	uint32_t frame_num;
	uint32_t idr_pic_id;
	uint32_t frames_since_idr;
	uint32_t level_idc;
	int first_frame_logged;
};

static void extdisp_rbsp_put_bits(struct extdisp_rbsp *r, uint32_t val, int n);
static void extdisp_rbsp_put_ue(struct extdisp_rbsp *r, uint32_t val);
static void extdisp_rbsp_finish_nal(struct extdisp_rbsp *r, uint8_t nal_header, uint8_t *out, size_t *out_len, size_t cap);
static void extdisp_d3d12_build_sps_pps(struct extdisp_d3d12 *d12);
static uint32_t extdisp_d3d12_level_to_idc(D3D12_VIDEO_ENCODER_LEVELS_H264 lvl);
static int extdisp_d3d12_create_committed(ID3D12Device *dev, const char *name, D3D12_HEAP_TYPE heap, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state, ID3D12Resource **out);
static int extdisp_d3d12_wait_fence(struct extdisp_d3d12 *d12, uint64_t val);

/*
 * Opens the D3D12 Video Processor + H.264 Video Encoder zero-copy pipeline
 * on the same DXGI adapter as d3d11_dev.  Returns 0 on success.
 */
int
extdisp_d3d12_open(
	IDXGIAdapter *adapter,
	ID3D11Device *d3d11_dev,
	ID3D11DeviceContext *d3d11_ctx,
	uint32_t width,
	uint32_t height,
	DXGI_FORMAT src_format,
	struct extdisp_d3d12 **out)
{
	struct extdisp_d3d12 *d12;
	pfn_d3d12_create_device create_dev;
	D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT sup;
	D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264 h264_cfg;
	D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_H264 h264_gop;
	D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP rc_cqp;
	D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC res_desc;
	D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_LIMITS res_limits;
	D3D12_VIDEO_ENCODER_PROFILE_H264 prof_h264 = D3D12_VIDEO_ENCODER_PROFILE_H264_MAIN;
	D3D12_VIDEO_ENCODER_LEVELS_H264 lvl_h264 = D3D12_VIDEO_ENCODER_LEVELS_H264_41;
	D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOURCE_REQUIREMENTS rreq;
	D3D11_TEXTURE2D_DESC t11;
	IDXGIResource1 *res1 = NULL;
	D3D12_COMMAND_QUEUE_DESC qdesc;
	D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC vp_in;
	D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC vp_out;
	D3D12_VIDEO_ENCODER_DESC edesc;
	D3D12_VIDEO_ENCODER_HEAP_DESC hdesc;
	D3D12_RESOURCE_DESC rdesc;
	UINT64 meta_size;
	uint32_t i;
	HRESULT hr;

	if (adapter == NULL || d3d11_dev == NULL || d3d11_ctx == NULL || out == NULL)
		return -1;
	*out = NULL;

	d12 = (struct extdisp_d3d12 *)calloc(1U, sizeof(*d12));
	if (d12 == NULL)
		return -1;

	d12->width = width & ~1U;
	d12->height = height & ~1U;
	d12->aligned_w = (d12->width + 15U) & ~15U;
	d12->aligned_h = (d12->height + 15U) & ~15U;
	d12->src_format = src_format;
	d12->d3d11_dev = d3d11_dev;
	d12->d3d11_ctx = d3d11_ctx;
	d12->pkt_cap = EXTDISP_D12_BITSTREAM_BYTES;
	d12->pkt_buf = (uint8_t *)malloc(d12->pkt_cap);
	if (d12->pkt_buf == NULL) {
		extdisp_d3d12_close(d12);
		return -1;
	}

	d12->d3d12_dll = LoadLibraryExW(L"d3d12.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (d12->d3d12_dll == NULL) {
		mikuterm_log("extdisp-d3d12: d3d12.dll not available err=%lu", (unsigned long)GetLastError());
		extdisp_d3d12_close(d12);
		return -1;
	}
	create_dev = (pfn_d3d12_create_device)(void *)GetProcAddress(d12->d3d12_dll, "D3D12CreateDevice");
	if (create_dev == NULL) {
		mikuterm_log("extdisp-d3d12: D3D12CreateDevice symbol missing");
		extdisp_d3d12_close(d12);
		return -1;
	}

	hr = create_dev((IUnknown *)adapter, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device, (void **)&d12->dev);
	if (FAILED(hr) || d12->dev == NULL) {
		mikuterm_log("extdisp-d3d12: D3D12CreateDevice failed hr=0x%08lx", (unsigned long)hr);
		extdisp_d3d12_close(d12);
		return -1;
	}

	hr = d12->dev->lpVtbl->QueryInterface(d12->dev, &IID_ID3D12VideoDevice3, (void **)&d12->vdev);
	if (FAILED(hr) || d12->vdev == NULL) {
		mikuterm_log("extdisp-d3d12: ID3D12VideoDevice3 unavailable hr=0x%08lx", (unsigned long)hr);
		extdisp_d3d12_close(d12);
		return -1;
	}

	memset(&h264_cfg, 0, sizeof(h264_cfg));
	h264_cfg.ConfigurationFlags = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_NONE;
	h264_cfg.DirectModeConfig = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_DIRECT_MODES_DISABLED;
	h264_cfg.DisableDeblockingFilterConfig = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_SLICES_DEBLOCKING_MODE_0_ALL_LUMA_CHROMA_SLICE_BLOCK_EDGES_ALWAYS_FILTERED;

	memset(&h264_gop, 0, sizeof(h264_gop));
	h264_gop.GOPLength = EXTDISP_D12_KEY_INTERVAL;
	h264_gop.PPicturePeriod = 1U;
	h264_gop.pic_order_cnt_type = 2U;
	h264_gop.log2_max_frame_num_minus4 = 0U;

	memset(&rc_cqp, 0, sizeof(rc_cqp));
	rc_cqp.ConstantQP_FullIntracodedFrame = EXTDISP_D12_DEFAULT_QP - EXTDISP_D12_KEY_QP_DELTA;
	rc_cqp.ConstantQP_InterPredictedFrame_PrevRefOnly = EXTDISP_D12_DEFAULT_QP;
	rc_cqp.ConstantQP_InterPredictedFrame_BiDirectionalRef = EXTDISP_D12_DEFAULT_QP;

	memset(&res_desc, 0, sizeof(res_desc));
	res_desc.Width = d12->width;
	res_desc.Height = d12->height;

	memset(&res_limits, 0, sizeof(res_limits));
	memset(&sup, 0, sizeof(sup));
	sup.Codec = D3D12_VIDEO_ENCODER_CODEC_H264;
	sup.InputFormat = DXGI_FORMAT_NV12;
	sup.CodecConfiguration.DataSize = sizeof(h264_cfg);
	sup.CodecConfiguration.pH264Config = &h264_cfg;
	sup.CodecGopSequence.DataSize = sizeof(h264_gop);
	sup.CodecGopSequence.pH264GroupOfPictures = &h264_gop;
	sup.RateControl.Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP;
	sup.RateControl.ConfigParams.DataSize = sizeof(rc_cqp);
	sup.RateControl.ConfigParams.pConfiguration_CQP = &rc_cqp;
	sup.RateControl.TargetFrameRate.Numerator = 60U;
	sup.RateControl.TargetFrameRate.Denominator = 1U;
	sup.IntraRefresh = D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE;
	sup.SubregionFrameEncoding = D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME;
	sup.ResolutionsListCount = 1U;
	sup.pResolutionList = &res_desc;
	sup.MaxReferenceFramesInDPB = 1U;
	sup.SuggestedProfile.DataSize = sizeof(prof_h264);
	sup.SuggestedProfile.pH264Profile = &prof_h264;
	sup.SuggestedLevel.DataSize = sizeof(lvl_h264);
	sup.SuggestedLevel.pH264LevelSetting = &lvl_h264;
	sup.pResolutionDependentSupport = &res_limits;

	hr = ID3D12VideoDevice3_CheckFeatureSupport(d12->vdev, D3D12_FEATURE_VIDEO_ENCODER_SUPPORT,
	    &sup, sizeof(sup));
	if (FAILED(hr) || (sup.SupportFlags & D3D12_VIDEO_ENCODER_SUPPORT_FLAG_GENERAL_SUPPORT_OK) == 0U) {
		mikuterm_log("extdisp-d3d12: CheckFeatureSupport failed hr=0x%08lx flags=0x%x val=0x%x",
		    (unsigned long)hr, (unsigned)sup.SupportFlags, (unsigned)sup.ValidationFlags);
		extdisp_d3d12_close(d12);
		return -1;
	}
	d12->level_idc = extdisp_d3d12_level_to_idc(lvl_h264);
	mikuterm_log("extdisp-d3d12: H.264 encode supported (%ux%u level_idc=%u max_dpb=%u)",
	    d12->width, d12->height, d12->level_idc, (unsigned)sup.MaxReferenceFramesInDPB);

	memset(&rreq, 0, sizeof(rreq));
	rreq.Codec = D3D12_VIDEO_ENCODER_CODEC_H264;
	rreq.Profile.DataSize = sizeof(prof_h264);
	rreq.Profile.pH264Profile = &prof_h264;
	rreq.InputFormat = DXGI_FORMAT_NV12;
	rreq.PictureTargetResolution = res_desc;
	hr = ID3D12VideoDevice3_CheckFeatureSupport(d12->vdev,
	    D3D12_FEATURE_VIDEO_ENCODER_RESOURCE_REQUIREMENTS, &rreq, sizeof(rreq));
	if (FAILED(hr) || !rreq.IsSupported) {
		mikuterm_log("extdisp-d3d12: resource requirements failed hr=0x%08lx", (unsigned long)hr);
		extdisp_d3d12_close(d12);
		return -1;
	}

	hr = d3d11_dev->lpVtbl->QueryInterface(d3d11_dev, &IID_ID3D11Device5, (void **)&d12->d3d11_dev5);
	if (FAILED(hr) || d12->d3d11_dev5 == NULL) {
		mikuterm_log("extdisp-d3d12: ID3D11Device5 unavailable hr=0x%08lx", (unsigned long)hr);
		extdisp_d3d12_close(d12);
		return -1;
	}
	hr = d3d11_ctx->lpVtbl->QueryInterface(d3d11_ctx, &IID_ID3D11DeviceContext4, (void **)&d12->d3d11_ctx4);
	if (FAILED(hr) || d12->d3d11_ctx4 == NULL) {
		mikuterm_log("extdisp-d3d12: ID3D11DeviceContext4 unavailable hr=0x%08lx", (unsigned long)hr);
		extdisp_d3d12_close(d12);
		return -1;
	}

	memset(&t11, 0, sizeof(t11));
	t11.Width = d12->width;
	t11.Height = d12->height;
	t11.MipLevels = 1U;
	t11.ArraySize = 1U;
	t11.Format = src_format;
	t11.SampleDesc.Count = 1U;
	t11.Usage = D3D11_USAGE_DEFAULT;
	t11.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	t11.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
	hr = d3d11_dev->lpVtbl->CreateTexture2D(d3d11_dev, &t11, NULL, &d12->d3d11_shared_tex);
	if (FAILED(hr) || d12->d3d11_shared_tex == NULL) {
		mikuterm_log("extdisp-d3d12: D3D11 CreateTexture2D(shared) failed hr=0x%08lx", (unsigned long)hr);
		extdisp_d3d12_close(d12);
		return -1;
	}
	hr = d12->d3d11_shared_tex->lpVtbl->QueryInterface(d12->d3d11_shared_tex,
	    &IID_IDXGIResource1, (void **)&res1);
	if (FAILED(hr) || res1 == NULL) {
		extdisp_d3d12_close(d12);
		return -1;
	}
	hr = res1->lpVtbl->CreateSharedHandle(res1, NULL,
	    DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, &d12->tex_handle);
	res1->lpVtbl->Release(res1);
	if (FAILED(hr) || d12->tex_handle == NULL) {
		mikuterm_log("extdisp-d3d12: CreateSharedHandle(tex) failed hr=0x%08lx", (unsigned long)hr);
		extdisp_d3d12_close(d12);
		return -1;
	}

	hr = ID3D12Device_OpenSharedHandle(d12->dev, d12->tex_handle,
	    &IID_ID3D12Resource, (void **)&d12->d3d12_src_tex);
	if (FAILED(hr) || d12->d3d12_src_tex == NULL) {
		mikuterm_log("extdisp-d3d12: OpenSharedHandle(tex) failed hr=0x%08lx", (unsigned long)hr);
		extdisp_d3d12_close(d12);
		return -1;
	}

	hr = ID3D12Device_CreateFence(d12->dev, 0ULL, D3D12_FENCE_FLAG_SHARED,
	    &IID_ID3D12Fence, (void **)&d12->shared_fence);
	if (FAILED(hr) || d12->shared_fence == NULL) {
		mikuterm_log("extdisp-d3d12: CreateFence(shared) failed hr=0x%08lx", (unsigned long)hr);
		extdisp_d3d12_close(d12);
		return -1;
	}
	hr = ID3D12Device_CreateSharedHandle(d12->dev, (ID3D12DeviceChild *)d12->shared_fence,
	    NULL, GENERIC_ALL, NULL, &d12->fence_handle);
	if (FAILED(hr) || d12->fence_handle == NULL) {
		extdisp_d3d12_close(d12);
		return -1;
	}
	hr = d12->d3d11_dev5->lpVtbl->OpenSharedFence(d12->d3d11_dev5, d12->fence_handle,
	    &IID_ID3D11Fence, (void **)&d12->d3d11_fence);
	if (FAILED(hr) || d12->d3d11_fence == NULL) {
		mikuterm_log("extdisp-d3d12: OpenSharedFence failed hr=0x%08lx", (unsigned long)hr);
		extdisp_d3d12_close(d12);
		return -1;
	}

	hr = ID3D12Device_CreateFence(d12->dev, 0ULL, D3D12_FENCE_FLAG_NONE,
	    &IID_ID3D12Fence, (void **)&d12->done_fence);
	if (FAILED(hr) || d12->done_fence == NULL) {
		extdisp_d3d12_close(d12);
		return -1;
	}
	d12->fence_event = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (d12->fence_event == NULL) {
		extdisp_d3d12_close(d12);
		return -1;
	}

	memset(&qdesc, 0, sizeof(qdesc));
	qdesc.Type = D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS;
	if (FAILED(ID3D12Device_CreateCommandQueue(d12->dev, &qdesc, &IID_ID3D12CommandQueue, (void **)&d12->vp_queue)) ||
	    FAILED(ID3D12Device_CreateCommandAllocator(d12->dev, D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS,
	    &IID_ID3D12CommandAllocator, (void **)&d12->vp_alloc)) ||
	    FAILED(ID3D12Device_CreateCommandList(d12->dev, 0U, D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS,
	    d12->vp_alloc, NULL, &IID_ID3D12VideoProcessCommandList, (void **)&d12->vp_list))) {
		mikuterm_log("extdisp-d3d12: failed to create video process command list");
		extdisp_d3d12_close(d12);
		return -1;
	}
	(void)ID3D12VideoProcessCommandList_Close(d12->vp_list);

	memset(&vp_in, 0, sizeof(vp_in));
	vp_in.Format = src_format;
	vp_in.ColorSpace = (src_format == DXGI_FORMAT_R10G10B10A2_UNORM) ?
	    DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
	vp_in.FrameRate.Numerator = 60U;
	vp_in.FrameRate.Denominator = 1U;
	vp_in.SourceSizeRange.MaxWidth = d12->width;
	vp_in.SourceSizeRange.MaxHeight = d12->height;
	vp_in.SourceSizeRange.MinWidth = d12->width;
	vp_in.SourceSizeRange.MinHeight = d12->height;
	vp_in.DestinationSizeRange = vp_in.SourceSizeRange;

	memset(&vp_out, 0, sizeof(vp_out));
	vp_out.Format = DXGI_FORMAT_NV12;
	vp_out.ColorSpace = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
	vp_out.FrameRate.Numerator = 60U;
	vp_out.FrameRate.Denominator = 1U;

	hr = ID3D12VideoDevice3_CreateVideoProcessor(d12->vdev, 0U, &vp_out, 1U, &vp_in,
	    &IID_ID3D12VideoProcessor, (void **)&d12->vp);
	if (FAILED(hr) || d12->vp == NULL) {
		mikuterm_log("extdisp-d3d12: CreateVideoProcessor failed hr=0x%08lx", (unsigned long)hr);
		extdisp_d3d12_close(d12);
		return -1;
	}

	qdesc.Type = D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE;
	if (FAILED(ID3D12Device_CreateCommandQueue(d12->dev, &qdesc, &IID_ID3D12CommandQueue, (void **)&d12->enc_queue)) ||
	    FAILED(ID3D12Device_CreateCommandAllocator(d12->dev, D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE,
	    &IID_ID3D12CommandAllocator, (void **)&d12->enc_alloc)) ||
	    FAILED(ID3D12Device_CreateCommandList(d12->dev, 0U, D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE,
	    d12->enc_alloc, NULL, &IID_ID3D12VideoEncodeCommandList2, (void **)&d12->enc_list))) {
		mikuterm_log("extdisp-d3d12: failed to create video encode command list");
		extdisp_d3d12_close(d12);
		return -1;
	}
	(void)ID3D12VideoEncodeCommandList2_Close(d12->enc_list);

	qdesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
	if (FAILED(ID3D12Device_CreateCommandQueue(d12->dev, &qdesc, &IID_ID3D12CommandQueue, (void **)&d12->copy_queue)) ||
	    FAILED(ID3D12Device_CreateCommandAllocator(d12->dev, D3D12_COMMAND_LIST_TYPE_COPY,
	    &IID_ID3D12CommandAllocator, (void **)&d12->copy_alloc)) ||
	    FAILED(ID3D12Device_CreateCommandList(d12->dev, 0U, D3D12_COMMAND_LIST_TYPE_COPY,
	    d12->copy_alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&d12->copy_list))) {
		mikuterm_log("extdisp-d3d12: failed to create copy command list");
		extdisp_d3d12_close(d12);
		return -1;
	}
	(void)ID3D12GraphicsCommandList_Close(d12->copy_list);

	memset(&edesc, 0, sizeof(edesc));
	edesc.NodeMask = 0U;
	edesc.Flags = D3D12_VIDEO_ENCODER_FLAG_NONE;
	edesc.EncodeCodec = D3D12_VIDEO_ENCODER_CODEC_H264;
	edesc.EncodeProfile.DataSize = sizeof(prof_h264);
	edesc.EncodeProfile.pH264Profile = &prof_h264;
	edesc.InputFormat = DXGI_FORMAT_NV12;
	edesc.CodecConfiguration.DataSize = sizeof(h264_cfg);
	edesc.CodecConfiguration.pH264Config = &h264_cfg;
	edesc.MaxMotionEstimationPrecision = D3D12_VIDEO_ENCODER_MOTION_ESTIMATION_PRECISION_MODE_MAXIMUM;
	hr = ID3D12VideoDevice3_CreateVideoEncoder(d12->vdev, &edesc,
	    &IID_ID3D12VideoEncoder, (void **)&d12->encoder);
	if (FAILED(hr) || d12->encoder == NULL) {
		mikuterm_log("extdisp-d3d12: CreateVideoEncoder failed hr=0x%08lx", (unsigned long)hr);
		extdisp_d3d12_close(d12);
		return -1;
	}

	memset(&hdesc, 0, sizeof(hdesc));
	hdesc.NodeMask = 0U;
	hdesc.EncodeCodec = D3D12_VIDEO_ENCODER_CODEC_H264;
	hdesc.EncodeProfile.DataSize = sizeof(prof_h264);
	hdesc.EncodeProfile.pH264Profile = &prof_h264;
	hdesc.EncodeLevel.DataSize = sizeof(lvl_h264);
	hdesc.EncodeLevel.pH264LevelSetting = &lvl_h264;
	hdesc.ResolutionsListCount = 1U;
	hdesc.pResolutionList = &res_desc;
	hr = ID3D12VideoDevice3_CreateVideoEncoderHeap(d12->vdev, &hdesc,
	    &IID_ID3D12VideoEncoderHeap, (void **)&d12->heap);
	if (FAILED(hr) || d12->heap == NULL) {
		mikuterm_log("extdisp-d3d12: CreateVideoEncoderHeap failed hr=0x%08lx", (unsigned long)hr);
		extdisp_d3d12_close(d12);
		return -1;
	}

	memset(&rdesc, 0, sizeof(rdesc));
	rdesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	rdesc.Width = d12->width;
	rdesc.Height = d12->height;
	rdesc.DepthOrArraySize = 1U;
	rdesc.MipLevels = 1U;
	rdesc.Format = DXGI_FORMAT_NV12;
	rdesc.SampleDesc.Count = 1U;
	rdesc.Flags = D3D12_RESOURCE_FLAG_NONE;
	if (extdisp_d3d12_create_committed(d12->dev, "nv12_tex", D3D12_HEAP_TYPE_DEFAULT, &rdesc,
	    D3D12_RESOURCE_STATE_COMMON, &d12->nv12_tex) != 0) {
		extdisp_d3d12_close(d12);
		return -1;
	}

	rdesc.Flags = D3D12_RESOURCE_FLAG_VIDEO_ENCODE_REFERENCE_ONLY | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
	for (i = 0U; i < EXTDISP_D12_DPB_SLOTS; i++) {
		if (extdisp_d3d12_create_committed(d12->dev, "dpb_tex", D3D12_HEAP_TYPE_DEFAULT, &rdesc,
		    D3D12_RESOURCE_STATE_COMMON, &d12->dpb_tex[i]) != 0) {
			extdisp_d3d12_close(d12);
			return -1;
		}
	}

	memset(&rdesc, 0, sizeof(rdesc));
	rdesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	rdesc.Width = EXTDISP_D12_BITSTREAM_BYTES;
	rdesc.Height = 1U;
	rdesc.DepthOrArraySize = 1U;
	rdesc.MipLevels = 1U;
	rdesc.Format = DXGI_FORMAT_UNKNOWN;
	rdesc.SampleDesc.Count = 1U;
	rdesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (extdisp_d3d12_create_committed(d12->dev, "bitstream_buf", D3D12_HEAP_TYPE_DEFAULT, &rdesc,
	    D3D12_RESOURCE_STATE_COMMON, &d12->bitstream_buf) != 0 ||
	    extdisp_d3d12_create_committed(d12->dev, "readback_buf", D3D12_HEAP_TYPE_READBACK, &rdesc,
	    D3D12_RESOURCE_STATE_COPY_DEST, &d12->readback_buf) != 0) {
		extdisp_d3d12_close(d12);
		return -1;
	}

	meta_size = (rreq.MaxEncoderOutputMetadataBufferSize + 255ULL) & ~255ULL;
	if (meta_size < 4096ULL)
		meta_size = 4096ULL;
	rdesc.Width = meta_size;
	if (extdisp_d3d12_create_committed(d12->dev, "meta_buf", D3D12_HEAP_TYPE_DEFAULT, &rdesc,
	    D3D12_RESOURCE_STATE_COMMON, &d12->meta_buf) != 0 ||
	    extdisp_d3d12_create_committed(d12->dev, "resolved_meta_buf", D3D12_HEAP_TYPE_READBACK, &rdesc,
	    D3D12_RESOURCE_STATE_COPY_DEST, &d12->resolved_meta_buf) != 0) {
		extdisp_d3d12_close(d12);
		return -1;
	}

	extdisp_d3d12_build_sps_pps(d12);
	{
		D3D11_TEXTURE2D_DESC cdesc;
		memset(&cdesc, 0, sizeof(cdesc));
		cdesc.Width = EXTDISP_D12_CURSOR_MAX;
		cdesc.Height = EXTDISP_D12_CURSOR_MAX;
		cdesc.MipLevels = 1U;
		cdesc.ArraySize = 1U;
		cdesc.Format = src_format;
		cdesc.SampleDesc.Count = 1U;
		cdesc.Usage = D3D11_USAGE_STAGING;
		cdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
		(void)d3d11_dev->lpVtbl->CreateTexture2D(d3d11_dev, &cdesc, NULL, &d12->d3d11_cursor_tex);
	}
	*out = d12;
	return 0;
}

static void
extdisp_d3d12_blend_cursor(
	uint8_t *patch,
	uint32_t patch_pitch,
	uint32_t pw,
	uint32_t ph,
	int off_x,
	int off_y,
	uint32_t cursor_type,
	uint32_t cursor_w,
	uint32_t cursor_h,
	uint32_t cursor_pitch,
	const uint8_t *shape)
{
	uint32_t y;
	uint32_t x;
	uint32_t vis_h = (cursor_type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) ? (cursor_h / 2U) : cursor_h;

	for (y = 0U; y < ph; y++) {
		uint32_t sy = (uint32_t)((int)y + off_y);
		uint8_t *dst_row = patch + (size_t)y * patch_pitch;
		if (sy >= vis_h)
			continue;
		for (x = 0U; x < pw; x++) {
			uint32_t sx = (uint32_t)((int)x + off_x);
			uint8_t *px = dst_row + (size_t)x * 4U;
			if (sx >= cursor_w)
				continue;
			if (cursor_type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
				uint8_t bit = (uint8_t)(0x80U >> (sx & 7U));
				uint8_t and_b = (shape[(size_t)sy * cursor_pitch + (sx >> 3U)] & bit) ? 0xFFU : 0x00U;
				uint8_t xor_b = (shape[(size_t)(sy + vis_h) * cursor_pitch + (sx >> 3U)] & bit) ? 0xFFU : 0x00U;
				px[0] = (px[0] & and_b) ^ xor_b;
				px[1] = (px[1] & and_b) ^ xor_b;
				px[2] = (px[2] & and_b) ^ xor_b;
			} else if (cursor_type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
				const uint8_t *src = shape + (size_t)sy * cursor_pitch + (size_t)sx * 4U;
				uint32_t a = src[3];
				if (a == 255U) {
					px[0] = src[0];
					px[1] = src[1];
					px[2] = src[2];
				} else if (a > 0U) {
					uint32_t inv = 255U - a;
					px[0] = (uint8_t)(src[0] + ((px[0] * inv) >> 8));
					px[1] = (uint8_t)(src[1] + ((px[1] * inv) >> 8));
					px[2] = (uint8_t)(src[2] + ((px[2] * inv) >> 8));
				}
			} else if (cursor_type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
				const uint8_t *src = shape + (size_t)sy * cursor_pitch + (size_t)sx * 4U;
				if (src[3] != 0U) {
					px[0] ^= src[0];
					px[1] ^= src[1];
					px[2] ^= src[2];
				} else {
					px[0] = src[0];
					px[1] = src[1];
					px[2] = src[2];
				}
			}
		}
	}
}

/*
 * Copies desktop_tex to the shared D3D11 texture, composites optional cursor,
 * converts BGRA/R10G10B10A2 to NV12 via ID3D12VideoProcessor, encodes H.264,
 * and prepends SPS/PPS on IDR.
 */
int
extdisp_d3d12_encode_texture(
	struct extdisp_d3d12 *d12,
	ID3D11Texture2D *desktop_tex,
	int cursor_visible,
	int cursor_x,
	int cursor_y,
	uint32_t cursor_type,
	uint32_t cursor_w,
	uint32_t cursor_h,
	uint32_t cursor_pitch,
	const uint8_t *cursor_shape,
	int force_key,
	const uint8_t **out_data,
	size_t *out_size,
	int *out_is_key)
{
	uint64_t sync_val;
	uint64_t done_val;
	D3D12_RESOURCE_BARRIER bar;
	D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS vp_in_args;
	D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS vp_out_args;
	D3D12_VIDEO_ENCODER_ENCODEFRAME_INPUT_ARGUMENTS enc_in;
	D3D12_VIDEO_ENCODER_ENCODEFRAME_OUTPUT_ARGUMENTS enc_out;
	D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_H264 h264_pic;
	D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_H264 gop_h264;
	D3D12_VIDEO_ENCODER_PROFILE_H264 prof_h264 = D3D12_VIDEO_ENCODER_PROFILE_H264_MAIN;
	D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP rc_cqp;
	D3D12_VIDEO_ENCODER_REFERENCE_PICTURE_DESCRIPTOR_H264 ref_desc;
	D3D12_VIDEO_ENCODER_RESOLVE_METADATA_INPUT_ARGUMENTS res_in;
	D3D12_VIDEO_ENCODER_RESOLVE_METADATA_OUTPUT_ARGUMENTS res_out;
	D3D12_VIDEO_ENCODER_OUTPUT_METADATA *meta = NULL;
	void *mapped_bs = NULL;
	UINT l0_idx;
	uint64_t bs_bytes;
	int is_idr;
	HRESULT hr;

	if (d12 == NULL || desktop_tex == NULL || out_data == NULL || out_size == NULL)
		return -1;
	*out_data = NULL;
	*out_size = 0U;
	if (out_is_key != NULL)
		*out_is_key = 0;

	if (!d12->first_frame_logged)
		mikuterm_log("extdisp-d3d12: frame0 step 1 CopyResource");
	d12->d3d11_ctx->lpVtbl->CopyResource(d12->d3d11_ctx,
	    (ID3D11Resource *)d12->d3d11_shared_tex, (ID3D11Resource *)desktop_tex);
	if (cursor_visible && cursor_shape != NULL && d12->d3d11_cursor_tex != NULL &&
	    d12->src_format == DXGI_FORMAT_B8G8R8A8_UNORM && cursor_w > 0U && cursor_h > 0U) {
		uint32_t vis_h = (cursor_type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) ? (cursor_h / 2U) : cursor_h;
		int x0 = cursor_x;
		int y0 = cursor_y;
		int x1 = cursor_x + (int)cursor_w;
		int y1 = cursor_y + (int)vis_h;
		if (x0 < 0) x0 = 0;
		if (y0 < 0) y0 = 0;
		if (x1 > (int)d12->width) x1 = (int)d12->width;
		if (y1 > (int)d12->height) y1 = (int)d12->height;
		if (x1 > x0 && y1 > y0) {
			uint32_t pw = (uint32_t)(x1 - x0);
			uint32_t ph = (uint32_t)(y1 - y0);
			if (pw > EXTDISP_D12_CURSOR_MAX) pw = EXTDISP_D12_CURSOR_MAX;
			if (ph > EXTDISP_D12_CURSOR_MAX) ph = EXTDISP_D12_CURSOR_MAX;
			{
				D3D11_BOX sbox;
				D3D11_MAPPED_SUBRESOURCE cmap;
				sbox.left = (UINT)x0;
				sbox.top = (UINT)y0;
				sbox.front = 0U;
				sbox.right = (UINT)x0 + pw;
				sbox.bottom = (UINT)y0 + ph;
				sbox.back = 1U;
				d12->d3d11_ctx->lpVtbl->CopySubresourceRegion(d12->d3d11_ctx,
				    (ID3D11Resource *)d12->d3d11_cursor_tex, 0U, 0U, 0U, 0U,
				    (ID3D11Resource *)d12->d3d11_shared_tex, 0U, &sbox);
				if (SUCCEEDED(d12->d3d11_ctx->lpVtbl->Map(d12->d3d11_ctx,
				    (ID3D11Resource *)d12->d3d11_cursor_tex, 0U, D3D11_MAP_READ_WRITE, 0U, &cmap))) {
					extdisp_d3d12_blend_cursor((uint8_t *)cmap.pData, cmap.RowPitch,
					    pw, ph, x0 - cursor_x, y0 - cursor_y,
					    cursor_type, cursor_w, cursor_h, cursor_pitch, cursor_shape);
					d12->d3d11_ctx->lpVtbl->Unmap(d12->d3d11_ctx,
					    (ID3D11Resource *)d12->d3d11_cursor_tex, 0U);
					sbox.left = 0U;
					sbox.top = 0U;
					sbox.right = pw;
					sbox.bottom = ph;
					d12->d3d11_ctx->lpVtbl->CopySubresourceRegion(d12->d3d11_ctx,
					    (ID3D11Resource *)d12->d3d11_shared_tex, 0U, (UINT)x0, (UINT)y0, 0U,
					    (ID3D11Resource *)d12->d3d11_cursor_tex, 0U, &sbox);
				}
			}
		}
	}
	if (!d12->first_frame_logged)
		mikuterm_log("extdisp-d3d12: frame0 step 2 Signal");
	sync_val = ++d12->fence_val;
	hr = d12->d3d11_ctx4->lpVtbl->Signal(d12->d3d11_ctx4, d12->d3d11_fence, sync_val);
	if (FAILED(hr))
		return -1;

	(void)ID3D12CommandQueue_Wait(d12->vp_queue, d12->shared_fence, sync_val);

	if (FAILED(ID3D12CommandAllocator_Reset(d12->vp_alloc)) ||
	    FAILED(ID3D12VideoProcessCommandList_Reset(d12->vp_list, d12->vp_alloc)))
		return -1;

	{
		D3D12_RESOURCE_BARRIER vp_bars[2];
		memset(vp_bars, 0, sizeof(vp_bars));
		vp_bars[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		vp_bars[0].Transition.pResource = d12->d3d12_src_tex;
		vp_bars[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		vp_bars[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		vp_bars[0].Transition.StateAfter = D3D12_RESOURCE_STATE_VIDEO_PROCESS_READ;
		vp_bars[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		vp_bars[1].Transition.pResource = d12->nv12_tex;
		vp_bars[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		vp_bars[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		vp_bars[1].Transition.StateAfter = D3D12_RESOURCE_STATE_VIDEO_PROCESS_WRITE;
		ID3D12VideoProcessCommandList_ResourceBarrier(d12->vp_list, 2U, vp_bars);
	}

	if (!d12->first_frame_logged)
		mikuterm_log("extdisp-d3d12: frame0 step 3 ProcessFrames");
	memset(&vp_in_args, 0, sizeof(vp_in_args));
	vp_in_args.InputStream[0].pTexture2D = d12->d3d12_src_tex;
	vp_in_args.Transform.SourceRectangle.right = (LONG)d12->width;
	vp_in_args.Transform.SourceRectangle.bottom = (LONG)d12->height;
	vp_in_args.Transform.DestinationRectangle = vp_in_args.Transform.SourceRectangle;

	memset(&vp_out_args, 0, sizeof(vp_out_args));
	vp_out_args.OutputStream[0].pTexture2D = d12->nv12_tex;
	vp_out_args.TargetRectangle = vp_in_args.Transform.SourceRectangle;

	ID3D12VideoProcessCommandList_ProcessFrames(d12->vp_list, d12->vp, &vp_out_args, 1U, &vp_in_args);

	{
		D3D12_RESOURCE_BARRIER vp_bars[2];
		memset(vp_bars, 0, sizeof(vp_bars));
		vp_bars[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		vp_bars[0].Transition.pResource = d12->d3d12_src_tex;
		vp_bars[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		vp_bars[0].Transition.StateBefore = D3D12_RESOURCE_STATE_VIDEO_PROCESS_READ;
		vp_bars[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
		vp_bars[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		vp_bars[1].Transition.pResource = d12->nv12_tex;
		vp_bars[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		vp_bars[1].Transition.StateBefore = D3D12_RESOURCE_STATE_VIDEO_PROCESS_WRITE;
		vp_bars[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
		ID3D12VideoProcessCommandList_ResourceBarrier(d12->vp_list, 2U, vp_bars);
	}

	if (FAILED(ID3D12VideoProcessCommandList_Close(d12->vp_list)))
		return -1;

	{
		ID3D12CommandList *lists[1];
		lists[0] = (ID3D12CommandList *)d12->vp_list;
		ID3D12CommandQueue_ExecuteCommandLists(d12->vp_queue, 1U, lists);
	}
	done_val = ++d12->fence_val;
	(void)ID3D12CommandQueue_Signal(d12->vp_queue, d12->done_fence, done_val);
	(void)ID3D12CommandQueue_Wait(d12->enc_queue, d12->done_fence, done_val);

	is_idr = force_key || d12->frames_since_idr == 0U || d12->frames_since_idr >= EXTDISP_D12_KEY_INTERVAL;
	if (is_idr) {
		d12->frame_num = 0U;
		d12->idr_pic_id = (d12->idr_pic_id + 1U) & 0xFFFFU;
	}

	if (FAILED(ID3D12CommandAllocator_Reset(d12->enc_alloc)) ||
	    FAILED(ID3D12VideoEncodeCommandList2_Reset(d12->enc_list, d12->enc_alloc)))
		return -1;

	{
		D3D12_RESOURCE_BARRIER pre_bars[5];
		UINT nb = 0U;

		memset(pre_bars, 0, sizeof(pre_bars));
		pre_bars[nb].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		pre_bars[nb].Transition.pResource = d12->nv12_tex;
		pre_bars[nb].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		pre_bars[nb].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		pre_bars[nb].Transition.StateAfter = D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ;
		nb++;

		pre_bars[nb].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		pre_bars[nb].Transition.pResource = d12->bitstream_buf;
		pre_bars[nb].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		pre_bars[nb].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		pre_bars[nb].Transition.StateAfter = D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE;
		nb++;

		pre_bars[nb].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		pre_bars[nb].Transition.pResource = d12->meta_buf;
		pre_bars[nb].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		pre_bars[nb].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		pre_bars[nb].Transition.StateAfter = D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE;
		nb++;

		pre_bars[nb].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		pre_bars[nb].Transition.pResource = d12->dpb_tex[d12->cur_dpb];
		pre_bars[nb].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		pre_bars[nb].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		pre_bars[nb].Transition.StateAfter = D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE;
		nb++;

		if (!is_idr) {
			pre_bars[nb].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			pre_bars[nb].Transition.pResource = d12->dpb_tex[d12->prev_dpb];
			pre_bars[nb].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			pre_bars[nb].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
			pre_bars[nb].Transition.StateAfter = D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ;
			nb++;
		}
		ID3D12VideoEncodeCommandList2_ResourceBarrier(d12->enc_list, nb, pre_bars);
	}

	memset(&ref_desc, 0, sizeof(ref_desc));
	ref_desc.ReconstructedPictureResourceIndex = 0U;
	ref_desc.IsLongTermReference = FALSE;
	ref_desc.FrameDecodingOrderNumber = (d12->frame_num > 0U) ? (d12->frame_num - 1U) : 0U;
	ref_desc.PictureOrderCountNumber = ref_desc.FrameDecodingOrderNumber * 2U;
	l0_idx = 0U;

	memset(&h264_pic, 0, sizeof(h264_pic));
	h264_pic.Flags = D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_H264_FLAG_NONE;
	h264_pic.FrameType = is_idr ?
	    D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_IDR_FRAME :
	    D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_P_FRAME;
	h264_pic.idr_pic_id = d12->idr_pic_id;
	h264_pic.FrameDecodingOrderNumber = d12->frame_num;
	h264_pic.PictureOrderCountNumber = d12->frame_num * 2U;
	if (!is_idr) {
		h264_pic.List0ReferenceFramesCount = 1U;
		h264_pic.pList0ReferenceFrames = &l0_idx;
		h264_pic.ReferenceFramesReconPictureDescriptorsCount = 1U;
		h264_pic.pReferenceFramesReconPictureDescriptors = &ref_desc;
	}

	memset(&gop_h264, 0, sizeof(gop_h264));
	gop_h264.GOPLength = EXTDISP_D12_KEY_INTERVAL;
	gop_h264.PPicturePeriod = 1U;
	gop_h264.pic_order_cnt_type = 2U;
	gop_h264.log2_max_frame_num_minus4 = 0U;

	memset(&rc_cqp, 0, sizeof(rc_cqp));
	rc_cqp.ConstantQP_FullIntracodedFrame = EXTDISP_D12_DEFAULT_QP - EXTDISP_D12_KEY_QP_DELTA;
	rc_cqp.ConstantQP_InterPredictedFrame_PrevRefOnly = EXTDISP_D12_DEFAULT_QP;
	rc_cqp.ConstantQP_InterPredictedFrame_BiDirectionalRef = EXTDISP_D12_DEFAULT_QP;

	memset(&enc_in, 0, sizeof(enc_in));
	enc_in.pInputFrame = d12->nv12_tex;
	enc_in.CurrentFrameBitstreamMetadataSize = 0ULL;
	enc_in.SequenceControlDesc.Flags = D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_FLAG_NONE;
	enc_in.SequenceControlDesc.CodecGopSequence.DataSize = sizeof(gop_h264);
	enc_in.SequenceControlDesc.CodecGopSequence.pH264GroupOfPictures = &gop_h264;
	enc_in.SequenceControlDesc.RateControl.Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP;
	enc_in.SequenceControlDesc.RateControl.ConfigParams.DataSize = sizeof(rc_cqp);
	enc_in.SequenceControlDesc.RateControl.ConfigParams.pConfiguration_CQP = &rc_cqp;
	enc_in.SequenceControlDesc.RateControl.TargetFrameRate.Numerator = 60U;
	enc_in.SequenceControlDesc.RateControl.TargetFrameRate.Denominator = 1U;
	enc_in.SequenceControlDesc.PictureTargetResolution.Width = d12->width;
	enc_in.SequenceControlDesc.PictureTargetResolution.Height = d12->height;
	enc_in.PictureControlDesc.Flags = D3D12_VIDEO_ENCODER_PICTURE_CONTROL_FLAG_USED_AS_REFERENCE_PICTURE;
	enc_in.PictureControlDesc.PictureControlCodecData.DataSize = sizeof(h264_pic);
	enc_in.PictureControlDesc.PictureControlCodecData.pH264PicData = &h264_pic;
	if (!is_idr) {
		enc_in.PictureControlDesc.ReferenceFrames.NumTexture2Ds = 1U;
		enc_in.PictureControlDesc.ReferenceFrames.ppTexture2Ds = &d12->dpb_tex[d12->prev_dpb];
	}

	memset(&enc_out, 0, sizeof(enc_out));
	enc_out.Bitstream.pBuffer = d12->bitstream_buf;
	enc_out.ReconstructedPicture.pReconstructedPicture = d12->dpb_tex[d12->cur_dpb];
	enc_out.EncoderOutputMetadata.pBuffer = d12->meta_buf;

	if (!d12->first_frame_logged)
		mikuterm_log("extdisp-d3d12: frame0 step 4 EncodeFrame");
	ID3D12VideoEncodeCommandList2_EncodeFrame(d12->enc_list, d12->encoder, d12->heap, &enc_in, &enc_out);

	memset(&bar, 0, sizeof(bar));
	bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

	bar.Transition.pResource = d12->nv12_tex;
	bar.Transition.StateBefore = D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ;
	bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
	ID3D12VideoEncodeCommandList2_ResourceBarrier(d12->enc_list, 1U, &bar);

	bar.Transition.pResource = d12->dpb_tex[d12->cur_dpb];
	bar.Transition.StateBefore = D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE;
	bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
	ID3D12VideoEncodeCommandList2_ResourceBarrier(d12->enc_list, 1U, &bar);

	if (!is_idr) {
		bar.Transition.pResource = d12->dpb_tex[d12->prev_dpb];
		bar.Transition.StateBefore = D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ;
		bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
		ID3D12VideoEncodeCommandList2_ResourceBarrier(d12->enc_list, 1U, &bar);
	}

	bar.Transition.pResource = d12->meta_buf;
	bar.Transition.StateBefore = D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE;
	bar.Transition.StateAfter = D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ;
	ID3D12VideoEncodeCommandList2_ResourceBarrier(d12->enc_list, 1U, &bar);

	if (!d12->first_frame_logged)
		mikuterm_log("extdisp-d3d12: frame0 step 5 ResolveEncoderOutputMetadata");
	memset(&res_in, 0, sizeof(res_in));
	res_in.EncoderCodec = D3D12_VIDEO_ENCODER_CODEC_H264;
	res_in.EncoderProfile.DataSize = sizeof(prof_h264);
	res_in.EncoderProfile.pH264Profile = &prof_h264;
	res_in.EncoderInputFormat = DXGI_FORMAT_NV12;
	res_in.EncodedPictureEffectiveResolution.Width = d12->width;
	res_in.EncodedPictureEffectiveResolution.Height = d12->height;
	res_in.HWLayoutMetadata.pBuffer = d12->meta_buf;

	memset(&res_out, 0, sizeof(res_out));
	res_out.ResolvedLayoutMetadata.pBuffer = d12->resolved_meta_buf;

	ID3D12VideoEncodeCommandList2_ResolveEncoderOutputMetadata(d12->enc_list, &res_in, &res_out);

	bar.Transition.pResource = d12->meta_buf;
	bar.Transition.StateBefore = D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ;
	bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
	ID3D12VideoEncodeCommandList2_ResourceBarrier(d12->enc_list, 1U, &bar);

	bar.Transition.pResource = d12->bitstream_buf;
	bar.Transition.StateBefore = D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE;
	bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
	ID3D12VideoEncodeCommandList2_ResourceBarrier(d12->enc_list, 1U, &bar);

	if (FAILED(ID3D12VideoEncodeCommandList2_Close(d12->enc_list)))
		return -1;

	{
		ID3D12CommandList *lists[1];
		lists[0] = (ID3D12CommandList *)d12->enc_list;
		ID3D12CommandQueue_ExecuteCommandLists(d12->enc_queue, 1U, lists);
	}
	done_val = ++d12->fence_val;
	(void)ID3D12CommandQueue_Signal(d12->enc_queue, d12->done_fence, done_val);
	(void)ID3D12CommandQueue_Wait(d12->copy_queue, d12->done_fence, done_val);

	if (FAILED(ID3D12CommandAllocator_Reset(d12->copy_alloc)) ||
	    FAILED(ID3D12GraphicsCommandList_Reset(d12->copy_list, d12->copy_alloc, NULL)))
		return -1;
	if (!d12->first_frame_logged)
		mikuterm_log("extdisp-d3d12: frame0 step 6 CopyResource(readback)");
	ID3D12GraphicsCommandList_CopyResource(d12->copy_list, d12->readback_buf, d12->bitstream_buf);
	if (FAILED(ID3D12GraphicsCommandList_Close(d12->copy_list)))
		return -1;

	{
		ID3D12CommandList *lists[1];
		lists[0] = (ID3D12CommandList *)d12->copy_list;
		ID3D12CommandQueue_ExecuteCommandLists(d12->copy_queue, 1U, lists);
	}
	done_val = ++d12->fence_val;
	(void)ID3D12CommandQueue_Signal(d12->copy_queue, d12->done_fence, done_val);
	if (extdisp_d3d12_wait_fence(d12, done_val) != 0)
		return -1;

	hr = ID3D12Resource_Map(d12->resolved_meta_buf, 0U, NULL, (void **)&meta);
	if (FAILED(hr) || meta == NULL)
		return -1;
	bs_bytes = meta->EncodedBitstreamWrittenBytesCount;
	if (meta->EncodeErrorFlags != 0ULL || bs_bytes == 0ULL || bs_bytes > EXTDISP_D12_BITSTREAM_BYTES) {
		mikuterm_log("extdisp-d3d12: encode metadata err=0x%llx bytes=%llu",
		    (unsigned long long)meta->EncodeErrorFlags, (unsigned long long)bs_bytes);
		ID3D12Resource_Unmap(d12->resolved_meta_buf, 0U, NULL);
		return -1;
	}
	ID3D12Resource_Unmap(d12->resolved_meta_buf, 0U, NULL);

	hr = ID3D12Resource_Map(d12->readback_buf, 0U, NULL, &mapped_bs);
	if (FAILED(hr) || mapped_bs == NULL)
		return -1;
	if (!d12->first_frame_logged) {
		char ps_hex[73];
		char bs_hex[73];
		size_t hi;
		size_t ps_n = d12->ps_len < 24U ? d12->ps_len : 24U;
		size_t bs_n = (size_t)bs_bytes < 24U ? (size_t)bs_bytes : 24U;
		const uint8_t *bs_ptr = (const uint8_t *)mapped_bs;

		for (hi = 0U; hi < ps_n; hi++)
			(void)snprintf(ps_hex + hi * 3U, 4U, "%02x ", d12->ps_buf[hi]);
		ps_hex[ps_n > 0U ? ps_n * 3U - 1U : 0U] = '\0';
		for (hi = 0U; hi < bs_n; hi++)
			(void)snprintf(bs_hex + hi * 3U, 4U, "%02x ", bs_ptr[hi]);
		bs_hex[bs_n > 0U ? bs_n * 3U - 1U : 0U] = '\0';
		mikuterm_log("extdisp-d3d12: frame0 step 7 Map ok bytes=%llu ps_len=%zu ps_hex=[%s] bs_hex=[%s]",
		    (unsigned long long)bs_bytes, d12->ps_len, ps_hex, bs_hex);
		d12->first_frame_logged = 1;
	}

	if (is_idr && d12->ps_len + (size_t)bs_bytes <= d12->pkt_cap) {
		memcpy(d12->pkt_buf, d12->ps_buf, d12->ps_len);
		memcpy(d12->pkt_buf + d12->ps_len, mapped_bs, (size_t)bs_bytes);
		*out_data = d12->pkt_buf;
		*out_size = d12->ps_len + (size_t)bs_bytes;
	} else if ((size_t)bs_bytes <= d12->pkt_cap) {
		memcpy(d12->pkt_buf, mapped_bs, (size_t)bs_bytes);
		*out_data = d12->pkt_buf;
		*out_size = (size_t)bs_bytes;
	} else {
		ID3D12Resource_Unmap(d12->readback_buf, 0U, NULL);
		return -1;
	}
	ID3D12Resource_Unmap(d12->readback_buf, 0U, NULL);

	if (out_is_key != NULL)
		*out_is_key = is_idr;

	d12->prev_dpb = d12->cur_dpb;
	d12->cur_dpb ^= 1U;
	d12->frame_num = (d12->frame_num + 1U) & 15U;
	d12->frames_since_idr = is_idr ? 1U : (d12->frames_since_idr + 1U);
	return 0;
}

/*
 * Releases all D3D12 and shared D3D11 resources.
 */
void
extdisp_d3d12_close(
	struct extdisp_d3d12 *d12)
{
	uint32_t i;

	if (d12 == NULL)
		return;

	if (d12->done_fence != NULL && d12->enc_queue != NULL && d12->fence_event != NULL) {
		uint64_t val = ++d12->fence_val;
		(void)ID3D12CommandQueue_Signal(d12->enc_queue, d12->done_fence, val);
		(void)extdisp_d3d12_wait_fence(d12, val);
	}

	if (d12->readback_buf != NULL)
		ID3D12Resource_Release(d12->readback_buf);
	if (d12->resolved_meta_buf != NULL)
		ID3D12Resource_Release(d12->resolved_meta_buf);
	if (d12->meta_buf != NULL)
		ID3D12Resource_Release(d12->meta_buf);
	if (d12->bitstream_buf != NULL)
		ID3D12Resource_Release(d12->bitstream_buf);
	for (i = 0U; i < EXTDISP_D12_DPB_SLOTS; i++) {
		if (d12->dpb_tex[i] != NULL)
			ID3D12Resource_Release(d12->dpb_tex[i]);
	}
	if (d12->nv12_tex != NULL)
		ID3D12Resource_Release(d12->nv12_tex);
	if (d12->d3d12_src_tex != NULL)
		ID3D12Resource_Release(d12->d3d12_src_tex);

	if (d12->heap != NULL)
		ID3D12VideoEncoderHeap_Release(d12->heap);
	if (d12->encoder != NULL)
		ID3D12VideoEncoder_Release(d12->encoder);
	if (d12->enc_list != NULL)
		ID3D12VideoEncodeCommandList2_Release(d12->enc_list);
	if (d12->enc_alloc != NULL)
		ID3D12CommandAllocator_Release(d12->enc_alloc);
	if (d12->enc_queue != NULL)
		ID3D12CommandQueue_Release(d12->enc_queue);

	if (d12->copy_list != NULL)
		ID3D12GraphicsCommandList_Release(d12->copy_list);
	if (d12->copy_alloc != NULL)
		ID3D12CommandAllocator_Release(d12->copy_alloc);
	if (d12->copy_queue != NULL)
		ID3D12CommandQueue_Release(d12->copy_queue);

	if (d12->vp != NULL)
		ID3D12VideoProcessor_Release(d12->vp);
	if (d12->vp_list != NULL)
		ID3D12VideoProcessCommandList_Release(d12->vp_list);
	if (d12->vp_alloc != NULL)
		ID3D12CommandAllocator_Release(d12->vp_alloc);
	if (d12->vp_queue != NULL)
		ID3D12CommandQueue_Release(d12->vp_queue);

	if (d12->fence_event != NULL)
		CloseHandle(d12->fence_event);
	if (d12->done_fence != NULL)
		ID3D12Fence_Release(d12->done_fence);
	if (d12->shared_fence != NULL)
		ID3D12Fence_Release(d12->shared_fence);
	if (d12->vdev != NULL)
		ID3D12VideoDevice3_Release(d12->vdev);
	if (d12->dev != NULL)
		ID3D12Device_Release(d12->dev);

	if (d12->d3d11_fence != NULL)
		d12->d3d11_fence->lpVtbl->Release(d12->d3d11_fence);
	if (d12->fence_handle != NULL)
		CloseHandle(d12->fence_handle);
	if (d12->tex_handle != NULL)
		CloseHandle(d12->tex_handle);
	if (d12->d3d11_cursor_tex != NULL)
		d12->d3d11_cursor_tex->lpVtbl->Release(d12->d3d11_cursor_tex);
	if (d12->d3d11_shared_tex != NULL)
		d12->d3d11_shared_tex->lpVtbl->Release(d12->d3d11_shared_tex);
	if (d12->d3d11_ctx4 != NULL)
		d12->d3d11_ctx4->lpVtbl->Release(d12->d3d11_ctx4);
	if (d12->d3d11_dev5 != NULL)
		d12->d3d11_dev5->lpVtbl->Release(d12->d3d11_dev5);
	if (d12->d3d12_dll != NULL)
		FreeLibrary(d12->d3d12_dll);

	free(d12->pkt_buf);
	free(d12);
}

static void
extdisp_rbsp_put_bits(
	struct extdisp_rbsp *r,
	uint32_t val,
	int n)
{
	int i;

	for (i = n - 1; i >= 0; i--) {
		r->cur = (r->cur << 1) | ((val >> i) & 1U);
		r->bits++;
		if (r->bits == 8) {
			if (r->bytes < sizeof(r->buf))
				r->buf[r->bytes++] = (uint8_t)r->cur;
			r->cur = 0U;
			r->bits = 0;
		}
	}
}

static void
extdisp_rbsp_put_ue(
	struct extdisp_rbsp *r,
	uint32_t val)
{
	uint32_t code = val + 1U;
	int bits = 0;
	uint32_t tmp = code;

	while (tmp > 0U) {
		bits++;
		tmp >>= 1;
	}
	if (bits > 1)
		extdisp_rbsp_put_bits(r, 0U, bits - 1);
	extdisp_rbsp_put_bits(r, code, bits);
}

static void
extdisp_rbsp_finish_nal(
	struct extdisp_rbsp *r,
	uint8_t nal_header,
	uint8_t *out,
	size_t *out_len,
	size_t cap)
{
	size_t i;
	size_t pos = *out_len;
	int zeros = 0;

	extdisp_rbsp_put_bits(r, 1U, 1);
	if (r->bits > 0)
		extdisp_rbsp_put_bits(r, 0U, 8 - r->bits);

	if (pos + 5U > cap)
		return;
	out[pos++] = 0x00U;
	out[pos++] = 0x00U;
	out[pos++] = 0x00U;
	out[pos++] = 0x01U;
	out[pos++] = nal_header;

	for (i = 0U; i < r->bytes && pos + 2U <= cap; i++) {
		uint8_t b = r->buf[i];
		if (zeros == 2 && b <= 3U) {
			out[pos++] = 0x03U;
			zeros = 0;
		}
		out[pos++] = b;
		zeros = (b == 0U) ? (zeros + 1) : 0;
	}
	*out_len = pos;
}

static void
extdisp_d3d12_build_sps_pps(
	struct extdisp_d3d12 *d12)
{
	struct extdisp_rbsp r;
	uint32_t mb_w = d12->aligned_w / 16U;
	uint32_t mb_h = d12->aligned_h / 16U;
	int crop = (d12->aligned_w != d12->width || d12->aligned_h != d12->height);

	d12->ps_len = 0U;

	memset(&r, 0, sizeof(r));
	extdisp_rbsp_put_bits(&r, 77U, 8);
	extdisp_rbsp_put_bits(&r, 0U, 8);
	extdisp_rbsp_put_bits(&r, d12->level_idc, 8);
	extdisp_rbsp_put_ue(&r, 0U);
	extdisp_rbsp_put_ue(&r, 0U);
	extdisp_rbsp_put_ue(&r, 2U);
	extdisp_rbsp_put_ue(&r, 1U);
	extdisp_rbsp_put_bits(&r, 0U, 1);
	extdisp_rbsp_put_ue(&r, mb_w - 1U);
	extdisp_rbsp_put_ue(&r, mb_h - 1U);
	extdisp_rbsp_put_bits(&r, 1U, 1);
	extdisp_rbsp_put_bits(&r, 1U, 1);
	extdisp_rbsp_put_bits(&r, crop ? 1U : 0U, 1);
	if (crop) {
		extdisp_rbsp_put_ue(&r, 0U);
		extdisp_rbsp_put_ue(&r, (d12->aligned_w - d12->width) / 2U);
		extdisp_rbsp_put_ue(&r, 0U);
		extdisp_rbsp_put_ue(&r, (d12->aligned_h - d12->height) / 2U);
	}
	extdisp_rbsp_put_bits(&r, 0U, 1);
	extdisp_rbsp_finish_nal(&r, 0x67U, d12->ps_buf, &d12->ps_len, sizeof(d12->ps_buf));

	memset(&r, 0, sizeof(r));
	extdisp_rbsp_put_ue(&r, 0U);
	extdisp_rbsp_put_ue(&r, 0U);
	extdisp_rbsp_put_bits(&r, 0U, 1);
	extdisp_rbsp_put_bits(&r, 0U, 1);
	extdisp_rbsp_put_ue(&r, 0U);
	extdisp_rbsp_put_ue(&r, 0U);
	extdisp_rbsp_put_ue(&r, 0U);
	extdisp_rbsp_put_bits(&r, 0U, 1);
	extdisp_rbsp_put_bits(&r, 0U, 2);
	extdisp_rbsp_put_ue(&r, 0U);
	extdisp_rbsp_put_ue(&r, 0U);
	extdisp_rbsp_put_ue(&r, 0U);
	extdisp_rbsp_put_bits(&r, 1U, 1);
	extdisp_rbsp_put_bits(&r, 0U, 1);
	extdisp_rbsp_put_bits(&r, 0U, 1);
	extdisp_rbsp_finish_nal(&r, 0x68U, d12->ps_buf, &d12->ps_len, sizeof(d12->ps_buf));
}

static uint32_t
extdisp_d3d12_level_to_idc(
	D3D12_VIDEO_ENCODER_LEVELS_H264 lvl)
{
	switch (lvl) {
	case D3D12_VIDEO_ENCODER_LEVELS_H264_31: return 31U;
	case D3D12_VIDEO_ENCODER_LEVELS_H264_32: return 32U;
	case D3D12_VIDEO_ENCODER_LEVELS_H264_4:  return 40U;
	case D3D12_VIDEO_ENCODER_LEVELS_H264_41: return 41U;
	case D3D12_VIDEO_ENCODER_LEVELS_H264_42: return 42U;
	case D3D12_VIDEO_ENCODER_LEVELS_H264_5:  return 50U;
	case D3D12_VIDEO_ENCODER_LEVELS_H264_51: return 51U;
	case D3D12_VIDEO_ENCODER_LEVELS_H264_52: return 52U;
	default: return 41U;
	}
}

static int
extdisp_d3d12_create_committed(
	ID3D12Device *dev,
	const char *name,
	D3D12_HEAP_TYPE heap,
	const D3D12_RESOURCE_DESC *desc,
	D3D12_RESOURCE_STATES state,
	ID3D12Resource **out)
{
	D3D12_HEAP_PROPERTIES hp;
	HRESULT hr;

	memset(&hp, 0, sizeof(hp));
	hp.Type = heap;
	hr = ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE,
	    desc, state, NULL, &IID_ID3D12Resource, (void **)out);
	if (FAILED(hr) || *out == NULL) {
		mikuterm_log("extdisp-d3d12: CreateCommittedResource(%s) failed hr=0x%08lx fmt=%d flags=0x%x state=0x%x heap=%d",
		    name, (unsigned long)hr, (int)desc->Format, (unsigned)desc->Flags,
		    (unsigned)state, (int)heap);
		return -1;
	}
	return 0;
}

static int
extdisp_d3d12_wait_fence(
	struct extdisp_d3d12 *d12,
	uint64_t val)
{
	if (ID3D12Fence_GetCompletedValue(d12->done_fence) < val) {
		if (FAILED(ID3D12Fence_SetEventOnCompletion(d12->done_fence, val, d12->fence_event)))
			return -1;
		WaitForSingleObject(d12->fence_event, 5000);
	}
	return 0;
}
