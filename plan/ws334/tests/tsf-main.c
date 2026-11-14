/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A small program for a check under Wine of libkeiland's TSF text store
 * (libkeiland/win32/ime.c), driven from the TSF side in the same process
 * as an input method would drive it:
 *   tsf-test
 * It opens a window of libkeiland, asks for text, finds the window's TSF
 * document through the thread's manager and, in edit sessions of its own,
 *  1. inserts text at the selection without a composition: the program
 *     gets it as a KL_WINDOW_TEXT_COMMIT, but not while it does not ask
 *     for text;
 *  2. starts a composition and writes into it: a KL_WINDOW_TEXT_PREEDIT
 *     with the text and the cursor at its end (UTF-8 bytes);
 *  3. ends the composition: the text as a KL_WINDOW_TEXT_COMMIT, then the
 *     composing text taken away (a KL_WINDOW_TEXT_PREEDIT with begin -1);
 *  4. starts another and then the program stops asking for text: the
 *     composing text is taken away and nothing is committed;
 *  5. the text's extent the input method puts its candidates by is the
 *     caret's rectangle (kl_window_text_cursor's size).
 * Each check prints "ok <what>" or "FAIL <what>", one the TSF at hand
 * cannot do (Wine's has no compositions) "not run <what>"; the last line
 * is "tsf-test: PASS (<n> checks, <k> not run)" or "tsf-test: FAIL (<n>
 * of <m>)", or
 * "tsf-test: no TSF (IMM32)" (exit 2) when the window has no TSF document.
 */

#define COBJMACROS
#include <keiland/keiland.h>

#include <windows.h>
#include <msctf.h>

#include <stdio.h>
#include <string.h>

/* How long the application is dispatched to see the inputs, in milliseconds. */
#define TSF_LOOK_MS		300U

/* What an edit session does. */
#define TSF_INSERT		1U
#define TSF_START		2U
#define TSF_END			3U
#define TSF_EXTENT		4U

/* The longest text an input is compared with. */
#define TSF_TEXT		64U

/*
 * The edit session of this program: its interface, what it does, the
 * text it writes, the context, the composition it keeps between
 * sessions, what the store answered, and the extent it reported.
 */
struct tsf_session {
	ITfEditSession iface;
	unsigned kind;
	const WCHAR *text;
	ITfContext *context;
	ITfComposition *composition;
	HRESULT answer;
	RECT extent;
};

/* The inputs the window gave since the last look, in order. */
struct tsf_seen {
	unsigned commits;
	unsigned preedits;
	char commit[TSF_TEXT];
	char preedit[TSF_TEXT];
	int begin;
	int cleared;
};

static HRESULT STDMETHODCALLTYPE session_query(ITfEditSession *iface, REFIID id, void **out);
static ULONG STDMETHODCALLTYPE session_add(ITfEditSession *iface);
static ULONG STDMETHODCALLTYPE session_release(ITfEditSession *iface);
static HRESULT STDMETHODCALLTYPE session_do(ITfEditSession *iface, TfEditCookie cookie);
static void tsf_compose(struct kl_app *app, struct kl_window *window, ITfContext *context, TfClientId client, struct tsf_session *session);
static ITfContext *tsf_context(ITfThreadMgr *threads);
static void tsf_look(struct kl_app *app, struct tsf_seen *seen);
static void tsf_check(int passed, const char *what);
static void tsf_unrun(const char *what);

/* The edit session's calls; constant for the life of the program. */
static ITfEditSessionVtbl tsf_session_calls = {
	.QueryInterface = session_query,
	.AddRef = session_add,
	.Release = session_release,
	.DoEditSession = session_do
};

/* The checks made, failed and not run so far (one thread). */
static unsigned tsf_checks;
static unsigned tsf_failed;
static unsigned tsf_unruns;

/*
 * Drives the window's text store as an input method would and checks what
 * the program gets.
 */
int
main(void)
{
	struct kl_app_options app_options;
	struct kl_window_options options;
	struct tsf_session session;
	struct tsf_seen seen;
	struct kl_window *window;
	struct kl_app *app;
	ITfContextComposition *composing;
	ITfContextView *view;
	ITfThreadMgr *threads;
	ITfContext *context;
	TfClientId client;
	HRESULT result;
	HRESULT answer;

	/* The application and its window, asking for text. */
	memset(&app_options, 0, sizeof(app_options));
	app_options.application = "tsf-test";
	app = kl_app_open(&app_options);
	if (app == NULL) {
		printf("tsf-test: FAIL (no application)\n");
		return 1;
	}
	memset(&options, 0, sizeof(options));
	options.title = "tsf-test";
	options.width = 320U;
	options.height = 200U;
	options.present = KL_PRESENT_NONE;
	window = kl_app_window_create(app, &options);
	if (window == NULL) {
		kl_app_close(app);
		printf("tsf-test: FAIL (no window)\n");
		return 1;
	}
	kl_window_text_input(window, 1);
	kl_window_text_cursor(window, 10, 20, 2, 16);
	memset(&seen, 0, sizeof(seen));
	tsf_look(app, &seen);

	/* The thread's manager (the one the window started), and the window's context. */
	result = CoCreateInstance(&CLSID_TF_ThreadMgr, NULL, CLSCTX_INPROC_SERVER, &IID_ITfThreadMgr, (void **)&threads);
	if (FAILED(result)) {
		kl_app_close(app);
		printf("tsf-test: no TSF (IMM32)\n");
		return 2;
	}
	result = ITfThreadMgr_Activate(threads, &client);
	if (FAILED(result)) {
		ITfThreadMgr_Release(threads);
		kl_app_close(app);
		printf("tsf-test: no TSF (IMM32)\n");
		return 2;
	}
	context = tsf_context(threads);
	if (context == NULL) {
		(void)ITfThreadMgr_Deactivate(threads);
		ITfThreadMgr_Release(threads);
		kl_app_close(app);
		printf("tsf-test: no TSF (IMM32)\n");
		return 2;
	}
	memset(&session, 0, sizeof(session));
	session.iface.lpVtbl = &tsf_session_calls;
	session.context = context;

	/* 1. Text inserted without a composition is committed. */
	session.kind = TSF_INSERT;
	session.text = L"\x3042\x3044";
	result = ITfContext_RequestEditSession(context, client, &session.iface, TF_ES_SYNC | TF_ES_READWRITE, &answer);
	tsf_check(SUCCEEDED(result) && SUCCEEDED(answer) && SUCCEEDED(session.answer), "insert: the edit session ran");
	memset(&seen, 0, sizeof(seen));
	tsf_look(app, &seen);
	tsf_check(seen.commits == 1U && strcmp(seen.commit, "\xe3\x81\x82\xe3\x81\x84") == 0, "insert: one commit of the text");
	tsf_check(seen.preedits == 0U, "insert: no composing text");

	/* 1b. Text the program does not want is not handed to it. */
	kl_window_text_input(window, 0);
	session.kind = TSF_INSERT;
	session.text = L"z";
	result = ITfContext_RequestEditSession(context, client, &session.iface, TF_ES_SYNC | TF_ES_READWRITE, &answer);
	tsf_check(SUCCEEDED(result) && SUCCEEDED(answer) && SUCCEEDED(session.answer), "unwanted: the edit session ran");
	memset(&seen, 0, sizeof(seen));
	tsf_look(app, &seen);
	tsf_check(seen.commits == 0U && seen.preedits == 0U, "unwanted: nothing handed to the program");
	kl_window_text_input(window, 1);

	/* 2 to 4 need TSF's compositions (Wine's TSF has none: those checks are not run there). */
	result = ITfContext_QueryInterface(context, &IID_ITfContextComposition, (void **)&composing);
	if (FAILED(result)) {
		tsf_unrun("compose, end and drop: this TSF has no compositions (ITfContextComposition)");
	} else {
		ITfContextComposition_Release(composing);
		tsf_compose(app, window, context, client, &session);
	}

	/* 5. The text's extent is the caret's rectangle (where the view can be had). */
	kl_window_text_input(window, 1);
	result = ITfContext_GetActiveView(context, &view);
	if (FAILED(result)) {
		printf("tsf-test: the context's view: 0x%08lx\n", (unsigned long)result);
		tsf_unrun("extent: this TSF gives no view of the context (ITfContext::GetActiveView)");
	} else {
		ITfContextView_Release(view);
		session.kind = TSF_EXTENT;
		result = ITfContext_RequestEditSession(context, client, &session.iface, TF_ES_SYNC | TF_ES_READ, &answer);
		tsf_check(SUCCEEDED(result) && SUCCEEDED(session.answer), "extent: reported");
		tsf_check(session.extent.right - session.extent.left == 2 && session.extent.bottom - session.extent.top == 16,
		    "extent: the caret's size (2 by 16)");
	}

	/* The end: the manager and the window. */
	ITfContext_Release(context);
	(void)ITfThreadMgr_Deactivate(threads);
	ITfThreadMgr_Release(threads);
	kl_window_close(window);
	kl_app_close(app);

	/* The checks. */
	if (tsf_failed != 0U) {
		printf("tsf-test: FAIL (%u of %u)\n", tsf_failed, tsf_checks);
		return 1;
	}
	printf("tsf-test: PASS (%u checks, %u not run)\n", tsf_checks, tsf_unruns);
	return 0;
}

/* Checks 2 to 4: a composition shown, ended and committed, and one dropped when text is no longer wanted. */
static void
tsf_compose(
	struct kl_app *app,
	struct kl_window *window,
	ITfContext *context,
	TfClientId client,
	struct tsf_session *session)
{
	struct tsf_seen seen;
	HRESULT result;
	HRESULT answer;

	/* 2. A composition's text is the composing text, the cursor at its end. */
	session->kind = TSF_START;
	session->text = L"\x304b\x306a";
	result = ITfContext_RequestEditSession(context, client, &session->iface, TF_ES_SYNC | TF_ES_READWRITE, &answer);
	tsf_check(SUCCEEDED(result) && SUCCEEDED(answer) && SUCCEEDED(session->answer) && session->composition != NULL,
	    "compose: the composition started");
	memset(&seen, 0, sizeof(seen));
	tsf_look(app, &seen);
	tsf_check(seen.commits == 0U, "compose: nothing committed");
	tsf_check(seen.preedits >= 1U && strcmp(seen.preedit, "\xe3\x81\x8b\xe3\x81\xaa") == 0 && seen.begin == 6,
	    "compose: the composing text with its cursor at 6 bytes");

	/* 3. Its end commits it and takes the composing text away. */
	session->kind = TSF_END;
	result = ITfContext_RequestEditSession(context, client, &session->iface, TF_ES_SYNC | TF_ES_READWRITE, &answer);
	tsf_check(SUCCEEDED(result) && SUCCEEDED(answer) && SUCCEEDED(session->answer), "end: the composition ended");
	memset(&seen, 0, sizeof(seen));
	tsf_look(app, &seen);
	tsf_check(seen.commits == 1U && strcmp(seen.commit, "\xe3\x81\x8b\xe3\x81\xaa") == 0, "end: one commit of the composed text");
	tsf_check(seen.cleared, "end: the composing text taken away");

	/* 4. A composition when text is no longer wanted is dropped. */
	session->kind = TSF_START;
	session->text = L"x";
	result = ITfContext_RequestEditSession(context, client, &session->iface, TF_ES_SYNC | TF_ES_READWRITE, &answer);
	tsf_check(SUCCEEDED(result) && SUCCEEDED(answer) && SUCCEEDED(session->answer), "drop: a composition started");
	memset(&seen, 0, sizeof(seen));
	tsf_look(app, &seen);
	tsf_check(seen.preedits >= 1U && strcmp(seen.preedit, "x") == 0, "drop: its composing text shown");
	kl_window_text_input(window, 0);
	memset(&seen, 0, sizeof(seen));
	tsf_look(app, &seen);
	tsf_check(seen.commits == 0U, "drop: nothing committed");
	tsf_check(seen.cleared, "drop: the composing text taken away");
	if (session->composition != NULL) {
		ITfComposition_Release(session->composition);
		session->composition = NULL;
	}
}

/* Gives the session's interface. */
static HRESULT STDMETHODCALLTYPE
session_query(
	ITfEditSession *iface,
	REFIID id,
	void **out)
{
	/* The session's, or nothing. */
	if (out == NULL)
		return E_POINTER;
	*out = NULL;
	if (!IsEqualIID(id, &IID_IUnknown) && !IsEqualIID(id, &IID_ITfEditSession))
		return E_NOINTERFACE;
	*out = iface;
	return S_OK;
}

/* Takes a reference (the session lives on main's stack). */
static ULONG STDMETHODCALLTYPE
session_add(
	ITfEditSession *iface)
{
	(void)iface;

	/* Always held. */
	return 2U;
}

/* Lets a reference go (the session lives on main's stack). */
static ULONG STDMETHODCALLTYPE
session_release(
	ITfEditSession *iface)
{
	(void)iface;

	/* Always held. */
	return 1U;
}

/* Does what the session is for, under the lock TSF granted. */
static HRESULT STDMETHODCALLTYPE
session_do(
	ITfEditSession *iface,
	TfEditCookie cookie)
{
	ITfContextComposition *composing;
	ITfInsertAtSelection *insert;
	ITfContextView *view;
	struct tsf_session *session;
	BOOL clipped;
	ITfRange *range;
	HRESULT result;

	/* The session. */
	session = (struct tsf_session *)iface;
	session->answer = E_FAIL;

	/* What it does. */
	switch (session->kind) {
	case TSF_INSERT:
		/* The text at the selection. */
		result = ITfContext_QueryInterface(session->context, &IID_ITfInsertAtSelection, (void **)&insert);
		if (FAILED(result))
			return result;
		range = NULL;
		result = ITfInsertAtSelection_InsertTextAtSelection(insert, cookie, 0U, session->text, (LONG)wcslen(session->text), &range);
		ITfInsertAtSelection_Release(insert);
		if (range != NULL)
			ITfRange_Release(range);
		session->answer = result;
		return result;
	case TSF_START:
		/* A composition over an empty range at the selection, and its text. */
		result = ITfContext_QueryInterface(session->context, &IID_ITfInsertAtSelection, (void **)&insert);
		if (FAILED(result))
			return result;
		range = NULL;
		result = ITfInsertAtSelection_InsertTextAtSelection(insert, cookie, TF_IAS_QUERYONLY, NULL, 0, &range);
		ITfInsertAtSelection_Release(insert);
		if (FAILED(result) || range == NULL)
			return E_FAIL;
		result = ITfContext_QueryInterface(session->context, &IID_ITfContextComposition, (void **)&composing);
		if (FAILED(result)) {
			ITfRange_Release(range);
			return result;
		}
		result = ITfContextComposition_StartComposition(composing, cookie, range, NULL, &session->composition);
		ITfContextComposition_Release(composing);
		if (FAILED(result) || session->composition == NULL) {
			ITfRange_Release(range);
			session->answer = E_FAIL;
			return E_FAIL;
		}
		result = ITfRange_SetText(range, cookie, 0U, session->text, (LONG)wcslen(session->text));
		ITfRange_Release(range);
		session->answer = result;
		return result;
	case TSF_END:
		/* The composition ends. */
		if (session->composition == NULL)
			return E_FAIL;
		result = ITfComposition_EndComposition(session->composition, cookie);
		ITfComposition_Release(session->composition);
		session->composition = NULL;
		session->answer = result;
		return result;
	case TSF_EXTENT:
		/* The selection's range, and where the view says it is. */
		result = ITfContext_QueryInterface(session->context, &IID_ITfInsertAtSelection, (void **)&insert);
		if (FAILED(result))
			return result;
		range = NULL;
		result = ITfInsertAtSelection_InsertTextAtSelection(insert, cookie, TF_IAS_QUERYONLY, NULL, 0, &range);
		ITfInsertAtSelection_Release(insert);
		if (FAILED(result) || range == NULL)
			return E_FAIL;
		result = ITfContext_GetActiveView(session->context, &view);
		if (FAILED(result)) {
			ITfRange_Release(range);
			return result;
		}
		clipped = FALSE;
		result = ITfContextView_GetTextExt(view, cookie, range, &session->extent, &clipped);
		ITfContextView_Release(view);
		ITfRange_Release(range);
		session->answer = result;
		return result;
	default:
		return E_INVALIDARG;
	}
}

/* Finds the window's context: the top of the thread's (only) document. */
static ITfContext *
tsf_context(
	ITfThreadMgr *threads)
{
	IEnumTfDocumentMgrs *documents;
	ITfDocumentMgr *document;
	ITfContext *context;
	ULONG fetched;
	HRESULT result;

	/* The thread's documents: the first with a context. */
	result = ITfThreadMgr_EnumDocumentMgrs(threads, &documents);
	if (FAILED(result))
		return NULL;
	context = NULL;
	for (;;) {
		document = NULL;
		fetched = 0U;
		result = IEnumTfDocumentMgrs_Next(documents, 1U, &document, &fetched);
		if (result != S_OK || fetched == 0U)
			break;
		result = ITfDocumentMgr_GetTop(document, &context);
		ITfDocumentMgr_Release(document);
		if (SUCCEEDED(result) && context != NULL)
			break;
		context = NULL;
	}
	IEnumTfDocumentMgrs_Release(documents);

	/* The context, or none. */
	return context;
}

/* Dispatches the application until no input comes for a while, and keeps the text inputs. */
static void
tsf_look(
	struct kl_app *app,
	struct tsf_seen *seen)
{
	struct kl_app_event event;
	ULONGLONG until;
	ULONGLONG now;
	int taken;

	/* Each event until a quiet moment. */
	until = GetTickCount64() + TSF_LOOK_MS;
	for (;;) {
		now = GetTickCount64();
		if (now >= until)
			break;
		(void)kl_app_dispatch(app, 50);
		for (;;) {
			taken = kl_app_take(app, &event);
			if (!taken)
				break;
			if (event.kind != KL_APP_WINDOW)
				continue;
			if (event.input.kind == KL_WINDOW_TEXT_COMMIT) {
				seen->commits++;
				snprintf(seen->commit, sizeof(seen->commit), "%s", event.input.text);
			}
			if (event.input.kind == KL_WINDOW_TEXT_PREEDIT) {
				seen->preedits++;
				snprintf(seen->preedit, sizeof(seen->preedit), "%s", event.input.text);
				seen->begin = event.input.begin;
				if (event.input.text[0] == '\0' && event.input.begin == -1)
					seen->cleared = 1;
			}
		}
	}
}

/* Counts a check and prints it. */
static void
tsf_check(
	int passed,
	const char *what)
{
	/* Counted. */
	tsf_checks++;
	if (!passed)
		tsf_failed++;
	printf("%s %s\n", passed ? "ok" : "FAIL", what);
	fflush(stdout);
}

/* Prints a check the TSF at hand cannot do, and counts it. */
static void
tsf_unrun(
	const char *what)
{
	/* Counted. */
	tsf_unruns++;
	printf("not run %s\n", what);
	fflush(stdout);
}
