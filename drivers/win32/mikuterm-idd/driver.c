/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * UMDF 2 / IddCx 1.4 virtual display driver for mikuterm Extended Display.
 * Exposes a virtual monitor, copies swapchain frames via a D3D11 staging
 * texture into a cross-session shared-memory BGRA ring, and signals a
 * named event for mikuterm.exe.
 */

#include <windows.h>
#include <wdf.h>
#include <iddcx.h>
#include <d3d11_2.h>
#include <dxgi1_3.h>
#include <sddl.h>
#include <string.h>

#include "mikuterm_idd_ipc.h"

static const GUID GUID_DEVINTERFACE_MIKUTERM_IDD = MIKUTERM_IDD_INTERFACE_GUID_INIT;

/* Cross-session DACL: System, Built-in Admins, and Authenticated Users (RW). */
#define MIKUTERM_IDD_SDDL L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)"

typedef struct _MIKUTERM_DEVICE_CTX {
	WDFDEVICE wdf_device;
	IDDCX_ADAPTER adapter;
	IDDCX_MONITOR monitor;
	HANDLE shm_handle;
	void *shm_view;
	HANDLE frame_event;
	uint32_t width;
	uint32_t height;
	uint32_t refresh_hz;
	uint32_t frame_seq;
} MIKUTERM_DEVICE_CTX;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MIKUTERM_DEVICE_CTX, mikuterm_get_device_ctx);

typedef struct _MIKUTERM_MONITOR_CTX {
	MIKUTERM_DEVICE_CTX *dev_ctx;
	IDDCX_SWAPCHAIN swapchain;
	HANDLE thread;
	HANDLE stop_event;
	ID3D11Device *d3d_device;
	ID3D11DeviceContext *d3d_ctx;
	ID3D11Texture2D *staging;
	uint32_t staging_w;
	uint32_t staging_h;
} MIKUTERM_MONITOR_CTX;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MIKUTERM_MONITOR_CTX, mikuterm_get_monitor_ctx);

static const BYTE mikuterm_default_edid[128] = {
	0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
	0x35, 0xAD, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x24, 0x01, 0x04, 0xA5, 0x3C, 0x22, 0x78,
	0x3B, 0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26,
	0x0F, 0x50, 0x54, 0x21, 0x08, 0x00, 0x81, 0xC0,
	0xA9, 0xC0, 0xD1, 0xC0, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x3A,
	0x80, 0x18, 0x71, 0x38, 0x2D, 0x40, 0x58, 0x2C,
	0x45, 0x00, 0x58, 0x54, 0x21, 0x00, 0x00, 0x1E,
	0x00, 0x00, 0x00, 0xFD, 0x00, 0x32, 0x4B, 0x1E,
	0x53, 0x11, 0x00, 0x0A, 0x20, 0x20, 0x20, 0x20,
	0x20, 0x20, 0x00, 0x00, 0x00, 0xFC, 0x00, 0x6D,
	0x69, 0x6B, 0x75, 0x74, 0x65, 0x72, 0x6D, 0x0A,
	0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xFF,
	0x00, 0x30, 0x30, 0x30, 0x31, 0x0A, 0x20, 0x20,
	0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x37
};

static NTSTATUS
mikuterm_init_ipc(MIKUTERM_DEVICE_CTX *ctx)
{
	SECURITY_ATTRIBUTES sa;
	PSECURITY_DESCRIPTOR sd;
	ULARGE_INTEGER sz;
	struct mikuterm_idd_shm_header *hdr;

	sd = NULL;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
	    MIKUTERM_IDD_SDDL, SDDL_REVISION_1, &sd, NULL))
		return STATUS_ACCESS_DENIED;

	memset(&sa, 0, sizeof(sa));
	sa.nLength = sizeof(sa);
	sa.lpSecurityDescriptor = sd;
	sa.bInheritHandle = FALSE;

	sz.QuadPart = (ULONGLONG)MIKUTERM_IDD_SHM_TOTAL_SIZE;
	ctx->shm_handle = CreateFileMappingW(
	    INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
	    sz.HighPart, sz.LowPart, MIKUTERM_IDD_SHM_NAME);
	if (ctx->shm_handle != NULL) {
		ctx->shm_view = MapViewOfFile(
		    ctx->shm_handle, FILE_MAP_ALL_ACCESS,
		    0, 0, MIKUTERM_IDD_SHM_TOTAL_SIZE);
	}
	ctx->frame_event = CreateEventW(&sa, FALSE, FALSE, MIKUTERM_IDD_EVENT_NAME);
	LocalFree(sd);

	if (ctx->shm_view == NULL || ctx->frame_event == NULL)
		return STATUS_INSUFFICIENT_RESOURCES;

	hdr = (struct mikuterm_idd_shm_header *)ctx->shm_view;
	hdr->magic = MIKUTERM_IDD_SHM_MAGIC;
	hdr->version = MIKUTERM_IDD_SHM_VERSION;
	hdr->width = ctx->width;
	hdr->height = ctx->height;
	hdr->stride = ctx->width * 4U;
	hdr->slot_index = 0U;
	hdr->frame_seq = 0U;
	hdr->active = 1U;
	return STATUS_SUCCESS;
}

static void
mikuterm_cleanup_ipc(MIKUTERM_DEVICE_CTX *ctx)
{
	struct mikuterm_idd_shm_header *hdr;

	if (ctx->shm_view != NULL) {
		hdr = (struct mikuterm_idd_shm_header *)ctx->shm_view;
		hdr->active = 0U;
		UnmapViewOfFile(ctx->shm_view);
		ctx->shm_view = NULL;
	}
	if (ctx->shm_handle != NULL) {
		CloseHandle(ctx->shm_handle);
		ctx->shm_handle = NULL;
	}
	if (ctx->frame_event != NULL) {
		CloseHandle(ctx->frame_event);
		ctx->frame_event = NULL;
	}
}

static HRESULT
mikuterm_ensure_d3d(MIKUTERM_MONITOR_CTX *mctx, uint32_t w, uint32_t h)
{
	D3D11_TEXTURE2D_DESC desc;
	HRESULT hr;

	if (mctx->d3d_device == NULL) {
		D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
		hr = D3D11CreateDevice(
		    NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
		    &fl, 1, D3D11_SDK_VERSION,
		    &mctx->d3d_device, NULL, &mctx->d3d_ctx);
		if (FAILED(hr))
			return hr;
	}

	if (mctx->staging != NULL && mctx->staging_w == w && mctx->staging_h == h)
		return S_OK;

	if (mctx->staging != NULL) {
		mctx->staging->lpVtbl->Release(mctx->staging);
		mctx->staging = NULL;
	}

	memset(&desc, 0, sizeof(desc));
	desc.Width = w;
	desc.Height = h;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_STAGING;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

	hr = mctx->d3d_device->lpVtbl->CreateTexture2D(
	    mctx->d3d_device, &desc, NULL, &mctx->staging);
	if (SUCCEEDED(hr)) {
		mctx->staging_w = w;
		mctx->staging_h = h;
	}
	return hr;
}

static DWORD WINAPI
mikuterm_swapchain_thread(LPVOID param)
{
	MIKUTERM_MONITOR_CTX *mctx = (MIKUTERM_MONITOR_CTX *)param;
	MIKUTERM_DEVICE_CTX *dctx = mctx->dev_ctx;

	for (;;) {
		IDARG_OUT_RELEASEANDACQUIREBUFFER buf_out;
		IDARG_IN_RELEASEANDACQUIREBUFFER buf_in;
		IDXGIResource *dxgi_res;
		ID3D11Texture2D *tex;
		D3D11_TEXTURE2D_DESC desc;
		D3D11_MAPPED_SUBRESOURCE mapped;
		NTSTATUS st;
		HRESULT hr;
		uint32_t slot;
		uint32_t y;
		uint8_t *dst;
		const uint8_t *src;
		struct mikuterm_idd_shm_header *hdr;

		if (WaitForSingleObject(mctx->stop_event, 0) == WAIT_OBJECT_0)
			break;

		memset(&buf_in, 0, sizeof(buf_in));
		memset(&buf_out, 0, sizeof(buf_out));
		st = IddCxSwapChainReleaseAndAcquireBuffer(
		    mctx->swapchain, &buf_in, &buf_out);
		if (st == E_PENDING) {
			WaitForSingleObject(mctx->stop_event, 16);
			continue;
		}
		if (!NT_SUCCESS(st))
			break;

		dxgi_res = NULL;
		tex = NULL;
		hr = buf_out.MetaData.pSurface->lpVtbl->QueryInterface(
		    buf_out.MetaData.pSurface, &IID_ID3D11Texture2D, (void **)&tex);
		if (SUCCEEDED(hr) && tex != NULL) {
			tex->lpVtbl->GetDesc(tex, &desc);
			if (desc.Width <= MIKUTERM_IDD_MAX_WIDTH &&
			    desc.Height <= MIKUTERM_IDD_MAX_HEIGHT &&
			    SUCCEEDED(mikuterm_ensure_d3d(mctx, desc.Width, desc.Height))) {
				mctx->d3d_ctx->lpVtbl->CopyResource(
				    mctx->d3d_ctx,
				    (ID3D11Resource *)mctx->staging,
				    (ID3D11Resource *)tex);
				hr = mctx->d3d_ctx->lpVtbl->Map(
				    mctx->d3d_ctx,
				    (ID3D11Resource *)mctx->staging,
				    0, D3D11_MAP_READ, 0, &mapped);
				if (SUCCEEDED(hr) && dctx->shm_view != NULL) {
					hdr = (struct mikuterm_idd_shm_header *)dctx->shm_view;
					slot = (hdr->frame_seq + 1U) % MIKUTERM_IDD_RING_SLOTS;
					dst = (uint8_t *)dctx->shm_view +
					    sizeof(struct mikuterm_idd_shm_header) +
					    (size_t)slot * (size_t)MIKUTERM_IDD_SLOT_BYTES;
					src = (const uint8_t *)mapped.pData;
					for (y = 0; y < desc.Height; y++) {
						memcpy(dst + (size_t)y * desc.Width * 4U,
						    src + (size_t)y * mapped.RowPitch,
						    (size_t)desc.Width * 4U);
					}
					hdr->width = desc.Width;
					hdr->height = desc.Height;
					hdr->stride = desc.Width * 4U;
					hdr->slot_index = slot;
					MemoryBarrier();
					hdr->frame_seq = ++dctx->frame_seq;
					SetEvent(dctx->frame_event);
					mctx->d3d_ctx->lpVtbl->Unmap(
					    mctx->d3d_ctx, (ID3D11Resource *)mctx->staging, 0);
				}
			}
			tex->lpVtbl->Release(tex);
		}

		IddCxSwapChainFinishedProcessingFrame(mctx->swapchain);
	}

	return 0;
}

static NTSTATUS
mikuterm_evt_assign_swapchain(
	IDDCX_MONITOR monitor,
	const IDARG_IN_SETSWAPCHAIN *in_args)
{
	MIKUTERM_MONITOR_CTX *mctx = mikuterm_get_monitor_ctx(monitor);

	mctx->swapchain = in_args->hSwapChain;
	mctx->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (mctx->stop_event == NULL)
		return STATUS_INSUFFICIENT_RESOURCES;
	mctx->thread = CreateThread(NULL, 0, mikuterm_swapchain_thread, mctx, 0, NULL);
	if (mctx->thread == NULL) {
		CloseHandle(mctx->stop_event);
		mctx->stop_event = NULL;
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	return STATUS_SUCCESS;
}

static NTSTATUS
mikuterm_evt_unassign_swapchain(IDDCX_MONITOR monitor)
{
	MIKUTERM_MONITOR_CTX *mctx = mikuterm_get_monitor_ctx(monitor);

	if (mctx->stop_event != NULL)
		SetEvent(mctx->stop_event);
	if (mctx->thread != NULL) {
		WaitForSingleObject(mctx->thread, INFINITE);
		CloseHandle(mctx->thread);
		mctx->thread = NULL;
	}
	if (mctx->stop_event != NULL) {
		CloseHandle(mctx->stop_event);
		mctx->stop_event = NULL;
	}
	if (mctx->staging != NULL) {
		mctx->staging->lpVtbl->Release(mctx->staging);
		mctx->staging = NULL;
	}
	if (mctx->d3d_ctx != NULL) {
		mctx->d3d_ctx->lpVtbl->Release(mctx->d3d_ctx);
		mctx->d3d_ctx = NULL;
	}
	if (mctx->d3d_device != NULL) {
		mctx->d3d_device->lpVtbl->Release(mctx->d3d_device);
		mctx->d3d_device = NULL;
	}
	mctx->swapchain = NULL;
	return STATUS_SUCCESS;
}

static NTSTATUS
mikuterm_evt_parse_monitor_desc(
	const IDARG_IN_PARSEMONITORDESCRIPTION *in_args,
	IDARG_OUT_PARSEMONITORDESCRIPTION *out_args)
{
	(void)in_args;
	out_args->MonitorModeBufferOutputCount = 1;
	if (in_args->MonitorModeBufferInputCount >= 1) {
		IDDCX_MONITOR_MODE *mode = &in_args->pMonitorModes[0];
		mode->Size = sizeof(*mode);
		mode->Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTION;
		mode->MonitorVideoSignalInfo.videoStandard = D3DKMDT_VSS_VESA_DMT;
		mode->MonitorVideoSignalInfo.totalSize.cx = 1920;
		mode->MonitorVideoSignalInfo.totalSize.cy = 1080;
		mode->MonitorVideoSignalInfo.activeSize.cx = 1920;
		mode->MonitorVideoSignalInfo.activeSize.cy = 1080;
		mode->MonitorVideoSignalInfo.vSyncFreq.Numerator = 60;
		mode->MonitorVideoSignalInfo.vSyncFreq.Denominator = 1;
		mode->MonitorVideoSignalInfo.hSyncFreq.Numerator = 67500;
		mode->MonitorVideoSignalInfo.hSyncFreq.Denominator = 1;
		mode->MonitorVideoSignalInfo.pixelRate = 148500000;
		mode->MonitorVideoSignalInfo.scanLineOrdering = D3DDDI_SCANLINEORDERING_PROGRESSIVE;
		out_args->PreferredMonitorModeIdx = 0;
	}
	return STATUS_SUCCESS;
}

static NTSTATUS
mikuterm_evt_get_default_modes(
	IDDCX_MONITOR monitor,
	const IDARG_IN_GETDEFAULTDESCRIPTIONMODES *in_args,
	IDARG_OUT_GETDEFAULTDESCRIPTIONMODES *out_args)
{
	(void)monitor;
	out_args->DefaultMonitorModeBufferOutputCount = 1;
	if (in_args->DefaultMonitorModeBufferInputCount >= 1) {
		IDDCX_MONITOR_MODE *mode = &in_args->pDefaultMonitorModes[0];
		mode->Size = sizeof(*mode);
		mode->Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER;
		mode->MonitorVideoSignalInfo.videoStandard = D3DKMDT_VSS_VESA_DMT;
		mode->MonitorVideoSignalInfo.totalSize.cx = 1920;
		mode->MonitorVideoSignalInfo.totalSize.cy = 1080;
		mode->MonitorVideoSignalInfo.activeSize.cx = 1920;
		mode->MonitorVideoSignalInfo.activeSize.cy = 1080;
		mode->MonitorVideoSignalInfo.vSyncFreq.Numerator = 60;
		mode->MonitorVideoSignalInfo.vSyncFreq.Denominator = 1;
		mode->MonitorVideoSignalInfo.hSyncFreq.Numerator = 67500;
		mode->MonitorVideoSignalInfo.hSyncFreq.Denominator = 1;
		mode->MonitorVideoSignalInfo.pixelRate = 148500000;
		mode->MonitorVideoSignalInfo.scanLineOrdering = D3DDDI_SCANLINEORDERING_PROGRESSIVE;
		out_args->PreferredMonitorModeIdx = 0;
	}
	return STATUS_SUCCESS;
}

static NTSTATUS
mikuterm_evt_query_target_modes(
	IDDCX_MONITOR monitor,
	const IDARG_IN_QUERYTARGETMODES *in_args,
	IDARG_OUT_QUERYTARGETMODES *out_args)
{
	MIKUTERM_MONITOR_CTX *mctx = mikuterm_get_monitor_ctx(monitor);
	uint32_t w = mctx->dev_ctx->width ? mctx->dev_ctx->width : 1920U;
	uint32_t h = mctx->dev_ctx->height ? mctx->dev_ctx->height : 1080U;
	uint32_t hz = mctx->dev_ctx->refresh_hz ? mctx->dev_ctx->refresh_hz : 60U;

	out_args->TargetModeBufferOutputCount = 1;
	if (in_args->TargetModeBufferInputCount >= 1) {
		IDDCX_TARGET_MODE *tm = &in_args->pTargetModes[0];
		tm->Size = sizeof(*tm);
		tm->TargetVideoSignalInfo.targetVideoSignalInfo.videoStandard = D3DKMDT_VSS_VESA_DMT;
		tm->TargetVideoSignalInfo.targetVideoSignalInfo.totalSize.cx = w;
		tm->TargetVideoSignalInfo.targetVideoSignalInfo.totalSize.cy = h;
		tm->TargetVideoSignalInfo.targetVideoSignalInfo.activeSize.cx = w;
		tm->TargetVideoSignalInfo.targetVideoSignalInfo.activeSize.cy = h;
		tm->TargetVideoSignalInfo.targetVideoSignalInfo.vSyncFreq.Numerator = hz;
		tm->TargetVideoSignalInfo.targetVideoSignalInfo.vSyncFreq.Denominator = 1;
		tm->TargetVideoSignalInfo.targetVideoSignalInfo.hSyncFreq.Numerator = h * hz;
		tm->TargetVideoSignalInfo.targetVideoSignalInfo.hSyncFreq.Denominator = 1;
		tm->TargetVideoSignalInfo.targetVideoSignalInfo.pixelRate = (UINT64)w * h * hz;
		tm->TargetVideoSignalInfo.targetVideoSignalInfo.scanLineOrdering = D3DDDI_SCANLINEORDERING_PROGRESSIVE;
	}
	return STATUS_SUCCESS;
}

static NTSTATUS
mikuterm_evt_adapter_init_finished(
	IDDCX_ADAPTER adapter,
	const IDARG_IN_ADAPTER_INIT_FINISHED *in_args)
{
	WDFDEVICE wdf_dev;
	MIKUTERM_DEVICE_CTX *dctx;
	WDF_OBJECT_ATTRIBUTES attr;
	IDDCX_MONITOR_INFO mon_info;
	IDARG_IN_MONITORCREATE mon_create;
	IDARG_OUT_MONITORCREATE mon_out;
	IDARG_OUT_MONITORARRIVAL arr_out;
	MIKUTERM_MONITOR_CTX *mctx;
	NTSTATUS st;

	if (!NT_SUCCESS(in_args->AdapterInitStatus))
		return in_args->AdapterInitStatus;

	wdf_dev = IddCxAdapterGetWdfDevice(adapter);
	dctx = mikuterm_get_device_ctx(wdf_dev);

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, MIKUTERM_MONITOR_CTX);
	memset(&mon_info, 0, sizeof(mon_info));
	mon_info.Size = sizeof(mon_info);
	mon_info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER;
	mon_info.ConnectorIndex = 0;
	mon_info.MonitorDescription.Size = sizeof(mon_info.MonitorDescription);
	mon_info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
	mon_info.MonitorDescription.DataSize = sizeof(mikuterm_default_edid);
	mon_info.MonitorDescription.pData = (PVOID)mikuterm_default_edid;
	CoCreateGuid(&mon_info.MonitorContainerId);

	memset(&mon_create, 0, sizeof(mon_create));
	mon_create.Size = sizeof(mon_create);
	mon_create.ObjectAttributes = &attr;
	mon_create.pMonitorInfo = &mon_info;

	st = IddCxMonitorCreate(adapter, &mon_create, &mon_out);
	if (!NT_SUCCESS(st))
		return st;

	dctx->monitor = mon_out.MonitorObject;
	mctx = mikuterm_get_monitor_ctx(dctx->monitor);
	mctx->dev_ctx = dctx;

	st = IddCxMonitorArrival(dctx->monitor, &arr_out);
	return st;
}

static VOID
mikuterm_evt_io_device_control(
	WDFQUEUE queue,
	WDFREQUEST request,
	size_t out_len,
	size_t in_len,
	ULONG ioctl_code)
{
	WDFDEVICE dev = WdfIoQueueGetDevice(queue);
	MIKUTERM_DEVICE_CTX *dctx = mikuterm_get_device_ctx(dev);
	NTSTATUS st = STATUS_INVALID_DEVICE_REQUEST;
	size_t bytes = 0;

	(void)out_len;
	(void)in_len;

	if (ioctl_code == IOCTL_MIKUTERM_IDD_SET_MODE) {
		struct mikuterm_idd_mode *mode = NULL;
		st = WdfRequestRetrieveInputBuffer(request, sizeof(*mode), (PVOID *)&mode, NULL);
		if (NT_SUCCESS(st) && mode->width >= 320U && mode->width <= MIKUTERM_IDD_MAX_WIDTH &&
		    mode->height >= 240U && mode->height <= MIKUTERM_IDD_MAX_HEIGHT) {
			dctx->width = mode->width;
			dctx->height = mode->height;
			dctx->refresh_hz = mode->refresh_hz ? mode->refresh_hz : 60U;
			st = STATUS_SUCCESS;
		} else {
			st = STATUS_INVALID_PARAMETER;
		}
	} else if (ioctl_code == IOCTL_MIKUTERM_IDD_GET_STATUS) {
		struct mikuterm_idd_status *status = NULL;
		st = WdfRequestRetrieveOutputBuffer(request, sizeof(*status), (PVOID *)&status, NULL);
		if (NT_SUCCESS(st)) {
			status->version = MIKUTERM_IDD_SHM_VERSION;
			status->connected = (dctx->monitor != NULL) ? 1U : 0U;
			status->width = dctx->width;
			status->height = dctx->height;
			status->refresh_hz = dctx->refresh_hz;
			status->frame_seq = dctx->frame_seq;
			bytes = sizeof(*status);
			st = STATUS_SUCCESS;
		}
	}

	WdfRequestCompleteWithInformation(request, st, bytes);
}

static VOID
mikuterm_evt_device_cleanup(WDFOBJECT obj)
{
	MIKUTERM_DEVICE_CTX *dctx = mikuterm_get_device_ctx((WDFDEVICE)obj);
	mikuterm_cleanup_ipc(dctx);
}

static NTSTATUS
mikuterm_evt_device_d0_entry(
	WDFDEVICE device,
	WDF_POWER_DEVICE_STATE prev_state)
{
	MIKUTERM_DEVICE_CTX *dctx = mikuterm_get_device_ctx(device);
	IDARG_IN_ADAPTER_INIT adapter_init;
	IDARG_OUT_ADAPTER_INIT adapter_out;
	IDDCX_ADAPTER_CAPS caps;
	IDDCX_ENDPOINT_VERSION ver;

	(void)prev_state;

	memset(&caps, 0, sizeof(caps));
	caps.Size = sizeof(caps);
	caps.MaxMonitorsSupported = 1;
	caps.EndPointDiagnostics.Size = sizeof(caps.EndPointDiagnostics);
	caps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
	caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;

	memset(&ver, 0, sizeof(ver));
	ver.Size = sizeof(ver);
	ver.MajorVer = 1;
	caps.EndPointDiagnostics.pEndPointFriendlyName = L"mikuterm Virtual Display";
	caps.EndPointDiagnostics.pEndPointManufacturerName = L"zedBSD";
	caps.EndPointDiagnostics.pEndPointModelName = L"mikuterm-idd";
	caps.EndPointDiagnostics.pFirmwareVersion = &ver;
	caps.EndPointDiagnostics.pHardwareVersion = &ver;

	memset(&adapter_init, 0, sizeof(adapter_init));
	adapter_init.WdfDevice = device;
	adapter_init.pCaps = &caps;

	return IddCxAdapterInitAsync(&adapter_init, &adapter_out);
}

static NTSTATUS
mikuterm_evt_device_add(
	WDFDRIVER driver,
	PWDFDEVICE_INIT device_init)
{
	WDF_OBJECT_ATTRIBUTES attr;
	WDF_PNPPOWER_EVENT_CALLBACKS pnp;
	IDD_CX_CLIENT_CONFIG idd_cfg;
	WDF_IO_QUEUE_CONFIG q_cfg;
	WDFDEVICE dev;
	MIKUTERM_DEVICE_CTX *dctx;
	NTSTATUS st;

	(void)driver;

	WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
	pnp.EvtDeviceD0Entry = mikuterm_evt_device_d0_entry;
	WdfDeviceInitSetPnpPowerEventCallbacks(device_init, &pnp);

	IDD_CX_CLIENT_CONFIG_INIT(&idd_cfg);
	idd_cfg.EvtIddCxAdapterInitFinished = mikuterm_evt_adapter_init_finished;
	idd_cfg.EvtIddCxParseMonitorDescription = mikuterm_evt_parse_monitor_desc;
	idd_cfg.EvtIddCxMonitorGetDefaultDescriptionModes = mikuterm_evt_get_default_modes;
	idd_cfg.EvtIddCxMonitorQueryTargetModes = mikuterm_evt_query_target_modes;
	idd_cfg.EvtIddCxMonitorAssignSwapChain = mikuterm_evt_assign_swapchain;
	idd_cfg.EvtIddCxMonitorUnassignSwapChain = mikuterm_evt_unassign_swapchain;
	st = IddCxDeviceInitConfig(device_init, &idd_cfg);
	if (!NT_SUCCESS(st))
		return st;

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, MIKUTERM_DEVICE_CTX);
	attr.EvtCleanupCallback = mikuterm_evt_device_cleanup;
	st = WdfDeviceCreate(&device_init, &attr, &dev);
	if (!NT_SUCCESS(st))
		return st;

	dctx = mikuterm_get_device_ctx(dev);
	dctx->wdf_device = dev;
	dctx->width = 1920U;
	dctx->height = 1080U;
	dctx->refresh_hz = 60U;

	st = mikuterm_init_ipc(dctx);
	if (!NT_SUCCESS(st))
		return st;

	st = IddCxDeviceInitialize(dev);
	if (!NT_SUCCESS(st))
		return st;

	st = WdfDeviceCreateDeviceInterface(dev, &GUID_DEVINTERFACE_MIKUTERM_IDD, NULL);
	if (!NT_SUCCESS(st))
		return st;

	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&q_cfg, WdfIoQueueDispatchParallel);
	q_cfg.EvtIoDeviceControl = mikuterm_evt_io_device_control;
	st = WdfIoQueueCreate(dev, &q_cfg, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
	return st;
}

NTSTATUS
DriverEntry(
	PDRIVER_OBJECT driver_object,
	PUNICODE_STRING registry_path)
{
	WDF_DRIVER_CONFIG cfg;

	WDF_DRIVER_CONFIG_INIT(&cfg, mikuterm_evt_device_add);
	return WdfDriverCreate(
	    driver_object, registry_path, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}
