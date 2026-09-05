/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares networkd's global managed-WLAN policy state.
 */

#ifndef ZEDBSD_NETWORKD_MANAGED_WLAN_H
#define ZEDBSD_NETWORKD_MANAGED_WLAN_H

#include <net/if.h>
#include <net/route.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <zedbsd/wlan.h>

#define NETWORKD_MANAGED_RESOLVER_MAX	1024U

enum networkd_managed_wlan_state {
	NETWORKD_WLAN_DISABLED,
	NETWORKD_WLAN_AUTO_SEARCHING,
	NETWORKD_WLAN_CONNECTING,
	NETWORKD_WLAN_CONNECTED,
	NETWORKD_WLAN_MANUAL_DISCONNECTED,
	NETWORKD_WLAN_RECONNECTING
};

enum networkd_managed_wlan_action {
	NETWORKD_WLAN_ACTION_NONE,
	NETWORKD_WLAN_ACTION_RECOVER,
	NETWORKD_WLAN_ACTION_RETIRE,
	NETWORKD_WLAN_ACTION_RESNAPSHOT
};

struct networkd_managed_route {
	uint32_t flags;
	uint32_t ifindex;
	uint32_t destination;
	uint32_t gateway;
	uint32_t netmask;
};

struct networkd_managed_l3 {
	uint32_t address;
	uint32_t netmask;
	uint32_t broadcast;
	int ipv4_owned;
	int default_route_present;
	int default_route_owned;
	struct networkd_managed_route default_route;
	int resolver_present;
	int resolver_owned;
	size_t resolver_length;
	unsigned char resolver[NETWORKD_MANAGED_RESOLVER_MAX];
};

struct networkd_managed_l3_cleanup {
	int clear_ipv4;
	int delete_default_route;
	int unlink_resolver;
	int degraded;
};

struct networkd_managed_wlan_connection {
	char interface[IFNAMSIZ];
	uint32_t ifindex;
	uint64_t device_generation;
	uint64_t event_floor;
	uint64_t last_event_sequence;
	unsigned char ssid[WLAN_SSID_MAX];
	size_t ssid_length;
	int owns_l3;
	struct networkd_managed_l3 l3;
};

struct networkd_managed_wlan {
	enum networkd_managed_wlan_state state;
	uid_t owner_uid;
	int owner_valid;
	struct networkd_managed_wlan_connection connection;
};

void networkd_managed_wlan_init(struct networkd_managed_wlan *);
int networkd_managed_wlan_enable(struct networkd_managed_wlan *, uid_t);
int networkd_managed_wlan_disable(struct networkd_managed_wlan *);
int networkd_managed_wlan_owner_matches(const struct networkd_managed_wlan *, uid_t);
int networkd_managed_wlan_begin_connect(struct networkd_managed_wlan *, const char *, uint32_t, uint64_t, const void *, size_t);
int networkd_managed_wlan_commit_l3(struct networkd_managed_wlan *, const struct networkd_managed_l3 *);
int networkd_managed_wlan_finish_connection(struct networkd_managed_wlan *, enum networkd_managed_wlan_state);
int networkd_managed_wlan_plan_l3_cleanup(const struct networkd_managed_wlan *, uint32_t, const struct networkd_managed_l3 *, struct networkd_managed_l3_cleanup *);
enum networkd_managed_wlan_action networkd_managed_wlan_event(struct networkd_managed_wlan *, const struct rtm_ifinfo *);
void networkd_managed_wlan_recovery_complete(struct networkd_managed_wlan *, int);

#endif
