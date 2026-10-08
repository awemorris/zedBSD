/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The DOM (plan/ws074/design.md §4): the node tree the parser builds, the
 * style and layout read and, later, scripts change.
 *
 * Every node is a cell of its document's heap and lives while something
 * reaches it: the tree, a script, the parser.  Names are atoms, so they
 * compare by pointer; an element also carries the number of its tag
 * (DOM_TAG_*) so the parser and the style can switch on it.  Text lives in
 * a growable buffer outside the heap, since the parser appends to it.
 */

#ifndef KEILAND_BROWSER_DOM_H
#define KEILAND_BROWSER_DOM_H

#include "vm/vm.h"

/* A script already prepared by the parser or by insertion is never run again. */
#define DOM_NODE_SCRIPT_STARTED 0x0001U

/* Setting a dynamic script's async property to false preserves insertion order. */
#define DOM_NODE_SCRIPT_ORDERED 0x0002U

/*
 * The kinds of node, numbered as the DOM's nodeType numbers them.
 */
enum dom_node_type {
	DOM_ELEMENT = 1,
	DOM_TEXT = 3,
	DOM_CDATA_SECTION = 4,
	DOM_PROCESSING_INSTRUCTION = 7,
	DOM_COMMENT = 8,
	DOM_DOCUMENT = 9,
	DOM_DOCUMENT_TYPE = 10,
	DOM_DOCUMENT_FRAGMENT = 11
};

/*
 * The namespaces an element or attribute can be in.
 */
enum dom_namespace {
	DOM_NS_NONE,
	DOM_NS_HTML,
	DOM_NS_SVG,
	DOM_NS_MATHML,
	DOM_NS_XLINK,
	DOM_NS_XML,
	DOM_NS_XMLNS,
	DOM_NS_OTHER
};

/*
 * The rendering mode a document's DOCTYPE chose.
 */
enum dom_quirks {
	DOM_NO_QUIRKS,
	DOM_QUIRKS,
	DOM_LIMITED_QUIRKS
};

/* The content kind selects document-specific naming without a browsing context. */
enum dom_document_content {
	DOM_CONTENT_HTML,
	DOM_CONTENT_XML,
	DOM_CONTENT_XHTML,
	DOM_CONTENT_SVG
};

/*
 * The element names the engine knows, sorted in byte order (the lookup is
 * a binary search over dom_tag_names).  Names are the lower case forms;
 * an SVG element's tag is that of its name folded to lower case.
 */
enum dom_tag {
	DOM_TAG_UNKNOWN,
	DOM_TAG_A,
	DOM_TAG_ABBR,
	DOM_TAG_ACRONYM,
	DOM_TAG_ADDRESS,
	DOM_TAG_ANNOTATION_XML,
	DOM_TAG_APPLET,
	DOM_TAG_AREA,
	DOM_TAG_ARTICLE,
	DOM_TAG_ASIDE,
	DOM_TAG_AUDIO,
	DOM_TAG_B,
	DOM_TAG_BASE,
	DOM_TAG_BASEFONT,
	DOM_TAG_BDI,
	DOM_TAG_BDO,
	DOM_TAG_BGSOUND,
	DOM_TAG_BIG,
	DOM_TAG_BLINK,
	DOM_TAG_BLOCKQUOTE,
	DOM_TAG_BODY,
	DOM_TAG_BR,
	DOM_TAG_BUTTON,
	DOM_TAG_CANVAS,
	DOM_TAG_CAPTION,
	DOM_TAG_CENTER,
	DOM_TAG_CITE,
	DOM_TAG_CODE,
	DOM_TAG_COL,
	DOM_TAG_COLGROUP,
	DOM_TAG_DATA,
	DOM_TAG_DATALIST,
	DOM_TAG_DD,
	DOM_TAG_DEL,
	DOM_TAG_DESC,
	DOM_TAG_DETAILS,
	DOM_TAG_DFN,
	DOM_TAG_DIALOG,
	DOM_TAG_DIR,
	DOM_TAG_DIV,
	DOM_TAG_DL,
	DOM_TAG_DT,
	DOM_TAG_EM,
	DOM_TAG_EMBED,
	DOM_TAG_FIELDSET,
	DOM_TAG_FIGCAPTION,
	DOM_TAG_FIGURE,
	DOM_TAG_FONT,
	DOM_TAG_FOOTER,
	DOM_TAG_FOREIGNOBJECT,
	DOM_TAG_FORM,
	DOM_TAG_FRAME,
	DOM_TAG_FRAMESET,
	DOM_TAG_H1,
	DOM_TAG_H2,
	DOM_TAG_H3,
	DOM_TAG_H4,
	DOM_TAG_H5,
	DOM_TAG_H6,
	DOM_TAG_HEAD,
	DOM_TAG_HEADER,
	DOM_TAG_HGROUP,
	DOM_TAG_HR,
	DOM_TAG_HTML,
	DOM_TAG_I,
	DOM_TAG_IFRAME,
	DOM_TAG_IMAGE,
	DOM_TAG_IMG,
	DOM_TAG_INPUT,
	DOM_TAG_INS,
	DOM_TAG_ISINDEX,
	DOM_TAG_KBD,
	DOM_TAG_KEYGEN,
	DOM_TAG_LABEL,
	DOM_TAG_LEGEND,
	DOM_TAG_LI,
	DOM_TAG_LINK,
	DOM_TAG_LISTING,
	DOM_TAG_MAIN,
	DOM_TAG_MALIGNMARK,
	DOM_TAG_MAP,
	DOM_TAG_MARK,
	DOM_TAG_MARQUEE,
	DOM_TAG_MATH,
	DOM_TAG_MENU,
	DOM_TAG_MENUITEM,
	DOM_TAG_META,
	DOM_TAG_METER,
	DOM_TAG_MGLYPH,
	DOM_TAG_MI,
	DOM_TAG_MN,
	DOM_TAG_MO,
	DOM_TAG_MS,
	DOM_TAG_MTEXT,
	DOM_TAG_MULTICOL,
	DOM_TAG_NAV,
	DOM_TAG_NEXTID,
	DOM_TAG_NOBR,
	DOM_TAG_NOEMBED,
	DOM_TAG_NOFRAMES,
	DOM_TAG_NOSCRIPT,
	DOM_TAG_OBJECT,
	DOM_TAG_OL,
	DOM_TAG_OPTGROUP,
	DOM_TAG_OPTION,
	DOM_TAG_OUTPUT,
	DOM_TAG_P,
	DOM_TAG_PARAM,
	DOM_TAG_PICTURE,
	DOM_TAG_PLAINTEXT,
	DOM_TAG_PRE,
	DOM_TAG_PROGRESS,
	DOM_TAG_Q,
	DOM_TAG_RB,
	DOM_TAG_RP,
	DOM_TAG_RT,
	DOM_TAG_RTC,
	DOM_TAG_RUBY,
	DOM_TAG_S,
	DOM_TAG_SAMP,
	DOM_TAG_SCRIPT,
	DOM_TAG_SEARCH,
	DOM_TAG_SECTION,
	DOM_TAG_SELECT,
	DOM_TAG_SLOT,
	DOM_TAG_SMALL,
	DOM_TAG_SOURCE,
	DOM_TAG_SPACER,
	DOM_TAG_SPAN,
	DOM_TAG_STRIKE,
	DOM_TAG_STRONG,
	DOM_TAG_STYLE,
	DOM_TAG_SUB,
	DOM_TAG_SUMMARY,
	DOM_TAG_SUP,
	DOM_TAG_SVG,
	DOM_TAG_TABLE,
	DOM_TAG_TBODY,
	DOM_TAG_TD,
	DOM_TAG_TEMPLATE,
	DOM_TAG_TEXTAREA,
	DOM_TAG_TFOOT,
	DOM_TAG_TH,
	DOM_TAG_THEAD,
	DOM_TAG_TIME,
	DOM_TAG_TITLE,
	DOM_TAG_TR,
	DOM_TAG_TRACK,
	DOM_TAG_TT,
	DOM_TAG_U,
	DOM_TAG_UL,
	DOM_TAG_VAR,
	DOM_TAG_VIDEO,
	DOM_TAG_WBR,
	DOM_TAG_XMP,
	DOM_TAG_COUNT
};

/*
 * The kinds of form control (dom_control_kind): what the element draws and
 * how it takes the user's input.
 */
enum dom_control_kind {
	DOM_CONTROL_NONE,
	DOM_CONTROL_TEXT,
	DOM_CONTROL_PASSWORD,
	DOM_CONTROL_BUTTON,
	DOM_CONTROL_SUBMIT,
	DOM_CONTROL_RESET,
	DOM_CONTROL_CHECKBOX,
	DOM_CONTROL_RADIO,
	DOM_CONTROL_HIDDEN,
	DOM_CONTROL_TEXTAREA,
	DOM_CONTROL_SELECT
};

/*
 * The part every node shares: its kind, its document and its place in the
 * tree.
 *
 * wrapper is the script's object for the node once a script has seen it,
 * and listeners the cell of its event listeners once one is added (both
 * the DOM binding's; the node keeps them alive, so a script's properties
 * and listeners on a node live as long as the node).
 */
struct dom_node {
	struct vm_cell cell;
	uint16_t type;
	uint16_t flags;
	uint32_t reserved;
	struct dom_document *document;
	struct dom_node *parent;
	struct dom_node *first_child;
	struct dom_node *last_child;
	struct dom_node *previous;
	struct dom_node *next;
	struct vm_object *wrapper;
	/* SameObject children survives while its actual node remains reachable. */
	struct vm_object *children_collection;
	struct vm_cell *listeners;
};

/*
 * One attribute of an element: its local name (an atom), its namespace and
 * prefix (for attributes of foreign content), and its value.
 * Explicit namespace_uri retains arbitrary exact identity as a traced VM edge;
 * legacy built-in attributes use NULL and their canonical namespace class.
 */
struct dom_attribute {
	struct vm_string *name;
	struct vm_string *prefix;
	struct vm_string *value;
	struct vm_string *namespace_uri;
	int ns;
};

/*
 * The state of a form control the user can change, kept beside its
 * element (control.c).
 *
 * One is made the first time the user or the page changes the control,
 * and lives as long as its element (the element's finalizer frees it).
 * value is the control's value once dirty says it no longer follows the
 * element's value attribute (or a textarea's text); caret is an offset
 * into the value in UTF-16 units.  checked is a checkbox's or radio
 * button's checkedness once checked_dirty says it no longer follows the
 * checked attribute.  preedit is the text an input method is composing
 * at the caret (UTF-16, not part of the value; empty when none), drawn
 * underlined there with its own cursor preedit_cursor units into it
 * (ws090-p025).
 *
 * The rest is what the display list last drew (drawn says it did), in
 * layout units in the document's coordinates: the text's unscrolled origin
 * and the content box's other dimensions, how far the text was scrolled to
 * the left to keep the caret in view, where the caret goes (its left, its
 * top and its height) and its color (0xAARRGGBB).  The page draws the caret
 * and turns a click into an offset with them.
 */
struct dom_control {
	struct wb_units value;
	int dirty;
	size_t caret;
	int checked;
	int checked_dirty;
	/* Group unchecking can establish current state without changing the dirty flag. */
	int checked_initialized;
	/* Independent input presentation state, initially false and copied by cloning. */
	int indeterminate;
	struct wb_units preedit;
	size_t preedit_cursor;
	/* Where the composed text's chosen part (an input method's clause) starts, in units; the part runs to preedit_cursor, none when it is not before it (ws177-p019). */
	size_t preedit_begin;
	int drawn;
	int32_t scroll_x;
	int32_t content_x;
	int32_t content_y;
	int32_t content_width;
	int32_t content_height;
	int32_t caret_x;
	int32_t caret_top;
	int32_t caret_height;
	uint32_t caret_color;
};

/*
 * An element: its name, namespace and tag number, and its attributes (a
 * malloc'd array the element owns; the strings are traced through it).
 * content is a template's contents, a document fragment.  created is the
 * document's generation when the element was made, which orders its
 * content attributes' event handlers among the listeners scripts added.
 * control is a form control's state once it has one (control.c; NULL
 * otherwise), which the element owns.
 */
struct dom_element {
	struct dom_node node;
	struct vm_string *local_name;
	struct vm_string *prefix;
	/* Script-created namespace identity is traced independently of built-in IDs. */
	struct vm_string *namespace_uri;
	uint16_t ns;
	uint16_t tag;
	uint32_t created;
	struct dom_attribute *attributes;
	size_t attribute_count;
	size_t attribute_capacity;
	struct dom_node *content;
	struct dom_control *control;
	/* Inline CSS state is traced opaquely; only binding state finalizes its C source model. */
	struct vm_cell *style_sheet;
	/* The binding owns a traced SameObject scalar SVG rect width graph. */
	struct vm_cell *svg_width;
	/* A form retains the SameObject wrapper for its live associated controls. */
	struct vm_object *controls_collection;
	/* A select retains its SameObject live native options wrapper. */
	struct vm_object *options_collection;
	/* Option selectedness separates script-owned state from its clean default attribute. */
	int option_selected;
	int option_dirty;
	/* The current nearest select is traced and reconciled immediately after tree mutations. */
	struct dom_node *option_select;
	/* Table, section and row roots retain their distinct SameObject live wrappers. */
	struct vm_object *bodies_collection;
	struct vm_object *rows_collection;
	struct vm_object *cells_collection;
	/* Script click suppresses recursion until its activation and callbacks finish. */
	int click_in_progress;
	/* Native submission events suppress same-form reentrancy until dispatch completes. */
	int firing_submission_events;
	/* A connected iframe retains its child realm until synchronous removal. */
	struct vm_cell *child_context;
	/* Last attempted source and generation invalidate stale asynchronous responses. */
	struct vm_string *child_source;
	uint64_t child_epoch;
	/* Distinguish an unobserved source from an attempted absent or empty source. */
	int child_load_seen;
};

/*
 * Native Text, CDATA, Comment or PI owns a growable UTF16 character buffer.
 * Only PI has a target, traced with the node and its actual owner graph.
 */
struct dom_character_data {
	struct dom_node node;
	struct wb_units data;
	struct vm_string *target;
};

/*
 * A DOCTYPE node: its name and identifiers (empty strings when absent).
 */
struct dom_doctype {
	struct dom_node node;
	struct vm_string *name;
	struct vm_string *public_id;
	struct vm_string *system_id;
};

/*
 * A document: the root of a tree, the heap its nodes live in and its mode.
 *
 * generation grows with every change to a tree of the document (a node
 * inserted or removed, text or an attribute changed), so the page can tell
 * that its style and layout are out of date.
 */
struct dom_document {
	struct dom_node node;
	/* A child browsing context stays alive while any node reaches its Document. */
	struct vm_cell *context;
	/* Binding view hooks are opaque to DOM; managed context traces their owner. */
	void *view;
	void (*removed)(struct dom_document *document, struct dom_node *node);
	/* Weak pre-removal registrations outlive either GC finalization order. */
	struct dom_removal_registry *removals;
	struct vm_heap *heap;
	/* Internal binding snapshots preserve script-created node realm identities. */
	struct vm_object *binding_prototypes;
	/* The cached DOMImplementation wrapper remains identical while reachable. */
	struct vm_object *implementation;
	/* SameObject links retains one live query rooted at this Document. */
	struct vm_object *links_collection;
	/* SameObject forms retains one live query rooted at this Document. */
	struct vm_object *forms_collection;
	/* SameObject images retains one live native HTML img query rooted at this Document. */
	struct vm_object *images_collection;
	/* The current native inline stylesheet query retains its SameObject wrapper. */
	struct vm_object *style_sheets;
	/* Loaded Documents own their final URL and normalized declared MIME essence. */
	struct vm_string *resource_url;
	struct vm_string *resource_mime;
	enum dom_document_content content;
	enum dom_quirks quirks;
	uint32_t generation;
};

/* Separately allocated weak subscriptions own a token, never their GC context. */
struct dom_removal_subscription;

/* Callbacks repair C state only: no allocation, GC, tree mutation or registry changes. */
int dom_removal_subscribe(struct dom_document *document, void *context, void (*notify)(void *context, struct dom_node *node), struct dom_removal_subscription **subscription);
void dom_removal_unsubscribe(struct dom_removal_subscription *subscription);
void dom_removal_set_root(struct dom_removal_subscription *subscription, struct dom_node *root);
int dom_removal_matches_document(const struct dom_removal_subscription *subscription, const struct dom_document *document);
void dom_removal_rebind_root(struct dom_removal_subscription *subscription, struct dom_node *root);
int dom_removal_prepare(struct dom_document *document);
int dom_removal_move_root(struct dom_node *root, struct dom_document *document);
void dom_removal_notify(struct dom_document *document, struct dom_node *node);
void dom_removal_set_updates(struct dom_removal_subscription *subscription, void (*inserted)(void *context, struct dom_node *node), void (*data)(void *context, struct dom_node *node, size_t offset, size_t removed, size_t added));
void dom_insertion_notify(struct dom_document *document, struct dom_node *node);
void dom_data_notify(struct dom_document *document, struct dom_node *node, size_t offset, size_t removed, size_t added);
void dom_removal_set_split(struct dom_removal_subscription *subscription, void (*split)(void *context, struct dom_node *node, struct dom_node *created, uint32_t offset));
void dom_split_notify(struct dom_document *document, struct dom_node *node, struct dom_node *created, uint32_t offset);
void dom_removal_release(struct dom_document *document);

/* The HTML serialization of nodes (serialize.c, ws074-p081). */
int dom_serialize_children(const struct dom_node *node, int scripting, struct wb_units *out);
int dom_serialize_node(const struct dom_node *node, int scripting, struct wb_units *out);

/* Documents and nodes (node.c). */
struct dom_document *dom_document_create(struct vm_heap *heap);
struct dom_element *dom_element_create(struct dom_document *document, int ns, struct vm_string *local_name, struct vm_string *prefix);
struct dom_node *dom_text_create(struct dom_document *document, const uint16_t *units, size_t length);
struct dom_node *dom_comment_create(struct dom_document *document, const uint16_t *units, size_t length);

/* Native XML factories copy caller-owned C data and root the actual Document and optional PI target. */
int dom_cdata_create(struct dom_document *document, const uint16_t *units, size_t length, struct dom_node **created);
int dom_pi_create(struct dom_document *document, struct vm_string *target, const uint16_t *units, size_t length, struct dom_node **created);
int dom_is_character_data(const struct dom_node *node);
int dom_is_text(const struct dom_node *node);
struct dom_node *dom_doctype_create(struct dom_document *document, struct vm_string *name, struct vm_string *public_id, struct vm_string *system_id);
struct dom_node *dom_fragment_create(struct dom_document *document);
int dom_adopt(struct dom_document *document, struct dom_node *node);
void dom_append_child(struct dom_node *parent, struct dom_node *child);
void dom_insert_before(struct dom_node *parent, struct dom_node *child, struct dom_node *reference);
void dom_remove(struct dom_node *child);
int dom_text_append(struct dom_node *node, const uint16_t *units, size_t length);
int dom_element_add_attribute(struct dom_element *element, int ns, struct vm_string *prefix, struct vm_string *name, struct vm_string *value);
struct dom_attribute *dom_element_find_attribute(const struct dom_element *element, int ns, const struct vm_string *name);

/* Exact URI helpers preserve custom expanded names; inputs are already validated native strings. */
int dom_element_add_attribute_uri(struct dom_element *element, struct vm_string *uri, struct vm_string *prefix, struct vm_string *name, struct vm_string *value);
struct dom_attribute *dom_element_find_attribute_uri(const struct dom_element *element, const struct vm_string *uri, const struct vm_string *name);
int dom_element_is(const struct dom_node *node, int ns, int tag);
int dom_is_node(const struct vm_cell *cell);
int dom_element_set_attribute(struct dom_element *element, struct vm_string *name, struct vm_string *value);
int dom_element_remove_attribute(struct dom_element *element, struct vm_string *name);
int dom_text_set(struct dom_node *node, const uint16_t *units, size_t length);
int dom_text_truncate(struct dom_node *node, size_t length);
int dom_text_replace(struct dom_node *node, size_t offset, size_t count, const uint16_t *units, size_t length);
int dom_is_inclusive_ancestor(const struct dom_node *ancestor, const struct dom_node *node);

/* Ordinary-tree ownership/categories; parser-special associations are not cached (form.c). */
int dom_form_listed(const struct dom_element *element);
int dom_form_control_member(const struct dom_element *element);
struct dom_element *dom_form_owner(const struct dom_element *element);

/* Form controls (control.c). */
int dom_control_kind(const struct dom_element *element);
struct dom_control *dom_control_of(struct dom_element *element);
int dom_control_value(struct dom_element *element, struct wb_units *out);

/* Native input text prepares owned storage before changing dirty value state. */
int dom_input_set_value(struct dom_element *element, const uint16_t *units, size_t length);
int dom_input_clone_value(struct dom_element *destination, const struct dom_element *source);
int dom_control_set_value(struct dom_element *element, const uint16_t *units, size_t length);
int dom_control_label(const struct dom_element *element, struct wb_units *out);
int dom_option_text(const struct dom_element *option, struct wb_units *out);
struct dom_element *dom_select_chosen(struct dom_element *select);

/* Explicit native setters use current radio membership without automatic mutation hooks. */
int dom_input_is_radio(const struct dom_element *element);
struct dom_element *dom_input_checked_radio(struct dom_element *element);
int dom_input_same_radio_group(struct dom_element *first, struct dom_element *second);
int dom_input_set_checked(struct dom_element *element, int checked, int dirty);
int dom_input_clone_checked(struct dom_element *destination, const struct dom_element *source);
int dom_control_checked(const struct dom_element *element);
void dom_control_free(struct dom_element *element);
struct vm_string *dom_attribute_ascii(const struct dom_element *element, const char *name);

/* Owned selection and current ordinary select option membership (select.c). */
void dom_select_reset(struct dom_node *select);
void dom_option_set_selected(struct dom_element *option, int selected, int dirty);
void dom_select_tree_changed(struct dom_node *root);
void dom_select_attribute_changed(struct dom_element *element, struct vm_string *name, int before, int after);
struct dom_node *dom_option_select(struct dom_node *option);
struct dom_node *dom_select_option_next(struct dom_node *select, struct dom_node *previous);

/* Names (names.c). */
int dom_tag_lookup(const uint16_t *units, size_t length);
const char *dom_tag_name(int tag);

#endif
