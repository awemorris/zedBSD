/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Where the desktop is installed (WS104 p007).  zedBSD's image puts it in
 * the system's own directories, the defaults here.  The Linux build (WS105,
 * Makefile.linux) puts all of it under /opt/keiland and defines each macro
 * on the compiler's command line.
 *
 * A path is written as the macro followed by the rest of the path, so that
 * the string a program sees is the same as before: KEILAND_BINDIR
 * "/terminal" is "/bin/terminal" on zedBSD.
 */

#ifndef KEILAND_PATHS_H
#define KEILAND_PATHS_H

/* The programs. */
#ifndef KEILAND_BINDIR
#define KEILAND_BINDIR "/bin"
#endif

/* The helpers the desktop starts (keiland-ime, keiland-x11). */
#ifndef KEILAND_LIBEXECDIR
#define KEILAND_LIBEXECDIR "/usr/libexec"
#endif

/*
 * The data: fonts/, keiland/ (the wallpapers), browser/, mview/ and kei/
 * (the input method's dictionaries).
 */
#ifndef KEILAND_DATADIR
#define KEILAND_DATADIR "/usr/share"
#endif

/*
 * The companions of the desktop's fonts (ws090-p020, libtruetype's
 * truetype_open_companions): the bold weight of the interface's font
 * (Mahora Bold), and the monospaced fallback (JetBrains Mono) that draws the
 * signs and the letters Mahora does not have yet.  A program opens them
 * beside its font; one that is not installed is left out.
 */
#define KEILAND_FONT_BOLD		KEILAND_DATADIR "/fonts/keiland-bold.ttf"
#define KEILAND_FONT_FALLBACK_MONO	KEILAND_DATADIR "/fonts/keiland-fallback-mono.ttf"

/* The configuration: keiland/ (apps.conf, desktop, open-with). */
#ifndef KEILAND_SYSCONFDIR
#define KEILAND_SYSCONFDIR "/etc"
#endif

#endif
