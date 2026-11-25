/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Network Display discovery, encrypted offer, virtual extended kwl_head,
 * dedicated H.264 encoder thread, and direct seat input injection.
 */

#ifndef KWL_NETDISP_H
#define KWL_NETDISP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KWL_NETDISP_MAX		16U

#define KWL_NETDISP_IDLE	0U
#define KWL_NETDISP_CONNECTING	1U
#define KWL_NETDISP_CONNECTED	2U

struct kwl_server;
struct kwl_compose;
struct kwl_head;
struct kwl_rdp_ext;

struct kwl_netdisp_info {
	char device_id[33];
	char name[64];
	char ip[64];
	char fingerprint[16];
	uint32_t width;
	uint32_t height;
	uint32_t scale120;
	unsigned state;
};

/* Starts the UDP discovery thread for non-headless sessions. */
void kwl_netdisp_start(struct kwl_server *server);

/* Stops the UDP discovery thread and closes any active virtual RDP head. */
void kwl_netdisp_stop(struct kwl_server *server);

/* Processes discovery updates and prunes timed-out advertisements. */
void kwl_netdisp_tick(struct kwl_server *server, uint64_t now_ms);

/* Returns ms until next expiry check, or -1 if none pending. */
int kwl_netdisp_timeout(const struct kwl_server *server, uint64_t now_ms);

/* Copies the current list of discovered network displays. */
size_t kwl_netdisp_snapshot(const struct kwl_server *server, struct kwl_netdisp_info *out, size_t cap);

/* Initiates connection to a discovered network display (requests one-shot credentials). */
int kwl_netdisp_connect(struct kwl_server *server, const char *device_id_hex);

/* Disconnects the active virtual RDP head (if matching device_id_hex or empty). */
int kwl_netdisp_disconnect(struct kwl_server *server, const char *device_id_hex);

/* Backend callback when sessiond returns one-shot credentials. */
void kwl_netdisp_extend_prepared(void *data, int error, const char *user, const char *password, unsigned port);

/* Backend callback when sessiond delivers rdp_fd via SCM_RIGHTS. */
void kwl_netdisp_extend_attach(void *data, int rdp_fd, uint32_t width, uint32_t height);

/* Copies rendered offscreen image to staging and signals encoder thread. */
void kwl_netdisp_copy_frame(struct kwl_compose *compose, struct kwl_head *head);

/* Registers active rdp_fd in server->watched for POLLIN. */
int kwl_netdisp_rdp_fd(const struct kwl_server *server);

/* Reads and processes control/input messages from the active virtual RDP head. */
void kwl_netdisp_poll(struct kwl_server *server);

/* Returns 1 if a virtual RDP head is currently open. */
int kwl_netdisp_active(const struct kwl_server *server);

#ifdef __cplusplus
}
#endif

#endif
