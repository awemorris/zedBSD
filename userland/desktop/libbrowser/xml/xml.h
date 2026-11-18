/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Immutable native XML records borrow only storage owned by their parsing model. */

#ifndef KEILAND_BROWSER_XML_H
#define KEILAND_BROWSER_XML_H

#include "base/base.h"

/* Real XML tree records preserve characters and processing instructions before DOM projection. */
enum xml_node_type {
	XML_DOCUMENT,
	XML_ELEMENT,
	XML_TEXT,
	XML_CDATA,
	XML_COMMENT,
	XML_PI
};

/* One immutable UTF16 view remains valid until the entire owning model is destroyed. */
struct xml_units {
	const uint16_t *data;
	size_t length;
};

/* An actual attribute keeps its qualified and expanded names and normalized value. */
struct xml_attribute {
	struct xml_attribute *next;
	struct xml_units name;
	struct xml_units prefix;
	struct xml_units local;
	struct xml_units uri;
	struct xml_units value;
};

/* Each stable arena record is one actual tree node; parent and child edges never cross model lifetimes. */
struct xml_node {
	int type;
	struct xml_node *parent;
	struct xml_node *first_child;
	struct xml_node *last_child;
	struct xml_node *next;
	struct xml_units name;
	struct xml_units prefix;
	struct xml_units local;
	struct xml_units uri;
	struct xml_units value;
	struct xml_attribute *attributes;
	struct xml_attribute *last_attribute;
};

/* One parse owns normalized input, every immutable record and all decoded entity values. */
struct xml_document {
	struct wb_arena arena;
	struct wb_units input;
	struct xml_node node;
	struct xml_units version;
	struct xml_units encoding;
	struct xml_units standalone;
	struct xml_units xml_uri;
	struct xml_units xmlns_uri;
	size_t records;
};

/* A rejected parse identifies its normalized UTF16 position without publishing a partial tree. */
struct xml_error {
	size_t offset;
	int status;
};

/* Successful parse publishes one owning model; every failure leaves *document NULL. */
int xml_document_parse(struct xml_document **document, const unsigned char *bytes, size_t length, struct xml_error *error);
void xml_document_destroy(struct xml_document *document);

#endif
