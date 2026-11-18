/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libwebp-compat: zedBSD's own implementation of the decoding part of
 * libwebp's interface, for the desktop programs and the browser.  It holds
 * no libwebp code; the names and the values are libwebp 1.5.0's
 * <webp/types.h>, declared here anew.  Programs of the packages use the
 * real libwebp (/usr/include/webp/types.h); base programs include
 * <compat/webp/types.h> (usually through <compat/webp/decode.h>).
 *
 * This header gives the memory calls every buffer the library hands out
 * is freed with, and the macros the other header's declarations use.
 */

#ifndef KERN_COMPAT_WEBP_TYPES_H
#define KERN_COMPAT_WEBP_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The storage class of the library's calls.  The shared library exports
 * exactly the calls its version script lists, so nothing more is needed.
 */
#ifndef WEBP_EXTERN
#define WEBP_EXTERN extern
#endif

/*
 * libwebp marks some calls so that a compiler warns when their result is
 * ignored; this implementation leaves the mark empty.
 */
#ifndef WEBP_NODISCARD
#define WEBP_NODISCARD
#endif

/*
 * libwebp's inline marker.  The other header writes its small helpers as
 * macros instead (the tree is ANSI C), so this is only for programs that
 * name it.
 */
#ifndef WEBP_INLINE
#define WEBP_INLINE
#endif

/*
 * Tells two interface versions apart: they are incompatible when their
 * major bytes differ.
 */
#define WEBP_ABI_IS_INCOMPATIBLE(a, b)	(((a) >> 8) != ((b) >> 8))

/*
 * Allocates size bytes as the library allocates its outputs; NULL when the
 * memory is not there.  A buffer from here is freed with WebPFree.
 */
WEBP_NODISCARD WEBP_EXTERN void *WebPMalloc(size_t size);

/*
 * Frees a buffer the library returned (WebPDecodeRGBA and its kind) or
 * WebPMalloc gave; NULL is ignored.
 */
WEBP_EXTERN void WebPFree(void *ptr);

#ifdef __cplusplus
}
#endif

#endif
