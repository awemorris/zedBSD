/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Shared IPC definitions between the mikuterm-idd UMDF 2 / IddCx 1.4
 * virtual display driver and the mikuterm.exe Extended Display host.
 */

#ifndef MIKUTERM_IDD_IPC_H
#define MIKUTERM_IDD_IPC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIKUTERM_IDD_HARDWARE_ID	L"Root\\MikutermIdd"
#define MIKUTERM_IDD_INSTANCE_ID	L"MikutermIdd0"

#define MIKUTERM_IDD_SHM_NAME		L"Global\\MikutermIddFrameRing"
#define MIKUTERM_IDD_EVENT_NAME		L"Global\\MikutermIddFrameEvent"

#define MIKUTERM_IDD_SHM_MAGIC		0x4D494444U
#define MIKUTERM_IDD_SHM_VERSION	1U
#define MIKUTERM_IDD_MAX_WIDTH		3840U
#define MIKUTERM_IDD_MAX_HEIGHT		2160U
#define MIKUTERM_IDD_RING_SLOTS		2U
#define MIKUTERM_IDD_SLOT_BYTES		(MIKUTERM_IDD_MAX_WIDTH * MIKUTERM_IDD_MAX_HEIGHT * 4U)

/* {6B7A9E21-4C3B-4D1A-9F52-88C1E3A40F01} */
#define MIKUTERM_IDD_INTERFACE_GUID_INIT \
	{ 0x6b7a9e21U, 0x4c3bU, 0x4d1aU, { 0x9fU, 0x52U, 0x88U, 0xc1U, 0xe3U, 0xa4U, 0x0fU, 0x01U } }

#define FILE_DEVICE_MIKUTERM_IDD	0x8000U
#ifndef CTL_CODE
#define CTL_CODE(DeviceType, Function, Method, Access) \
	(((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))
#endif
#ifndef METHOD_BUFFERED
#define METHOD_BUFFERED			0U
#endif
#ifndef FILE_ANY_ACCESS
#define FILE_ANY_ACCESS			0U
#endif

#define IOCTL_MIKUTERM_IDD_SET_MODE \
	CTL_CODE(FILE_DEVICE_MIKUTERM_IDD, 0x801U, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MIKUTERM_IDD_GET_STATUS \
	CTL_CODE(FILE_DEVICE_MIKUTERM_IDD, 0x802U, METHOD_BUFFERED, FILE_ANY_ACCESS)

struct mikuterm_idd_mode {
	uint32_t width;
	uint32_t height;
	uint32_t refresh_hz;
	uint32_t reserved;
};

struct mikuterm_idd_status {
	uint32_t version;
	uint32_t connected;
	uint32_t width;
	uint32_t height;
	uint32_t refresh_hz;
	uint32_t frame_seq;
};

struct mikuterm_idd_shm_header {
	uint32_t magic;
	uint32_t version;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t slot_index;
	volatile uint32_t frame_seq;
	uint32_t active;
};

#define MIKUTERM_IDD_SHM_TOTAL_SIZE \
	(sizeof(struct mikuterm_idd_shm_header) + (size_t)MIKUTERM_IDD_RING_SLOTS * (size_t)MIKUTERM_IDD_SLOT_BYTES)

#ifdef __cplusplus
}
#endif

#endif
