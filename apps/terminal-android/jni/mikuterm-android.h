/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * mikuterm's body on Android, which the program's native thread runs.
 */

#ifndef MIKUTERM_ANDROID_H
#define MIKUTERM_ANDROID_H

/*
 * Sets the program up (its data, its settings, its fonts) and runs the terminal
 * until its last tab or its window closes.
 */
void mikuterm_android_main(void);

#endif /* MIKUTERM_ANDROID_H */
