/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The desktop's library, with two jobs.
 *
 * It wraps zdesktop's non-standard Wayland (xdg) extensions: a client of the
 * desktop (zdesktop-x11server, an application) uses standard Wayland and
 * Vulkan, and reaches anything only zdesktop offers through this library,
 * never through a private protocol of its own.
 *
 * It is also the desktop's way into the operating system: zdesktop does not
 * talk to networkd, audiod or the other daemons itself.  Everything it needs
 * from the system, other than drawing through Vulkan and its windows through
 * Wayland, comes through this library.  A daemon's protocol or an extension
 * can then change in one place, and moving the desktop to another system
 * means rewriting this library and nothing else.
 *
 * The library is empty for now.  Each feature adds its calls here when it
 * arrives with its first user; until then only the version is public, so
 * that nothing is promised before it exists.
 */

#ifndef ZDESKTOP_H
#define ZDESKTOP_H

#ifdef __cplusplus
extern "C" {
#endif

/* The interface version this header describes. */
#define ZDESKTOP_VERSION	1U

/*
 * Reports the interface version of the library that was loaded.
 *
 * A program built against this header may compare the result with
 * ZDESKTOP_VERSION to learn whether the library it runs with is older.
 */
unsigned zdesktop_version(void);

#ifdef __cplusplus
}
#endif

#endif
