/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Strict native XML output becomes an owning DOM graph before any browsing-context activation. */

#ifndef KEILAND_BROWSER_XML_DOM_H
#define KEILAND_BROWSER_XML_DOM_H

#include "dom/dom.h"
#include "xml/xml.h"

/* Model must be a successful immutable parse; output never borrows its storage after conversion. */
int xml_document_project(struct vm_heap *heap, const struct xml_document *model, enum dom_document_content content, struct dom_document **created);

#endif
