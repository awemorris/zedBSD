/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the private net-to-networkd protocol.
 */

#ifndef KERN_NETWORKD_PROTOCOL_H
#define KERN_NETWORKD_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifndef NETWORKD_SOCKET
#define NETWORKD_SOCKET "/run/networkd.sock"
#endif

#define NETWORKD_PROTOCOL_MAGIC		"ZNV2"
#define NETWORKD_PROTOCOL_HEADER_MAX	32U
#define NETWORKD_REQUEST_MAX		4096U
#define NETWORKD_RESPONSE_MAX		32768U
#define NETWORKD_DIAGNOSTIC_MAX		512U
#define NETWORKD_CONFIRMED_MINUTES_MAX	1440U
#define NETWORKD_ROLLBACK_OPERATION_MAX	64U
#define NETWORKD_ROLLBACK_LINE_MAX	4096U
#define NETWORKD_ROLLBACK_PROGRAM_MAX	32768U
#define NETWORKD_ROLLBACK_DIAGNOSTIC_MAX	512U
#define NETWORKD_ROLLBACK_PATH_MAX	255U

/*
 * Present while Wi-Fi was last turned off by request: networkd makes it on
 * an explicit off and removes it on an explicit on, and a boot (net startup)
 * leaves Wi-Fi off while it is there (2026-10-03 user decision, ws005-p020).
 */
#define NETWORKD_WIFI_OFF_DIRECTORY	"/var/db"
#define NETWORKD_WIFI_OFF_PATH		"/var/db/wifi-off"

/* How long one WIFI_SCAN_START asks for fresh scans, in seconds (ws089-p021). */
#define NETWORKD_WIFI_SCAN_LEASE_SECONDS	30U

/* One daemon transaction, including teardown; clients allow transport margin. */
#define NETWORKD_WIFI_CLEANUP_SECONDS	10U
#define NETWORKD_WIFI_TRANSPORT_MARGIN	15U
/* A wired request can first wait for a background Wi-Fi actor to retire. */
#define NETWORKD_CONTROL_YIELD_SECONDS	(NETWORKD_WIFI_CLEANUP_SECONDS + 5U)
#define NETWORKD_WIFI_REQUEST_SECONDS(op) \
	((op) == NETWORKD_OP_WIFI_CONNECT || (op) == NETWORKD_OP_WIFI_ENABLE ? \
	90U : ((op) == NETWORKD_OP_WIFI_PROFILES_CHANGED || \
	(op) == NETWORKD_OP_WIFI_SESSION_OPEN ? 2U : 30U))

enum networkd_opcode {
	NETWORKD_OP_SHOW = 1,
	NETWORKD_OP_UP = 2,
	NETWORKD_OP_DOWN = 3,
	NETWORKD_OP_DHCP = 4,
	NETWORKD_OP_STATIC = 5,
	NETWORKD_OP_DEFAULT_ROUTE = 6,
	NETWORKD_OP_DNS = 7,
	NETWORKD_OP_RELOAD = 8,
	NETWORKD_OP_DEFAULT_ROUTE_CLEAR = 9,
	NETWORKD_OP_DNS_CLEAR = 10,
	/* IPv6 (ws130-p005): IPv6 on or off at an interface, a static address, a route, and the default route removed. */
	NETWORKD_OP_IPV6 = 11,
	NETWORKD_OP_STATIC6 = 12,
	NETWORKD_OP_ROUTE6 = 13,
	NETWORKD_OP_ROUTE6_CLEAR = 14,
	/* ws177-p045: a static IPv6 address and an IPv6 route that net.conf no longer names, taken away. */
	NETWORKD_OP_STATIC6_REMOVE = 15,
	NETWORKD_OP_CONFIRMED_ARM = 16,
	NETWORKD_OP_CONFIRMED_DISARM = 17,
	NETWORKD_OP_CONFIRMED_ROLLBACK = 18,
	NETWORKD_OP_CONFIRMED_CHECK = 19,
	NETWORKD_OP_ROUTE6_REMOVE = 20,
	NETWORKD_OP_WIFI_ENABLE = 32,
	NETWORKD_OP_WIFI_DISABLE = 33,
	NETWORKD_OP_WIFI_LIST = 34,
	NETWORKD_OP_WIFI_CONNECT = 35,
	NETWORKD_OP_WIFI_DISCONNECT = 36,
	NETWORKD_OP_WIFI_PROFILES_CHANGED = 37,

	/*
	 * A login session opened or closed (ws005-p024, the user's decision
	 * of 2026-10-02).  While an account's session is open, the store of
	 * that account's saved networks is one networkd may join from on its
	 * own; when it closes, the store is dropped again and a connection made
	 * from it is ended.  The request names the account (ACCOUNT); networkd
	 * finds the store from the account database, never from a path a
	 * client sends, and keys never travel on this socket.  Root (sessiond)
	 * may name any account; a member of the network group only itself.
	 */
	NETWORKD_OP_WIFI_SESSION_OPEN = 38,
	NETWORKD_OP_WIFI_SESSION_CLOSE = 39,

	/*
	 * A desktop that shows the networks around asks for fresh scans
	 * (ws089-p021, the user's request of 2026-10-04).  SCAN_START asks
	 * for them for NETWORKD_WIFI_SCAN_LEASE_SECONDS, and is sent again
	 * before that runs out while the list is shown; SCAN_STOP ends the
	 * asking.  The lease is the daemon's guard against a desktop that went
	 * without saying so.  While it holds and the radios are on but not
	 * connected and not searching on their own (manual-disconnected),
	 * the daemon starts a scan every NETWORKD_WLAN_RESCAN_SECONDS; the
	 * automatic search scans on its own, and a connected radio cannot
	 * scan (the kernel refuses while it is associated).  Neither carries
	 * a field, and the answer only says the daemon heard; the scans are
	 * read with WIFI_LIST.  Every admitted peer may ask, as for WIFI_LIST.
	 */
	NETWORKD_OP_WIFI_SCAN_START = 40,
	NETWORKD_OP_WIFI_SCAN_STOP = 41,

	/*
	 * Wired management.  These say what the daemon is to do from now on
	 * rather than what it is to do now: the work they ask for is carried
	 * out in the background, and the answer says only that the daemon
	 * heard.  A caller that wants to know the outcome asks SHOW.
	 */
	NETWORKD_OP_LAN_ENABLE = 48,
	NETWORKD_OP_LAN_DISABLE = 49,

	/*
	 * One wired interface's configuration, from a member of the network
	 * group as well as root (ws089-p022, net lan set): INTERFACE alone is
	 * DHCP; with ADDRESS and NETMASK (and a GATEWAY) a static IPv4
	 * address; up to two DNS fields name static name servers (none: the
	 * servers DHCP gives).  The daemon checks every field, writes that
	 * interface's entry in net.conf (and the default route and the name
	 * servers), and applies it now; the answer says whether it did.
	 */
	NETWORKD_OP_LAN_CONFIGURE = 50,

	/*
	 * Watching the network instead of asking about it.
	 *
	 * SUBSCRIBE does not end its connection.  The daemon answers it once
	 * with the state as it stands, and then sends the same answer again
	 * whenever that state changes, until the watcher closes.  A taskbar
	 * that would otherwise ask SHOW on a timer reads this instead: it
	 * shows the change when it happens, and nothing is spent while
	 * nothing happens.
	 *
	 * Every frame on a watch carries the id of the SUBSCRIBE that opened
	 * it.  The protocol has no id zero -- the header encoder refuses it --
	 * and a watch connection carries nothing but that one subscription,
	 * so reusing its id cannot be mistaken for the answer to anything else.
	 */
	NETWORKD_OP_SUBSCRIBE = 64,

	/*
	 * The machine's sleep (ws052-p010).  sessiond (root only) asks
	 * SLEEP_PREPARE before it asks the kernel for S0 idle: networkd
	 * records the Wi-Fi policy, retires the connection and turns the
	 * radios off without changing the policy, and answers once they are
	 * off (within NETWORKD_SLEEP_PREPARE_SECONDS).  SLEEP_END after the
	 * sleep takes the recorded policy up again; it is never refused.
	 */
	NETWORKD_OP_SLEEP_PREPARE = 80,
	NETWORKD_OP_SLEEP_END = 81
};

/*
 * How long a SLEEP_PREPARE may take in the daemon, in seconds: the cleanup
 * of an automatic Wi-Fi attempt it stops, and a radio's three Wi-Fi helper
 * runs (disconnect, search-stop, down) of up to 10 seconds each.
 */
#define NETWORKD_SLEEP_PREPARE_SECONDS	(NETWORKD_WIFI_CLEANUP_SECONDS + 30U)

/*
 * How many watchers the daemon keeps.
 *
 * The daemon is the only thing that can configure the network, so a watcher
 * is a program the operator is running, not a connection from anywhere.
 * A small number is enough, and a fixed one means a watcher cannot make the
 * daemon allocate.
 */
#define NETWORKD_SUBSCRIBER_MAX		8U

enum networkd_field_type {
	NETWORKD_FIELD_STATUS = 1,
	NETWORKD_FIELD_ERROR = 2,
	NETWORKD_FIELD_STAGE = 3,
	NETWORKD_FIELD_OUTPUT = 4,
	NETWORKD_FIELD_INTERFACE = 16,
	NETWORKD_FIELD_TIMEOUT = 17,
	NETWORKD_FIELD_ADDRESS = 18,
	NETWORKD_FIELD_NETMASK = 19,
	NETWORKD_FIELD_GATEWAY = 20,
	NETWORKD_FIELD_DNS = 21,
	NETWORKD_FIELD_PATH = 22,
	NETWORKD_FIELD_TOKEN = 23,
	NETWORKD_FIELD_SSID = 32,
	/* The numeric user ID of the account a session request is about. */
	NETWORKD_FIELD_ACCOUNT = 33
};

enum networkd_result_status {
	NETWORKD_RESULT_OK = 0,
	NETWORKD_RESULT_ERROR = 1,
	NETWORKD_RESULT_DEGRADED = 2,
	NETWORKD_RESULT_NO_CANDIDATE = 3,
	NETWORKD_RESULT_IN_PROGRESS = 4
};

struct networkd_protocol_header {
	uint32_t request_id;
	uint32_t opcode;
	size_t payload_length;
};

struct networkd_field_writer {
	unsigned char *bytes;
	size_t capacity;
	size_t used;
};

struct networkd_field_reader {
	const unsigned char *bytes;
	size_t length;
	size_t offset;
};

struct networkd_field {
	uint16_t type;
	const unsigned char *value;
	size_t length;
};

int networkd_protocol_header_encode(char *, size_t,
	const struct networkd_protocol_header *, size_t *);
int networkd_protocol_header_decode(const char *, size_t,
	struct networkd_protocol_header *);
void networkd_field_writer_init(struct networkd_field_writer *, void *,
	size_t);
int networkd_field_write(struct networkd_field_writer *, uint16_t,
	const void *, size_t);
int networkd_field_write_u32(struct networkd_field_writer *, uint16_t,
	uint32_t);
void networkd_field_reader_init(struct networkd_field_reader *, const void *,
	size_t);
int networkd_field_read(struct networkd_field_reader *, struct networkd_field *);
int networkd_field_read_u32(const struct networkd_field *, uint32_t *);
int networkd_protocol_write_frame(int,
	const struct networkd_protocol_header *, const void *);
int networkd_protocol_read_frame(int, struct networkd_protocol_header *,
	void *, size_t, size_t);
void networkd_protocol_clear(void *, size_t);
int networkd_protocol_read_frame_timed(int, struct networkd_protocol_header *,
	void *, size_t, size_t, unsigned);

#endif
