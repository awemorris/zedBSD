/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Routes committed CLI changes through the same compositor API used by desktop apps. */
#include "notify.h"
#include <keiland/keiland.h>
#include <wayland-client.h>

/*
 * Notifies the compositor after persistence without requiring a desktop for offline use.
 */
void
media_db_notify(
	void)
{
	struct wl_display *display;
	struct kl_system *system;
	int error;

	/* An offline CLI still commits and returns its metadata without a compositor. */
	display = wl_display_connect(NULL);
	if (display == NULL)
		return;
	system = kl_system_open(display);
	if (system == NULL) {
		wl_display_disconnect(display);
		return;
	}

	/* Completes a round trip so the compositor receives the wakeup before this process exits. */
	error = kl_system_media_notify(system);
	if (error == 0)
		(void)wl_display_roundtrip(display);
	kl_system_close(system);
	wl_display_disconnect(display);
}
