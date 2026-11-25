/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Win32 Extended Display host implementation.
 *
 * Runs a background UDP 33890 listener for Network Display ADV packets,
 * sends encrypted OFFER packets on user Connect, hosts an ephemeral
 * TLS/RDP server with librdp + libmedia H.264, reads BGRA frames from
 * the mikuterm-idd shared-memory ring (or generates standby frames if
 * the driver is not installed), and injects mouse/keyboard input via
 * SendInput mapped to the virtual monitor rectangle.
 */

#include "extdisp.h"
#include "netdisp-proto.h"

#include "drivers/win32/mikuterm-idd/mikuterm_idd_ipc.h"
#include "userland/desktop/libmedia/media-encoder.h"
#include "userland/desktop/librdp/rdp.h"
#include "userland/desktop/mediafile/mediafile.h"

#include <compat/crypto/evp.h>
#include <compat/crypto/rand.h>
#include <compat/ssl/ssl.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXTDISP_DEVICE_TTL_MS	7000ULL
#define EXTDISP_DEFAULT_W	1920U
#define EXTDISP_DEFAULT_H	1080U
#define EXTDISP_ACCEPT_MS	15000U

struct extdisp_sw_create_info {
	ULONG cbSize;
	PCWSTR pszInstanceId;
	LPCWSTR pszzHardwareIds;
	LPCWSTR pszzCompatibleIds;
	const GUID *pContainerId;
	ULONG CapabilityFlags;
	PCWSTR pszDeviceDescription;
	PCWSTR pszDeviceLocation;
	const SECURITY_DESCRIPTOR *pSecurityDescriptor;
};

typedef void (WINAPI *extdisp_sw_callback_fn)(
	void *hSwDevice,
	HRESULT CreateResult,
	void *pContext,
	PCWSTR pszDeviceInstanceId);

typedef HRESULT (WINAPI *extdisp_sw_create_fn)(
	PCWSTR pszEnumeratorName,
	PCWSTR pszParentDeviceInstance,
	const struct extdisp_sw_create_info *pCreateInfo,
	ULONG cPropertyCount,
	const void *pProperties,
	extdisp_sw_callback_fn pCallback,
	void *pContext,
	void **phSwDevice);

typedef VOID (WINAPI *extdisp_sw_close_fn)(void *hSwDevice);

struct extdisp_peer {
	int used;
	struct netdisp_adv adv;
	struct sockaddr_in addr;
	char ip[64];
	char id_hex[33];
	char fingerprint[16];
	ULONGLONG last_seen_ms;
};

struct extdisp_session {
	int active;
	int stop;
	SOCKET listen_sock;
	SOCKET client_sock;
	SSL_CTX *ssl_ctx;
	SSL *ssl;
	int tls_active;
	struct rdp_server *srv;
	struct media_encoder *encoder;
	uint32_t width;
	uint32_t height;
	char username[NETDISP_MAX_USER_LEN + 1U];
	char password[NETDISP_MAX_PASS_LEN + 1U];
	char target_id[33];
	char target_name[64];
	void *sw_device;
	HMODULE cfgmgr32;
	extdisp_sw_close_fn sw_close;
	HANDLE shm_handle;
	const struct mikuterm_idd_shm_header *shm_hdr;
	HANDLE frame_event;
	uint32_t last_frame_seq;
	uint8_t *y_plane;
	uint8_t *u_plane;
	uint8_t *v_plane;
	int force_key;
	int authed;
	int connected;
	int running;
};

struct extdisp_state {
	int initialized;
	int stop;
	CRITICAL_SECTION lock;
	HANDLE disc_thread;
	HANDLE sess_thread;
	SOCKET disc_sock;
	struct extdisp_peer peers[TERMINAL_EXTDISP_DEVICES_MAX];
	struct extdisp_session sess;
	char status_text[128];
	int wsa_started;
};

static struct extdisp_state extdisp;

static uint8_t
clamp_u8(int v)
{
	if (v < 0)
		return 0U;
	if (v > 255)
		return 255U;
	return (uint8_t)v;
}

static void
bgra_to_i420(
	const uint8_t *bgra,
	uint32_t stride,
	uint32_t width,
	uint32_t height,
	uint8_t *y_plane,
	uint8_t *u_plane,
	uint8_t *v_plane)
{
	uint32_t y;
	uint32_t x;
	uint32_t uv_w = width / 2U;

	for (y = 0U; y < height; y++) {
		const uint8_t *row = bgra + (size_t)y * stride;
		uint8_t *ydst = y_plane + (size_t)y * width;
		for (x = 0U; x < width; x++) {
			int b = row[x * 4U + 0U];
			int g = row[x * 4U + 1U];
			int r = row[x * 4U + 2U];
			ydst[x] = clamp_u8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
		}
		if ((y & 1U) == 0U && y + 1U < height) {
			const uint8_t *row2 = bgra + (size_t)(y + 1U) * stride;
			uint8_t *udst = u_plane + (size_t)(y / 2U) * uv_w;
			uint8_t *vdst = v_plane + (size_t)(y / 2U) * uv_w;
			for (x = 0U; x + 1U < width; x += 2U) {
				int b = (row[x * 4U + 0U] + row[(x + 1U) * 4U + 0U] +
				    row2[x * 4U + 0U] + row2[(x + 1U) * 4U + 0U]) >> 2;
				int g = (row[x * 4U + 1U] + row[(x + 1U) * 4U + 1U] +
				    row2[x * 4U + 1U] + row2[(x + 1U) * 4U + 1U]) >> 2;
				int r = (row[x * 4U + 2U] + row[(x + 1U) * 4U + 2U] +
				    row2[x * 4U + 2U] + row2[(x + 1U) * 4U + 2U]) >> 2;
				udst[x / 2U] = clamp_u8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
				vdst[x / 2U] = clamp_u8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
			}
		}
	}
}

static void
fill_standby_i420(
	uint32_t width,
	uint32_t height,
	uint8_t *y_plane,
	uint8_t *u_plane,
	uint8_t *v_plane)
{
	size_t y_sz = (size_t)width * height;
	size_t uv_sz = ((size_t)width / 2U) * ((size_t)height / 2U);

	memset(y_plane, 32, y_sz);
	memset(u_plane, 132, uv_sz);
	memset(v_plane, 124, uv_sz);
}

struct extdisp_mon_search {
	uint32_t target_w;
	uint32_t target_h;
	RECT best_rect;
	int found_secondary;
};

static BOOL CALLBACK
extdisp_mon_enum(
	HMONITOR mon,
	HDC hdc,
	LPRECT rect,
	LPARAM lparam)
{
	struct extdisp_mon_search *ms = (struct extdisp_mon_search *)lparam;
	MONITORINFO mi;

	(void)hdc;
	memset(&mi, 0, sizeof(mi));
	mi.cbSize = sizeof(mi);
	if (GetMonitorInfoW(mon, &mi)) {
		if ((mi.dwFlags & MONITORINFOF_PRIMARY) == 0) {
			uint32_t mw = (uint32_t)(rect->right - rect->left);
			uint32_t mh = (uint32_t)(rect->bottom - rect->top);
			if (!ms->found_secondary ||
			    (mw == ms->target_w && mh == ms->target_h)) {
				ms->best_rect = *rect;
				ms->found_secondary = 1;
				if (mw == ms->target_w && mh == ms->target_h)
					return FALSE;
			}
		}
	}
	return TRUE;
}

static void
extdisp_get_target_rect(
	uint32_t w,
	uint32_t h,
	RECT *out)
{
	struct extdisp_mon_search ms;

	memset(&ms, 0, sizeof(ms));
	ms.target_w = w;
	ms.target_h = h;
	EnumDisplayMonitors(NULL, NULL, extdisp_mon_enum, (LPARAM)&ms);
	if (ms.found_secondary) {
		*out = ms.best_rect;
		return;
	}
	out->left = GetSystemMetrics(SM_XVIRTUALSCREEN);
	out->top = GetSystemMetrics(SM_YVIRTUALSCREEN);
	out->right = out->left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
	out->bottom = out->top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

static int
cb_send_bytes(
	void *user,
	const uint8_t *data,
	size_t len)
{
	struct extdisp_session *s = (struct extdisp_session *)user;
	size_t sent = 0U;

	while (sent < len) {
		int n;
		if (s->tls_active && s->ssl != NULL)
			n = SSL_write(s->ssl, data + sent, (int)(len - sent));
		else
			n = send(s->client_sock, (const char *)(data + sent), (int)(len - sent), 0);
		if (n <= 0)
			return -1;
		sent += (size_t)n;
	}
	return 0;
}

static int
cb_on_ntlm_lookup(
	void *user,
	const char *domain,
	const char *user_name,
	uint8_t out_nt_hash[16])
{
	struct extdisp_session *s = (struct extdisp_session *)user;

	(void)domain;
	if (user_name == NULL || strcmp(user_name, s->username) != 0)
		return -1;
	rdp_ntlm_nt_hash(s->password, out_nt_hash);
	s->authed = 1;
	return 0;
}

static void
cb_on_auth_request(
	void *user,
	const char *domain,
	const char *user_name,
	const char *password)
{
	struct extdisp_session *s = (struct extdisp_session *)user;

	(void)domain;
	if (!s->authed && user_name != NULL && password != NULL &&
	    strcmp(user_name, s->username) == 0 &&
	    strcmp(password, s->password) == 0) {
		s->authed = 1;
		(void)rdp_server_accept_auth(s->srv);
		return;
	}
	(void)rdp_server_reject_auth(s->srv, 0U);
	s->running = 0;
}

static void
cb_on_connected(
	void *user,
	uint32_t width,
	uint32_t height)
{
	struct extdisp_session *s = (struct extdisp_session *)user;

	(void)width;
	(void)height;
	s->connected = 1;
}

static void
cb_on_disconnect(
	void *user,
	uint32_t error_info)
{
	struct extdisp_session *s = (struct extdisp_session *)user;

	(void)error_info;
	s->running = 0;
}

static void
cb_on_input_mouse(
	void *user,
	uint16_t flags,
	uint16_t x,
	uint16_t y)
{
	struct extdisp_session *s = (struct extdisp_session *)user;
	RECT mon;
	int vx, vy, vw, vh;
	LONG abs_x, abs_y;
	INPUT inp;
	DWORD mflags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;

	extdisp_get_target_rect(s->width, s->height, &mon);
	if (x >= s->width)
		x = (uint16_t)(s->width > 0U ? s->width - 1U : 0U);
	if (y >= s->height)
		y = (uint16_t)(s->height > 0U ? s->height - 1U : 0U);

	vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
	vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
	vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
	vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
	if (vw <= 0)
		vw = 1;
	if (vh <= 0)
		vh = 1;

	{
		LONG px = mon.left + (LONG)(((uint64_t)x * (uint64_t)(mon.right - mon.left)) / (s->width ? s->width : 1U));
		LONG py = mon.top + (LONG)(((uint64_t)y * (uint64_t)(mon.bottom - mon.top)) / (s->height ? s->height : 1U));
		abs_x = (LONG)(((int64_t)(px - vx) * 65535LL) / vw);
		abs_y = (LONG)(((int64_t)(py - vy) * 65535LL) / vh);
	}

	if ((flags & 0x0800U) != 0U)
		mflags |= MOUSEEVENTF_MOVE;
	if ((flags & 0x1000U) != 0U)
		mflags |= ((flags & 0x8000U) != 0U) ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
	if ((flags & 0x2000U) != 0U)
		mflags |= ((flags & 0x8000U) != 0U) ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
	if ((flags & 0x4000U) != 0U)
		mflags |= ((flags & 0x8000U) != 0U) ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;

	memset(&inp, 0, sizeof(inp));
	inp.type = INPUT_MOUSE;
	inp.mi.dx = abs_x;
	inp.mi.dy = abs_y;
	inp.mi.dwFlags = mflags;
	SendInput(1, &inp, sizeof(inp));
}

static void
cb_on_input_key(
	void *user,
	uint16_t scancode,
	uint16_t flags)
{
	INPUT inp;

	(void)user;
	memset(&inp, 0, sizeof(inp));
	inp.type = INPUT_KEYBOARD;
	inp.ki.wScan = scancode;
	inp.ki.dwFlags = KEYEVENTF_SCANCODE;
	if ((flags & 0x0100U) != 0U)
		inp.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
	if ((flags & 0x8000U) != 0U)
		inp.ki.dwFlags |= KEYEVENTF_KEYUP;
	SendInput(1, &inp, sizeof(inp));
}

static void
cb_on_input_unicode(
	void *user,
	uint16_t codepoint,
	uint16_t flags)
{
	INPUT inp;

	(void)user;
	memset(&inp, 0, sizeof(inp));
	inp.type = INPUT_KEYBOARD;
	inp.ki.wScan = codepoint;
	inp.ki.dwFlags = KEYEVENTF_UNICODE;
	if ((flags & 0x8000U) != 0U)
		inp.ki.dwFlags |= KEYEVENTF_KEYUP;
	SendInput(1, &inp, sizeof(inp));
}

static void WINAPI
extdisp_sw_callback(
	void *hSwDevice,
	HRESULT CreateResult,
	void *pContext,
	PCWSTR pszDeviceInstanceId)
{
	(void)hSwDevice;
	(void)CreateResult;
	(void)pContext;
	(void)pszDeviceInstanceId;
}

static void
extdisp_try_create_sw_device(struct extdisp_session *s)
{
	HMODULE mod;
	extdisp_sw_create_fn sw_create;
	extdisp_sw_close_fn sw_close;
	struct extdisp_sw_create_info info;
	static const WCHAR hw_ids[] = L"Root\\MikutermIdd\0";

	mod = LoadLibraryExW(L"cfgmgr32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (mod == NULL)
		return;
	sw_create = (extdisp_sw_create_fn)(void *)GetProcAddress(mod, "SwDeviceCreate");
	sw_close = (extdisp_sw_close_fn)(void *)GetProcAddress(mod, "SwDeviceClose");
	if (sw_create == NULL || sw_close == NULL) {
		FreeLibrary(mod);
		return;
	}

	memset(&info, 0, sizeof(info));
	info.cbSize = sizeof(info);
	info.pszInstanceId = MIKUTERM_IDD_INSTANCE_ID;
	info.pszzHardwareIds = hw_ids;
	info.pszzCompatibleIds = hw_ids;
	info.CapabilityFlags = 0x1U | 0x2U;
	info.pszDeviceDescription = L"mikuterm Virtual Display";

	if (FAILED(sw_create(L"Root", L"HTREE\\ROOT\\0", &info, 0, NULL,
	    extdisp_sw_callback, NULL, &s->sw_device))) {
		s->sw_device = NULL;
		FreeLibrary(mod);
		return;
	}
	s->cfgmgr32 = mod;
	s->sw_close = sw_close;
}

static void
extdisp_open_driver_shm(struct extdisp_session *s)
{
	s->shm_handle = OpenFileMappingW(FILE_MAP_READ, FALSE, MIKUTERM_IDD_SHM_NAME);
	if (s->shm_handle != NULL) {
		s->shm_hdr = (const struct mikuterm_idd_shm_header *)MapViewOfFile(
		    s->shm_handle, FILE_MAP_READ, 0, 0, MIKUTERM_IDD_SHM_TOTAL_SIZE);
		if (s->shm_hdr == NULL ||
		    s->shm_hdr->magic != MIKUTERM_IDD_SHM_MAGIC ||
		    s->shm_hdr->version != MIKUTERM_IDD_SHM_VERSION) {
			if (s->shm_hdr != NULL) {
				UnmapViewOfFile(s->shm_hdr);
				s->shm_hdr = NULL;
			}
			CloseHandle(s->shm_handle);
			s->shm_handle = NULL;
		}
	}
	s->frame_event = OpenEventW(SYNCHRONIZE, FALSE, MIKUTERM_IDD_EVENT_NAME);
}

static void
extdisp_close_driver_shm(struct extdisp_session *s)
{
	if (s->frame_event != NULL) {
		CloseHandle(s->frame_event);
		s->frame_event = NULL;
	}
	if (s->shm_hdr != NULL) {
		UnmapViewOfFile(s->shm_hdr);
		s->shm_hdr = NULL;
	}
	if (s->shm_handle != NULL) {
		CloseHandle(s->shm_handle);
		s->shm_handle = NULL;
	}
}

static void
extdisp_session_cleanup(struct extdisp_session *s)
{
	if (s->srv != NULL) {
		rdp_server_destroy(s->srv);
		s->srv = NULL;
	}
	if (s->encoder != NULL) {
		media_encoder_close(s->encoder);
		s->encoder = NULL;
	}
	if (s->ssl != NULL) {
		SSL_free(s->ssl);
		s->ssl = NULL;
	}
	if (s->ssl_ctx != NULL) {
		SSL_CTX_free(s->ssl_ctx);
		s->ssl_ctx = NULL;
	}
	if (s->client_sock != INVALID_SOCKET) {
		closesocket(s->client_sock);
		s->client_sock = INVALID_SOCKET;
	}
	if (s->listen_sock != INVALID_SOCKET) {
		closesocket(s->listen_sock);
		s->listen_sock = INVALID_SOCKET;
	}
	extdisp_close_driver_shm(s);
	if (s->sw_device != NULL && s->sw_close != NULL) {
		s->sw_close(s->sw_device);
		s->sw_device = NULL;
	}
	if (s->cfgmgr32 != NULL) {
		FreeLibrary(s->cfgmgr32);
		s->cfgmgr32 = NULL;
		s->sw_close = NULL;
	}
	free(s->y_plane);
	free(s->u_plane);
	free(s->v_plane);
	s->y_plane = NULL;
	s->u_plane = NULL;
	s->v_plane = NULL;
	s->active = 0;
	s->connected = 0;
	s->running = 0;
}

static DWORD WINAPI
extdisp_session_thread(LPVOID param)
{
	struct extdisp_session *s = &extdisp.sess;
	struct rdp_callbacks cb;
	struct rdp_server_config cfg;
	struct media_encoder_config ecfg;
	uint8_t io_buf[16384];
	ULONGLONG start_ms;
	ULONGLONG last_frame_ms = 0ULL;
	int accepted = 0;

	(void)param;
	extdisp_try_create_sw_device(s);
	extdisp_open_driver_shm(s);

	start_ms = GetTickCount64();
	while (!s->stop && !extdisp.stop && (GetTickCount64() - start_ms) < EXTDISP_ACCEPT_MS) {
		fd_set rfds;
		struct timeval tv;
		int rc;

		FD_ZERO(&rfds);
		FD_SET(s->listen_sock, &rfds);
		tv.tv_sec = 0;
		tv.tv_usec = 100000;
		rc = select(0, &rfds, NULL, NULL, &tv);
		if (rc > 0 && FD_ISSET(s->listen_sock, &rfds)) {
			s->client_sock = accept(s->listen_sock, NULL, NULL);
			if (s->client_sock != INVALID_SOCKET) {
				int nodelay = 1;
				setsockopt(s->client_sock, IPPROTO_TCP, TCP_NODELAY,
				    (const char *)&nodelay, sizeof(nodelay));
				accepted = 1;
				break;
			}
		}
	}

	if (!accepted) {
		EnterCriticalSection(&extdisp.lock);
		(void)snprintf(extdisp.status_text, sizeof(extdisp.status_text),
		    "Timed out waiting for peer connection.");
		extdisp_session_cleanup(s);
		LeaveCriticalSection(&extdisp.lock);
		return 0;
	}

	s->y_plane = (uint8_t *)malloc((size_t)s->width * s->height);
	s->u_plane = (uint8_t *)malloc(((size_t)s->width / 2U) * ((size_t)s->height / 2U));
	s->v_plane = (uint8_t *)malloc(((size_t)s->width / 2U) * ((size_t)s->height / 2U));
	if (s->y_plane == NULL || s->u_plane == NULL || s->v_plane == NULL) {
		EnterCriticalSection(&extdisp.lock);
		(void)snprintf(extdisp.status_text, sizeof(extdisp.status_text), "Out of memory.");
		extdisp_session_cleanup(s);
		LeaveCriticalSection(&extdisp.lock);
		return 0;
	}

	memset(&ecfg, 0, sizeof(ecfg));
	ecfg.codec = MEDIA_CODEC_H264;
	ecfg.profile = MEDIA_ENCODER_PROFILE_H264_CONSTRAINED_BASELINE;
	ecfg.width = s->width;
	ecfg.height = s->height;
	ecfg.rate_num = 60U;
	ecfg.rate_den = 1U;
	ecfg.input = MEDIA_ENCODER_INPUT_I420;
	ecfg.allow = MEDIA_ENCODER_SOFTWARE | MEDIA_ENCODER_HARDWARE;
	ecfg.purpose = MEDIA_ENCODER_LOW_LATENCY;
	ecfg.output = MEDIA_ENCODER_ANNEX_B;
	ecfg.rate_control = MEDIA_ENCODER_RATE_CQP;
	ecfg.qp = 26U;
	if (media_encoder_create(&ecfg, &s->encoder) != 0) {
		EnterCriticalSection(&extdisp.lock);
		(void)snprintf(extdisp.status_text, sizeof(extdisp.status_text),
		    "Failed to create H.264 encoder.");
		extdisp_session_cleanup(s);
		LeaveCriticalSection(&extdisp.lock);
		return 0;
	}

	memset(&cb, 0, sizeof(cb));
	cb.send_bytes = cb_send_bytes;
	cb.on_auth_request = cb_on_auth_request;
	cb.on_ntlm_lookup = cb_on_ntlm_lookup;
	cb.on_connected = cb_on_connected;
	cb.on_input_key = cb_on_input_key;
	cb.on_input_unicode = cb_on_input_unicode;
	cb.on_input_mouse = cb_on_input_mouse;
	cb.on_disconnect = cb_on_disconnect;

	memset(&cfg, 0, sizeof(cfg));
	cfg.width = s->width;
	cfg.height = s->height;
	(void)SSL_CTX_get0_certificate_ASN1(s->ssl_ctx, &cfg.tls_cert_der,
	    &cfg.tls_cert_der_len);

	s->srv = rdp_server_create(&cb, s, &cfg);
	if (s->srv == NULL) {
		EnterCriticalSection(&extdisp.lock);
		(void)snprintf(extdisp.status_text, sizeof(extdisp.status_text),
		    "Failed to create RDP server.");
		extdisp_session_cleanup(s);
		LeaveCriticalSection(&extdisp.lock);
		return 0;
	}

	EnterCriticalSection(&extdisp.lock);
	(void)snprintf(extdisp.status_text, sizeof(extdisp.status_text),
	    "Connected to %s (%ux%u).", s->target_name, s->width, s->height);
	LeaveCriticalSection(&extdisp.lock);

	s->running = 1;
	s->force_key = 1;

	while (s->running && !s->stop && !extdisp.stop) {
		fd_set rfds;
		struct timeval tv;
		int rc;

		if (rdp_server_needs_tls(s->srv) != 0 && !s->tls_active) {
			s->ssl = SSL_new(s->ssl_ctx);
			if (s->ssl == NULL)
				break;
			if (SSL_set_fd(s->ssl, (int)s->client_sock) != 1 ||
			    SSL_accept(s->ssl) != 1)
				break;
			s->tls_active = 1;
			if (rdp_server_tls_established(s->srv) != 0)
				break;
		}

		FD_ZERO(&rfds);
		FD_SET(s->client_sock, &rfds);
		tv.tv_sec = 0;
		tv.tv_usec = 16000;
		rc = select(0, &rfds, NULL, NULL, &tv);
		if (rc < 0)
			break;

		if (rc > 0 && FD_ISSET(s->client_sock, &rfds)) {
			int n;
			if (s->tls_active)
				n = SSL_read(s->ssl, io_buf, sizeof(io_buf));
			else
				n = recv(s->client_sock, (char *)io_buf, sizeof(io_buf), 0);
			if (n <= 0)
				break;
			if (rdp_server_feed(s->srv, io_buf, (size_t)n) != 0)
				break;
		}

		if (rdp_server_is_gfx_ready(s->srv) != 0 &&
		    rdp_server_unacked_frames(s->srv) < 6U) {
			ULONGLONG now_ms = GetTickCount64();
			int have_frame = 0;

			if (s->shm_hdr != NULL && s->shm_hdr->active != 0U) {
				uint32_t seq = s->shm_hdr->frame_seq;
				if (seq != s->last_frame_seq &&
				    s->shm_hdr->width == s->width &&
				    s->shm_hdr->height == s->height) {
					uint32_t slot = s->shm_hdr->slot_index % MIKUTERM_IDD_RING_SLOTS;
					const uint8_t *src = (const uint8_t *)s->shm_hdr +
					    sizeof(struct mikuterm_idd_shm_header) +
					    (size_t)slot * (size_t)MIKUTERM_IDD_SLOT_BYTES;
					bgra_to_i420(src, s->shm_hdr->stride, s->width, s->height,
					    s->y_plane, s->u_plane, s->v_plane);
					s->last_frame_seq = seq;
					have_frame = 1;
				}
			} else if (now_ms - last_frame_ms >= 66ULL) {
				fill_standby_i420(s->width, s->height,
				    s->y_plane, s->u_plane, s->v_plane);
				have_frame = 1;
			}

			if (have_frame) {
				struct media_encoder_picture pic;
				struct media_encoded pkt;
				unsigned eflags = s->force_key ? MEDIA_ENCODE_FORCE_KEY : 0U;

				last_frame_ms = now_ms;
				memset(&pic, 0, sizeof(pic));
				pic.plane[0] = s->y_plane;
				pic.plane[1] = s->u_plane;
				pic.plane[2] = s->v_plane;
				pic.stride[0] = s->width;
				pic.stride[1] = s->width / 2U;
				pic.stride[2] = s->width / 2U;

				if (media_encoder_send(s->encoder, &pic, (int64_t)(now_ms * 1000ULL), eflags) == 0) {
					s->force_key = 0;
					while (media_encoder_receive(s->encoder, MEDIA_RECEIVE_NOWAIT, &pkt) == 0) {
						struct rdp_avc_rect rect;
						struct rdp_avc_quant quant;
						struct rdp_avc_stream avc;
						uint32_t frame_id = 0U;

						memset(&rect, 0, sizeof(rect));
						rect.right = (uint16_t)s->width;
						rect.bottom = (uint16_t)s->height;
						memset(&quant, 0, sizeof(quant));
						quant.qp = 26U;
						quant.quality = 100U;
						memset(&avc, 0, sizeof(avc));
						avc.luma.rect_count = 1U;
						avc.luma.rects = &rect;
						avc.luma.quants = &quant;
						avc.luma.data = pkt.data;
						avc.luma.data_len = pkt.size;
						(void)rdp_server_send_avc_frame(
						    s->srv, 0U, &avc, (uint32_t)now_ms, &frame_id);
					}
				}
			}
		}
	}

	EnterCriticalSection(&extdisp.lock);
	(void)snprintf(extdisp.status_text, sizeof(extdisp.status_text), "Idle.");
	extdisp_session_cleanup(s);
	LeaveCriticalSection(&extdisp.lock);
	return 0;
}

static DWORD WINAPI
extdisp_discovery_thread(LPVOID param)
{
	uint8_t buf[512];

	(void)param;
	while (!extdisp.stop) {
		fd_set rfds;
		struct timeval tv;
		int rc;
		ULONGLONG now_ms;
		size_t i;

		FD_ZERO(&rfds);
		FD_SET(extdisp.disc_sock, &rfds);
		tv.tv_sec = 0;
		tv.tv_usec = 200000;
		rc = select(0, &rfds, NULL, NULL, &tv);
		now_ms = GetTickCount64();

		if (rc > 0 && FD_ISSET(extdisp.disc_sock, &rfds)) {
			struct sockaddr_in from;
			int from_len = (int)sizeof(from);
			int n = recvfrom(extdisp.disc_sock, (char *)buf, sizeof(buf), 0,
			    (struct sockaddr *)&from, &from_len);
			struct netdisp_adv adv;

			if (n > 0 && netdisp_adv_decode(buf, (size_t)n, &adv) == 0) {
				char id_hex[33];
				char fp[16];
				size_t free_idx = TERMINAL_EXTDISP_DEVICES_MAX;
				size_t match_idx = TERMINAL_EXTDISP_DEVICES_MAX;

				netdisp_device_id_hex(adv.device_id, id_hex);
				netdisp_fingerprint_hex(adv.pk, adv.nonce, fp);

				EnterCriticalSection(&extdisp.lock);
				for (i = 0U; i < TERMINAL_EXTDISP_DEVICES_MAX; i++) {
					if (extdisp.peers[i].used &&
					    strcmp(extdisp.peers[i].id_hex, id_hex) == 0) {
						match_idx = i;
						break;
					}
					if (!extdisp.peers[i].used && free_idx == TERMINAL_EXTDISP_DEVICES_MAX)
						free_idx = i;
				}
				if (match_idx == TERMINAL_EXTDISP_DEVICES_MAX)
					match_idx = free_idx;
				if (match_idx < TERMINAL_EXTDISP_DEVICES_MAX) {
					struct extdisp_peer *p = &extdisp.peers[match_idx];
					p->used = 1;
					p->adv = adv;
					p->addr = from;
					if (inet_ntop(AF_INET, &from.sin_addr, p->ip, sizeof(p->ip)) == NULL)
						(void)snprintf(p->ip, sizeof(p->ip), "0.0.0.0");
					(void)snprintf(p->id_hex, sizeof(p->id_hex), "%s", id_hex);
					(void)snprintf(p->fingerprint, sizeof(p->fingerprint), "%s", fp);
					p->last_seen_ms = now_ms;
				}
				LeaveCriticalSection(&extdisp.lock);
			}
		}

		EnterCriticalSection(&extdisp.lock);
		for (i = 0U; i < TERMINAL_EXTDISP_DEVICES_MAX; i++) {
			if (extdisp.peers[i].used &&
			    now_ms - extdisp.peers[i].last_seen_ms > EXTDISP_DEVICE_TTL_MS) {
				extdisp.peers[i].used = 0;
			}
		}
		LeaveCriticalSection(&extdisp.lock);
	}
	return 0;
}

static unsigned
extdisp_op_list(
	void *data,
	struct terminal_extdisp_device *out,
	unsigned cap)
{
	unsigned count = 0U;
	size_t i;

	(void)data;
	EnterCriticalSection(&extdisp.lock);
	for (i = 0U; i < TERMINAL_EXTDISP_DEVICES_MAX && count < cap; i++) {
		const struct extdisp_peer *p = &extdisp.peers[i];
		if (!p->used)
			continue;
		memset(&out[count], 0, sizeof(out[count]));
		(void)snprintf(out[count].device_id, sizeof(out[count].device_id), "%s", p->id_hex);
		(void)snprintf(out[count].name, sizeof(out[count].name), "%s", p->adv.device_name);
		(void)snprintf(out[count].address, sizeof(out[count].address), "%s", p->ip);
		(void)snprintf(out[count].fingerprint, sizeof(out[count].fingerprint), "%s", p->fingerprint);
		out[count].width = p->adv.pref_width ? p->adv.pref_width : EXTDISP_DEFAULT_W;
		out[count].height = p->adv.pref_height ? p->adv.pref_height : EXTDISP_DEFAULT_H;
		out[count].connected = (extdisp.sess.active &&
		    strcmp(extdisp.sess.target_id, p->id_hex) == 0) ? 1 : 0;
		count++;
	}
	LeaveCriticalSection(&extdisp.lock);
	return count;
}

static int
extdisp_op_status(
	void *data,
	char *text,
	size_t size,
	int *connected)
{
	(void)data;
	EnterCriticalSection(&extdisp.lock);
	if (text != NULL && size > 0U)
		(void)snprintf(text, size, "%s", extdisp.status_text);
	if (connected != NULL)
		*connected = extdisp.sess.active ? 1 : 0;
	LeaveCriticalSection(&extdisp.lock);
	return 0;
}

static int
extdisp_op_connect(
	void *data,
	const char *device_id,
	char *why,
	size_t size)
{
	struct extdisp_peer peer;
	struct netdisp_offer_plain plain;
	uint8_t pkt[NETDISP_OFFER_MAX_LEN];
	size_t pkt_len = 0U;
	uint8_t rnd[8];
	struct sockaddr_in bind_addr;
	int addr_len;
	SOCKET lsock = INVALID_SOCKET;
	SSL_CTX *ctx = NULL;
	int found = 0;
	size_t i;

	(void)data;
	if (device_id == NULL)
		return -1;

	EnterCriticalSection(&extdisp.lock);
	if (extdisp.sess.active) {
		if (why != NULL && size > 0U)
			(void)snprintf(why, size, "An Extended Display session is already active.");
		LeaveCriticalSection(&extdisp.lock);
		return -1;
	}
	for (i = 0U; i < TERMINAL_EXTDISP_DEVICES_MAX; i++) {
		if (extdisp.peers[i].used && strcmp(extdisp.peers[i].id_hex, device_id) == 0) {
			peer = extdisp.peers[i];
			found = 1;
			break;
		}
	}
	LeaveCriticalSection(&extdisp.lock);

	if (!found) {
		if (why != NULL && size > 0U)
			(void)snprintf(why, size, "Selected Network Display is no longer available.");
		return -1;
	}

	if (extdisp.sess_thread != NULL) {
		WaitForSingleObject(extdisp.sess_thread, INFINITE);
		CloseHandle(extdisp.sess_thread);
		extdisp.sess_thread = NULL;
	}

	lsock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (lsock == INVALID_SOCKET) {
		if (why != NULL && size > 0U)
			(void)snprintf(why, size, "Failed to create TCP listen socket.");
		return -1;
	}
	memset(&bind_addr, 0, sizeof(bind_addr));
	bind_addr.sin_family = AF_INET;
	bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	bind_addr.sin_port = 0;
	addr_len = (int)sizeof(bind_addr);
	if (bind(lsock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0 ||
	    listen(lsock, 1) != 0 ||
	    getsockname(lsock, (struct sockaddr *)&bind_addr, &addr_len) != 0) {
		closesocket(lsock);
		if (why != NULL && size > 0U)
			(void)snprintf(why, size, "Failed to bind ephemeral RDP port.");
		return -1;
	}

	ctx = SSL_CTX_new(TLS_server_method());
	if (ctx == NULL || SSL_CTX_generate_self_signed(ctx, "mikuterm-extdisp") != 1) {
		if (ctx != NULL)
			SSL_CTX_free(ctx);
		closesocket(lsock);
		if (why != NULL && size > 0U)
			(void)snprintf(why, size, "Failed to generate ephemeral TLS certificate.");
		return -1;
	}

	if (RAND_bytes(rnd, sizeof(rnd)) != 1) {
		SSL_CTX_free(ctx);
		closesocket(lsock);
		return -1;
	}

	memset(&plain, 0, sizeof(plain));
	memcpy(plain.nonce, peer.adv.nonce, NETDISP_NONCE_LEN);
	plain.rdp_port = ntohs(bind_addr.sin_port);
	(void)snprintf(plain.username, sizeof(plain.username), "mikuterm+extend1");
	for (i = 0U; i < sizeof(rnd); i++)
		(void)snprintf(plain.password + i * 2U, 3U, "%02x", rnd[i]);

	if (netdisp_offer_encrypt(peer.adv.device_id, peer.adv.pk, &plain,
	    pkt, sizeof(pkt), &pkt_len) != 0) {
		SSL_CTX_free(ctx);
		closesocket(lsock);
		if (why != NULL && size > 0U)
			(void)snprintf(why, size, "Failed to encrypt OFFER packet.");
		return -1;
	}

	EnterCriticalSection(&extdisp.lock);
	memset(&extdisp.sess, 0, sizeof(extdisp.sess));
	extdisp.sess.active = 1;
	extdisp.sess.listen_sock = lsock;
	extdisp.sess.client_sock = INVALID_SOCKET;
	extdisp.sess.ssl_ctx = ctx;
	extdisp.sess.width = peer.adv.pref_width ? peer.adv.pref_width : EXTDISP_DEFAULT_W;
	extdisp.sess.height = peer.adv.pref_height ? peer.adv.pref_height : EXTDISP_DEFAULT_H;
	extdisp.sess.width &= ~1U;
	extdisp.sess.height &= ~1U;
	(void)snprintf(extdisp.sess.username, sizeof(extdisp.sess.username), "%s", plain.username);
	(void)snprintf(extdisp.sess.password, sizeof(extdisp.sess.password), "%s", plain.password);
	(void)snprintf(extdisp.sess.target_id, sizeof(extdisp.sess.target_id), "%s", peer.id_hex);
	(void)snprintf(extdisp.sess.target_name, sizeof(extdisp.sess.target_name), "%s", peer.adv.device_name);
	(void)snprintf(extdisp.status_text, sizeof(extdisp.status_text),
	    "Connecting to %s...", peer.adv.device_name);

	extdisp.sess_thread = CreateThread(NULL, 0, extdisp_session_thread, NULL, 0, NULL);
	if (extdisp.sess_thread == NULL) {
		extdisp_session_cleanup(&extdisp.sess);
		(void)snprintf(extdisp.status_text, sizeof(extdisp.status_text),
		    "Failed to start session thread.");
		LeaveCriticalSection(&extdisp.lock);
		return -1;
	}
	LeaveCriticalSection(&extdisp.lock);

	{
		struct sockaddr_in dest = peer.addr;
		dest.sin_port = htons(peer.adv.reply_port);
		(void)sendto(extdisp.disc_sock, (const char *)pkt, (int)pkt_len, 0,
		    (const struct sockaddr *)&dest, sizeof(dest));
	}
	return 0;
}

static void
extdisp_op_disconnect(void *data)
{
	HANDLE thr;

	(void)data;
	EnterCriticalSection(&extdisp.lock);
	extdisp.sess.stop = 1;
	extdisp.sess.running = 0;
	if (extdisp.sess.client_sock != INVALID_SOCKET)
		shutdown(extdisp.sess.client_sock, SD_BOTH);
	thr = extdisp.sess_thread;
	extdisp.sess_thread = NULL;
	LeaveCriticalSection(&extdisp.lock);

	if (thr != NULL) {
		WaitForSingleObject(thr, 3000);
		CloseHandle(thr);
	}
}

static const struct terminal_extdisp_ops extdisp_ops_table = {
	extdisp_op_list,
	extdisp_op_status,
	extdisp_op_connect,
	extdisp_op_disconnect
};

int
mikuterm_extdisp_start(void)
{
	WSADATA wsa;
	struct sockaddr_in addr;
	int reuse = 1;

	if (extdisp.initialized)
		return 0;

	memset(&extdisp, 0, sizeof(extdisp));
	extdisp.disc_sock = INVALID_SOCKET;
	extdisp.sess.listen_sock = INVALID_SOCKET;
	extdisp.sess.client_sock = INVALID_SOCKET;
	InitializeCriticalSection(&extdisp.lock);
	(void)snprintf(extdisp.status_text, sizeof(extdisp.status_text), "Listening for Display Sink Targets...");

	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return -1;
	extdisp.wsa_started = 1;

	extdisp.disc_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (extdisp.disc_sock == INVALID_SOCKET)
		return -1;
	(void)setsockopt(extdisp.disc_sock, SOL_SOCKET, SO_REUSEADDR,
	    (const char *)&reuse, sizeof(reuse));
	(void)setsockopt(extdisp.disc_sock, SOL_SOCKET, SO_BROADCAST,
	    (const char *)&reuse, sizeof(reuse));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons((uint16_t)NETDISP_DISCOVERY_PORT);
	if (bind(extdisp.disc_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		closesocket(extdisp.disc_sock);
		extdisp.disc_sock = INVALID_SOCKET;
		return -1;
	}

	extdisp.disc_thread = CreateThread(NULL, 0, extdisp_discovery_thread, NULL, 0, NULL);
	if (extdisp.disc_thread == NULL) {
		closesocket(extdisp.disc_sock);
		extdisp.disc_sock = INVALID_SOCKET;
		return -1;
	}
	extdisp.initialized = 1;
	return 0;
}

void
mikuterm_extdisp_stop(void)
{
	if (!extdisp.initialized)
		return;
	extdisp.stop = 1;
	extdisp_op_disconnect(NULL);
	if (extdisp.disc_thread != NULL) {
		WaitForSingleObject(extdisp.disc_thread, 2000);
		CloseHandle(extdisp.disc_thread);
		extdisp.disc_thread = NULL;
	}
	if (extdisp.disc_sock != INVALID_SOCKET) {
		closesocket(extdisp.disc_sock);
		extdisp.disc_sock = INVALID_SOCKET;
	}
	if (extdisp.wsa_started) {
		WSACleanup();
		extdisp.wsa_started = 0;
	}
	DeleteCriticalSection(&extdisp.lock);
	extdisp.initialized = 0;
}

const struct terminal_extdisp_ops *
mikuterm_extdisp_ops(void)
{
	return &extdisp_ops_table;
}
