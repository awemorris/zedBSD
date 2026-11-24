/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Drops onto a window on Windows: OLE's IDropTarget registered on the
 * window's content (C vtable of IDropTarget).
 *
 * kl_window_accept_drops turns the target on or off for the types the
 * window takes (KL_DROP_URIS as CF_HDROP, KL_DROP_IMAGE as the
 * registered "PNG" format, KL_DROP_TEXT as CF_UNICODETEXT).  DragEnter,
 * DragOver, DragLeave and Drop push KL_WINDOW_DROP_ENTER, _MOTION,
 * _LEAVE and _DROP in content coordinates, and kl_window_answer_drop's
 * reply sets the effect returned to OLE.
 *
 * Drop reads and converts the dropped data before returning, because the
 * source's data object may be released as soon as Drop returns, while the
 * application reads the drop afterwards with kl_window_receive_drop or
 * kl_window_take_drop.
 */

#define COBJMACROS
#include "apps/libkeiland/win32/window-win32.h"
#include "apps/libkeiland/win32/wide.h"

#include <ole2.h>
#include <shellapi.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The largest drop read for each type (keiland_window.h). */
#define DROP_MAX_URIS		(1024U * 1024U)
#define DROP_MAX_TEXT		(16U * 1024U * 1024U)
#define DROP_MAX_IMAGE		(64U * 1024U * 1024U)

/* The initial buffer size when building a text/uri-list. */
#define DROP_URI_INITIAL	1024U

/*
 * The window's IDropTarget COM object: its vtable pointer, its reference
 * count, and the window it reports to (cleared when the window closes).
 */
struct drop_target {
	IDropTarget iface;
	LONG references;
	struct kl_window *window;
};

static CLIPFORMAT drop_png_format(void);
static unsigned drop_offered_types(struct kl_window *window, IDataObject *data_object);
static int drop_has_format(IDataObject *data_object, CLIPFORMAT format);
static void drop_client_point(const struct kl_window *window, POINTL pt, int32_t *x, int32_t *y);
static DWORD drop_effect(const struct kl_window *window, DWORD allowed);
static unsigned drop_pick_type(unsigned offered);
static void drop_clear_data(struct kl_window *window);
static void drop_read_object(struct kl_window *window, IDataObject *data_object, unsigned type);
static void drop_read_hdrop(struct kl_window *window, HGLOBAL memory);
static void drop_read_png(struct kl_window *window, HGLOBAL memory);
static void drop_read_text(struct kl_window *window, HGLOBAL memory);
static int drop_append_uri(char **buffer, size_t *length, size_t *capacity, const char *utf8);
static int drop_append_bytes(char **buffer, size_t *length, size_t *capacity, const char *bytes, size_t count);
static int drop_uri_unreserved(unsigned char byte);
static HRESULT STDMETHODCALLTYPE target_query(IDropTarget *iface, REFIID id, void **out);
static ULONG STDMETHODCALLTYPE target_add(IDropTarget *iface);
static ULONG STDMETHODCALLTYPE target_release(IDropTarget *iface);
static HRESULT STDMETHODCALLTYPE target_enter(IDropTarget *iface, IDataObject *data_object, DWORD key_state, POINTL pt, DWORD *effect);
static HRESULT STDMETHODCALLTYPE target_over(IDropTarget *iface, DWORD key_state, POINTL pt, DWORD *effect);
static HRESULT STDMETHODCALLTYPE target_leave(IDropTarget *iface);
static HRESULT STDMETHODCALLTYPE target_drop(IDropTarget *iface, IDataObject *data_object, DWORD key_state, POINTL pt, DWORD *effect);

/* The drop target's calls; constant for the life of the program. */
static IDropTargetVtbl drop_target_calls = {
	target_query,
	target_add,
	target_release,
	target_enter,
	target_over,
	target_leave,
	target_drop
};

/*
 * Takes the drags of some types (KL_DROP_*; 0 refuses every drag).
 * Returns 0, EINVAL for another type, ENOMEM, or ENOTSUP if OLE failed.
 */
int
kl_window_accept_drops(
	struct kl_window *window,
	unsigned types)
{
	struct drop_target *target;
	HRESULT result;

	/* A window, and only the types known. */
	if (window == NULL || (types & ~(KL_DROP_TEXT | KL_DROP_URIS | KL_DROP_IMAGE)) != 0U)
		return EINVAL;

	/* Updates the accepted types. */
	window->drop_types = types;

	/* Turning drops off revokes any registered target. */
	if (types == 0U) {
		if (window->drop_target != NULL) {
			target = window->drop_target;
			if (window->content != NULL)
				(void)RevokeDragDrop(window->content);
			target->window = NULL;
			IDropTarget_Release(&target->iface);
			window->drop_target = NULL;
		}
		return 0;
	}

	/* Already registered. */
	if (window->drop_target != NULL)
		return 0;

	/* OLE on the application's thread, once. */
	if (!window->app->ole) {
		result = OleInitialize(NULL);
		if (FAILED(result))
			return ENOTSUP;
		window->app->ole = 1;
	}

	/* Allocates and registers the drop target on the content HWND. */
	if (window->content == NULL)
		return EINVAL;
	target = calloc(1U, sizeof(*target));
	if (target == NULL)
		return ENOMEM;
	target->iface.lpVtbl = &drop_target_calls;
	target->references = 1;
	target->window = window;
	result = RegisterDragDrop(window->content, &target->iface);
	if (FAILED(result)) {
		IDropTarget_Release(&target->iface);
		return ENOTSUP;
	}
	window->drop_target = target;
	return 0;
}

/*
 * Answers the drop target for the drag over the window: the actions
 * taken (KL_DND_*; 0 takes nothing) and the one preferred.
 */
void
kl_window_answer_drop(
	struct kl_window *window,
	unsigned actions,
	unsigned preferred)
{
	/* Only a drag over the window. */
	if (window == NULL || window->drop_offered == 0U)
		return;

	/* Remembers the answer for DragOver and Drop. */
	window->drop_answered = 1;
	window->drop_answer_actions = actions;
	window->drop_answer_preferred = preferred;
}

/*
 * Reads what was dropped on the window into memory of its own (the
 * caller frees it): the file names ("text/uri-list"), the picture (a
 * PNG) or the text, and tells which (KL_DROP_*).  Returns 0, ENOENT
 * without a drop, ENOMEM, or E2BIG past the type's limit.
 */
int
kl_window_receive_drop(
	struct kl_window *window,
	char **data,
	size_t *length,
	unsigned *type)
{
	char *empty;
	int error;

	/* Nothing yet. */
	if (data == NULL || length == NULL || type == NULL)
		return EINVAL;
	*data = NULL;
	*length = 0U;
	*type = 0U;
	if (window == NULL || !window->drop_pending)
		return ENOENT;

	/* Reports any error from reading the drop. */
	if (window->drop_error != 0) {
		error = window->drop_error;
		return error;
	}

	/* Transfers ownership of the buffered drop data. */
	*type = window->drop_data_type;
	*length = window->drop_length;
	if (window->drop_data != NULL) {
		*data = window->drop_data;
		window->drop_data = NULL;
		window->drop_length = 0U;
		return 0;
	}

	/* An empty payload still gives a NUL-terminated buffer. */
	empty = malloc(1U);
	if (empty == NULL)
		return ENOMEM;
	empty[0] = '\0';
	*data = empty;
	window->drop_length = 0U;
	return 0;
}

/*
 * Ends the drop on the window and discards any unread data.
 */
void
kl_window_finish_drop(
	struct kl_window *window,
	unsigned action)
{
	(void)action;

	/* Only a window. */
	if (window == NULL)
		return;

	/* Clears the pending drop and its buffer. */
	window->drop_pending = 0;
	drop_clear_data(window);
}

/*
 * Reads what was dropped on the window into a buffer and finishes the
 * drop as a copy: the file names ("text/uri-list") or the text, and
 * tells which (KL_DROP_*, 0 with nothing).  Returns the bytes copied.
 */
size_t
kl_window_take_drop(
	struct kl_window *window,
	char *text,
	size_t size,
	unsigned *type)
{
	char *data;
	size_t length;
	int error;

	/* Only a drop waiting, taken once. */
	if (type != NULL)
		*type = 0U;
	if (window == NULL || text == NULL || size == 0U || type == NULL)
		return 0U;
	if (!window->drop_pending)
		return 0U;

	/* What was dropped, as much as the buffer holds. */
	length = 0U;
	error = kl_window_receive_drop(window, &data, &length, type);
	if (error == 0) {
		if (length > size)
			length = size;
		if (length != 0U)
			memcpy(text, data, length);
		free(data);
	}

	/* The drop is finished as a copy. */
	kl_window_finish_drop(window, KL_DND_COPY);

	/* Succeeded: the bytes copied. */
	return length;
}

/*
 * Revokes the window's drop target and frees any buffered drop data
 * before the window's HWNDs are destroyed.
 */
void
keiui_win32_drop_close(
	struct kl_window *window)
{
	struct drop_target *target;

	/* Revokes the registered target while the content HWND still exists. */
	if (window->drop_target != NULL) {
		target = window->drop_target;
		if (window->content != NULL)
			(void)RevokeDragDrop(window->content);
		target->window = NULL;
		IDropTarget_Release(&target->iface);
		window->drop_target = NULL;
	}

	/* Frees any unread drop payload. */
	window->drop_offered = 0U;
	window->drop_pending = 0;
	drop_clear_data(window);
}

/* Returns the registered clipboard format for PNG images. */
static CLIPFORMAT
drop_png_format(void)
{
	return (CLIPFORMAT)RegisterClipboardFormatW(L"PNG");
}

/* Checks which accepted KL_DROP_* types the data object offers. */
static unsigned
drop_offered_types(
	struct kl_window *window,
	IDataObject *data_object)
{
	unsigned offered;
	CLIPFORMAT png;

	/* Checks each supported format that the window accepts. */
	offered = 0U;
	if ((window->drop_types & KL_DROP_URIS) != 0U && drop_has_format(data_object, CF_HDROP))
		offered |= KL_DROP_URIS;
	if ((window->drop_types & KL_DROP_IMAGE) != 0U) {
		png = drop_png_format();
		if (png != 0U && drop_has_format(data_object, png))
			offered |= KL_DROP_IMAGE;
	}
	if ((window->drop_types & KL_DROP_TEXT) != 0U && drop_has_format(data_object, CF_UNICODETEXT))
		offered |= KL_DROP_TEXT;
	return offered;
}

/* Returns 1 if the data object has the given format as HGLOBAL, else 0. */
static int
drop_has_format(
	IDataObject *data_object,
	CLIPFORMAT format)
{
	FORMATETC etc;
	HRESULT result;

	/* Queries the data object without allocating storage. */
	memset(&etc, 0, sizeof(etc));
	etc.cfFormat = format;
	etc.dwAspect = DVASPECT_CONTENT;
	etc.lindex = -1;
	etc.tymed = TYMED_HGLOBAL;
	result = IDataObject_QueryGetData(data_object, &etc);
	if (result == S_OK)
		return 1;
	return 0;
}

/* Converts screen coordinates from OLE into content-relative pixels. */
static void
drop_client_point(
	const struct kl_window *window,
	POINTL pt,
	int32_t *x,
	int32_t *y)
{
	POINT point;

	/* Maps through the content HWND so coordinates match pointer events. */
	point.x = pt.x;
	point.y = pt.y;
	if (window->content != NULL)
		(void)ScreenToClient(window->content, &point);
	*x = (int32_t)point.x;
	*y = (int32_t)point.y;
}

/* Chooses the OLE drop effect from the application's answer and allowed effects. */
static DWORD
drop_effect(
	const struct kl_window *window,
	DWORD allowed)
{
	unsigned actions;
	unsigned preferred;

	/* Nothing accepted. */
	actions = window->drop_answer_actions;
	preferred = window->drop_answer_preferred;
	if (actions == 0U)
		return DROPEFFECT_NONE;

	/* Tries the preferred action first if both sides allow it. */
	if ((preferred & KL_DND_MOVE) != 0U &&
	    (actions & KL_DND_MOVE) != 0U &&
	    (allowed & DROPEFFECT_MOVE) != 0U)
		return DROPEFFECT_MOVE;
	if ((preferred & KL_DND_COPY) != 0U &&
	    (actions & KL_DND_COPY) != 0U &&
	    (allowed & DROPEFFECT_COPY) != 0U)
		return DROPEFFECT_COPY;

	/* Falls back to any mutually allowed action. */
	if ((actions & KL_DND_COPY) != 0U && (allowed & DROPEFFECT_COPY) != 0U)
		return DROPEFFECT_COPY;
	if ((actions & KL_DND_MOVE) != 0U && (allowed & DROPEFFECT_MOVE) != 0U)
		return DROPEFFECT_MOVE;
	return DROPEFFECT_NONE;
}

/* Picks the highest-priority KL_DROP_* type from offered bits. */
static unsigned
drop_pick_type(
	unsigned offered)
{
	/* File names first, then picture, then text. */
	if ((offered & KL_DROP_URIS) != 0U)
		return KL_DROP_URIS;
	if ((offered & KL_DROP_IMAGE) != 0U)
		return KL_DROP_IMAGE;
	if ((offered & KL_DROP_TEXT) != 0U)
		return KL_DROP_TEXT;
	return 0U;
}

/* Frees any buffered drop payload on the window. */
static void
drop_clear_data(
	struct kl_window *window)
{
	/* Discards the buffer and resets metadata. */
	free(window->drop_data);
	window->drop_data = NULL;
	window->drop_length = 0U;
	window->drop_data_type = 0U;
	window->drop_error = 0;
}

/* Reads the chosen format from the data object into window->drop_data. */
static void
drop_read_object(
	struct kl_window *window,
	IDataObject *data_object,
	unsigned type)
{
	FORMATETC etc;
	STGMEDIUM medium;
	HRESULT result;

	/* Prepares the format request for the chosen type. */
	drop_clear_data(window);
	window->drop_data_type = type;
	memset(&etc, 0, sizeof(etc));
	etc.dwAspect = DVASPECT_CONTENT;
	etc.lindex = -1;
	etc.tymed = TYMED_HGLOBAL;
	if (type == KL_DROP_URIS)
		etc.cfFormat = CF_HDROP;
	else if (type == KL_DROP_IMAGE)
		etc.cfFormat = drop_png_format();
	else if (type == KL_DROP_TEXT)
		etc.cfFormat = CF_UNICODETEXT;
	else {
		window->drop_error = ENOENT;
		return;
	}

	/* Requests the storage medium from the source. */
	memset(&medium, 0, sizeof(medium));
	result = IDataObject_GetData(data_object, &etc, &medium);
	if (FAILED(result) || medium.tymed != TYMED_HGLOBAL || medium.hGlobal == NULL) {
		if (SUCCEEDED(result))
			ReleaseStgMedium(&medium);
		window->drop_error = ENOENT;
		return;
	}

	/* Extracts and converts the payload, then releases the medium. */
	if (type == KL_DROP_URIS)
		drop_read_hdrop(window, medium.hGlobal);
	else if (type == KL_DROP_IMAGE)
		drop_read_png(window, medium.hGlobal);
	else
		drop_read_text(window, medium.hGlobal);
	ReleaseStgMedium(&medium);
}

/* Extracts CF_HDROP paths into a UTF-8 text/uri-list buffer. */
static void
drop_read_hdrop(
	struct kl_window *window,
	HGLOBAL memory)
{
	HDROP hdrop;
	UINT count;
	UINT index;
	UINT chars;
	WCHAR *wide;
	char *utf8;
	char *uris;
	size_t uri_len;
	size_t uri_cap;
	size_t utf8_cap;
	int error;

	/* Counts the dropped paths. */
	hdrop = (HDROP)memory;
	count = DragQueryFileW(hdrop, 0xFFFFFFFFU, NULL, 0U);
	uris = NULL;
	uri_len = 0U;
	uri_cap = 0U;
	error = 0;

	/* Converts each path to a file:// URI line. */
	for (index = 0U; index < count; index++) {
		chars = DragQueryFileW(hdrop, index, NULL, 0U);
		wide = malloc(((size_t)chars + 1U) * sizeof(WCHAR));
		if (wide == NULL) {
			error = ENOMEM;
			break;
		}
		if (DragQueryFileW(hdrop, index, wide, chars + 1U) == 0U) {
			free(wide);
			continue;
		}
		utf8_cap = (size_t)chars * 4U + 1U;
		utf8 = malloc(utf8_cap);
		if (utf8 == NULL) {
			free(wide);
			error = ENOMEM;
			break;
		}
		error = keiui_win32_utf8_from_utf16((const uint16_t *)wide, (size_t)chars, utf8, utf8_cap, NULL);
		free(wide);
		if (error != 0) {
			free(utf8);
			break;
		}
		error = drop_append_uri(&uris, &uri_len, &uri_cap, utf8);
		free(utf8);
		if (error != 0)
			break;
	}

	/* Keeps the result or records the error. */
	if (error != 0) {
		free(uris);
		window->drop_error = error;
		return;
	}
	window->drop_data = uris;
	window->drop_length = uri_len;
}

/* Copies a PNG payload from global memory up to DROP_MAX_IMAGE. */
static void
drop_read_png(
	struct kl_window *window,
	HGLOBAL memory)
{
	SIZE_T raw_size;
	const void *locked;
	char *copy;

	/* Checks the size before copying. */
	raw_size = GlobalSize(memory);
	if (raw_size > DROP_MAX_IMAGE) {
		window->drop_error = E2BIG;
		return;
	}
	locked = GlobalLock(memory);
	if (locked == NULL) {
		window->drop_error = ENOENT;
		return;
	}

	/* Copies the PNG bytes with a trailing NUL. */
	copy = malloc((size_t)raw_size + 1U);
	if (copy == NULL) {
		(void)GlobalUnlock(memory);
		window->drop_error = ENOMEM;
		return;
	}
	if (raw_size != 0U)
		memcpy(copy, locked, (size_t)raw_size);
	copy[raw_size] = '\0';
	(void)GlobalUnlock(memory);
	window->drop_data = copy;
	window->drop_length = (size_t)raw_size;
}

/* Converts CF_UNICODETEXT from global memory to UTF-8 up to DROP_MAX_TEXT. */
static void
drop_read_text(
	struct kl_window *window,
	HGLOBAL memory)
{
	SIZE_T raw_bytes;
	size_t units_count;
	const uint16_t *locked;
	size_t cap;
	char *utf8;
	size_t actual;
	int error;

	/* Locks the UTF-16 buffer and bounds its unit count. */
	raw_bytes = GlobalSize(memory);
	units_count = (size_t)(raw_bytes / sizeof(uint16_t));
	locked = (const uint16_t *)GlobalLock(memory);
	if (locked == NULL) {
		window->drop_error = ENOENT;
		return;
	}

	/* Measures up to the first NUL unit. */
	actual = 0U;
	while (actual < units_count && locked[actual] != 0U)
		actual++;

	/* Allocates enough UTF-8 room (at most 3 bytes per BMP unit, 4 per pair). */
	cap = actual * 4U + 1U;
	if (cap > DROP_MAX_TEXT + 4U)
		cap = DROP_MAX_TEXT + 4U;
	utf8 = malloc(cap);
	if (utf8 == NULL) {
		(void)GlobalUnlock(memory);
		window->drop_error = ENOMEM;
		return;
	}

	/* Converts and checks the size limit. */
	error = keiui_win32_utf8_from_utf16(locked, actual, utf8, cap, NULL);
	(void)GlobalUnlock(memory);
	if (error == ERANGE) {
		free(utf8);
		window->drop_error = E2BIG;
		return;
	}
	if (error != 0) {
		free(utf8);
		window->drop_error = error;
		return;
	}
	actual = strlen(utf8);
	if (actual > DROP_MAX_TEXT) {
		free(utf8);
		window->drop_error = E2BIG;
		return;
	}
	window->drop_data = utf8;
	window->drop_length = actual;
}

/* Appends one Windows UTF-8 path as a percent-encoded file:// URI line. */
static int
drop_append_uri(
	char **buffer,
	size_t *length,
	size_t *capacity,
	const char *utf8)
{
	static const char hex[] = "0123456789ABCDEF";
	const char *prefix;
	size_t index;
	unsigned char byte;
	char encoded[3];
	int error;

	/* UNC paths (\\server\share) become file://server/share; drive paths become file:///C:/... */
	if (utf8[0] == '\\' && utf8[1] == '\\') {
		prefix = "file:";
	} else {
		prefix = "file:///";
	}
	error = drop_append_bytes(buffer, length, capacity, prefix, strlen(prefix));
	if (error != 0)
		return error;

	/* Encodes each byte, turning backslashes into forward slashes. */
	for (index = 0U; utf8[index] != '\0'; index++) {
		byte = (unsigned char)utf8[index];
		if (byte == '\\')
			byte = '/';
		if (drop_uri_unreserved(byte) || byte == '/' || byte == ':') {
			encoded[0] = (char)byte;
			error = drop_append_bytes(buffer, length, capacity, encoded, 1U);
		} else {
			encoded[0] = '%';
			encoded[1] = hex[(byte >> 4) & 0x0FU];
			encoded[2] = hex[byte & 0x0FU];
			error = drop_append_bytes(buffer, length, capacity, encoded, 3U);
		}
		if (error != 0)
			return error;
	}

	/* Ends the text/uri-list line with CRLF. */
	return drop_append_bytes(buffer, length, capacity, "\r\n", 2U);
}

/* Appends bytes to a growing buffer, enforcing DROP_MAX_URIS. */
static int
drop_append_bytes(
	char **buffer,
	size_t *length,
	size_t *capacity,
	const char *bytes,
	size_t count)
{
	size_t needed;
	size_t next;
	char *grown;

	/* Enforces the 1 MiB limit for URI lists. */
	if (count > DROP_MAX_URIS || *length + count > DROP_MAX_URIS)
		return E2BIG;
	needed = *length + count + 1U;
	if (needed > *capacity) {
		next = *capacity == 0U ? DROP_URI_INITIAL : *capacity;
		while (next < needed)
			next *= 2U;
		grown = realloc(*buffer, next);
		if (grown == NULL)
			return ENOMEM;
		*buffer = grown;
		*capacity = next;
	}

	/* Copies the bytes and keeps the buffer NUL-terminated. */
	if (count != 0U)
		memcpy(*buffer + *length, bytes, count);
	*length += count;
	(*buffer)[*length] = '\0';
	return 0;
}

/* Returns 1 for RFC 3986 unreserved characters. */
static int
drop_uri_unreserved(
	unsigned char byte)
{
	if (byte >= 'A' && byte <= 'Z')
		return 1;
	if (byte >= 'a' && byte <= 'z')
		return 1;
	if (byte >= '0' && byte <= '9')
		return 1;
	if (byte == '-' || byte == '.' || byte == '_' || byte == '~')
		return 1;
	return 0;
}

/* Answers IUnknown and IDropTarget queries. */
static HRESULT STDMETHODCALLTYPE
target_query(
	IDropTarget *iface,
	REFIID id,
	void **out)
{
	/* Nothing yet. */
	if (out == NULL)
		return E_POINTER;
	*out = NULL;
	if (IsEqualIID(id, &IID_IUnknown) || IsEqualIID(id, &IID_IDropTarget)) {
		*out = iface;
		IDropTarget_AddRef(iface);
		return S_OK;
	}
	return E_NOINTERFACE;
}

/* Adds one reference to the drop target. */
static ULONG STDMETHODCALLTYPE
target_add(
	IDropTarget *iface)
{
	struct drop_target *target;

	/* Counts the reference. */
	target = (struct drop_target *)iface;
	return (ULONG)InterlockedIncrement(&target->references);
}

/* Drops one reference from the drop target and frees it at zero. */
static ULONG STDMETHODCALLTYPE
target_release(
	IDropTarget *iface)
{
	struct drop_target *target;
	LONG left;

	/* Frees the target when the last reference leaves. */
	target = (struct drop_target *)iface;
	left = InterlockedDecrement(&target->references);
	if (left == 0)
		free(target);
	return (ULONG)left;
}

/* Handles a drag entering the content window. */
static HRESULT STDMETHODCALLTYPE
target_enter(
	IDropTarget *iface,
	IDataObject *data_object,
	DWORD key_state,
	POINTL pt,
	DWORD *effect)
{
	struct drop_target *target;
	struct kl_window *window;
	struct kl_window_event *input;
	unsigned offered;

	(void)key_state;

	/* Refuses if the window is gone or offers nothing accepted. */
	target = (struct drop_target *)iface;
	window = target->window;
	if (effect == NULL)
		return E_POINTER;
	if (window == NULL || data_object == NULL) {
		*effect = DROPEFFECT_NONE;
		return S_OK;
	}
	offered = drop_offered_types(window, data_object);
	if (offered == 0U) {
		window->drop_offered = 0U;
		*effect = DROPEFFECT_NONE;
		return S_OK;
	}

	/* Records the drag and defaults to copy until the application answers. */
	window->drop_offered = offered;
	window->drop_answered = 0;
	window->drop_answer_actions = KL_DND_COPY;
	window->drop_answer_preferred = KL_DND_COPY;
	drop_client_point(window, pt, &window->drop_x, &window->drop_y);

	/* Pushes KL_WINDOW_DROP_ENTER with offered types and whether it is the window's own drag. */
	input = keiui_win32_push(window, KL_WINDOW_DROP_ENTER);
	if (input != NULL) {
		input->code = offered;
		input->x = window->drop_x;
		input->y = window->drop_y;
		if (window->dragging)
			input->pressed = 1;
	}

	/* Returns the initial effect. */
	*effect = drop_effect(window, *effect);
	return S_OK;
}

/* Handles a drag moving over the content window. */
static HRESULT STDMETHODCALLTYPE
target_over(
	IDropTarget *iface,
	DWORD key_state,
	POINTL pt,
	DWORD *effect)
{
	struct drop_target *target;
	struct kl_window *window;
	struct kl_window_event *input;

	(void)key_state;

	/* Ignores motion when no accepted drag is over the window. */
	target = (struct drop_target *)iface;
	window = target->window;
	if (effect == NULL)
		return E_POINTER;
	if (window == NULL || window->drop_offered == 0U) {
		*effect = DROPEFFECT_NONE;
		return S_OK;
	}

	/* Updates coordinates and pushes KL_WINDOW_DROP_MOTION. */
	drop_client_point(window, pt, &window->drop_x, &window->drop_y);
	input = keiui_win32_push(window, KL_WINDOW_DROP_MOTION);
	if (input != NULL) {
		input->code = window->drop_offered;
		input->x = window->drop_x;
		input->y = window->drop_y;
	}

	/* Returns the effect from the application's latest answer. */
	*effect = drop_effect(window, *effect);
	return S_OK;
}

/* Handles a drag leaving the content window. */
static HRESULT STDMETHODCALLTYPE
target_leave(
	IDropTarget *iface)
{
	struct drop_target *target;
	struct kl_window *window;
	struct kl_window_event *input;

	/* Ignores leave when no accepted drag was active. */
	target = (struct drop_target *)iface;
	window = target->window;
	if (window == NULL || window->drop_offered == 0U)
		return S_OK;

	/* Clears the drag state and pushes KL_WINDOW_DROP_LEAVE. */
	window->drop_offered = 0U;
	input = keiui_win32_push(window, KL_WINDOW_DROP_LEAVE);
	if (input != NULL) {
		input->code = 0U;
		input->x = window->drop_x;
		input->y = window->drop_y;
	}
	return S_OK;
}

/* Handles a drop onto the content window. */
static HRESULT STDMETHODCALLTYPE
target_drop(
	IDropTarget *iface,
	IDataObject *data_object,
	DWORD key_state,
	POINTL pt,
	DWORD *effect)
{
	struct drop_target *target;
	struct kl_window *window;
	struct kl_window_event *input;
	unsigned type;
	DWORD chosen;

	(void)key_state;

	/* Ignores drop when no accepted drag was over the window. */
	target = (struct drop_target *)iface;
	window = target->window;
	if (effect == NULL)
		return E_POINTER;
	if (window == NULL || data_object == NULL || window->drop_offered == 0U) {
		*effect = DROPEFFECT_NONE;
		return S_OK;
	}

	/* Determines the chosen effect and the format to read. */
	chosen = drop_effect(window, *effect);
	type = drop_pick_type(window->drop_offered);
	drop_client_point(window, pt, &window->drop_x, &window->drop_y);
	window->drop_offered = 0U;
	if (chosen == DROPEFFECT_NONE || type == 0U) {
		*effect = DROPEFFECT_NONE;
		return S_OK;
	}

	/* Reads the payload synchronously before OLE releases the data object. */
	drop_read_object(window, data_object, type);
	window->drop_pending = 1;

	/* Notifies the application with KL_WINDOW_DROP. */
	input = keiui_win32_push(window, KL_WINDOW_DROP);
	if (input != NULL) {
		input->code = type;
		input->x = window->drop_x;
		input->y = window->drop_y;
	}

	/* Reports the effect carried out. */
	*effect = chosen;
	return S_OK;
}
