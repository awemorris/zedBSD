/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The input method on Windows: the Text Services Framework (TSF) where it
 * can be started on the window's thread, and IMM32 otherwise.
 *
 * TSF: each window has a document of its own (ITfDocumentMgr) with one
 * context over libkeiland's text store (ITextStoreACP, with the
 * ITfContextOwnerCompositionSink of the same object).  The store holds
 * only what the input method is writing: the text being composed, and
 * text it inserted that was not handed on yet.  After each lock the store
 * granted, the text before the composition (all of it when none goes on)
 * is handed on as a KL_WINDOW_TEXT_COMMIT (or into the title bar's field
 * while it is edited) and leaves the store, and the composition is a
 * KL_WINDOW_TEXT_PREEDIT with its cursor (the selection's end) in UTF-8
 * bytes.  The store's text extent is the caret's rectangle, where the
 * candidates' window is put.  While text is wanted the frame's focus is
 * the window's document (ITfThreadMgr::AssociateFocus), otherwise none,
 * which turns the input method off for the frame.  A composition that
 * loses its place (the text is no longer wanted, the field's editing
 * starts or ends) is ended and dropped.  The surrounding text
 * (kl_window_text_context) is not in the store, so reconversion of text
 * already written is not offered.
 *
 * IMM32, when TSF cannot be started (no apartment-threaded COM on the
 * thread, no TSF): while the program asks for text (kl_window_text_input)
 * or the title bar's field is edited, the frame has the input method's
 * default context (ImmAssociateContextEx); otherwise it has none.  The
 * composition is libkeiland's to show: the system's composition window is
 * not shown (WM_IME_SETCONTEXT without ISC_SHOWUICOMPOSITIONWINDOW, and
 * WM_IME_COMPOSITION not passed on), the text being composed is a
 * KL_WINDOW_TEXT_PREEDIT (its cursor in UTF-8 bytes) and the text
 * finished a KL_WINDOW_TEXT_COMMIT, or the field's while it is edited.
 * The candidates' window is put at the caret (kl_window_text_cursor's
 * rectangle, or the field's caret) and kept off it.  Both ways keep the
 * IMM32 context's place, which an input method of the old kind reads.
 */

#define COBJMACROS
#include "userland/desktop/libkeiland/win32/window-win32.h"

#include <imm.h>
#include <msctf.h>
#include <olectl.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* The longest composition read (UTF-16 units), and the most the text store holds. */
#define IME_UNITS	1024U

/* The text store's one view. */
#define IME_VIEW	1U

/* Where the text the input method writes goes: nowhere, the program, or the title bar's field. */
#define IME_TARGET_NONE		0U
#define IME_TARGET_PROGRAM	1U
#define IME_TARGET_FIELD	2U

/*
 * libkeiland's text store: its two interfaces, its references, its window
 * (NULL once the window closed), the sink TSF advised (with the changes it
 * wants told), the lock granted now and the one asked for while it was
 * (TS_LF_* bits, 0 for none), the text and its selection (UTF-16 offsets),
 * whether a composition goes on and its range, whether the text or the
 * selection changed since the last hand-on, whether a composition is
 * shown, whether the hand-on drops the text instead (a composition that
 * lost its place) and whether one is under way, and where the text goes
 * (IME_TARGET_*).
 */
struct ime_store {
	ITextStoreACP store;
	ITfContextOwnerCompositionSink composition;
	LONG references;
	struct kl_window *window;
	ITextStoreACPSink *sink;
	DWORD sink_mask;
	DWORD lock;
	DWORD queued;
	WCHAR text[IME_UNITS];
	LONG length;
	LONG selection_start;
	LONG selection_end;
	int composing;
	LONG composition_start;
	LONG composition_end;
	int changed;
	int shown;
	int discarding;
	int handing;
	unsigned target;
};

/*
 * A window's TSF: the thread's manager and the client id it gave, the
 * document, its context and edit cookie, the text store, whether this
 * window started COM on the thread, whether the frame's focus is the
 * document.
 */
struct keiui_win32_tsf {
	ITfThreadMgr *threads;
	TfClientId client;
	ITfDocumentMgr *document;
	ITfContext *context;
	TfEditCookie cookie;
	struct ime_store *store;
	int com;
	int focused;
};

static int ime_string(HIMC context, DWORD kind, char *text, size_t size, LONG cursor, size_t *offset);
static void ime_copy(char *to, size_t size, const char *from);
static void ime_caret(const struct kl_window *window, RECT *caret);
static void ime_tsf_place(struct kl_window *window);
static void ime_tsf_end(struct keiui_win32_tsf *tsf);
static struct ime_store *ime_store_of(ITextStoreACP *iface);
static struct ime_store *ime_store_of_sink(ITfContextOwnerCompositionSink *iface);
static void ime_store_hand_on(struct ime_store *store);
static void ime_store_commit(struct ime_store *store, LONG count);
static void ime_store_preedit(struct ime_store *store);
static void ime_store_range(struct ime_store *store, ITfCompositionView *view, ITfRange *range);
static HRESULT ime_store_replace(struct ime_store *store, LONG start, LONG end, const WCHAR *text, ULONG count, TS_TEXTCHANGE *change);
static HRESULT STDMETHODCALLTYPE store_query(ITextStoreACP *iface, REFIID id, void **out);
static ULONG STDMETHODCALLTYPE store_add(ITextStoreACP *iface);
static ULONG STDMETHODCALLTYPE store_release(ITextStoreACP *iface);
static HRESULT STDMETHODCALLTYPE store_advise(ITextStoreACP *iface, REFIID id, IUnknown *sink, DWORD mask);
static HRESULT STDMETHODCALLTYPE store_unadvise(ITextStoreACP *iface, IUnknown *sink);
static HRESULT STDMETHODCALLTYPE store_lock(ITextStoreACP *iface, DWORD flags, HRESULT *session);
static HRESULT STDMETHODCALLTYPE store_status(ITextStoreACP *iface, TS_STATUS *status);
static HRESULT STDMETHODCALLTYPE store_query_insert(ITextStoreACP *iface, LONG start, LONG end, ULONG count, LONG *result_start, LONG *result_end);
static HRESULT STDMETHODCALLTYPE store_selection(ITextStoreACP *iface, ULONG index, ULONG count, TS_SELECTION_ACP *selection, ULONG *fetched);
static HRESULT STDMETHODCALLTYPE store_select(ITextStoreACP *iface, ULONG count, const TS_SELECTION_ACP *selection);
static HRESULT STDMETHODCALLTYPE store_text(ITextStoreACP *iface, LONG start, LONG end, WCHAR *plain, ULONG plain_size, ULONG *plain_count, TS_RUNINFO *runs, ULONG run_size, ULONG *run_count, LONG *next);
static HRESULT STDMETHODCALLTYPE store_set_text(ITextStoreACP *iface, DWORD flags, LONG start, LONG end, const WCHAR *text, ULONG count, TS_TEXTCHANGE *change);
static HRESULT STDMETHODCALLTYPE store_formatted(ITextStoreACP *iface, LONG start, LONG end, IDataObject **object);
static HRESULT STDMETHODCALLTYPE store_embedded(ITextStoreACP *iface, LONG position, REFGUID service, REFIID id, IUnknown **out);
static HRESULT STDMETHODCALLTYPE store_query_embedded(ITextStoreACP *iface, const GUID *service, const FORMATETC *format, WINBOOL *insertable);
static HRESULT STDMETHODCALLTYPE store_insert_embedded(ITextStoreACP *iface, DWORD flags, LONG start, LONG end, IDataObject *object, TS_TEXTCHANGE *change);
static HRESULT STDMETHODCALLTYPE store_insert(ITextStoreACP *iface, DWORD flags, const WCHAR *text, ULONG count, LONG *start, LONG *end, TS_TEXTCHANGE *change);
static HRESULT STDMETHODCALLTYPE store_insert_embedded_here(ITextStoreACP *iface, DWORD flags, IDataObject *object, LONG *start, LONG *end, TS_TEXTCHANGE *change);
static HRESULT STDMETHODCALLTYPE store_attributes(ITextStoreACP *iface, DWORD flags, ULONG count, const TS_ATTRID *filter);
static HRESULT STDMETHODCALLTYPE store_attributes_at(ITextStoreACP *iface, LONG position, ULONG count, const TS_ATTRID *filter, DWORD flags);
static HRESULT STDMETHODCALLTYPE store_attributes_changing(ITextStoreACP *iface, LONG position, ULONG count, const TS_ATTRID *filter, DWORD flags);
static HRESULT STDMETHODCALLTYPE store_attribute_next(ITextStoreACP *iface, LONG start, LONG halt, ULONG count, const TS_ATTRID *filter, DWORD flags, LONG *next, WINBOOL *found, LONG *offset);
static HRESULT STDMETHODCALLTYPE store_attribute_values(ITextStoreACP *iface, ULONG count, TS_ATTRVAL *values, ULONG *fetched);
static HRESULT STDMETHODCALLTYPE store_end(ITextStoreACP *iface, LONG *end);
static HRESULT STDMETHODCALLTYPE store_view(ITextStoreACP *iface, TsViewCookie *view);
static HRESULT STDMETHODCALLTYPE store_at_point(ITextStoreACP *iface, TsViewCookie view, const POINT *point, DWORD flags, LONG *position);
static HRESULT STDMETHODCALLTYPE store_extent(ITextStoreACP *iface, TsViewCookie view, LONG start, LONG end, RECT *rectangle, WINBOOL *clipped);
static HRESULT STDMETHODCALLTYPE store_screen(ITextStoreACP *iface, TsViewCookie view, RECT *rectangle);
static HRESULT STDMETHODCALLTYPE store_window(ITextStoreACP *iface, TsViewCookie view, HWND *handle);
static HRESULT STDMETHODCALLTYPE sink_query(ITfContextOwnerCompositionSink *iface, REFIID id, void **out);
static ULONG STDMETHODCALLTYPE sink_add(ITfContextOwnerCompositionSink *iface);
static ULONG STDMETHODCALLTYPE sink_release(ITfContextOwnerCompositionSink *iface);
static HRESULT STDMETHODCALLTYPE sink_start(ITfContextOwnerCompositionSink *iface, ITfCompositionView *view, WINBOOL *ok);
static HRESULT STDMETHODCALLTYPE sink_update(ITfContextOwnerCompositionSink *iface, ITfCompositionView *view, ITfRange *range);
static HRESULT STDMETHODCALLTYPE sink_end(ITfContextOwnerCompositionSink *iface, ITfCompositionView *view);

/* The text store's calls; constant for the life of the program. */
static ITextStoreACPVtbl ime_store_calls = {
	.QueryInterface = store_query,
	.AddRef = store_add,
	.Release = store_release,
	.AdviseSink = store_advise,
	.UnadviseSink = store_unadvise,
	.RequestLock = store_lock,
	.GetStatus = store_status,
	.QueryInsert = store_query_insert,
	.GetSelection = store_selection,
	.SetSelection = store_select,
	.GetText = store_text,
	.SetText = store_set_text,
	.GetFormattedText = store_formatted,
	.GetEmbedded = store_embedded,
	.QueryInsertEmbedded = store_query_embedded,
	.InsertEmbedded = store_insert_embedded,
	.InsertTextAtSelection = store_insert,
	.InsertEmbeddedAtSelection = store_insert_embedded_here,
	.RequestSupportedAttrs = store_attributes,
	.RequestAttrsAtPosition = store_attributes_at,
	.RequestAttrsTransitioningAtPosition = store_attributes_changing,
	.FindNextAttrTransition = store_attribute_next,
	.RetrieveRequestedAttrs = store_attribute_values,
	.GetEndACP = store_end,
	.GetActiveView = store_view,
	.GetACPFromPoint = store_at_point,
	.GetTextExt = store_extent,
	.GetScreenExt = store_screen,
	.GetWnd = store_window
};

/* The composition sink's calls (the same object's); constant for the life of the program. */
static ITfContextOwnerCompositionSinkVtbl ime_sink_calls = {
	.QueryInterface = sink_query,
	.AddRef = sink_add,
	.Release = sink_release,
	.OnStartComposition = sink_start,
	.OnUpdateComposition = sink_update,
	.OnEndComposition = sink_end
};

/*
 * Asks for text from the input method, or stops asking.
 */
void
kl_window_text_input(
	struct kl_window *window,
	int enabled)
{
	/* A window. */
	if (window == NULL)
		return;

	/* The wish, and the context that follows it. */
	window->text_wanted = enabled != 0;
	keiui_win32_ime_place(window);
}

/*
 * Tells where the caret is (the content's pixels), for the input method's
 * windows.
 */
void
kl_window_text_cursor(
	struct kl_window *window,
	int x,
	int y,
	int width,
	int height)
{
	struct ime_store *store;

	/* A window. */
	if (window == NULL)
		return;

	/* The rectangle, and the windows put there. */
	window->text_caret.left = x;
	window->text_caret.top = y;
	window->text_caret.right = x + width;
	window->text_caret.bottom = y + height;
	keiui_win32_ime_place(window);

	/* TSF's input method hears that the caret moved, when no lock is held. */
	if (window->tsf == NULL)
		return;
	store = window->tsf->store;
	if (store->sink == NULL || store->lock != 0U || (store->sink_mask & TS_AS_LAYOUT_CHANGE) == 0U)
		return;
	(void)ITextStoreACPSink_OnLayoutChange(store->sink, TS_LC_CHANGE, IME_VIEW);
}

/*
 * Takes the text around the caret: kept for nothing yet (the surrounding
 * text is TSF's).
 */
void
kl_window_text_context(
	struct kl_window *window,
	const char *text,
	size_t cursor,
	size_t anchor,
	unsigned hint,
	unsigned purpose)
{
	(void)window;
	(void)text;
	(void)cursor;
	(void)anchor;
	(void)hint;
	(void)purpose;
}

/*
 * Gives the frame the input method's context when text is wanted (or the
 * field is edited), and puts its windows at the caret.
 */
void
keiui_win32_ime_place(
	struct kl_window *window)
{
	COMPOSITIONFORM composition;
	CANDIDATEFORM candidate;
	HIMC context;
	RECT caret;

	/* TSF's focus and composition follow the wish first. */
	ime_tsf_place(window);

	/* No text wanted: no context. */
	if (!window->text_wanted && !window->editing) {
		(void)ImmAssociateContextEx(window->frame, NULL, 0U);
		return;
	}

	/* Gives the frame its default input context back, since text is wanted. */
	(void)ImmAssociateContextEx(window->frame, NULL, IACE_DEFAULT);

	/* The caret in the frame's pixels. */
	ime_caret(window, &caret);

	/* The composition's and the candidates' windows there. */
	context = ImmGetContext(window->frame);
	if (context == NULL)
		return;
	memset(&composition, 0, sizeof(composition));
	composition.dwStyle = CFS_POINT;
	composition.ptCurrentPos.x = caret.left;
	composition.ptCurrentPos.y = caret.top;
	(void)ImmSetCompositionWindow(context, &composition);
	memset(&candidate, 0, sizeof(candidate));
	candidate.dwIndex = 0U;
	candidate.dwStyle = CFS_EXCLUDE;
	candidate.ptCurrentPos.x = caret.left;
	candidate.ptCurrentPos.y = caret.bottom;
	candidate.rcArea = caret;
	(void)ImmSetCandidateWindow(context, &candidate);
	(void)ImmReleaseContext(window->frame, context);
}

/*
 * Starts TSF for a window: the thread's manager, the window's document
 * and its context over a text store of its own.  A window where it cannot
 * be started keeps IMM32 (window->tsf stays NULL).
 */
void
keiui_win32_tsf_open(
	struct kl_window *window)
{
	struct keiui_win32_tsf *tsf;
	struct ime_store *store;
	HRESULT result;

	/* The state, and the store with its first reference (the window's). */
	tsf = calloc(1U, sizeof(*tsf));
	store = calloc(1U, sizeof(*store));
	if (tsf == NULL || store == NULL) {
		free(tsf);
		free(store);
		return;
	}

	/* Joins the store's interfaces and gives the store its first reference. */
	store->store.lpVtbl = &ime_store_calls;
	store->composition.lpVtbl = &ime_sink_calls;
	store->references = 1;
	store->window = window;
	tsf->store = store;

	/* COM on the thread, apartment-threaded as TSF needs (a thread of the other kind keeps IMM32). */
	result = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	if (FAILED(result)) {
		ime_tsf_end(tsf);
		return;
	}

	/* Records that this call started COM, so ending the state ends it. */
	tsf->com = 1;

	/* The thread's manager, started for this client. */
	result = CoCreateInstance(&CLSID_TF_ThreadMgr, NULL, CLSCTX_INPROC_SERVER, &IID_ITfThreadMgr, (void **)&tsf->threads);
	if (FAILED(result)) {
		tsf->threads = NULL;
		ime_tsf_end(tsf);
		return;
	}

	/* Starts the manager for this client. */
	result = ITfThreadMgr_Activate(tsf->threads, &tsf->client);
	if (FAILED(result)) {
		ITfThreadMgr_Release(tsf->threads);
		tsf->threads = NULL;
		ime_tsf_end(tsf);
		return;
	}

	/* The window's document, with one context over the store. */
	result = ITfThreadMgr_CreateDocumentMgr(tsf->threads, &tsf->document);
	if (FAILED(result)) {
		tsf->document = NULL;
		ime_tsf_end(tsf);
		return;
	}

	/* Makes the context over the store. */
	result = ITfDocumentMgr_CreateContext(tsf->document, tsf->client, 0U, (IUnknown *)&store->store, &tsf->context, &tsf->cookie);
	if (FAILED(result)) {
		tsf->context = NULL;
		ime_tsf_end(tsf);
		return;
	}

	/* Makes the context the document's current one. */
	result = ITfDocumentMgr_Push(tsf->document, tsf->context);
	if (FAILED(result)) {
		ime_tsf_end(tsf);
		return;
	}

	/*
	 * The window's input method is TSF's.  The frame starts with no
	 * document (focused set so that the first placing says so): the input
	 * method is off until text is wanted.
	 */
	window->tsf = tsf;
	tsf->focused = 1;
	ime_tsf_place(window);
}

/*
 * Stops a window's TSF (the window closes): its focus, its document and
 * context, the thread's manager, and COM as this window started it.
 */
void
keiui_win32_tsf_close(
	struct kl_window *window)
{
	ITfDocumentMgr *previous;
	struct keiui_win32_tsf *tsf;

	/* A window without TSF. */
	tsf = window->tsf;
	if (tsf == NULL)
		return;
	window->tsf = NULL;

	/* The frame's focus is no document any more. */
	if (tsf->focused) {
		previous = NULL;
		(void)ITfThreadMgr_AssociateFocus(tsf->threads, window->frame, NULL, &previous);
		if (previous != NULL)
			ITfDocumentMgr_Release(previous);
		tsf->focused = 0;
	}

	/* The context comes off the document, and the rest goes. */
	(void)ITfDocumentMgr_Pop(tsf->document, TF_POPF_ALL);
	ime_tsf_end(tsf);
}

/*
 * Handles the input method's messages to the frame.  Returns 1 with
 * *result when it did (the message is not passed on), 0 to pass it on.
 */
int
keiui_win32_ime_message(
	struct kl_window *window,
	UINT message,
	WPARAM wparam,
	LPARAM lparam,
	LRESULT *result)
{
	struct kl_window_event *input;
	char *text;
	HIMC context;
	size_t offset;
	LONG cursor;
	int found;

	/* The word parameter carries nothing the composition messages need here. */
	(void)wparam;

	/* What the message is. */
	switch (message) {
	case WM_IME_SETCONTEXT:
		/* The system shows no composition window of its own. */
		*result = DefWindowProcW(window->frame, message, wparam, lparam & ~(LPARAM)ISC_SHOWUICOMPOSITIONWINDOW);
		return 1;
	case WM_IME_STARTCOMPOSITION:
		/* The composition starts at the caret. */
		keiui_win32_ime_place(window);
		*result = 0;
		return 1;
	case WM_IME_COMPOSITION:
		break;
	case WM_IME_ENDCOMPOSITION:
		/* The composing text goes. */
		if (window->editing) {
			keiui_win32_field_compose(window, "", 0U);
		} else {
			input = keiui_win32_push(window, KL_WINDOW_TEXT_PREEDIT);
			if (input != NULL) {
				input->begin = -1;
				input->end = -1;
			}
		}

		/* Takes the message as handled. */
		*result = 0;
		return 1;
	default:
		return 0;
	}

	/* The composition's text: finished, then being composed. */
	text = malloc(KEIUI_WIN32_CONTROL_TEXT);
	context = ImmGetContext(window->frame);
	if (text == NULL || context == NULL) {
		free(text);
		if (context != NULL)
			(void)ImmReleaseContext(window->frame, context);
		*result = 0;
		return 1;
	}

	/* Takes the finished string when the input method has one. */
	if (lparam & GCS_RESULTSTR) {
		found = ime_string(context, GCS_RESULTSTR, text, KEIUI_WIN32_CONTROL_TEXT, -1, &offset);
		if (found && window->editing) {
			keiui_win32_field_insert(window, text);
		} else if (found) {
			input = keiui_win32_push(window, KL_WINDOW_TEXT_COMMIT);
			if (input != NULL)
				ime_copy(input->text, sizeof(input->text), text);
		}
	}

	/* Takes the string still being composed, with its cursor. */
	if (lparam & GCS_COMPSTR) {
		cursor = -1;
		if (lparam & GCS_CURSORPOS)
			cursor = ImmGetCompositionStringW(context, GCS_CURSORPOS, NULL, 0U);
		found = ime_string(context, GCS_COMPSTR, text, KEIUI_WIN32_CONTROL_TEXT, cursor, &offset);
		if (!found)
			text[0] = '\0';
		if (window->editing) {
			keiui_win32_field_compose(window, text, offset);
		} else {
			input = keiui_win32_push(window, KL_WINDOW_TEXT_PREEDIT);
			if (input != NULL) {
				ime_copy(input->text, sizeof(input->text), text);
				input->begin = (int32_t)offset;
				input->end = (int32_t)offset;
			}
		}
	}

	/* Gives the input context back and releases the text. */
	(void)ImmReleaseContext(window->frame, context);
	free(text);

	/* Handled: the system's composition window stays away. */
	*result = 0;
	return 1;
}

/* One of the composition's strings as UTF-8, and a cursor (UTF-16 units, -1 for none) as its UTF-8 offset; 1 when read. */
static int
ime_string(
	HIMC context,
	DWORD kind,
	char *text,
	size_t size,
	LONG cursor,
	size_t *offset)
{
	uint16_t units[IME_UNITS];
	LONG bytes;
	size_t count;
	size_t consumed;
	int error;

	/* The string's units. */
	text[0] = '\0';
	*offset = 0U;
	bytes = ImmGetCompositionStringW(context, kind, units, sizeof(units));
	if (bytes < 0)
		return 0;
	count = (size_t)bytes / sizeof(units[0]);

	/* The cursor's offset: the UTF-8 of the units before it. */
	if (cursor >= 0 && (size_t)cursor <= count) {
		error = keiui_win32_utf8_from_utf16(units, (size_t)cursor, text, size, &consumed);
		if (error == 0)
			*offset = strlen(text);
	}

	/* The whole string. */
	(void)keiui_win32_utf8_from_utf16(units, count, text, size, &consumed);
	if (cursor < 0)
		*offset = strlen(text);
	return 1;
}

/* Copies UTF-8 text, as many whole characters as fit. */
static void
ime_copy(
	char *to,
	size_t size,
	const char *from)
{
	size_t length;

	/* As much as fits, not cutting a character. */
	length = strlen(from);
	if (length + 1U > size) {
		length = size - 1U;
		while (length > 0U && ((unsigned char)from[length] & 0xc0U) == 0x80U)
			length--;
	}

	/* Copies the characters that fit and ends the text. */
	memcpy(to, from, length);
	to[length] = '\0';
}

/* The caret in the frame's pixels: the field's while it is edited, or the program's below the title bar. */
static void
ime_caret(
	const struct kl_window *window,
	RECT *caret)
{
	const struct keiui_win32_control *control;
	uint32_t caption;

	/* The program's rectangle, moved below the title bar. */
	caption = keiui_win32_caption_pixels(window);
	*caret = window->text_caret;
	caret->top += (LONG)caption;
	caret->bottom += (LONG)caption;

	/* The field's box while it is edited. */
	if (window->editing && window->field.control < window->control_count) {
		control = &window->controls[window->field.control];
		caret->left = control->x;
		caret->top = control->y;
		caret->right = control->x + (LONG)control->width;
		caret->bottom = control->y + (LONG)control->height;
	}
}

/*
 * Makes TSF follow where text goes: the frame's focus is the window's
 * document while text is wanted, none otherwise, and a composition whose
 * place changed is ended and dropped.
 */
static void
ime_tsf_place(
	struct kl_window *window)
{
	ITfContextOwnerCompositionServices *services;
	ITfDocumentMgr *previous;
	struct keiui_win32_tsf *tsf;
	struct ime_store *store;
	unsigned target;
	HRESULT result;

	/* A window without TSF. */
	tsf = window->tsf;
	if (tsf == NULL)
		return;
	store = tsf->store;

	/* Where the text goes now. */
	target = IME_TARGET_NONE;
	if (window->text_wanted)
		target = IME_TARGET_PROGRAM;
	if (window->editing)
		target = IME_TARGET_FIELD;

	/*
	 * A composition of the old place is ended by the input method and its
	 * text dropped (discarding makes the hand-on throw it away), so that
	 * it never lands where it was not typed.
	 */
	if (target != store->target && store->composing) {
		result = ITfContext_QueryInterface(tsf->context, &IID_ITfContextOwnerCompositionServices, (void **)&services);
		if (SUCCEEDED(result)) {
			store->discarding = 1;
			(void)ITfContextOwnerCompositionServices_TerminateComposition(services, NULL);
			ITfContextOwnerCompositionServices_Release(services);
			store->discarding = 0;
		}
	}

	/* Remembers where the text goes now. */
	store->target = target;

	/* The frame's focus: the document while text is wanted, none otherwise. */
	if (target != IME_TARGET_NONE && !tsf->focused) {
		previous = NULL;
		result = ITfThreadMgr_AssociateFocus(tsf->threads, window->frame, tsf->document, &previous);
		if (previous != NULL)
			ITfDocumentMgr_Release(previous);
		if (SUCCEEDED(result))
			tsf->focused = 1;
	} else if (target == IME_TARGET_NONE && tsf->focused) {
		previous = NULL;
		(void)ITfThreadMgr_AssociateFocus(tsf->threads, window->frame, NULL, &previous);
		if (previous != NULL)
			ITfDocumentMgr_Release(previous);
		tsf->focused = 0;
	}
}

/* Releases what a window's TSF holds, whatever was made of it, and frees it. */
static void
ime_tsf_end(
	struct keiui_win32_tsf *tsf)
{
	struct ime_store *store;

	/* The context and the document. */
	if (tsf->context != NULL)
		ITfContext_Release(tsf->context);
	if (tsf->document != NULL)
		ITfDocumentMgr_Release(tsf->document);

	/* The manager, stopped for this client. */
	if (tsf->threads != NULL) {
		(void)ITfThreadMgr_Deactivate(tsf->threads);
		ITfThreadMgr_Release(tsf->threads);
	}

	/* The store forgets its window; TSF may hold it a little longer. */
	store = tsf->store;
	if (store != NULL) {
		store->window = NULL;
		ITextStoreACP_Release(&store->store);
	}

	/* COM, as this window started it, and the state. */
	if (tsf->com)
		CoUninitialize();
	free(tsf);
}

/* The store of its ITextStoreACP. */
static struct ime_store *
ime_store_of(
	ITextStoreACP *iface)
{
	/* The interface is the store's first member. */
	return (struct ime_store *)iface;
}

/* The store of its composition sink. */
static struct ime_store *
ime_store_of_sink(
	ITfContextOwnerCompositionSink *iface)
{
	/* The sink's member, back to the whole store. */
	return (struct ime_store *)(void *)((char *)iface - offsetof(struct ime_store, composition));
}

/*
 * Hands on what the input method wrote after a lock: the finished text
 * (before the composition, or all when none goes on) to the program or
 * the field, leaving the store, then the composition as the text being
 * composed.  TSF hears that the store lost the finished text.
 */
static void
ime_store_hand_on(
	struct ime_store *store)
{
	TS_TEXTCHANGE change;
	LONG finished;

	/* Nothing changed, a hand-on under way, or no window any more. */
	if (!store->changed || store->handing || store->window == NULL)
		return;
	store->handing = 1;
	store->changed = 0;

	/* The finished text: before the composition, or all of it. */
	finished = store->length;
	if (store->composing)
		finished = store->composition_start;
	if (finished < 0)
		finished = 0;
	if (finished > store->length)
		finished = store->length;

	/* Handed on (or dropped), then out of the store with the offsets after it. */
	if (finished > 0) {
		if (!store->discarding)
			ime_store_commit(store, finished);
		memmove(store->text, store->text + finished, (size_t)(store->length - finished) * sizeof(store->text[0]));
		store->length -= finished;
		store->selection_start -= finished;
		store->selection_end -= finished;
		if (store->selection_start < 0)
			store->selection_start = 0;
		if (store->selection_end < 0)
			store->selection_end = 0;
		store->composition_start -= finished;
		store->composition_end -= finished;
	}

	/* The composition shown, or what was shown taken away. */
	ime_store_preedit(store);

	/*
	 * TSF hears that the finished text left, outside any lock; it may ask
	 * for a lock at once, whose own hand-on waits for this one's end.
	 */
	if (finished > 0 && store->sink != NULL) {
		change.acpStart = 0;
		change.acpOldEnd = finished;
		change.acpNewEnd = 0;
		if ((store->sink_mask & TS_AS_TEXT_CHANGE) != 0U)
			(void)ITextStoreACPSink_OnTextChange(store->sink, 0U, &change);
		if ((store->sink_mask & TS_AS_SEL_CHANGE) != 0U)
			(void)ITextStoreACPSink_OnSelectionChange(store->sink);
	}

	/* Ends the handing of the change to the sink. */
	store->handing = 0;

	/* A lock granted meanwhile changed the store again: handed on too. */
	if (store->changed)
		ime_store_hand_on(store);
}

/* Hands on the store's first count units as finished text: to the field while it is edited, or to the program. */
static void
ime_store_commit(
	struct ime_store *store,
	LONG count)
{
	struct kl_window_event *input;
	struct kl_window *window;
	size_t consumed;
	char *text;
	int error;

	/* The text as UTF-8. */
	window = store->window;
	text = malloc(KEIUI_WIN32_CONTROL_TEXT);
	if (text == NULL)
		return;
	error = keiui_win32_utf8_from_utf16((const uint16_t *)store->text, (size_t)count, text, KEIUI_WIN32_CONTROL_TEXT, &consumed);
	if (error != 0) {
		free(text);
		return;
	}

	/* Into the field, or the program's input (nowhere when neither wants it). */
	if (store->target == IME_TARGET_FIELD) {
		keiui_win32_field_insert(window, text);
	} else if (store->target == IME_TARGET_PROGRAM) {
		input = keiui_win32_push(window, KL_WINDOW_TEXT_COMMIT);
		if (input != NULL)
			ime_copy(input->text, sizeof(input->text), text);
	}

	/* Releases the text read from the input method. */
	free(text);
}

/* Shows the composition with its cursor (the selection's end), or takes away the one shown when none goes on. */
static void
ime_store_preedit(
	struct ime_store *store)
{
	struct kl_window_event *input;
	struct kl_window *window;
	LONG start;
	LONG end;
	LONG cursor;
	size_t offset;
	size_t consumed;
	char *text;
	int error;

	/* No composition: what was shown goes. */
	window = store->window;
	if (!store->composing || store->discarding) {
		if (!store->shown)
			return;
		store->shown = 0;
		if (store->target == IME_TARGET_FIELD) {
			keiui_win32_field_compose(window, "", 0U);
			return;
		}

		/* Shows an empty pre-edit text. */
		input = keiui_win32_push(window, KL_WINDOW_TEXT_PREEDIT);
		if (input != NULL) {
			input->begin = -1;
			input->end = -1;
		}

		/* Stops here: the composition has no text. */
		return;
	}

	/* The composition's range, inside the store, and its cursor inside it. */
	start = store->composition_start;
	end = store->composition_end;
	if (start < 0)
		start = 0;
	if (end > store->length)
		end = store->length;
	if (end < start)
		end = start;
	cursor = store->selection_end;
	if (cursor < start)
		cursor = start;
	if (cursor > end)
		cursor = end;

	/* Its text as UTF-8, and the cursor's byte offset. */
	text = malloc(KEIUI_WIN32_CONTROL_TEXT);
	if (text == NULL)
		return;
	offset = 0U;
	error = keiui_win32_utf8_from_utf16((const uint16_t *)store->text + start, (size_t)(cursor - start), text, KEIUI_WIN32_CONTROL_TEXT, &consumed);
	if (error == 0)
		offset = strlen(text);
	error = keiui_win32_utf8_from_utf16((const uint16_t *)store->text + start, (size_t)(end - start), text, KEIUI_WIN32_CONTROL_TEXT, &consumed);
	if (error != 0)
		text[0] = '\0';

	/* Into the field, or the program's input. */
	store->shown = 1;
	if (store->target == IME_TARGET_FIELD) {
		keiui_win32_field_compose(window, text, offset);
	} else {
		input = keiui_win32_push(window, KL_WINDOW_TEXT_PREEDIT);
		if (input != NULL) {
			ime_copy(input->text, sizeof(input->text), text);
			input->begin = (int32_t)offset;
			input->end = (int32_t)offset;
		}
	}

	/* Releases the text read from the input method. */
	free(text);
}

/* Reads the composition's range in the store from the range given, or from the composition's own. */
static void
ime_store_range(
	struct ime_store *store,
	ITfCompositionView *view,
	ITfRange *range)
{
	ITfRangeACP *offsets;
	ITfRange *own;
	LONG start;
	LONG length;
	HRESULT result;

	/* The range: the one given, or the composition's. */
	own = NULL;
	if (range == NULL) {
		result = ITfCompositionView_GetRange(view, &own);
		if (FAILED(result))
			return;
		range = own;
	}

	/* Its offsets in the store. */
	result = ITfRange_QueryInterface(range, &IID_ITfRangeACP, (void **)&offsets);
	if (SUCCEEDED(result)) {
		result = ITfRangeACP_GetExtent(offsets, &start, &length);
		if (SUCCEEDED(result)) {
			store->composition_start = start;
			store->composition_end = start + length;
		}

		/* Releases the range's interface. */
		ITfRangeACP_Release(offsets);
	}

	/* Releases the range taken for the view. */
	if (own != NULL)
		ITfRange_Release(own);
}

/* Replaces a range of the store with text, the selection after it; S_OK, TS_E_INVALIDPOS or E_OUTOFMEMORY. */
static HRESULT
ime_store_replace(
	struct ime_store *store,
	LONG start,
	LONG end,
	const WCHAR *text,
	ULONG count,
	TS_TEXTCHANGE *change)
{
	LONG length;

	/* A range inside the store, and room for the text. */
	if (start < 0 || end < start || end > store->length)
		return TS_E_INVALIDPOS;
	if (count > (ULONG)IME_UNITS)
		return E_OUTOFMEMORY;
	length = store->length - (end - start) + (LONG)count;
	if (length > (LONG)IME_UNITS)
		return E_OUTOFMEMORY;

	/* The text after the range moves, and the new text goes in. */
	memmove(store->text + start + count, store->text + end, (size_t)(store->length - end) * sizeof(store->text[0]));
	if (count != 0U)
		memcpy(store->text + start, text, (size_t)count * sizeof(store->text[0]));
	store->length = length;
	store->selection_start = start + (LONG)count;
	store->selection_end = start + (LONG)count;
	store->changed = 1;

	/* What changed. */
	if (change != NULL) {
		change->acpStart = start;
		change->acpOldEnd = end;
		change->acpNewEnd = start + (LONG)count;
	}

	/* Succeeded: the store has the text. */
	return S_OK;
}

/* Gives the store's interfaces. */
static HRESULT STDMETHODCALLTYPE
store_query(
	ITextStoreACP *iface,
	REFIID id,
	void **out)
{
	struct ime_store *store;
	BOOL is_store;
	BOOL is_composition;

	/* Somewhere to put it. */
	if (out == NULL)
		return E_POINTER;
	*out = NULL;
	store = ime_store_of(iface);

	/* Tells which of the two interfaces the caller asks for. */
	is_store = IsEqualIID(id, &IID_IUnknown);
	if (!is_store)
		is_store = IsEqualIID(id, &IID_ITextStoreACP);
	is_composition = IsEqualIID(id, &IID_ITfContextOwnerCompositionSink);

	/* The store's, or the composition sink's. */
	if (is_store) {
		*out = &store->store;
	} else if (is_composition) {
		*out = &store->composition;
	} else {
		return E_NOINTERFACE;
	}

	/* Succeeded: one more reference. */
	(void)InterlockedIncrement(&store->references);
	return S_OK;
}

/* Takes a reference. */
static ULONG STDMETHODCALLTYPE
store_add(
	ITextStoreACP *iface)
{
	struct ime_store *store;
	LONG count;

	/* One more. */
	store = ime_store_of(iface);
	count = InterlockedIncrement(&store->references);
	return (ULONG)count;
}

/* Lets a reference go; the last frees the store. */
static ULONG STDMETHODCALLTYPE
store_release(
	ITextStoreACP *iface)
{
	struct ime_store *store;
	LONG count;

	/* One fewer. */
	store = ime_store_of(iface);
	count = InterlockedDecrement(&store->references);
	if (count != 0)
		return (ULONG)count;

	/* The last: the sink and the store go. */
	if (store->sink != NULL)
		ITextStoreACPSink_Release(store->sink);
	free(store);
	return 0U;
}

/* Keeps TSF's sink and the changes it wants told; one sink at a time. */
static HRESULT STDMETHODCALLTYPE
store_advise(
	ITextStoreACP *iface,
	REFIID id,
	IUnknown *sink,
	DWORD mask)
{
	ITextStoreACPSink *wanted;
	IUnknown *kept;
	IUnknown *given;
	struct ime_store *store;
	HRESULT result;
	BOOL is_sink;

	/* Only the text store's sink. */
	store = ime_store_of(iface);
	if (sink == NULL)
		return E_INVALIDARG;
	is_sink = IsEqualIID(id, &IID_ITextStoreACPSink);
	if (!is_sink)
		return E_INVALIDARG;

	/* The same sink again changes only its mask; another is refused while one is kept. */
	if (store->sink != NULL) {
		result = ITextStoreACPSink_QueryInterface(store->sink, &IID_IUnknown, (void **)&kept);
		if (FAILED(result))
			return result;
		result = IUnknown_QueryInterface(sink, &IID_IUnknown, (void **)&given);
		if (FAILED(result)) {
			IUnknown_Release(kept);
			return result;
		}

		/* Releases the two identities; they are compared next. */
		IUnknown_Release(kept);
		IUnknown_Release(given);
		if (kept != given)
			return CONNECT_E_ADVISELIMIT;
		store->sink_mask = mask;
		return S_OK;
	}

	/* The sink, kept. */
	result = IUnknown_QueryInterface(sink, &IID_ITextStoreACPSink, (void **)&wanted);
	if (FAILED(result))
		return E_INVALIDARG;
	store->sink = wanted;
	store->sink_mask = mask;

	/* Succeeded: TSF hears the store's changes. */
	return S_OK;
}

/* Lets TSF's sink go. */
static HRESULT STDMETHODCALLTYPE
store_unadvise(
	ITextStoreACP *iface,
	IUnknown *sink)
{
	struct ime_store *store;

	/* The sink the caller names is not needed: the store keeps only one. */
	(void)sink;

	/* The sink kept, if any. */
	store = ime_store_of(iface);
	if (store->sink == NULL)
		return CONNECT_E_NOCONNECTION;
	ITextStoreACPSink_Release(store->sink);
	store->sink = NULL;
	store->sink_mask = 0U;

	/* Succeeded: no sink. */
	return S_OK;
}

/*
 * Grants TSF a lock on the store: at once when none is held, or after the
 * one held when it may wait (TS_S_ASYNC); one that must not wait is
 * refused then (TS_E_SYNCHRONOUS).  What the input method wrote is handed
 * on after the locks.
 */
static HRESULT STDMETHODCALLTYPE
store_lock(
	ITextStoreACP *iface,
	DWORD flags,
	HRESULT *session)
{
	struct ime_store *store;
	DWORD lock;

	/* A sink to grant it to. */
	store = ime_store_of(iface);
	if (session == NULL)
		return E_INVALIDARG;
	if (store->sink == NULL)
		return E_UNEXPECTED;

	/* A lock is held: refused when it must not wait, else queued (a write wins over a read). */
	if (store->lock != 0U) {
		if ((flags & TS_LF_SYNC) != 0U) {
			*session = TS_E_SYNCHRONOUS;
			return S_OK;
		}

		/* Queues the strongest lock asked for while this one is held. */
		if ((flags & TS_LF_READWRITE) == TS_LF_READWRITE)
			store->queued = TS_LF_READWRITE;
		else if (store->queued == 0U)
			store->queued = TS_LF_READ;
		*session = TS_S_ASYNC;
		return S_OK;
	}

	/* Granted now, then the ones asked for meanwhile. */
	store->lock = flags & TS_LF_READWRITE;
	*session = ITextStoreACPSink_OnLockGranted(store->sink, store->lock);
	store->lock = 0U;
	while (store->queued != 0U && store->sink != NULL) {
		lock = store->queued;
		store->queued = 0U;
		store->lock = lock;
		(void)ITextStoreACPSink_OnLockGranted(store->sink, lock);
		store->lock = 0U;
	}

	/* What the input method wrote, handed on. */
	ime_store_hand_on(store);
	return S_OK;
}

/* Reports the store's status: plain text, never hidden. */
static HRESULT STDMETHODCALLTYPE
store_status(
	ITextStoreACP *iface,
	TS_STATUS *status)
{
	(void)iface;

	/* Somewhere to put it. */
	if (status == NULL)
		return E_INVALIDARG;

	/* No dynamic state, no hidden text. */
	status->dwDynamicFlags = 0U;
	status->dwStaticFlags = TS_SS_NOHIDDENTEXT;
	return S_OK;
}

/* Tells where text would go in place of a range: the range itself. */
static HRESULT STDMETHODCALLTYPE
store_query_insert(
	ITextStoreACP *iface,
	LONG start,
	LONG end,
	ULONG count,
	LONG *result_start,
	LONG *result_end)
{
	struct ime_store *store;

	/* The number of characters is not needed here. */
	(void)count;

	/* A range inside the store. */
	store = ime_store_of(iface);
	if (result_start == NULL || result_end == NULL)
		return E_INVALIDARG;
	if (start < 0 || end < start || end > store->length)
		return E_INVALIDARG;

	/* The same range. */
	*result_start = start;
	*result_end = end;
	return S_OK;
}

/* Reports the selection (the store has one), under a lock. */
static HRESULT STDMETHODCALLTYPE
store_selection(
	ITextStoreACP *iface,
	ULONG index,
	ULONG count,
	TS_SELECTION_ACP *selection,
	ULONG *fetched)
{
	struct ime_store *store;

	/* A read lock, and the one selection. */
	store = ime_store_of(iface);
	if (selection == NULL || fetched == NULL)
		return E_INVALIDARG;
	*fetched = 0U;
	if ((store->lock & TS_LF_READ) == 0U)
		return TS_E_NOLOCK;
	if (index != (ULONG)TF_DEFAULT_SELECTION && index != 0U)
		return TS_E_NOSELECTION;
	if (count == 0U)
		return S_OK;

	/* The selection, its active end at its end. */
	selection[0].acpStart = store->selection_start;
	selection[0].acpEnd = store->selection_end;
	selection[0].style.ase = TS_AE_END;
	selection[0].style.fInterimChar = FALSE;
	*fetched = 1U;
	return S_OK;
}

/* Sets the selection, under a write lock. */
static HRESULT STDMETHODCALLTYPE
store_select(
	ITextStoreACP *iface,
	ULONG count,
	const TS_SELECTION_ACP *selection)
{
	struct ime_store *store;

	/* A write lock and a selection inside the store. */
	store = ime_store_of(iface);
	if ((store->lock & TS_LF_READWRITE) != TS_LF_READWRITE)
		return TS_E_NOLOCK;
	if (count == 0U || selection == NULL)
		return E_INVALIDARG;
	if (selection[0].acpStart < 0 || selection[0].acpEnd < selection[0].acpStart || selection[0].acpEnd > store->length)
		return TS_E_INVALIDPOS;

	/* The first selection (the store has one). */
	store->selection_start = selection[0].acpStart;
	store->selection_end = selection[0].acpEnd;
	store->changed = 1;
	return S_OK;
}

/* Copies text of the store, as one plain run, under a lock. */
static HRESULT STDMETHODCALLTYPE
store_text(
	ITextStoreACP *iface,
	LONG start,
	LONG end,
	WCHAR *plain,
	ULONG plain_size,
	ULONG *plain_count,
	TS_RUNINFO *runs,
	ULONG run_size,
	ULONG *run_count,
	LONG *next)
{
	struct ime_store *store;
	ULONG count;

	/* A read lock and a range inside the store (an end of -1 is the store's end). */
	store = ime_store_of(iface);
	if (plain_count == NULL || run_count == NULL || next == NULL)
		return E_INVALIDARG;
	*plain_count = 0U;
	*run_count = 0U;
	*next = start;
	if ((store->lock & TS_LF_READ) == 0U)
		return TS_E_NOLOCK;
	if (end == -1)
		end = store->length;
	if (start < 0 || end < start || end > store->length)
		return TS_E_INVALIDPOS;

	/* As much as the caller has room for. */
	count = (ULONG)(end - start);
	if (count > plain_size)
		count = plain_size;
	if (plain != NULL && count != 0U)
		memcpy(plain, store->text + start, (size_t)count * sizeof(store->text[0]));
	if (plain == NULL)
		count = 0U;
	*plain_count = count;

	/* One plain run over it. */
	if (runs != NULL && run_size != 0U && count != 0U) {
		runs[0].uCount = count;
		runs[0].type = TS_RT_PLAIN;
		*run_count = 1U;
	}

	/* Reports where the next read starts. */
	*next = start + (LONG)count;
	return S_OK;
}

/* Replaces a range with text, under a write lock. */
static HRESULT STDMETHODCALLTYPE
store_set_text(
	ITextStoreACP *iface,
	DWORD flags,
	LONG start,
	LONG end,
	const WCHAR *text,
	ULONG count,
	TS_TEXTCHANGE *change)
{
	struct ime_store *store;
	HRESULT result;

	/* The flags carry nothing this store needs. */
	(void)flags;

	/* A write lock. */
	store = ime_store_of(iface);
	if ((store->lock & TS_LF_READWRITE) != TS_LF_READWRITE)
		return TS_E_NOLOCK;
	if (text == NULL && count != 0U)
		return E_INVALIDARG;

	/* The range replaced. */
	result = ime_store_replace(store, start, end, text, count, change);
	if (FAILED(result))
		return result;

	/* Succeeded: the store has the text. */
	return S_OK;
}

/* Gives no formatted text (the store is plain). */
static HRESULT STDMETHODCALLTYPE
store_formatted(
	ITextStoreACP *iface,
	LONG start,
	LONG end,
	IDataObject **object)
{
	(void)iface;
	(void)start;
	(void)end;

	/* None. */
	if (object != NULL)
		*object = NULL;
	return E_NOTIMPL;
}

/* Gives no embedded object (the store has none). */
static HRESULT STDMETHODCALLTYPE
store_embedded(
	ITextStoreACP *iface,
	LONG position,
	REFGUID service,
	REFIID id,
	IUnknown **out)
{
	(void)iface;
	(void)position;
	(void)service;
	(void)id;

	/* None. */
	if (out != NULL)
		*out = NULL;
	return E_NOTIMPL;
}

/* Tells that no object can be embedded. */
static HRESULT STDMETHODCALLTYPE
store_query_embedded(
	ITextStoreACP *iface,
	const GUID *service,
	const FORMATETC *format,
	WINBOOL *insertable)
{
	(void)iface;
	(void)service;
	(void)format;

	/* Somewhere to put it. */
	if (insertable == NULL)
		return E_INVALIDARG;

	/* Never. */
	*insertable = FALSE;
	return S_OK;
}

/* Embeds no object. */
static HRESULT STDMETHODCALLTYPE
store_insert_embedded(
	ITextStoreACP *iface,
	DWORD flags,
	LONG start,
	LONG end,
	IDataObject *object,
	TS_TEXTCHANGE *change)
{
	(void)iface;
	(void)flags;
	(void)start;
	(void)end;
	(void)object;
	(void)change;

	/* Not offered. */
	return E_NOTIMPL;
}

/* Puts text in place of the selection (or tells where it would go), under a lock. */
static HRESULT STDMETHODCALLTYPE
store_insert(
	ITextStoreACP *iface,
	DWORD flags,
	const WCHAR *text,
	ULONG count,
	LONG *start,
	LONG *end,
	TS_TEXTCHANGE *change)
{
	struct ime_store *store;
	LONG from;
	LONG to;
	HRESULT result;

	/* Where it would go: the selection, under a read lock. */
	store = ime_store_of(iface);
	from = store->selection_start;
	to = store->selection_end;
	if ((flags & TF_IAS_QUERYONLY) != 0U) {
		if ((store->lock & TS_LF_READ) == 0U)
			return TS_E_NOLOCK;
		if (start != NULL)
			*start = from;
		if (end != NULL)
			*end = to;
		return S_OK;
	}

	/* Put there, under a write lock. */
	if ((store->lock & TS_LF_READWRITE) != TS_LF_READWRITE)
		return TS_E_NOLOCK;
	if (text == NULL && count != 0U)
		return E_INVALIDARG;
	result = ime_store_replace(store, from, to, text, count, change);
	if (FAILED(result))
		return result;

	/* Succeeded: where the text went, unless it was not asked for. */
	if ((flags & TF_IAS_NOQUERY) == 0U) {
		if (start != NULL)
			*start = from;
		if (end != NULL)
			*end = from + (LONG)count;
	}

	/* Succeeded: the text is in the store. */
	return S_OK;
}

/* Embeds no object at the selection. */
static HRESULT STDMETHODCALLTYPE
store_insert_embedded_here(
	ITextStoreACP *iface,
	DWORD flags,
	IDataObject *object,
	LONG *start,
	LONG *end,
	TS_TEXTCHANGE *change)
{
	(void)iface;
	(void)flags;
	(void)object;
	(void)start;
	(void)end;
	(void)change;

	/* Not offered. */
	return E_NOTIMPL;
}

/* Asks for the attributes the store supports: none. */
static HRESULT STDMETHODCALLTYPE
store_attributes(
	ITextStoreACP *iface,
	DWORD flags,
	ULONG count,
	const TS_ATTRID *filter)
{
	(void)iface;
	(void)flags;
	(void)count;
	(void)filter;

	/* None to retrieve. */
	return S_OK;
}

/* Asks for the attributes at a place: none. */
static HRESULT STDMETHODCALLTYPE
store_attributes_at(
	ITextStoreACP *iface,
	LONG position,
	ULONG count,
	const TS_ATTRID *filter,
	DWORD flags)
{
	(void)iface;
	(void)position;
	(void)count;
	(void)filter;
	(void)flags;

	/* None to retrieve. */
	return S_OK;
}

/* Asks for the attributes that change at a place: none. */
static HRESULT STDMETHODCALLTYPE
store_attributes_changing(
	ITextStoreACP *iface,
	LONG position,
	ULONG count,
	const TS_ATTRID *filter,
	DWORD flags)
{
	(void)iface;
	(void)position;
	(void)count;
	(void)filter;
	(void)flags;

	/* None to retrieve. */
	return S_OK;
}

/* Finds the next change of attributes: none before the halt. */
static HRESULT STDMETHODCALLTYPE
store_attribute_next(
	ITextStoreACP *iface,
	LONG start,
	LONG halt,
	ULONG count,
	const TS_ATTRID *filter,
	DWORD flags,
	LONG *next,
	WINBOOL *found,
	LONG *offset)
{
	(void)iface;
	(void)start;
	(void)count;
	(void)filter;
	(void)flags;

	/* Somewhere to put it. */
	if (next == NULL || found == NULL || offset == NULL)
		return E_INVALIDARG;

	/* None. */
	*next = halt;
	*found = FALSE;
	*offset = 0;
	return S_OK;
}

/* Gives the attributes asked for: none. */
static HRESULT STDMETHODCALLTYPE
store_attribute_values(
	ITextStoreACP *iface,
	ULONG count,
	TS_ATTRVAL *values,
	ULONG *fetched)
{
	(void)iface;
	(void)count;
	(void)values;

	/* Somewhere to put it. */
	if (fetched == NULL)
		return E_INVALIDARG;

	/* None. */
	*fetched = 0U;
	return S_OK;
}

/* Reports the store's length, under a lock. */
static HRESULT STDMETHODCALLTYPE
store_end(
	ITextStoreACP *iface,
	LONG *end)
{
	struct ime_store *store;

	/* A read lock. */
	store = ime_store_of(iface);
	if (end == NULL)
		return E_INVALIDARG;
	if ((store->lock & TS_LF_READ) == 0U)
		return TS_E_NOLOCK;

	/* The length. */
	*end = store->length;
	return S_OK;
}

/* Reports the store's one view. */
static HRESULT STDMETHODCALLTYPE
store_view(
	ITextStoreACP *iface,
	TsViewCookie *view)
{
	(void)iface;

	/* Somewhere to put it. */
	if (view == NULL)
		return E_INVALIDARG;

	/* The one view. */
	*view = IME_VIEW;
	return S_OK;
}

/* Finds no offset at a point (libkeiland draws the composition; its glyphs' places are not known here). */
static HRESULT STDMETHODCALLTYPE
store_at_point(
	ITextStoreACP *iface,
	TsViewCookie view,
	const POINT *point,
	DWORD flags,
	LONG *position)
{
	(void)iface;
	(void)view;
	(void)point;
	(void)flags;
	(void)position;

	/* Not offered. */
	return E_NOTIMPL;
}

/* Reports where a range of text is on the screen: the caret's rectangle, under a lock. */
static HRESULT STDMETHODCALLTYPE
store_extent(
	ITextStoreACP *iface,
	TsViewCookie view,
	LONG start,
	LONG end,
	RECT *rectangle,
	WINBOOL *clipped)
{
	struct ime_store *store;
	POINT corner;
	RECT caret;

	/* The range is not needed: the caret is the only rectangle the store has. */
	(void)start;
	(void)end;

	/* A read lock, the view, and the window. */
	store = ime_store_of(iface);
	if (rectangle == NULL || clipped == NULL)
		return E_INVALIDARG;
	if ((store->lock & TS_LF_READ) == 0U)
		return TS_E_NOLOCK;
	if (view != IME_VIEW)
		return E_INVALIDARG;
	if (store->window == NULL)
		return E_FAIL;

	/* The caret, from the frame's pixels to the screen's. */
	ime_caret(store->window, &caret);
	corner.x = caret.left;
	corner.y = caret.top;
	(void)ClientToScreen(store->window->frame, &corner);
	rectangle->left = corner.x;
	rectangle->top = corner.y;
	rectangle->right = corner.x + (caret.right - caret.left);
	rectangle->bottom = corner.y + (caret.bottom - caret.top);
	*clipped = FALSE;
	return S_OK;
}

/* Reports the view's place on the screen: the frame's. */
static HRESULT STDMETHODCALLTYPE
store_screen(
	ITextStoreACP *iface,
	TsViewCookie view,
	RECT *rectangle)
{
	struct ime_store *store;
	BOOL found;

	/* The view and the window. */
	store = ime_store_of(iface);
	if (rectangle == NULL || view != IME_VIEW)
		return E_INVALIDARG;
	if (store->window == NULL)
		return E_FAIL;

	/* The frame's rectangle. */
	found = GetWindowRect(store->window->frame, rectangle);
	if (!found)
		return E_FAIL;
	return S_OK;
}

/* Reports the view's window: the frame, which has the keys. */
static HRESULT STDMETHODCALLTYPE
store_window(
	ITextStoreACP *iface,
	TsViewCookie view,
	HWND *handle)
{
	struct ime_store *store;

	/* The view and the window. */
	store = ime_store_of(iface);
	if (handle == NULL || view != IME_VIEW)
		return E_INVALIDARG;
	*handle = NULL;
	if (store->window == NULL)
		return E_FAIL;

	/* The frame. */
	*handle = store->window->frame;
	return S_OK;
}

/* Gives the store's interfaces, from the composition sink. */
static HRESULT STDMETHODCALLTYPE
sink_query(
	ITfContextOwnerCompositionSink *iface,
	REFIID id,
	void **out)
{
	struct ime_store *store;
	HRESULT result;

	/* The store's answer. */
	store = ime_store_of_sink(iface);
	result = store_query(&store->store, id, out);
	return result;
}

/* Takes a reference, from the composition sink. */
static ULONG STDMETHODCALLTYPE
sink_add(
	ITfContextOwnerCompositionSink *iface)
{
	struct ime_store *store;
	ULONG count;

	/* The store's count. */
	store = ime_store_of_sink(iface);
	count = store_add(&store->store);
	return count;
}

/* Lets a reference go, from the composition sink. */
static ULONG STDMETHODCALLTYPE
sink_release(
	ITfContextOwnerCompositionSink *iface)
{
	struct ime_store *store;
	ULONG count;

	/* The store's count. */
	store = ime_store_of_sink(iface);
	count = store_release(&store->store);
	return count;
}

/* A composition starts: allowed, and its range kept. */
static HRESULT STDMETHODCALLTYPE
sink_start(
	ITfContextOwnerCompositionSink *iface,
	ITfCompositionView *view,
	WINBOOL *ok)
{
	struct ime_store *store;

	/* Allowed. */
	store = ime_store_of_sink(iface);
	if (ok != NULL)
		*ok = TRUE;

	/* Its range, at the selection until it is read. */
	store->composing = 1;
	store->composition_start = store->selection_start;
	store->composition_end = store->selection_end;
	if (view != NULL)
		ime_store_range(store, view, NULL);
	store->changed = 1;
	return S_OK;
}

/* A composition changes: its new range kept. */
static HRESULT STDMETHODCALLTYPE
sink_update(
	ITfContextOwnerCompositionSink *iface,
	ITfCompositionView *view,
	ITfRange *range)
{
	struct ime_store *store;

	/* The new range. */
	store = ime_store_of_sink(iface);
	if (view != NULL || range != NULL)
		ime_store_range(store, view, range);
	store->changed = 1;
	return S_OK;
}

/* A composition ends: its text is finished, handed on after the lock. */
static HRESULT STDMETHODCALLTYPE
sink_end(
	ITfContextOwnerCompositionSink *iface,
	ITfCompositionView *view)
{
	struct ime_store *store;

	/* The view is not needed: only one composition goes on. */
	(void)view;

	/* No composition goes on. */
	store = ime_store_of_sink(iface);
	store->composing = 0;
	store->changed = 1;

	/*
	 * An end outside a lock (the input method ended it at the program's
	 * wish) is handed on now; inside one, after it.
	 */
	if (store->lock == 0U)
		ime_store_hand_on(store);
	return S_OK;
}
