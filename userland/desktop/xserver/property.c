/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Atoms and window properties.
 *
 * Atoms: the core's predefined atoms keep their numbers; any other name is
 * given the next number, for the server's life.  Properties: any property
 * a client sets on a window is kept with the window, its 16- and 32-bit
 * units least significant byte first and turned to each client's order as
 * it reads them; each change and deletion is told to the clients that
 * asked for PropertyNotify on the window.  A property's data is charged to
 * the memory of the client that wrote it last.
 */

#include "userland/desktop/xserver/internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ChangeProperty's modes, and GetProperty's "any type". */
#define PROPERTY_REPLACE	0U
#define PROPERTY_PREPEND	1U
#define PROPERTY_APPEND		2U
#define PROPERTY_ANY_TYPE	0U

/*
 * The largest property kept, in bytes (one GetProperty must be able to
 * answer it whole within a client's output), and the bytes a property is
 * charged for besides its data.
 */
#define PROPERTY_MAX		(X11_OUTPUT_CAP - 64U)
#define PROPERTY_OVERHEAD	32U

/* The first number of properties a window keeps room for. */
#define PROPERTY_FIRST		4U

/* The first room of the atoms' names, and the most atoms the server gives numbers. */
#define ATOM_FIRST		256U
#define ATOM_LIMIT		0x00ffffffU

/* The bytes an atom is charged for besides its name. */
#define ATOM_OVERHEAD		16U

/* The name zedBSD's icon path atom answers to. */
#define ATOM_ICON_PATH_NAME	"_ZED_ICON_PATH"

/*
 * The core's predefined atoms, 1 to 68 in order (X Window System
 * Protocol, Appendix B, "Predefined Atoms").
 */
static const char *const property_predefined[X11_ATOM_LAST_PREDEFINED] = {
	"PRIMARY", "SECONDARY", "ARC", "ATOM", "BITMAP", "CARDINAL", "COLORMAP", "CURSOR",
	"CUT_BUFFER0", "CUT_BUFFER1", "CUT_BUFFER2", "CUT_BUFFER3", "CUT_BUFFER4", "CUT_BUFFER5", "CUT_BUFFER6", "CUT_BUFFER7",
	"DRAWABLE", "FONT", "INTEGER", "PIXMAP", "POINT", "RECTANGLE", "RESOURCE_MANAGER", "RGB_COLOR_MAP",
	"RGB_BEST_MAP", "RGB_BLUE_MAP", "RGB_DEFAULT_MAP", "RGB_GRAY_MAP", "RGB_GREEN_MAP", "RGB_RED_MAP", "STRING", "VISUALID",
	"WINDOW", "WM_COMMAND", "WM_HINTS", "WM_CLIENT_MACHINE", "WM_ICON_NAME", "WM_ICON_SIZE", "WM_NAME", "WM_NORMAL_HINTS",
	"WM_SIZE_HINTS", "WM_ZOOM_HINTS", "MIN_SPACE", "NORM_SPACE", "MAX_SPACE", "END_SPACE", "SUPERSCRIPT_X", "SUPERSCRIPT_Y",
	"SUBSCRIPT_X", "SUBSCRIPT_Y", "UNDERLINE_POSITION", "UNDERLINE_THICKNESS", "STRIKEOUT_ASCENT", "STRIKEOUT_DESCENT", "ITALIC_ANGLE", "X_HEIGHT",
	"QUAD_WIDTH", "WEIGHT", "POINT_SIZE", "RESOLUTION", "COPYRIGHT", "NOTICE", "FONT_NAME", "FAMILY_NAME",
	"FULL_NAME", "CAP_HEIGHT", "WM_CLASS", "WM_TRANSIENT_FOR"
};

static uint32_t property_hash(const char *name, size_t length);
static uint32_t property_lookup(struct x11server *server, const char *name, size_t length);
static int property_new_atom(struct x11server *server, const char *name, size_t length, uint32_t *atom);
static int property_table_grow(struct x11server *server);
static void property_order(uint8_t *target, const uint8_t *source, size_t length, uint8_t format, int msb);
static void property_remove(struct x11server *server, struct x11_window *window, unsigned index);
static void property_title(struct x11_window *window, const struct x11_property *property);
static void property_forget_tree(struct x11_window *window, unsigned client);

/*
 * Gives the core's predefined atoms their numbers.  Returns 0, or ENOMEM.
 */
int
x11_atoms_init(
	struct x11server *server)
{
	uint32_t atom;
	unsigned index;
	int error;

	/* Each predefined name in turn gets its number, 1 on. */
	for (index = 0U; index < X11_ATOM_LAST_PREDEFINED; index++) {
		error = property_new_atom(server, property_predefined[index], strlen(property_predefined[index]), &atom);
		if (error != 0)
			return error;
	}

	/* Succeeded: the next name gets the number after the last predefined one. */
	return 0;
}

/*
 * Frees the atoms' names when the server ends.
 */
void
x11_atoms_fini(
	struct x11server *server)
{
	uint32_t index;

	/* Each name, then the tables. */
	for (index = 0U; index < server->atom_count; index++)
		free(server->atom_names[index]);
	free(server->atom_names);
	free(server->atom_lengths);
	free(server->atom_table);
	server->atom_names = NULL;
	server->atom_lengths = NULL;
	server->atom_table = NULL;
	server->atom_count = 0U;
}

/*
 * Finds a name's atom; a new name gets the next number unless only an
 * existing one is asked for.  Returns the atom, or 0 (None) for an unknown
 * name asked only if it exists, or when the server has no room for it.
 */
uint32_t
x11_atom_intern(
	struct x11server *server,
	const char *name,
	size_t length,
	int only_if_exists)
{
	uint32_t atom;
	int same;
	int error;

	/* zedBSD's icon path keeps its number. */
	if (length == sizeof(ATOM_ICON_PATH_NAME) - 1U) {
		same = memcmp(name, ATOM_ICON_PATH_NAME, length);
		if (same == 0)
			return X11_ATOM_ICON_PATH;
	}

	/* A name given a number before. */
	atom = property_lookup(server, name, length);
	if (atom != 0U)
		return atom;

	/* An unknown one asked only if it exists. */
	if (only_if_exists)
		return 0U;

	/* The next number, when there is room. */
	error = property_new_atom(server, name, length, &atom);
	if (error != 0)
		return 0U;

	/* Succeeded: the new atom. */
	return atom;
}

/*
 * Tells whether an atom names something (a predefined atom, one given a
 * number, or zedBSD's icon path).
 */
int
x11_atom_valid(
	struct x11server *server,
	uint32_t atom)
{
	/* zedBSD's icon path. */
	if (atom == X11_ATOM_ICON_PATH)
		return 1;

	/* None and the numbers not given. */
	if (atom == 0U || atom > server->atom_count)
		return 0;

	/* Succeeded: an atom. */
	return 1;
}

/*
 * Returns an atom's name (not NUL-terminated; its length in *length), or
 * NULL for an atom that names nothing.
 */
const char *
x11_atom_name(
	struct x11server *server,
	uint32_t atom,
	size_t *length)
{
	/* zedBSD's icon path. */
	if (atom == X11_ATOM_ICON_PATH) {
		*length = sizeof(ATOM_ICON_PATH_NAME) - 1U;
		return ATOM_ICON_PATH_NAME;
	}

	/* None and the numbers not given. */
	*length = 0U;
	if (atom == 0U || atom > server->atom_count)
		return NULL;

	/* Succeeded: the name. */
	*length = server->atom_lengths[atom - 1U];
	return server->atom_names[atom - 1U];
}

/*
 * Returns a window's property of a name, or NULL.
 */
struct x11_property *
x11_property_find(
	struct x11_window *window,
	uint32_t atom)
{
	unsigned index;

	/* Each of the window's properties. */
	for (index = 0U; index < window->property_count; index++) {
		if (window->properties[index].atom == atom)
			return &window->properties[index];
	}

	/* None of the name. */
	return NULL;
}

/*
 * Sets a property's data (16- and 32-bit units least significant byte
 * first): replaced (the type and format with it), or added before or
 * after what is there, charged to the writer (X11_NO_CLIENT: the server).
 * The window's PropertyNotify clients are told.  Returns 0, or the error:
 * BadMatch for data added of another type or format, BadAlloc.
 */
unsigned
x11_property_set(
	struct x11server *server,
	struct x11_window *window,
	uint32_t atom,
	uint32_t type,
	uint8_t format,
	unsigned mode,
	const uint8_t *data,
	size_t length,
	unsigned writer)
{
	struct x11_property *property;
	struct x11_property *grown;
	uint8_t *joined;
	unsigned capacity;
	size_t kept;
	size_t total;
	int error;

	/* The property there; added data must be of its type and format. */
	property = x11_property_find(window, atom);
	if (property != NULL && mode != PROPERTY_REPLACE) {
		if (property->type != type || property->format != format)
			return X11_BAD_MATCH;
	}

	/* The whole length, within what a property may hold. */
	kept = 0U;
	if (property != NULL && mode != PROPERTY_REPLACE)
		kept = property->length;
	if (length > PROPERTY_MAX || kept > PROPERTY_MAX - length)
		return X11_BAD_ALLOC;
	total = kept + length;

	/* A new property needs room in the window's list. */
	if (property == NULL && window->property_count == window->property_capacity) {
		capacity = PROPERTY_FIRST;
		if (window->property_capacity != 0U)
			capacity = window->property_capacity * 2U;
		grown = realloc(window->properties, (size_t)capacity * sizeof(*grown));
		if (grown == NULL)
			return X11_BAD_ALLOC;
		window->properties = grown;
		window->property_capacity = capacity;
	}

	/* The writer is charged for the whole of it, the holder before for nothing. */
	if (property != NULL)
		x11_memory_release(server, property->charged, property->length + PROPERTY_OVERHEAD);
	error = x11_memory_charge(server, writer, total + PROPERTY_OVERHEAD);
	if (error != 0) {
		if (property != NULL)
			(void)x11_memory_charge(server, property->charged, property->length + PROPERTY_OVERHEAD);
		return X11_BAD_ALLOC;
	}

	/* The new data: what is added, and what was there on its side. */
	joined = malloc(total + 1U);
	if (joined == NULL) {
		x11_memory_release(server, writer, total + PROPERTY_OVERHEAD);
		if (property != NULL)
			(void)x11_memory_charge(server, property->charged, property->length + PROPERTY_OVERHEAD);
		return X11_BAD_ALLOC;
	}
	if (mode == PROPERTY_PREPEND && kept != 0U) {
		memcpy(joined, data, length);
		memcpy(joined + length, property->data, kept);
	} else if (mode == PROPERTY_APPEND && kept != 0U) {
		memcpy(joined, property->data, kept);
		memcpy(joined + kept, data, length);
	} else {
		memcpy(joined, data, length);
	}
	joined[total] = 0U;

	/* A new property takes the next place in the list. */
	if (property == NULL) {
		property = &window->properties[window->property_count];
		window->property_count++;
		memset(property, 0, sizeof(*property));
		property->atom = atom;
	} else {
		free(property->data);
	}

	/* The property holds it. */
	property->type = type;
	property->format = format;
	property->data = joined;
	property->length = total;
	property->charged = writer;

	/* Succeeded: WM_NAME is the window's title, and the window's clients are told. */
	if (atom == X11_ATOM_WM_NAME)
		property_title(window, property);
	x11_event_property(server, window, atom, 0);
	return 0U;
}

/*
 * Deletes a window's property, telling the window's PropertyNotify clients
 * (one that is not there is nothing).
 */
void
x11_property_delete(
	struct x11server *server,
	struct x11_window *window,
	uint32_t atom)
{
	struct x11_property *property;

	/* The property, if the window has it. */
	property = x11_property_find(window, atom);
	if (property == NULL)
		return;

	/* It goes, and its clients are told. */
	property_remove(server, window, (unsigned)(property - window->properties));
	if (atom == X11_ATOM_WM_NAME)
		window->name[0] = '\0';
	x11_event_property(server, window, atom, 1);
}

/*
 * Frees every property of a window that is being destroyed (nobody is
 * told).
 */
void
x11_property_forget_window(
	struct x11server *server,
	struct x11_window *window)
{
	/* Each property, from the last. */
	while (window->property_count > 0U)
		property_remove(server, window, window->property_count - 1U);

	/* The list. */
	free(window->properties);
	window->properties = NULL;
	window->property_capacity = 0U;
}

/*
 * Moves the charge of every property a client wrote that outlives it (on
 * another client's window or the root window) to the server.
 */
void
x11_property_forget_client(
	struct x11server *server,
	unsigned client)
{
	/* Every window of the tree. */
	property_forget_tree(server->root, client);
}

/*
 * Gives the class of a window from its WM_CLASS (two strings, the instance
 * and the class; the instance when there is no class).  Returns 0, or
 * ENOENT when the window has none.
 */
int
x11_window_class(
	struct x11server *server,
	uint32_t window,
	char *name,
	size_t size)
{
	struct x11_property *property;
	struct x11_window *found;
	const char *data;
	size_t instance;
	size_t used;

	/* The window. */
	found = x11_window_find(server, window);
	if (found == NULL || size == 0U)
		return ENOENT;

	/* An 8-bit WM_CLASS with some text. */
	property = x11_property_find(found, X11_ATOM_WM_CLASS);
	if (property == NULL || property->format != 8U || property->length == 0U)
		return ENOENT;
	data = (const char *)property->data;

	/* The class after the instance's NUL, else the instance. */
	instance = strnlen(data, property->length);
	if (instance + 1U < property->length && data[instance + 1U] != '\0') {
		data += instance + 1U;
		instance = strnlen(data, property->length - (size_t)(data - (const char *)property->data));
	}

	/* As much as fits. */
	used = instance;
	if (used > size - 1U)
		used = size - 1U;
	memcpy(name, data, used);
	name[used] = '\0';
	if (used == 0U)
		return ENOENT;

	/* Succeeded: the class. */
	return 0;
}

/* InternAtom: a name's atom (only if it exists, when asked). */
unsigned
x11_request_intern_atom(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	uint8_t reply[32];
	size_t count;
	uint32_t atom;

	/* The name, filling the request to its last word. */
	client = &server->clients[index];
	if (length < 8U)
		return X11_BAD_LENGTH;
	count = x11_read16(request + 4, client->order);
	if (8U + ((count + 3U) & ~(size_t)3U) != length)
		return X11_BAD_LENGTH;
	if (request[1] > 1U)
		return X11_BAD_VALUE;

	/* The atom (None when there is no room for a new one). */
	atom = x11_atom_intern(server, (const char *)request + 8, count, request[1] != 0U);
	if (atom == 0U && request[1] == 0U)
		return X11_BAD_ALLOC;

	/* Succeeded: the reply. */
	memset(reply, 0, sizeof(reply));
	x11_write32(reply + 8, atom, client->order);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/* GetAtomName: an atom's name. */
unsigned
x11_request_get_atom_name(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	const char *name;
	uint8_t *reply;
	size_t count;
	size_t padded;

	/* The atom's name. */
	client = &server->clients[index];
	if (length != 8U)
		return X11_BAD_LENGTH;
	name = x11_atom_name(server, x11_read32(request + 4, client->order), &count);
	if (name == NULL)
		return X11_BAD_ATOM;

	/* The reply with the name, padded. */
	padded = (count + 3U) & ~(size_t)3U;
	reply = calloc(1U, 32U + padded);
	if (reply == NULL)
		return X11_BAD_ALLOC;
	x11_write16(reply + 8, (uint16_t)count, client->order);
	memcpy(reply + 32, name, count);
	x11_reply(server, client, reply, 32U + padded);
	free(reply);

	/* Succeeded: answered. */
	return 0U;
}

/* ChangeProperty: a property of a window replaced, prepended to or appended to. */
unsigned
x11_request_change_property(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_window *window;
	struct x11_client *client;
	uint8_t *data;
	uint32_t atom;
	uint32_t type;
	uint32_t count;
	uint64_t bytes;
	uint8_t format;
	unsigned code;
	int valid;

	/* The window, the mode, the property, its type and format, and its data's length. */
	client = &server->clients[index];
	if (length < 24U)
		return X11_BAD_LENGTH;
	if (request[1] > PROPERTY_APPEND)
		return X11_BAD_VALUE;
	window = x11_window_find(server, x11_read32(request + 4, client->order));
	if (window == NULL)
		return X11_BAD_WINDOW;
	atom = x11_read32(request + 8, client->order);
	valid = x11_atom_valid(server, atom);
	if (!valid)
		return X11_BAD_ATOM;
	type = x11_read32(request + 12, client->order);
	valid = x11_atom_valid(server, type);
	if (!valid)
		return X11_BAD_ATOM;
	format = request[16];
	if (format != 8U && format != 16U && format != 32U)
		return X11_BAD_VALUE;

	/* The data fills the request to its last word. */
	count = x11_read32(request + 20, client->order);
	bytes = (uint64_t)count * (uint64_t)(format / 8U);
	if (bytes > length - 24U || 24U + (((size_t)bytes + 3U) & ~(size_t)3U) != length)
		return X11_BAD_LENGTH;

	/* The data, least significant byte first. */
	data = malloc((size_t)bytes + 1U);
	if (data == NULL)
		return X11_BAD_ALLOC;
	property_order(data, request + 24, (size_t)bytes, format, client->order);

	/* Kept, charged to the client. */
	code = x11_property_set(server, window, atom, type, format, request[1], data, (size_t)bytes, index);
	free(data);
	if (code != 0U)
		return code;

	/* Succeeded: changed. */
	return 0U;
}

/* DeleteProperty: the property goes (one that is not there is fine). */
unsigned
x11_request_delete_property(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_window *window;
	struct x11_client *client;
	uint32_t atom;
	int valid;

	/* The window and the property. */
	client = &server->clients[index];
	if (length != 12U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, client->order));
	if (window == NULL)
		return X11_BAD_WINDOW;
	atom = x11_read32(request + 8, client->order);
	valid = x11_atom_valid(server, atom);
	if (!valid)
		return X11_BAD_ATOM;

	/* Succeeded: it is gone. */
	x11_property_delete(server, window, atom);
	return 0U;
}

/*
 * GetProperty: the type and format, the part asked for (from long-offset,
 * at most long-length units of four bytes) and what is left after it;
 * deleted when asked and all of it was read.  A property the window does
 * not have answers None; one of another type than asked answers its type
 * and length and no data.
 */
unsigned
x11_request_get_property(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_property *property;
	struct x11_window *window;
	struct x11_client *client;
	uint8_t *reply;
	uint32_t atom;
	uint32_t type;
	uint64_t offset;
	uint64_t most;
	size_t count;
	size_t after;
	size_t padded;
	int valid;
	int msb;

	/* The window, the property and the type asked for. */
	client = &server->clients[index];
	msb = client->order;
	if (length != 24U)
		return X11_BAD_LENGTH;
	if (request[1] > 1U)
		return X11_BAD_VALUE;
	window = x11_window_find(server, x11_read32(request + 4, msb));
	if (window == NULL)
		return X11_BAD_WINDOW;
	atom = x11_read32(request + 8, msb);
	valid = x11_atom_valid(server, atom);
	if (!valid)
		return X11_BAD_ATOM;
	type = x11_read32(request + 12, msb);
	if (type != PROPERTY_ANY_TYPE) {
		valid = x11_atom_valid(server, type);
		if (!valid)
			return X11_BAD_ATOM;
	}

	/* No such property: None. */
	property = x11_property_find(window, atom);
	if (property == NULL) {
		reply = calloc(1U, 32U);
		if (reply == NULL)
			return X11_BAD_ALLOC;
		x11_reply(server, client, reply, 32U);
		free(reply);
		return 0U;
	}

	/* Another type than asked: its type and format and its whole length, no data. */
	if (type != PROPERTY_ANY_TYPE && type != property->type) {
		reply = calloc(1U, 32U);
		if (reply == NULL)
			return X11_BAD_ALLOC;
		reply[1] = property->format;
		x11_write32(reply + 8, property->type, msb);
		x11_write32(reply + 12, (uint32_t)property->length, msb);
		x11_reply(server, client, reply, 32U);
		free(reply);
		return 0U;
	}

	/* The part asked for, and what is left after it. */
	offset = (uint64_t)x11_read32(request + 16, msb) * 4U;
	if (offset > property->length)
		return X11_BAD_VALUE;
	count = property->length - (size_t)offset;
	most = (uint64_t)x11_read32(request + 20, msb) * 4U;
	if (count > most)
		count = (size_t)most;
	after = property->length - (size_t)offset - count;
	padded = (count + 3U) & ~(size_t)3U;

	/* The reply: the type, the format, what is after, the units given, and the data in the client's order. */
	reply = calloc(1U, 32U + padded);
	if (reply == NULL)
		return X11_BAD_ALLOC;
	reply[1] = property->format;
	x11_write32(reply + 8, property->type, msb);
	x11_write32(reply + 12, (uint32_t)after, msb);
	x11_write32(reply + 16, (uint32_t)(count / (size_t)(property->format / 8U)), msb);
	property_order(reply + 32, property->data + offset, count, property->format, msb);
	x11_reply(server, client, reply, 32U + padded);
	free(reply);

	/* Deleted when asked and all of it was read. */
	if (request[1] != 0U && after == 0U)
		x11_property_delete(server, window, atom);

	/* Succeeded: answered. */
	return 0U;
}

/* ListProperties: the names of a window's properties. */
unsigned
x11_request_list_properties(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_window *window;
	struct x11_client *client;
	uint8_t *reply;
	unsigned slot;

	/* The window. */
	client = &server->clients[index];
	if (length != 8U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, client->order));
	if (window == NULL)
		return X11_BAD_WINDOW;

	/* The reply: the count and each atom. */
	reply = calloc(1U, 32U + (size_t)window->property_count * 4U);
	if (reply == NULL)
		return X11_BAD_ALLOC;
	x11_write16(reply + 8, (uint16_t)window->property_count, client->order);
	for (slot = 0U; slot < window->property_count; slot++)
		x11_write32(reply + 32 + (size_t)slot * 4U, window->properties[slot].atom, client->order);
	x11_reply(server, client, reply, 32U + (size_t)window->property_count * 4U);
	free(reply);

	/* Succeeded: answered. */
	return 0U;
}

/*
 * RotateProperties: the values of the properties named move along the
 * list by delta (property I's value becomes property I + delta's), and
 * each is told to the window's clients in the order named.
 */
unsigned
x11_request_rotate_properties(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_property *saved;
	struct x11_property **named;
	struct x11_window *window;
	struct x11_client *client;
	uint32_t atom;
	unsigned count;
	unsigned slot;
	unsigned other;
	long delta;
	int valid;

	/* The window, the count and delta, and the names filling the request. */
	client = &server->clients[index];
	if (length < 12U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, client->order));
	if (window == NULL)
		return X11_BAD_WINDOW;
	count = x11_read16(request + 8, client->order);
	if (12U + (size_t)count * 4U != length)
		return X11_BAD_LENGTH;
	delta = (int16_t)x11_read16(request + 10, client->order);

	/* Each name an atom, and a property of the window named once. */
	named = calloc((size_t)count + 1U, sizeof(*named));
	if (named == NULL)
		return X11_BAD_ALLOC;
	for (slot = 0U; slot < count; slot++) {
		atom = x11_read32(request + 12 + (size_t)slot * 4U, client->order);
		valid = x11_atom_valid(server, atom);
		if (!valid) {
			free(named);
			return X11_BAD_ATOM;
		}
		named[slot] = x11_property_find(window, atom);
		if (named[slot] == NULL) {
			free(named);
			return X11_BAD_MATCH;
		}
		for (other = 0U; other < slot; other++) {
			if (named[other] == named[slot]) {
				free(named);
				return X11_BAD_MATCH;
			}
		}
	}

	/* A delta that brings each value back changes nothing. */
	if (count == 0U) {
		free(named);
		return 0U;
	}
	delta %= (long)count;
	if (delta < 0)
		delta += (long)count;
	if (delta == 0) {
		free(named);
		return 0U;
	}

	/* The values as they were. */
	saved = malloc((size_t)count * sizeof(*saved));
	if (saved == NULL) {
		free(named);
		return X11_BAD_ALLOC;
	}
	for (slot = 0U; slot < count; slot++)
		saved[slot] = *named[slot];

	/* Each value to its new name (the names stay where they are). */
	for (slot = 0U; slot < count; slot++) {
		other = (unsigned)(((long)slot + delta) % (long)count);
		atom = named[other]->atom;
		*named[other] = saved[slot];
		named[other]->atom = atom;
	}
	free(saved);

	/* The title follows WM_NAME, and each property is told in the order named. */
	for (slot = 0U; slot < count; slot++) {
		if (named[slot]->atom == X11_ATOM_WM_NAME)
			property_title(window, named[slot]);
		x11_event_property(server, window, named[slot]->atom, 0);
	}
	free(named);

	/* Succeeded: rotated. */
	return 0U;
}

/* Returns the hash of a name (FNV-1a), never 0. */
static uint32_t
property_hash(
	const char *name,
	size_t length)
{
	uint32_t hash;
	size_t index;

	/* Each byte. */
	hash = 2166136261U;
	for (index = 0U; index < length; index++) {
		hash ^= (uint8_t)name[index];
		hash *= 16777619U;
	}

	/* Succeeded: the hash. */
	return hash;
}

/* Returns a name's atom, or 0 when the name has none. */
static uint32_t
property_lookup(
	struct x11server *server,
	const char *name,
	size_t length)
{
	uint32_t atom;
	size_t slot;
	int same;

	/* No table yet. */
	if (server->atom_table_capacity == 0U)
		return 0U;

	/* The probe, up to an empty slot. */
	slot = property_hash(name, length) & (server->atom_table_capacity - 1U);
	for (;;) {
		atom = server->atom_table[slot];
		if (atom == 0U)
			return 0U;
		if (server->atom_lengths[atom - 1U] == length) {
			same = memcmp(server->atom_names[atom - 1U], name, length);
			if (same == 0)
				return atom;
		}
		slot = (slot + 1U) & (server->atom_table_capacity - 1U);
	}
}

/* Gives a new name the next atom, charged to the server.  Returns 0, or ENOMEM. */
static int
property_new_atom(
	struct x11server *server,
	const char *name,
	size_t length,
	uint32_t *atom)
{
	char **names;
	uint16_t *lengths;
	uint32_t capacity;
	size_t slot;
	char *copy;
	int error;

	/* A name of up to the protocol's length, while the server gives numbers. */
	if (length > 0xffffU || server->atom_count >= ATOM_LIMIT)
		return ENOMEM;

	/* Room in the lists. */
	if (server->atom_count == server->atom_capacity) {
		capacity = ATOM_FIRST;
		if (server->atom_capacity != 0U)
			capacity = server->atom_capacity * 2U;
		names = realloc(server->atom_names, (size_t)capacity * sizeof(*names));
		if (names == NULL)
			return ENOMEM;
		server->atom_names = names;
		lengths = realloc(server->atom_lengths, (size_t)capacity * sizeof(*lengths));
		if (lengths == NULL)
			return ENOMEM;
		server->atom_lengths = lengths;
		server->atom_capacity = capacity;
	}

	/* Room in the table, kept at most half full. */
	if ((size_t)(server->atom_count + 1U) * 2U > server->atom_table_capacity) {
		error = property_table_grow(server);
		if (error != 0)
			return error;
	}

	/* The server holds the name. */
	error = x11_memory_charge(server, X11_NO_CLIENT, length + ATOM_OVERHEAD);
	if (error != 0)
		return error;
	copy = malloc(length + 1U);
	if (copy == NULL) {
		x11_memory_release(server, X11_NO_CLIENT, length + ATOM_OVERHEAD);
		return ENOMEM;
	}
	memcpy(copy, name, length);
	copy[length] = '\0';

	/* The name in the lists and in the table. */
	server->atom_names[server->atom_count] = copy;
	server->atom_lengths[server->atom_count] = (uint16_t)length;
	server->atom_count++;
	slot = property_hash(name, length) & (server->atom_table_capacity - 1U);
	while (server->atom_table[slot] != 0U)
		slot = (slot + 1U) & (server->atom_table_capacity - 1U);
	server->atom_table[slot] = server->atom_count;

	/* Succeeded: the atom. */
	*atom = server->atom_count;
	return 0;
}

/* Makes the name table twice as large and puts every atom back.  Returns 0, or ENOMEM. */
static int
property_table_grow(
	struct x11server *server)
{
	uint32_t *table;
	size_t capacity;
	size_t slot;
	uint32_t atom;

	/* The new table. */
	capacity = (size_t)ATOM_FIRST * 2U;
	if (server->atom_table_capacity != 0U)
		capacity = server->atom_table_capacity * 2U;
	table = calloc(capacity, sizeof(*table));
	if (table == NULL)
		return ENOMEM;

	/* Each atom at its place. */
	for (atom = 1U; atom <= server->atom_count; atom++) {
		slot = property_hash(server->atom_names[atom - 1U], server->atom_lengths[atom - 1U]) & (capacity - 1U);
		while (table[slot] != 0U)
			slot = (slot + 1U) & (capacity - 1U);
		table[slot] = atom;
	}

	/* Succeeded: the new table. */
	free(server->atom_table);
	server->atom_table = table;
	server->atom_table_capacity = capacity;
	return 0;
}

/*
 * Copies property data from one byte order to the other: 8-bit data as it
 * is, 16- and 32-bit units turned around when one side is most
 * significant byte first.
 */
static void
property_order(
	uint8_t *target,
	const uint8_t *source,
	size_t length,
	uint8_t format,
	int msb)
{
	size_t index;

	/* Bytes, or the same order on both sides. */
	if (format == 8U || !msb) {
		memcpy(target, source, length);
		return;
	}

	/* Each unit turned around. */
	if (format == 16U) {
		for (index = 0U; index + 2U <= length; index += 2U) {
			target[index] = source[index + 1U];
			target[index + 1U] = source[index];
		}
	} else {
		for (index = 0U; index + 4U <= length; index += 4U) {
			target[index] = source[index + 3U];
			target[index + 1U] = source[index + 2U];
			target[index + 2U] = source[index + 1U];
			target[index + 3U] = source[index];
		}
	}
}

/* Frees one of a window's properties, gives its charge back, and closes the list over it. */
static void
property_remove(
	struct x11server *server,
	struct x11_window *window,
	unsigned index)
{
	struct x11_property *property;

	/* The data and its charge. */
	property = &window->properties[index];
	x11_memory_release(server, property->charged, property->length + PROPERTY_OVERHEAD);
	free(property->data);

	/* The later properties move down over it. */
	memmove(&window->properties[index], &window->properties[index + 1U], (size_t)(window->property_count - index - 1U) * sizeof(*property));
	window->property_count--;
}

/* Keeps an 8-bit WM_NAME as the window's title, cut to the room there is. */
static void
property_title(
	struct x11_window *window,
	const struct x11_property *property)
{
	size_t copied;

	/* Only text. */
	window->name[0] = '\0';
	if (property->format != 8U)
		return;

	/* As much as fits, up to a NUL. */
	copied = strnlen((const char *)property->data, property->length);
	if (copied > sizeof(window->name) - 1U)
		copied = sizeof(window->name) - 1U;
	memcpy(window->name, property->data, copied);
	window->name[copied] = '\0';
}

/* Moves the charge of a client's properties to the server on a window and every window under it. */
static void
property_forget_tree(
	struct x11_window *top,
	unsigned client)
{
	struct x11_window *window;
	unsigned index;

	/* Each window of the tree, and each of its properties the client wrote. */
	for (window = top;
	     window != NULL;
	     window = x11_window_next(window, top)) {
		for (index = 0U; index < window->property_count; index++) {
			if (window->properties[index].charged == client)
				window->properties[index].charged = X11_NO_CLIENT;
		}
	}
}
