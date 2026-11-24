/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Virtual-file OLE drag-and-drop from the SFTP view tab to Explorer:
 * dedicated MTA COM thread hosting IDataObject, IDataObjectAsyncCapability,
 * and IStream, marshaled to the UI STA thread for DoDragDrop.
 */

#ifndef COBJMACROS
#define COBJMACROS
#endif

#include "sftp-drag-win32.h"
#include "apps/mikuterm-common/sftp-transfer.h"

#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <shldisp.h>

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Readahead buffer size per open virtual-file IStream (2 MiB). */
#define DRAG_STREAM_READAHEAD	(2U * 1024U * 1024U)

/* Offset between Windows FILETIME (1601-01-01) and Unix epoch (1970-01-01) in 100-ns units. */
#define DRAG_UNIX_EPOCH_100NS	116444736000000000ULL
#define DRAG_100NS_PER_SEC	10000000ULL

/* Number of clipboard formats advertised by the virtual-file data object. */
#define DRAG_FORMAT_COUNT	3U

/*
 * Lifetime and worker-access context shared between an SFTP view tab and
 * any active or async virtual-file drag operations it started.
 */
struct sftp_drag_context {
	CRITICAL_SECTION lock;
	LONG references;
	struct sftp_worker *worker;
	void (*notify)(void *data, const char *error_text);
	void *notify_data;
	int detached;
	int cancelled;
	size_t active_async;
};

/*
 * One flattened file or directory entry in the virtual-file descriptor.
 */
struct drag_desc_item {
	char remote_path[SSH_SFTP_PATH_MAX];
	wchar_t rel_wide[MAX_PATH];
	uint64_t size;
	uint32_t mtime;
	int is_dir;
};

/*
 * Pending directory expansion node used during iterative traversal.
 */
struct drag_dir_node {
	char remote_path[SSH_SFTP_PATH_MAX];
	char rel_utf8[SSH_SFTP_PATH_MAX];
};

/*
 * Virtual-file COM data object living in the dedicated MTA thread.
 */
struct drag_data_object {
	IDataObject iface;
	IDataObjectAsyncCapability async_iface;
	LONG references;
	CRITICAL_SECTION lock;
	struct sftp_drag_context *ctx;
	struct sftp_drag_item *top_items;
	size_t top_count;
	struct drag_desc_item *desc_items;
	size_t desc_count;
	size_t desc_cap;
	uint64_t total_bytes;
	int expanded;
	int expand_failed;
	BOOL async_mode;
	BOOL in_operation;
	int counted_active;
	int sync_limit_reported;
	CLIPFORMAT fmt_descriptor;
	CLIPFORMAT fmt_contents;
	CLIPFORMAT fmt_effect;
};

/*
 * COM IStream backed by an urgent sftp-worker stream with readahead.
 */
struct drag_stream {
	IStream iface;
	LONG references;
	struct drag_data_object *owner;
	struct sftp_drag_context *ctx;
	char remote_path[SSH_SFTP_PATH_MAX];
	uint64_t expected_size;
	uint64_t pos;
	int opened;
	uint64_t stream_id;
	unsigned char *buf;
	uint64_t buf_offset;
	size_t buf_len;
	size_t buf_cap;
};

/*
 * COM IEnumFORMATETC over the three virtual-file formats.
 */
struct drag_enum_formats {
	IEnumFORMATETC iface;
	LONG references;
	FORMATETC formats[DRAG_FORMAT_COUNT];
	ULONG index;
};

/*
 * COM IDropSource used on the UI STA thread during DoDragDrop.
 */
struct drag_drop_source {
	IDropSource iface;
	LONG references;
};

/*
 * Request passed to the dedicated MTA COM thread to create and marshal a
 * virtual-file data object.
 */
struct drag_com_request {
	struct sftp_drag_context *ctx;
	const struct sftp_drag_item *items;
	size_t count;
	IStream *marshal_stream;
	HRESULT hr;
	HANDLE done_event;
	struct drag_com_request *next;
};

/* Dedicated MTA COM thread state, initialized once per process. */
static INIT_ONCE drag_com_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION drag_com_lock;
static HANDLE drag_com_wake;
static HANDLE drag_com_thread_handle;
static struct drag_com_request *drag_com_head;
static struct drag_com_request *drag_com_tail;
static int drag_com_ready;
static DWORD drag_ole_tls = TLS_OUT_OF_INDEXES;

static BOOL CALLBACK drag_com_init_cb(PINIT_ONCE once, PVOID param, PVOID *context);
static DWORD WINAPI drag_com_thread_proc(LPVOID param);
static HRESULT drag_com_create_marshaled(struct sftp_drag_context *ctx, const struct sftp_drag_item *items, size_t count, IStream **stream_out);

static void drag_ctx_retain(struct sftp_drag_context *ctx);
static void drag_ctx_release(struct sftp_drag_context *ctx);
static void drag_ctx_notify(struct sftp_drag_context *ctx, const char *message);
static struct sftp_worker *drag_ctx_acquire_worker(struct sftp_drag_context *ctx);

static FILETIME drag_unix_to_filetime(uint32_t sec);
static int drag_utf8_to_wide(const char *utf8, wchar_t *wide, size_t wide_cap);
static int drag_join_rel_utf8(char *out, size_t out_cap, const char *parent, const char *name);
static int drag_append_desc_item(struct drag_data_object *obj, const char *remote_path, const char *rel_utf8, uint64_t size, uint32_t mtime, int is_dir);
static HRESULT drag_ensure_expanded(struct drag_data_object *obj);
static void drag_fill_formats(const struct drag_data_object *obj, FORMATETC formats[DRAG_FORMAT_COUNT]);
static int drag_match_format(const struct drag_data_object *obj, const FORMATETC *fmt);

static struct drag_data_object *drag_object_create(struct sftp_drag_context *ctx, const struct sftp_drag_item *items, size_t count);
static struct drag_data_object *drag_object_from_async(IDataObjectAsyncCapability *iface);
static HRESULT STDMETHODCALLTYPE dobj_query(IDataObject *iface, REFIID riid, void **out);
static ULONG STDMETHODCALLTYPE dobj_add(IDataObject *iface);
static ULONG STDMETHODCALLTYPE dobj_release(IDataObject *iface);
static HRESULT STDMETHODCALLTYPE dobj_get_data(IDataObject *iface, FORMATETC *fmt, STGMEDIUM *medium);
static HRESULT STDMETHODCALLTYPE dobj_get_data_here(IDataObject *iface, FORMATETC *fmt, STGMEDIUM *medium);
static HRESULT STDMETHODCALLTYPE dobj_query_get_data(IDataObject *iface, FORMATETC *fmt);
static HRESULT STDMETHODCALLTYPE dobj_canonical(IDataObject *iface, FORMATETC *fmt, FORMATETC *out);
static HRESULT STDMETHODCALLTYPE dobj_set_data(IDataObject *iface, FORMATETC *fmt, STGMEDIUM *medium, BOOL release);
static HRESULT STDMETHODCALLTYPE dobj_enum_formats(IDataObject *iface, DWORD dir, IEnumFORMATETC **out);
static HRESULT STDMETHODCALLTYPE dobj_dadvise(IDataObject *iface, FORMATETC *fmt, DWORD advf, IAdviseSink *sink, DWORD *conn);
static HRESULT STDMETHODCALLTYPE dobj_dunadvise(IDataObject *iface, DWORD conn);
static HRESULT STDMETHODCALLTYPE dobj_enum_dadvise(IDataObject *iface, IEnumSTATDATA **out);

static HRESULT STDMETHODCALLTYPE async_query(IDataObjectAsyncCapability *iface, REFIID riid, void **out);
static ULONG STDMETHODCALLTYPE async_add(IDataObjectAsyncCapability *iface);
static ULONG STDMETHODCALLTYPE async_release(IDataObjectAsyncCapability *iface);
static HRESULT STDMETHODCALLTYPE async_set_mode(IDataObjectAsyncCapability *iface, BOOL do_async);
static HRESULT STDMETHODCALLTYPE async_get_mode(IDataObjectAsyncCapability *iface, BOOL *is_async);
static HRESULT STDMETHODCALLTYPE async_start_op(IDataObjectAsyncCapability *iface, IBindCtx *reserved);
static HRESULT STDMETHODCALLTYPE async_in_op(IDataObjectAsyncCapability *iface, BOOL *in_op);
static HRESULT STDMETHODCALLTYPE async_end_op(IDataObjectAsyncCapability *iface, HRESULT result, IBindCtx *reserved, DWORD effects);

static struct drag_stream *drag_stream_create(struct drag_data_object *owner, const struct drag_desc_item *item);
static HRESULT drag_stream_fill_buffer(struct drag_stream *s, uint64_t offset);
static HRESULT STDMETHODCALLTYPE stream_query(IStream *iface, REFIID riid, void **out);
static ULONG STDMETHODCALLTYPE stream_add(IStream *iface);
static ULONG STDMETHODCALLTYPE stream_release(IStream *iface);
static HRESULT STDMETHODCALLTYPE stream_read(IStream *iface, void *pv, ULONG cb, ULONG *pcb_read);
static HRESULT STDMETHODCALLTYPE stream_write(IStream *iface, const void *pv, ULONG cb, ULONG *pcb_written);
static HRESULT STDMETHODCALLTYPE stream_seek(IStream *iface, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *new_pos);
static HRESULT STDMETHODCALLTYPE stream_set_size(IStream *iface, ULARGE_INTEGER new_size);
static HRESULT STDMETHODCALLTYPE stream_copy_to(IStream *iface, IStream *dest, ULARGE_INTEGER cb, ULARGE_INTEGER *read_out, ULARGE_INTEGER *written_out);
static HRESULT STDMETHODCALLTYPE stream_commit(IStream *iface, DWORD flags);
static HRESULT STDMETHODCALLTYPE stream_revert(IStream *iface);
static HRESULT STDMETHODCALLTYPE stream_lock_region(IStream *iface, ULARGE_INTEGER offset, ULARGE_INTEGER cb, DWORD type);
static HRESULT STDMETHODCALLTYPE stream_unlock_region(IStream *iface, ULARGE_INTEGER offset, ULARGE_INTEGER cb, DWORD type);
static HRESULT STDMETHODCALLTYPE stream_stat(IStream *iface, STATSTG *statstg, DWORD flag);
static HRESULT STDMETHODCALLTYPE stream_clone(IStream *iface, IStream **out);

static struct drag_enum_formats *drag_enum_create(const FORMATETC formats[DRAG_FORMAT_COUNT], ULONG index);
static HRESULT STDMETHODCALLTYPE enum_query(IEnumFORMATETC *iface, REFIID riid, void **out);
static ULONG STDMETHODCALLTYPE enum_add(IEnumFORMATETC *iface);
static ULONG STDMETHODCALLTYPE enum_release(IEnumFORMATETC *iface);
static HRESULT STDMETHODCALLTYPE enum_next(IEnumFORMATETC *iface, ULONG celt, FORMATETC *elts, ULONG *fetched);
static HRESULT STDMETHODCALLTYPE enum_skip(IEnumFORMATETC *iface, ULONG celt);
static HRESULT STDMETHODCALLTYPE enum_reset(IEnumFORMATETC *iface);
static HRESULT STDMETHODCALLTYPE enum_clone(IEnumFORMATETC *iface, IEnumFORMATETC **out);

static HRESULT STDMETHODCALLTYPE dsrc_query(IDropSource *iface, REFIID riid, void **out);
static ULONG STDMETHODCALLTYPE dsrc_add(IDropSource *iface);
static ULONG STDMETHODCALLTYPE dsrc_release(IDropSource *iface);
static HRESULT STDMETHODCALLTYPE dsrc_continue(IDropSource *iface, BOOL escape, DWORD keys);
static HRESULT STDMETHODCALLTYPE dsrc_feedback(IDropSource *iface, DWORD effect);

static IDataObjectVtbl drag_object_vtbl = {
	dobj_query,
	dobj_add,
	dobj_release,
	dobj_get_data,
	dobj_get_data_here,
	dobj_query_get_data,
	dobj_canonical,
	dobj_set_data,
	dobj_enum_formats,
	dobj_dadvise,
	dobj_dunadvise,
	dobj_enum_dadvise
};

static IDataObjectAsyncCapabilityVtbl drag_async_vtbl = {
	async_query,
	async_add,
	async_release,
	async_set_mode,
	async_get_mode,
	async_start_op,
	async_in_op,
	async_end_op
};

static IStreamVtbl drag_stream_vtbl = {
	stream_query,
	stream_add,
	stream_release,
	stream_read,
	stream_write,
	stream_seek,
	stream_set_size,
	stream_copy_to,
	stream_commit,
	stream_revert,
	stream_lock_region,
	stream_unlock_region,
	stream_stat,
	stream_clone
};

static IEnumFORMATETCVtbl drag_enum_vtbl = {
	enum_query,
	enum_add,
	enum_release,
	enum_next,
	enum_skip,
	enum_reset,
	enum_clone
};

static IDropSourceVtbl drag_source_vtbl = {
	dsrc_query,
	dsrc_add,
	dsrc_release,
	dsrc_continue,
	dsrc_feedback
};

/*
 * Creates a drag context bound to worker for one SFTP view tab.
 */
struct sftp_drag_context *
sftp_drag_context_create(
	struct sftp_worker *worker,
	void (*notify)(void *data, const char *error_text),
	void *notify_data)
{
	struct sftp_drag_context *ctx;

	ctx = calloc(1U, sizeof(*ctx));
	if (ctx == NULL)
		return NULL;

	InitializeCriticalSection(&ctx->lock);
	ctx->references = 1;
	ctx->worker = worker;
	ctx->notify = notify;
	ctx->notify_data = notify_data;
	return ctx;
}

/*
 * Detaches the drag context when its view tab closes, cancelling any
 * in-flight virtual-file reads before the worker is stopped.
 */
void
sftp_drag_context_detach(
	struct sftp_drag_context *ctx)
{
	if (ctx == NULL)
		return;

	EnterCriticalSection(&ctx->lock);
	ctx->detached = 1;
	ctx->cancelled = 1;
	ctx->worker = NULL;
	ctx->notify = NULL;
	ctx->notify_data = NULL;
	LeaveCriticalSection(&ctx->lock);

	drag_ctx_release(ctx);
}

/*
 * Returns the number of active asynchronous drag operations still running
 * after DoDragDrop returned.
 */
size_t
sftp_drag_context_active_count(
	struct sftp_drag_context *ctx)
{
	size_t count;

	if (ctx == NULL)
		return 0U;

	EnterCriticalSection(&ctx->lock);
	count = ctx->active_async;
	LeaveCriticalSection(&ctx->lock);
	return count;
}

/*
 * Marks all active virtual-file drags as cancelled.
 */
void
sftp_drag_context_cancel_all(
	struct sftp_drag_context *ctx)
{
	if (ctx == NULL)
		return;

	EnterCriticalSection(&ctx->lock);
	ctx->cancelled = 1;
	LeaveCriticalSection(&ctx->lock);
}

/*
 * Creates a virtual-file IDataObject on the dedicated MTA COM thread and
 * marshals its proxy to the caller's apartment.
 */
int
sftp_drag_create_object(
	struct sftp_drag_context *ctx,
	const struct sftp_drag_item *items,
	size_t count,
	struct IDataObject **object_out)
{
	IStream *marshal_stream;
	IDataObject *proxy;
	HRESULT hr;

	if (ctx == NULL || items == NULL || count == 0U || object_out == NULL)
		return EINVAL;

	*object_out = NULL;
	if (!InitOnceExecuteOnce(&drag_com_once, drag_com_init_cb, NULL, NULL) || !drag_com_ready)
		return EIO;

	if (drag_ole_tls != TLS_OUT_OF_INDEXES && TlsGetValue(drag_ole_tls) == NULL) {
		hr = OleInitialize(NULL);
		if (SUCCEEDED(hr))
			(void)TlsSetValue(drag_ole_tls, (LPVOID)(uintptr_t)1);
	}

	EnterCriticalSection(&ctx->lock);
	ctx->cancelled = 0;
	LeaveCriticalSection(&ctx->lock);

	marshal_stream = NULL;
	hr = drag_com_create_marshaled(ctx, items, count, &marshal_stream);
	if (FAILED(hr) || marshal_stream == NULL)
		return EIO;

	proxy = NULL;
	hr = CoGetInterfaceAndReleaseStream(marshal_stream, &IID_IDataObject, (void **)&proxy);
	if (FAILED(hr) || proxy == NULL)
		return EIO;

	*object_out = proxy;
	return 0;
}

/*
 * Runs an OLE virtual-file drag-and-drop operation on the UI STA thread
 * for the selected remote items.
 */
int
sftp_drag_run(
	struct sftp_drag_context *ctx,
	const struct sftp_drag_item *items,
	size_t count)
{
	IDataObject *proxy;
	IDataObjectAsyncCapability *async_cap;
	struct drag_drop_source *source;
	DWORD effect;
	HRESULT hr;
	BOOL in_op;
	int error;

	proxy = NULL;
	error = sftp_drag_create_object(ctx, items, count, &proxy);
	if (error != 0)
		return error;

	source = calloc(1U, sizeof(*source));
	if (source == NULL) {
		IDataObject_Release(proxy);
		return ENOMEM;
	}
	source->iface.lpVtbl = &drag_source_vtbl;
	source->references = 1;

	effect = DROPEFFECT_NONE;
	hr = DoDragDrop(proxy, &source->iface, DROPEFFECT_COPY, &effect);

	in_op = FALSE;
	async_cap = NULL;
	if (SUCCEEDED(IDataObject_QueryInterface(proxy, &IID_IDataObjectAsyncCapability, (void **)&async_cap)) &&
	    async_cap != NULL) {
		(void)IDataObjectAsyncCapability_InOperation(async_cap, &in_op);
		IDataObjectAsyncCapability_Release(async_cap);
	}
	(void)in_op;

	IDataObject_Release(proxy);
	IDropSource_Release(&source->iface);

	if (hr == DRAGDROP_S_DROP || hr == DRAGDROP_S_CANCEL)
		return 0;
	return EIO;
}

/* One-time initialization of the dedicated MTA COM thread. */
static BOOL CALLBACK
drag_com_init_cb(
	PINIT_ONCE once,
	PVOID param,
	PVOID *context)
{
	DWORD tid;

	(void)once;
	(void)param;
	(void)context;

	InitializeCriticalSection(&drag_com_lock);
	drag_ole_tls = TlsAlloc();
	drag_com_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (drag_com_wake == NULL)
		return FALSE;

	drag_com_thread_handle = CreateThread(NULL, 0U, drag_com_thread_proc, NULL, 0U, &tid);
	if (drag_com_thread_handle == NULL) {
		CloseHandle(drag_com_wake);
		drag_com_wake = NULL;
		return FALSE;
	}
	drag_com_ready = 1;
	return TRUE;
}

/* Entry point of the dedicated MTA COM thread. */
static DWORD WINAPI
drag_com_thread_proc(
	LPVOID param)
{
	struct drag_com_request *req;
	struct drag_data_object *obj;
	HRESULT hr;

	(void)param;

	hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	if (FAILED(hr))
		return 1U;

	for (;;) {
		WaitForSingleObject(drag_com_wake, INFINITE);

		for (;;) {
			EnterCriticalSection(&drag_com_lock);
			req = drag_com_head;
			if (req != NULL) {
				drag_com_head = req->next;
				if (drag_com_head == NULL)
					drag_com_tail = NULL;
			}
			LeaveCriticalSection(&drag_com_lock);

			if (req == NULL)
				break;

			obj = drag_object_create(req->ctx, req->items, req->count);
			if (obj == NULL) {
				req->hr = E_OUTOFMEMORY;
				req->marshal_stream = NULL;
			} else {
				req->marshal_stream = NULL;
				req->hr = CoMarshalInterThreadInterfaceInStream(&IID_IDataObject,
				    (IUnknown *)&obj->iface, &req->marshal_stream);
				IDataObject_Release(&obj->iface);
			}
			SetEvent(req->done_event);
		}
	}
}

/* Creates a virtual-file IDataObject on the MTA thread and marshals it to the caller's thread. */
static HRESULT
drag_com_create_marshaled(
	struct sftp_drag_context *ctx,
	const struct sftp_drag_item *items,
	size_t count,
	IStream **stream_out)
{
	struct drag_com_request req;

	*stream_out = NULL;
	if (!InitOnceExecuteOnce(&drag_com_once, drag_com_init_cb, NULL, NULL) || !drag_com_ready)
		return E_FAIL;

	memset(&req, 0, sizeof(req));
	req.ctx = ctx;
	req.items = items;
	req.count = count;
	req.hr = E_FAIL;
	req.done_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (req.done_event == NULL)
		return E_OUTOFMEMORY;

	EnterCriticalSection(&drag_com_lock);
	if (drag_com_tail != NULL)
		drag_com_tail->next = &req;
	else
		drag_com_head = &req;
	drag_com_tail = &req;
	LeaveCriticalSection(&drag_com_lock);

	SetEvent(drag_com_wake);
	WaitForSingleObject(req.done_event, INFINITE);
	CloseHandle(req.done_event);

	*stream_out = req.marshal_stream;
	return req.hr;
}

/* Increments the drag context reference count. */
static void
drag_ctx_retain(
	struct sftp_drag_context *ctx)
{
	if (ctx != NULL)
		InterlockedIncrement(&ctx->references);
}

/* Decrements the drag context reference count and frees it at zero. */
static void
drag_ctx_release(
	struct sftp_drag_context *ctx)
{
	if (ctx == NULL)
		return;

	if (InterlockedDecrement(&ctx->references) == 0) {
		DeleteCriticalSection(&ctx->lock);
		free(ctx);
	}
}

/* Reports an error message to the owning view if still attached. */
static void
drag_ctx_notify(
	struct sftp_drag_context *ctx,
	const char *message)
{
	void (*cb)(void *data, const char *error_text);
	void *cb_data;

	if (ctx == NULL)
		return;

	EnterCriticalSection(&ctx->lock);
	cb = ctx->detached ? NULL : ctx->notify;
	cb_data = ctx->notify_data;
	LeaveCriticalSection(&ctx->lock);

	if (cb != NULL)
		cb(cb_data, message);
}

/* Returns the worker pointer if the context is active and not cancelled. */
static struct sftp_worker *
drag_ctx_acquire_worker(
	struct sftp_drag_context *ctx)
{
	struct sftp_worker *worker;

	if (ctx == NULL)
		return NULL;

	EnterCriticalSection(&ctx->lock);
	if (ctx->detached || ctx->cancelled)
		worker = NULL;
	else
		worker = ctx->worker;
	LeaveCriticalSection(&ctx->lock);
	return worker;
}

/* Converts Unix seconds to Windows FILETIME. */
static FILETIME
drag_unix_to_filetime(
	uint32_t sec)
{
	ULARGE_INTEGER uli;
	FILETIME ft;

	uli.QuadPart = DRAG_UNIX_EPOCH_100NS + (uint64_t)sec * DRAG_100NS_PER_SEC;
	ft.dwLowDateTime = uli.LowPart;
	ft.dwHighDateTime = uli.HighPart;
	return ft;
}

/* Converts UTF-8 to UTF-16, failing if truncated or invalid. */
static int
drag_utf8_to_wide(
	const char *utf8,
	wchar_t *wide,
	size_t wide_cap)
{
	int needed;

	if (utf8 == NULL || wide == NULL || wide_cap == 0U)
		return EINVAL;

	needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, wide, (int)wide_cap);
	if (needed <= 0)
		return ENAMETOOLONG;
	return 0;
}

/* Joins parent and child UTF-8 relative paths with a Windows backslash. */
static int
drag_join_rel_utf8(
	char *out,
	size_t out_cap,
	const char *parent,
	const char *name)
{
	int written;

	if (parent == NULL || parent[0] == '\0')
		written = snprintf(out, out_cap, "%s", name);
	else
		written = snprintf(out, out_cap, "%s\\%s", parent, name);
	if (written < 0 || (size_t)written >= out_cap)
		return ENAMETOOLONG;
	return 0;
}

/* Appends one validated item to the data object's descriptor list. */
static int
drag_append_desc_item(
	struct drag_data_object *obj,
	const char *remote_path,
	const char *rel_utf8,
	uint64_t size,
	uint32_t mtime,
	int is_dir)
{
	wchar_t rel_wide[MAX_PATH];
	struct drag_desc_item *grown;
	size_t new_cap;

	if (obj->desc_count >= SFTP_DRAG_ITEMS_MAX)
		return E2BIG;

	if (drag_utf8_to_wide(rel_utf8, rel_wide, MAX_PATH) != 0)
		return ENAMETOOLONG;

	if (obj->desc_count == obj->desc_cap) {
		new_cap = obj->desc_cap == 0U ? 32U : obj->desc_cap * 2U;
		if (new_cap > SFTP_DRAG_ITEMS_MAX)
			new_cap = SFTP_DRAG_ITEMS_MAX;
		grown = realloc(obj->desc_items, new_cap * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		obj->desc_items = grown;
		obj->desc_cap = new_cap;
	}

	(void)snprintf(obj->desc_items[obj->desc_count].remote_path,
	    sizeof(obj->desc_items[obj->desc_count].remote_path), "%s", remote_path);
	wcscpy(obj->desc_items[obj->desc_count].rel_wide, rel_wide);
	obj->desc_items[obj->desc_count].size = is_dir ? 0ULL : size;
	obj->desc_items[obj->desc_count].mtime = mtime;
	obj->desc_items[obj->desc_count].is_dir = is_dir ? 1 : 0;
	obj->desc_count++;
	if (!is_dir)
		obj->total_bytes += size;
	return 0;
}

/* Expands top-level selected items (recursing into directories) once on demand. */
static HRESULT
drag_ensure_expanded(
	struct drag_data_object *obj)
{
	struct sftp_worker *worker;
	struct drag_dir_node *stack;
	size_t stack_count;
	size_t stack_cap;
	size_t i;
	int err;

	EnterCriticalSection(&obj->lock);
	if (obj->expanded) {
		HRESULT hr = obj->expand_failed ? E_FAIL : S_OK;
		LeaveCriticalSection(&obj->lock);
		return hr;
	}

	worker = drag_ctx_acquire_worker(obj->ctx);
	if (worker == NULL) {
		obj->expanded = 1;
		obj->expand_failed = 1;
		LeaveCriticalSection(&obj->lock);
		return E_FAIL;
	}

	stack = NULL;
	stack_count = 0U;
	stack_cap = 0U;

	for (i = 0U; i < obj->top_count; i++) {
		const struct sftp_drag_item *top = &obj->top_items[i];
		const char *why = NULL;

		if (!sftp_transfer_valid_download_name(top->name, strlen(top->name), &why))
			continue;

		err = drag_append_desc_item(obj, top->remote_path, top->name, top->size, top->mtime, top->is_dir);
		if (err == E2BIG) {
			drag_ctx_notify(obj->ctx, "Too many items to drag (limit 10,000); use Download instead.");
			free(stack);
			obj->expanded = 1;
			obj->expand_failed = 1;
			LeaveCriticalSection(&obj->lock);
			return E_FAIL;
		}
		if (err == ENAMETOOLONG)
			continue;
		if (err != 0) {
			free(stack);
			obj->expanded = 1;
			obj->expand_failed = 1;
			LeaveCriticalSection(&obj->lock);
			return E_OUTOFMEMORY;
		}

		if (top->is_dir) {
			if (stack_count == stack_cap) {
				struct drag_dir_node *grown;
				size_t nc = stack_cap == 0U ? 16U : stack_cap * 2U;

				grown = realloc(stack, nc * sizeof(*grown));
				if (grown == NULL) {
					free(stack);
					obj->expanded = 1;
					obj->expand_failed = 1;
					LeaveCriticalSection(&obj->lock);
					return E_OUTOFMEMORY;
				}
				stack = grown;
				stack_cap = nc;
			}
			(void)snprintf(stack[stack_count].remote_path, sizeof(stack[stack_count].remote_path),
			    "%s", top->remote_path);
			(void)snprintf(stack[stack_count].rel_utf8, sizeof(stack[stack_count].rel_utf8),
			    "%s", top->name);
			stack_count++;
		}
	}

	while (stack_count > 0U) {
		struct drag_dir_node cur;
		struct sftp_dir_listing *listing;
		size_t j;

		stack_count--;
		cur = stack[stack_count];

		worker = drag_ctx_acquire_worker(obj->ctx);
		if (worker == NULL) {
			free(stack);
			obj->expanded = 1;
			obj->expand_failed = 1;
			LeaveCriticalSection(&obj->lock);
			return E_FAIL;
		}

		listing = NULL;
		err = sftp_worker_list_dir(worker, cur.remote_path, 0, &listing);
		if (err != 0 || listing == NULL || listing->error != 0) {
			if (listing != NULL)
				sftp_dir_listing_free(listing);
			free(stack);
			obj->expanded = 1;
			obj->expand_failed = 1;
			LeaveCriticalSection(&obj->lock);
			return E_FAIL;
		}

		for (j = 0U; j < listing->count; j++) {
			const struct sftp_dir_entry *ent = &listing->entries[j];
			char child_remote[SSH_SFTP_PATH_MAX];
			char child_rel[SSH_SFTP_PATH_MAX];
			const char *why = NULL;
			int is_dir;

			if (strcmp(ent->name, ".") == 0 || strcmp(ent->name, "..") == 0)
				continue;
			if (!sftp_transfer_valid_download_name(ent->name, strlen(ent->name), &why))
				continue;
			if (ent->is_link && ent->link_is_dir)
				continue;

			if (sftp_transfer_join_remote(child_remote, sizeof(child_remote), cur.remote_path, ent->name) != 0)
				continue;
			if (drag_join_rel_utf8(child_rel, sizeof(child_rel), cur.rel_utf8, ent->name) != 0)
				continue;

			is_dir = ent->is_dir ? 1 : 0;
			err = drag_append_desc_item(obj, child_remote, child_rel, ent->size, ent->mtime, is_dir);
			if (err == E2BIG) {
				sftp_dir_listing_free(listing);
				drag_ctx_notify(obj->ctx, "Too many items to drag (limit 10,000); use Download instead.");
				free(stack);
				obj->expanded = 1;
				obj->expand_failed = 1;
				LeaveCriticalSection(&obj->lock);
				return E_FAIL;
			}
			if (err == ENAMETOOLONG)
				continue;
			if (err != 0) {
				sftp_dir_listing_free(listing);
				free(stack);
				obj->expanded = 1;
				obj->expand_failed = 1;
				LeaveCriticalSection(&obj->lock);
				return E_OUTOFMEMORY;
			}

			if (is_dir) {
				if (stack_count == stack_cap) {
					struct drag_dir_node *grown;
					size_t nc = stack_cap == 0U ? 16U : stack_cap * 2U;

					grown = realloc(stack, nc * sizeof(*grown));
					if (grown == NULL) {
						sftp_dir_listing_free(listing);
						free(stack);
						obj->expanded = 1;
						obj->expand_failed = 1;
						LeaveCriticalSection(&obj->lock);
						return E_OUTOFMEMORY;
					}
					stack = grown;
					stack_cap = nc;
				}
				(void)snprintf(stack[stack_count].remote_path, sizeof(stack[stack_count].remote_path),
				    "%s", child_remote);
				(void)snprintf(stack[stack_count].rel_utf8, sizeof(stack[stack_count].rel_utf8),
				    "%s", child_rel);
				stack_count++;
			}
		}
		sftp_dir_listing_free(listing);
	}

	free(stack);
	obj->expanded = 1;
	obj->expand_failed = 0;
	LeaveCriticalSection(&obj->lock);
	return S_OK;
}

/* Populates the three FORMATETC entries offered by the data object. */
static void
drag_fill_formats(
	const struct drag_data_object *obj,
	FORMATETC formats[DRAG_FORMAT_COUNT])
{
	formats[0].cfFormat = obj->fmt_descriptor;
	formats[0].ptd = NULL;
	formats[0].dwAspect = DVASPECT_CONTENT;
	formats[0].lindex = -1;
	formats[0].tymed = TYMED_HGLOBAL;

	formats[1].cfFormat = obj->fmt_contents;
	formats[1].ptd = NULL;
	formats[1].dwAspect = DVASPECT_CONTENT;
	formats[1].lindex = -1;
	formats[1].tymed = TYMED_ISTREAM;

	formats[2].cfFormat = obj->fmt_effect;
	formats[2].ptd = NULL;
	formats[2].dwAspect = DVASPECT_CONTENT;
	formats[2].lindex = -1;
	formats[2].tymed = TYMED_HGLOBAL;
}

/* Matches fmt against the supported formats, returning 0..2 or -1. */
static int
drag_match_format(
	const struct drag_data_object *obj,
	const FORMATETC *fmt)
{
	if (fmt == NULL || fmt->dwAspect != DVASPECT_CONTENT)
		return -1;

	if (fmt->cfFormat == obj->fmt_descriptor && (fmt->tymed & TYMED_HGLOBAL) != 0U &&
	    (fmt->lindex == -1 || fmt->lindex == 0))
		return 0;

	if (fmt->cfFormat == obj->fmt_contents && (fmt->tymed & TYMED_ISTREAM) != 0U)
		return 1;

	if (fmt->cfFormat == obj->fmt_effect && (fmt->tymed & TYMED_HGLOBAL) != 0U &&
	    (fmt->lindex == -1 || fmt->lindex == 0))
		return 2;

	return -1;
}

/* Allocates and initializes a virtual-file COM data object. */
static struct drag_data_object *
drag_object_create(
	struct sftp_drag_context *ctx,
	const struct sftp_drag_item *items,
	size_t count)
{
	struct drag_data_object *obj;

	obj = calloc(1U, sizeof(*obj));
	if (obj == NULL)
		return NULL;

	obj->top_items = calloc(count, sizeof(*obj->top_items));
	if (obj->top_items == NULL) {
		free(obj);
		return NULL;
	}
	memcpy(obj->top_items, items, count * sizeof(*obj->top_items));
	obj->top_count = count;

	obj->iface.lpVtbl = &drag_object_vtbl;
	obj->async_iface.lpVtbl = &drag_async_vtbl;
	obj->references = 1;
	InitializeCriticalSection(&obj->lock);
	obj->ctx = ctx;
	drag_ctx_retain(ctx);
	obj->async_mode = TRUE;
	obj->fmt_descriptor = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
	obj->fmt_contents = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILECONTENTS);
	obj->fmt_effect = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
	return obj;
}

/* Recovers the enclosing drag_data_object from its IDataObjectAsyncCapability sub-interface. */
static struct drag_data_object *
drag_object_from_async(
	IDataObjectAsyncCapability *iface)
{
	return (struct drag_data_object *)((char *)iface - offsetof(struct drag_data_object, async_iface));
}

/* IDataObject::QueryInterface. */
static HRESULT STDMETHODCALLTYPE
dobj_query(
	IDataObject *iface,
	REFIID riid,
	void **out)
{
	struct drag_data_object *obj = (struct drag_data_object *)iface;

	if (out == NULL)
		return E_POINTER;

	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDataObject)) {
		*out = &obj->iface;
		dobj_add(&obj->iface);
		return S_OK;
	}
	if (IsEqualIID(riid, &IID_IDataObjectAsyncCapability)) {
		*out = &obj->async_iface;
		dobj_add(&obj->iface);
		return S_OK;
	}

	*out = NULL;
	return E_NOINTERFACE;
}

/* IDataObject::AddRef. */
static ULONG STDMETHODCALLTYPE
dobj_add(
	IDataObject *iface)
{
	struct drag_data_object *obj = (struct drag_data_object *)iface;

	return (ULONG)InterlockedIncrement(&obj->references);
}

/* IDataObject::Release. */
static ULONG STDMETHODCALLTYPE
dobj_release(
	IDataObject *iface)
{
	struct drag_data_object *obj = (struct drag_data_object *)iface;
	LONG count;

	count = InterlockedDecrement(&obj->references);
	if (count == 0) {
		if (obj->counted_active && obj->ctx != NULL) {
			EnterCriticalSection(&obj->ctx->lock);
			if (obj->ctx->active_async > 0U)
				obj->ctx->active_async--;
			LeaveCriticalSection(&obj->ctx->lock);
		}
		drag_ctx_release(obj->ctx);
		DeleteCriticalSection(&obj->lock);
		free(obj->top_items);
		free(obj->desc_items);
		free(obj);
	}
	return (ULONG)count;
}

/* IDataObject::GetData. */
static HRESULT STDMETHODCALLTYPE
dobj_get_data(
	IDataObject *iface,
	FORMATETC *fmt,
	STGMEDIUM *medium)
{
	struct drag_data_object *obj = (struct drag_data_object *)iface;
	int kind;
	HRESULT hr;

	if (fmt == NULL || medium == NULL)
		return E_INVALIDARG;
	memset(medium, 0, sizeof(*medium));

	kind = drag_match_format(obj, fmt);
	if (kind < 0)
		return DV_E_FORMATETC;

	if (kind == 2) {
		HGLOBAL mem;
		DWORD *effect;

		mem = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
		if (mem == NULL)
			return E_OUTOFMEMORY;
		effect = GlobalLock(mem);
		if (effect == NULL) {
			GlobalFree(mem);
			return E_OUTOFMEMORY;
		}
		*effect = DROPEFFECT_COPY;
		GlobalUnlock(mem);
		medium->tymed = TYMED_HGLOBAL;
		medium->hGlobal = mem;
		medium->pUnkForRelease = NULL;
		return S_OK;
	}

	hr = drag_ensure_expanded(obj);
	if (FAILED(hr))
		return hr;

	if (kind == 0) {
		size_t bytes;
		HGLOBAL mem;
		FILEGROUPDESCRIPTORW *fgd;
		size_t i;

		if (obj->desc_count == 0U)
			return E_FAIL;

		bytes = offsetof(FILEGROUPDESCRIPTORW, fgd) + obj->desc_count * sizeof(FILEDESCRIPTORW);
		mem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
		if (mem == NULL)
			return E_OUTOFMEMORY;

		fgd = GlobalLock(mem);
		if (fgd == NULL) {
			GlobalFree(mem);
			return E_OUTOFMEMORY;
		}
		fgd->cItems = (UINT)obj->desc_count;
		for (i = 0U; i < obj->desc_count; i++) {
			const struct drag_desc_item *item = &obj->desc_items[i];
			FILEDESCRIPTORW *fd = &fgd->fgd[i];

			fd->dwFlags = FD_ATTRIBUTES | FD_PROGRESSUI | FD_UNICODE;
			if (item->is_dir) {
				fd->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
			} else {
				fd->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
				fd->dwFlags |= FD_FILESIZE;
				fd->nFileSizeHigh = (DWORD)(item->size >> 32);
				fd->nFileSizeLow = (DWORD)(item->size & 0xffffffffU);
			}
			if (item->mtime != 0U) {
				fd->dwFlags |= FD_WRITESTIME;
				fd->ftLastWriteTime = drag_unix_to_filetime(item->mtime);
			}
			wcsncpy(fd->cFileName, item->rel_wide, MAX_PATH - 1U);
			fd->cFileName[MAX_PATH - 1U] = L'\0';
		}
		GlobalUnlock(mem);

		medium->tymed = TYMED_HGLOBAL;
		medium->hGlobal = mem;
		medium->pUnkForRelease = NULL;
		return S_OK;
	}

	/* CFSTR_FILECONTENTS (TYMED_ISTREAM). */
	if (fmt->lindex < 0 || (size_t)fmt->lindex >= obj->desc_count)
		return DV_E_LINDEX;
	if (obj->desc_items[fmt->lindex].is_dir)
		return DV_E_LINDEX;

	EnterCriticalSection(&obj->lock);
	if (!obj->in_operation && obj->total_bytes > SFTP_DRAG_SYNC_LIMIT_BYTES) {
		int should_notify = !obj->sync_limit_reported;

		obj->sync_limit_reported = 1;
		LeaveCriticalSection(&obj->lock);
		if (should_notify)
			drag_ctx_notify(obj->ctx, "This application cannot take a large drag; use Download instead.");
		return E_FAIL;
	}
	LeaveCriticalSection(&obj->lock);

	{
		struct drag_stream *stream;

		stream = drag_stream_create(obj, &obj->desc_items[fmt->lindex]);
		if (stream == NULL)
			return E_OUTOFMEMORY;
		medium->tymed = TYMED_ISTREAM;
		medium->pstm = &stream->iface;
		medium->pUnkForRelease = NULL;
		return S_OK;
	}
}

/* IDataObject::GetDataHere (not implemented). */
static HRESULT STDMETHODCALLTYPE
dobj_get_data_here(
	IDataObject *iface,
	FORMATETC *fmt,
	STGMEDIUM *medium)
{
	(void)iface;
	(void)fmt;
	(void)medium;
	return E_NOTIMPL;
}

/* IDataObject::QueryGetData. */
static HRESULT STDMETHODCALLTYPE
dobj_query_get_data(
	IDataObject *iface,
	FORMATETC *fmt)
{
	struct drag_data_object *obj = (struct drag_data_object *)iface;

	return drag_match_format(obj, fmt) >= 0 ? S_OK : DV_E_FORMATETC;
}

/* IDataObject::GetCanonicalFormatEtc. */
static HRESULT STDMETHODCALLTYPE
dobj_canonical(
	IDataObject *iface,
	FORMATETC *fmt,
	FORMATETC *out)
{
	(void)iface;
	if (fmt == NULL || out == NULL)
		return E_INVALIDARG;
	*out = *fmt;
	out->ptd = NULL;
	return DATA_S_SAMEFORMATETC;
}

/* IDataObject::SetData (not stored). */
static HRESULT STDMETHODCALLTYPE
dobj_set_data(
	IDataObject *iface,
	FORMATETC *fmt,
	STGMEDIUM *medium,
	BOOL release)
{
	(void)iface;
	(void)fmt;
	(void)medium;
	(void)release;
	return E_NOTIMPL;
}

/* IDataObject::EnumFormatEtc. */
static HRESULT STDMETHODCALLTYPE
dobj_enum_formats(
	IDataObject *iface,
	DWORD dir,
	IEnumFORMATETC **out)
{
	struct drag_data_object *obj = (struct drag_data_object *)iface;
	FORMATETC formats[DRAG_FORMAT_COUNT];
	struct drag_enum_formats *enm;

	if (out == NULL)
		return E_POINTER;
	*out = NULL;
	if (dir != DATADIR_GET)
		return E_NOTIMPL;

	drag_fill_formats(obj, formats);
	enm = drag_enum_create(formats, 0U);
	if (enm == NULL)
		return E_OUTOFMEMORY;
	*out = &enm->iface;
	return S_OK;
}

/* IDataObject::DAdvise. */
static HRESULT STDMETHODCALLTYPE
dobj_dadvise(
	IDataObject *iface,
	FORMATETC *fmt,
	DWORD advf,
	IAdviseSink *sink,
	DWORD *conn)
{
	(void)iface;
	(void)fmt;
	(void)advf;
	(void)sink;
	(void)conn;
	return OLE_E_ADVISENOTSUPPORTED;
}

/* IDataObject::DUnadvise. */
static HRESULT STDMETHODCALLTYPE
dobj_dunadvise(
	IDataObject *iface,
	DWORD conn)
{
	(void)iface;
	(void)conn;
	return OLE_E_ADVISENOTSUPPORTED;
}

/* IDataObject::EnumDAdvise. */
static HRESULT STDMETHODCALLTYPE
dobj_enum_dadvise(
	IDataObject *iface,
	IEnumSTATDATA **out)
{
	(void)iface;
	if (out != NULL)
		*out = NULL;
	return OLE_E_ADVISENOTSUPPORTED;
}

/* IDataObjectAsyncCapability::QueryInterface. */
static HRESULT STDMETHODCALLTYPE
async_query(
	IDataObjectAsyncCapability *iface,
	REFIID riid,
	void **out)
{
	struct drag_data_object *obj = drag_object_from_async(iface);

	return dobj_query(&obj->iface, riid, out);
}

/* IDataObjectAsyncCapability::AddRef. */
static ULONG STDMETHODCALLTYPE
async_add(
	IDataObjectAsyncCapability *iface)
{
	struct drag_data_object *obj = drag_object_from_async(iface);

	return dobj_add(&obj->iface);
}

/* IDataObjectAsyncCapability::Release. */
static ULONG STDMETHODCALLTYPE
async_release(
	IDataObjectAsyncCapability *iface)
{
	struct drag_data_object *obj = drag_object_from_async(iface);

	return dobj_release(&obj->iface);
}

/* IDataObjectAsyncCapability::SetAsyncMode. */
static HRESULT STDMETHODCALLTYPE
async_set_mode(
	IDataObjectAsyncCapability *iface,
	BOOL do_async)
{
	struct drag_data_object *obj = drag_object_from_async(iface);

	EnterCriticalSection(&obj->lock);
	obj->async_mode = do_async ? TRUE : FALSE;
	LeaveCriticalSection(&obj->lock);
	return S_OK;
}

/* IDataObjectAsyncCapability::GetAsyncMode. */
static HRESULT STDMETHODCALLTYPE
async_get_mode(
	IDataObjectAsyncCapability *iface,
	BOOL *is_async)
{
	struct drag_data_object *obj = drag_object_from_async(iface);

	if (is_async == NULL)
		return E_INVALIDARG;

	EnterCriticalSection(&obj->lock);
	*is_async = obj->async_mode;
	LeaveCriticalSection(&obj->lock);
	return S_OK;
}

/* IDataObjectAsyncCapability::StartOperation. */
static HRESULT STDMETHODCALLTYPE
async_start_op(
	IDataObjectAsyncCapability *iface,
	IBindCtx *reserved)
{
	struct drag_data_object *obj = drag_object_from_async(iface);

	(void)reserved;
	EnterCriticalSection(&obj->lock);
	obj->in_operation = TRUE;
	if (!obj->counted_active && obj->ctx != NULL) {
		obj->counted_active = 1;
		EnterCriticalSection(&obj->ctx->lock);
		obj->ctx->active_async++;
		LeaveCriticalSection(&obj->ctx->lock);
	}
	LeaveCriticalSection(&obj->lock);
	return S_OK;
}

/* IDataObjectAsyncCapability::InOperation. */
static HRESULT STDMETHODCALLTYPE
async_in_op(
	IDataObjectAsyncCapability *iface,
	BOOL *in_op)
{
	struct drag_data_object *obj = drag_object_from_async(iface);

	if (in_op == NULL)
		return E_INVALIDARG;

	EnterCriticalSection(&obj->lock);
	*in_op = obj->in_operation;
	LeaveCriticalSection(&obj->lock);
	return S_OK;
}

/* IDataObjectAsyncCapability::EndOperation. */
static HRESULT STDMETHODCALLTYPE
async_end_op(
	IDataObjectAsyncCapability *iface,
	HRESULT result,
	IBindCtx *reserved,
	DWORD effects)
{
	struct drag_data_object *obj = drag_object_from_async(iface);

	(void)result;
	(void)reserved;
	(void)effects;

	EnterCriticalSection(&obj->lock);
	obj->in_operation = FALSE;
	if (obj->counted_active && obj->ctx != NULL) {
		obj->counted_active = 0;
		EnterCriticalSection(&obj->ctx->lock);
		if (obj->ctx->active_async > 0U)
			obj->ctx->active_async--;
		LeaveCriticalSection(&obj->ctx->lock);
	}
	LeaveCriticalSection(&obj->lock);
	return S_OK;
}

/* Creates an IStream for one virtual file item. */
static struct drag_stream *
drag_stream_create(
	struct drag_data_object *owner,
	const struct drag_desc_item *item)
{
	struct drag_stream *s;

	s = calloc(1U, sizeof(*s));
	if (s == NULL)
		return NULL;

	s->iface.lpVtbl = &drag_stream_vtbl;
	s->references = 1;
	s->owner = owner;
	dobj_add(&owner->iface);
	s->ctx = owner->ctx;
	drag_ctx_retain(s->ctx);
	(void)snprintf(s->remote_path, sizeof(s->remote_path), "%s", item->remote_path);
	s->expected_size = item->size;
	return s;
}

/* Fills the readahead buffer starting at offset. */
static HRESULT
drag_stream_fill_buffer(
	struct drag_stream *s,
	uint64_t offset)
{
	struct sftp_worker *worker;
	uint64_t remaining;
	size_t target;
	size_t filled;
	size_t got;
	int err;

	worker = drag_ctx_acquire_worker(s->ctx);
	if (worker == NULL)
		return E_FAIL;

	if (!s->opened) {
		err = sftp_worker_stream_open(worker, s->remote_path, &s->stream_id);
		if (err != 0)
			return E_FAIL;
		s->opened = 1;
	}

	remaining = s->expected_size - offset;
	target = remaining > (uint64_t)DRAG_STREAM_READAHEAD ? DRAG_STREAM_READAHEAD : (size_t)remaining;
	if (target == 0U) {
		s->buf_offset = offset;
		s->buf_len = 0U;
		return S_OK;
	}

	if (s->buf_cap < target) {
		unsigned char *nbuf = malloc(target);

		if (nbuf == NULL)
			return E_OUTOFMEMORY;
		free(s->buf);
		s->buf = nbuf;
		s->buf_cap = target;
	}

	filled = 0U;
	while (filled < target) {
		size_t chunk = target - filled;

		if (chunk > SFTP_CLIENT_CHUNK)
			chunk = SFTP_CLIENT_CHUNK;
		worker = drag_ctx_acquire_worker(s->ctx);
		if (worker == NULL)
			return E_FAIL;
		got = 0U;
		err = sftp_worker_stream_read(worker, s->stream_id, offset + filled,
		    s->buf + filled, chunk, &got);
		if (err != 0 || got == 0U)
			return E_FAIL;
		filled += got;
	}

	s->buf_offset = offset;
	s->buf_len = filled;
	return S_OK;
}

/* IStream::QueryInterface. */
static HRESULT STDMETHODCALLTYPE
stream_query(
	IStream *iface,
	REFIID riid,
	void **out)
{
	if (out == NULL)
		return E_POINTER;

	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ISequentialStream) ||
	    IsEqualIID(riid, &IID_IStream)) {
		*out = iface;
		stream_add(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

/* IStream::AddRef. */
static ULONG STDMETHODCALLTYPE
stream_add(
	IStream *iface)
{
	struct drag_stream *s = (struct drag_stream *)iface;

	return (ULONG)InterlockedIncrement(&s->references);
}

/* IStream::Release. */
static ULONG STDMETHODCALLTYPE
stream_release(
	IStream *iface)
{
	struct drag_stream *s = (struct drag_stream *)iface;
	LONG count;

	count = InterlockedDecrement(&s->references);
	if (count == 0) {
		if (s->opened) {
			struct sftp_worker *worker = drag_ctx_acquire_worker(s->ctx);

			if (worker != NULL)
				sftp_worker_stream_close(worker, s->stream_id);
		}
		free(s->buf);
		dobj_release(&s->owner->iface);
		drag_ctx_release(s->ctx);
		free(s);
	}
	return (ULONG)count;
}

/* IStream::Read. */
static HRESULT STDMETHODCALLTYPE
stream_read(
	IStream *iface,
	void *pv,
	ULONG cb,
	ULONG *pcb_read)
{
	struct drag_stream *s = (struct drag_stream *)iface;
	unsigned char *dst = (unsigned char *)pv;
	ULONG total_read;
	uint64_t avail;
	ULONG want;

	if (pcb_read != NULL)
		*pcb_read = 0U;
	if (pv == NULL && cb > 0U)
		return STG_E_INVALIDPOINTER;
	if (cb == 0U)
		return S_OK;
	if (s->pos >= s->expected_size)
		return S_FALSE;

	avail = s->expected_size - s->pos;
	want = (uint64_t)cb > avail ? (ULONG)avail : cb;
	total_read = 0U;

	while (total_read < want) {
		size_t in_buf;
		size_t take;
		HRESULT hr;

		if (s->buf == NULL || s->pos < s->buf_offset || s->pos >= s->buf_offset + s->buf_len) {
			hr = drag_stream_fill_buffer(s, s->pos);
			if (FAILED(hr)) {
				if (pcb_read != NULL)
					*pcb_read = total_read;
				return hr;
			}
		}

		in_buf = (size_t)(s->pos - s->buf_offset);
		take = s->buf_len - in_buf;
		if (take > (size_t)(want - total_read))
			take = (size_t)(want - total_read);
		memcpy(dst + total_read, s->buf + in_buf, take);
		s->pos += take;
		total_read += (ULONG)take;
	}

	if (pcb_read != NULL)
		*pcb_read = total_read;
	return total_read < cb ? S_FALSE : S_OK;
}

/* IStream::Write (read-only). */
static HRESULT STDMETHODCALLTYPE
stream_write(
	IStream *iface,
	const void *pv,
	ULONG cb,
	ULONG *pcb_written)
{
	(void)iface;
	(void)pv;
	(void)cb;
	if (pcb_written != NULL)
		*pcb_written = 0U;
	return STG_E_ACCESSDENIED;
}

/* IStream::Seek: supports querying current position, seeking to 0, or seeking to current position. */
static HRESULT STDMETHODCALLTYPE
stream_seek(
	IStream *iface,
	LARGE_INTEGER move,
	DWORD origin,
	ULARGE_INTEGER *new_pos)
{
	struct drag_stream *s = (struct drag_stream *)iface;
	int64_t target;

	switch (origin) {
	case STREAM_SEEK_SET:
		target = move.QuadPart;
		break;
	case STREAM_SEEK_CUR:
		target = (int64_t)s->pos + move.QuadPart;
		break;
	case STREAM_SEEK_END:
		target = (int64_t)s->expected_size + move.QuadPart;
		break;
	default:
		return STG_E_INVALIDFUNCTION;
	}

	if (target == 0 || (uint64_t)target == s->pos) {
		s->pos = (uint64_t)target;
		if (new_pos != NULL)
			new_pos->QuadPart = s->pos;
		return S_OK;
	}
	return STG_E_INVALIDFUNCTION;
}

/* IStream::SetSize. */
static HRESULT STDMETHODCALLTYPE
stream_set_size(
	IStream *iface,
	ULARGE_INTEGER new_size)
{
	(void)iface;
	(void)new_size;
	return STG_E_ACCESSDENIED;
}

/* IStream::CopyTo. */
static HRESULT STDMETHODCALLTYPE
stream_copy_to(
	IStream *iface,
	IStream *dest,
	ULARGE_INTEGER cb,
	ULARGE_INTEGER *read_out,
	ULARGE_INTEGER *written_out)
{
	(void)iface;
	(void)dest;
	(void)cb;
	(void)read_out;
	(void)written_out;
	return E_NOTIMPL;
}

/* IStream::Commit. */
static HRESULT STDMETHODCALLTYPE
stream_commit(
	IStream *iface,
	DWORD flags)
{
	(void)iface;
	(void)flags;
	return S_OK;
}

/* IStream::Revert. */
static HRESULT STDMETHODCALLTYPE
stream_revert(
	IStream *iface)
{
	(void)iface;
	return E_NOTIMPL;
}

/* IStream::LockRegion. */
static HRESULT STDMETHODCALLTYPE
stream_lock_region(
	IStream *iface,
	ULARGE_INTEGER offset,
	ULARGE_INTEGER cb,
	DWORD type)
{
	(void)iface;
	(void)offset;
	(void)cb;
	(void)type;
	return STG_E_INVALIDFUNCTION;
}

/* IStream::UnlockRegion. */
static HRESULT STDMETHODCALLTYPE
stream_unlock_region(
	IStream *iface,
	ULARGE_INTEGER offset,
	ULARGE_INTEGER cb,
	DWORD type)
{
	(void)iface;
	(void)offset;
	(void)cb;
	(void)type;
	return STG_E_INVALIDFUNCTION;
}

/* IStream::Stat. */
static HRESULT STDMETHODCALLTYPE
stream_stat(
	IStream *iface,
	STATSTG *statstg,
	DWORD flag)
{
	struct drag_stream *s = (struct drag_stream *)iface;

	(void)flag;
	if (statstg == NULL)
		return STG_E_INVALIDPOINTER;

	memset(statstg, 0, sizeof(*statstg));
	statstg->type = STGTY_STREAM;
	statstg->cbSize.QuadPart = s->expected_size;
	return S_OK;
}

/* IStream::Clone. */
static HRESULT STDMETHODCALLTYPE
stream_clone(
	IStream *iface,
	IStream **out)
{
	(void)iface;
	if (out != NULL)
		*out = NULL;
	return E_NOTIMPL;
}

/* Creates an IEnumFORMATETC over formats starting at index. */
static struct drag_enum_formats *
drag_enum_create(
	const FORMATETC formats[DRAG_FORMAT_COUNT],
	ULONG index)
{
	struct drag_enum_formats *enm;

	enm = calloc(1U, sizeof(*enm));
	if (enm == NULL)
		return NULL;
	enm->iface.lpVtbl = &drag_enum_vtbl;
	enm->references = 1;
	memcpy(enm->formats, formats, sizeof(enm->formats));
	enm->index = index;
	return enm;
}

/* IEnumFORMATETC::QueryInterface. */
static HRESULT STDMETHODCALLTYPE
enum_query(
	IEnumFORMATETC *iface,
	REFIID riid,
	void **out)
{
	if (out == NULL)
		return E_POINTER;
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IEnumFORMATETC)) {
		*out = iface;
		enum_add(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

/* IEnumFORMATETC::AddRef. */
static ULONG STDMETHODCALLTYPE
enum_add(
	IEnumFORMATETC *iface)
{
	struct drag_enum_formats *enm = (struct drag_enum_formats *)iface;

	return (ULONG)InterlockedIncrement(&enm->references);
}

/* IEnumFORMATETC::Release. */
static ULONG STDMETHODCALLTYPE
enum_release(
	IEnumFORMATETC *iface)
{
	struct drag_enum_formats *enm = (struct drag_enum_formats *)iface;
	LONG count;

	count = InterlockedDecrement(&enm->references);
	if (count == 0)
		free(enm);
	return (ULONG)count;
}

/* IEnumFORMATETC::Next. */
static HRESULT STDMETHODCALLTYPE
enum_next(
	IEnumFORMATETC *iface,
	ULONG celt,
	FORMATETC *elts,
	ULONG *fetched)
{
	struct drag_enum_formats *enm = (struct drag_enum_formats *)iface;
	ULONG copied;

	if (elts == NULL)
		return E_INVALIDARG;
	if (celt > 1U && fetched == NULL)
		return E_INVALIDARG;

	copied = 0U;
	while (copied < celt && enm->index < DRAG_FORMAT_COUNT) {
		elts[copied] = enm->formats[enm->index];
		enm->index++;
		copied++;
	}
	if (fetched != NULL)
		*fetched = copied;
	return copied == celt ? S_OK : S_FALSE;
}

/* IEnumFORMATETC::Skip. */
static HRESULT STDMETHODCALLTYPE
enum_skip(
	IEnumFORMATETC *iface,
	ULONG celt)
{
	struct drag_enum_formats *enm = (struct drag_enum_formats *)iface;
	ULONG avail = DRAG_FORMAT_COUNT - enm->index;

	if (celt > avail) {
		enm->index = DRAG_FORMAT_COUNT;
		return S_FALSE;
	}
	enm->index += celt;
	return S_OK;
}

/* IEnumFORMATETC::Reset. */
static HRESULT STDMETHODCALLTYPE
enum_reset(
	IEnumFORMATETC *iface)
{
	struct drag_enum_formats *enm = (struct drag_enum_formats *)iface;

	enm->index = 0U;
	return S_OK;
}

/* IEnumFORMATETC::Clone. */
static HRESULT STDMETHODCALLTYPE
enum_clone(
	IEnumFORMATETC *iface,
	IEnumFORMATETC **out)
{
	struct drag_enum_formats *enm = (struct drag_enum_formats *)iface;
	struct drag_enum_formats *copy;

	if (out == NULL)
		return E_POINTER;
	copy = drag_enum_create(enm->formats, enm->index);
	if (copy == NULL) {
		*out = NULL;
		return E_OUTOFMEMORY;
	}
	*out = &copy->iface;
	return S_OK;
}

/* IDropSource::QueryInterface. */
static HRESULT STDMETHODCALLTYPE
dsrc_query(
	IDropSource *iface,
	REFIID riid,
	void **out)
{
	if (out == NULL)
		return E_POINTER;
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDropSource)) {
		*out = iface;
		dsrc_add(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

/* IDropSource::AddRef. */
static ULONG STDMETHODCALLTYPE
dsrc_add(
	IDropSource *iface)
{
	struct drag_drop_source *src = (struct drag_drop_source *)iface;

	return (ULONG)InterlockedIncrement(&src->references);
}

/* IDropSource::Release. */
static ULONG STDMETHODCALLTYPE
dsrc_release(
	IDropSource *iface)
{
	struct drag_drop_source *src = (struct drag_drop_source *)iface;
	LONG count;

	count = InterlockedDecrement(&src->references);
	if (count == 0)
		free(src);
	return (ULONG)count;
}

/* IDropSource::QueryContinueDrag. */
static HRESULT STDMETHODCALLTYPE
dsrc_continue(
	IDropSource *iface,
	BOOL escape,
	DWORD keys)
{
	(void)iface;
	if (escape)
		return DRAGDROP_S_CANCEL;
	if ((keys & MK_LBUTTON) == 0U)
		return DRAGDROP_S_DROP;
	return S_OK;
}

/* IDropSource::GiveFeedback. */
static HRESULT STDMETHODCALLTYPE
dsrc_feedback(
	IDropSource *iface,
	DWORD effect)
{
	(void)iface;
	(void)effect;
	return DRAGDROP_S_USEDEFAULTCURSORS;
}
