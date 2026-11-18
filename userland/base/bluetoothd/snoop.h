/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's record of the HCI traffic in the btsnoop format
 * (ws143-p005 i02, plan/ws143/phase005/phase.md section 9.11), for an
 * outside reader (tshark) to check how bluetoothd's SDP, HIDP, ATT and
 * L2CAP bytes read: the file's header ("btsnoop\0", version 1, datalink
 * 1002 = HCI as UART's H4, a packet type byte before each packet) and one
 * record a packet (lengths, flags, drops, time in microseconds since the
 * year 0), big-endian.  The builders have no system calls; the writer
 * appends whole records to a file the daemon opened.
 */

#ifndef BLUETOOTHD_SNOOP_H
#define BLUETOOTHD_SNOOP_H

#include <stddef.h>
#include <stdint.h>

/* The header's and a record's fixed lengths. */
#define BTD_SNOOP_HEADER	16U
#define BTD_SNOOP_RECORD	24U

/* The H4 packet types. */
#define BTD_SNOOP_COMMAND	0x01U
#define BTD_SNOOP_ACL		0x02U
#define BTD_SNOOP_EVENT		0x04U

/* The longest packet recorded whole (a longer one is cut, its original length kept). */
#define BTD_SNOOP_PACKET_MAX	1100U

size_t btd_snoop_header(uint8_t *out, size_t size);
size_t btd_snoop_record(uint8_t *out, size_t size, uint8_t packet_type, int received, uint64_t unix_us, const uint8_t *packet, size_t length);
int btd_snoop_open(const char *path);
void btd_snoop_write(int descriptor, uint8_t packet_type, int received, const uint8_t *packet, size_t length);
size_t btd_snoop_hide(const uint8_t *packet, size_t length, uint8_t *out, size_t size);

#endif
