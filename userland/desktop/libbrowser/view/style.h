/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The style thread of a view of its own threads ("browser-style"), shared
 * by front.c and style.c and by nobody else.
 *
 * The style thread runs the view's style, layout and display list work: a
 * script thread that needs it (a rendering update, a script's question
 * about a style or a box that is stale) lends the page's heap and DOM to
 * the style thread through the agent's lender (vm_heap_lend) and waits
 * until the work came back.  The style thread runs one lent work at a time,
 * in the order they came; it runs no script, starts no fetch and never
 * collects (vm_thread_set_borrower), and while a work runs its lender's
 * heap does not collect either.  Outside a lend the style thread touches
 * nothing of a page, so a script thread reads the style and the layout a
 * lend left directly when nothing changed since.
 */

#ifndef KEILAND_BROWSER_VIEW_STYLE_H
#define KEILAND_BROWSER_VIEW_STYLE_H

#include "vm/vm.h"

#include <stdint.h>

struct view_style;

/* The style thread, started (with the call it makes first on its thread) and ended once no work can come. */
int view_style_start(void (*enter)(void *context), void *context, struct view_style **style);
void view_style_stop(struct view_style *style);

/* The agent's lender (vm_lender): runs a work on the style thread while the caller waits. */
int view_style_lend(void *context, vm_lend_work work, void *argument);

/* How many works were lent so far and the nanoseconds the lenders waited for them in all. */
void view_style_counts(struct view_style *style, uint64_t *lends, uint64_t *waited_ns);

#endif
