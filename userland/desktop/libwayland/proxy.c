/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Manages wire object identities and request marshalling under the display lock.
 */

#include "internal.h"

static struct wl_proxy *wlc_marshal_variadic(struct wl_proxy *proxy, uint32_t opcode, const struct wl_interface *interface, uint32_t version, uint32_t flags, va_list arguments);

/*
 * Allocates a client object associated with a factory's display and event queue.
 */
struct wl_proxy *
wl_proxy_create(
	struct wl_proxy *factory,
	const struct wl_interface *interface)
{
	struct wl_proxy *created;

	/* Rejects an absent factory or protocol description. */
	if (factory == NULL || interface == NULL) {
		errno = EINVAL;
		return NULL;
	}

	/* Serializes allocation with message decoding and object deletion. */
	pthread_mutex_lock(&factory->display->mutex);

	created = wlc_proxy_allocate(factory, interface, (uint32_t)interface->version);

	pthread_mutex_unlock(&factory->display->mutex);

	/* Propagates an allocation or exhausted-ID failure. */
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the new object proxy. */
	return created;
}

/*
 * Suppresses callbacks and drops caller ownership of a protocol proxy.
 */
void
wl_proxy_destroy(
	struct wl_proxy *proxy)
{
	struct wl_display *display;

	/* Allows ordinary cleanup to omit an uncreated object. */
	if (proxy == NULL)
		return;

	/* Keeps the connection address valid if this is the proxy's final hold. */
	display = proxy->display;
	pthread_mutex_lock(&display->mutex);

	wlc_proxy_destroy(proxy);

	pthread_mutex_unlock(&display->mutex);

	/* Succeeded: queued events can no longer call this proxy's listener. */
	return;
}

/*
 * Creates a queue-selecting wrapper without changing the original proxy.
 */
void *
wl_proxy_create_wrapper(
	void *object)
{
	struct wl_proxy *proxy;
	struct wl_proxy *wrapper;
	struct wl_proxy *real;
	struct wl_display *display;

	/* Rejects an absent object instead of creating a detached wrapper. */
	if (object == NULL) {
		errno = EINVAL;
		return NULL;
	}

	/* Reserves wrapper storage before entering the display critical section. */
	wrapper = calloc(1, sizeof(*wrapper));
	if (wrapper == NULL)
		return NULL;

	/* Resolves and retains the underlying object while its map can change. */
	proxy = object;
	display = proxy->display;
	pthread_mutex_lock(&display->mutex);

	real = wlc_proxy_real(proxy);

	/* Refuses constructing requests through an already destroyed object. */
	if (real->destroyed) {
		pthread_mutex_unlock(&display->mutex);
		free(wrapper);
		errno = EINVAL;
		return NULL;
	}

	/* The wrapper reference keeps the real proxy alive beyond map removal. */
	real->references++;
	wrapper->interface = real->interface;
	wrapper->display = display;
	wrapper->queue = proxy->queue;
	wrapper->wrapped = real;
	wrapper->id = real->id;
	wrapper->version = real->version;
	wrapper->references = 1;
	wrapper->next = display->wrappers;
	display->wrappers = wrapper;

	pthread_mutex_unlock(&display->mutex);

	/* Succeeded: the wrapper can choose a private constructor event queue. */
	return wrapper;
}

/*
 * Releases a queue-selecting wrapper while retaining the original proxy.
 */
void
wl_proxy_wrapper_destroy(
	void *object)
{
	struct wl_proxy *wrapper;
	struct wl_proxy **link;
	struct wl_display *display;

	/* Makes failed wrapper setup easy to unwind. */
	if (object == NULL)
		return;

	/* Removes this wrapper from the connection's ownership list. */
	wrapper = object;
	display = wrapper->display;
	pthread_mutex_lock(&display->mutex);

	/* Refuses applying wrapper ownership rules to an ordinary object. */
	if (wrapper->wrapped == NULL) {
		pthread_mutex_unlock(&display->mutex);
		return;
	}

	/* Finds the wrapper without altering any protocol object identity. */
	for (link = &display->wrappers; *link != NULL; link = &(*link)->next) {
		/* Unlinks the exact wrapper registered by create_wrapper. */
		if (*link == wrapper) {
			*link = wrapper->next;
			break;
		}
	}

	/* Ends the wrapper's strong reference to the original object. */
	wlc_proxy_unref(wrapper->wrapped);
	free(wrapper);

	pthread_mutex_unlock(&display->mutex);

	/* Succeeded: the caller's original proxy is otherwise unchanged. */
	return;
}

/*
 * Queues a request and optionally creates or destroys a protocol proxy.
 */
struct wl_proxy *
wl_proxy_marshal_array_flags(
	struct wl_proxy *proxy,
	uint32_t opcode,
	const struct wl_interface *interface,
	uint32_t version,
	uint32_t flags,
	union wl_argument *arguments)
{
	struct wl_display *display;
	struct wl_proxy *created;
	struct wl_proxy *real;
	int error;

	/* Rejects a missing request target. */
	if (proxy == NULL) {
		errno = EINVAL;
		return NULL;
	}

	/* Associates the wire request and constructor identity atomically. */
	display = proxy->display;
	created = NULL;
	pthread_mutex_lock(&display->mutex);

	real = wlc_proxy_real(proxy);

	/* Prevents requests after the connection has lost protocol ordering. */
	if (display->error != 0) {
		errno = display->error;
		pthread_mutex_unlock(&display->mutex);
		return NULL;
	}

	/* Rejects requests through a retired object or unknown marshal flags. */
	if (real->destroyed || (flags & ~WL_MARSHAL_FLAG_DESTROY) != 0) {
		wlc_display_error(display, EINVAL);
		pthread_mutex_unlock(&display->mutex);
		return NULL;
	}

	/* Allocates the requested constructor before publishing any wire bytes. */
	if (interface != NULL) {
		created = wlc_proxy_allocate(proxy, interface, version);
		if (created == NULL) {
			wlc_display_error(display, errno);
			pthread_mutex_unlock(&display->mutex);
			return NULL;
		}
	}

	/* Serializes all arguments and retains descriptor references in the packet. */
	error = wlc_wire_queue(proxy, opcode, arguments, created);
	if (error != 0) {
		/* An unpublished constructor never waits for a server delete_id. */
		if (created != NULL) {
			wlc_proxy_remove(created);
			wlc_proxy_destroy(created);
		}

		wlc_display_error(display, error);
		pthread_mutex_unlock(&display->mutex);
		return NULL;
	}

	/* Retires destructor requests only after their bytes are safely queued. */
	if ((flags & WL_MARSHAL_FLAG_DESTROY) != 0)
		wlc_proxy_destroy(real);

	pthread_mutex_unlock(&display->mutex);

	/* Succeeded: non-constructors intentionally return no object. */
	return created;
}

/*
 * Marshals a legacy argument array without creating a new proxy implicitly.
 */
void
wl_proxy_marshal_array(
	struct wl_proxy *proxy,
	uint32_t opcode,
	union wl_argument *arguments)
{
	/* Uses the common atomic request queue without constructor flags. */
	wl_proxy_marshal_array_flags(proxy, opcode, NULL, 0, 0, arguments);

	/* Succeeded: any marshalling failure is retained by the display. */
	return;
}

/*
 * Installs typed event callbacks on a protocol object.
 */
int
wl_proxy_add_listener(
	struct wl_proxy *proxy,
	void (**implementation)(void),
	void *data)
{
	/* Rejects absent listeners and queue-only wrappers. */
	if (proxy == NULL || implementation == NULL) {
		errno = EINVAL;
		return -1;
	}

	/* Serializes listener installation with decoder and dispatch snapshots. */
	pthread_mutex_lock(&proxy->display->mutex);

	/* Keeps listener replacement from changing an in-flight callback contract. */
	if (proxy->listener != NULL || proxy->dispatcher != NULL ||
	    proxy->wrapped != NULL || proxy->destroyed) {
		pthread_mutex_unlock(&proxy->display->mutex);
		errno = EINVAL;
		return -1;
	}

	proxy->listener = implementation;
	proxy->user_data = data;

	pthread_mutex_unlock(&proxy->display->mutex);

	/* Succeeded: decoded events will use this object's typed listener. */
	return 0;
}

/*
 * Installs a protocol-aware dispatcher for a custom language binding.
 */
int
wl_proxy_add_dispatcher(
	struct wl_proxy *proxy,
	wl_dispatcher_func_t dispatcher,
	const void *implementation,
	void *data)
{
	/* Rejects an absent object or dispatcher callback. */
	if (proxy == NULL || dispatcher == NULL) {
		errno = EINVAL;
		return -1;
	}

	/* Serializes dispatcher installation with callback snapshots. */
	pthread_mutex_lock(&proxy->display->mutex);

	/* A proxy has either one typed listener or one generic dispatcher. */
	if (proxy->listener != NULL || proxy->dispatcher != NULL ||
	    proxy->wrapped != NULL || proxy->destroyed) {
		pthread_mutex_unlock(&proxy->display->mutex);
		errno = EINVAL;
		return -1;
	}

	proxy->dispatcher = dispatcher;
	proxy->dispatcher_data = implementation;
	proxy->user_data = data;

	pthread_mutex_unlock(&proxy->display->mutex);

	/* Succeeded: the binding owns interpretation of decoded argument types. */
	return 0;
}

/*
 * Assigns the event queue inherited by this proxy's new child objects.
 */
void
wl_proxy_set_queue(
	struct wl_proxy *proxy,
	struct wl_event_queue *queue)
{
	struct wl_display *display;

	/* Resolves the default queue without altering other wrappers or proxies. */
	display = proxy->display;
	if (queue == NULL)
		queue = &display->default_queue;

	/* Refuses crossing display ownership at the queue boundary. */
	if (queue->display != display) {
		errno = EINVAL;
		return;
	}

	/* Serializes future event routing with incoming message decoding. */
	pthread_mutex_lock(&display->mutex);

	proxy->queue = queue;

	pthread_mutex_unlock(&display->mutex);

	/* Succeeded: already queued events retain their original destination. */
	return;
}

/*
 * Marshals a typed variadic request through the common wire queue.
 */
struct wl_proxy *
wl_proxy_marshal_flags(
	struct wl_proxy *proxy,
	uint32_t opcode,
	const struct wl_interface *interface,
	uint32_t version,
	uint32_t flags,
	...)
{
	va_list arguments;
	struct wl_proxy *created;

	/* Extracts arguments according to the request metadata. */
	va_start(arguments, flags);
	created = wlc_marshal_variadic(proxy, opcode, interface, version, flags, arguments);
	va_end(arguments);

	/* Succeeded: the display retains the queued request or a fatal error. */
	return created;
}

/*
 * Marshals a typed variadic request through the common wire queue.
 */
struct wl_proxy *
wl_proxy_marshal_constructor(
	struct wl_proxy *proxy,
	uint32_t opcode,
	const struct wl_interface *interface,
	...)
{
	va_list arguments;
	struct wl_proxy *created;

	/* Extracts arguments according to the request metadata. */
	va_start(arguments, interface);
	created = wlc_marshal_variadic(proxy, opcode, interface, (uint32_t)interface->version, 0, arguments);
	va_end(arguments);

	/* Succeeded: the display retains the queued request or a fatal error. */
	return created;
}

/*
 * Marshals a typed variadic request through the common wire queue.
 */
struct wl_proxy *
wl_proxy_marshal_constructor_versioned(
	struct wl_proxy *proxy,
	uint32_t opcode,
	const struct wl_interface *interface,
	uint32_t version,
	...)
{
	va_list arguments;
	struct wl_proxy *created;

	/* Extracts arguments according to the request metadata. */
	va_start(arguments, version);
	created = wlc_marshal_variadic(proxy, opcode, interface, version, 0, arguments);
	va_end(arguments);

	/* Succeeded: the display retains the queued request or a fatal error. */
	return created;
}

/*
 * Marshals a typed variadic request through the common wire queue.
 */
void
wl_proxy_marshal(
	struct wl_proxy *proxy,
	uint32_t opcode,
	...)
{
	va_list arguments;

	/* Extracts arguments according to the request metadata. */
	va_start(arguments, opcode);
	wlc_marshal_variadic(proxy, opcode, NULL, 0, 0, arguments);
	va_end(arguments);

	/* Succeeded: the display retains the queued request or a fatal error. */
	return;
}

/*
 * Constructs a proxy from an explicitly supplied argument array.
 */
struct wl_proxy *
wl_proxy_marshal_array_constructor(
	struct wl_proxy *proxy,
	uint32_t opcode,
	union wl_argument *arguments,
	const struct wl_interface *interface)
{
	struct wl_proxy *created;

	/* Preserves atomic object creation and event queue inheritance. */
	created = wl_proxy_marshal_array_flags(proxy, opcode, interface, (uint32_t)interface->version, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the constructed object. */
	return created;
}

/*
 * Constructs a proxy from an explicitly supplied argument array.
 */
struct wl_proxy *
wl_proxy_marshal_array_constructor_versioned(
	struct wl_proxy *proxy,
	uint32_t opcode,
	union wl_argument *arguments,
	const struct wl_interface *interface,
	uint32_t version)
{
	struct wl_proxy *created;

	/* Preserves atomic object creation and event queue inheritance. */
	created = wl_proxy_marshal_array_flags(proxy, opcode, interface, version, 0, arguments);
	if (created == NULL)
		return NULL;

	/* Succeeded: the caller owns the constructed object. */
	return created;
}

/*
 * Associates caller metadata with one proxy or wrapper.
 */
void
wl_proxy_set_user_data(
	struct wl_proxy *proxy,
	void *data)
{
	/* Serializes metadata replacement with event-dispatch snapshots. */
	pthread_mutex_lock(&proxy->display->mutex);

	proxy->user_data = data;

	pthread_mutex_unlock(&proxy->display->mutex);

	/* Succeeded: the proxy retains the caller-owned metadata pointer. */
	return;
}

/*
 * Associates caller metadata with one proxy or wrapper.
 */
void
wl_proxy_set_tag(
	struct wl_proxy *proxy,
	const char *const *data)
{
	/* Serializes metadata replacement with event-dispatch snapshots. */
	pthread_mutex_lock(&proxy->display->mutex);

	proxy->tag = data;

	pthread_mutex_unlock(&proxy->display->mutex);

	/* Succeeded: the proxy retains the caller-owned metadata pointer. */
	return;
}

/*
 * Obtains the requested proxy property under its display mutex.
 */
void *
wl_proxy_get_user_data(
	struct wl_proxy *proxy)
{
	void *answer;

	/* Reads one coherent property while listeners and queue assignment change. */
	pthread_mutex_lock(&proxy->display->mutex);

	answer = proxy->user_data;

	pthread_mutex_unlock(&proxy->display->mutex);

	/* Succeeded: reports the property without transferring its ownership. */
	return answer;
}

/*
 * Obtains the requested proxy property under its display mutex.
 */
const char *const *
wl_proxy_get_tag(
	struct wl_proxy *proxy)
{
	const char *const *answer;

	/* Reads one coherent property while listeners and queue assignment change. */
	pthread_mutex_lock(&proxy->display->mutex);

	answer = proxy->tag;

	pthread_mutex_unlock(&proxy->display->mutex);

	/* Succeeded: reports the property without transferring its ownership. */
	return answer;
}

/*
 * Obtains the requested proxy property under its display mutex.
 */
uint32_t
wl_proxy_get_version(
	struct wl_proxy *proxy)
{
	uint32_t answer;

	/* Reads one coherent property while listeners and queue assignment change. */
	pthread_mutex_lock(&proxy->display->mutex);

	answer = proxy->version;

	pthread_mutex_unlock(&proxy->display->mutex);

	/* Succeeded: reports the property without transferring its ownership. */
	return answer;
}

/*
 * Obtains the requested proxy property under its display mutex.
 */
uint32_t
wl_proxy_get_id(
	struct wl_proxy *proxy)
{
	uint32_t answer;

	/* Reads one coherent property while listeners and queue assignment change. */
	pthread_mutex_lock(&proxy->display->mutex);

	answer = proxy->id;

	pthread_mutex_unlock(&proxy->display->mutex);

	/* Succeeded: reports the property without transferring its ownership. */
	return answer;
}

/*
 * Obtains the requested proxy property under its display mutex.
 */
const char *
wl_proxy_get_class(
	struct wl_proxy *proxy)
{
	const char *answer;

	/* Reads one coherent property while listeners and queue assignment change. */
	pthread_mutex_lock(&proxy->display->mutex);

	answer = proxy->interface->name;

	pthread_mutex_unlock(&proxy->display->mutex);

	/* Succeeded: reports the property without transferring its ownership. */
	return answer;
}

/*
 * Obtains the requested proxy property under its display mutex.
 */
struct wl_display *
wl_proxy_get_display(
	struct wl_proxy *proxy)
{
	struct wl_display *answer;

	/* Reads one coherent property while listeners and queue assignment change. */
	pthread_mutex_lock(&proxy->display->mutex);

	answer = proxy->display;

	pthread_mutex_unlock(&proxy->display->mutex);

	/* Succeeded: reports the property without transferring its ownership. */
	return answer;
}

/*
 * Obtains the requested proxy property under its display mutex.
 */
struct wl_event_queue *
wl_proxy_get_queue(
	const struct wl_proxy *proxy)
{
	struct wl_event_queue *answer;

	/* Reads one coherent property while listeners and queue assignment change. */
	pthread_mutex_lock(&proxy->display->mutex);

	answer = proxy->queue;

	pthread_mutex_unlock(&proxy->display->mutex);

	/* Succeeded: reports the property without transferring its ownership. */
	return answer;
}

/*
 * Obtains the requested proxy property under its display mutex.
 */
const void *
wl_proxy_get_listener(
	struct wl_proxy *proxy)
{
	const void *answer;

	/* Reads one coherent property while listeners and queue assignment change. */
	pthread_mutex_lock(&proxy->display->mutex);

	answer = proxy->listener;

	pthread_mutex_unlock(&proxy->display->mutex);

	/* Succeeded: reports the property without transferring its ownership. */
	return answer;
}

/*
 * Resolves a wrapper to its immutable underlying protocol identity.
 */
struct wl_proxy *
wlc_proxy_real(
	struct wl_proxy *proxy)
{
	/* Wrappers retain the exact original generation even after map removal. */
	if (proxy->wrapped != NULL)
		return proxy->wrapped;

	/* Succeeded: an ordinary proxy is already the wire identity. */
	return proxy;
}

/*
 * Finds the current generation of a wire object under the display mutex.
 */
struct wl_proxy *
wlc_proxy_lookup(
	struct wl_display *display,
	uint32_t id)
{
	struct wl_proxy *proxy;

	/* The display singleton has a connection-owned lifetime. */
	if (id == 1)
		return &display->proxy;

	/* Searches dynamic object storage without imposing a fixed proxy ceiling. */
	for (proxy = display->objects; proxy != NULL; proxy = proxy->next) {
		/* A map entry identifies exactly one acknowledged wire generation. */
		if (proxy->id == id)
			return proxy;
	}

	/* Succeeded: the requested object has no current map generation. */
	return NULL;
}

/*
 * Allocates a constructor proxy while the display mutex is held.
 */
struct wl_proxy *
wlc_proxy_allocate(
	struct wl_proxy *factory,
	const struct wl_interface *interface,
	uint32_t version)
{
	struct wl_display *display;
	struct wl_proxy *proxy;
	struct wl_proxy *existing;
	uint32_t candidate;
	uint32_t first;

	/* Rejects descriptions that cannot interpret the requested version. */
	if (interface == NULL || version == 0) {
		errno = EINVAL;
		return NULL;
	}

	/* Keeps version negotiation bounded by the supplied message tables. */
	if (interface->version < 1 || version > (uint32_t)interface->version) {
		errno = EINVAL;
		return NULL;
	}

	/* Searches only client-owned IDs, retaining unacknowledged tombstones. */
	display = factory->display;
	candidate = display->next_id;
	first = candidate;
	while (1) {
		/* A deleted map entry is reusable without affecting old queued events. */
		existing = wlc_proxy_lookup(display, candidate);
		if (existing == NULL)
			break;

		candidate++;

		/* Wraps before the protocol's reserved server-created ID namespace. */
		if (candidate >= WLC_SERVER_ID_START)
			candidate = 2;

		/* Reports exhaustion after examining each available wire identity. */
		if (candidate == first) {
			errno = ENOSPC;
			return NULL;
		}
	}

	/* Allocates the object before consuming its map identity. */
	proxy = calloc(1, sizeof(*proxy));
	if (proxy == NULL)
		return NULL;

	/* The map and caller independently retain this object generation. */
	proxy->interface = interface;
	proxy->display = display;
	proxy->queue = factory->queue;
	proxy->id = candidate;
	proxy->version = version;
	proxy->references = 2;
	proxy->in_map = 1;
	proxy->next = display->objects;
	display->objects = proxy;
	display->next_id = candidate + 1;

	/* Keeps the next constructor within the client ID namespace. */
	if (display->next_id >= WLC_SERVER_ID_START)
		display->next_id = 2;

	/* Succeeded: map lookup and caller ownership see the same generation. */
	return proxy;
}

/*
 * Makes the proxy of an object the server created in an event (a new_id
 * argument, such as wl_data_offer) while the display mutex is held.
 *
 * The identity is the server's, in its own range; the proxy has the event's
 * proxy's queue and version.  It starts with three holds: the map's, the
 * caller's (the listener's, dropped by wl_proxy_destroy) and the event's.
 */
struct wl_proxy *
wlc_proxy_insert_server(
	struct wl_proxy *factory,
	const struct wl_interface *interface,
	uint32_t id)
{
	struct wl_display *display;
	struct wl_proxy *proxy;
	uint32_t version;

	/* Only an identity in the server's range, and one not in use. */
	display = factory->display;
	if (id < WLC_SERVER_ID_START) {
		errno = EINVAL;
		return NULL;
	}

	/* An identity the map still holds cannot be created again, unless it is a zombie the client destroyed. */
	proxy = wlc_proxy_lookup(display, id);
	if (proxy != NULL && !proxy->destroyed) {
		errno = EEXIST;
		return NULL;
	}

	/* The zombie leaves the map; the new object takes its identity. */
	if (proxy != NULL)
		wlc_proxy_remove(proxy);

	/* The creating object's version, within what the interface describes. */
	version = factory->version;
	if (interface->version >= 1 && version > (uint32_t)interface->version)
		version = (uint32_t)interface->version;

	/* The proxy's storage. */
	proxy = calloc(1, sizeof(*proxy));
	if (proxy == NULL)
		return NULL;

	/* The map, the listener and the event each hold this generation. */
	proxy->interface = interface;
	proxy->display = display;
	proxy->queue = factory->queue;
	proxy->id = id;
	proxy->version = version;
	proxy->references = 3;
	proxy->in_map = 1;
	proxy->next = display->objects;
	display->objects = proxy;

	/* Succeeded: later events can name the object, and the listener receives it. */
	return proxy;
}

/*
 * Drops one proxy reference while the display mutex is held.
 */
void
wlc_proxy_unref(
	struct wl_proxy *proxy)
{
	/* The display singleton is released with its enclosing connection. */
	if (proxy == &proxy->display->proxy)
		return;

	/* Map, callers, wrappers and queued events each own a strong reference. */
	proxy->references--;
	if (proxy->references == 0)
		free(proxy);

	/* Succeeded: storage remains only while an owner can reach the proxy. */
	return;
}

/*
 * Removes a server-acknowledged wire identity from the display map.
 */
void
wlc_proxy_remove(
	struct wl_proxy *proxy)
{
	struct wl_proxy **link;

	/* An already removed generation is retained only by its other owners. */
	if (!proxy->in_map)
		return;

	/* Unlinks only this generation, never a newly reused numeric identity. */
	for (link = &proxy->display->objects; *link != NULL; link = &(*link)->next) {
		/* Drops the map hold after hiding the entry from future lookups. */
		if (*link == proxy) {
			*link = proxy->next;
			proxy->in_map = 0;
			wlc_proxy_unref(proxy);
			return;
		}
	}

	/* Succeeded: no map entry remains for this generation. */
	return;
}

/*
 * Retires caller ownership without reusing an unacknowledged wire ID.
 */
void
wlc_proxy_destroy(
	struct wl_proxy *proxy)
{
	/* Wrappers and the connection singleton have separate destroy operations. */
	if (proxy->wrapped != NULL || proxy == &proxy->display->proxy)
		return;

	/* Makes repeated internal retirement harmless while events still hold it. */
	if (proxy->destroyed)
		return;

	/*
	 * Destroyed suppresses callbacks; delete_id independently retires the
	 * map.  The server never sends delete_id for an object it created: such
	 * an identity stays in the map as a zombie, so events the server sent
	 * before it saw the destroy are dropped (their fds closed) instead of
	 * naming an unknown object, until the server creates the identity again.
	 */
	proxy->destroyed = 1;
	wlc_proxy_unref(proxy);

	/* Succeeded: any surviving references belong to protocol infrastructure. */
	return;
}

/* Decodes variadic arguments using the immutable request signature. */
static struct wl_proxy *
wlc_marshal_variadic(
	struct wl_proxy *proxy,
	uint32_t opcode,
	const struct wl_interface *interface,
	uint32_t version,
	uint32_t flags,
	va_list source)
{
	union wl_argument *arguments;
	struct wl_proxy *created;
	const char *signature;
	size_t count;
	size_t index;
	uint32_t since;
	char type;
	int nullable;

	/* Rejects a request absent from the supplied protocol class. */
	if (proxy == NULL || opcode >= (uint32_t)proxy->interface->method_count) {
		errno = EINVAL;
		return NULL;
	}

	/* Sizes temporary argument storage from the actual immutable signature. */
	signature = proxy->interface->methods[opcode].signature;
	count = wlc_signature_count(signature);
	arguments = calloc(count + 1, sizeof(*arguments));
	if (arguments == NULL) {
		pthread_mutex_lock(&proxy->display->mutex);
		wlc_display_error(proxy->display, ENOMEM);
		pthread_mutex_unlock(&proxy->display->mutex);
		return NULL;
	}

	/* Interprets each argument with its promoted C calling convention. */
	signature = wlc_signature_start(signature, &since);
	for (index = 0; index < count; index++) {
		/* Advances past nullable qualifiers before selecting the argument type. */
		signature = wlc_signature_next(signature, &type, &nullable);

		/* Uses the corresponding union member for every public wire kind. */
		switch (type) {
		case 'i':
		case 'f':
			arguments[index].i = va_arg(source, int32_t);
			break;
		case 'u':
			arguments[index].u = va_arg(source, uint32_t);
			break;
		case 'h':
			arguments[index].h = va_arg(source, int);
			break;
		case 's':
			arguments[index].s = va_arg(source, const char *);
			break;
		case 'a':
			arguments[index].a = va_arg(source, struct wl_array *);
			break;
		case 'o':
		case 'n':
			arguments[index].o = va_arg(source, struct wl_object *);
			break;
		default:
			free(arguments);
			errno = EINVAL;
			return NULL;
		}
	}

	/* Applies the same serialized constructor and failure contract as arrays. */
	created = wl_proxy_marshal_array_flags(proxy, opcode, interface, version, flags, arguments);
	free(arguments);

	/* Succeeded: a constructor returns its proxy; other requests return null. */
	return created;
}
