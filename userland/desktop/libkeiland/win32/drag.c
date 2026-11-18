/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A drag out of a window on Windows: OLE's DoDragDrop with a data object
 * and a drop source of this file (C vtables of IDataObject and
 * IDropSource).
 *
 * The program starts the drag from a press (kl_window_start_drag_icon,
 * kl_window_start_drag, kl_window_drag_text) and may fill a type's data
 * afterwards (kl_window_drag_fill) before it dispatches again.  So the
 * start keeps a copy of the types and their data and posts a message to
 * the frame; DoDragDrop, which keeps the thread in the system's loop until
 * the drop, runs when the dispatch takes that message, with the data
 * filled by then.  Its end is the window's KL_WINDOW_DRAG_DONE (code 1 when
 * it was dropped, begin the action: KL_DND_COPY or KL_DND_MOVE).
 *
 * The types become the clipboard's formats: "image/png" the registered
 * "PNG" (what the Windows programs that take a picture read), the text
 * types ("text/plain", "text/plain;charset=utf-8", "UTF8_STRING")
 * CF_UNICODETEXT, "text/uri-list" CF_UNICODETEXT of its first URI and
 * "text/html" CF_HTML (cf-html.c).  A type of none of these is left out.
 * The drag's picture is not shown (the system's cursor shows the drag).
 * OLE is started on the application's thread at the first drag.
 */

#define COBJMACROS
#include "userland/desktop/libkeiland/win32/window-win32.h"
#include "userland/desktop/libkeiland/win32/cf-html.h"
#include "userland/desktop/libkeiland/win32/wide.h"

#include <ole2.h>
#include <shlobj.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The most types one drag offers. */
#define DRAG_TYPES		8U

/* The kinds of format a type becomes. */
#define DRAG_PNG		1U
#define DRAG_TEXT		2U
#define DRAG_URIS		3U
#define DRAG_HTML		4U

/*
 * One type of a drag: its name, its data (a copy, NULL until filled), and
 * the format it is offered as.
 */
struct drag_type {
	char *name;
	unsigned char *data;
	size_t length;
	unsigned kind;
	CLIPFORMAT format;
};

/*
 * A drag waiting for its DoDragDrop, or running: its window, its types,
 * the actions it allows (KL_DND_*), and the references the data object
 * and the drop source hold (the drag is freed when both let it go).
 */
struct keiui_win32_drag {
	struct kl_window *window;
	struct drag_type types[DRAG_TYPES];
	unsigned count;
	unsigned actions;
};

/* The data object over a drag, and its references. */
struct drag_object {
	IDataObject iface;
	LONG references;
	struct keiui_win32_drag *drag;
};

/* The drop source, and its references. */
struct drag_source {
	IDropSource iface;
	LONG references;
};

static int drag_begin(struct kl_window *window, const struct kl_drag_data *data, size_t count, unsigned actions);
static void drag_free(struct keiui_win32_drag *drag);
static unsigned drag_kind(const char *type);
static int drag_find(const struct keiui_win32_drag *drag, const FORMATETC *format);
static HGLOBAL drag_global(const struct drag_type *type);
static HRESULT STDMETHODCALLTYPE object_query(IDataObject *iface, REFIID id, void **out);
static ULONG STDMETHODCALLTYPE object_add(IDataObject *iface);
static ULONG STDMETHODCALLTYPE object_release(IDataObject *iface);
static HRESULT STDMETHODCALLTYPE object_get(IDataObject *iface, FORMATETC *format, STGMEDIUM *medium);
static HRESULT STDMETHODCALLTYPE object_get_here(IDataObject *iface, FORMATETC *format, STGMEDIUM *medium);
static HRESULT STDMETHODCALLTYPE object_has(IDataObject *iface, FORMATETC *format);
static HRESULT STDMETHODCALLTYPE object_canonical(IDataObject *iface, FORMATETC *format, FORMATETC *out);
static HRESULT STDMETHODCALLTYPE object_set(IDataObject *iface, FORMATETC *format, STGMEDIUM *medium, BOOL release);
static HRESULT STDMETHODCALLTYPE object_formats(IDataObject *iface, DWORD direction, IEnumFORMATETC **formats);
static HRESULT STDMETHODCALLTYPE object_advise(IDataObject *iface, FORMATETC *format, DWORD flags, IAdviseSink *sink, DWORD *connection);
static HRESULT STDMETHODCALLTYPE object_unadvise(IDataObject *iface, DWORD connection);
static HRESULT STDMETHODCALLTYPE object_advices(IDataObject *iface, IEnumSTATDATA **advices);
static HRESULT STDMETHODCALLTYPE source_query(IDropSource *iface, REFIID id, void **out);
static ULONG STDMETHODCALLTYPE source_add(IDropSource *iface);
static ULONG STDMETHODCALLTYPE source_release(IDropSource *iface);
static HRESULT STDMETHODCALLTYPE source_continue(IDropSource *iface, BOOL escape, DWORD keys);
static HRESULT STDMETHODCALLTYPE source_feedback(IDropSource *iface, DWORD effect);

/* The data object's calls; constant for the life of the program. */
static IDataObjectVtbl drag_object_calls = {
	object_query,
	object_add,
	object_release,
	object_get,
	object_get_here,
	object_has,
	object_canonical,
	object_set,
	object_formats,
	object_advise,
	object_unadvise,
	object_advices
};

/* The drop source's calls; constant for the life of the program. */
static IDropSourceVtbl drag_source_calls = {
	source_query,
	source_add,
	source_release,
	source_continue,
	source_feedback
};

/*
 * Starts a drag of a few types out of the window, with its picture (not
 * shown on Windows).  Returns 0, EINVAL, EBUSY (a drag is on), ENOMEM.
 */
int
kl_window_start_drag_icon(
	struct kl_window *window,
	const struct kl_drag_data *data,
	size_t count,
	unsigned actions,
	uint32_t serial,
	const struct kl_drag_icon *icon)
{
	int error;

	/* The serial and the icon are not used on this system. */
	(void)serial;
	(void)icon;

	/* The drag. */
	error = drag_begin(window, data, count, actions);
	if (error != 0)
		return error;

	/* Succeeded: it starts at the next dispatch. */
	return 0;
}

/*
 * Starts a drag of a few types out of the window.  Returns 0, EINVAL,
 * EBUSY (a drag is on), ENOMEM.
 */
int
kl_window_start_drag(
	struct kl_window *window,
	const struct kl_drag_data *data,
	size_t count,
	unsigned actions,
	uint32_t serial)
{
	int error;

	/* The serial is not used on this system. */
	(void)serial;

	/* The drag. */
	error = drag_begin(window, data, count, actions);
	if (error != 0)
		return error;

	/* Succeeded: it starts at the next dispatch. */
	return 0;
}

/*
 * Starts a drag of text out of the window, a copy.  Returns 0, EINVAL,
 * EBUSY, ENOMEM.
 */
int
kl_window_drag_text(
	struct kl_window *window,
	const char *text,
	size_t length,
	uint32_t serial)
{
	struct kl_drag_data data;
	int error;

	/* The serial is not used on this system. */
	(void)serial;

	/* Text. */
	if (text == NULL)
		return EINVAL;

	/* The drag of its one type. */
	data.type = "text/plain;charset=utf-8";
	data.data = text;
	data.length = length;
	error = drag_begin(window, &data, 1U, KL_DND_COPY);
	if (error != 0)
		return error;

	/* Succeeded: it starts at the next dispatch. */
	return 0;
}

/*
 * Fills the data of a type of the drag that waits for its start.  Returns
 * 0, EINVAL (no such type, or no drag waiting), ENOMEM.
 */
int
kl_window_drag_fill(
	struct kl_window *window,
	const char *type,
	const void *data,
	size_t length)
{
	struct keiui_win32_drag *drag;
	unsigned char *copy;
	unsigned index;
	int same;

	/* A window with a drag, a type and its data. */
	if (window == NULL || type == NULL || (data == NULL && length != 0U))
		return EINVAL;
	drag = window->drag;
	if (drag == NULL)
		return EINVAL;

	/* The type's data, a copy. */
	for (index = 0U; index < drag->count; index++) {
		same = strcmp(drag->types[index].name, type);
		if (same != 0)
			continue;

		/* The copy in place of what it had. */
		copy = malloc(length + 1U);
		if (copy == NULL)
			return ENOMEM;
		if (length != 0U)
			memcpy(copy, data, length);
		free(drag->types[index].data);
		drag->types[index].data = copy;
		drag->types[index].length = length;
		return 0;
	}

	/* No such type. */
	return EINVAL;
}

/*
 * Runs the drag that waits for its start (the frame took its message):
 * DoDragDrop, until the drop or its end, then the window's
 * KL_WINDOW_DRAG_DONE.
 */
void
keiui_win32_drag_run(
	struct kl_window *window)
{
	struct keiui_win32_drag *drag;
	struct drag_object *object;
	struct drag_source *source;
	struct kl_window_event *input;
	DWORD allowed;
	DWORD effect;
	HRESULT result;

	/* The drag that waits; none, nothing to do. */
	drag = window->drag;
	if (drag == NULL)
		return;
	window->drag = NULL;

	/* OLE on the application's thread, once. */
	if (!window->app->ole) {
		result = OleInitialize(NULL);
		if (SUCCEEDED(result))
			window->app->ole = 1;
	}

	/* The data object and the drop source, each holding the drag. */
	object = calloc(1U, sizeof(*object));
	source = calloc(1U, sizeof(*source));
	if (object == NULL || source == NULL || !window->app->ole) {
		free(object);
		free(source);
		drag_free(drag);
		input = keiui_win32_push(window, KL_WINDOW_DRAG_DONE);
		if (input != NULL)
			input->code = 0U;
		return;
	}

	/* Joins the object's interface and gives it its first reference. */
	object->iface.lpVtbl = &drag_object_calls;
	object->references = 1;
	object->drag = drag;
	source->iface.lpVtbl = &drag_source_calls;
	source->references = 1;

	/* The actions, and the system's loop until the drop. */
	allowed = 0U;
	if ((drag->actions & KL_DND_COPY) != 0U)
		allowed |= DROPEFFECT_COPY;
	if ((drag->actions & KL_DND_MOVE) != 0U)
		allowed |= DROPEFFECT_MOVE;
	if (allowed == 0U)
		allowed = DROPEFFECT_COPY;
	effect = DROPEFFECT_NONE;
	result = DoDragDrop(&object->iface, &source->iface, allowed, &effect);
	IDropSource_Release(&source->iface);
	IDataObject_Release(&object->iface);

	/* The end: dropped or not, and the action the target carried out. */
	input = keiui_win32_push(window, KL_WINDOW_DRAG_DONE);
	if (input == NULL)
		return;
	input->code = 0U;
	if (result == DRAGDROP_S_DROP && effect != DROPEFFECT_NONE)
		input->code = 1U;
	input->begin = KL_DND_COPY;
	if ((effect & DROPEFFECT_MOVE) != 0U)
		input->begin = KL_DND_MOVE;
}

/*
 * Forgets the drag a window keeps waiting (the window closes).
 */
void
keiui_win32_drag_drop(
	struct kl_window *window)
{
	/* The drag, freed. */
	drag_free(window->drag);
	window->drag = NULL;
}

/* Keeps a copy of a drag's types for its start and posts the start; 0 or an errno value. */
static int
drag_begin(
	struct kl_window *window,
	const struct kl_drag_data *data,
	size_t count,
	unsigned actions)
{
	struct keiui_win32_drag *drag;
	struct drag_type *type;
	size_t index;
	size_t name;
	unsigned kind;
	BOOL posted;

	/* A window, the types, and no drag on. */
	if (window == NULL || data == NULL || count == 0U)
		return EINVAL;
	if (window->drag != NULL)
		return EBUSY;

	/* The drag. */
	drag = calloc(1U, sizeof(*drag));
	if (drag == NULL)
		return ENOMEM;
	drag->window = window;
	drag->actions = actions;

	/* Each type this backend offers, copied (the first DRAG_TYPES). */
	for (index = 0U; index < count && drag->count < DRAG_TYPES; index++) {
		if (data[index].type == NULL)
			continue;
		kind = drag_kind(data[index].type);
		if (kind == 0U)
			continue;

		/* Its name. */
		type = &drag->types[drag->count];
		name = strlen(data[index].type);
		type->name = malloc(name + 1U);
		if (type->name == NULL) {
			drag_free(drag);
			return ENOMEM;
		}

		/* Copies the type's name. */
		memcpy(type->name, data[index].type, name + 1U);
		type->kind = kind;
		drag->count++;

		/* Its data, when it is given now. */
		if (data[index].data != NULL) {
			type->data = malloc(data[index].length + 1U);
			if (type->data == NULL) {
				drag_free(drag);
				return ENOMEM;
			}

			/* Copies the type's data. */
			memcpy(type->data, data[index].data, data[index].length);
			type->length = data[index].length;
		}
	}

	/* No type this backend offers. */
	if (drag->count == 0U) {
		drag_free(drag);
		return EINVAL;
	}

	/* The formats. */
	for (index = 0U; index < drag->count; index++) {
		type = &drag->types[index];
		type->format = CF_UNICODETEXT;
		if (type->kind == DRAG_PNG)
			type->format = (CLIPFORMAT)RegisterClipboardFormatW(L"PNG");
		if (type->kind == DRAG_HTML)
			type->format = (CLIPFORMAT)RegisterClipboardFormatW(L"HTML Format");
	}

	/* Kept, and the start posted to the frame. */
	posted = PostMessageW(window->frame, KEIUI_WIN32_DRAG_MESSAGE, 0, 0);
	if (!posted) {
		drag_free(drag);
		return ENOMEM;
	}

	/* Hands the drag to the window. */
	window->drag = drag;

	/* Succeeded: the drag starts at the next dispatch. */
	return 0;
}

/* Frees a drag and its types (NULL does nothing). */
static void
drag_free(
	struct keiui_win32_drag *drag)
{
	unsigned index;

	/* No drag. */
	if (drag == NULL)
		return;

	/* Each type, then the drag. */
	for (index = 0U; index < drag->count; index++) {
		free(drag->types[index].name);
		free(drag->types[index].data);
	}

	/* Releases the drag itself. */
	free(drag);
}

/* The kind of format a type is offered as (DRAG_*), 0 for none. */
static unsigned
drag_kind(
	const char *type)
{
	int same;

	/* The picture. */
	same = strcmp(type, "image/png");
	if (same == 0)
		return DRAG_PNG;

	/* The text types. */
	same = strcmp(type, "text/plain;charset=utf-8");
	if (same == 0)
		return DRAG_TEXT;
	same = strcmp(type, "text/plain");
	if (same == 0)
		return DRAG_TEXT;
	same = strcmp(type, "UTF8_STRING");
	if (same == 0)
		return DRAG_TEXT;

	/* The URIs and HTML. */
	same = strcmp(type, "text/uri-list");
	if (same == 0)
		return DRAG_URIS;
	same = strcmp(type, "text/html");
	if (same == 0)
		return DRAG_HTML;

	/* None. */
	return 0U;
}

/* The index of the drag's type of a format in global memory, or -1. */
static int
drag_find(
	const struct keiui_win32_drag *drag,
	const FORMATETC *format)
{
	unsigned index;

	/* Only global memory of the whole content. */
	if ((format->tymed & TYMED_HGLOBAL) == 0U)
		return -1;
	if (format->dwAspect != DVASPECT_CONTENT)
		return -1;

	/* The first type of the format that has its data. */
	for (index = 0U; index < drag->count; index++) {
		if (drag->types[index].format == format->cfFormat && drag->types[index].data != NULL)
			return (int)index;
	}

	/* None. */
	return -1;
}

/* A type's data as the format's global memory (the caller owns it), or NULL. */
static HGLOBAL
drag_global(
	const struct drag_type *type)
{
	const char *text;
	HGLOBAL memory;
	uint16_t *units;
	char *html;
	size_t length;
	size_t count;
	size_t size;
	size_t made;
	void *locked;
	int error;

	/* A picture: its bytes as they are. */
	if (type->kind == DRAG_PNG) {
		memory = GlobalAlloc(GMEM_MOVEABLE, type->length);
		if (memory == NULL)
			return NULL;
		locked = GlobalLock(memory);
		if (locked == NULL) {
			(void)GlobalFree(memory);
			return NULL;
		}

		/* Copies the picture's bytes into the memory. */
		memcpy(locked, type->data, type->length);
		(void)GlobalUnlock(memory);
		return memory;
	}

	/* HTML: the CF_HTML header and the fragment. */
	if (type->kind == DRAG_HTML) {
		size = type->length + 512U;
		html = malloc(size);
		if (html == NULL)
			return NULL;
		error = keiui_win32_cf_html_encode((const char *)type->data, type->length, NULL, html, size, &made);
		if (error != 0) {
			free(html);
			return NULL;
		}

		/* Allocates the memory for the HTML. */
		memory = GlobalAlloc(GMEM_MOVEABLE, made + 1U);
		locked = NULL;
		if (memory != NULL)
			locked = GlobalLock(memory);
		if (locked == NULL) {
			if (memory != NULL)
				(void)GlobalFree(memory);
			free(html);
			return NULL;
		}

		/* Copies the HTML and ends it. */
		memcpy(locked, html, made);
		((char *)locked)[made] = '\0';
		(void)GlobalUnlock(memory);
		free(html);
		return memory;
	}

	/* Text, or the URIs' first line: UTF-16 with its NUL. */
	text = (const char *)type->data;
	length = type->length;
	if (type->kind == DRAG_URIS) {
		for (count = 0U; count < length; count++) {
			if (text[count] == '\r' || text[count] == '\n')
				break;
		}

		/* Takes the text up to the line's end as the length. */
		length = count;
	}

	/* Allocates the memory for the text. */
	memory = GlobalAlloc(GMEM_MOVEABLE, (length + 1U) * sizeof(uint16_t));
	if (memory == NULL)
		return NULL;
	units = GlobalLock(memory);
	if (units == NULL) {
		(void)GlobalFree(memory);
		return NULL;
	}

	/* Converts the text to UTF-16. */
	count = 0U;
	error = keiui_win32_utf16_from_utf8(text, length, units, length + 1U, &count);
	if (error != 0)
		count = 0U;
	units[count] = 0U;
	(void)GlobalUnlock(memory);

	/* The text. */
	return memory;
}

/* IUnknown's QueryInterface of the data object. */
static HRESULT STDMETHODCALLTYPE
object_query(
	IDataObject *iface,
	REFIID id,
	void **out)
{
	BOOL ours;

	/* Tells whether the interface asked for is one this object is. */
	ours = IsEqualIID(id, &IID_IUnknown);
	if (!ours)
		ours = IsEqualIID(id, &IID_IDataObject);

	/* IUnknown and IDataObject are this object. */
	if (ours) {
		*out = iface;
		IDataObject_AddRef(iface);
		return S_OK;
	}

	/* Nothing else. */
	*out = NULL;
	return E_NOINTERFACE;
}

/* Adds a reference to the data object. */
static ULONG STDMETHODCALLTYPE
object_add(
	IDataObject *iface)
{
	struct drag_object *object;
	LONG count;

	/* One more. */
	object = (struct drag_object *)iface;
	count = InterlockedIncrement(&object->references);
	return (ULONG)count;
}

/* Lets a reference of the data object go; the last frees it and the drag. */
static ULONG STDMETHODCALLTYPE
object_release(
	IDataObject *iface)
{
	struct drag_object *object;
	LONG count;

	/* One less; at none, the object and its drag go. */
	object = (struct drag_object *)iface;
	count = InterlockedDecrement(&object->references);
	if (count == 0) {
		drag_free(object->drag);
		free(object);
	}

	/* Reports the references that are left. */
	return (ULONG)count;
}

/* Gives a format's data in new global memory the caller releases. */
static HRESULT STDMETHODCALLTYPE
object_get(
	IDataObject *iface,
	FORMATETC *format,
	STGMEDIUM *medium)
{
	struct drag_object *object;
	HGLOBAL memory;
	int index;

	/* The type of the format. */
	object = (struct drag_object *)iface;
	index = drag_find(object->drag, format);
	if (index < 0)
		return DV_E_FORMATETC;

	/* Its data. */
	memory = drag_global(&object->drag->types[index]);
	if (memory == NULL)
		return E_OUTOFMEMORY;
	medium->tymed = TYMED_HGLOBAL;
	medium->hGlobal = memory;
	medium->pUnkForRelease = NULL;
	return S_OK;
}

/* The data in the caller's memory: not given. */
static HRESULT STDMETHODCALLTYPE
object_get_here(
	IDataObject *iface,
	FORMATETC *format,
	STGMEDIUM *medium)
{
	(void)iface;
	(void)format;
	(void)medium;

	/* Not given. */
	return E_NOTIMPL;
}

/* Tells whether a format is given. */
static HRESULT STDMETHODCALLTYPE
object_has(
	IDataObject *iface,
	FORMATETC *format)
{
	struct drag_object *object;
	int index;

	/* The type of the format. */
	object = (struct drag_object *)iface;
	index = drag_find(object->drag, format);
	if (index < 0)
		return DV_E_FORMATETC;

	/* Given. */
	return S_OK;
}

/* The canonical format: the same one without a device. */
static HRESULT STDMETHODCALLTYPE
object_canonical(
	IDataObject *iface,
	FORMATETC *format,
	FORMATETC *out)
{
	(void)iface;

	/* The same format. */
	*out = *format;
	out->ptd = NULL;
	return DATA_S_SAMEFORMATETC;
}

/* Data set by the target (the shell's drag image): not kept. */
static HRESULT STDMETHODCALLTYPE
object_set(
	IDataObject *iface,
	FORMATETC *format,
	STGMEDIUM *medium,
	BOOL release)
{
	(void)iface;
	(void)format;
	(void)medium;
	(void)release;

	/* Not kept. */
	return E_NOTIMPL;
}

/* The formats given, by the shell's enumerator. */
static HRESULT STDMETHODCALLTYPE
object_formats(
	IDataObject *iface,
	DWORD direction,
	IEnumFORMATETC **formats)
{
	struct drag_object *object;
	FORMATETC given[DRAG_TYPES];
	unsigned index;
	unsigned count;
	HRESULT result;

	/* Only what may be got. */
	*formats = NULL;
	if (direction != DATADIR_GET)
		return E_NOTIMPL;

	/* Each type that has its data. */
	object = (struct drag_object *)iface;
	count = 0U;
	for (index = 0U; index < object->drag->count; index++) {
		if (object->drag->types[index].data == NULL)
			continue;
		given[count].cfFormat = object->drag->types[index].format;
		given[count].ptd = NULL;
		given[count].dwAspect = DVASPECT_CONTENT;
		given[count].lindex = -1;
		given[count].tymed = TYMED_HGLOBAL;
		count++;
	}

	/* The enumerator. */
	result = SHCreateStdEnumFmtEtc(count, given, formats);
	return result;
}

/* Advice: not given. */
static HRESULT STDMETHODCALLTYPE
object_advise(
	IDataObject *iface,
	FORMATETC *format,
	DWORD flags,
	IAdviseSink *sink,
	DWORD *connection)
{
	(void)iface;
	(void)format;
	(void)flags;
	(void)sink;
	(void)connection;

	/* Not given. */
	return OLE_E_ADVISENOTSUPPORTED;
}

/* Advice: not given. */
static HRESULT STDMETHODCALLTYPE
object_unadvise(
	IDataObject *iface,
	DWORD connection)
{
	(void)iface;
	(void)connection;

	/* Not given. */
	return OLE_E_ADVISENOTSUPPORTED;
}

/* Advice: not given. */
static HRESULT STDMETHODCALLTYPE
object_advices(
	IDataObject *iface,
	IEnumSTATDATA **advices)
{
	(void)iface;

	/* Not given. */
	*advices = NULL;
	return OLE_E_ADVISENOTSUPPORTED;
}

/* IUnknown's QueryInterface of the drop source. */
static HRESULT STDMETHODCALLTYPE
source_query(
	IDropSource *iface,
	REFIID id,
	void **out)
{
	BOOL ours;

	/* Tells whether the interface asked for is one this object is. */
	ours = IsEqualIID(id, &IID_IUnknown);
	if (!ours)
		ours = IsEqualIID(id, &IID_IDropSource);

	/* IUnknown and IDropSource are this object. */
	if (ours) {
		*out = iface;
		IDropSource_AddRef(iface);
		return S_OK;
	}

	/* Nothing else. */
	*out = NULL;
	return E_NOINTERFACE;
}

/* Adds a reference to the drop source. */
static ULONG STDMETHODCALLTYPE
source_add(
	IDropSource *iface)
{
	struct drag_source *source;
	LONG count;

	/* One more. */
	source = (struct drag_source *)iface;
	count = InterlockedIncrement(&source->references);
	return (ULONG)count;
}

/* Lets a reference of the drop source go; the last frees it. */
static ULONG STDMETHODCALLTYPE
source_release(
	IDropSource *iface)
{
	struct drag_source *source;
	LONG count;

	/* One less; at none, the source goes. */
	source = (struct drag_source *)iface;
	count = InterlockedDecrement(&source->references);
	if (count == 0)
		free(source);
	return (ULONG)count;
}

/* Whether the drag goes on: Escape cancels, the left button let go drops. */
static HRESULT STDMETHODCALLTYPE
source_continue(
	IDropSource *iface,
	BOOL escape,
	DWORD keys)
{
	(void)iface;

	/* Escape. */
	if (escape)
		return DRAGDROP_S_CANCEL;

	/* The button let go. */
	if ((keys & MK_LBUTTON) == 0U)
		return DRAGDROP_S_DROP;

	/* On. */
	return S_OK;
}

/* The system's cursors show the drag. */
static HRESULT STDMETHODCALLTYPE
source_feedback(
	IDropSource *iface,
	DWORD effect)
{
	(void)iface;
	(void)effect;

	/* The system's cursors. */
	return DRAGDROP_S_USEDEFAULTCURSORS;
}
