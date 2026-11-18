/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The states of networkd's managed-WLAN policy (managed-wlan.h), apart from
 * the policy's network types so that a part that only names a state
 * (sleep-state.h, ws052-p010) and its host tests need no network headers.
 */

#ifndef KERN_NETWORKD_MANAGED_WLAN_STATE_H
#define KERN_NETWORKD_MANAGED_WLAN_STATE_H

enum networkd_managed_wlan_state {
	NETWORKD_WLAN_DISABLED,
	NETWORKD_WLAN_AUTO_SEARCHING,
	NETWORKD_WLAN_CONNECTING,
	NETWORKD_WLAN_CONNECTED,
	NETWORKD_WLAN_MANUAL_DISCONNECTED,
	NETWORKD_WLAN_RECONNECTING,
	NETWORKD_WLAN_RETIRING
};

#endif
