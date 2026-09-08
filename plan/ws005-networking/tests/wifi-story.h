/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef WIFI_STORY_H
#define WIFI_STORY_H
#include "userland/base/net/netutil.h"
#include "userland/base/net/wifi-store.h"
#include "userland/base/networkd/managed-wlan.h"
#include <stdio.h>
#include <time.h>
#include <sys/socket.h>

#define STORY_RADIOS 2
struct story_radio {
 int present, up, visible;
 unsigned state, scan, scans, polls, connect_polls;
 int up_error, scan_error, list_error, malformed, auth_error, disconnect_error;
 int status_error, stop_pending, stop_error, scan_delay, rapid_scan;
 int connect_error;
 unsigned connect_delay;
 unsigned index, generation, snapshot, connect_generation;
 unsigned connections, down_calls, connection_attempts;
 uint32_t address, netmask, broadcast;
 char ssid[33];
};
struct story_world {
 uint64_t now;
 unsigned uid, dhcp_count, dhcp_fail, max_links, next_generation;
 int eof_exit_delay, child_wait_fault, child_deadline, stop_fault;
 int last_child_connect;
 unsigned concurrent_requests;
 int resolver_present, route_present;
 uint32_t route_index;
 char resolver[128];
 struct story_radio radios[STORY_RADIOS];
 char stores[2][32768];
 size_t store_lengths[2];
 unsigned invalid_store;
};
extern struct story_world *story;
extern char story_output[65536];
int story_ioctl(int, unsigned long, ...);
int story_socket(int, int, int);
int story_setsockopt(int, int, int, const void *, socklen_t);
int story_getsockopt(int, int, int, void *, socklen_t *);
int story_open(const char *, int, ...);
int story_unlink(const char *);
int story_clock_gettime(clockid_t, struct timespec *);
int story_nanosleep(const struct timespec *, struct timespec *);
int story_execv(const char *, char *const []);
pid_t story_waitpid(pid_t, int *, int);
int story_accept4(int, struct sockaddr *, socklen_t *, int);
pid_t story_concurrent_begin(void);
void story_concurrent_end(pid_t);
int story_wifi_printf(const char *, ...);
int story_client_socket(int, int, int);
int story_client_connect(int, const struct sockaddr *, socklen_t);
int story_client_shutdown(int, int);
int story_net_command(int, char **);
int story_wifi_main(int, char **);
void story_daemon_handle(int);
void story_daemon_reset(void);
void story_daemon_tick(void);
void story_daemon_listen(int);
void story_daemon_service(void);
void story_daemon_event(unsigned, int, int);
int story_daemon_state(void);
unsigned story_daemon_owner(void);
int story_daemon_l3(void);
const char *story_daemon_interface(void);
void story_check(int, const char *);
#endif
