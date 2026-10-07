/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's socket (ws143-p003, plan/ws143/phase003/phase.md section 4):
 * text lines, as volumed's.
 *
 * A client writes one request a line:
 *
 *   SHOW               the state and the controller
 *   DEVICES            the devices of the last scan
 *   SCAN SECONDS       a scan of 1 to 30 seconds (root only in this Phase),
 *                      answered when it ends with the devices found
 *
 * and reads the answer's lines up to DONE:
 *
 *   STATE WORD [REASON]
 *   CONTROLLER node=... vendor=... product=... name="..." address=...
 *              hci=... manufacturer=... le=0|1 p256=0|1 dhkey=0|1
 *              firmware=loaded|-
 *   DEVICE address=... type=bredr|le-public|le-random [rssi=...]
 *          [class=0x...] [appearance=0x...] name="..."
 *   ERROR WHY
 *   DONE
 */

#ifndef BLUETOOTHD_PROTOCOL_H
#define BLUETOOTHD_PROTOCOL_H

/* The socket, and the longest line either side writes. */
#define BTD_SOCKET		"/run/bluetoothd.sock"
#define BTD_LINE_MAX		512U

/* Where Intel's firmware files are. */
#define BTD_FIRMWARE_FOLDER	"/lib/firmware/intel"

#endif
