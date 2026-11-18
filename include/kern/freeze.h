/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Freezing the user processes for a system sleep (ws052-p006).
 */

#ifndef KERN_KERN_FREEZE_H
#define KERN_KERN_FREEZE_H

void kern_freeze_user(void);
void kern_thaw_user(void);
void kern_freeze_user_return(void);

#endif
