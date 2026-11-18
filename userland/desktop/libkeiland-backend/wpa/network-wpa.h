/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Private control connections shared by the watch and credential operations.
 */
#ifndef KL_BACKEND_NETWORK_WPA_H
#define KL_BACKEND_NETWORK_WPA_H
#include "userland/desktop/libkeiland-backend/keiland-backend.h"
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>

#define KWPA_REPLY_MAX 4096U

/* One connection owns its private directory and socket until kwpa_close. */
struct kwpa_socket {
	int fd;
	char directory[64];
	char path[108];
	char interface[KL_BACKEND_NETWORK_NAME_MAX];
};

int kwpa_open(struct kwpa_socket *connection, const char *interface);
void kwpa_close(struct kwpa_socket *connection);
int kwpa_send(struct kwpa_socket *connection, const char *command);
int kwpa_read(struct kwpa_socket *connection, char *reply, size_t capacity);
int kwpa_call(struct kwpa_socket *connection, const char *command, char *reply, size_t capacity);
int kwpa_ok(const char *reply);
void kwpa_decode(const char *encoded, char *ssid, size_t capacity);
int kwpa_profile(char *reply, const char *ssid, unsigned *id);
uint64_t kwpa_milliseconds(void);
int kwpa_radio(const char *interface, unsigned enabled);
void kwpa_links(struct kl_backend_network_state *state);
const char *kwpa_control_directory(void);
int kwpa_socket_address(struct sockaddr_un *address, const char *path, socklen_t *length);
int kwpa_wireless(const char *interface);
int kwpa_link_usable(const struct kl_backend_network_link *link);
void kwpa_link_inet6(struct kl_backend_network_link *link, const struct sockaddr *address, const struct sockaddr *netmask);
#endif
