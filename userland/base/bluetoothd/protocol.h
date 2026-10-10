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
 *   POWER on|off       Bluetooth on or off for the user (ws143-p006): off,
 *                      no scan or pairing starts (ERROR off) and SHOW says
 *                      STATE off; the saved keys stay; the switch is kept
 *                      across starts (on the first time)
 *
 * ws143-p004 to p006 add PAIR, AGENT, FORGET and BONDS (plan/ws143/phase004
 * section 5).  SHOW ends with "POWER on|off" (ws143-p006).  ws197-p002
 * adds PAIR's option phone=1 (a phone for the phone link, BR/EDR only:
 * ERROR phone-le otherwise, ERROR busy-links while the HID host uses every
 * link), whose PAIRED line ends with "phone=1", or "phone=0 why=WORD" when
 * the phone link did not take it; and, for root, PHONE DROP ADDRESS.
 * ws197-p003 adds the phone's requests (plan/ws197/phase004/phase.md
 * section 5, which is their grammar): PHONE SHOW, PHONE LINK ADDRESS
 * on|off [profiles=m,c,h], PHONE SUBSCRIBE (events alone from then on:
 * PHONE STATE, PHONE MESSAGE with its text, PHONE SENT, PHONE
 * MESSAGE-GONE, PHONE DROPPED), PHONE PAGE messages since=N [limit=N]
 * [cursor=C] count=N, PHONE READ handle=H and PHONE SEND to="N" length=N
 * followed by the text's bytes.  ws197-p005 adds PHONE PAGE contacts
 * [cursor=C] count=N and PHONE PAGE calls since=N [cursor=C] count=N
 * (plan/ws197/phase005/phase.md section 5.2: PHONE CONTACT with its
 * reduced vCard, PHONE CALL-LOG, PHONE PAGE-END), and contacts= and
 * contacts_why= in SHOW's and STATE's line.  The daemon's phone lines are
 * up to 2047 bytes, and a line with length=N is followed by N bytes.  A
 * pairing's questions, to the agent or the pairing's client, name the
 * device and who started it:
 *
 *   CONFIRM NUMBER address=... type=... uid=...   answered YES or NO
 *   CONSENT address=... type=... uid=...          answered YES or NO
 *   PASSKEY NUMBER address=... type=... uid=...   shown, not answered
 *   ASK-END                                       the question is over
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
