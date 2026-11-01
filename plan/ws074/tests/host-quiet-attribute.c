/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the data-* attributes that no style sheet names
 * (BUG-397: a store page set such an attribute on each of its images
 * between reads of their computed style, and each read laid the whole page
 * out again).
 *
 *   host-quiet-attribute
 *
 * Run from the repository's root (the fonts are the checked-in ones).  A
 * page reads computed widths through getComputedStyle, which lays it out
 * when it is out of date:
 * 1. A data-* attribute named by a selector (plain, in :is(), :where(),
 *    :not() and :has(), under @media), or by attr() in generated content,
 *    changes the style it names when it is set, changed and removed.
 * 2. A data-* attribute no sheet names, set or changed 200 times between
 *    reads, lays the page out no more (the layouts are counted), and the
 *    widths read stay right.
 * 3. A sheet added after such an attribute was set (a <style> element, and
 *    a rule inserted through CSSOM) styles the element by its value.
 * Prints one line per failed check and a summary.
 */

#include "bind/internal.h"
#include "page/page.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The checks made and the ones that failed. */
static unsigned checks;
static unsigned failures;

/* The page: elements whose widths their data-* attributes decide through the sheet. */
static const char quiet_page[] =
	"<!doctype html><html><head><style>"
	"div{display:block;width:100px}"
	"#plain[data-plain]{width:11px}"
	":is(#is[data-is=on]){width:12px}"
	"div:where(#where[data-where]){width:13px}"
	"#not:not([data-not]){width:14px}"
	"#has:has(> [data-has]){width:15px}"
	"@media (min-width:1px){#media[data-media]{width:16px}}"
	"#attr{display:inline-block;width:auto}#attr::before{content:attr(data-attr)}"
	"#upper[DATA-UPPER]{width:17px}"
	"</style></head><body>"
	"<div id=plain></div><div id=is></div><div id=where></div><div id=not></div>"
	"<div id=has><span id=child></span></div><div id=media></div><span id=attr></span>"
	"<div id=upper></div><div id=quiet></div><div id=late></div><div id=cssom></div>"
	"</body></html>";

static void quiet_check(int condition, const char *what);
static int quiet_script(struct page *page, const char *source, vm_value *answer);
static void quiet_width(struct page *page, const char *setup, const char *id, const char *expected, const char *what);
static void quiet_named(struct page *page);
static void quiet_unnamed(struct page *page);
static void quiet_late(struct page *page);

/*
 * Checks named, unnamed and later-named data-* attributes.
 */
int
main(
	void)
{
	static const struct text_font_paths paths = {
		"userland/desktop/fonts/Mahora-Regular.ttf",
		"userland/desktop/fonts/JetBrainsMono-Regular.ttf",
		"userland/desktop/fonts/DroidSansFallbackFull.ttf",
	};
	struct page *page;
	int error;

	/* The page, with its fonts and viewport, so that computed widths lay it out. */
	error = page_create(&page, __builtin_frame_address(0));
	if (error != 0)
		return 2;
	page_set_fonts(page, &paths);
	page_set_viewport(page, 800, 600);
	error = page_load_html(page, (const unsigned char *)quiet_page, sizeof(quiet_page) - 1U);
	if (error == 0)
		error = page_open_fonts(page, &paths);
	if (error != 0) {
		page_destroy(page);
		return 2;
	}

	/* The three parts. */
	quiet_named(page);
	quiet_unnamed(page);
	quiet_late(page);
	page_destroy(page);

	/* The summary. */
	printf("host-quiet-attribute: %u checks, %u failed\n", checks, failures);
	if (failures != 0)
		return 1;
	return 0;
}

/* Counts a check and reports it when it failed. */
static void
quiet_check(
	int condition,
	const char *what)
{
	/* Each check counts; a failed one is printed. */
	checks++;
	if (condition)
		return;
	failures++;
	printf("FAIL %s\n", what);
}

/* Runs a script in the page's realm, with its completion value in answer. */
static int
quiet_script(
	struct page *page,
	const char *source,
	vm_value *answer)
{
	struct js_syntax_error syntax;
	struct wb_units units;
	int error;

	/* The source as UTF-16, run as a classic script. */
	wb_units_init(&units);
	error = wb_utf8_to_units((const unsigned char *)source, strlen(source), &units);
	if (error == 0)
		error = js_run_script(page->realm, units.data, units.length, 0, answer, &syntax);
	wb_units_release(&units);
	if (error != 0)
		return error;

	/* Succeeded: the script ran. */
	return 0;
}

/* Runs setup, then checks the computed width of the element id is expected (a CSS length's text). */
static void
quiet_width(
	struct page *page,
	const char *setup,
	const char *id,
	const char *expected,
	const char *what)
{
	struct vm_string *string;
	char source[512];
	char line[256];
	vm_value answer;
	int same;
	int error;

	/* The setup and the read, as one script whose value is the width. */
	snprintf(source, sizeof(source), "%s;getComputedStyle(document.getElementById('%s')).width", setup, id);
	error = quiet_script(page, source, &answer);
	snprintf(line, sizeof(line), "%s: the script ran", what);
	quiet_check(error == 0, line);
	if (error != 0)
		return;

	/* The width's text. */
	same = 0;
	if (vm_value_is_string(answer)) {
		string = (struct vm_string *)vm_value_as_cell(answer);
		same = vm_string_equal_ascii(string, expected);
	}
	snprintf(line, sizeof(line), "%s: the width is %s", what, expected);
	quiet_check(same, line);
}

/* A data-* attribute that a selector or attr() names changes the style when it is set, changed and removed. */
static void
quiet_named(
	struct page *page)
{
	/* Each kind of selector, set and removed. */
	quiet_width(page, "0", "plain", "100px", "plain selector before");
	quiet_width(page, "document.getElementById('plain').setAttribute('data-plain','')", "plain", "11px", "plain selector set");
	quiet_width(page, "document.getElementById('plain').removeAttribute('data-plain')", "plain", "100px", "plain selector removed");
	quiet_width(page, "document.getElementById('is').setAttribute('data-is','off')", "is", "100px", ":is() other value");
	quiet_width(page, "document.getElementById('is').setAttribute('data-is','on')", "is", "12px", ":is() value changed");
	quiet_width(page, "document.getElementById('where').setAttribute('data-where','1')", "where", "13px", ":where() set");
	quiet_width(page, "0", "not", "14px", ":not() before");
	quiet_width(page, "document.getElementById('not').setAttribute('data-not','')", "not", "100px", ":not() set");
	quiet_width(page, "document.getElementById('child').setAttribute('data-has','')", "has", "15px", ":has() set on a child");
	quiet_width(page, "document.getElementById('child').removeAttribute('data-has')", "has", "100px", ":has() removed from a child");
	quiet_width(page, "document.getElementById('media').setAttribute('data-media','')", "media", "16px", "@media selector set");
	quiet_width(page, "document.getElementById('upper').setAttribute('data-upper','')", "upper", "17px", "upper-case selector set");

	/* attr() in generated content: a longer value makes the box wider. */
	quiet_width(page, "var a=document.getElementById('attr');a.setAttribute('data-attr','');var w0=a.getBoundingClientRect().width;"
	    "a.setAttribute('data-attr','WWWWWWWWWW');var w1=a.getBoundingClientRect().width;"
	    "if(w1>w0+20)a.style.width='18px'", "attr", "18px", "attr() value changed");
}

/* A data-* attribute that no sheet names leaves the layout as it is, and the widths stay right. */
static void
quiet_unnamed(
	struct page *page)
{
	uint32_t serial;
	char line[128];

	/* A layout to start from. */
	quiet_width(page, "0", "quiet", "100px", "unnamed before");
	serial = page->layout_serial;

	/* Many changes between reads, as the store page made. */
	quiet_width(page, "for(var i=0;i<200;i++){var q=document.getElementById('quiet');q.setAttribute('data-quiet',String(i));"
	    "q.setAttribute('data-other','x'+i);if(getComputedStyle(q).width!='100px')throw new Error('width')}", "quiet", "100px", "unnamed changes");
	snprintf(line, sizeof(line), "unnamed changes: no layout (%u layouts)", (unsigned)(page->layout_serial - serial));
	quiet_check(page->layout_serial == serial, line);

	/* A named one still lays the page out again. */
	quiet_width(page, "document.getElementById('plain').setAttribute('data-plain','x')", "plain", "11px", "named after unnamed");
	quiet_check(page->layout_serial != serial, "named after unnamed: laid out again");
}

/* A sheet added after an unnamed attribute was set styles the element by it. */
static void
quiet_late(
	struct page *page)
{
	/* A <style> element added after the attribute. */
	quiet_width(page, "document.getElementById('late').setAttribute('data-late','')", "late", "100px", "late attribute set");
	quiet_width(page, "var s=document.createElement('style');s.textContent='#late[data-late]{width:21px}';document.head.appendChild(s)",
	    "late", "21px", "late <style> added");

	/* A rule inserted through CSSOM after the attribute. */
	quiet_width(page, "document.getElementById('cssom').setAttribute('data-cssom','')", "cssom", "100px", "CSSOM attribute set");
	quiet_width(page, "document.styleSheets[0].insertRule('#cssom[data-cssom]{width:22px}',document.styleSheets[0].cssRules.length)",
	    "cssom", "22px", "CSSOM rule inserted");

	/* And a change of it now that a sheet names it. */
	quiet_width(page, "document.getElementById('cssom').removeAttribute('data-cssom')", "cssom", "100px", "CSSOM-named attribute removed");
}
