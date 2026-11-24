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

	/* Allocates the context and initializes its lock and callback fields. */
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
	/* Clears callbacks and marks the context detached under its lock. */
	if (ctx == NULL)
		return;
	EnterCriticalSection(&ctx->lock);
	ctx->detached = 1;
	ctx->cancelled = 1;
	ctx->worker = NULL;
	ctx->notify = NULL;
	ctx->notify_data = NULL;
	LeaveCriticalSection(&ctx->lock);

	/* Drops the view's reference to the context. */
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

	/* Reads the active async count under the context lock. */
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
	/* Sets the cancellation flag under the context lock. */
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
	BOOL once_ok;
	LPVOID tls_val;

	/* Validates arguments and ensures the MTA COM thread is ready. */
	if (ctx == NULL || items == NULL || count == 0U || object_out == NULL)
		return EINVAL;
	*object_out = NULL;
	once_ok = InitOnceExecuteOnce(&drag_com_once, drag_com_init_cb, NULL, NULL);
	if (!once_ok || !drag_com_ready)
		return EIO;

	/* Initializes OLE once per caller thread. */
	if (drag_ole_tls != TLS_OUT_OF_INDEXES) {
		tls_val = TlsGetValue(drag_ole_tls);
		if (tls_val == NULL) {
			hr = OleInitialize(NULL);
			if (hr >= 0)
				(void)TlsSetValue(drag_ole_tls, (LPVOID)(uintptr_t)1);
		}
	}

	/* Resets cancellation state before creating the data object. */
	EnterCriticalSection(&ctx->lock);
	ctx->cancelled = 0;
	LeaveCriticalSection(&ctx->lock);

	/* Creates the data object on the MTA thread and marshals it here. */
	marshal_stream = NULL;
	hr = drag_com_create_marshaled(ctx, items, count, &marshal_stream);
	if (hr < 0 || marshal_stream == NULL)
		return EIO;

	/* Unmarshals the IDataObject proxy for the caller's apartment. */
	proxy = NULL;
	hr = CoGetInterfaceAndReleaseStream(marshal_stream, &IID_IDataObject, (void **)&proxy);
	if (hr < 0 || proxy == NULL)
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
	HRESULT qhr;
	BOOL in_op;
	int error;

	/* Creates the marshaled data object and local drop source. */
	proxy = NULL;
	error = sftp_drag_create_object(ctx, items, count, &proxy);
	if (error != 0)
		return error;
	source = calloc(1U, sizeof(*source));
	if (source == NULL) {
		IDataObject_Release(proxy);
		return ENOMEM;
	}

	/* Initializes the drop source and runs the modal OLE drag loop. */
	source->iface.lpVtbl = &drag_source_vtbl;
	source->references = 1;

	/* Runs the modal OLE drag loop. */
	effect = DROPEFFECT_NONE;
	hr = DoDragDrop(proxy, &source->iface, DROPEFFECT_COPY, &effect);

	/* Checks whether the drop target took over asynchronously. */
	in_op = FALSE;
	async_cap = NULL;
	qhr = IDataObject_QueryInterface(proxy, &IID_IDataObjectAsyncCapability, (void **)&async_cap);
	if (qhr >= 0 && async_cap != NULL) {
		(void)IDataObjectAsyncCapability_InOperation(async_cap, &in_op);
		IDataObjectAsyncCapability_Release(async_cap);
	}

	/* Releases local COM references and translates the drag result. */
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

	/* Creates synchronization primitives and spawns the MTA worker thread. */
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

	/* Marks the MTA thread ready. */
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

	/* Initializes COM in the multithreaded apartment. */
	(void)param;
	hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	if (hr < 0)
		return 1U;

	/* Services queued data-object creation and marshaling requests. */
	for (;;) {
		WaitForSingleObject(drag_com_wake, INFINITE);

		/* Drains all pending requests before sleeping again. */
		for (;;) {
			EnterCriticalSection(&drag_com_lock);
			req = drag_com_head;
			if (req != NULL) {
				drag_com_head = req->next;
				if (drag_com_head == NULL)
					drag_com_tail = NULL;
			}

			/* Releases the queue lock before processing the request. */
			LeaveCriticalSection(&drag_com_lock);
			if (req == NULL)
				break;

			/* Creates and marshals the requested IDataObject. */
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

			/* Wakes the requesting thread. */
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
	BOOL once_ok;

	/* Ensures the MTA thread is running and initializes the request. */
	*stream_out = NULL;
	once_ok = InitOnceExecuteOnce(&drag_com_once, drag_com_init_cb, NULL, NULL);
	if (!once_ok || !drag_com_ready)
		return E_FAIL;
	memset(&req, 0, sizeof(req));
	req.ctx = ctx;
	req.items = items;
	req.count = count;
	req.hr = E_FAIL;
	req.done_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (req.done_event == NULL)
		return E_OUTOFMEMORY;

	/* Enqueues the request on the MTA thread's list. */
	EnterCriticalSection(&drag_com_lock);
	if (drag_com_tail != NULL)
		drag_com_tail->next = &req;
	else
		drag_com_head = &req;
	drag_com_tail = &req;
	LeaveCriticalSection(&drag_com_lock);

	/* Signals the MTA thread and waits for completion. */
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
	LONG refs;

	/* Frees the context once its final reference is released. */
	if (ctx == NULL)
		return;
	refs = InterlockedDecrement(&ctx->references);
	if (refs == 0) {
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

	/* Snapshots the callback under the lock before invoking it. */
	if (ctx == NULL)
		return;
	EnterCriticalSection(&ctx->lock);
	if (ctx->detached)
		cb = NULL;
	else
		cb = ctx->notify;
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

	/* Reads the worker pointer under the context lock. */
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

	/* Shifts Unix epoch seconds to Windows 100-ns FILETIME units. */
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

	/* Rejects invalid buffers and converts with strict UTF-8 validation. */
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

	/* Formats the relative path with or without a parent prefix. */
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
	int conv_err;

	/* Enforces the descriptor item limit and converts the relative path to UTF-16. */
	if (obj->desc_count >= SFTP_DRAG_ITEMS_MAX)
		return E2BIG;
	conv_err = drag_utf8_to_wide(rel_utf8, rel_wide, MAX_PATH);
	if (conv_err != 0)
		return ENAMETOOLONG;

	/* Grows the descriptor array if full. */
	if (obj->desc_count == obj->desc_cap) {
		if (obj->desc_cap == 0U)
			new_cap = 32U;
		else
			new_cap = obj->desc_cap * 2U;
		if (new_cap > SFTP_DRAG_ITEMS_MAX)
			new_cap = SFTP_DRAG_ITEMS_MAX;
		grown = realloc(obj->desc_items, new_cap * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		obj->desc_items = grown;
		obj->desc_cap = new_cap;
	}

	/* Appends the descriptor item and updates total byte size. */
	(void)snprintf(obj->desc_items[obj->desc_count].remote_path,
	    sizeof(obj->desc_items[obj->desc_count].remote_path), "%s", remote_path);
	wcscpy(obj->desc_items[obj->desc_count].rel_wide, rel_wide);
	if (is_dir) {
		obj->desc_items[obj->desc_count].size = 0ULL;
		obj->desc_items[obj->desc_count].is_dir = 1;
	} else {
		obj->desc_items[obj->desc_count].size = size;
		obj->desc_items[obj->desc_count].is_dir = 0;
	}

	/* Records modification time and updates total file byte count. */
	obj->desc_items[obj->desc_count].mtime = mtime;
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
	struct drag_dir_node *grown;
	struct drag_dir_node cur;
	struct sftp_dir_listing *listing;
	const struct sftp_drag_item *top;
	const struct sftp_dir_entry *ent;
	char child_remote[SSH_SFTP_PATH_MAX];
	char child_rel[SSH_SFTP_PATH_MAX];
	const char *why;
	size_t stack_count;
	size_t stack_cap;
	size_t nc;
	size_t i;
	size_t j;
	size_t name_len;
	HRESULT hr;
	int err;
	int valid;
	int is_dir;
	int cmp_dot;
	int cmp_dotdot;

	/* Returns cached expansion status if already expanded. */
	EnterCriticalSection(&obj->lock);
	if (obj->expanded) {
		if (obj->expand_failed)
			hr = E_FAIL;
		else
			hr = S_OK;
		LeaveCriticalSection(&obj->lock);
		return hr;
	}

	/* Acquires the active worker before traversing directories. */
	worker = drag_ctx_acquire_worker(obj->ctx);
	if (worker == NULL) {
		obj->expanded = 1;
		obj->expand_failed = 1;
		LeaveCriticalSection(&obj->lock);
		return E_FAIL;
	}

	/* Seeds the descriptor list and directory stack with top-level items. */
	stack = NULL;
	stack_count = 0U;
	stack_cap = 0U;
	for (i = 0U; i < obj->top_count; i++) {
		top = &obj->top_items[i];
		why = NULL;
		name_len = strlen(top->name);
		valid = sftp_transfer_valid_download_name(top->name, name_len, &why);
		if (!valid)
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

		/* Skips items whose path exceeds MAX_PATH and aborts on allocation failure. */
		if (err == ENAMETOOLONG)
			continue;
		if (err != 0) {
			free(stack);
			obj->expanded = 1;
			obj->expand_failed = 1;
			LeaveCriticalSection(&obj->lock);
			return E_OUTOFMEMORY;
		}

		/* Pushes top-level directories onto the traversal stack. */
		if (top->is_dir) {
			if (stack_count == stack_cap) {
				if (stack_cap == 0U)
					nc = 16U;
				else
					nc = stack_cap * 2U;
				grown = realloc(stack, nc * sizeof(*grown));
				if (grown == NULL) {
					free(stack);
					obj->expanded = 1;
					obj->expand_failed = 1;
					LeaveCriticalSection(&obj->lock);
					return E_OUTOFMEMORY;
				}

				/* Adopts the resized directory stack. */
				stack = grown;
				stack_cap = nc;
			}

			/* Records the directory's remote and relative paths. */
			(void)snprintf(stack[stack_count].remote_path, sizeof(stack[stack_count].remote_path),
			    "%s", top->remote_path);
			(void)snprintf(stack[stack_count].rel_utf8, sizeof(stack[stack_count].rel_utf8),
			    "%s", top->name);
			stack_count++;
		}
	}

	/* Iteratively expands all queued directories. */
	while (stack_count > 0U) {
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

		/* Lists the directory on the remote server. */
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

		/* Appends each valid child entry and queues subdirectories. */
		for (j = 0U; j < listing->count; j++) {
			ent = &listing->entries[j];
			why = NULL;
			cmp_dot = strcmp(ent->name, ".");
			cmp_dotdot = strcmp(ent->name, "..");
			if (cmp_dot == 0 || cmp_dotdot == 0)
				continue;
			name_len = strlen(ent->name);
			valid = sftp_transfer_valid_download_name(ent->name, name_len, &why);
			if (!valid)
				continue;
			if (ent->is_link && ent->link_is_dir)
				continue;
			err = sftp_transfer_join_remote(child_remote, sizeof(child_remote), cur.remote_path, ent->name);
			if (err != 0)
				continue;
			err = drag_join_rel_utf8(child_rel, sizeof(child_rel), cur.rel_utf8, ent->name);
			if (err != 0)
				continue;
			if (ent->is_dir)
				is_dir = 1;
			else
				is_dir = 0;
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

			/* Skips entries exceeding MAX_PATH and aborts on allocation failure. */
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

			/* Pushes child directories onto the traversal stack. */
			if (is_dir) {
				if (stack_count == stack_cap) {
					if (stack_cap == 0U)
						nc = 16U;
					else
						nc = stack_cap * 2U;
					grown = realloc(stack, nc * sizeof(*grown));
					if (grown == NULL) {
						sftp_dir_listing_free(listing);
						free(stack);
						obj->expanded = 1;
						obj->expand_failed = 1;
						LeaveCriticalSection(&obj->lock);
						return E_OUTOFMEMORY;
					}

					/* Adopts the resized directory stack. */
					stack = grown;
					stack_cap = nc;
				}

				/* Records the child directory's remote and relative paths. */
				(void)snprintf(stack[stack_count].remote_path, sizeof(stack[stack_count].remote_path),
				    "%s", child_remote);
				(void)snprintf(stack[stack_count].rel_utf8, sizeof(stack[stack_count].rel_utf8),
				    "%s", child_rel);
				stack_count++;
			}
		}

		/* Releases the listing for this directory. */
		sftp_dir_listing_free(listing);
	}

	/* Marks expansion complete and releases the traversal stack. */
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
	/* Fills descriptor, contents, and preferred drop-effect formats. */
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
	/* Validates content aspect and matches each supported clipboard format. */
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

	/* Allocates the object and copies top-level drag items. */
	obj = calloc(1U, sizeof(*obj));
	if (obj == NULL)
		return NULL;
	obj->top_items = calloc(count, sizeof(*obj->top_items));
	if (obj->top_items == NULL) {
		free(obj);
		return NULL;
	}

	/* Initializes COM vtables, synchronization, and clipboard formats. */
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
	int eq_unk;
	int eq_dobj;
	int eq_async;

	/* Exposes IUnknown, IDataObject, and IDataObjectAsyncCapability. */
	if (out == NULL)
		return E_POINTER;
	eq_unk = IsEqualIID(riid, &IID_IUnknown);
	eq_dobj = IsEqualIID(riid, &IID_IDataObject);
	if (eq_unk || eq_dobj) {
		*out = &obj->iface;
		dobj_add(&obj->iface);
		return S_OK;
	}

	/* Checks for the async capability interface. */
	eq_async = IsEqualIID(riid, &IID_IDataObjectAsyncCapability);
	if (eq_async) {
		*out = &obj->async_iface;
		dobj_add(&obj->iface);
		return S_OK;
	}

	/* Unsupported interface. */
	*out = NULL;
	return E_NOINTERFACE;
}

/* IDataObject::AddRef. */
static ULONG STDMETHODCALLTYPE
dobj_add(
	IDataObject *iface)
{
	struct drag_data_object *obj = (struct drag_data_object *)iface;

	/* Increments the COM reference count atomically. */
	return (ULONG)InterlockedIncrement(&obj->references);
}

/* IDataObject::Release. */
static ULONG STDMETHODCALLTYPE
dobj_release(
	IDataObject *iface)
{
	struct drag_data_object *obj = (struct drag_data_object *)iface;
	LONG count;

	/* Decrements the reference count and frees the object at zero. */
	count = InterlockedDecrement(&obj->references);
	if (count == 0) {
		if (obj->counted_active && obj->ctx != NULL) {
			EnterCriticalSection(&obj->ctx->lock);
			if (obj->ctx->active_async > 0U)
				obj->ctx->active_async--;
			LeaveCriticalSection(&obj->ctx->lock);
		}

		/* Releases the context reference and heap buffers. */
		drag_ctx_release(obj->ctx);
		DeleteCriticalSection(&obj->lock);
		free(obj->top_items);
		free(obj->desc_items);
		free(obj);
	}

	/* Returns the updated reference count. */
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
	const struct drag_desc_item *item;
	struct drag_stream *stream;
	FILEGROUPDESCRIPTORW *fgd;
	FILEDESCRIPTORW *fd;
	HGLOBAL mem;
	DWORD *effect;
	size_t bytes;
	size_t i;
	int kind;
	int should_notify;
	HRESULT hr;

	/* Validates outputs and matches the requested clipboard format. */
	if (fmt == NULL || medium == NULL)
		return E_INVALIDARG;
	memset(medium, 0, sizeof(*medium));
	kind = drag_match_format(obj, fmt);
	if (kind < 0)
		return DV_E_FORMATETC;

	/* Handles CFSTR_PREFERREDDROPEFFECT before expanding remote directories. */
	if (kind == 2) {
		mem = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
		if (mem == NULL)
			return E_OUTOFMEMORY;
		effect = GlobalLock(mem);
		if (effect == NULL) {
			GlobalFree(mem);
			return E_OUTOFMEMORY;
		}

		/* Advertises copy-only drop semantics. */
		*effect = DROPEFFECT_COPY;
		GlobalUnlock(mem);
		medium->tymed = TYMED_HGLOBAL;
		medium->hGlobal = mem;
		medium->pUnkForRelease = NULL;
		return S_OK;
	}

	/* Expands the selected remote tree on first descriptor or content request. */
	hr = drag_ensure_expanded(obj);
	if (hr < 0)
		return hr;

	/* Builds CFSTR_FILEDESCRIPTORW from the expanded item list. */
	if (kind == 0) {
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

		/* Populates each FILEDESCRIPTORW entry. */
		fgd->cItems = (UINT)obj->desc_count;
		for (i = 0U; i < obj->desc_count; i++) {
			item = &obj->desc_items[i];
			fd = &fgd->fgd[i];
			fd->dwFlags = FD_ATTRIBUTES | FD_PROGRESSUI | FD_UNICODE;
			if (item->is_dir) {
				fd->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
			} else {
				fd->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
				fd->dwFlags |= FD_FILESIZE;
				fd->nFileSizeHigh = (DWORD)(item->size >> 32);
				fd->nFileSizeLow = (DWORD)(item->size & 0xffffffffU);
			}

			/* Sets modification timestamp when available. */
			if (item->mtime != 0U) {
				fd->dwFlags |= FD_WRITESTIME;
				fd->ftLastWriteTime = drag_unix_to_filetime(item->mtime);
			}

			/* Copies the UTF-16 relative path. */
			wcsncpy(fd->cFileName, item->rel_wide, MAX_PATH - 1U);
			fd->cFileName[MAX_PATH - 1U] = L'\0';
		}

		/* Unlocks and returns the global descriptor handle. */
		GlobalUnlock(mem);
		medium->tymed = TYMED_HGLOBAL;
		medium->hGlobal = mem;
		medium->pUnkForRelease = NULL;
		return S_OK;
	}

	/* Validates lindex for CFSTR_FILECONTENTS (TYMED_ISTREAM). */
	if (fmt->lindex < 0 || (size_t)fmt->lindex >= obj->desc_count)
		return DV_E_LINDEX;
	if (obj->desc_items[fmt->lindex].is_dir)
		return DV_E_LINDEX;

	/* Rejects large synchronous drops to avoid blocking the UI thread. */
	EnterCriticalSection(&obj->lock);
	if (!obj->in_operation && obj->total_bytes > SFTP_DRAG_SYNC_LIMIT_BYTES) {
		should_notify = !obj->sync_limit_reported;
		obj->sync_limit_reported = 1;
		LeaveCriticalSection(&obj->lock);
		if (should_notify)
			drag_ctx_notify(obj->ctx, "This application cannot take a large drag; use Download instead.");
		return E_FAIL;
	}

	/* Releases the object lock before creating the stream. */
	LeaveCriticalSection(&obj->lock);

	/* Creates an IStream backed by the remote file. */
	stream = drag_stream_create(obj, &obj->desc_items[fmt->lindex]);
	if (stream == NULL)
		return E_OUTOFMEMORY;
	medium->tymed = TYMED_ISTREAM;
	medium->pstm = &stream->iface;
	medium->pUnkForRelease = NULL;
	return S_OK;
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
	int kind;

	/* Returns S_OK when fmt matches one of our supported formats. */
	kind = drag_match_format(obj, fmt);
	if (kind >= 0)
		return S_OK;
	return DV_E_FORMATETC;
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

	/* Validates arguments and direction. */
	if (out == NULL)
		return E_POINTER;
	*out = NULL;
	if (dir != DATADIR_GET)
		return E_NOTIMPL;

	/* Creates an enumerator over the supported formats. */
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

	/* Delegates QueryInterface to the enclosing data object. */
	return dobj_query(&obj->iface, riid, out);
}

/* IDataObjectAsyncCapability::AddRef. */
static ULONG STDMETHODCALLTYPE
async_add(
	IDataObjectAsyncCapability *iface)
{
	struct drag_data_object *obj = drag_object_from_async(iface);

	/* Delegates AddRef to the enclosing data object. */
	return dobj_add(&obj->iface);
}

/* IDataObjectAsyncCapability::Release. */
static ULONG STDMETHODCALLTYPE
async_release(
	IDataObjectAsyncCapability *iface)
{
	struct drag_data_object *obj = drag_object_from_async(iface);

	/* Delegates Release to the enclosing data object. */
	return dobj_release(&obj->iface);
}

/* IDataObjectAsyncCapability::SetAsyncMode. */
static HRESULT STDMETHODCALLTYPE
async_set_mode(
	IDataObjectAsyncCapability *iface,
	BOOL do_async)
{
	struct drag_data_object *obj = drag_object_from_async(iface);

	/* Records whether the drop target supports asynchronous transfers. */
	EnterCriticalSection(&obj->lock);
	if (do_async)
		obj->async_mode = TRUE;
	else
		obj->async_mode = FALSE;
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

	/* Returns the current async mode flag under the object lock. */
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

	/* Marks the operation active and increments the context's async counter. */
	(void)reserved;
	EnterCriticalSection(&obj->lock);
	obj->in_operation = TRUE;
	if (!obj->counted_active && obj->ctx != NULL) {
		obj->counted_active = 1;
		EnterCriticalSection(&obj->ctx->lock);
		obj->ctx->active_async++;
		LeaveCriticalSection(&obj->ctx->lock);
	}

	/* Releases the object lock. */
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

	/* Reads whether an async drag operation is currently running. */
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

	/* Clears the operation flag and decrements the context's async counter. */
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

	/* Releases the object lock. */
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

	/* Allocates the stream and retains the owning data object and context. */
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
	unsigned char *nbuf;
	uint64_t remaining;
	size_t target;
	size_t filled;
	size_t chunk;
	size_t got;
	int err;

	/* Opens the remote stream lazily on the first read. */
	worker = drag_ctx_acquire_worker(s->ctx);
	if (worker == NULL)
		return E_FAIL;
	if (!s->opened) {
		err = sftp_worker_stream_open(worker, s->remote_path, &s->stream_id);
		if (err != 0)
			return E_FAIL;
		s->opened = 1;
	}

	/* Computes how many bytes to read ahead from offset. */
	remaining = s->expected_size - offset;
	if (remaining > (uint64_t)DRAG_STREAM_READAHEAD)
		target = DRAG_STREAM_READAHEAD;
	else
		target = (size_t)remaining;
	if (target == 0U) {
		s->buf_offset = offset;
		s->buf_len = 0U;
		return S_OK;
	}

	/* Grows the readahead buffer if needed. */
	if (s->buf_cap < target) {
		nbuf = malloc(target);
		if (nbuf == NULL)
			return E_OUTOFMEMORY;
		free(s->buf);
		s->buf = nbuf;
		s->buf_cap = target;
	}

	/* Fills the readahead buffer in SFTP_CLIENT_CHUNK-sized pieces. */
	filled = 0U;
	while (filled < target) {
		chunk = target - filled;
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

	/* Updates buffer window metadata. */
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
	int eq_unk;
	int eq_seq;
	int eq_stm;

	/* Exposes IUnknown, ISequentialStream, and IStream. */
	if (out == NULL)
		return E_POINTER;
	eq_unk = IsEqualIID(riid, &IID_IUnknown);
	eq_seq = IsEqualIID(riid, &IID_ISequentialStream);
	eq_stm = IsEqualIID(riid, &IID_IStream);
	if (eq_unk || eq_seq || eq_stm) {
		*out = iface;
		stream_add(iface);
		return S_OK;
	}

	/* Unsupported interface. */
	*out = NULL;
	return E_NOINTERFACE;
}

/* IStream::AddRef. */
static ULONG STDMETHODCALLTYPE
stream_add(
	IStream *iface)
{
	struct drag_stream *s = (struct drag_stream *)iface;

	/* Increments the COM reference count atomically. */
	return (ULONG)InterlockedIncrement(&s->references);
}

/* IStream::Release. */
static ULONG STDMETHODCALLTYPE
stream_release(
	IStream *iface)
{
	struct drag_stream *s = (struct drag_stream *)iface;
	struct sftp_worker *worker;
	LONG count;

	/* Closes the remote stream and frees resources when the last reference drops. */
	count = InterlockedDecrement(&s->references);
	if (count == 0) {
		if (s->opened) {
			worker = drag_ctx_acquire_worker(s->ctx);
			if (worker != NULL)
				sftp_worker_stream_close(worker, s->stream_id);
		}

		/* Releases buffers and parent references. */
		free(s->buf);
		dobj_release(&s->owner->iface);
		drag_ctx_release(s->ctx);
		free(s);
	}

	/* Returns the updated reference count. */
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
	size_t in_buf;
	size_t take;
	HRESULT hr;

	/* Validates arguments and handles zero-byte or EOF reads. */
	if (pcb_read != NULL)
		*pcb_read = 0U;
	if (pv == NULL && cb > 0U)
		return STG_E_INVALIDPOINTER;
	if (cb == 0U)
		return S_OK;
	if (s->pos >= s->expected_size)
		return S_FALSE;

	/* Clamps the requested byte count to remaining file length. */
	avail = s->expected_size - s->pos;
	if ((uint64_t)cb > avail)
		want = (ULONG)avail;
	else
		want = cb;
	total_read = 0U;

	/* Copies from the readahead buffer, refilling it whenever pos leaves the window. */
	while (total_read < want) {
		if (s->buf == NULL || s->pos < s->buf_offset || s->pos >= s->buf_offset + s->buf_len) {
			hr = drag_stream_fill_buffer(s, s->pos);
			if (hr < 0) {
				if (pcb_read != NULL)
					*pcb_read = total_read;
				return hr;
			}
		}

		/* Copies the available slice from the current buffer window. */
		in_buf = (size_t)(s->pos - s->buf_offset);
		take = s->buf_len - in_buf;
		if (take > (size_t)(want - total_read))
			take = (size_t)(want - total_read);
		memcpy(dst + total_read, s->buf + in_buf, take);
		s->pos += take;
		total_read += (ULONG)take;
	}

	/* Reports bytes read and returns S_FALSE if truncated at EOF. */
	if (pcb_read != NULL)
		*pcb_read = total_read;
	if (total_read < cb)
		return S_FALSE;
	return S_OK;
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

	/* Computes the target offset from the requested origin. */
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

	/* Allows seeking to 0 or querying the current position. */
	if (target == 0 || (uint64_t)target == s->pos) {
		s->pos = (uint64_t)target;
		if (new_pos != NULL)
			new_pos->QuadPart = s->pos;
		return S_OK;
	}

	/* Arbitrary random seeks are not supported. */
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

	/* Populates stream type and expected byte size. */
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

	/* Allocates and initializes the format enumerator. */
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
	int eq_unk;
	int eq_enum;

	/* Exposes IUnknown and IEnumFORMATETC. */
	if (out == NULL)
		return E_POINTER;
	eq_unk = IsEqualIID(riid, &IID_IUnknown);
	eq_enum = IsEqualIID(riid, &IID_IEnumFORMATETC);
	if (eq_unk || eq_enum) {
		*out = iface;
		enum_add(iface);
		return S_OK;
	}

	/* Unsupported interface. */
	*out = NULL;
	return E_NOINTERFACE;
}

/* IEnumFORMATETC::AddRef. */
static ULONG STDMETHODCALLTYPE
enum_add(
	IEnumFORMATETC *iface)
{
	struct drag_enum_formats *enm = (struct drag_enum_formats *)iface;

	/* Increments the COM reference count atomically. */
	return (ULONG)InterlockedIncrement(&enm->references);
}

/* IEnumFORMATETC::Release. */
static ULONG STDMETHODCALLTYPE
enum_release(
	IEnumFORMATETC *iface)
{
	struct drag_enum_formats *enm = (struct drag_enum_formats *)iface;
	LONG count;

	/* Frees the enumerator once its reference count reaches zero. */
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

	/* Validates outputs before copying formats. */
	if (elts == NULL)
		return E_INVALIDARG;
	if (celt > 1U && fetched == NULL)
		return E_INVALIDARG;

	/* Copies up to celt remaining FORMATETC entries. */
	copied = 0U;
	while (copied < celt && enm->index < DRAG_FORMAT_COUNT) {
		elts[copied] = enm->formats[enm->index];
		enm->index++;
		copied++;
	}

	/* Reports copied count and whether all requested items were returned. */
	if (fetched != NULL)
		*fetched = copied;
	if (copied == celt)
		return S_OK;
	return S_FALSE;
}

/* IEnumFORMATETC::Skip. */
static HRESULT STDMETHODCALLTYPE
enum_skip(
	IEnumFORMATETC *iface,
	ULONG celt)
{
	struct drag_enum_formats *enm = (struct drag_enum_formats *)iface;
	ULONG avail = DRAG_FORMAT_COUNT - enm->index;

	/* Advances up to the end of the format array. */
	if (celt > avail) {
		enm->index = DRAG_FORMAT_COUNT;
		return S_FALSE;
	}

	/* Advances by celt entries. */
	enm->index += celt;
	return S_OK;
}

/* IEnumFORMATETC::Reset. */
static HRESULT STDMETHODCALLTYPE
enum_reset(
	IEnumFORMATETC *iface)
{
	struct drag_enum_formats *enm = (struct drag_enum_formats *)iface;

	/* Resets the enumeration cursor to the start. */
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

	/* Creates an independent enumerator at the same position. */
	if (out == NULL)
		return E_POINTER;
	copy = drag_enum_create(enm->formats, enm->index);
	if (copy == NULL) {
		*out = NULL;
		return E_OUTOFMEMORY;
	}

	/* Returns the cloned interface. */
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
	int eq_unk;
	int eq_dsrc;

	/* Exposes IUnknown and IDropSource. */
	if (out == NULL)
		return E_POINTER;
	eq_unk = IsEqualIID(riid, &IID_IUnknown);
	eq_dsrc = IsEqualIID(riid, &IID_IDropSource);
	if (eq_unk || eq_dsrc) {
		*out = iface;
		dsrc_add(iface);
		return S_OK;
	}

	/* Unsupported interface. */
	*out = NULL;
	return E_NOINTERFACE;
}

/* IDropSource::AddRef. */
static ULONG STDMETHODCALLTYPE
dsrc_add(
	IDropSource *iface)
{
	struct drag_drop_source *src = (struct drag_drop_source *)iface;

	/* Increments the COM reference count atomically. */
	return (ULONG)InterlockedIncrement(&src->references);
}

/* IDropSource::Release. */
static ULONG STDMETHODCALLTYPE
dsrc_release(
	IDropSource *iface)
{
	struct drag_drop_source *src = (struct drag_drop_source *)iface;
	LONG count;

	/* Frees the drop source when its reference count reaches zero. */
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
