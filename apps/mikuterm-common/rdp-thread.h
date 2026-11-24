/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Worker thread running librdp, TLS (libssl-compat), and H.264 decoding for
 * an RDP tab in mikuterm.
 */

#ifndef MIKUTERM_RDP_THREAD_H
#define MIKUTERM_RDP_THREAD_H

#include "rdp-decoder.h"
#include "rdp-hosts.h"
#include "rdp-tab.h"

/*
 * Arguments passed to the detached RDP worker thread.
 */
struct rdp_thread_args {
	struct rdp_tab *tab;
	struct rdp_host_entry target;
	const struct rdp_decoder_ops *decoder_ops;
	unsigned connect_timeout;
};

int rdp_thread_start(struct rdp_tab *tab, const struct rdp_host_entry *target,
		     const struct rdp_decoder_ops *decoder_ops,
		     unsigned connect_timeout);

#endif
