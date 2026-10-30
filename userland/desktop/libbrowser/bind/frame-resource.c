/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Loaded child owners retain actual resource metadata and collectible independent realms.
 */

#include "bind/internal.h"
#include "net/net.h"

#include <errno.h>
#include <string.h>

/*
 * Copies the actual Document URL or its inherited blank fallback base.
 */
int
bind_document_url(
	struct dom_document *document,
	int use_base,
	struct wb_buffer *out)
{
	struct bind_window *window;
	struct vm_string *url;
	int error;

	/* Loaded URLs belong to the Document; only blank resolution uses inherited metadata. */
	window = document->view;
	url = document->resource_url;
	if (use_base && window != NULL) {
		/* A blank owner retains its fallback independently of its parent's later navigation. */
		if (window->base_url != NULL)
			url = window->base_url;
	}

	/* Copy native strings without allocating VM objects or invoking script. */
	if (url != NULL) {
		error = vm_string_to_utf8(url, out);
	} else if (window != NULL && window->host.location != NULL) {
		error = window->host.location(window->host.context, BIND_LOCATION_HREF, out);
	} else {
		error = wb_buffer_append_string(out, "about:blank");
	}

	/* Report storage failure before exposing an incomplete URL. */
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the actual URL text. */
	return 0;
}

/*
 * Reports child location components from its actual owned resource URL.
 */
int
bind_frame_location(
	struct bind_window *window,
	int part,
	struct wb_buffer *out)
{
	struct wb_buffer text;
	struct net_url url;
	int component;
	int error;

	/* Binding and native URL component enums have independent layouts. */
	component = NET_URL_HREF;
	switch (part) {
	case BIND_LOCATION_ORIGIN:
		component = NET_URL_ORIGIN;
		break;
	case BIND_LOCATION_PROTOCOL:
		component = NET_URL_PROTOCOL;
		break;
	case BIND_LOCATION_HOST:
		component = NET_URL_HOST;
		break;
	case BIND_LOCATION_HOSTNAME:
		component = NET_URL_HOSTNAME;
		break;
	case BIND_LOCATION_PORT:
		component = NET_URL_PORT;
		break;
	case BIND_LOCATION_PATHNAME:
		component = NET_URL_PATHNAME;
		break;
	case BIND_LOCATION_SEARCH:
		component = NET_URL_SEARCH;
		break;
	case BIND_LOCATION_HASH:
		component = NET_URL_HASH;
		break;
	default:
		break;
	}

	/* Prepare owned URL text separately from the independently released parsed record. */
	wb_buffer_init(&text);

	/* A zeroed URL can be released even when obtaining its source text fails. */
	memset(&url, 0, sizeof(url));

	/* Copy this Document's URL before parsing or selecting a location component. */
	error = bind_document_url(window->document, 0, &text);

	/* Parse only a complete source copy; a saved loaded owner needs no primary Page. */
	if (error == 0)
		error = net_url_parse(wb_buffer_string(&text), text.length, NULL, &url);

	/* Append the requested component only when the complete URL parsed successfully. */
	if (error == 0)
		error = net_url_component(&url, component, out);

	/* Both independent C records are released regardless of which operation failed. */
	net_url_release(&url);
	wb_buffer_release(&text);

	/* The caller must not treat partial component text as a successful location. */
	if (error != 0)
		return error;

	/* Succeeded: the returned component describes this actual Document. */
	return 0;
}

/*
 * Installs a complete actual child context before retiring its previous owner.
 */
int
bind_frame_install(
	struct bind_window *parent,
	struct dom_element *element,
	struct dom_document *document,
	struct bind_window **window)
{
	struct vm_cell *roots[4];
	struct vm_heap *heap;
	struct vm_realm *realm;
	struct vm_realm *previous;
	struct bind_window *made;
	struct bind_window *retired;
	struct bind_host host;
	struct wb_buffer base;
	unsigned index;
	unsigned registered;
	int error;

	/* Failed construction never exposes a partially created child owner. */
	*window = NULL;

	/* Prepare root slots before any allocation can collect the parent, frame or Document. */
	heap = parent->realm->heap;
	roots[0] = &parent->document->node.cell;
	roots[1] = &element->node.cell;
	roots[2] = &document->node.cell;
	roots[3] = NULL;
	registered = 0;

	/* Keep inherited blank-base text in independent C storage until ownership is copied. */
	wb_buffer_init(&base);

	/* Register each root slot before allocation; registered counts the holds to unwind. */
	error = 0;
	for (index = 0; index < 4U; index++) {
		error = vm_heap_add_root(heap, &roots[index]);
		if (error != 0)
			break;
		registered++;
	}

	/* The existing finite context bound applies to real resource navigation too. */
	if (error == 0 && parent->context_depth >= 64U)
		error = EOVERFLOW;

	/* Create a separately collectible realm under the same tab heap and agent. */
	realm = NULL;
	made = NULL;
	if (error == 0) {
		error = vm_realm_create_managed(heap, &realm);
		if (error == 0)
			roots[3] = &realm->cell;
	}

	/* Share well-known symbols while allocating independent intrinsic objects. */
	if (error == 0) {
		/* Preserve agent-wide symbol identities in the newly collectible child realm. */
		for (index = 0; index < VM_SYMBOLS; index++)
			realm->symbols[index] = parent->realm->symbols[index];

		/* Allocate the child realm's own intrinsic graph only after symbol sharing is ready. */
		error = js_install_builtins(realm);
	}

	/* The retained actual owner supplies location; Page callbacks are used only while active. */
	if (error == 0) {
		memset(&host, 0, sizeof(host));
		host.context = parent->host.context;
		host.console = parent->host.console;
		host.node_box = parent->host.node_box;
		host.user_agent = parent->host.user_agent;
		host.checkpoint = parent->host.checkpoint;
		host.frame = element;

		/* Construct the independently retained owner with this complete abstract host adapter. */
		error = bind_window_create(realm, document, &host, &made);
	}

	/* Blank contexts need a copied inherited fallback before publication. */
	if (error == 0 && document->resource_url == NULL) {
		error = bind_document_url(parent->document, 1, &base);
		if (error == 0) {
			made->base_url = vm_string_from_utf8(heap, wb_buffer_string(&base), base.length);
			if (made->base_url == NULL)
				error = ENOMEM;
		}
	}

	/* Publish only after all fallible ownership initialization has succeeded. */
	if (error == 0) {
		made->parent_global = parent->realm->global;
		made->top_global = parent->top_global;
		if (made->top_global == NULL)
			made->top_global = parent->realm->global;

		/* This depth bounds descendants, while the frame edge names the owning native element. */
		made->frame = element;
		made->context_depth = parent->context_depth + 1U;

		/* Initialize the child with the parent's current viewport and timer origin before publication. */
		bind_window_set_viewport(made, parent->viewport_width, parent->viewport_height);
		bind_window_set_time(made, parent->now);

		/* Saved references keep the former Document, but no timer or host callback remains active. */
		previous = (struct vm_realm *)element->child_context;
		if (previous != NULL) {
			retired = previous->host;
			if (retired != NULL) {
				retired->frame = NULL;
				retired->realm->job_target = parent->realm;
				bind_window_detach(retired);
			}
		}

		/* The native element now owns the new actual global and Document pair. */
		element->child_context = &realm->cell;
		element->child_loaded_epoch = element->child_epoch;
		*window = made;
	}

	/* Unwind only successfully registered temporary roots on every exit. */
	wb_buffer_release(&base);

	/* Remove each registered slot in reverse order; zero means no temporary owner remains rooted. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(heap, &roots[registered]);
	}

	/* Report incomplete construction without publishing a half-built context. */
	if (error != 0)
		return error;

	/* Succeeded: the element owns a complete collectible child context. */
	return 0;
}
