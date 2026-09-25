/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef DRV_PCAT_SERIAL_MIRROR_H
#define DRV_PCAT_SERIAL_MIRROR_H

/*
 * The console on the first serial port.
 *
 * The display shows the console to a person at the machine; this repeats the
 * same stream where a host running an emulator, or a serial cable, can read
 * it, and carries what is typed there back the other way.  Both directions
 * are built only when CONFIG_PCAT_SERIAL_MIRROR selects them.
 */

/* Repeats one console character on the port. */
void drv_pcat_serial_mirror(int character);

/*
 * Starts reading the port.
 *
 * Called once the terminal above the console can take input.  Before this,
 * characters arriving on the line are discarded rather than queued: a line
 * that has been connected since power-on would otherwise deliver whatever
 * noise the other end sent while nothing was listening.
 */
void drv_pcat_serial_mirror_start_input(void);

#endif
