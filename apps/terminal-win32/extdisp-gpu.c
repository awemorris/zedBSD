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

#include "extdisp-gpu.h"
#include "extdisp-nv12-spv.h"
#include "apps/mikuterm-common/mikuterm.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_2.h>
#include <d3d11_1.h>
#include <setupapi.h>
#include <newdev.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "extdisp-d3d12.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXTDISP_GPU_MAX_DEVICES		8U
#define EXTDISP_GPU_DPB_SLOTS		2U
#define EXTDISP_GPU_BITSTREAM_BYTES	(4U * 1024U * 1024U)
#define EXTDISP_GPU_DEFAULT_QP		26U
#define EXTDISP_GPU_KEY_QP_DELTA	3U
#define EXTDISP_GPU_KEY_INTERVAL	120U
#define EXTDISP_GPU_VDD_PIPE		L"\\\\.\\pipe\\MTTVirtualDisplayPipe"

typedef HRESULT (WINAPI *pfn_create_dxgi_factory1)(REFIID riid, void **ppFactory);
typedef HRESULT (WINAPI *pfn_d3d11_create_device)(
	IDXGIAdapter *pAdapter,
	D3D_DRIVER_TYPE DriverType,
	HMODULE Software,
	UINT Flags,
	const D3D_FEATURE_LEVEL *pFeatureLevels,
	UINT FeatureLevels,
	UINT SDKVersion,
	ID3D11Device **ppDevice,
	D3D_FEATURE_LEVEL *pFeatureLevel,
	ID3D11DeviceContext **ppImmediateContext);

/*
 * Dynamically resolved Vulkan entry points for instance, device, compute,
 * Win32 external memory import, and Vulkan Video H.264 encode.
 */
struct extdisp_vk_fn {
	PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
	PFN_vkCreateInstance vkCreateInstance;
	PFN_vkDestroyInstance vkDestroyInstance;
	PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices;
	PFN_vkGetPhysicalDeviceProperties2 vkGetPhysicalDeviceProperties2;
	PFN_vkGetPhysicalDeviceQueueFamilyProperties2 vkGetPhysicalDeviceQueueFamilyProperties2;
	PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
	PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR vkGetPhysicalDeviceVideoCapabilitiesKHR;
	PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties;
	PFN_vkCreateDevice vkCreateDevice;
	PFN_vkDestroyDevice vkDestroyDevice;
	PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;
	PFN_vkGetDeviceQueue vkGetDeviceQueue;
	PFN_vkCreateCommandPool vkCreateCommandPool;
	PFN_vkDestroyCommandPool vkDestroyCommandPool;
	PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
	PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
	PFN_vkEndCommandBuffer vkEndCommandBuffer;
	PFN_vkQueueSubmit vkQueueSubmit;
	PFN_vkDeviceWaitIdle vkDeviceWaitIdle;
	PFN_vkCreateSemaphore vkCreateSemaphore;
	PFN_vkDestroySemaphore vkDestroySemaphore;
	PFN_vkCreateFence vkCreateFence;
	PFN_vkDestroyFence vkDestroyFence;
	PFN_vkWaitForFences vkWaitForFences;
	PFN_vkResetFences vkResetFences;
	PFN_vkCreateImage vkCreateImage;
	PFN_vkDestroyImage vkDestroyImage;
	PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;
	PFN_vkBindImageMemory vkBindImageMemory;
	PFN_vkCreateImageView vkCreateImageView;
	PFN_vkDestroyImageView vkDestroyImageView;
	PFN_vkCreateSampler vkCreateSampler;
	PFN_vkDestroySampler vkDestroySampler;
	PFN_vkCreateBuffer vkCreateBuffer;
	PFN_vkDestroyBuffer vkDestroyBuffer;
	PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
	PFN_vkBindBufferMemory vkBindBufferMemory;
	PFN_vkAllocateMemory vkAllocateMemory;
	PFN_vkFreeMemory vkFreeMemory;
	PFN_vkMapMemory vkMapMemory;
	PFN_vkUnmapMemory vkUnmapMemory;
	PFN_vkCreateShaderModule vkCreateShaderModule;
	PFN_vkDestroyShaderModule vkDestroyShaderModule;
	PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout;
	PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout;
	PFN_vkCreateDescriptorPool vkCreateDescriptorPool;
	PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool;
	PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets;
	PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets;
	PFN_vkCreatePipelineLayout vkCreatePipelineLayout;
	PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout;
	PFN_vkCreateComputePipelines vkCreateComputePipelines;
	PFN_vkDestroyPipeline vkDestroyPipeline;
	PFN_vkCreateQueryPool vkCreateQueryPool;
	PFN_vkDestroyQueryPool vkDestroyQueryPool;
	PFN_vkGetQueryPoolResults vkGetQueryPoolResults;
	PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;
	PFN_vkCmdBindPipeline vkCmdBindPipeline;
	PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets;
	PFN_vkCmdPushConstants vkCmdPushConstants;
	PFN_vkCmdDispatch vkCmdDispatch;
	PFN_vkCmdResetQueryPool vkCmdResetQueryPool;
	PFN_vkCmdBeginQuery vkCmdBeginQuery;
	PFN_vkCmdEndQuery vkCmdEndQuery;
	PFN_vkCreateVideoSessionKHR vkCreateVideoSessionKHR;
	PFN_vkDestroyVideoSessionKHR vkDestroyVideoSessionKHR;
	PFN_vkGetVideoSessionMemoryRequirementsKHR vkGetVideoSessionMemoryRequirementsKHR;
	PFN_vkBindVideoSessionMemoryKHR vkBindVideoSessionMemoryKHR;
	PFN_vkCreateVideoSessionParametersKHR vkCreateVideoSessionParametersKHR;
	PFN_vkDestroyVideoSessionParametersKHR vkDestroyVideoSessionParametersKHR;
	PFN_vkGetEncodedVideoSessionParametersKHR vkGetEncodedVideoSessionParametersKHR;
	PFN_vkCmdBeginVideoCodingKHR vkCmdBeginVideoCodingKHR;
	PFN_vkCmdEndVideoCodingKHR vkCmdEndVideoCodingKHR;
	PFN_vkCmdControlVideoCodingKHR vkCmdControlVideoCodingKHR;
	PFN_vkCmdEncodeVideoKHR vkCmdEncodeVideoKHR;
};

/*
 * Push constant payload passed to the BGRA/RGBA-to-NV12 compute shader.
 */
struct extdisp_compute_pc {
	uint32_t visible_w;
	uint32_t visible_h;
	uint32_t coded_w;
	uint32_t coded_h;
};

/*
 * Active DXGI capture, D3D11 shared texture, Vulkan compute converter, and
 * Vulkan Video H.264 encoder state for one Extended Display session.
 */
struct extdisp_gpu {
	HMODULE dxgi_dll;
	HMODULE d3d11_dll;
	HMODULE vulkan_dll;
	pfn_create_dxgi_factory1 create_dxgi_factory1;
	pfn_d3d11_create_device d3d11_create_device;

	uint32_t pref_width;
	uint32_t pref_height;
	uint32_t width;
	uint32_t height;
	uint32_t coded_w;
	uint32_t coded_h;
	DXGI_FORMAT dxgi_format;
	VkFormat vk_src_format;
	LUID adapter_luid;

	IDXGIFactory1 *factory;
	IDXGIAdapter1 *adapter;
	IDXGIOutput1 *output1;
	ID3D11Device *d3d_dev;
	ID3D11DeviceContext *d3d_ctx;
	IDXGIOutputDuplication *dupl;
	ID3D11Texture2D *shared_tex;
	ID3D11Texture2D *staging_tex;
	IDXGIKeyedMutex *keyed_mutex;
	HANDLE shared_handle;
	int found_secondary;
	int found_vdd;
	int require_vdd;
	int zero_copy;
	struct extdisp_d3d12 *d3d12;
	RECT desktop_rect;

	uint8_t *cursor_shape;
	UINT cursor_shape_cap;
	DXGI_OUTDUPL_POINTER_SHAPE_INFO cursor_info;
	int cursor_visible;
	int cursor_x;
	int cursor_y;

	struct extdisp_vk_fn vk;
	VkInstance instance;
	VkPhysicalDevice physical;
	VkPhysicalDeviceMemoryProperties mem_props;
	VkDevice device;
	uint32_t compute_family;
	uint32_t encode_family;
	VkQueue compute_queue;
	VkQueue encode_queue;
	VkCommandPool compute_pool;
	VkCommandPool encode_pool;
	VkCommandBuffer compute_cmd;
	VkCommandBuffer encode_cmd;
	VkSemaphore compute_done;
	VkFence encode_fence;

	VkImage src_image;
	VkDeviceMemory src_mem;
	VkImageView src_view;
	VkSampler src_sampler;

	VkImage nv12_image;
	VkDeviceMemory nv12_mem;
	VkImageView nv12_y_view;
	VkImageView nv12_uv_view;
	VkImageView nv12_enc_view;

	VkShaderModule shader;
	VkDescriptorSetLayout desc_layout;
	VkDescriptorPool desc_pool;
	VkDescriptorSet desc_set;
	VkPipelineLayout pipe_layout;
	VkPipeline compute_pipe;

	VkVideoProfileInfoKHR profile;
	VkVideoEncodeH264ProfileInfoKHR h264_profile;
	VkVideoSessionKHR session;
	VkDeviceMemory session_mem[8];
	uint32_t session_mem_count;
	VkVideoSessionParametersKHR parameters;
	VkImage dpb_image[EXTDISP_GPU_DPB_SLOTS];
	VkDeviceMemory dpb_mem[EXTDISP_GPU_DPB_SLOTS];
	VkImageView dpb_view[EXTDISP_GPU_DPB_SLOTS];
	VkBuffer out_buf;
	VkDeviceMemory out_mem;
	void *out_map;
	VkQueryPool query_pool;

	uint8_t *ps_buf;
	size_t ps_len;
	uint8_t *pkt_buf;
	size_t pkt_cap;

	uint32_t cur_dpb;
	uint32_t prev_dpb;
	uint32_t frame_num;
	uint32_t idr_pic_id;
	uint32_t frames_since_idr;
	int session_reset;
	int resources_ready;
};

static int extdisp_gpu_load_dlls(struct extdisp_gpu *gpu);
static int extdisp_gpu_init_vulkan_instance(struct extdisp_gpu *gpu);
static int extdisp_gpu_select_output(struct extdisp_gpu *gpu);
static int extdisp_gpu_select_vk_device(struct extdisp_gpu *gpu);
static int extdisp_gpu_create_compute_pipeline(struct extdisp_gpu *gpu);
static int extdisp_gpu_create_frame_resources(struct extdisp_gpu *gpu);
static void extdisp_gpu_destroy_frame_resources(struct extdisp_gpu *gpu);
static int extdisp_gpu_find_mem_type(const VkPhysicalDeviceMemoryProperties *props, uint32_t bits, VkMemoryPropertyFlags req, uint32_t *out);
static int extdisp_gpu_create_nv12_image(struct extdisp_gpu *gpu, VkImageUsageFlags usage, VkImage *out_img, VkDeviceMemory *out_mem);
static void extdisp_gpu_update_cursor(struct extdisp_gpu *gpu, const DXGI_OUTDUPL_FRAME_INFO *finfo);
static void extdisp_gpu_composite_cursor(const struct extdisp_gpu *gpu, uint8_t *bgra, uint32_t stride, uint32_t w, uint32_t h);
static int extdisp_gpu_create_staging(struct extdisp_gpu *gpu);
static void extdisp_gpu_destroy_vulkan(struct extdisp_gpu *gpu);
static void extdisp_gpu_release_dxgi(struct extdisp_gpu *gpu);
static int extdisp_wcsistr(const wchar_t *haystack, const wchar_t *needle);
static int extdisp_vdd_inf_path(wchar_t *out, size_t cap);
static int extdisp_vdd_device_exists(HMODULE setupapi, const GUID *cls);

typedef HDEVINFO (WINAPI *pfn_setup_di_get_class_devs_w)(const GUID *, PCWSTR, HWND, DWORD);
typedef BOOL (WINAPI *pfn_setup_di_enum_device_info)(HDEVINFO, DWORD, PSP_DEVINFO_DATA);
typedef BOOL (WINAPI *pfn_setup_di_get_dev_reg_prop_w)(HDEVINFO, PSP_DEVINFO_DATA, DWORD, PDWORD, PBYTE, DWORD, PDWORD);
typedef BOOL (WINAPI *pfn_setup_di_destroy_dev_info_list)(HDEVINFO);
typedef HDEVINFO (WINAPI *pfn_setup_di_create_dev_info_list)(const GUID *, HWND);
typedef BOOL (WINAPI *pfn_setup_di_create_dev_info_w)(HDEVINFO, PCWSTR, const GUID *, PCWSTR, HWND, DWORD, PSP_DEVINFO_DATA);
typedef BOOL (WINAPI *pfn_setup_di_set_dev_reg_prop_w)(HDEVINFO, PSP_DEVINFO_DATA, DWORD, const BYTE *, DWORD);
typedef BOOL (WINAPI *pfn_setup_di_call_class_installer)(DI_FUNCTION, HDEVINFO, PSP_DEVINFO_DATA);
typedef BOOL (WINAPI *pfn_update_driver_pnp_w)(HWND, LPCWSTR, LPCWSTR, DWORD, PBOOL);
typedef BOOL (WINAPI *pfn_di_install_driver_w)(HWND, LPCWSTR, DWORD, PBOOL);

/*
 * Resolves <exe-dir>\vdd\MttVDD.inf into out.  Returns 0 on success.
 */
static int
extdisp_vdd_inf_path(
	wchar_t *out,
	size_t cap)
{
	wchar_t exe[MAX_PATH];
	DWORD len;
	wchar_t *slash;

	len = GetModuleFileNameW(NULL, exe, MAX_PATH);
	if (len == 0U || len >= MAX_PATH)
		return -1;
	slash = wcsrchr(exe, L'\\');
	if (slash == NULL)
		slash = wcsrchr(exe, L'/');
	if (slash == NULL)
		return -1;
	slash[1] = L'\0';
	if (_snwprintf(out, cap, L"%svdd\\MttVDD.inf", exe) < 0)
		return -1;
	out[cap - 1U] = L'\0';
	return 0;
}

/*
 * Checks whether any Root\MttVDD device node is already registered.
 */
static int
extdisp_vdd_device_exists(
	HMODULE setupapi,
	const GUID *cls)
{
	pfn_setup_di_get_class_devs_w get_devs;
	pfn_setup_di_enum_device_info enum_info;
	pfn_setup_di_get_dev_reg_prop_w get_prop;
	pfn_setup_di_destroy_dev_info_list destroy_list;
	HDEVINFO devs;
	SP_DEVINFO_DATA ddata;
	DWORD idx;
	int found = 0;

	get_devs = (pfn_setup_di_get_class_devs_w)(void *)GetProcAddress(setupapi, "SetupDiGetClassDevsW");
	enum_info = (pfn_setup_di_enum_device_info)(void *)GetProcAddress(setupapi, "SetupDiEnumDeviceInfo");
	get_prop = (pfn_setup_di_get_dev_reg_prop_w)(void *)GetProcAddress(setupapi, "SetupDiGetDeviceRegistryPropertyW");
	destroy_list = (pfn_setup_di_destroy_dev_info_list)(void *)GetProcAddress(setupapi, "SetupDiDestroyDeviceInfoList");
	if (get_devs == NULL || enum_info == NULL || get_prop == NULL || destroy_list == NULL)
		return 0;

	devs = get_devs(cls, NULL, NULL, DIGCF_ALLCLASSES);
	if (devs == INVALID_HANDLE_VALUE)
		return 0;

	memset(&ddata, 0, sizeof(ddata));
	ddata.cbSize = sizeof(ddata);
	for (idx = 0U; enum_info(devs, idx, &ddata); idx++) {
		wchar_t hwid[512];
		DWORD reg_type = 0U;
		DWORD req_sz = 0U;
		const wchar_t *p;

		memset(hwid, 0, sizeof(hwid));
		if (!get_prop(devs, &ddata, SPDRP_HARDWAREID, &reg_type,
		    (PBYTE)hwid, (DWORD)(sizeof(hwid) - sizeof(wchar_t)), &req_sz))
			continue;
		for (p = hwid; *p != L'\0'; p += wcslen(p) + 1U) {
			if (_wcsicmp(p, L"Root\\MttVDD") == 0) {
				found = 1;
				break;
			}
		}
		if (found)
			break;
	}
	destroy_list(devs);
	return found;
}

/*
 * Returns 1 if vdd\MttVDD.inf is present next to mikuterm.exe, 0 otherwise.
 */
int
extdisp_vdd_package_present(void)
{
	wchar_t inf_path[MAX_PATH];
	DWORD attrs;

	if (extdisp_vdd_inf_path(inf_path, MAX_PATH) != 0)
		return 0;
	attrs = GetFileAttributesW(inf_path);
	if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0U)
		return 0;
	return 1;
}

/*
 * Elevated --install-vdd entry point: registers Root\MttVDD if not already
 * present and installs vdd\MttVDD.inf using Windows PnP APIs without
 * modifying certificate stores.  Returns 0 on success.
 */
int
extdisp_vdd_install(void)
{
	static const GUID guid_display = {
		0x4d36e968U, 0xe325U, 0x11ceU,
		{ 0xbfU, 0xc1U, 0x08U, 0x00U, 0x2bU, 0xe1U, 0x03U, 0x18U }
	};
	static const wchar_t hw_id[] = L"Root\\MttVDD\0";
	wchar_t inf_path[MAX_PATH];
	HMODULE setupapi = NULL;
	HMODULE newdev = NULL;
	pfn_setup_di_create_dev_info_list create_list;
	pfn_setup_di_create_dev_info_w create_info;
	pfn_setup_di_set_dev_reg_prop_w set_prop;
	pfn_setup_di_call_class_installer call_inst;
	pfn_setup_di_destroy_dev_info_list destroy_list;
	pfn_update_driver_pnp_w update_drv;
	pfn_di_install_driver_w di_install;
	BOOL reboot = FALSE;
	int rc = 1;

	if (extdisp_vdd_inf_path(inf_path, MAX_PATH) != 0 ||
	    GetFileAttributesW(inf_path) == INVALID_FILE_ATTRIBUTES)
		return 1;

	setupapi = LoadLibraryExW(L"setupapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	newdev = LoadLibraryExW(L"newdev.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (setupapi == NULL || newdev == NULL)
		goto out;

	create_list = (pfn_setup_di_create_dev_info_list)(void *)GetProcAddress(setupapi, "SetupDiCreateDeviceInfoList");
	create_info = (pfn_setup_di_create_dev_info_w)(void *)GetProcAddress(setupapi, "SetupDiCreateDeviceInfoW");
	set_prop = (pfn_setup_di_set_dev_reg_prop_w)(void *)GetProcAddress(setupapi, "SetupDiSetDeviceRegistryPropertyW");
	call_inst = (pfn_setup_di_call_class_installer)(void *)GetProcAddress(setupapi, "SetupDiCallClassInstaller");
	destroy_list = (pfn_setup_di_destroy_dev_info_list)(void *)GetProcAddress(setupapi, "SetupDiDestroyDeviceInfoList");
	update_drv = (pfn_update_driver_pnp_w)(void *)GetProcAddress(newdev, "UpdateDriverForPlugAndPlayDevicesW");
	di_install = (pfn_di_install_driver_w)(void *)GetProcAddress(newdev, "DiInstallDriverW");

	if (create_list == NULL || create_info == NULL || set_prop == NULL ||
	    call_inst == NULL || destroy_list == NULL ||
	    (update_drv == NULL && di_install == NULL))
		goto out;

	if (!extdisp_vdd_device_exists(setupapi, &guid_display)) {
		HDEVINFO devs = create_list(&guid_display, NULL);
		SP_DEVINFO_DATA ddata;

		if (devs == INVALID_HANDLE_VALUE)
			goto out;
		memset(&ddata, 0, sizeof(ddata));
		ddata.cbSize = sizeof(ddata);
		if (!create_info(devs, L"Display", &guid_display,
		    L"Virtual Display Driver", NULL, DICD_GENERATE_ID, &ddata) ||
		    !set_prop(devs, &ddata, SPDRP_HARDWAREID,
		    (const BYTE *)hw_id, (DWORD)sizeof(hw_id)) ||
		    !call_inst(DIF_REGISTERDEVICE, devs, &ddata)) {
			destroy_list(devs);
			goto out;
		}
		destroy_list(devs);
	}

	if (update_drv != NULL &&
	    update_drv(NULL, L"Root\\MttVDD", inf_path, INSTALLFLAG_FORCE, &reboot)) {
		rc = 0;
	} else if (di_install != NULL &&
	    di_install(NULL, inf_path, DIIRFLAG_FORCE_INF, &reboot)) {
		rc = 0;
	}

	if (rc == 0)
		(void)WaitNamedPipeW(EXTDISP_GPU_VDD_PIPE, 3000);

out:
	if (newdev != NULL)
		FreeLibrary(newdev);
	if (setupapi != NULL)
		FreeLibrary(setupapi);
	return rc;
}

/*
 * Sends a plaintext command to Virtual-Display-Driver's named pipe if the
 * driver is loaded.  Uses a short timeout so absent drivers fail immediately.
 */
int
extdisp_vdd_pipe_command(
	const char *cmd,
	char *resp,
	size_t resp_cap)
{
	HANDLE pipe;
	DWORD written = 0U;
	DWORD read_bytes = 0U;
	size_t cmd_len;

	if (cmd == NULL)
		return -1;
	if (!WaitNamedPipeW(EXTDISP_GPU_VDD_PIPE, 1000)) {
		mikuterm_log("extdisp-gpu: WaitNamedPipeW failed err=%lu", (unsigned long)GetLastError());
		return -1;
	}

	pipe = CreateFileW(EXTDISP_GPU_VDD_PIPE, GENERIC_READ | GENERIC_WRITE,
	    0, NULL, OPEN_EXISTING, 0, NULL);
	if (pipe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY) {
		if (WaitNamedPipeW(EXTDISP_GPU_VDD_PIPE, 1000)) {
			pipe = CreateFileW(EXTDISP_GPU_VDD_PIPE, GENERIC_READ | GENERIC_WRITE,
			    0, NULL, OPEN_EXISTING, 0, NULL);
		}
	}
	if (pipe == INVALID_HANDLE_VALUE) {
		mikuterm_log("extdisp-gpu: CreateFileW(pipe) failed err=%lu", (unsigned long)GetLastError());
		return -1;
	}

	cmd_len = strlen(cmd);
	if (!WriteFile(pipe, cmd, (DWORD)cmd_len, &written, NULL) || written != (DWORD)cmd_len) {
		CloseHandle(pipe);
		return -1;
	}
	if (resp != NULL && resp_cap > 0U) {
		if (ReadFile(pipe, resp, (DWORD)(resp_cap - 1U), &read_bytes, NULL))
			resp[read_bytes] = '\0';
		else
			resp[0] = '\0';
	}
	CloseHandle(pipe);
	return 0;
}

/*
 * Opens the DXGI-to-Vulkan zero-copy capture and H.264 encode pipeline.
 * Returns 0 and sets *out on success, or -1 when any required capability
 * (DXGI duplication, LUID match, external Win32 memory, or Vulkan Video
 * H.264 encode) is unavailable.
 */
int
extdisp_gpu_open(
	uint32_t pref_width,
	uint32_t pref_height,
	int wait_for_secondary,
	struct extdisp_gpu **out)
{
	struct extdisp_gpu *gpu;
	int attempt;
	int max_attempts = wait_for_secondary ? 10 : 1;
	int output_ok = 0;

	if (out == NULL)
		return -1;
	*out = NULL;

	gpu = (struct extdisp_gpu *)calloc(1U, sizeof(*gpu));
	if (gpu == NULL)
		return -1;

	gpu->pref_width = pref_width;
	gpu->pref_height = pref_height;
	gpu->pkt_cap = EXTDISP_GPU_BITSTREAM_BYTES;
	gpu->pkt_buf = (uint8_t *)malloc(gpu->pkt_cap);
	if (gpu->pkt_buf == NULL) {
		extdisp_gpu_close(gpu);
		return -1;
	}

	if (extdisp_gpu_load_dlls(gpu) != 0) {
		extdisp_gpu_close(gpu);
		return -1;
	}

	for (attempt = 0; attempt < max_attempts; attempt++) {
		if (extdisp_gpu_select_output(gpu) == 0) {
			output_ok = 1;
			if (!wait_for_secondary || gpu->found_vdd)
				break;
		}
		if (attempt + 1 < max_attempts) {
			mikuterm_log("extdisp-gpu: waiting for virtual display attachment (%d/%d)",
			    attempt + 1, max_attempts);
			extdisp_gpu_release_dxgi(gpu);
			output_ok = 0;
			Sleep(150);
		}
	}

	if (!output_ok) {
		mikuterm_log("extdisp-gpu: DXGI output duplication unavailable");
		extdisp_gpu_close(gpu);
		return -1;
	}
	gpu->require_vdd = gpu->found_vdd;

	if (extdisp_gpu_init_vulkan_instance(gpu) == 0 &&
	    extdisp_gpu_select_vk_device(gpu) == 0 &&
	    extdisp_gpu_create_compute_pipeline(gpu) == 0 &&
	    extdisp_gpu_create_frame_resources(gpu) == 0) {
		gpu->zero_copy = 1;
		mikuterm_log("extdisp-gpu: mode=vulkan-video (zero-copy %ux%u)", gpu->width, gpu->height);
		*out = gpu;
		return 0;
	}

	extdisp_gpu_destroy_vulkan(gpu);
	if (extdisp_d3d12_open((IDXGIAdapter *)gpu->adapter, gpu->d3d_dev, gpu->d3d_ctx,
	    gpu->width, gpu->height, gpu->dxgi_format, &gpu->d3d12) == 0) {
		gpu->zero_copy = 1;
		mikuterm_log("extdisp-gpu: mode=d3d12-video (zero-copy %ux%u)", gpu->width, gpu->height);
		*out = gpu;
		return 0;
	}

	if (extdisp_gpu_create_staging(gpu) != 0) {
		mikuterm_log("extdisp-gpu: failed to create D3D11 staging fallback");
		extdisp_gpu_close(gpu);
		return -1;
	}

	gpu->zero_copy = 0;
	mikuterm_log("extdisp-gpu: mode=dxgi-staging (fallback %ux%u)", gpu->width, gpu->height);
	*out = gpu;
	return 0;
}

/*
 * Returns 1 if the pipeline is using zero-copy GPU encode, 0 for DXGI staging.
 */
int
extdisp_gpu_is_zero_copy(
	const struct extdisp_gpu *gpu)
{
	return (gpu != NULL && gpu->zero_copy) ? 1 : 0;
}

/*
 * Returns the active pipeline mode name for status display and logging.
 */
const char *
extdisp_gpu_mode_name(
	const struct extdisp_gpu *gpu)
{
	if (gpu == NULL)
		return "none";
	if (gpu->d3d12 != NULL)
		return "d3d12-video";
	if (gpu->zero_copy)
		return "vulkan-video";
	return "dxgi-staging";
}

/*
 * Rebuilds the DXGI duplication, shared texture, NV12 image, and video session
 * after DXGI_ERROR_ACCESS_LOST or a dynamic display resolution change.
 */
int
extdisp_gpu_rebuild(
	struct extdisp_gpu *gpu)
{
	int attempt;
	int ok = 0;

	if (gpu == NULL)
		return -1;

	if (gpu->device != VK_NULL_HANDLE)
		(void)gpu->vk.vkDeviceWaitIdle(gpu->device);

	extdisp_gpu_destroy_frame_resources(gpu);
	if (gpu->d3d12 != NULL) {
		extdisp_d3d12_close(gpu->d3d12);
		gpu->d3d12 = NULL;
	}
	if (gpu->staging_tex != NULL) {
		gpu->staging_tex->lpVtbl->Release(gpu->staging_tex);
		gpu->staging_tex = NULL;
	}

	extdisp_gpu_release_dxgi(gpu);

	for (attempt = 0; attempt < 2; attempt++) {
		if (extdisp_gpu_select_output(gpu) == 0) {
			if (!gpu->require_vdd || gpu->found_vdd) {
				ok = 1;
				break;
			}
			mikuterm_log("extdisp-gpu: rebuild waiting for VDD output (%d/2)", attempt + 1);
		}
		extdisp_gpu_release_dxgi(gpu);
		if (gpu->require_vdd && attempt == 0)
			(void)extdisp_vdd_pipe_command("SETDISPLAYCOUNT 1\n", NULL, 0U);
		Sleep(100);
	}
	if (!ok)
		return -1;

	if (gpu->zero_copy && gpu->device != VK_NULL_HANDLE) {
		if (extdisp_gpu_create_frame_resources(gpu) != 0)
			return -1;
	} else if (gpu->zero_copy) {
		if (extdisp_d3d12_open((IDXGIAdapter *)gpu->adapter, gpu->d3d_dev, gpu->d3d_ctx,
		    gpu->width, gpu->height, gpu->dxgi_format, &gpu->d3d12) != 0) {
			gpu->zero_copy = 0;
			if (extdisp_gpu_create_staging(gpu) != 0)
				return -1;
		}
	} else {
		if (extdisp_gpu_create_staging(gpu) != 0)
			return -1;
	}

	gpu->cur_dpb = 0U;
	gpu->prev_dpb = 0U;
	gpu->frame_num = 0U;
	gpu->idr_pic_id = 0U;
	gpu->frames_since_idr = 0U;
	gpu->session_reset = 0;

	mikuterm_log("extdisp-gpu: rebuilt pipeline %ux%u (zero_copy=%d)",
	    gpu->width, gpu->height, gpu->zero_copy);
	return 0;
}

/*
 * Reports the active capture dimensions for the duplicated output.
 */
void
extdisp_gpu_dimensions(
	const struct extdisp_gpu *gpu,
	uint32_t *out_w,
	uint32_t *out_h)
{
	if (gpu == NULL)
		return;
	if (out_w != NULL)
		*out_w = gpu->width;
	if (out_h != NULL)
		*out_h = gpu->height;
}

/*
 * Reports the captured output's desktop rectangle in virtual-screen coordinates.
 */
int
extdisp_gpu_desktop_rect(
	const struct extdisp_gpu *gpu,
	int *left,
	int *top,
	int *right,
	int *bottom)
{
	if (gpu == NULL || gpu->desktop_rect.right <= gpu->desktop_rect.left ||
	    gpu->desktop_rect.bottom <= gpu->desktop_rect.top)
		return -1;
	if (left != NULL)
		*left = (int)gpu->desktop_rect.left;
	if (top != NULL)
		*top = (int)gpu->desktop_rect.top;
	if (right != NULL)
		*right = (int)gpu->desktop_rect.right;
	if (bottom != NULL)
		*bottom = (int)gpu->desktop_rect.bottom;
	return 0;
}

/*
 * Captures one frame from DXGI Desktop Duplication, converts it to NV12 on
 * the GPU via compute shader, and encodes an H.264 Annex-B access unit.
 */
int
extdisp_gpu_capture_encode(
	struct extdisp_gpu *gpu,
	int force_key,
	const uint8_t **out_data,
	size_t *out_size,
	int *out_is_key)
{
	DXGI_OUTDUPL_FRAME_INFO finfo;
	IDXGIResource *desktop_res = NULL;
	ID3D11Texture2D *desktop_tex = NULL;
	D3D11_TEXTURE2D_DESC tdesc;
	HRESULT hr;
	VkCommandBufferBeginInfo binfo;
	VkImageMemoryBarrier bar;
	struct extdisp_compute_pc pc;
	VkSubmitInfo sub;
	VkWin32KeyedMutexAcquireReleaseInfoKHR km_info;
	uint64_t acq_key = 1ULL;
	uint64_t rel_key = 0ULL;
	uint32_t acq_timeout = 100U;
	VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	VkVideoPictureResourceInfoKHR dpb_res[2];
	StdVideoEncodeH264ReferenceInfo std_setup_ref;
	StdVideoEncodeH264ReferenceInfo std_l0_ref;
	VkVideoEncodeH264DpbSlotInfoKHR setup_dpb;
	VkVideoEncodeH264DpbSlotInfoKHR l0_dpb;
	VkVideoReferenceSlotInfoKHR slots[2];
	VkVideoBeginCodingInfoKHR begin_info;
	VkVideoPictureResourceInfoKHR src_res;
	StdVideoEncodeH264PictureInfo std_pic;
	StdVideoEncodeH264SliceHeader std_slice;
	VkVideoEncodeH264NaluSliceInfoKHR slice_info;
	VkVideoEncodeH264PictureInfoKHR h264_pic;
	VkVideoEncodeInfoKHR enc_info;
	VkVideoEndCodingInfoKHR end_info;
	uint32_t fb_vals[3];
	uint32_t bs_offset;
	uint32_t bs_bytes;
	int is_idr;
	VkResult res;

	if (gpu == NULL || (!gpu->resources_ready && gpu->d3d12 == NULL) ||
	    out_data == NULL || out_size == NULL)
		return EXTDISP_GPU_ERROR;

	*out_data = NULL;
	*out_size = 0U;
	if (out_is_key != NULL)
		*out_is_key = 0;

	memset(&finfo, 0, sizeof(finfo));
	hr = gpu->dupl->lpVtbl->AcquireNextFrame(gpu->dupl, 16, &finfo, &desktop_res);
	if (hr == DXGI_ERROR_WAIT_TIMEOUT)
		return EXTDISP_GPU_IDLE;
	if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL)
		return EXTDISP_GPU_REBUILD;
	if (FAILED(hr) || desktop_res == NULL)
		return EXTDISP_GPU_ERROR;

	if (finfo.LastMouseUpdateTime.QuadPart != 0)
		extdisp_gpu_update_cursor(gpu, &finfo);

	if (finfo.AccumulatedFrames == 0U && finfo.LastMouseUpdateTime.QuadPart == 0) {
		desktop_res->lpVtbl->Release(desktop_res);
		(void)gpu->dupl->lpVtbl->ReleaseFrame(gpu->dupl);
		return EXTDISP_GPU_IDLE;
	}

	hr = desktop_res->lpVtbl->QueryInterface(desktop_res, &IID_ID3D11Texture2D, (void **)&desktop_tex);
	desktop_res->lpVtbl->Release(desktop_res);
	if (FAILED(hr) || desktop_tex == NULL) {
		(void)gpu->dupl->lpVtbl->ReleaseFrame(gpu->dupl);
		return EXTDISP_GPU_ERROR;
	}

	memset(&tdesc, 0, sizeof(tdesc));
	desktop_tex->lpVtbl->GetDesc(desktop_tex, &tdesc);
	if (tdesc.Width != gpu->width || tdesc.Height != gpu->height || tdesc.Format != gpu->dxgi_format) {
		desktop_tex->lpVtbl->Release(desktop_tex);
		(void)gpu->dupl->lpVtbl->ReleaseFrame(gpu->dupl);
		return EXTDISP_GPU_REBUILD;
	}

	if (gpu->d3d12 != NULL) {
		int rc12 = extdisp_d3d12_encode_texture(
		    gpu->d3d12, desktop_tex,
		    gpu->cursor_visible, gpu->cursor_x, gpu->cursor_y,
		    gpu->cursor_info.Type, gpu->cursor_info.Width, gpu->cursor_info.Height,
		    gpu->cursor_info.Pitch, gpu->cursor_shape,
		    force_key, out_data, out_size, out_is_key);
		desktop_tex->lpVtbl->Release(desktop_tex);
		(void)gpu->dupl->lpVtbl->ReleaseFrame(gpu->dupl);
		return (rc12 == 0) ? EXTDISP_GPU_OK : EXTDISP_GPU_ERROR;
	}

	hr = gpu->keyed_mutex->lpVtbl->AcquireSync(gpu->keyed_mutex, 0ULL, 100);
	if (FAILED(hr)) {
		desktop_tex->lpVtbl->Release(desktop_tex);
		(void)gpu->dupl->lpVtbl->ReleaseFrame(gpu->dupl);
		return EXTDISP_GPU_REBUILD;
	}
	gpu->d3d_ctx->lpVtbl->CopyResource(gpu->d3d_ctx, (ID3D11Resource *)gpu->shared_tex,
	    (ID3D11Resource *)desktop_tex);
	(void)gpu->keyed_mutex->lpVtbl->ReleaseSync(gpu->keyed_mutex, 1ULL);
	desktop_tex->lpVtbl->Release(desktop_tex);
	(void)gpu->dupl->lpVtbl->ReleaseFrame(gpu->dupl);

	memset(&binfo, 0, sizeof(binfo));
	binfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	binfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (gpu->vk.vkBeginCommandBuffer(gpu->compute_cmd, &binfo) != VK_SUCCESS)
		return EXTDISP_GPU_ERROR;

	memset(&bar, 0, sizeof(bar));
	bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	bar.srcAccessMask = 0U;
	bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	bar.image = gpu->src_image;
	bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	bar.subresourceRange.levelCount = 1U;
	bar.subresourceRange.layerCount = 1U;
	gpu->vk.vkCmdPipelineBarrier(gpu->compute_cmd,
	    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	    0U, 0U, NULL, 0U, NULL, 1U, &bar);

	bar.srcAccessMask = 0U;
	bar.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	bar.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	bar.image = gpu->nv12_image;
	gpu->vk.vkCmdPipelineBarrier(gpu->compute_cmd,
	    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	    0U, 0U, NULL, 0U, NULL, 1U, &bar);

	gpu->vk.vkCmdBindPipeline(gpu->compute_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gpu->compute_pipe);
	gpu->vk.vkCmdBindDescriptorSets(gpu->compute_cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
	    gpu->pipe_layout, 0U, 1U, &gpu->desc_set, 0U, NULL);
	pc.visible_w = gpu->width;
	pc.visible_h = gpu->height;
	pc.coded_w = gpu->coded_w;
	pc.coded_h = gpu->coded_h;
	gpu->vk.vkCmdPushConstants(gpu->compute_cmd, gpu->pipe_layout,
	    VK_SHADER_STAGE_COMPUTE_BIT, 0U, (uint32_t)sizeof(pc), &pc);
	gpu->vk.vkCmdDispatch(gpu->compute_cmd, (gpu->coded_w + 15U) / 16U,
	    (gpu->coded_h + 15U) / 16U, 1U);

	bar.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	bar.dstAccessMask = 0U;
	bar.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	bar.newLayout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR;
	bar.srcQueueFamilyIndex = gpu->compute_family;
	bar.dstQueueFamilyIndex = gpu->encode_family;
	bar.image = gpu->nv12_image;
	gpu->vk.vkCmdPipelineBarrier(gpu->compute_cmd,
	    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
	    0U, 0U, NULL, 0U, NULL, 1U, &bar);

	if (gpu->vk.vkEndCommandBuffer(gpu->compute_cmd) != VK_SUCCESS)
		return EXTDISP_GPU_ERROR;

	memset(&km_info, 0, sizeof(km_info));
	km_info.sType = VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR;
	km_info.acquireCount = 1U;
	km_info.pAcquireSyncs = &gpu->src_mem;
	km_info.pAcquireKeys = &acq_key;
	km_info.pAcquireTimeouts = &acq_timeout;
	km_info.releaseCount = 1U;
	km_info.pReleaseSyncs = &gpu->src_mem;
	km_info.pReleaseKeys = &rel_key;

	memset(&sub, 0, sizeof(sub));
	sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	sub.pNext = &km_info;
	sub.commandBufferCount = 1U;
	sub.pCommandBuffers = &gpu->compute_cmd;
	sub.signalSemaphoreCount = 1U;
	sub.pSignalSemaphores = &gpu->compute_done;
	res = gpu->vk.vkQueueSubmit(gpu->compute_queue, 1U, &sub, VK_NULL_HANDLE);
	if (res != VK_SUCCESS)
		return EXTDISP_GPU_ERROR;

	is_idr = force_key || !gpu->session_reset || gpu->frames_since_idr >= EXTDISP_GPU_KEY_INTERVAL;
	if (is_idr) {
		gpu->frame_num = 0U;
		gpu->idr_pic_id = (gpu->idr_pic_id + 1U) & 0xFFFFU;
	}

	if (gpu->vk.vkBeginCommandBuffer(gpu->encode_cmd, &binfo) != VK_SUCCESS)
		return EXTDISP_GPU_ERROR;

	if (gpu->compute_family != gpu->encode_family) {
		bar.srcAccessMask = 0U;
		bar.dstAccessMask = VK_ACCESS_NONE;
		bar.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		bar.newLayout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR;
		bar.srcQueueFamilyIndex = gpu->compute_family;
		bar.dstQueueFamilyIndex = gpu->encode_family;
		bar.image = gpu->nv12_image;
		gpu->vk.vkCmdPipelineBarrier(gpu->encode_cmd,
		    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		    0U, 0U, NULL, 0U, NULL, 1U, &bar);
	}

	gpu->vk.vkCmdResetQueryPool(gpu->encode_cmd, gpu->query_pool, 0U, 1U);

	memset(dpb_res, 0, sizeof(dpb_res));
	dpb_res[0].sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
	dpb_res[0].codedExtent.width = gpu->coded_w;
	dpb_res[0].codedExtent.height = gpu->coded_h;
	dpb_res[0].imageViewBinding = gpu->dpb_view[gpu->cur_dpb];

	dpb_res[1].sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
	dpb_res[1].codedExtent.width = gpu->coded_w;
	dpb_res[1].codedExtent.height = gpu->coded_h;
	dpb_res[1].imageViewBinding = gpu->dpb_view[gpu->prev_dpb];

	memset(&std_setup_ref, 0, sizeof(std_setup_ref));
	std_setup_ref.FrameNum = gpu->frame_num;
	std_setup_ref.PicOrderCnt = (int32_t)(gpu->frame_num * 2U);

	memset(&setup_dpb, 0, sizeof(setup_dpb));
	setup_dpb.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR;
	setup_dpb.pStdReferenceInfo = &std_setup_ref;

	memset(&std_l0_ref, 0, sizeof(std_l0_ref));
	std_l0_ref.FrameNum = (gpu->frame_num > 0U) ? (gpu->frame_num - 1U) : 0U;
	std_l0_ref.PicOrderCnt = (int32_t)(std_l0_ref.FrameNum * 2U);

	memset(&l0_dpb, 0, sizeof(l0_dpb));
	l0_dpb.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR;
	l0_dpb.pStdReferenceInfo = &std_l0_ref;

	memset(slots, 0, sizeof(slots));
	slots[0].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
	slots[0].pNext = &setup_dpb;
	slots[0].slotIndex = (int32_t)gpu->cur_dpb;
	slots[0].pPictureResource = &dpb_res[0];

	slots[1].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
	slots[1].pNext = &l0_dpb;
	slots[1].slotIndex = (int32_t)gpu->prev_dpb;
	slots[1].pPictureResource = &dpb_res[1];

	memset(&begin_info, 0, sizeof(begin_info));
	begin_info.sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
	begin_info.videoSession = gpu->session;
	begin_info.videoSessionParameters = gpu->parameters;
	begin_info.referenceSlotCount = is_idr ? 1U : 2U;
	begin_info.pReferenceSlots = slots;
	gpu->vk.vkCmdBeginVideoCodingKHR(gpu->encode_cmd, &begin_info);

	if (!gpu->session_reset) {
		VkVideoCodingControlInfoKHR ctrl_info;

		memset(&ctrl_info, 0, sizeof(ctrl_info));
		ctrl_info.sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR;
		ctrl_info.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
		gpu->vk.vkCmdControlVideoCodingKHR(gpu->encode_cmd, &ctrl_info);
		gpu->session_reset = 1;
	}

	gpu->vk.vkCmdBeginQuery(gpu->encode_cmd, gpu->query_pool, 0U, 0U);

	memset(&src_res, 0, sizeof(src_res));
	src_res.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
	src_res.codedExtent.width = gpu->coded_w;
	src_res.codedExtent.height = gpu->coded_h;
	src_res.imageViewBinding = gpu->nv12_enc_view;

	memset(&std_pic, 0, sizeof(std_pic));
	std_pic.primary_pic_type = is_idr ?
	    STD_VIDEO_H264_PICTURE_TYPE_IDR : STD_VIDEO_H264_PICTURE_TYPE_P;
	std_pic.frame_num = gpu->frame_num;
	std_pic.idr_pic_id = (uint16_t)gpu->idr_pic_id;
	std_pic.PicOrderCnt = (int32_t)(gpu->frame_num * 2U);
	std_pic.flags.IdrPicFlag = is_idr ? 1U : 0U;
	std_pic.flags.is_reference = 1U;

	memset(&std_slice, 0, sizeof(std_slice));
	std_slice.slice_type = is_idr ?
	    STD_VIDEO_H264_SLICE_TYPE_I : STD_VIDEO_H264_SLICE_TYPE_P;

	memset(&slice_info, 0, sizeof(slice_info));
	slice_info.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_NALU_SLICE_INFO_KHR;
	slice_info.constantQp = (int32_t)(is_idr ?
	    (EXTDISP_GPU_DEFAULT_QP - EXTDISP_GPU_KEY_QP_DELTA) : EXTDISP_GPU_DEFAULT_QP);
	slice_info.pStdSliceHeader = &std_slice;

	memset(&h264_pic, 0, sizeof(h264_pic));
	h264_pic.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PICTURE_INFO_KHR;
	h264_pic.naluSliceEntryCount = 1U;
	h264_pic.pNaluSliceEntries = &slice_info;
	h264_pic.pStdPictureInfo = &std_pic;

	memset(&enc_info, 0, sizeof(enc_info));
	enc_info.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR;
	enc_info.pNext = &h264_pic;
	enc_info.dstBuffer = gpu->out_buf;
	enc_info.dstBufferOffset = 0;
	enc_info.dstBufferRange = EXTDISP_GPU_BITSTREAM_BYTES;
	enc_info.srcPictureResource = src_res;
	enc_info.pSetupReferenceSlot = &slots[0];
	enc_info.referenceSlotCount = is_idr ? 0U : 1U;
	enc_info.pReferenceSlots = is_idr ? NULL : &slots[1];
	gpu->vk.vkCmdEncodeVideoKHR(gpu->encode_cmd, &enc_info);

	gpu->vk.vkCmdEndQuery(gpu->encode_cmd, gpu->query_pool, 0U);

	memset(&end_info, 0, sizeof(end_info));
	end_info.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR;
	gpu->vk.vkCmdEndVideoCodingKHR(gpu->encode_cmd, &end_info);

	if (gpu->vk.vkEndCommandBuffer(gpu->encode_cmd) != VK_SUCCESS)
		return EXTDISP_GPU_ERROR;

	(void)gpu->vk.vkResetFences(gpu->device, 1U, &gpu->encode_fence);
	memset(&sub, 0, sizeof(sub));
	sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	sub.waitSemaphoreCount = 1U;
	sub.pWaitSemaphores = &gpu->compute_done;
	sub.pWaitDstStageMask = &wait_stage;
	sub.commandBufferCount = 1U;
	sub.pCommandBuffers = &gpu->encode_cmd;
	if (gpu->vk.vkQueueSubmit(gpu->encode_queue, 1U, &sub, gpu->encode_fence) != VK_SUCCESS)
		return EXTDISP_GPU_ERROR;

	if (gpu->vk.vkWaitForFences(gpu->device, 1U, &gpu->encode_fence, VK_TRUE, 5000000000ULL) != VK_SUCCESS)
		return EXTDISP_GPU_ERROR;

	memset(fb_vals, 0, sizeof(fb_vals));
	res = gpu->vk.vkGetQueryPoolResults(gpu->device, gpu->query_pool, 0U, 1U,
	    sizeof(fb_vals), fb_vals, sizeof(fb_vals),
	    VK_QUERY_RESULT_WAIT_BIT | VK_QUERY_RESULT_WITH_STATUS_BIT_KHR);
	if (res != VK_SUCCESS || fb_vals[2] == 0U)
		return EXTDISP_GPU_ERROR;

	bs_offset = fb_vals[0];
	bs_bytes = fb_vals[1];
	if (bs_bytes == 0U || (size_t)bs_offset + (size_t)bs_bytes > EXTDISP_GPU_BITSTREAM_BYTES)
		return EXTDISP_GPU_ERROR;

	if (is_idr && gpu->ps_buf != NULL && gpu->ps_len + (size_t)bs_bytes <= gpu->pkt_cap) {
		memcpy(gpu->pkt_buf, gpu->ps_buf, gpu->ps_len);
		memcpy(gpu->pkt_buf + gpu->ps_len, (const uint8_t *)gpu->out_map + bs_offset, bs_bytes);
		*out_data = gpu->pkt_buf;
		*out_size = gpu->ps_len + (size_t)bs_bytes;
	} else if ((size_t)bs_bytes <= gpu->pkt_cap) {
		memcpy(gpu->pkt_buf, (const uint8_t *)gpu->out_map + bs_offset, bs_bytes);
		*out_data = gpu->pkt_buf;
		*out_size = (size_t)bs_bytes;
	} else {
		return EXTDISP_GPU_ERROR;
	}

	if (out_is_key != NULL)
		*out_is_key = is_idr;

	gpu->prev_dpb = gpu->cur_dpb;
	gpu->cur_dpb ^= 1U;
	gpu->frame_num = (gpu->frame_num + 1U) & 15U;
	gpu->frames_since_idr = is_idr ? 1U : (gpu->frames_since_idr + 1U);
	return EXTDISP_GPU_OK;
}

/*
 * Captures one frame from DXGI Desktop Duplication into dst_bgra via a
 * D3D11 staging texture for CPU-side encoding fallback.
 */
int
extdisp_gpu_capture_bgra(
	struct extdisp_gpu *gpu,
	uint8_t *dst_bgra,
	uint32_t dst_stride,
	uint32_t dst_w,
	uint32_t dst_h)
{
	DXGI_OUTDUPL_FRAME_INFO finfo;
	IDXGIResource *desktop_res = NULL;
	ID3D11Texture2D *desktop_tex = NULL;
	D3D11_TEXTURE2D_DESC tdesc;
	D3D11_MAPPED_SUBRESOURCE mapped;
	HRESULT hr;
	uint32_t copy_w;
	uint32_t copy_h;
	uint32_t y;

	if (gpu == NULL || gpu->dupl == NULL || gpu->staging_tex == NULL ||
	    dst_bgra == NULL || dst_w == 0U || dst_h == 0U)
		return EXTDISP_GPU_ERROR;

	memset(&finfo, 0, sizeof(finfo));
	hr = gpu->dupl->lpVtbl->AcquireNextFrame(gpu->dupl, 16, &finfo, &desktop_res);
	if (hr == DXGI_ERROR_WAIT_TIMEOUT)
		return EXTDISP_GPU_IDLE;
	if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
		mikuterm_log("extdisp-gpu: AcquireNextFrame hr=0x%08lx (rebuild)", (unsigned long)hr);
		return EXTDISP_GPU_REBUILD;
	}
	if (FAILED(hr) || desktop_res == NULL) {
		mikuterm_log("extdisp-gpu: AcquireNextFrame failed hr=0x%08lx", (unsigned long)hr);
		return EXTDISP_GPU_ERROR;
	}

	if (finfo.LastMouseUpdateTime.QuadPart != 0)
		extdisp_gpu_update_cursor(gpu, &finfo);

	if (finfo.AccumulatedFrames == 0U && finfo.LastMouseUpdateTime.QuadPart == 0) {
		desktop_res->lpVtbl->Release(desktop_res);
		(void)gpu->dupl->lpVtbl->ReleaseFrame(gpu->dupl);
		return EXTDISP_GPU_IDLE;
	}

	hr = desktop_res->lpVtbl->QueryInterface(desktop_res, &IID_ID3D11Texture2D, (void **)&desktop_tex);
	desktop_res->lpVtbl->Release(desktop_res);
	if (FAILED(hr) || desktop_tex == NULL) {
		(void)gpu->dupl->lpVtbl->ReleaseFrame(gpu->dupl);
		return EXTDISP_GPU_ERROR;
	}

	memset(&tdesc, 0, sizeof(tdesc));
	desktop_tex->lpVtbl->GetDesc(desktop_tex, &tdesc);
	if (tdesc.Width != gpu->width || tdesc.Height != gpu->height || tdesc.Format != gpu->dxgi_format) {
		desktop_tex->lpVtbl->Release(desktop_tex);
		(void)gpu->dupl->lpVtbl->ReleaseFrame(gpu->dupl);
		return EXTDISP_GPU_REBUILD;
	}

	gpu->d3d_ctx->lpVtbl->CopyResource(gpu->d3d_ctx, (ID3D11Resource *)gpu->staging_tex,
	    (ID3D11Resource *)desktop_tex);
	desktop_tex->lpVtbl->Release(desktop_tex);
	(void)gpu->dupl->lpVtbl->ReleaseFrame(gpu->dupl);

	memset(&mapped, 0, sizeof(mapped));
	hr = gpu->d3d_ctx->lpVtbl->Map(gpu->d3d_ctx, (ID3D11Resource *)gpu->staging_tex,
	    0U, D3D11_MAP_READ, 0U, &mapped);
	if (FAILED(hr) || mapped.pData == NULL) {
		mikuterm_log("extdisp-gpu: staging Map failed hr=0x%08lx", (unsigned long)hr);
		return EXTDISP_GPU_ERROR;
	}

	copy_w = (gpu->width < dst_w) ? gpu->width : dst_w;
	copy_h = (gpu->height < dst_h) ? gpu->height : dst_h;
	if (gpu->dxgi_format == DXGI_FORMAT_R10G10B10A2_UNORM) {
		uint32_t x;

		for (y = 0U; y < copy_h; y++) {
			const uint32_t *src = (const uint32_t *)((const uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch);
			uint8_t *dst = dst_bgra + (size_t)y * dst_stride;

			for (x = 0U; x < copy_w; x++) {
				uint32_t p = src[x];
				dst[x * 4U + 0U] = (uint8_t)((p >> 22) & 0xFFU);
				dst[x * 4U + 1U] = (uint8_t)((p >> 12) & 0xFFU);
				dst[x * 4U + 2U] = (uint8_t)((p >> 2) & 0xFFU);
				dst[x * 4U + 3U] = 0xFFU;
			}
		}
	} else {
		for (y = 0U; y < copy_h; y++) {
			const uint8_t *src = (const uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch;
			uint8_t *dst = dst_bgra + (size_t)y * dst_stride;
			memcpy(dst, src, (size_t)copy_w * 4U);
		}
	}

	gpu->d3d_ctx->lpVtbl->Unmap(gpu->d3d_ctx, (ID3D11Resource *)gpu->staging_tex, 0U);
	extdisp_gpu_composite_cursor(gpu, dst_bgra, dst_stride, dst_w, dst_h);
	return EXTDISP_GPU_OK;
}

static void
extdisp_gpu_update_cursor(
	struct extdisp_gpu *gpu,
	const DXGI_OUTDUPL_FRAME_INFO *finfo)
{
	UINT req_sz = 0U;
	HRESULT hr;

	gpu->cursor_visible = finfo->PointerPosition.Visible ? 1 : 0;
	gpu->cursor_x = (int)finfo->PointerPosition.Position.x;
	gpu->cursor_y = (int)finfo->PointerPosition.Position.y;

	if (finfo->PointerShapeBufferSize > 0U) {
		if (finfo->PointerShapeBufferSize > gpu->cursor_shape_cap) {
			uint8_t *nb = (uint8_t *)realloc(gpu->cursor_shape, finfo->PointerShapeBufferSize);
			if (nb == NULL)
				return;
			gpu->cursor_shape = nb;
			gpu->cursor_shape_cap = finfo->PointerShapeBufferSize;
		}
		hr = gpu->dupl->lpVtbl->GetFramePointerShape(gpu->dupl,
		    finfo->PointerShapeBufferSize, gpu->cursor_shape, &req_sz, &gpu->cursor_info);
		if (FAILED(hr))
			gpu->cursor_visible = 0;
	}
}

static void
extdisp_gpu_composite_cursor(
	const struct extdisp_gpu *gpu,
	uint8_t *bgra,
	uint32_t stride,
	uint32_t w,
	uint32_t h)
{
	uint32_t cw;
	uint32_t ch;
	uint32_t vis_h;
	uint32_t sy;
	uint32_t sx;

	if (!gpu->cursor_visible || gpu->cursor_shape == NULL)
		return;

	cw = gpu->cursor_info.Width;
	ch = gpu->cursor_info.Height;
	if (cw == 0U || ch == 0U)
		return;
	vis_h = (gpu->cursor_info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) ? (ch / 2U) : ch;

	for (sy = 0U; sy < vis_h; sy++) {
		int dy = gpu->cursor_y + (int)sy;
		uint8_t *dst_row;
		if (dy < 0 || (uint32_t)dy >= h)
			continue;
		dst_row = bgra + (size_t)dy * stride;
		for (sx = 0U; sx < cw; sx++) {
			int dx = gpu->cursor_x + (int)sx;
			uint8_t *px;
			if (dx < 0 || (uint32_t)dx >= w)
				continue;
			px = dst_row + (size_t)dx * 4U;
			if (gpu->cursor_info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
				uint8_t bit = (uint8_t)(0x80U >> (sx & 7U));
				uint8_t and_b = (gpu->cursor_shape[(size_t)sy * gpu->cursor_info.Pitch + (sx >> 3U)] & bit) ? 0xFFU : 0x00U;
				uint8_t xor_b = (gpu->cursor_shape[(size_t)(sy + vis_h) * gpu->cursor_info.Pitch + (sx >> 3U)] & bit) ? 0xFFU : 0x00U;
				px[0] = (px[0] & and_b) ^ xor_b;
				px[1] = (px[1] & and_b) ^ xor_b;
				px[2] = (px[2] & and_b) ^ xor_b;
			} else if (gpu->cursor_info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
				const uint8_t *src = gpu->cursor_shape + (size_t)sy * gpu->cursor_info.Pitch + (size_t)sx * 4U;
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
			} else if (gpu->cursor_info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
				const uint8_t *src = gpu->cursor_shape + (size_t)sy * gpu->cursor_info.Pitch + (size_t)sx * 4U;
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

static int
extdisp_gpu_create_staging(
	struct extdisp_gpu *gpu)
{
	D3D11_TEXTURE2D_DESC tdesc;
	HRESULT hr;

	memset(&tdesc, 0, sizeof(tdesc));
	tdesc.Width = gpu->width;
	tdesc.Height = gpu->height;
	tdesc.MipLevels = 1U;
	tdesc.ArraySize = 1U;
	tdesc.Format = gpu->dxgi_format;
	tdesc.SampleDesc.Count = 1U;
	tdesc.Usage = D3D11_USAGE_STAGING;
	tdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

	hr = gpu->d3d_dev->lpVtbl->CreateTexture2D(gpu->d3d_dev, &tdesc, NULL, &gpu->staging_tex);
	if (FAILED(hr) || gpu->staging_tex == NULL) {
		mikuterm_log("extdisp-gpu: CreateTexture2D(staging) failed hr=0x%08lx", (unsigned long)hr);
		return -1;
	}
	return 0;
}

static void
extdisp_gpu_destroy_vulkan(
	struct extdisp_gpu *gpu)
{
	if (gpu->device != VK_NULL_HANDLE)
		(void)gpu->vk.vkDeviceWaitIdle(gpu->device);

	extdisp_gpu_destroy_frame_resources(gpu);

	if (gpu->compute_pipe != VK_NULL_HANDLE) {
		gpu->vk.vkDestroyPipeline(gpu->device, gpu->compute_pipe, NULL);
		gpu->compute_pipe = VK_NULL_HANDLE;
	}
	if (gpu->pipe_layout != VK_NULL_HANDLE) {
		gpu->vk.vkDestroyPipelineLayout(gpu->device, gpu->pipe_layout, NULL);
		gpu->pipe_layout = VK_NULL_HANDLE;
	}
	if (gpu->desc_pool != VK_NULL_HANDLE) {
		gpu->vk.vkDestroyDescriptorPool(gpu->device, gpu->desc_pool, NULL);
		gpu->desc_pool = VK_NULL_HANDLE;
	}
	if (gpu->desc_layout != VK_NULL_HANDLE) {
		gpu->vk.vkDestroyDescriptorSetLayout(gpu->device, gpu->desc_layout, NULL);
		gpu->desc_layout = VK_NULL_HANDLE;
	}
	if (gpu->shader != VK_NULL_HANDLE) {
		gpu->vk.vkDestroyShaderModule(gpu->device, gpu->shader, NULL);
		gpu->shader = VK_NULL_HANDLE;
	}
	if (gpu->src_sampler != VK_NULL_HANDLE) {
		gpu->vk.vkDestroySampler(gpu->device, gpu->src_sampler, NULL);
		gpu->src_sampler = VK_NULL_HANDLE;
	}
	if (gpu->query_pool != VK_NULL_HANDLE) {
		gpu->vk.vkDestroyQueryPool(gpu->device, gpu->query_pool, NULL);
		gpu->query_pool = VK_NULL_HANDLE;
	}
	if (gpu->encode_fence != VK_NULL_HANDLE) {
		gpu->vk.vkDestroyFence(gpu->device, gpu->encode_fence, NULL);
		gpu->encode_fence = VK_NULL_HANDLE;
	}
	if (gpu->compute_done != VK_NULL_HANDLE) {
		gpu->vk.vkDestroySemaphore(gpu->device, gpu->compute_done, NULL);
		gpu->compute_done = VK_NULL_HANDLE;
	}
	if (gpu->encode_pool != VK_NULL_HANDLE && gpu->encode_pool != gpu->compute_pool) {
		gpu->vk.vkDestroyCommandPool(gpu->device, gpu->encode_pool, NULL);
		gpu->encode_pool = VK_NULL_HANDLE;
	}
	if (gpu->compute_pool != VK_NULL_HANDLE) {
		gpu->vk.vkDestroyCommandPool(gpu->device, gpu->compute_pool, NULL);
		gpu->compute_pool = VK_NULL_HANDLE;
		gpu->encode_pool = VK_NULL_HANDLE;
	}
	if (gpu->device != VK_NULL_HANDLE) {
		gpu->vk.vkDestroyDevice(gpu->device, NULL);
		gpu->device = VK_NULL_HANDLE;
	}
	if (gpu->instance != VK_NULL_HANDLE) {
		gpu->vk.vkDestroyInstance(gpu->instance, NULL);
		gpu->instance = VK_NULL_HANDLE;
	}
}

/*
 * Releases all D3D11, DXGI, and Vulkan resources held by the GPU pipeline.
 */
void
extdisp_gpu_close(
	struct extdisp_gpu *gpu)
{
	if (gpu == NULL)
		return;

	extdisp_gpu_destroy_vulkan(gpu);

	if (gpu->d3d12 != NULL) {
		extdisp_d3d12_close(gpu->d3d12);
		gpu->d3d12 = NULL;
	}
	if (gpu->staging_tex != NULL)
		gpu->staging_tex->lpVtbl->Release(gpu->staging_tex);
	if (gpu->dupl != NULL)
		gpu->dupl->lpVtbl->Release(gpu->dupl);
	if (gpu->d3d_ctx != NULL)
		gpu->d3d_ctx->lpVtbl->Release(gpu->d3d_ctx);
	if (gpu->d3d_dev != NULL)
		gpu->d3d_dev->lpVtbl->Release(gpu->d3d_dev);
	if (gpu->output1 != NULL)
		gpu->output1->lpVtbl->Release(gpu->output1);
	if (gpu->adapter != NULL)
		gpu->adapter->lpVtbl->Release(gpu->adapter);
	if (gpu->factory != NULL)
		gpu->factory->lpVtbl->Release(gpu->factory);

	if (gpu->vulkan_dll != NULL)
		FreeLibrary(gpu->vulkan_dll);
	if (gpu->d3d11_dll != NULL)
		FreeLibrary(gpu->d3d11_dll);
	if (gpu->dxgi_dll != NULL)
		FreeLibrary(gpu->dxgi_dll);

	free(gpu->cursor_shape);
	free(gpu->pkt_buf);
	free(gpu);
}

static int
extdisp_gpu_load_dlls(
	struct extdisp_gpu *gpu)
{
	gpu->dxgi_dll = LoadLibraryExW(L"dxgi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	gpu->d3d11_dll = LoadLibraryExW(L"d3d11.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	gpu->vulkan_dll = LoadLibraryExW(L"vulkan-1.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (gpu->dxgi_dll == NULL || gpu->d3d11_dll == NULL) {
		mikuterm_log("extdisp-gpu: LoadLibrary failed dxgi=%p d3d11=%p err=%lu",
		    (void *)gpu->dxgi_dll, (void *)gpu->d3d11_dll, (unsigned long)GetLastError());
		return -1;
	}

	gpu->create_dxgi_factory1 = (pfn_create_dxgi_factory1)(void *)GetProcAddress(
	    gpu->dxgi_dll, "CreateDXGIFactory1");
	gpu->d3d11_create_device = (pfn_d3d11_create_device)(void *)GetProcAddress(
	    gpu->d3d11_dll, "D3D11CreateDevice");
	if (gpu->create_dxgi_factory1 == NULL || gpu->d3d11_create_device == NULL) {
		mikuterm_log("extdisp-gpu: missing DXGI/D3D11 entry points");
		return -1;
	}

	if (gpu->vulkan_dll != NULL) {
		gpu->vk.vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(
		    gpu->vulkan_dll, "vkGetInstanceProcAddr");
		if (gpu->vk.vkGetInstanceProcAddr != NULL) {
			gpu->vk.vkCreateInstance = (PFN_vkCreateInstance)(void *)gpu->vk.vkGetInstanceProcAddr(
			    NULL, "vkCreateInstance");
		}
	} else {
		mikuterm_log("extdisp-gpu: vulkan-1.dll not present, will use dxgi-staging");
	}

	return 0;
}

static void
extdisp_gpu_release_dxgi(
	struct extdisp_gpu *gpu)
{
	if (gpu->dupl != NULL) {
		gpu->dupl->lpVtbl->Release(gpu->dupl);
		gpu->dupl = NULL;
	}
	if (gpu->d3d_ctx != NULL) {
		gpu->d3d_ctx->lpVtbl->Release(gpu->d3d_ctx);
		gpu->d3d_ctx = NULL;
	}
	if (gpu->d3d_dev != NULL) {
		gpu->d3d_dev->lpVtbl->Release(gpu->d3d_dev);
		gpu->d3d_dev = NULL;
	}
	if (gpu->output1 != NULL) {
		gpu->output1->lpVtbl->Release(gpu->output1);
		gpu->output1 = NULL;
	}
	if (gpu->adapter != NULL) {
		gpu->adapter->lpVtbl->Release(gpu->adapter);
		gpu->adapter = NULL;
	}
	if (gpu->factory != NULL) {
		gpu->factory->lpVtbl->Release(gpu->factory);
		gpu->factory = NULL;
	}
}

static int
extdisp_wcsistr(
	const wchar_t *haystack,
	const wchar_t *needle)
{
	size_t nlen;
	size_t i;

	if (haystack == NULL || needle == NULL)
		return 0;
	nlen = wcslen(needle);
	if (nlen == 0U)
		return 1;
	for (; *haystack != L'\0'; haystack++) {
		for (i = 0U; i < nlen; i++) {
			wchar_t a = haystack[i];
			wchar_t b = needle[i];
			if (a == L'\0')
				return 0;
			if (a >= L'A' && a <= L'Z')
				a = (wchar_t)(a + (L'a' - L'A'));
			if (b >= L'A' && b <= L'Z')
				b = (wchar_t)(b + (L'a' - L'A'));
			if (a != b)
				break;
		}
		if (i == nlen)
			return 1;
	}
	return 0;
}

static int
extdisp_gpu_select_output(
	struct extdisp_gpu *gpu)
{
	IDXGIAdapter1 *adp = NULL;
	IDXGIAdapter1 *best_adp = NULL;
	IDXGIOutput1 *best_out1 = NULL;
	DXGI_ADAPTER_DESC1 best_desc;
	WCHAR best_out_name[32];
	WCHAR best_out_str[128];
	uint32_t best_w = 0U;
	uint32_t best_h = 0U;
	int best_is_vdd = 0;
	UINT ai;
	int best_score = -1;
	D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
	HRESULT hr;

	gpu->found_secondary = 0;
	gpu->found_vdd = 0;
	memset(best_out_name, 0, sizeof(best_out_name));
	memset(best_out_str, 0, sizeof(best_out_str));

	if (gpu->factory != NULL && !gpu->factory->lpVtbl->IsCurrent(gpu->factory)) {
		gpu->factory->lpVtbl->Release(gpu->factory);
		gpu->factory = NULL;
	}
	if (gpu->factory == NULL) {
		hr = gpu->create_dxgi_factory1(&IID_IDXGIFactory1, (void **)&gpu->factory);
		if (FAILED(hr) || gpu->factory == NULL)
			return -1;
	}

	memset(&best_desc, 0, sizeof(best_desc));
	for (ai = 0U; gpu->factory->lpVtbl->EnumAdapters1(gpu->factory, ai, &adp) != DXGI_ERROR_NOT_FOUND; ai++) {
		DXGI_ADAPTER_DESC1 adesc;
		IDXGIOutput *out = NULL;
		UINT oi;

		if (FAILED(adp->lpVtbl->GetDesc1(adp, &adesc)) ||
		    (adesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0U) {
			adp->lpVtbl->Release(adp);
			continue;
		}

		for (oi = 0U; adp->lpVtbl->EnumOutputs(adp, oi, &out) != DXGI_ERROR_NOT_FOUND; oi++) {
			DXGI_OUTPUT_DESC odesc;
			IDXGIOutput1 *out1 = NULL;
			DISPLAY_DEVICEW dd;
			uint32_t ow;
			uint32_t oh;
			int is_vdd = 0;
			int is_sec = 0;
			int score = 0;

			if (FAILED(out->lpVtbl->GetDesc(out, &odesc)) || !odesc.AttachedToDesktop) {
				out->lpVtbl->Release(out);
				continue;
			}
			if (FAILED(out->lpVtbl->QueryInterface(out, &IID_IDXGIOutput1, (void **)&out1))) {
				out->lpVtbl->Release(out);
				continue;
			}
			out->lpVtbl->Release(out);

			memset(&dd, 0, sizeof(dd));
			dd.cb = sizeof(dd);
			if (!EnumDisplayDevicesW(odesc.DeviceName, 0U, &dd, 0U)) {
				dd.DeviceString[0] = L'\0';
				dd.DeviceID[0] = L'\0';
			}

			{
				DISPLAY_DEVICEW dd_if;
				DISPLAY_DEVICEW adp_dd;
				DWORD di;

				memset(&dd_if, 0, sizeof(dd_if));
				dd_if.cb = sizeof(dd_if);
				(void)EnumDisplayDevicesW(odesc.DeviceName, 0U, &dd_if, EDD_GET_DEVICE_INTERFACE_NAME);

				if (extdisp_wcsistr(dd.DeviceString, L"MttVDD") ||
				    extdisp_wcsistr(dd.DeviceString, L"Virtual Display") ||
				    extdisp_wcsistr(dd.DeviceString, L"Indirect") ||
				    extdisp_wcsistr(dd.DeviceID, L"MTT") ||
				    extdisp_wcsistr(dd.DeviceID, L"Virtual") ||
				    extdisp_wcsistr(dd_if.DeviceID, L"MTT") ||
				    extdisp_wcsistr(dd_if.DeviceID, L"Virtual")) {
					is_vdd = 1;
				}

				for (di = 0U; ; di++) {
					memset(&adp_dd, 0, sizeof(adp_dd));
					adp_dd.cb = sizeof(adp_dd);
					if (!EnumDisplayDevicesW(NULL, di, &adp_dd, 0U))
						break;
					if (_wcsicmp(adp_dd.DeviceName, odesc.DeviceName) == 0) {
						if (extdisp_wcsistr(adp_dd.DeviceString, L"MttVDD") ||
						    extdisp_wcsistr(adp_dd.DeviceString, L"Virtual") ||
						    extdisp_wcsistr(adp_dd.DeviceString, L"Indirect") ||
						    extdisp_wcsistr(adp_dd.DeviceID, L"MTT") ||
						    extdisp_wcsistr(adp_dd.DeviceID, L"Virtual")) {
							is_vdd = 1;
						}
						break;
					}
				}
				if (is_vdd)
					score += 1000;

				ow = (uint32_t)(odesc.DesktopCoordinates.right - odesc.DesktopCoordinates.left);
				oh = (uint32_t)(odesc.DesktopCoordinates.bottom - odesc.DesktopCoordinates.top);
				is_sec = (oi > 0U || odesc.DesktopCoordinates.left != 0 ||
				    odesc.DesktopCoordinates.top != 0) ? 1 : 0;

				mikuterm_log("extdisp-gpu: adapter[%u] '%ls' output[%u] '%ls' ('%ls', id='%ls', if='%ls') rect=(%ld,%ld %ux%u) vdd=%d",
				    ai, adesc.Description, oi, odesc.DeviceName, dd.DeviceString,
				    dd.DeviceID, dd_if.DeviceID,
				    odesc.DesktopCoordinates.left, odesc.DesktopCoordinates.top, ow, oh, is_vdd);
			}

			if (is_sec)
				score += 10;
			if (ow == gpu->pref_width && oh == gpu->pref_height)
				score += 20;

			if (score > best_score) {
				if (best_out1 != NULL)
					best_out1->lpVtbl->Release(best_out1);
				if (best_adp != NULL)
					best_adp->lpVtbl->Release(best_adp);
				best_score = score;
				best_adp = adp;
				best_adp->lpVtbl->AddRef(best_adp);
				best_out1 = out1;
				best_desc = adesc;
				best_w = ow;
				best_h = oh;
				best_is_vdd = is_vdd;
				gpu->desktop_rect = odesc.DesktopCoordinates;
				wcsncpy(best_out_name, odesc.DeviceName, 31U);
				best_out_name[31] = L'\0';
				wcsncpy(best_out_str, dd.DeviceString, 127U);
				best_out_str[127] = L'\0';
				gpu->found_secondary = is_sec;
				gpu->found_vdd = is_vdd;
			} else {
				out1->lpVtbl->Release(out1);
			}
		}
		adp->lpVtbl->Release(adp);
	}

	if (best_adp == NULL || best_out1 == NULL) {
		mikuterm_log("extdisp-gpu: no attached DXGI output found");
		return -1;
	}

	mikuterm_log("extdisp-gpu: selected output '%ls' ('%ls') %ux%u vdd=%d",
	    best_out_name, best_out_str, best_w, best_h, best_is_vdd);

	if (best_is_vdd && (best_w != gpu->pref_width || best_h != gpu->pref_height) &&
	    gpu->pref_width >= 320U && gpu->pref_height >= 200U) {
		DEVMODEW dm;
		LONG cds;

		memset(&dm, 0, sizeof(dm));
		dm.dmSize = sizeof(dm);
		dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
		dm.dmPelsWidth = gpu->pref_width;
		dm.dmPelsHeight = gpu->pref_height;
		cds = ChangeDisplaySettingsExW(best_out_name, &dm, NULL, CDS_UPDATEREGISTRY, NULL);
		mikuterm_log("extdisp-gpu: ChangeDisplaySettingsExW('%ls', %ux%u) rc=%ld",
		    best_out_name, gpu->pref_width, gpu->pref_height, (long)cds);
		if (cds == DISP_CHANGE_SUCCESSFUL)
			Sleep(150);
	}

	gpu->adapter = best_adp;
	gpu->output1 = best_out1;
	gpu->adapter_luid = best_desc.AdapterLuid;
	mikuterm_log("extdisp-gpu: selected adapter '%ls' luid=%08lx:%08lx secondary=%d vdd=%d",
	    best_desc.Description, (unsigned long)best_desc.AdapterLuid.HighPart,
	    (unsigned long)best_desc.AdapterLuid.LowPart, gpu->found_secondary, gpu->found_vdd);

	hr = gpu->d3d11_create_device((IDXGIAdapter *)gpu->adapter, D3D_DRIVER_TYPE_UNKNOWN,
	    NULL, 0U, &fl, 1U, D3D11_SDK_VERSION, &gpu->d3d_dev, NULL, &gpu->d3d_ctx);
	if (FAILED(hr) || gpu->d3d_dev == NULL) {
		mikuterm_log("extdisp-gpu: D3D11CreateDevice failed hr=0x%08lx", (unsigned long)hr);
		return -1;
	}

	hr = gpu->output1->lpVtbl->DuplicateOutput(gpu->output1, (IUnknown *)gpu->d3d_dev, &gpu->dupl);
	if (FAILED(hr) || gpu->dupl == NULL) {
		mikuterm_log("extdisp-gpu: DuplicateOutput failed hr=0x%08lx", (unsigned long)hr);
		return -1;
	}

	{
		DXGI_OUTDUPL_DESC ddesc;
		memset(&ddesc, 0, sizeof(ddesc));
		gpu->dupl->lpVtbl->GetDesc(gpu->dupl, &ddesc);
		gpu->width = ddesc.ModeDesc.Width & ~1U;
		gpu->height = ddesc.ModeDesc.Height & ~1U;
		gpu->coded_w = (gpu->width + 15U) & ~15U;
		gpu->coded_h = (gpu->height + 15U) & ~15U;
		gpu->dxgi_format = ddesc.ModeDesc.Format;
		if (gpu->dxgi_format == DXGI_FORMAT_R10G10B10A2_UNORM)
			gpu->vk_src_format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
		else
			gpu->vk_src_format = VK_FORMAT_B8G8R8A8_UNORM;
	}
	if (gpu->width < 16U || gpu->height < 16U)
		return -1;

	return 0;
}

static int
extdisp_gpu_init_vulkan_instance(
	struct extdisp_gpu *gpu)
{
	static const char *const exts[] = {
		VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
		VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
		VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME
	};
	VkApplicationInfo app;
	VkInstanceCreateInfo ici;
	VkResult res_inst;

	memset(&app, 0, sizeof(app));
	app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName = "mikuterm";
	app.apiVersion = VK_API_VERSION_1_3;

	memset(&ici, 0, sizeof(ici));
	ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	ici.pApplicationInfo = &app;
	ici.enabledExtensionCount = (uint32_t)(sizeof(exts) / sizeof(exts[0]));
	ici.ppEnabledExtensionNames = exts;

	if (gpu->vk.vkCreateInstance == NULL)
		return -1;

	res_inst = gpu->vk.vkCreateInstance(&ici, NULL, &gpu->instance);
	if (res_inst != VK_SUCCESS) {
		mikuterm_log("extdisp-gpu: vkCreateInstance failed res=%d", (int)res_inst);
		return -1;
	}

#define LOAD_VK_INST(name) \
	gpu->vk.name = (PFN_##name)(void *)gpu->vk.vkGetInstanceProcAddr(gpu->instance, #name); \
	if (gpu->vk.name == NULL) return -1

	LOAD_VK_INST(vkDestroyInstance);
	LOAD_VK_INST(vkEnumeratePhysicalDevices);
	LOAD_VK_INST(vkGetPhysicalDeviceProperties2);
	LOAD_VK_INST(vkGetPhysicalDeviceQueueFamilyProperties2);
	LOAD_VK_INST(vkGetPhysicalDeviceMemoryProperties);
	LOAD_VK_INST(vkGetPhysicalDeviceVideoCapabilitiesKHR);
	LOAD_VK_INST(vkEnumerateDeviceExtensionProperties);
	LOAD_VK_INST(vkCreateDevice);
	LOAD_VK_INST(vkGetDeviceProcAddr);
#undef LOAD_VK_INST

	return 0;
}

static int
extdisp_gpu_select_vk_device(
	struct extdisp_gpu *gpu)
{
	VkPhysicalDevice devs[EXTDISP_GPU_MAX_DEVICES];
	uint32_t dev_count = EXTDISP_GPU_MAX_DEVICES;
	uint32_t i;
	VkPhysicalDevice chosen = VK_NULL_HANDLE;
	uint32_t comp_fam = UINT32_MAX;
	uint32_t enc_fam = UINT32_MAX;
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qinfos[2];
	uint32_t qcount = 0U;
	static const char *const dev_exts[] = {
		VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
		VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
		VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME,
		VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
		VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,
		VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME
	};
	VkDeviceCreateInfo dci;
	VkCommandPoolCreateInfo pinfo;
	VkCommandBufferAllocateInfo cinfo;
	VkSemaphoreCreateInfo sinfo;
	VkFenceCreateInfo finfo;
	VkQueryPoolVideoEncodeFeedbackCreateInfoKHR fb_q;
	VkQueryPoolCreateInfo qinfo;

	if (gpu->vk.vkEnumeratePhysicalDevices(gpu->instance, &dev_count, devs) < 0)
		return -1;

	for (i = 0U; i < dev_count; i++) {
		VkPhysicalDeviceIDProperties idp;
		VkPhysicalDeviceProperties2 p2;
		uint32_t qf_count = 0U;
		VkQueueFamilyProperties2 qf_props[16];
		VkQueueFamilyVideoPropertiesKHR qf_vid[16];
		uint32_t qi;
		uint32_t cf = UINT32_MAX;
		uint32_t ef = UINT32_MAX;
		uint32_t ext_count = 0U;
		VkExtensionProperties *exts = NULL;
		uint32_t ei;
		uint32_t needed = 0U;

		memset(&idp, 0, sizeof(idp));
		idp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
		memset(&p2, 0, sizeof(p2));
		p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
		p2.pNext = &idp;
		gpu->vk.vkGetPhysicalDeviceProperties2(devs[i], &p2);

		if (!idp.deviceLUIDValid ||
		    memcmp(idp.deviceLUID, &gpu->adapter_luid, sizeof(gpu->adapter_luid)) != 0) {
			mikuterm_log("extdisp-gpu: vk dev[%u] '%s' LUID mismatch (valid=%u)",
			    i, p2.properties.deviceName, (unsigned)idp.deviceLUIDValid);
			continue;
		}

		gpu->vk.vkGetPhysicalDeviceQueueFamilyProperties2(devs[i], &qf_count, NULL);
		if (qf_count == 0U || qf_count > 16U)
			continue;
		memset(qf_props, 0, sizeof(qf_props));
		memset(qf_vid, 0, sizeof(qf_vid));
		for (qi = 0U; qi < qf_count; qi++) {
			qf_vid[qi].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
			qf_props[qi].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
			qf_props[qi].pNext = &qf_vid[qi];
		}
		gpu->vk.vkGetPhysicalDeviceQueueFamilyProperties2(devs[i], &qf_count, qf_props);
		for (qi = 0U; qi < qf_count; qi++) {
			VkQueueFlags fl = qf_props[qi].queueFamilyProperties.queueFlags;
			if (cf == UINT32_MAX && (fl & VK_QUEUE_COMPUTE_BIT) != 0U)
				cf = qi;
			if (ef == UINT32_MAX && (fl & VK_QUEUE_VIDEO_ENCODE_BIT_KHR) != 0U &&
			    (qf_vid[qi].videoCodecOperations & VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR) != 0U)
				ef = qi;
		}
		if (cf == UINT32_MAX || ef == UINT32_MAX) {
			mikuterm_log("extdisp-gpu: vk dev[%u] '%s' LUID matched but missing queue (comp=%u enc=%u)",
			    i, p2.properties.deviceName, cf, ef);
			continue;
		}

		if (gpu->vk.vkEnumerateDeviceExtensionProperties(devs[i], NULL, &ext_count, NULL) != VK_SUCCESS ||
		    ext_count == 0U)
			continue;
		exts = (VkExtensionProperties *)calloc(ext_count, sizeof(*exts));
		if (exts == NULL)
			continue;
		if (gpu->vk.vkEnumerateDeviceExtensionProperties(devs[i], NULL, &ext_count, exts) == VK_SUCCESS) {
			size_t di;
			for (di = 0U; di < sizeof(dev_exts) / sizeof(dev_exts[0]); di++) {
				int has_ext = 0;
				for (ei = 0U; ei < ext_count; ei++) {
					if (strcmp(exts[ei].extensionName, dev_exts[di]) == 0) {
						has_ext = 1;
						needed++;
						break;
					}
				}
				if (!has_ext) {
					mikuterm_log("extdisp-gpu: vk dev[%u] '%s' missing ext %s",
					    i, p2.properties.deviceName, dev_exts[di]);
				}
			}
		}
		free(exts);
		if (needed == (uint32_t)(sizeof(dev_exts) / sizeof(dev_exts[0]))) {
			chosen = devs[i];
			comp_fam = cf;
			enc_fam = ef;
			break;
		}
	}

	if (chosen == VK_NULL_HANDLE) {
		mikuterm_log("extdisp-gpu: no matching Vulkan Video H.264 device (checked %u)", dev_count);
		return -1;
	}

	gpu->physical = chosen;
	gpu->compute_family = comp_fam;
	gpu->encode_family = enc_fam;
	gpu->vk.vkGetPhysicalDeviceMemoryProperties(chosen, &gpu->mem_props);

	gpu->h264_profile.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR;
	gpu->h264_profile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_BASELINE;
	gpu->profile.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
	gpu->profile.pNext = &gpu->h264_profile;
	gpu->profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
	gpu->profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
	gpu->profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	gpu->profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;

	memset(qinfos, 0, sizeof(qinfos));
	qinfos[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	qinfos[0].queueFamilyIndex = comp_fam;
	qinfos[0].queueCount = 1U;
	qinfos[0].pQueuePriorities = &prio;
	qcount = 1U;
	if (enc_fam != comp_fam) {
		qinfos[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		qinfos[1].queueFamilyIndex = enc_fam;
		qinfos[1].queueCount = 1U;
		qinfos[1].pQueuePriorities = &prio;
		qcount = 2U;
	}

	memset(&dci, 0, sizeof(dci));
	dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	dci.queueCreateInfoCount = qcount;
	dci.pQueueCreateInfos = qinfos;
	dci.enabledExtensionCount = (uint32_t)(sizeof(dev_exts) / sizeof(dev_exts[0]));
	dci.ppEnabledExtensionNames = dev_exts;
	if (gpu->vk.vkCreateDevice(chosen, &dci, NULL, &gpu->device) != VK_SUCCESS)
		return -1;

#define LOAD_VK_DEV(name) \
	gpu->vk.name = (PFN_##name)(void *)gpu->vk.vkGetDeviceProcAddr(gpu->device, #name); \
	if (gpu->vk.name == NULL) return -1

	LOAD_VK_DEV(vkDestroyDevice);
	LOAD_VK_DEV(vkGetDeviceQueue);
	LOAD_VK_DEV(vkCreateCommandPool);
	LOAD_VK_DEV(vkDestroyCommandPool);
	LOAD_VK_DEV(vkAllocateCommandBuffers);
	LOAD_VK_DEV(vkBeginCommandBuffer);
	LOAD_VK_DEV(vkEndCommandBuffer);
	LOAD_VK_DEV(vkQueueSubmit);
	LOAD_VK_DEV(vkDeviceWaitIdle);
	LOAD_VK_DEV(vkCreateSemaphore);
	LOAD_VK_DEV(vkDestroySemaphore);
	LOAD_VK_DEV(vkCreateFence);
	LOAD_VK_DEV(vkDestroyFence);
	LOAD_VK_DEV(vkWaitForFences);
	LOAD_VK_DEV(vkResetFences);
	LOAD_VK_DEV(vkCreateImage);
	LOAD_VK_DEV(vkDestroyImage);
	LOAD_VK_DEV(vkGetImageMemoryRequirements);
	LOAD_VK_DEV(vkBindImageMemory);
	LOAD_VK_DEV(vkCreateImageView);
	LOAD_VK_DEV(vkDestroyImageView);
	LOAD_VK_DEV(vkCreateSampler);
	LOAD_VK_DEV(vkDestroySampler);
	LOAD_VK_DEV(vkCreateBuffer);
	LOAD_VK_DEV(vkDestroyBuffer);
	LOAD_VK_DEV(vkGetBufferMemoryRequirements);
	LOAD_VK_DEV(vkBindBufferMemory);
	LOAD_VK_DEV(vkAllocateMemory);
	LOAD_VK_DEV(vkFreeMemory);
	LOAD_VK_DEV(vkMapMemory);
	LOAD_VK_DEV(vkUnmapMemory);
	LOAD_VK_DEV(vkCreateShaderModule);
	LOAD_VK_DEV(vkDestroyShaderModule);
	LOAD_VK_DEV(vkCreateDescriptorSetLayout);
	LOAD_VK_DEV(vkDestroyDescriptorSetLayout);
	LOAD_VK_DEV(vkCreateDescriptorPool);
	LOAD_VK_DEV(vkDestroyDescriptorPool);
	LOAD_VK_DEV(vkAllocateDescriptorSets);
	LOAD_VK_DEV(vkUpdateDescriptorSets);
	LOAD_VK_DEV(vkCreatePipelineLayout);
	LOAD_VK_DEV(vkDestroyPipelineLayout);
	LOAD_VK_DEV(vkCreateComputePipelines);
	LOAD_VK_DEV(vkDestroyPipeline);
	LOAD_VK_DEV(vkCreateQueryPool);
	LOAD_VK_DEV(vkDestroyQueryPool);
	LOAD_VK_DEV(vkGetQueryPoolResults);
	LOAD_VK_DEV(vkCmdPipelineBarrier);
	LOAD_VK_DEV(vkCmdBindPipeline);
	LOAD_VK_DEV(vkCmdBindDescriptorSets);
	LOAD_VK_DEV(vkCmdPushConstants);
	LOAD_VK_DEV(vkCmdDispatch);
	LOAD_VK_DEV(vkCmdResetQueryPool);
	LOAD_VK_DEV(vkCmdBeginQuery);
	LOAD_VK_DEV(vkCmdEndQuery);
	LOAD_VK_DEV(vkCreateVideoSessionKHR);
	LOAD_VK_DEV(vkDestroyVideoSessionKHR);
	LOAD_VK_DEV(vkGetVideoSessionMemoryRequirementsKHR);
	LOAD_VK_DEV(vkBindVideoSessionMemoryKHR);
	LOAD_VK_DEV(vkCreateVideoSessionParametersKHR);
	LOAD_VK_DEV(vkDestroyVideoSessionParametersKHR);
	LOAD_VK_DEV(vkGetEncodedVideoSessionParametersKHR);
	LOAD_VK_DEV(vkCmdBeginVideoCodingKHR);
	LOAD_VK_DEV(vkCmdEndVideoCodingKHR);
	LOAD_VK_DEV(vkCmdControlVideoCodingKHR);
	LOAD_VK_DEV(vkCmdEncodeVideoKHR);
#undef LOAD_VK_DEV

	gpu->vk.vkGetDeviceQueue(gpu->device, comp_fam, 0U, &gpu->compute_queue);
	gpu->vk.vkGetDeviceQueue(gpu->device, enc_fam, 0U, &gpu->encode_queue);

	memset(&pinfo, 0, sizeof(pinfo));
	pinfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pinfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pinfo.queueFamilyIndex = comp_fam;
	if (gpu->vk.vkCreateCommandPool(gpu->device, &pinfo, NULL, &gpu->compute_pool) != VK_SUCCESS)
		return -1;
	if (enc_fam != comp_fam) {
		pinfo.queueFamilyIndex = enc_fam;
		if (gpu->vk.vkCreateCommandPool(gpu->device, &pinfo, NULL, &gpu->encode_pool) != VK_SUCCESS)
			return -1;
	} else {
		gpu->encode_pool = gpu->compute_pool;
	}

	memset(&cinfo, 0, sizeof(cinfo));
	cinfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cinfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cinfo.commandBufferCount = 1U;
	cinfo.commandPool = gpu->compute_pool;
	if (gpu->vk.vkAllocateCommandBuffers(gpu->device, &cinfo, &gpu->compute_cmd) != VK_SUCCESS)
		return -1;
	cinfo.commandPool = gpu->encode_pool;
	if (gpu->vk.vkAllocateCommandBuffers(gpu->device, &cinfo, &gpu->encode_cmd) != VK_SUCCESS)
		return -1;

	memset(&sinfo, 0, sizeof(sinfo));
	sinfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	if (gpu->vk.vkCreateSemaphore(gpu->device, &sinfo, NULL, &gpu->compute_done) != VK_SUCCESS)
		return -1;

	memset(&finfo, 0, sizeof(finfo));
	finfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	if (gpu->vk.vkCreateFence(gpu->device, &finfo, NULL, &gpu->encode_fence) != VK_SUCCESS)
		return -1;

	memset(&fb_q, 0, sizeof(fb_q));
	fb_q.sType = VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_FEEDBACK_CREATE_INFO_KHR;
	fb_q.pNext = &gpu->profile;
	fb_q.encodeFeedbackFlags =
	    VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BUFFER_OFFSET_BIT_KHR |
	    VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR;

	memset(&qinfo, 0, sizeof(qinfo));
	qinfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	qinfo.pNext = &fb_q;
	qinfo.queryType = VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR;
	qinfo.queryCount = 1U;
	if (gpu->vk.vkCreateQueryPool(gpu->device, &qinfo, NULL, &gpu->query_pool) != VK_SUCCESS)
		return -1;

	return 0;
}

static int
extdisp_gpu_create_compute_pipeline(
	struct extdisp_gpu *gpu)
{
	VkSamplerCreateInfo samp;
	VkShaderModuleCreateInfo smci;
	VkDescriptorSetLayoutBinding bindings[3];
	VkDescriptorSetLayoutCreateInfo dsl;
	VkDescriptorPoolSize psizes[2];
	VkDescriptorPoolCreateInfo dpci;
	VkDescriptorSetAllocateInfo dsai;
	VkPushConstantRange pcr;
	VkPipelineLayoutCreateInfo plci;
	VkComputePipelineCreateInfo cpci;

	memset(&samp, 0, sizeof(samp));
	samp.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samp.magFilter = VK_FILTER_NEAREST;
	samp.minFilter = VK_FILTER_NEAREST;
	samp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	if (gpu->vk.vkCreateSampler(gpu->device, &samp, NULL, &gpu->src_sampler) != VK_SUCCESS)
		return -1;

	memset(&smci, 0, sizeof(smci));
	smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	smci.codeSize = sizeof(extdisp_nv12_spv);
	smci.pCode = extdisp_nv12_spv;
	if (gpu->vk.vkCreateShaderModule(gpu->device, &smci, NULL, &gpu->shader) != VK_SUCCESS)
		return -1;

	memset(bindings, 0, sizeof(bindings));
	bindings[0].binding = 0U;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[0].descriptorCount = 1U;
	bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[1].binding = 1U;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[1].descriptorCount = 1U;
	bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[2].binding = 2U;
	bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[2].descriptorCount = 1U;
	bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	memset(&dsl, 0, sizeof(dsl));
	dsl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	dsl.bindingCount = 3U;
	dsl.pBindings = bindings;
	if (gpu->vk.vkCreateDescriptorSetLayout(gpu->device, &dsl, NULL, &gpu->desc_layout) != VK_SUCCESS)
		return -1;

	psizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	psizes[0].descriptorCount = 1U;
	psizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	psizes[1].descriptorCount = 2U;
	memset(&dpci, 0, sizeof(dpci));
	dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	dpci.maxSets = 1U;
	dpci.poolSizeCount = 2U;
	dpci.pPoolSizes = psizes;
	if (gpu->vk.vkCreateDescriptorPool(gpu->device, &dpci, NULL, &gpu->desc_pool) != VK_SUCCESS)
		return -1;

	memset(&dsai, 0, sizeof(dsai));
	dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	dsai.descriptorPool = gpu->desc_pool;
	dsai.descriptorSetCount = 1U;
	dsai.pSetLayouts = &gpu->desc_layout;
	if (gpu->vk.vkAllocateDescriptorSets(gpu->device, &dsai, &gpu->desc_set) != VK_SUCCESS)
		return -1;

	memset(&pcr, 0, sizeof(pcr));
	pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pcr.offset = 0U;
	pcr.size = (uint32_t)sizeof(struct extdisp_compute_pc);

	memset(&plci, 0, sizeof(plci));
	plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	plci.setLayoutCount = 1U;
	plci.pSetLayouts = &gpu->desc_layout;
	plci.pushConstantRangeCount = 1U;
	plci.pPushConstantRanges = &pcr;
	if (gpu->vk.vkCreatePipelineLayout(gpu->device, &plci, NULL, &gpu->pipe_layout) != VK_SUCCESS)
		return -1;

	memset(&cpci, 0, sizeof(cpci));
	cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	cpci.stage.module = gpu->shader;
	cpci.stage.pName = "main";
	cpci.layout = gpu->pipe_layout;
	if (gpu->vk.vkCreateComputePipelines(gpu->device, VK_NULL_HANDLE, 1U, &cpci, NULL, &gpu->compute_pipe) != VK_SUCCESS)
		return -1;

	return 0;
}

static int
extdisp_gpu_create_frame_resources(
	struct extdisp_gpu *gpu)
{
	D3D11_TEXTURE2D_DESC tdesc;
	IDXGIResource1 *res1 = NULL;
	HRESULT hr;
	VkExternalMemoryImageCreateInfo ext_img;
	VkImageCreateInfo ici;
	VkMemoryRequirements req;
	VkImportMemoryWin32HandleInfoKHR imp_info;
	VkMemoryDedicatedAllocateInfo ded_info;
	VkMemoryAllocateInfo ainfo;
	uint32_t type_idx = 0U;
	VkImageViewCreateInfo vci;
	VkDescriptorImageInfo dii[3];
	VkWriteDescriptorSet wds[3];
	VkVideoEncodeH264CapabilitiesKHR h264_caps;
	VkVideoEncodeCapabilitiesKHR enc_caps;
	VkVideoCapabilitiesKHR caps;
	VkExtensionProperties std_hdr;
	VkVideoEncodeH264SessionCreateInfoKHR h264_sess;
	VkVideoSessionCreateInfoKHR sess_info;
	VkVideoSessionMemoryRequirementsKHR reqs[8];
	VkBindVideoSessionMemoryInfoKHR binds[8];
	uint32_t req_count = 8U;
	uint32_t i;
	StdVideoH264SequenceParameterSet sps;
	StdVideoH264PictureParameterSet pps;
	VkVideoEncodeH264SessionParametersAddInfoKHR add_info;
	VkVideoEncodeH264SessionParametersCreateInfoKHR h264_params;
	VkVideoSessionParametersCreateInfoKHR params_info;
	VkVideoEncodeH264SessionParametersGetInfoKHR h264_get;
	VkVideoEncodeSessionParametersGetInfoKHR get_info;
	size_t ps_size = 0U;
	VkBufferCreateInfo bci;

	memset(&tdesc, 0, sizeof(tdesc));
	tdesc.Width = gpu->width;
	tdesc.Height = gpu->height;
	tdesc.MipLevels = 1U;
	tdesc.ArraySize = 1U;
	tdesc.Format = gpu->dxgi_format;
	tdesc.SampleDesc.Count = 1U;
	tdesc.Usage = D3D11_USAGE_DEFAULT;
	tdesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	tdesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

	hr = gpu->d3d_dev->lpVtbl->CreateTexture2D(gpu->d3d_dev, &tdesc, NULL, &gpu->shared_tex);
	if (FAILED(hr) || gpu->shared_tex == NULL)
		return -1;

	hr = gpu->shared_tex->lpVtbl->QueryInterface(gpu->shared_tex, &IID_IDXGIKeyedMutex, (void **)&gpu->keyed_mutex);
	if (FAILED(hr) || gpu->keyed_mutex == NULL)
		return -1;

	hr = gpu->shared_tex->lpVtbl->QueryInterface(gpu->shared_tex, &IID_IDXGIResource1, (void **)&res1);
	if (FAILED(hr) || res1 == NULL)
		return -1;
	hr = res1->lpVtbl->CreateSharedHandle(res1, NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
	    NULL, &gpu->shared_handle);
	res1->lpVtbl->Release(res1);
	if (FAILED(hr) || gpu->shared_handle == NULL)
		return -1;

	memset(&ext_img, 0, sizeof(ext_img));
	ext_img.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	ext_img.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;

	memset(&ici, 0, sizeof(ici));
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.pNext = &ext_img;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = gpu->vk_src_format;
	ici.extent.width = gpu->width;
	ici.extent.height = gpu->height;
	ici.extent.depth = 1U;
	ici.mipLevels = 1U;
	ici.arrayLayers = 1U;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (gpu->vk.vkCreateImage(gpu->device, &ici, NULL, &gpu->src_image) != VK_SUCCESS)
		return -1;

	gpu->vk.vkGetImageMemoryRequirements(gpu->device, gpu->src_image, &req);
	if (extdisp_gpu_find_mem_type(&gpu->mem_props, req.memoryTypeBits,
	    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type_idx) != 0)
		return -1;

	memset(&ded_info, 0, sizeof(ded_info));
	ded_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
	ded_info.image = gpu->src_image;

	memset(&imp_info, 0, sizeof(imp_info));
	imp_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
	imp_info.pNext = &ded_info;
	imp_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
	imp_info.handle = gpu->shared_handle;

	memset(&ainfo, 0, sizeof(ainfo));
	ainfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	ainfo.pNext = &imp_info;
	ainfo.allocationSize = req.size;
	ainfo.memoryTypeIndex = type_idx;
	if (gpu->vk.vkAllocateMemory(gpu->device, &ainfo, NULL, &gpu->src_mem) != VK_SUCCESS)
		return -1;
	if (gpu->vk.vkBindImageMemory(gpu->device, gpu->src_image, gpu->src_mem, 0) != VK_SUCCESS)
		return -1;

	memset(&vci, 0, sizeof(vci));
	vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vci.image = gpu->src_image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = gpu->vk_src_format;
	vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	vci.subresourceRange.levelCount = 1U;
	vci.subresourceRange.layerCount = 1U;
	if (gpu->vk.vkCreateImageView(gpu->device, &vci, NULL, &gpu->src_view) != VK_SUCCESS)
		return -1;

	if (extdisp_gpu_create_nv12_image(gpu,
	    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR,
	    &gpu->nv12_image, &gpu->nv12_mem) != 0)
		return -1;

	memset(&vci, 0, sizeof(vci));
	vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vci.image = gpu->nv12_image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = VK_FORMAT_R8_UNORM;
	vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
	vci.subresourceRange.levelCount = 1U;
	vci.subresourceRange.layerCount = 1U;
	if (gpu->vk.vkCreateImageView(gpu->device, &vci, NULL, &gpu->nv12_y_view) != VK_SUCCESS)
		return -1;

	vci.format = VK_FORMAT_R8G8_UNORM;
	vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
	if (gpu->vk.vkCreateImageView(gpu->device, &vci, NULL, &gpu->nv12_uv_view) != VK_SUCCESS)
		return -1;

	vci.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	if (gpu->vk.vkCreateImageView(gpu->device, &vci, NULL, &gpu->nv12_enc_view) != VK_SUCCESS)
		return -1;

	memset(dii, 0, sizeof(dii));
	dii[0].sampler = gpu->src_sampler;
	dii[0].imageView = gpu->src_view;
	dii[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	dii[1].imageView = gpu->nv12_y_view;
	dii[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	dii[2].imageView = gpu->nv12_uv_view;
	dii[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

	memset(wds, 0, sizeof(wds));
	wds[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	wds[0].dstSet = gpu->desc_set;
	wds[0].dstBinding = 0U;
	wds[0].descriptorCount = 1U;
	wds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	wds[0].pImageInfo = &dii[0];

	wds[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	wds[1].dstSet = gpu->desc_set;
	wds[1].dstBinding = 1U;
	wds[1].descriptorCount = 1U;
	wds[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	wds[1].pImageInfo = &dii[1];

	wds[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	wds[2].dstSet = gpu->desc_set;
	wds[2].dstBinding = 2U;
	wds[2].descriptorCount = 1U;
	wds[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	wds[2].pImageInfo = &dii[2];
	gpu->vk.vkUpdateDescriptorSets(gpu->device, 3U, wds, 0U, NULL);

	memset(&h264_caps, 0, sizeof(h264_caps));
	h264_caps.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_KHR;
	memset(&enc_caps, 0, sizeof(enc_caps));
	enc_caps.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR;
	enc_caps.pNext = &h264_caps;
	memset(&caps, 0, sizeof(caps));
	caps.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
	caps.pNext = &enc_caps;
	if (gpu->vk.vkGetPhysicalDeviceVideoCapabilitiesKHR(gpu->physical, &gpu->profile, &caps) != VK_SUCCESS)
		return -1;

	memset(&h264_sess, 0, sizeof(h264_sess));
	h264_sess.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_CREATE_INFO_KHR;
	h264_sess.useMaxLevelIdc = VK_TRUE;
	h264_sess.maxLevelIdc = h264_caps.maxLevelIdc;

	memset(&std_hdr, 0, sizeof(std_hdr));
	snprintf(std_hdr.extensionName, sizeof(std_hdr.extensionName),
	    "%s", VK_STD_VULKAN_VIDEO_CODEC_H264_ENCODE_EXTENSION_NAME);
	std_hdr.specVersion = VK_STD_VULKAN_VIDEO_CODEC_H264_ENCODE_SPEC_VERSION;

	memset(&sess_info, 0, sizeof(sess_info));
	sess_info.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR;
	sess_info.pNext = &h264_sess;
	sess_info.queueFamilyIndex = gpu->encode_family;
	sess_info.pVideoProfile = &gpu->profile;
	sess_info.pictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	sess_info.maxCodedExtent.width = gpu->coded_w;
	sess_info.maxCodedExtent.height = gpu->coded_h;
	sess_info.referencePictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	sess_info.maxDpbSlots = EXTDISP_GPU_DPB_SLOTS;
	sess_info.maxActiveReferencePictures = 1U;
	sess_info.pStdHeaderVersion = &std_hdr;
	if (gpu->vk.vkCreateVideoSessionKHR(gpu->device, &sess_info, NULL, &gpu->session) != VK_SUCCESS)
		return -1;

	memset(reqs, 0, sizeof(reqs));
	for (i = 0U; i < 8U; i++)
		reqs[i].sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR;
	if (gpu->vk.vkGetVideoSessionMemoryRequirementsKHR(gpu->device, gpu->session, &req_count, reqs) != VK_SUCCESS ||
	    req_count == 0U)
		return -1;

	memset(binds, 0, sizeof(binds));
	for (i = 0U; i < req_count; i++) {
		if (extdisp_gpu_find_mem_type(&gpu->mem_props, reqs[i].memoryRequirements.memoryTypeBits,
		    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type_idx) != 0 &&
		    extdisp_gpu_find_mem_type(&gpu->mem_props, reqs[i].memoryRequirements.memoryTypeBits,
		    0U, &type_idx) != 0)
			return -1;

		memset(&ainfo, 0, sizeof(ainfo));
		ainfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		ainfo.allocationSize = reqs[i].memoryRequirements.size;
		ainfo.memoryTypeIndex = type_idx;
		if (gpu->vk.vkAllocateMemory(gpu->device, &ainfo, NULL, &gpu->session_mem[i]) != VK_SUCCESS)
			return -1;
		gpu->session_mem_count++;
		binds[i].sType = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR;
		binds[i].memoryBindIndex = reqs[i].memoryBindIndex;
		binds[i].memory = gpu->session_mem[i];
		binds[i].memorySize = reqs[i].memoryRequirements.size;
	}
	if (gpu->vk.vkBindVideoSessionMemoryKHR(gpu->device, gpu->session, req_count, binds) != VK_SUCCESS)
		return -1;

	memset(&sps, 0, sizeof(sps));
	sps.profile_idc = STD_VIDEO_H264_PROFILE_IDC_BASELINE;
	sps.level_idc = h264_sess.maxLevelIdc;
	sps.chroma_format_idc = STD_VIDEO_H264_CHROMA_FORMAT_IDC_420;
	sps.pic_order_cnt_type = STD_VIDEO_H264_POC_TYPE_2;
	sps.max_num_ref_frames = 1U;
	sps.pic_width_in_mbs_minus1 = (gpu->coded_w / 16U) - 1U;
	sps.pic_height_in_map_units_minus1 = (gpu->coded_h / 16U) - 1U;
	sps.flags.frame_mbs_only_flag = 1U;
	sps.flags.direct_8x8_inference_flag = 1U;
	sps.flags.constraint_set1_flag = 1U;
	if (gpu->coded_w != gpu->width || gpu->coded_h != gpu->height) {
		sps.flags.frame_cropping_flag = 1U;
		sps.frame_crop_right_offset = (gpu->coded_w - gpu->width) / 2U;
		sps.frame_crop_bottom_offset = (gpu->coded_h - gpu->height) / 2U;
	}

	memset(&pps, 0, sizeof(pps));
	pps.flags.deblocking_filter_control_present_flag = 1U;

	memset(&add_info, 0, sizeof(add_info));
	add_info.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR;
	add_info.stdSPSCount = 1U;
	add_info.pStdSPSs = &sps;
	add_info.stdPPSCount = 1U;
	add_info.pStdPPSs = &pps;

	memset(&h264_params, 0, sizeof(h264_params));
	h264_params.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR;
	h264_params.maxStdSPSCount = 1U;
	h264_params.maxStdPPSCount = 1U;
	h264_params.pParametersAddInfo = &add_info;

	memset(&params_info, 0, sizeof(params_info));
	params_info.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR;
	params_info.pNext = &h264_params;
	params_info.videoSession = gpu->session;
	if (gpu->vk.vkCreateVideoSessionParametersKHR(gpu->device, &params_info, NULL, &gpu->parameters) != VK_SUCCESS)
		return -1;

	memset(&h264_get, 0, sizeof(h264_get));
	h264_get.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_GET_INFO_KHR;
	h264_get.writeStdSPS = VK_TRUE;
	h264_get.writeStdPPS = VK_TRUE;

	memset(&get_info, 0, sizeof(get_info));
	get_info.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_GET_INFO_KHR;
	get_info.pNext = &h264_get;
	get_info.videoSessionParameters = gpu->parameters;

	if (gpu->vk.vkGetEncodedVideoSessionParametersKHR(gpu->device, &get_info, NULL, &ps_size, NULL) == VK_SUCCESS &&
	    ps_size > 0U) {
		gpu->ps_buf = (uint8_t *)malloc(ps_size);
		if (gpu->ps_buf == NULL)
			return -1;
		if (gpu->vk.vkGetEncodedVideoSessionParametersKHR(gpu->device, &get_info, NULL, &ps_size, gpu->ps_buf) != VK_SUCCESS)
			return -1;
		gpu->ps_len = ps_size;
	}

	for (i = 0U; i < EXTDISP_GPU_DPB_SLOTS; i++) {
		if (extdisp_gpu_create_nv12_image(gpu, VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR,
		    &gpu->dpb_image[i], &gpu->dpb_mem[i]) != 0)
			return -1;
		memset(&vci, 0, sizeof(vci));
		vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		vci.image = gpu->dpb_image[i];
		vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vci.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
		vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		vci.subresourceRange.levelCount = 1U;
		vci.subresourceRange.layerCount = 1U;
		if (gpu->vk.vkCreateImageView(gpu->device, &vci, NULL, &gpu->dpb_view[i]) != VK_SUCCESS)
			return -1;
	}

	memset(&bci, 0, sizeof(bci));
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size = EXTDISP_GPU_BITSTREAM_BYTES;
	bci.usage = VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (gpu->vk.vkCreateBuffer(gpu->device, &bci, NULL, &gpu->out_buf) != VK_SUCCESS)
		return -1;

	gpu->vk.vkGetBufferMemoryRequirements(gpu->device, gpu->out_buf, &req);
	if (extdisp_gpu_find_mem_type(&gpu->mem_props, req.memoryTypeBits,
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &type_idx) != 0)
		return -1;

	memset(&ainfo, 0, sizeof(ainfo));
	ainfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	ainfo.allocationSize = req.size;
	ainfo.memoryTypeIndex = type_idx;
	if (gpu->vk.vkAllocateMemory(gpu->device, &ainfo, NULL, &gpu->out_mem) != VK_SUCCESS)
		return -1;
	if (gpu->vk.vkBindBufferMemory(gpu->device, gpu->out_buf, gpu->out_mem, 0) != VK_SUCCESS)
		return -1;
	if (gpu->vk.vkMapMemory(gpu->device, gpu->out_mem, 0, EXTDISP_GPU_BITSTREAM_BYTES, 0U, &gpu->out_map) != VK_SUCCESS)
		return -1;

	gpu->resources_ready = 1;
	return 0;
}

static void
extdisp_gpu_destroy_frame_resources(
	struct extdisp_gpu *gpu)
{
	uint32_t i;

	gpu->resources_ready = 0;

	if (gpu->device != VK_NULL_HANDLE) {
		if (gpu->out_map != NULL) {
			gpu->vk.vkUnmapMemory(gpu->device, gpu->out_mem);
			gpu->out_map = NULL;
		}
		if (gpu->out_buf != VK_NULL_HANDLE) {
			gpu->vk.vkDestroyBuffer(gpu->device, gpu->out_buf, NULL);
			gpu->out_buf = VK_NULL_HANDLE;
		}
		if (gpu->out_mem != VK_NULL_HANDLE) {
			gpu->vk.vkFreeMemory(gpu->device, gpu->out_mem, NULL);
			gpu->out_mem = VK_NULL_HANDLE;
		}
		for (i = 0U; i < EXTDISP_GPU_DPB_SLOTS; i++) {
			if (gpu->dpb_view[i] != VK_NULL_HANDLE) {
				gpu->vk.vkDestroyImageView(gpu->device, gpu->dpb_view[i], NULL);
				gpu->dpb_view[i] = VK_NULL_HANDLE;
			}
			if (gpu->dpb_image[i] != VK_NULL_HANDLE) {
				gpu->vk.vkDestroyImage(gpu->device, gpu->dpb_image[i], NULL);
				gpu->dpb_image[i] = VK_NULL_HANDLE;
			}
			if (gpu->dpb_mem[i] != VK_NULL_HANDLE) {
				gpu->vk.vkFreeMemory(gpu->device, gpu->dpb_mem[i], NULL);
				gpu->dpb_mem[i] = VK_NULL_HANDLE;
			}
		}
		if (gpu->parameters != VK_NULL_HANDLE) {
			gpu->vk.vkDestroyVideoSessionParametersKHR(gpu->device, gpu->parameters, NULL);
			gpu->parameters = VK_NULL_HANDLE;
		}
		if (gpu->session != VK_NULL_HANDLE) {
			gpu->vk.vkDestroyVideoSessionKHR(gpu->device, gpu->session, NULL);
			gpu->session = VK_NULL_HANDLE;
		}
		for (i = 0U; i < gpu->session_mem_count; i++) {
			if (gpu->session_mem[i] != VK_NULL_HANDLE) {
				gpu->vk.vkFreeMemory(gpu->device, gpu->session_mem[i], NULL);
				gpu->session_mem[i] = VK_NULL_HANDLE;
			}
		}
		gpu->session_mem_count = 0U;

		if (gpu->nv12_enc_view != VK_NULL_HANDLE) {
			gpu->vk.vkDestroyImageView(gpu->device, gpu->nv12_enc_view, NULL);
			gpu->nv12_enc_view = VK_NULL_HANDLE;
		}
		if (gpu->nv12_uv_view != VK_NULL_HANDLE) {
			gpu->vk.vkDestroyImageView(gpu->device, gpu->nv12_uv_view, NULL);
			gpu->nv12_uv_view = VK_NULL_HANDLE;
		}
		if (gpu->nv12_y_view != VK_NULL_HANDLE) {
			gpu->vk.vkDestroyImageView(gpu->device, gpu->nv12_y_view, NULL);
			gpu->nv12_y_view = VK_NULL_HANDLE;
		}
		if (gpu->nv12_image != VK_NULL_HANDLE) {
			gpu->vk.vkDestroyImage(gpu->device, gpu->nv12_image, NULL);
			gpu->nv12_image = VK_NULL_HANDLE;
		}
		if (gpu->nv12_mem != VK_NULL_HANDLE) {
			gpu->vk.vkFreeMemory(gpu->device, gpu->nv12_mem, NULL);
			gpu->nv12_mem = VK_NULL_HANDLE;
		}
		if (gpu->src_view != VK_NULL_HANDLE) {
			gpu->vk.vkDestroyImageView(gpu->device, gpu->src_view, NULL);
			gpu->src_view = VK_NULL_HANDLE;
		}
		if (gpu->src_image != VK_NULL_HANDLE) {
			gpu->vk.vkDestroyImage(gpu->device, gpu->src_image, NULL);
			gpu->src_image = VK_NULL_HANDLE;
		}
		if (gpu->src_mem != VK_NULL_HANDLE) {
			gpu->vk.vkFreeMemory(gpu->device, gpu->src_mem, NULL);
			gpu->src_mem = VK_NULL_HANDLE;
		}
	}

	free(gpu->ps_buf);
	gpu->ps_buf = NULL;
	gpu->ps_len = 0U;

	if (gpu->shared_handle != NULL) {
		CloseHandle(gpu->shared_handle);
		gpu->shared_handle = NULL;
	}
	if (gpu->keyed_mutex != NULL) {
		gpu->keyed_mutex->lpVtbl->Release(gpu->keyed_mutex);
		gpu->keyed_mutex = NULL;
	}
	if (gpu->shared_tex != NULL) {
		gpu->shared_tex->lpVtbl->Release(gpu->shared_tex);
		gpu->shared_tex = NULL;
	}
}

static int
extdisp_gpu_find_mem_type(
	const VkPhysicalDeviceMemoryProperties *props,
	uint32_t bits,
	VkMemoryPropertyFlags req,
	uint32_t *out)
{
	uint32_t i;

	for (i = 0U; i < props->memoryTypeCount; i++) {
		if ((bits & (1U << i)) != 0U &&
		    (props->memoryTypes[i].propertyFlags & req) == req) {
			*out = i;
			return 0;
		}
	}
	return -1;
}

static int
extdisp_gpu_create_nv12_image(
	struct extdisp_gpu *gpu,
	VkImageUsageFlags usage,
	VkImage *out_img,
	VkDeviceMemory *out_mem)
{
	VkVideoProfileListInfoKHR plist;
	VkImageCreateInfo ici;
	VkMemoryRequirements req;
	VkMemoryAllocateInfo ainfo;
	uint32_t type_idx = 0U;

	memset(&plist, 0, sizeof(plist));
	plist.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
	plist.profileCount = 1U;
	plist.pProfiles = &gpu->profile;

	memset(&ici, 0, sizeof(ici));
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.pNext = &plist;
	ici.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	ici.extent.width = gpu->coded_w;
	ici.extent.height = gpu->coded_h;
	ici.extent.depth = 1U;
	ici.mipLevels = 1U;
	ici.arrayLayers = 1U;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = usage;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (gpu->vk.vkCreateImage(gpu->device, &ici, NULL, out_img) != VK_SUCCESS)
		return -1;

	gpu->vk.vkGetImageMemoryRequirements(gpu->device, *out_img, &req);
	if (extdisp_gpu_find_mem_type(&gpu->mem_props, req.memoryTypeBits,
	    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type_idx) != 0 &&
	    extdisp_gpu_find_mem_type(&gpu->mem_props, req.memoryTypeBits, 0U, &type_idx) != 0)
		return -1;

	memset(&ainfo, 0, sizeof(ainfo));
	ainfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	ainfo.allocationSize = req.size;
	ainfo.memoryTypeIndex = type_idx;
	if (gpu->vk.vkAllocateMemory(gpu->device, &ainfo, NULL, out_mem) != VK_SUCCESS)
		return -1;
	if (gpu->vk.vkBindImageMemory(gpu->device, *out_img, *out_mem, 0) != VK_SUCCESS)
		return -1;

	return 0;
}
