/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The user agent's style sheet: the default look of HTML elements, written
 * for browser after the rendering section of the HTML Standard
 * (the first pass: display, margins, fonts, lists and links; and the form
 * controls' look, after Chromium's sheet, ws074-p032).  Scripts always run
 * in this browser, so noscript is never rendered.
 */

#include "css/internal.h"

/*
 * The sheet's text.  Elements that are not rendered are display: none;
 * the block elements, the headings and the lists get their usual margins.
 */
const char css_user_agent_sheet[] =
	"html, address, blockquote, body, center, dialog, div, figure, figcaption, footer, form, header, hr,"
	" legend, listing, main, p, plaintext, pre, search, xmp, article, aside, h1, h2, h3, h4, h5, h6, hgroup,"
	" nav, section, dir, dd, dl, dt, menu, ol, ul, details, summary, fieldset, optgroup { display: block; }\n"
	"area, base, basefont, datalist, head, link, meta, noembed, noframes, param, rp, script, style,"
	" template, title { display: none; }\n"
	"[hidden] { display: none; }\n"
	"li { display: list-item; }\n"
	"details > summary:first-of-type { display: list-item; }\n"
	"table { display: table; }\n"
	"tr { display: table-row; }\n"
	"td, th { display: table-cell; }\n"
	"thead, tbody, tfoot { display: table-row-group; }\n"
	"caption { display: table-caption; text-align: center; }\n"
	"col { display: table-column; }\n"
	"colgroup { display: table-column-group; }\n"
	"table { border-spacing: 2px; border-collapse: separate; box-sizing: border-box; }\n"
	"thead, tbody, tfoot { vertical-align: middle; }\n"
	"tr, td, th { vertical-align: inherit; }\n"
	"th { text-align: center; }\n"
	"html { color: black; font-family: serif; font-size: medium; line-height: normal; }\n"
	"body { margin: 8px; }\n"
	"p, blockquote, figure, listing, plaintext, pre, xmp, dl { margin-block-start: 1em; margin-block-end: 1em; }\n"
	"blockquote, figure { margin-inline-start: 40px; margin-inline-end: 40px; }\n"
	"dd { margin-inline-start: 40px; }\n"
	"h1 { font-size: 2em; margin-block-start: 0.67em; margin-block-end: 0.67em; font-weight: bold; }\n"
	"h2 { font-size: 1.5em; margin-block-start: 0.83em; margin-block-end: 0.83em; font-weight: bold; }\n"
	"h3 { font-size: 1.17em; margin-block-start: 1em; margin-block-end: 1em; font-weight: bold; }\n"
	"h4 { font-size: 1em; margin-block-start: 1.33em; margin-block-end: 1.33em; font-weight: bold; }\n"
	"h5 { font-size: 0.83em; margin-block-start: 1.67em; margin-block-end: 1.67em; font-weight: bold; }\n"
	"h6 { font-size: 0.67em; margin-block-start: 2.33em; margin-block-end: 2.33em; font-weight: bold; }\n"
	"ul, ol, menu, dir { margin-block-start: 1em; margin-block-end: 1em; padding-inline-start: 40px; }\n"
	"ol { list-style-type: decimal; }\n"
	"ul ul, ol ul { list-style-type: circle; }\n"
	"ul ul ul, ol ul ul, ul ol ul, ol ol ul { list-style-type: square; }\n"
	"ul ul, ul ol, ol ol, ol ul { margin-block-start: 0; margin-block-end: 0; }\n"
	"b, strong, th { font-weight: bold; }\n"
	"i, em, cite, dfn, var, address { font-style: italic; }\n"
	"pre, code, kbd, samp, tt, listing, plaintext, xmp { font-family: monospace; }\n"
	"pre, listing, plaintext, xmp { white-space: pre; }\n"
	"small { font-size: smaller; }\n"
	"big { font-size: larger; }\n"
	"sub, sup { font-size: smaller; }\n"
	"sub { vertical-align: sub; }\n"
	"sup { vertical-align: super; }\n"
	"a:link { color: #0000ee; text-decoration: underline; }\n"
	"u, ins { text-decoration: underline; }\n"
	"center { text-align: center; }\n"
	"[dir=rtl i] { direction: rtl; }\n"
	"[dir=ltr i] { direction: ltr; }\n"
	"hr { border: 1px inset gray; margin-block-start: 0.5em; margin-block-end: 0.5em; }\n"
	"td, th { padding: 1px; }\n"
	"th { text-align: center; }\n"
	"mark { background-color: yellow; color: black; }\n"
	"fieldset { margin-inline-start: 2px; margin-inline-end: 2px; border: 2px groove gray;"
	" padding: 0.35em 0.75em 0.625em; }\n"
	"noscript { display: none; }\n"
	"input, textarea, select, button { margin: 0; font-size: 13.333333px; font-family: sans-serif;"
	" font-weight: normal; font-style: normal; color: black; line-height: normal; text-align: start;"
	" display: inline-block; white-space: normal; }\n"
	"input { padding: 1px 2px; border: 2px inset #767676; background-color: white; }\n"
	"input[type=hidden] { display: none; }\n"
	"input[type=search] { box-sizing: border-box; }\n"
	"input[type=submit], input[type=button], input[type=reset], input[type=image], button {"
	" padding: 1px 6px; border: 2px outset #767676; background-color: #efefef; box-sizing: border-box;"
	" text-align: center; }\n"
	"input[type=submit], input[type=button], input[type=reset], input[type=image] {"
	" white-space: pre; }\n"
	"input[type=checkbox] { margin: 3px 3px 3px 4px; padding: 0; border: none; background-color: transparent;"
	" box-sizing: border-box; }\n"
	"input[type=radio] { margin: 3px 3px 0 5px; padding: 0; border: none; background-color: transparent;"
	" box-sizing: border-box; }\n"
	"select { padding: 0 0 0 3px; border: 1px outset #767676; background-color: white; }\n"
	"textarea { font-family: monospace; padding: 2px; border: 1px solid #767676; background-color: white;"
	" white-space: pre-wrap; }\n"
	":root::view-transition { display: block; position: fixed; top: 0; right: 0; bottom: 0; left: 0; }\n"
	":root::view-transition-group(*) { display: block; position: absolute;"
	" animation-name: -ua-view-transition-group; animation-duration: 0.25s; }\n"
	":root::view-transition-image-pair(*) { display: block; position: absolute; top: 0; right: 0; bottom: 0; left: 0;"
	" animation-duration: inherit; animation-delay: inherit; animation-play-state: inherit; }\n"
	":root::view-transition-old(*), :root::view-transition-new(*) { display: block; position: absolute; top: 0; left: 0;"
	" width: 100%; height: auto; mix-blend-mode: plus-lighter; animation-duration: inherit; animation-delay: inherit;"
	" animation-play-state: inherit; }\n"
	":root::view-transition-old(*) { animation-name: -ua-view-transition-fade-out; }\n"
	":root::view-transition-new(*) { animation-name: -ua-view-transition-fade-in; }\n";
