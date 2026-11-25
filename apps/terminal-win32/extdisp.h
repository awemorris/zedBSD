/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Win32 Extended Display host: UDP 33890 discovery listener, encrypted
 * OFFER sender, IddCx virtual display / SHM capture, libmedia H.264
 * encoding, librdp rdp_server streaming, and SendInput injection.
 */

#ifndef MIKUTERM_WIN32_EXTDISP_H
#define MIKUTERM_WIN32_EXTDISP_H

#include "userland/desktop/terminal/settings-page.h"

int mikuterm_extdisp_start(void);
void mikuterm_extdisp_stop(void);
const struct terminal_extdisp_ops *mikuterm_extdisp_ops(void);

#endif
