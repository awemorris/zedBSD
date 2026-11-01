/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The program's entry that does not depend on the system: main-posix.c
 * calls it from main, and another system's entry point (the Windows
 * program's) calls it with the command line as UTF-8.
 */

#ifndef KEILAND_BROWSER_MAIN_H
#define KEILAND_BROWSER_MAIN_H

/*
 * Runs the mode the command line names (argv[0] is the program's name).
 * Returns the program's exit status.
 */
int browser_main(int argc, char **argv);

#endif
