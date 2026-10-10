/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The interfaces of Keiland's system extension as libwayland marshals
 * them (system-protocol.c; libkeiland/system/kl-system-protocol.h has the opcodes).
 * The settings (settings.c) and the system (system.c) share them; they
 * stay inside the library (exports.map, WS131 review 17).
 */

#ifndef KEILAND_SYSTEM_PROTOCOL_H
#define KEILAND_SYSTEM_PROTOCOL_H

#include <wayland-client.h>

extern const struct wl_interface kl_system_manager_v1_interface;
extern const struct wl_interface kl_system_settings_v1_interface;
extern const struct wl_interface kl_system_network_v1_interface;
extern const struct wl_interface kl_system_audio_v1_interface;
extern const struct wl_interface kl_system_power_v1_interface;
extern const struct wl_interface kl_system_devices_v1_interface;
extern const struct wl_interface kl_system_monitor_v1_interface;
extern const struct wl_interface kl_system_account_v1_interface;
extern const struct wl_interface kl_system_sharing_v1_interface;
extern const struct wl_interface kl_system_notify_v1_interface;
extern const struct wl_interface kl_system_mail_v1_interface;
extern const struct wl_interface kl_system_phone_v1_interface;
extern const struct wl_interface kl_system_printers_v1_interface;
extern const struct wl_interface kl_system_bluetooth_v1_interface;
extern const struct wl_interface kl_system_displays_v1_interface;
extern const struct wl_interface kl_system_machine_v1_interface;
extern const struct wl_interface kl_system_media_v1_interface;

#endif
