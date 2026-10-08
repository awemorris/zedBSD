/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's privilege separation (ws143-p004, D16 (a), plan section 4):
 * a small parent that stays root and only opens the controller's node,
 * and a child that runs as the account _bluetooth with everything that
 * reads the air.  The child asks for a node with a datagram, and the
 * parent answers with the node's descriptor (SCM_RIGHTS).  Each side ends
 * when the other does.
 *
 * ws143-p005 (phase005 section 4.10): the child also asks for an open of
 * /dev/input/bridge (OPEN-HID), one for each HID device it makes; the
 * parent does not count them (the kernel limits the opens).
 */

#ifndef BLUETOOTHD_PRIVSEP_H
#define BLUETOOTHD_PRIVSEP_H

#include <stddef.h>

/* The daemon's account and the folder of the bonds. */
#define BTD_ACCOUNT		"_bluetooth"
#define BTD_DATA_PARENT		"/var/db"

/* The node that makes HID devices (include/uapi/input-bridge.h). */
#define BTD_BRIDGE_PATH		"/dev/input/bridge"

/*
 * The child's ends of the separation: the datagram channel to the parent,
 * and the stream whose end tells that the parent went.
 */
struct btd_privsep {
	int channel;
	int liveness;
};

int btd_privsep_start(const char *node, const char *keys_folder, int listener, struct btd_privsep *privsep);
int btd_privsep_open(const struct btd_privsep *privsep, char *path, size_t size, int *descriptor);
int btd_privsep_open_bridge(const struct btd_privsep *privsep, int *descriptor);

#endif
