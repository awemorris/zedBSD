/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Observes real asynchronous child responses, native contexts, task order and precise ownership. */

#include "page/page.h"
#include "bind/internal.h"
#include "net/net.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Observations accumulate across independent resources and lifetime cases until final reporting. */
static unsigned checks;
/* Preserve failures while still exercising request cancellation and every cleanup path. */
static unsigned failures;
/* Unexpected console exceptions are a separate actual interpreter observation. */
static unsigned exceptions;
/* The live main frame supplies ordinary scanning outside explicit precise-only experiments. */
static const void *load_stack_base;

static void load_check(int condition, const char *name);
static void load_console(void *context, int level, const char *text, size_t length);
static int load_source(struct page *page, const char *source, vm_value *answer);
static int load_assert(struct page *page, const char *source, const char *name);
static int load_poll(struct page *page, struct net_loader *loader, unsigned rounds);
static int load_create(const char *base, struct net_loader *loader, struct page **page);
static int load_http(struct page *page, struct net_loader *loader);
static int load_settle(struct page *page);
static int load_lifetime(struct page *page);
static int load_pressure(struct page *page);
static int load_files(const char *directory);

/*
 * Tests production child loading against real loopback HTTP and ordinary local files.
 */
int
main(
	int argc,
	char **argv)
{
	struct page *page;
	struct net_loader *loader;
	int error;
	int printed;

	/* The driver supplies actual resource URLs without engine hooks or test-dependent source behavior. */
	if (argc != 3)
		return 2;
	/* Nullable owners cover every failed real request or Page operation. */
	page = NULL;
	loader = NULL;
	load_stack_base = __builtin_frame_address(0);
	error = net_loader_create(NULL, NULL, &loader);
	if (error != 0)
		goto cleanup;

	/* Load the genuine parent through the ordinary borrowed loader. */
	error = load_create(argv[1], loader, &page);
	if (error != 0)
		goto cleanup;

	/* Observe original asynchronous response, epoch, MIME and event ordering cases. */
	error = load_http(page, loader);
	if (error != 0)
		goto cleanup;

	/* Settle actual delayed requests while independent native timers advance. */
	error = load_settle(page);
	if (error != 0)
		goto cleanup;

	/* Retire a loaded child and observe its original sole native owner. */
	error = load_lifetime(page);
	if (error != 0)
		goto cleanup;

	/* Trigger the original real allocation threshold during child installation. */
	error = load_pressure(page);
	if (error != 0)
		goto cleanup;

	/* Teardown cancels a real delayed outstanding request before its callback can arrive. */
	error = load_assert(page, "var dying=document.createElement('iframe');dying.src='slow.xml';document.body.appendChild(dying);true;", "pending teardown source prepared");
	if (error != 0)
		goto cleanup;

	/* Start the pending source only after its checked native tree publication. */
	error = page_frames_checkpoint(page);
	if (error != 0)
		goto cleanup;
	load_check(page->frame_loads.count != 0, "actual request pending before Page teardown");

	/* Loader processing after release must never invoke a freed callback or VM owner. */
	page_destroy(page);
	page = NULL;
	error = load_poll(NULL, loader, 25U);
	if (error != 0)
		goto cleanup;

	/* Local file metadata uses an independent actual Page after the HTTP loader finishes. */
	net_loader_destroy(loader);
	loader = NULL;
	error = load_files(argv[2]);
	if (error != 0)
		goto cleanup;

cleanup:
	/* Both live and already-retired optional owners are safe to release on every exit. */
	page_destroy(page);
	net_loader_destroy(loader);

	/* Actual interpreter exceptions remain an independent final failure criterion. */
	load_check(exceptions == 0, "no actual child Uncaught or console exception");
	printed = printf("native child loading: %u/%u passed; error=%d\n", checks - failures, checks, error);
	if (printed < 0 || error != 0)
		return 2;

	/* Every failed actual child observation invalidates the final fixture result. */
	if (failures != 0)
		return 1;

	/* Succeeded: real response, event and native lifetime observations all passed. */
	return 0;
}

/* Retains every native assertion outcome without stopping independent cleanup. */
static void
load_check(
	int condition,
	const char *name)
{
	int printed;

	/* Record the failing observation where it first becomes visible. */
	checks++;
	if (!condition) {
		failures++;
		printed = fprintf(stderr, "FAIL %s\n", name);
		if (printed < 0)
			failures++;
	}

	/* Succeeded: this observation is included in the final totals. */
	return;
}

/* Observes actual interpreter reporting instead of suppressing thrown fixture errors. */
static void
load_console(
	void *context,
	int level,
	const char *text,
	size_t length)
{
	int printed;

	UNUSED_PARAMETER(context);

	/* Error messages remain visible in both sanitizer and ordinary evidence. */
	if (level == BIND_CONSOLE_ERROR) {
		exceptions++;
		printed = fprintf(stderr, "console: %.*s\n", (int)length, text);
		if (printed < 0)
			exceptions++;
	}

	/* Succeeded: no console output changes engine behavior. */
	return;
}

/* Executes ordinary source through the actual interpreter without changing production hooks. */
static int
load_source(
	struct page *page,
	const char *source,
	vm_value *answer)
{
	struct wb_units units;
	struct js_syntax_error syntax;
	int error;

	/* Copy UTF8 source before ordinary native execution. */
	wb_units_init(&units);
	error = wb_utf8_to_units((const unsigned char *)source, strlen(source), &units);
	if (error != 0) {
		wb_units_release(&units);
		return error;
	}

	/* Execute only the completely converted original native script. */
	error = js_run_script(page->realm, units.data, units.length, 0, answer, &syntax);
	if (error != 0) {
		wb_units_release(&units);
		return error;
	}

	/* Completed native execution no longer borrows converted source bytes. */
	wb_units_release(&units);

	/* Succeeded: the actual script completion is available. */
	return 0;
}

/* Asserts a native script completion without relying on formatted console success. */
static int
load_assert(
	struct page *page,
	const char *source,
	const char *name)
{
	vm_value answer;
	int correct;
	int error;
	int printed;

	/* Record the real interpreter result before any asynchronous Page checkpoint. */
	error = load_source(page, source, &answer);
	if (error != 0) {
		printed = fprintf(stderr, "script failure %d: %s\n", error, name);
		if (printed < 0)
			exceptions++;
		return error;
	}

	/* Script predicates inspect actual child Documents, globals and native event consequences. */
	correct = vm_to_boolean(answer);
	load_check(correct, name);

	/* Succeeded: the fixture used the default production binding. */
	return 0;
}

/* Processes the actual loader's news and then the Page's outer native task checkpoint. */
static int
load_poll(
	struct page *page,
	struct net_loader *loader,
	unsigned rounds)
{
	unsigned index;
	int error;

	/* A fixed number of short real waits bounds the fixture independently of engine success. */
	for (index = 0; index < rounds; index++) {
		net_loader_wait(loader, 10);
		net_loader_dispatch(loader);

		/* No JavaScript executes inside net_loader_dispatch's borrowed response callback. */
		if (page != NULL) {
			error = page_set_time(page, page->now + 10);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: actual request callbacks and outer tasks were exercised separately. */
	return 0;
}

/* Creates a genuine Page and parses its initial HTML before using its body. */
static int
load_create(
	const char *base,
	struct net_loader *loader,
	struct page **page)
{
	const char *html;
	int error;

	/* This parent has a real URL and production Document rather than a prebuilt fake tree. */
	*page = NULL;
	error = page_create(page, load_stack_base);
	if (error != 0)
		return error;
	page_set_loader(*page, loader);
	page_set_console(*page, load_console, NULL);
	html = "<!doctype html><body><script>var notes=[];var xmlOwner=false;</script></body>";
	error = page_load_bytes(*page, (const unsigned char *)html, strlen(html), base, "text/html", 9);
	if (error != 0)
		return error;

	/* Succeeded: the parent is a normal parsed active Page. */
	return 0;
}

/* Checks real metadata, namespace gating, child globals and actual event ordering. */
static int
load_http(
	struct page *page,
	struct net_loader *loader)
{
	int error;

	/* Create distinct actual resources, preserving the synchronous initial blank observation. */
	error = load_assert(
	    page,
	    "var fs={};var loads={};function add(n,u,obj){var e=document.createElement(obj?'object':'iframe');fs[n]=e;loads[n]=0;e.onload=function(){loads[n]++;notes.push(n+'-embed');};if(obj)e.data=u;else e.src=u;document.body.appendChild(e);return e;}"
	    "var v=add('v','redirect',false);var initial=v.contentDocument;var initialWindow=v.contentWindow;var o=add('o','vector.html',true);"
	    "add('h','html.xml',false);add('x','good.bin',false);add('w','wrong.xml',false);add('c','case.xml',false);add('b','bad.xml',false);add('p','plain.html',false);add('f','foreign',false);"
	    "initial.URL==='about:blank'&&initial.body!==null&&o.contentDocument===null;",
	    "real initial iframe blank and unloaded object null");
	if (error != 0)
		return error;

	/* Process the current native frame epoch and its pending resource requests. */
	error = page_frames_checkpoint(page);
	if (error != 0)
		return error;

	/* Observe actual pending-request ownership after the completed native checkpoint. */
	load_check(page->frame_loads.count == 9U, "nine actual pending resource tasks");

	/* Poll the original bounded response interval outside borrowed resource callbacks. */
	error = load_poll(page, loader, 80U);
	if (error != 0)
		return error;

	/* SVG availability follows the real redirect's declared MIME, not the misleading .html extension. */
	error = load_assert(page, "v.getSVGDocument()===v.contentDocument&&o.getSVGDocument()===o.contentDocument&&v.contentDocument.documentElement.namespaceURI==='http://www.w3.org/2000/svg';", "actual SVG Documents and cached wrappers");
	if (error != 0)
		return error;

	/* Observe actual final URL and declared normalized MIME. */
	error = load_assert(page, "v.contentDocument.URL.slice(-11)==='vector.html'&&v.contentDocument.contentType==='image/svg+xml'&&v.contentWindow.location.href===v.contentDocument.URL;", "actual final URL and declared normalized MIME");
	if (error != 0)
		return error;

	/* Observe actual native PI namespace attribute and CDATA. */
	error = load_assert(page, "v.contentDocument.firstChild.nodeType===7&&v.contentDocument.firstChild.target==='trace'&&v.contentDocument.documentElement.firstChild.firstChild.nodeType===4;", "actual native PI namespace attribute and CDATA");
	if (error != 0)
		return error;

	/* Observe actual replaced collectible realm and retired initial owner. */
	error = load_assert(page, "v.contentDocument!==initial&&v.contentWindow!==initialWindow&&initialWindow.closed&&initial.defaultView===null&&v.contentWindow.Object!==Object&&v.contentWindow.parent===window&&v.contentWindow.top===window&&v.contentDocument instanceof v.contentWindow.Document;", "actual replaced collectible realm and retired initial owner");
	if (error != 0)
		return error;

	/* Observe real parser insertion and DCL Window embedding event order. */
	error = load_assert(page, "notes.indexOf('html-script')<notes.indexOf('html-dcl')&&notes.indexOf('html-dcl')<notes.indexOf('html-load')&&notes.indexOf('html-load')<notes.indexOf('h-embed')&&fs.h.contentDocument.getElementById('written').textContent==='native';", "real parser insertion and DCL Window embedding event order");
	if (error != 0)
		return error;

	/* Observe full XML validation and exact namespace case script gating. */
	error = load_assert(page, "xmlOwner&&notes.indexOf('xml-script')>=0&&fs.x.contentDocument.contentType==='text/xml'&&notes.indexOf('wrong-script')<0&&notes.indexOf('case-script')<0&&notes.indexOf('bad-script')<0&&notes.indexOf('plain-script')<0;", "full XML validation and exact namespace case script gating");
	if (error != 0)
		return error;

	/* Observe actual parse failure Document and original error evidence. */
	error = load_assert(page, "fs.b.contentDocument.documentElement.localName==='parsererror'&&fs.b.contentDocument.documentElement.textContent.indexOf('offset')>=0&&fs.b.contentDocument.getElementsByTagName('script').length===0;", "actual parse failure Document and original error evidence");
	if (error != 0)
		return error;

	/* Observe exact supported load counts, and a frame redirected to another origin loading while its Document stays out of reach. */
	error = load_assert(page, "loads.v===1&&loads.o===1&&loads.h===1&&loads.x===1&&loads.w===1&&loads.c===1&&loads.b===1&&loads.p===0&&loads.f===1&&fs.f.getSVGDocument()===null&&fs.f.contentDocument===null&&fs.p.getSVGDocument()===null;", "exact supported load counts and foreign Document refusal");
	if (error != 0)
		return error;

	/* Observe borrowed method native brand and actual iframe src reflection. */
	error = load_assert(page, "var branded=false;try{HTMLObjectElement.prototype.getSVGDocument.call(v);}catch(e){branded=e instanceof TypeError;}branded&&fs.h.getSVGDocument()===null&&v.src.indexOf('/redirect')>=0;", "borrowed method native brand and actual iframe src reflection");
	if (error != 0)
		return error;

	/* Changed source must cancel the old delayed response and execute only the current native epoch. */
	error = load_assert(page, "var newer=add('n','old.xml',false);true;", "delayed old source prepared");
	if (error != 0)
		return error;

	/* Process the current native frame epoch and its pending resource requests. */
	error = page_frames_checkpoint(page);
	if (error != 0)
		return error;

	/* Observe source replaced before old response. */
	error = load_assert(page, "newer.src='new.xml';true;", "source replaced before old response");
	if (error != 0)
		return error;

	/* Process the current native frame epoch and its pending resource requests. */
	error = page_frames_checkpoint(page);
	if (error != 0)
		return error;

	/* Poll the original bounded response interval outside borrowed resource callbacks. */
	error = load_poll(page, loader, 45U);
	if (error != 0)
		return error;

	/* Observe stale delayed source cannot replace actual current Document or fire load. */
	error = load_assert(page, "loads.n===1&&newer.contentDocument.documentElement.localName==='new';", "stale delayed source cannot replace actual current Document or fire load");
	if (error != 0)
		return error;

	/* Native removal and reinsertion invalidate a pending generation even when the same source returns. */
	error = load_assert(page, "var gone=add('gone','slow.xml',false);true;", "pending removal source prepared");
	if (error != 0)
		return error;

	/* Process the current native frame epoch and its pending resource requests. */
	error = page_frames_checkpoint(page);
	if (error != 0)
		return error;

	/* Observe native pending removal performed. */
	error = load_assert(page, "document.body.removeChild(gone);true;", "native pending removal performed");
	if (error != 0)
		return error;

	/* Process the current native frame epoch and its pending resource requests. */
	error = page_frames_checkpoint(page);
	if (error != 0)
		return error;

	/* Observe actual pending-request ownership after the completed native checkpoint. */
	load_check(page->frame_loads.count == 0, "removed request canceled and temporary root released");

	/* Poll the original bounded response interval outside borrowed resource callbacks. */
	error = load_poll(page, loader, 25U);
	if (error != 0)
		return error;

	/* Observe removed stale response cannot run callback or expose child. */
	error = load_assert(page, "loads.gone===0&&gone.contentDocument===null;", "removed stale response cannot run callback or expose child");
	if (error != 0)
		return error;

	/* Reinsertion starts a fresh actual request and blank replacement yields a new genuine Document. */
	error = load_assert(page, "document.body.appendChild(gone);newer.src='about:blank';true;", "reinsertion and actual blank navigation prepared");
	if (error != 0)
		return error;

	/* Process the current native frame epoch and its pending resource requests. */
	error = page_frames_checkpoint(page);
	if (error != 0)
		return error;

	/* Poll the original bounded response interval outside borrowed resource callbacks. */
	error = load_poll(page, loader, 25U);
	if (error != 0)
		return error;

	/* Observe fresh reinsertion load and genuine completed blank replacement. */
	error = load_assert(page, "loads.gone===1&&loads.n===2&&newer.contentDocument.URL==='about:blank'&&newer.contentDocument.body!==null;", "fresh reinsertion load and genuine completed blank replacement");
	if (error != 0)
		return error;

	/* Adoption into an inactive native Document cancels the old owning context and request. */
	error = load_assert(page, "var adopted=add('adopt','slow.xml',false);true;", "pending adoption prepared");
	if (error != 0)
		return error;

	/* Process the current native frame epoch and its pending resource requests. */
	error = page_frames_checkpoint(page);
	if (error != 0)
		return error;

	/* Observe pending native adoption into inactive Document. */
	error = load_assert(page, "var inactive=document.implementation.createDocument(null,'holder',null);inactive.documentElement.appendChild(adopted);true;", "pending native adoption into inactive Document");
	if (error != 0)
		return error;

	/* Process the current native frame epoch and its pending resource requests. */
	error = page_frames_checkpoint(page);
	if (error != 0)
		return error;

	/* Observe actual pending-request ownership after the completed native checkpoint. */
	load_check(page->frame_loads.count == 0, "adopted stale request canceled with temporary root released");

	/* Poll the original bounded response interval outside borrowed resource callbacks. */
	error = load_poll(page, loader, 25U);
	if (error != 0)
		return error;

	/* Observe inactive adopted owner cannot activate or expose stale response. */
	error = load_assert(page, "loads.adopt===0&&adopted.ownerDocument===inactive&&adopted.contentDocument===null;", "inactive adopted owner cannot activate or expose stale response");
	if (error != 0)
		return error;

	/* An opaque data origin cannot execute its embedded HTML as an accessible same-origin child. */
	error = load_assert(page, "var opaque=add('opaque','data:text/html,<script>parent.notes.push(123)</script>',false);true;", "opaque data source prepared");
	if (error != 0)
		return error;

	/* Process the current native frame epoch and its pending resource requests. */
	error = page_frames_checkpoint(page);
	if (error != 0)
		return error;

	/* Observe opaque data origin refused without executing supplied script. */
	error = load_assert(page, "loads.opaque===0&&notes.indexOf(123)<0&&opaque.getSVGDocument()===null;", "opaque data origin refused without executing supplied script");
	if (error != 0)
		return error;

	/* Succeeded: each real child navigation observation has been recorded. */
	return 0;
}

/* Retains only an actual loaded Document after disconnection, then verifies its final collection. */
static int
load_lifetime(
	struct page *page)
{
	struct dom_node *node;
	struct dom_document *document;
	struct dom_element *element;
	struct dom_character_data *data;
	struct vm_cell *held;
	struct vm_cell *found;
	vm_value answer;
	uintptr_t address;
	uintptr_t uri;
	uintptr_t target;
	int same;
	int error;

	/* Extract the real loaded native graph without retaining a public wrapper expando. */
	error = load_source(page, "v.contentDocument;", &answer);
	if (error != 0)
		return error;
	node = bind_node_of(answer);
	if (node == NULL)
		return EINVAL;
	document = node->document;
	held = &document->node.cell;
	address = (uintptr_t)held;

	/* Saved native metadata must have its real PI and expanded element fields. */
	if (document->node.first_child == NULL ||
	    document->node.first_child->type != DOM_PROCESSING_INSTRUCTION ||
	    document->node.first_child->next == NULL ||
	    document->node.first_child->next->type != DOM_ELEMENT)
		return EINVAL;
	data = (struct dom_character_data *)document->node.first_child;
	element = (struct dom_element *)document->node.first_child->next;

	/* The third actual expanded attribute requires its complete native storage. */
	if (element->attribute_count < 3 || element->attributes == NULL)
		return EINVAL;

	/* Every copied expanded string must exist before independent character comparisons. */
	if (element->attributes[2].namespace_uri == NULL ||
	    element->attributes[2].prefix == NULL ||
	    element->attributes[2].name == NULL ||
	    element->attributes[2].value == NULL)
		return EINVAL;

	/* Existing projected PI and third expanded attribute supply the original identities. */
	target = (uintptr_t)data->target;
	uri = (uintptr_t)element->attributes[2].namespace_uri;

	/* Observe expanded attribute identity directly through the actual projected native record. */
	same = vm_string_equal_ascii(element->attributes[2].namespace_uri, "urn:actual");
	load_check(same, "actual projected expanded attribute namespace URI");
	same = vm_string_equal_ascii(element->attributes[2].prefix, "q");
	load_check(same, "actual projected expanded attribute prefix");
	same = vm_string_equal_ascii(element->attributes[2].name, "a");
	load_check(same, "actual projected expanded attribute local name");
	same = vm_string_equal_ascii(element->attributes[2].value, "v");
	load_check(same, "actual projected expanded attribute value");
	error = vm_heap_add_root(page->heap, &held);
	if (error != 0)
		return error;

	/* Disconnect and erase script references before disabling all conservative stack roots. */
	error = load_assert(page, "document.body.removeChild(v);delete fs.v;v=null;initial=null;initialWindow=null;true;", "loaded native graph detached before sole root retention");
	if (error != 0)
		goto cleanup;

	/* Collect only after successful disconnection and public alias removal. */
	vm_heap_set_stack_base(page->heap, NULL);
	vm_heap_collect(page->heap);
	found = vm_heap_find_cell(page->heap, address);
	load_check(found == held, "saved-only actual Document retains managed owner after GC");
	if (found != held) {
		error = EINVAL;
		goto cleanup;
	}

	/* The surviving sole native Document must also retain copied character metadata. */
	found = vm_heap_find_cell(page->heap, target);
	load_check(found != NULL, "saved-only Document retains copied PI target");
	found = vm_heap_find_cell(page->heap, uri);
	load_check(found != NULL, "saved-only Document retains non-atom expanded attribute URI");
	same = vm_string_equal_ascii(document->resource_mime, "image/svg+xml");
	load_check(same, "saved-only Document retains real response metadata");
	held = NULL;
	vm_heap_collect(page->heap);
	found = vm_heap_find_cell(page->heap, address);
	load_check(found == NULL, "last release collects actual loaded Document without permanent navigation roots");
	found = vm_heap_find_cell(page->heap, target);
	load_check(found == NULL, "last release collects copied non-atom PI target");
	found = vm_heap_find_cell(page->heap, uri);
	load_check(found == NULL, "last release collects copied non-atom namespace URI");

cleanup:
	/* Release the one acquired result slot and restore the original live embedding frame. */
	vm_heap_remove_root(page->heap, &held);
	vm_heap_set_stack_base(page->heap, load_stack_base);
	if (error != 0)
		return error;

	/* Succeeded: actual graph ownership is independent of request storage and caller stack. */
	return 0;
}

/* Triggers the real production collection threshold inside actual child realm installation. */
static int
load_pressure(
	struct page *page)
{
	struct dom_document *document;
	struct dom_element *element;
	struct dom_node *node;
	struct bind_window *window;
	struct vm_cell *roots[2];
	struct vm_string *scratch;
	struct vm_heap_stats before;
	struct vm_heap_stats after;
	vm_value answer;
	char *bytes;
	size_t length;
	unsigned index;
	unsigned registered;
	int error;

	/* This is a real connected frame and independently owning native Document. */
	error = load_source(page, "var pressure=document.createElement('iframe');document.body.appendChild(pressure);pressure;", &answer);
	if (error != 0)
		return error;
	node = bind_node_of(answer);
	if (node == NULL || node->type != DOM_ELEMENT)
		return EINVAL;

	/* Complete caller setup owns only successfully registered original slots. */
	element = (struct dom_element *)node;
	bytes = NULL;
	registered = 0;
	roots[0] = &element->node.cell;
	roots[1] = NULL;

	/* Acquire each original caller-owned construction and output registration separately. */
	for (index = 0; index < 2U; index++) {
		error = vm_heap_add_root(page->heap, &roots[index]);
		if (error != 0)
			goto cleanup;

		/* Cleanup owns this slot only after its successful registration. */
		registered++;
	}

	/* Only precise production owner edges remain during construction. */
	vm_heap_set_stack_base(page->heap, NULL);
	document = dom_document_create(page->heap);
	if (document == NULL) {
		error = ENOMEM;
		goto cleanup;
	}

	/* The complete constructor output occupies only the original result slot. */
	roots[1] = &document->node.cell;
	vm_heap_collect(page->heap);
	vm_heap_stats(page->heap, &before);
	length = before.live_bytes;
	if (length < 8U * 1024U * 1024U)
		length = 8U * 1024U * 1024U;
	length -= 32768U;
	bytes = malloc(length);
	if (bytes == NULL) {
		error = ENOMEM;
		goto cleanup;
	}

	/* Populate the original default-threshold pressure source before copying it into the VM. */
	memset(bytes, 'x', length);
	scratch = vm_string_from_utf8(page->heap, bytes, length);
	if (scratch == NULL) {
		error = ENOMEM;
		goto cleanup;
	}

	/* Actual VM pressure owns its copied bytes before temporary source release. */
	free(bytes);
	bytes = NULL;
	vm_heap_stats(page->heap, &before);
	error = bind_frame_install(page->window, element, document, &window);
	if (error != 0)
		goto cleanup;

	/* Observe collector progress only after complete actual child publication. */
	vm_heap_stats(page->heap, &after);
	load_check(error == 0 && window != NULL, "actual child install survives precise-only mid-construction GC");
	if (window == NULL) {
		error = EINVAL;
		goto cleanup;
	}

	/* Successful original child installation must cross the real unchanged allocation threshold. */
	load_check(after.collections > before.collections, "production 8MiB threshold crossed inside actual context installation");
	load_check(element->child_context == &window->realm->cell && window->document == document, "actual child ownership published after collected construction");

cleanup:
	/* Remove only acquired original slots without changing collector policy or experiment inputs. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(page->heap, &roots[registered]);
	}

	/* Every pressure exit releases any still-owned ordinary source allocation. */
	free(bytes);
	vm_heap_set_stack_base(page->heap, load_stack_base);
	if (error != 0)
		return error;

	/* Succeeded: the actual production allocation path supplies the lifetime evidence. */
	return 0;
}

/* Exercises actual local file URL resolution with complete response metadata. */
static int
load_files(
	const char *directory)
{
	struct page *page;
	struct wb_buffer path;
	struct wb_buffer url;
	int error;

	/* A normal nonexistent parent path still establishes the ordinary local file profile. */
	page = NULL;
	wb_buffer_init(&path);
	wb_buffer_init(&url);
	error = wb_buffer_append_string(&path, directory);
	if (error != 0)
		goto cleanup;

	/* Complete the ordinary parent source path before local URL resolution. */
	error = wb_buffer_append_string(&path, "/parent.html");
	if (error != 0)
		goto cleanup;

	/* Resolve the complete local path through the ordinary file URL profile. */
	error = net_url_from_file_path(wb_buffer_string(&path), &url);
	if (error != 0)
		goto cleanup;

	/* Create the independent native Page with its actual resolved local URL. */
	error = load_create(wb_buffer_string(&url), NULL, &page);
	if (error != 0)
		goto cleanup;

	/* Observe actual local source prepared. */
	error = load_assert(page, "var local=document.createElement('iframe');var count=0;local.onload=function(){count++;};local.src='local.xml?actual#fragment';document.body.appendChild(local);true;", "actual local source prepared");
	if (error != 0)
		goto cleanup;

	/* Process the current native frame epoch and its pending resource requests. */
	error = page_frames_checkpoint(page);
	if (error != 0)
		goto cleanup;

	/* Observe actual local parsed Document complete URL and declared MIME. */
	error = load_assert(page, "count===1&&local.contentDocument.documentElement.localName==='local'&&local.contentDocument.contentType==='application/xml'&&local.contentDocument.URL.slice(-19)==='xml?actual#fragment';", "actual local parsed Document complete URL and declared MIME");
	if (error != 0)
		goto cleanup;

cleanup:
	/* Ordinary teardown owns every local task, Document and managed child. */
	page_destroy(page);
	wb_buffer_release(&path);
	wb_buffer_release(&url);
	if (error != 0)
		return error;

	/* Succeeded: actual file metadata and parser supplied the child result. */
	return 0;
}

/* Exercises headless settling with actual delayed requests and independent intervening timers. */
static int
load_settle(
	struct page *page)
{
	int error;

	/* The request starts after initial Page loading, so a pre-settle network pass cannot supply it. */
	error = load_assert(
	    page,
	    "var settleFrame=document.createElement('iframe');var settledLoads=0;var timerProgress=0;settleFrame.src='slow.xml';settleFrame.onload=function(){settledLoads++;};document.body.appendChild(settleFrame);setTimeout(function(){timerProgress++;},5);true;",
	    "real delayed headless child and due timer prepared");
	if (error != 0)
		return error;

	/* Settle the original delayed request and independent due timers. */
	error = page_settle(page, page->now + 2000);
	if (error != 0)
		return error;

	/* Observe the original completed task consequence through actual script state. */
	error = load_assert(
	    page,
	    "settledLoads===1&&timerProgress===1&&settleFrame.contentDocument.documentElement.localName==='slow';",
	    "headless settle polls real HTTP response while actual due timer progresses");
	if (error != 0)
		return error;

	/* Completed actual response ownership leaves no native pending root. */
	load_check(page->frame_loads.count == 0, "headless complete response releases pending native request root");

	/* A due cancellation timer must run before the delayed network response can publish a child. */
	error = load_assert(
	    page,
	    "var cancelledFrame=document.createElement('iframe');var cancelledLoads=0;cancelledFrame.src='slow.xml';cancelledFrame.onload=function(){cancelledLoads++;};document.body.appendChild(cancelledFrame);setTimeout(function(){document.body.removeChild(cancelledFrame);},5);true;",
	    "headless real pending request cancellation timer prepared");
	if (error != 0)
		return error;

	/* Settle the original delayed request and independent due timers. */
	error = page_settle(page, page->now + 2000);
	if (error != 0)
		return error;

	/* Observe the original completed task consequence through actual script state. */
	error = load_assert(
	    page,
	    "cancelledLoads===0&&cancelledFrame.contentDocument===null;",
	    "headless due timer cancels actual pending request without stale load event");
	if (error != 0)
		return error;

	/* The cancellation checkpoint also releases its native pending root. */
	load_check(page->frame_loads.count == 0, "headless cancellation releases pending native request root");

	/* Succeeded: virtual timers and genuine network work made independent bounded progress. */
	return 0;
}
