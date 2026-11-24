/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host unit test for local root-tab pan, pinch-to-zoom, resize, and
 * pointer coordinate mapping in apps/mikuterm-common/x11-tab.c.
 */

#include "apps/mikuterm-common/session.h"
#include "apps/mikuterm-common/x11-tab.h"
#include "userland/desktop/terminal/connection.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned waker_rings;
static uint64_t fake_clock_us = 1000ULL;

int
kl_mutex_init(
	struct kl_mutex *mutex)
{
	memset(mutex, 0, sizeof(*mutex));
	return 0;
}

void
kl_mutex_destroy(
	struct kl_mutex *mutex)
{
	(void)mutex;
}

void
kl_mutex_lock(
	struct kl_mutex *mutex)
{
	(void)mutex;
}

void
kl_mutex_unlock(
	struct kl_mutex *mutex)
{
	(void)mutex;
}

uint64_t
kl_clock_us(void)
{
	fake_clock_us += 10000ULL;
	return fake_clock_us;
}

void
kl_socket_waker_ring(
	struct kl_socket_waker *waker)
{
	(void)waker;
	waker_rings++;
}

int
session_app_post(
	struct kl_app *app,
	uint32_t number)
{
	(void)app;
	(void)number;
	return 0;
}

void
session_release(
	struct mikuterm_session *session)
{
	(void)session;
}

static void
send_touch(
	struct x11_tab *tab,
	unsigned kind,
	int32_t id,
	int32_t x,
	int32_t y)
{
	struct terminal_picture_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.kind = kind;
	ev.id = id;
	ev.x = x;
	ev.y = y;
	tab->connection.ops->picture->input(&tab->connection, &ev);
}

int
main(void)
{
	struct mikuterm_session session;
	struct x11_tab *tab;
	struct x11_tab_input in;
	uint8_t *src;
	uint32_t *dst;
	unsigned x;
	unsigned y;
	unsigned w;
	unsigned h;
	uint32_t px;
	uint32_t c;
	uint32_t c0;
	uint32_t c1;
	int has_blended;

	memset(&session, 0, sizeof(session));
	assert(x11_tab_create(1U, 800U, 600U, 100U, "host", (const uint8_t *)"root", 4U, &tab) == 0);
	tab->touch.mode = X11_TOUCH_MODE_ROOT;
	x11_tab_attach(tab, &session, 1U);

	/* Fill 800x600 root buffer: top-left green, rest blue. */
	src = malloc(800U * 600U * 4U);
	dst = malloc(400U * 400U * 4U);
	assert(src != NULL && dst != NULL);
	for (y = 0U; y < 600U; y++) {
		for (x = 0U; x < 800U; x++) {
			uint32_t px = (x < 400U && y < 300U) ? 0xFF00FF00U : 0xFF0000FFU;
			x11_link_put32(src + ((size_t)y * 800U + x) * 4U, px);
		}
	}
	assert(x11_tab_put(tab, 0U, 0U, 800U, 600U, src) == 0);

	/* Resize viewport to 400x400: handled locally without X11_LINK_TAB_CONFIGURE. */
	waker_rings = 0U;
	assert(tab->connection.ops->resize(&tab->connection, 0U, 0U, 400U, 400U) == 0);
	assert(waker_rings == 0U);
	assert(x11_tab_take_size(tab, &w, &h) == 0);
	assert((tab->connection.ops->pending(&tab->connection) & TERMINAL_READY_OUTPUT) != 0U);

	/* Draw at fit scale (0.5): y=0..49 is background, y=50..199 is green, y=200..349 is blue. */
	assert(tab->connection.ops->picture->draw(&tab->connection, dst, 400U, 400U) == 0);
	assert(dst[10U * 400U + 200U] == 0xFF202020U);
	assert(dst[60U * 400U + 100U] == 0xFF00FF00U);
	assert(dst[250U * 400U + 300U] == 0xFF0000FFU);
	assert(dst[390U * 400U + 200U] == 0xFF202020U);
	tab->connection.ops->read(&tab->connection, NULL, 0U);
	assert((tab->connection.ops->pending(&tab->connection) & TERMINAL_READY_OUTPUT) == 0U);

	/* Pinch zoom in by 2x around (200, 200): immediate local dirty, zero network traffic on pinch moves. */
	send_touch(tab, TERMINAL_PICTURE_TOUCH_DOWN, 1, 150, 200);
	send_touch(tab, TERMINAL_PICTURE_TOUCH_DOWN, 2, 250, 200);
	while (x11_tab_take_input(tab, &in) != 0)
		;
	waker_rings = 0U;
	send_touch(tab, TERMINAL_PICTURE_TOUCH_MOVE, 1, 100, 200);
	send_touch(tab, TERMINAL_PICTURE_TOUCH_MOVE, 2, 300, 200);
	assert(waker_rings == 0U);
	assert(x11_tab_take_input(tab, &in) == 0);
	assert((tab->connection.ops->pending(&tab->connection) & TERMINAL_READY_OUTPUT) != 0U);

	/* At 1.0x zoom centered on (400, 300), top/bottom edges are now filled with X11 pixels (no letterbox). */
	assert(tab->connection.ops->picture->draw(&tab->connection, dst, 400U, 400U) == 0);
	assert(dst[10U * 400U + 10U] == 0xFF00FF00U);
	assert(dst[390U * 400U + 390U] == 0xFF0000FFU);

	/* Release fingers, then single-finger tap at (200, 200) -> maps to root near (400, 300) and rings waker. */
	send_touch(tab, TERMINAL_PICTURE_TOUCH_UP, 1, 100, 200);
	send_touch(tab, TERMINAL_PICTURE_TOUCH_UP, 2, 300, 200);
	waker_rings = 0U;
	send_touch(tab, TERMINAL_PICTURE_TOUCH_DOWN, 3, 200, 200);
	send_touch(tab, TERMINAL_PICTURE_TOUCH_UP, 3, 200, 200);
	assert(waker_rings > 0U);
	assert(x11_tab_take_input(tab, &in) == 1);
	assert(in.kind == X11_LINK_INPUT_POINTER);
	assert((int32_t)in.numbers[1] >= 350 && (int32_t)in.numbers[1] <= 450);
	assert((int32_t)in.numbers[2] >= 298 && (int32_t)in.numbers[2] <= 302);

	/* Test bilinear interpolation across a sharp black/white edge when scaled. */
	for (y = 0U; y < 600U; y++) {
		for (x = 0U; x < 800U; x++) {
			px = 0xFFFFFFFFU;
			if (x < 400U)
				px = 0xFF000000U;
			x11_link_put32(src + ((size_t)y * 800U + x) * 4U, px);
		}
	}

	assert(x11_tab_put(tab, 0U, 0U, 800U, 600U, src) == 0);
	/* Zoom in to ~2.0x around (200, 200) -> root (400, 300) is near screen x=250. */
	send_touch(tab, TERMINAL_PICTURE_TOUCH_DOWN, 4, 150, 200);
	send_touch(tab, TERMINAL_PICTURE_TOUCH_DOWN, 5, 250, 200);
	send_touch(tab, TERMINAL_PICTURE_TOUCH_MOVE, 4, 100, 200);
	send_touch(tab, TERMINAL_PICTURE_TOUCH_MOVE, 5, 300, 200);
	send_touch(tab, TERMINAL_PICTURE_TOUCH_UP, 4, 100, 200);
	send_touch(tab, TERMINAL_PICTURE_TOUCH_UP, 5, 300, 200);
	assert(tab->connection.ops->picture->draw(&tab->connection, dst, 400U, 400U) == 0);
	has_blended = 0;
	for (x = 0U; x < 400U; x++) {
		c = dst[200U * 400U + x] & 0x00FFFFFFU;
		if (c != 0x00000000U && c != 0x00FFFFFFU)
			has_blended = 1;
	}

	assert(has_blended);
	assert(dst[200U * 400U + 10U] == 0xFF000000U);
	assert(dst[200U * 400U + 390U] == 0xFFFFFFFFU);

	/* Test 1:1 unscaled exact copy on a 1px checkerboard pattern. */
	for (y = 0U; y < 600U; y++) {
		for (x = 0U; x < 800U; x++) {
			px = 0xFF000000U;
			if (((x ^ y) & 1U) != 0U)
				px = 0xFFFFFFFFU;
			x11_link_put32(src + ((size_t)y * 800U + x) * 4U, px);
		}
	}

	assert(x11_tab_put(tab, 0U, 0U, 800U, 600U, src) == 0);
	/* Pinch out by 0.5x back to 1.0x detent. */
	send_touch(tab, TERMINAL_PICTURE_TOUCH_DOWN, 6, 100, 200);
	send_touch(tab, TERMINAL_PICTURE_TOUCH_DOWN, 7, 300, 200);
	send_touch(tab, TERMINAL_PICTURE_TOUCH_MOVE, 6, 150, 200);
	send_touch(tab, TERMINAL_PICTURE_TOUCH_MOVE, 7, 250, 200);
	send_touch(tab, TERMINAL_PICTURE_TOUCH_UP, 6, 150, 200);
	send_touch(tab, TERMINAL_PICTURE_TOUCH_UP, 7, 250, 200);
	assert(tab->connection.ops->picture->draw(&tab->connection, dst, 400U, 400U) == 0);
	for (x = 10U; x < 20U; x++) {
		c0 = dst[10U * 400U + x];
		c1 = dst[10U * 400U + x + 1U];
		assert(c0 == 0xFF000000U || c0 == 0xFFFFFFFFU);
		assert(c1 == 0xFF000000U || c1 == 0xFFFFFFFFU);
		assert(c0 != c1);
	}

	free(src);
	free(dst);
	tab->connection.ops->close(&tab->connection);
	x11_tab_release(tab);
	printf("OK x11-tab-root-host-test\n");
	return 0;
}
