/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Remote Desktop Protocol server daemon (rdpd): TLS listener, passkey
 * authentication, headless Wayland session management, /dev/input/bridge
 * virtual HID injection, and MS-RDPEGFX H.264 streaming.
 */

#ifndef RDPD_H
#define RDPD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RDPD_DEFAULT_PORT	3389U
#define RDPD_CERT_PATH		"/etc/rdpd/cert.pem"
#define RDPD_KEY_PATH		"/etc/rdpd/key.pem"
#define RDPD_PASSKEY_PATH	"/sbin/passkey"
#define RDPD_WAYLAND_PATH	"/bin/wayland"
#define RDPD_SESSION_PATH	"/etc/keiland/session"
#define RDPD_INPUT_BRIDGE	"/dev/input/bridge"

/* Virtual HID state (keyboard + absolute mouse over /dev/input/bridge). */
struct rdpd_hid {
	int fd;
	uint32_t event_dev;
	uint32_t width;
	uint32_t height;
	uint8_t modifiers;
	uint8_t keys[6];
	uint8_t buttons;
};

/* Active headless Wayland + desktop session owned by one RDP client. */
struct rdpd_session {
	pid_t compositor_pid;
	pid_t session_pid;
	int rdp_fd;
	int sessiond_fd;
	uint32_t width;
	uint32_t height;
	char wayland_socket[128];
};

/* Builds the composite HID report descriptor (keyboard + absolute mouse). */
size_t rdpd_hid_descriptor(uint8_t *out, size_t cap);

/* Maps a PS/2 Set 1 scancode + RDP_KEY_* flags to a USB HID Usage ID (0x07). */
uint8_t rdpd_ps2_to_hid_usage(uint16_t scancode, uint16_t flags);

/* Formats a 9-byte keyboard HID report (Report ID 1) into report[9]. */
int rdpd_hid_format_key(struct rdpd_hid *hid, uint16_t scancode, uint16_t flags, uint8_t report[9]);

/* Formats a 7-byte absolute mouse HID report (Report ID 2) into report[7]. */
int rdpd_hid_format_mouse(struct rdpd_hid *hid, uint16_t flags, uint16_t x, uint16_t y, uint8_t report[7]);

/* Opens /dev/input/bridge and registers the virtual HID device. */
int rdpd_hid_open(struct rdpd_hid *hid, uint32_t width, uint32_t height);

/* Updates desktop dimensions used for mouse coordinate scaling. */
void rdpd_hid_set_size(struct rdpd_hid *hid, uint32_t width, uint32_t height);

/* Injects a keyboard event into /dev/input/bridge. */
void rdpd_hid_key(struct rdpd_hid *hid, uint16_t scancode, uint16_t flags);

/* Injects a mouse event into /dev/input/bridge. */
void rdpd_hid_mouse(struct rdpd_hid *hid, uint16_t flags, uint16_t x, uint16_t y);

/* Closes the /dev/input/bridge descriptor. */
void rdpd_hid_close(struct rdpd_hid *hid);

/* Authenticates user/password via /sbin/passkey and returns account info. */
int rdpd_auth_user(const char *user, const char *password, uid_t *out_uid, gid_t *out_gid, char *out_home, size_t home_cap, char *out_shell, size_t shell_cap);

/* Launches headless wayland (--headless --rdp-fd=N --seat-fd=4) via sessiond seat isolation. */
int rdpd_session_start(struct rdpd_session *sess, uid_t uid, gid_t gid, const char *user, const char *home, const char *shell, uint32_t width, uint32_t height, uint32_t event_dev);

/* Sends a RESIZE control message to the headless compositor. */
void rdpd_session_resize(struct rdpd_session *sess, uint32_t width, uint32_t height);

/* Sends a FORCE_KEY control message to the headless compositor. */
void rdpd_session_force_key(struct rdpd_session *sess);

/* Terminates the headless compositor and session processes. */
void rdpd_session_stop(struct rdpd_session *sess);

#ifdef __cplusplus
}
#endif

#endif
