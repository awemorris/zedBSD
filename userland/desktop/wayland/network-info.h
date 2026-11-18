/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The network's details for the system bar (network-info.c, ws099-p032):
 * the rows of the panel an Alt+click on the network's icon opens, worked
 * out from the watch's state and scan and from a reading of the
 * interfaces and the DNS servers (libkeiland-backend's
 * kl_backend_network_get_links and _get_dns).
 *
 * It draws nothing and keeps nothing but the caller's previous sample (for
 * the transfer rates), so the host tests run it alone.
 */

#ifndef KWL_NETWORK_INFO_H
#define KWL_NETWORK_INFO_H

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <stddef.h>
#include <stdint.h>

/* The most rows, and a row's label and value with their NULs. */
#define KWL_NETWORK_INFO_ROWS		12U
#define KWL_NETWORK_INFO_LABEL		24U
#define KWL_NETWORK_INFO_VALUE		64U

/* One row: its label, and its value. */
struct kwl_network_info_row {
	char label[KWL_NETWORK_INFO_LABEL];
	char value[KWL_NETWORK_INFO_VALUE];
};

/*
 * The interface's byte counts at a reading, kept by the caller between
 * readings to show the rates: when (ms), which interface, its counts, and
 * whether it holds a reading (valid).
 */
struct kwl_network_info_sample {
	unsigned valid;
	uint64_t ms;
	char name[KL_BACKEND_NETWORK_NAME_MAX];
	uint64_t received;
	uint64_t sent;
};

/*
 * What a reading is made of: the watch's state and scan, the interfaces
 * and the DNS servers read, and the time of the reading.
 */
struct kwl_network_info_input {
	const struct kl_backend_network_state *state;
	const struct kl_backend_network_ap *scan;
	size_t scan_count;
	const struct kl_backend_network_link *links;
	size_t link_count;
	const char (*dns)[KL_BACKEND_NETWORK_ADDRESS_MAX];
	size_t dns_count;
	uint64_t now_ms;
};

/*
 * Works out the rows (at most capacity) and returns how many there are;
 * previous is the last reading's sample, replaced by this one's.  The
 * title is the first row's value when the label is empty ("Wi-Fi" or
 * "Ethernet", or "Network" when nothing is connected).
 */
unsigned kwl_network_info_build(const struct kwl_network_info_input *input, struct kwl_network_info_sample *previous, struct kwl_network_info_row *rows, unsigned capacity);

/* Writes a count of bytes as B, KB, MB or GB (powers of 1024, one decimal above B). */
void kwl_network_info_bytes(uint64_t bytes, char *text, size_t size);

#endif
