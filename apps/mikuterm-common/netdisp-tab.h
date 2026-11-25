/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Network Display discovery and encrypted offer receiver for mikuterm.
 */

#ifndef MIKUTERM_NETDISP_TAB_H
#define MIKUTERM_NETDISP_TAB_H

#include "rdp-hosts.h"
#include "session.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Broadcasts Network Display advertisements while waiting for an encrypted
 * offer from a zedBSD desktop.  Populates out_target on success (returns 0),
 * returns ECANCELED if the user cancels (Esc / Ctrl+C / Ctrl+D), or EPIPE if
 * the tab is closing.
 */
int netdisp_wait_and_receive(struct mikuterm_session *session,
			     struct rdp_host_entry *out_target);

#ifdef __cplusplus
}
#endif

#endif
