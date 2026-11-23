/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of zedBSD's xserver program around the core (x11server.h):
 * the Wayland desktop as the backend (wayland.c) and the AF_UNIX socket
 * the clients connect to as the transport (unix.c).  main.c runs them in
 * one poll(2) loop.
 */

#ifndef KEILAND_XSERVER_PROGRAM_H
#define KEILAND_XSERVER_PROGRAM_H

#include "userland/desktop/xserver/x11backend.h"

#include <poll.h>

/* The Vulkan WSI extension of the Wayland backend's surfaces. */
#define X11_WAYLAND_WSI		"VK_KHR_wayland_surface"

/*
 * The most clients the socket serves at once (as many as the core takes
 * when its options leave clients_max 0, so that the core is the one that
 * refuses a connection, and says so in its log), and the most descriptors
 * the transport waits on (the listening socket and the clients).
 */
#define X11_UNIX_CLIENTS	64U
#define X11_UNIX_POLLFDS	(1U + X11_UNIX_CLIENTS)

/* The connection to the desktop (wayland.c), and the socket and its clients (unix.c). */
struct x11_wayland;
struct x11_unix;

/* wayland.c: the Wayland desktop as the core's backend. */
extern const struct x11_backend_ops x11_wayland_ops;
int x11_wayland_open(struct x11_wayland **result, const char *display);
void x11_wayland_attach(struct x11_wayland *wayland, struct x11server *server);
int x11_wayland_fd(const struct x11_wayland *wayland);
int x11_wayland_dispatch(struct x11_wayland *wayland, int readable);
void x11_wayland_close(struct x11_wayland *wayland);

/* unix.c: the listening socket and its clients' connections as the core's transport. */
int x11_unix_open(struct x11_unix **result, const char *path);
unsigned x11_unix_pollfds(struct x11_unix *transport, struct x11server *server, struct pollfd *descriptors, unsigned capacity);
void x11_unix_accept(struct x11_unix *transport, struct x11server *server, const struct pollfd *descriptors, unsigned count);
void x11_unix_read(struct x11_unix *transport, struct x11server *server);
void x11_unix_write(struct x11_unix *transport, struct x11server *server);
void x11_unix_close(struct x11_unix *transport);

#endif
