/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A body for the native thread that only paints the window one colour, for
 * trying the Activity's glue before the program is there.
 */

#ifndef MIKUTERM_ANDROID_HELLO_H
#define MIKUTERM_ANDROID_HELLO_H

/*
 * Runs the events of the Activity and paints each window; does not return.
 */
void hello_main(void);

#endif /* MIKUTERM_ANDROID_HELLO_H */
