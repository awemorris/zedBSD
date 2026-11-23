/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * mikuterm's X server process on Windows (mikuterm.exe --x11-server): one
 * X server of one SSH connection, its windows on this desktop, and its
 * clients over the link to the mikuterm that started it.
 *
 * The process reads the description of its socket's end from its
 * standard input (a pipe the parent closes), says HELLO, and takes the
 * parent's HELLO with the settings (the host's name, the fake cookie, the
 * scale, the clipboard's policy, the root window shown as one window or
 * not, the windows shown in mikuterm's tabs or not: x11-tab-child.h).
 * Then one thread runs the X server's core and the Windows backend and
 * the link (x11-serve.h): each OPEN is an X client checked against the
 * cookie, its DATA the client's bytes, and the client's replies go back
 * as DATA within the parent's credit.  The process ends with the link, on
 * SHUTDOWN, or when the parent's frames break the rules; it shows no
 * message of its own.
 */

#include "mikuterm.h"
#include "x11-host.h"
#include "x11-link.h"
#include "x11-serve.h"
#include "x11-tab-child.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include "userland/desktop/xserver/win32/backend.h"
#include "userland/desktop/xserver/x11server.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The Vulkan instance extension the backend's windows are shown through. */
#define CHILD_VULKAN_WSI	"VK_KHR_win32_surface"

/* The longest wait for the link (ms). */
#define CHILD_WAIT_MS		1000U

/*
 * The X server process's state: the link's socket and its event, the
 * link's side (x11-serve.h), and the backend (and the tabs around it,
 * NULL when the windows are not shown in tabs).
 */
struct child {
	SOCKET socket;
	WSAEVENT event;
	struct x11_serve serve;
	struct x11_win32 *backend;
	struct x11_tab_child *tabs;
};

static int child_connect(struct child *child);
static void child_round(struct child *child);
static void child_service(void *context);
static int child_receive(void *context, void *buffer, size_t size, size_t *received);
static int child_send(void *context, const void *bytes, size_t size, size_t *sent);
static int child_start_server(void *context, struct x11_serve *serve);
static int child_tab_take(void *context, const struct x11_link_frame *frame);
static void child_tab_send(void *context);
static int child_link_frame(void *context, unsigned type, uint32_t id, const void *body, uint32_t size);
static size_t child_link_waiting(void *context);

/*
 * Runs mikuterm's X server process until its link ends.  Returns the
 * process's exit code: 0 for an end the parent asked for or the link's
 * end, 1 for a start that failed, 2 for a parent that broke the rules.
 */
int
mikuterm_x11_child(void)
{
	static struct child child;
	static struct x11_serve_system system;
	DWORD now;
	uint32_t wake;
	DWORD timeout;
	HANDLE handles[1];
	unsigned waited;
	int error;
	int status;

	/* The folder (for the fonts), the link, and its side with the HELLO. */
	memset(&child, 0, sizeof(child));
	child.socket = INVALID_SOCKET;
	(void)mikuterm_folder_init();
	error = child_connect(&child);
	if (error != 0)
		return 1;
	system.receive = child_receive;
	system.send = child_send;
	system.start = child_start_server;
	system.tab_take = child_tab_take;
	system.tab_send = child_tab_send;
	system.context = &child;
	error = x11_serve_init(&child.serve, &system);
	if (error != 0)
		return 1;

	/* Round after round until the link ends. */
	status = 0;
	for (;;) {
		child_round(&child);
		if (child.serve.ending)
			break;

		/* The wait: the socket, the windows' messages, the core's next time. */
		timeout = CHILD_WAIT_MS;
		if (child.serve.server != NULL) {
			now = GetTickCount();
			wake = (uint32_t)now + CHILD_WAIT_MS;
			(void)x11server_tick(child.serve.server, (uint32_t)now, &wake);
			if (wake - (uint32_t)now < timeout)
				timeout = wake - (uint32_t)now;

			/* The replies and events the tick made go to the parent before the wait, not after it. */
			x11_serve_flush(&child.serve);
			if (child.serve.ending)
				break;
		}
		handles[0] = child.event;
		if (child.backend != NULL) {
			waited = x11_win32_wait(child.backend, (void *const *)handles, 1, timeout);
			if (waited == X11_WIN32_WAIT_QUIT)
				break;
		} else {
			(void)WaitForSingleObject(child.event, timeout);
		}
	}
	if (child.serve.ending == X11_SERVE_BROKEN)
		status = 2;

	/* The end: the core and the fonts' bytes, the tabs, the backend, the link. */
	x11_serve_end(&child.serve);
	if (child.tabs != NULL)
		x11_tab_child_destroy(child.tabs);
	if (child.backend != NULL)
		x11_win32_destroy(child.backend);
	closesocket(child.socket);
	WSACloseEvent(child.event);
	WSACleanup();
	return status;
}

/*
 * Opens the link: the socket's description from standard input, the
 * socket, its event (which makes it non-blocking).  Returns 0, or an
 * errno value.
 */
static int
child_connect(
	struct child *child)
{
	WSADATA data;
	WSAPROTOCOL_INFOW information;
	HANDLE input;
	DWORD got;
	DWORD total;
	BOOL done;

	/* WinSock. */
	if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
		return EIO;

	/* The description, whole, from the pipe. */
	input = GetStdHandle(STD_INPUT_HANDLE);
	total = 0;
	while (total < sizeof(information)) {
		done = ReadFile(input, (uint8_t *)&information + total, sizeof(information) - total, &got, NULL);
		if (!done || got == 0)
			return EIO;
		total += got;
	}
	CloseHandle(input);

	/* The socket, and its event. */
	child->socket = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &information, 0, 0);
	if (child->socket == INVALID_SOCKET)
		return EIO;
	child->event = WSACreateEvent();
	if (child->event == WSA_INVALID_EVENT)
		return EIO;
	if (WSAEventSelect(child->socket, child->event, FD_READ | FD_WRITE | FD_CLOSE) != 0)
		return EIO;

	/* Succeeded: the link is open. */
	return 0;
}

/*
 * Does one round of work that does not wait: what the parent sent, the
 * core's events and the clients' replies, and the frames.
 */
static void
child_round(
	struct child *child)
{
	WSANETWORKEVENTS events;

	/* Clears the socket's event; what it says is read below. */
	(void)WSAEnumNetworkEvents(child->socket, child->event, &events);

	/* The link's round. */
	x11_serve_round(&child->serve);
}

/* Keeps the server and the link going while Windows runs its own loop (a window moved or sized). */
static void
child_service(
	void *context)
{
	struct child *child;
	uint32_t wake;

	/* The core's time, and a round. */
	child = context;
	if (child->serve.server != NULL)
		(void)x11server_tick(child->serve.server, (uint32_t)GetTickCount(), &wake);
	child_round(child);
}

/* Reads what the link has, without waiting (x11-serve.h's receive). */
static int
child_receive(
	void *context,
	void *buffer,
	size_t size,
	size_t *received)
{
	struct child *child;
	int got;

	/* What waits; none is EAGAIN. */
	child = context;
	*received = 0;
	got = recv(child->socket, (char *)buffer, (int)size, 0);
	if (got == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
		return EAGAIN;
	if (got < 0)
		return EPIPE;

	/* Succeeded: the bytes (0: the link ended). */
	*received = (size_t)got;
	return 0;
}

/* Writes what the link takes, without waiting (x11-serve.h's send). */
static int
child_send(
	void *context,
	const void *bytes,
	size_t size,
	size_t *sent)
{
	struct child *child;
	int put;

	/* What the socket takes; no room is EAGAIN. */
	child = context;
	*sent = 0;
	put = send(child->socket, (const char *)bytes, (int)size, 0);
	if (put == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
		return EAGAIN;
	if (put <= 0)
		return EPIPE;

	/* Succeeded: the bytes sent. */
	*sent = (size_t)put;
	return 0;
}

/*
 * Starts the backend and the core: the windows of the host, the root the
 * size of the virtual screen (or, shown as one window, the size the
 * settings give), the core fonts from mikuterm's folder, the windows
 * drawn and shown through the system's Vulkan when it has one (and
 * X11Render is not cpu), else drawn in memory and shown through GDI.
 * With the windows in mikuterm's tabs, the tabs wrap the backend and
 * every window is shown through the backend's present (drawn by Vulkan
 * or not), whose pixels a tab sends.  Returns 0, or an errno value.
 */
static int
child_start_server(
	void *context,
	struct x11_serve *serve)
{
	struct child *child;
	struct x11_win32_options backend_options;
	struct x11_tab_child_link link;
	struct x11server_options options;
	unsigned width;
	unsigned height;
	int error;

	/* The backend. */
	child = context;
	memset(&backend_options, 0, sizeof(backend_options));
	backend_options.host = serve->host;
	backend_options.scale_percent = serve->scale;
	backend_options.trusted = (serve->flags & X11_HOST_TRUSTED) != 0;
	backend_options.primary_to_clipboard = (serve->flags & X11_HOST_PRIMARY) != 0;
	if (serve->root_width != 0 && serve->root_height != 0)
		backend_options.rooted = 1;
	backend_options.service = child_service;
	backend_options.service_context = child;
	error = x11_win32_create(&backend_options, &child->backend);
	if (error != 0)
		return error;

	/* The core, its root the virtual screen in X's pixels, or one window of the size the settings give. */
	x11_win32_root_size(child->backend, &width, &height);
	memset(&options, 0, sizeof(options));
	options.mode = X11SERVER_ROOTLESS;
	if (serve->root_width != 0 && serve->root_height != 0) {
		width = serve->root_width;
		height = serve->root_height;
		options.mode = X11SERVER_ROOTED;
	}
	options.width = width;
	options.height = height;
	options.backend = &x11_win32_ops;
	options.backend_context = child->backend;
	options.vulkan_wsi = NULL;

	/* The system's Vulkan loader, unless the processor is asked to draw; without one the core draws in memory. */
	if ((serve->flags & X11_HOST_RENDER_CPU) == 0U) {
		error = x11_win32_vulkan_loader(child->backend, &options.vulkan_loader);
		if (error == 0)
			options.vulkan_wsi = CHILD_VULKAN_WSI;
	}

	/* The windows in mikuterm's tabs: the tabs around the backend, and no swapchain (a tab's pixels are presented). */
	if ((serve->flags & X11_HOST_TABIFY) != 0U) {
		link.frame = child_link_frame;
		link.waiting = child_link_waiting;
		link.context = child;
		error = x11_tab_child_create(child->backend, &link, &child->tabs);
		if (error != 0)
			return error;
		options.backend = &x11_tab_child_ops;
		options.backend_context = child->tabs;
		options.vulkan_wsi = NULL;
	}
	x11_serve_options(serve, &options);
	error = x11server_create(&options, (uint32_t)GetTickCount(), &serve->server);
	if (error != 0)
		return error;

	/* Succeeded: the backend and the tabs hear the core. */
	x11_win32_attach(child->backend, serve->server);
	if (child->tabs != NULL)
		x11_tab_child_attach(child->tabs, serve->server);
	return 0;
}

/* Hands a tab's frame from the parent to the tabs (x11-serve.h's tab_take). */
static int
child_tab_take(
	void *context,
	const struct x11_link_frame *frame)
{
	struct child *child;
	int error;

	/* An X server without tabs has none to take. */
	child = context;
	if (child->tabs == NULL)
		return 0;
	error = x11_tab_child_take(child->tabs, frame, (uint32_t)GetTickCount());
	return error;
}

/* Lets the tabs send their pixels after the clients' replies (x11-serve.h's tab_send). */
static void
child_tab_send(
	void *context)
{
	struct child *child;

	/* Only with tabs. */
	child = context;
	if (child->tabs != NULL)
		x11_tab_child_send(child->tabs, (uint32_t)GetTickCount());
}

/* Queues one frame of a tab for the parent (the tabs' link).  Returns 0, or ENOMEM. */
static int
child_link_frame(
	void *context,
	unsigned type,
	uint32_t id,
	const void *body,
	uint32_t size)
{
	struct child *child;
	int error;

	/* The frame after the others. */
	child = context;
	error = x11_serve_frame(&child->serve, type, id, body, size);
	if (error != 0)
		return error;

	/* Succeeded: queued. */
	return 0;
}

/* Tells the tabs how many bytes wait for the socket. */
static size_t
child_link_waiting(
	void *context)
{
	struct child *child;

	/* The frames queued. */
	child = context;
	return x11_serve_waiting(&child->serve);
}
