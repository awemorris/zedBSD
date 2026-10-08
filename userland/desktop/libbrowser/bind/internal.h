/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The inside of the DOM binding: the window's record, the tables that
 * describe each interface, and the helpers the interfaces share.
 *
 * An interface is described by a table (its name, its parent, its
 * attributes with their getters and setters, its operations and its
 * constants) from which window.c makes the interface object and its
 * prototype.  The tables are what a WebIDL generator would write; they are
 * written by hand while the interfaces are few.
 */

#ifndef KEILAND_BROWSER_BIND_INTERNAL_H
#define KEILAND_BROWSER_BIND_INTERNAL_H

#include "bind/bind.h"

/* The parent of an interface that has none. */
#define BIND_NO_PARENT (-1)

/* The phases of an event (Event.eventPhase). */
#define BIND_PHASE_NONE 0
#define BIND_PHASE_CAPTURING 1
#define BIND_PHASE_AT_TARGET 2
#define BIND_PHASE_BUBBLING 3

/*
 * The interfaces the binding makes, parents before children (window.c
 * makes them in this order, so a prototype's parent exists when it is
 * made).
 */
enum bind_interface_index {
	BIND_EVENT_TARGET,
	BIND_NODE,
	BIND_CHARACTER_DATA,
	BIND_TEXT,
	BIND_CDATA_SECTION,
	BIND_PROCESSING_INSTRUCTION,
	BIND_COMMENT,
	BIND_DOCUMENT,
	BIND_XML_DOCUMENT,
	BIND_DOM_IMPLEMENTATION,
	BIND_DOCUMENT_TYPE,
	BIND_DOCUMENT_FRAGMENT,
	BIND_ELEMENT,
	BIND_HTML_ELEMENT,
	BIND_WINDOW,
	BIND_EVENT,
	BIND_UI_EVENT,
	BIND_MOUSE_EVENT,
	BIND_CUSTOM_EVENT,
	BIND_MESSAGE_EVENT,
	BIND_KEYBOARD_EVENT,
	BIND_FOCUS_EVENT,
	BIND_WHEEL_EVENT,
	BIND_NAVIGATOR,
	BIND_SCREEN,
	BIND_PERFORMANCE,
	BIND_LOCATION,
	BIND_HTML_IMAGE_ELEMENT,
	BIND_HTML_SCRIPT_ELEMENT,
	BIND_DOM_TOKEN_LIST,
	BIND_DOM_STRING_MAP,
	BIND_DOM_RECT,
	BIND_CSS_STYLE_DECLARATION,
	BIND_TEXT_ENCODER,
	BIND_TEXT_DECODER,
	BIND_STORAGE,
	BIND_HTML_TEMPLATE_ELEMENT,
	BIND_DOM_EXCEPTION,
	BIND_HTML_IFRAME_ELEMENT,
	BIND_HTML_BUTTON_ELEMENT,
	BIND_HTML_LABEL_ELEMENT,
	BIND_HTML_META_ELEMENT,
	BIND_HTML_OBJECT_ELEMENT,
	BIND_TREE_WALKER,
	BIND_NODE_ITERATOR,
	BIND_HTML_COLLECTION,
	BIND_HTML_FORM_ELEMENT,
	BIND_HTML_FORM_CONTROLS_COLLECTION,
	BIND_NODE_LIST,
	BIND_RADIO_NODE_LIST,
	BIND_HTML_INPUT_ELEMENT,
	BIND_SUBMIT_EVENT,
	BIND_HTML_TABLE_ELEMENT,
	BIND_HTML_TABLE_SECTION_ELEMENT,
	BIND_HTML_TABLE_ROW_ELEMENT,
	BIND_HTML_TABLE_CAPTION_ELEMENT,
	BIND_HTML_SELECT_ELEMENT,
	BIND_HTML_OPTION_ELEMENT,
	BIND_HTML_OPT_GROUP_ELEMENT,
	BIND_HTML_OPTIONS_COLLECTION,
	BIND_ABSTRACT_RANGE,
	BIND_RANGE,
	BIND_HTML_STYLE_ELEMENT,
	BIND_STYLE_SHEET,
	BIND_CSS_STYLE_SHEET,
	BIND_STYLE_SHEET_LIST,
	BIND_CSS_RULE_LIST,
	BIND_SVG_ELEMENT,
	BIND_SVG_GRAPHICS_ELEMENT,
	BIND_SVG_TEXT_CONTENT_ELEMENT,
	BIND_SVG_TEXT_POSITIONING_ELEMENT,
	BIND_SVG_TEXT_ELEMENT,
	BIND_SVG_TSPAN_ELEMENT,
	BIND_SVG_TEXT_PATH_ELEMENT,
	BIND_SVG_GEOMETRY_ELEMENT,
	BIND_SVG_RECT_ELEMENT,
	BIND_SVG_ANIMATED_LENGTH,
	BIND_SVG_LENGTH,
	BIND_HTML_MEDIA_ELEMENT,
	BIND_HTML_VIDEO_ELEMENT,
	BIND_HTML_AUDIO_ELEMENT,
	BIND_HTML_TEXT_AREA_ELEMENT,
	BIND_INTERFACES
};

/*
 * One attribute of an interface: an accessor on the prototype with a
 * native getter and, unless the attribute is read-only, a native setter.
 */
struct bind_attribute {
	const char *name;
	vm_native getter;
	vm_native setter;
};

/*
 * One operation of an interface: a method on the prototype.
 */
struct bind_operation {
	const char *name;
	unsigned length;
	vm_native method;
};

/*
 * One constant of an interface, on both the interface object and its
 * prototype.
 */
struct bind_constant {
	const char *name;
	int value;
};

/*
 * An interface: what window.c needs to make its interface object (a
 * constructor, which throws unless the interface has one) and its
 * prototype.  Each list ends with an entry whose name is NULL; a NULL list
 * is empty.
 */
struct bind_interface {
	const char *name;
	int parent;
	unsigned length;
	vm_native construct;
	const struct bind_attribute *attributes;
	const struct bind_operation *operations;
	const struct bind_constant *constants;
};

/*
 * A timer of setTimeout, setInterval or requestAnimationFrame: when it is
 * due (in the window's milliseconds), its period for an interval, what it
 * calls and with what, and the order it was made in (timers due at the
 * same time run in that order).
 */
struct bind_timer {
	uint32_t id;
	int repeat;
	int animation;
	double due;
	double interval;
	uint64_t sequence;
	vm_value callback;
	vm_value arguments;
};

/* Window-owned C style storage and parser/request state use private definitions. */
struct bind_style_context;

struct html_parser;

struct environment_fetch_pending;

struct environment_mutation_observer;

/*
 * The window of a page: the realm whose global object it is, the
 * document, the host, the prototypes of the interfaces, the window's own
 * event listeners, the timers, and the time and viewport the page gave
 * it.
 *
 * A primary lives from bind_window_create to bind_window_destroy; its tracer
 * keeps the cells it holds alive. A managed child is traced and finalized by
 * its realm, so saved script references retain both records.  ready_state is a static string.
 * location is the window's Location object (document.location reports it
 * too), and time_origin the moment the window was made, in milliseconds
 * since 1970 (performance.timeOrigin).  local_storage and session_storage
 * are the window's Storage objects, made when a script first asks for them
 * (NULL until then; ws074-p080).
 */
struct bind_window {
	struct vm_realm *realm;
	/* Owned windows are finalized with their realm; detached windows run no tasks. */
	int owned;
	int detached;
	/* Globals retain relatives; frame is cleared before child retirement. */
	struct vm_object *parent_global;
	struct vm_object *top_global;
	struct dom_element *frame;
	unsigned context_depth;
	/* Initial blank children retain the actual inherited fallback base URL. */
	struct vm_string *base_url;
	struct dom_document *document;
	struct bind_host host;
	/* This child-only C cache owns engine storage without registering VM roots. */
	struct bind_style_context *style_context;
	/* A script-created parser is traced and destroyed only through its Window owner. */
	struct html_parser *document_parser;
	/* Active parse calls defer requested EOF until the outer native stack unwinds. */
	unsigned document_parser_depth;
	int document_parser_close;
	struct vm_object *prototypes[BIND_INTERFACES];
	struct vm_object *console;
	struct vm_cell *listeners;
	struct wb_vector timers;
	uint32_t next_timer_id;
	uint64_t next_sequence;
	double now;
	int viewport_width;
	int viewport_height;
	const char *ready_state;
	struct vm_object *location;
	double time_origin;
	struct vm_object *local_storage;
	struct vm_object *session_storage;
	struct environment_fetch_pending *fetches;
	struct environment_mutation_observer *mutation_observers;
};

/* One traced inline source owns its C model and original DOM Text until its final GC release. */
struct bind_style_sheet {
	struct vm_cell cell;
	struct dom_element *owner;
	struct css_rule_model *model;
	struct wb_units original;
	/* A genuine model edit selects current rule sources instead of untouched original DOM Text. */
	int changed;
	/* Current source identity retains its independently collectible public wrappers. */
	struct vm_object *wrapper;
	struct vm_object *rules;
};

/* URL reflection helpers borrow the network module's parsed URL record. */
struct net_url;

/*
 * One event listener of a target: its type, its callback (a function or
 * an object with handleEvent), its flags, and the document's generation
 * when it was added.  An event handler (onclick and the like) is a
 * listener too, marked handler, placed where it was first set; its
 * callback is null while the handler is null.
 */
struct bind_listener {
	struct vm_string *type;
	vm_value callback;
	int capture;
	int once;
	int handler;
	uint32_t generation;
};

/*
 * The cell of an event target's listeners.
 */
struct bind_listeners {
	struct vm_cell cell;
	struct bind_listener *items;
	size_t count;
	size_t capacity;
};

/*
 * The state of an event, the cell an Event object wraps: its type, where
 * it is being dispatched, its flags, for a mouse or wheel event the
 * pointer, and for a key event its key, its code and whether it repeats
 * (key and code are NULL for other events; the modifiers of a key event
 * are kept with the pointer's).  target_override is the target the event
 * reports instead of the one it is dispatched at (the document for the
 * window's load), or the empty value.
 */
struct bind_event {
	struct vm_cell cell;
	struct vm_string *type;
	vm_value target;
	vm_value current_target;
	vm_value detail;
	/* UI view retains its Window global through the event cell tracer. */
	vm_value view;
	/* A SubmitEvent strongly retains its actual triggering HTMLElement. */
	vm_value submitter;
	vm_value source;
	vm_value ports;
	vm_value target_override;
	struct vm_string *origin;
	struct vm_string *last_event_id;
	int phase;
	int bubbles;
	int cancelable;
	/* Event.composed observes native state; shadow-path construction remains separate. */
	int composed;
	int canceled;
	int stop;
	int stop_immediate;
	int dispatching;
	/* Factory events cannot dispatch until a legacy initializer sets this flag. */
	int initialized;
	/* Internal interface identity supplies UI receiver branding without prototype tests. */
	int interface;
	int trusted;
	double time_stamp;
	struct bind_mouse mouse;
	struct vm_string *key;
	struct vm_string *code;
	int repeat;
};

/* Native canonical SVG node classification and text-content interfaces. */
extern const struct bind_interface bind_svg_geometry_element_interface;
extern const struct bind_interface bind_svg_rect_element_interface;
extern const struct bind_interface bind_svg_animated_length_interface;
extern const struct bind_interface bind_svg_length_interface;
extern const struct bind_interface bind_svg_element_interface;
extern const struct bind_interface bind_svg_graphics_element_interface;
extern const struct bind_interface bind_svg_text_content_element_interface;
extern const struct bind_interface bind_svg_text_positioning_element_interface;
extern const struct bind_interface bind_svg_text_element_interface;
extern const struct bind_interface bind_svg_tspan_element_interface;
extern const struct bind_interface bind_svg_text_path_element_interface;

/* Scoped branded HTML reflected attributes (html-reflection.c). */
extern const struct bind_interface bind_html_button_element_interface;
extern const struct bind_interface bind_html_label_element_interface;
extern const struct bind_interface bind_html_meta_element_interface;
extern const struct bind_interface bind_html_object_element_interface;
extern const struct bind_interface bind_tree_walker_interface;
extern const struct bind_interface bind_node_iterator_interface;
extern const struct bind_interface bind_html_collection_interface;
extern const struct bind_interface bind_html_table_element_interface;
extern const struct bind_interface bind_html_table_section_element_interface;
extern const struct bind_interface bind_html_table_row_element_interface;
extern const struct bind_interface bind_html_table_caption_element_interface;
extern const struct bind_interface bind_html_form_element_interface;
extern const struct bind_interface bind_html_input_element_interface;
extern const struct bind_interface bind_html_form_controls_collection_interface;
extern const struct bind_interface bind_node_list_interface;
extern const struct bind_interface bind_radio_node_list_interface;

/* Native range boundary factories and interface identities. */
extern const struct bind_interface bind_abstract_range_interface;
extern const struct bind_interface bind_range_interface;

/* Select-family identities and SameObject native option membership. */
extern const struct bind_interface bind_html_select_element_interface;
extern const struct bind_interface bind_html_option_element_interface;
extern const struct bind_interface bind_html_opt_group_element_interface;
extern const struct bind_interface bind_html_options_collection_interface;

/* Initial HTML iframe contexts and their synchronous removal hooks (frame.c). */
extern const struct bind_interface bind_html_iframe_element_interface;

/* Native CSSOM interfaces expose genuine inline source identity and current list counts. */
extern const struct bind_interface bind_html_style_element_interface;
extern const struct bind_interface bind_style_sheet_interface;
extern const struct bind_interface bind_css_style_sheet_interface;
extern const struct bind_interface bind_style_sheet_list_interface;
extern const struct bind_interface bind_css_rule_list_interface;

/* The interface tables (node.c, element.c, document.c, event.c, window.c). */
extern const struct bind_interface bind_event_target_interface;
extern const struct bind_interface bind_node_interface;
extern const struct bind_interface bind_character_data_interface;
extern const struct bind_interface bind_text_interface;
extern const struct bind_interface bind_cdata_interface;
extern const struct bind_interface bind_pi_interface;
extern const struct bind_interface bind_comment_interface;
extern const struct bind_interface bind_document_interface;
extern const struct bind_interface bind_xml_document_interface;
extern const struct bind_interface bind_dom_implementation_interface;
extern const struct bind_interface bind_document_type_interface;
extern const struct bind_interface bind_document_fragment_interface;
extern const struct bind_interface bind_element_interface;
extern const struct bind_interface bind_html_element_interface;
extern const struct bind_interface bind_window_interface;
extern const struct bind_interface bind_event_interface;
extern const struct bind_interface bind_submit_event_interface;
extern const struct bind_interface bind_ui_event_interface;
extern const struct bind_interface bind_mouse_event_interface;
extern const struct bind_interface bind_custom_event_interface;
extern const struct bind_interface bind_message_event_interface;
extern const struct bind_interface bind_keyboard_event_interface;
extern const struct bind_interface bind_focus_event_interface;
extern const struct bind_interface bind_wheel_event_interface;
extern const struct bind_interface bind_navigator_interface;
extern const struct bind_interface bind_screen_interface;
extern const struct bind_interface bind_performance_interface;
extern const struct bind_interface bind_location_interface;
extern const struct bind_interface bind_html_image_element_interface;
extern const struct bind_interface bind_html_media_element_interface;
extern const struct bind_interface bind_html_video_element_interface;
extern const struct bind_interface bind_html_audio_element_interface;
extern const struct bind_interface bind_html_text_area_element_interface;
extern const struct bind_interface bind_html_script_element_interface;
extern const struct bind_interface bind_dom_token_list_interface;
extern const struct bind_interface bind_dom_string_map_interface;
extern const struct bind_interface bind_dom_rect_interface;
extern const struct bind_interface bind_css_style_declaration_interface;
extern const struct bind_interface bind_text_encoder_interface;
extern const struct bind_interface bind_text_decoder_interface;
extern const struct bind_interface bind_storage_interface;
extern const struct bind_interface bind_html_template_element_interface;
extern const struct bind_interface bind_dom_exception_interface;

/* Native node classification and range/options construction share these entry points. */
int bind_svg_node_interface(const struct dom_element *element);
int bind_create_range(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
int bind_select_options(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);

/* SameObject live Document links (collection.c). */
int bind_document_forms(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
int bind_document_images(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
int bind_document_links(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);

/* URL resolution and frame helpers retain actual parent/child document ownership. */
int bind_document_url(struct dom_document *document, int use_base, struct wb_buffer *out);
int bind_frame_location(struct bind_window *window, int part, struct wb_buffer *out);
int bind_frame_install(struct bind_window *parent, struct dom_element *element, struct dom_document *document, struct bind_window **window);
int bind_run_inline_script(struct bind_window *window, struct dom_element *script);
int bind_reflection_base(struct dom_document *document, struct net_url *base);
int bind_reflect_url_get(struct vm_realm *realm, vm_value receiver, int tag, const char *name, vm_value *result);
int bind_reflect_url_set(struct vm_realm *realm, vm_value receiver, int tag, const char *name, const vm_value *args, unsigned count, vm_value *result);
int bind_object_document(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
int bind_object_svg_document(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
void bind_frames_attach(struct bind_window *window);
void bind_frames_detach(struct bind_window *window);
void bind_frames_release(struct bind_window *window);
int bind_frame_parent(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_frame_top(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_frame_self(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_frame_closed(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_frame_element(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);

/* Native CSSOM getters and mutations operate on the traced source model. */
int bind_cssom_document_sheets(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
int bind_cssom_style_sheet(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
int bind_cssom_owner(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
int bind_cssom_href(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
int bind_cssom_rules(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
int bind_cssom_sheets_length(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
int bind_cssom_sheets_item(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
int bind_cssom_rules_length(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
int bind_cssom_insert(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
int bind_cssom_delete(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);

/* Inline source association stays native before public CSSOM wrapper construction. */
int bind_style_sheet_get(struct dom_element *element, struct bind_style_sheet **sheet);
void bind_style_sheet_changed(struct bind_style_sheet *sheet);

/* Window-owned child CSS cascade and connected ancestor inheritance. */
int bind_frame_viewport(struct bind_window *window);
int bind_style_context_engine(struct bind_window *window, struct css_engine **engine);
int bind_style_context_compute(struct bind_window *window, struct dom_element *element, struct css_style *style);
void bind_style_context_release(struct bind_window *window);

/* Window environment state is traced, disconnected and checkpointed by its owner. */
void bind_environment_release(struct bind_window *window);
void bind_environment_trace(struct vm_heap *heap, struct bind_window *window);
void bind_environment_disconnect(struct bind_window *window);
int bind_environment_checkpoint(struct bind_window *window, int *queued);
int bind_environment_child_mutation(struct bind_window *window, struct dom_node *parent, struct dom_node *added, struct dom_node *removed);

/* Markup (markup.c, ws074-p081). */
int bind_template_contents(struct dom_element *element, struct dom_node **contents);
int bind_inner_html_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_inner_html_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_outer_html_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_outer_html_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_insert_adjacent_html(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_insert_adjacent_element(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_insert_adjacent_text(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);

/* Web Storage (storage.c, ws074-p080). */
int bind_local_storage(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_session_storage(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);

/* The inline style (style.c, ws074-p031). */
int bind_style_install(struct bind_window *window);

/* The computed style (computed.c, ws074-p082). */
int bind_get_computed_style(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_computed_element(vm_value value, struct dom_element **element);
int bind_computed_value(struct vm_realm *realm, struct dom_element *element, const struct vm_string *name, vm_value *result);
size_t bind_computed_count(void);
const char *bind_computed_name(size_t index);
int bind_style(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_style_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);

/* The Selectors API, classList and dataset (query.c, ws074-p031). */
int bind_query_selector(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_query_selector_all(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_matches(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_closest(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_class_list(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_dataset(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_split_classes(struct vm_realm *realm, const struct vm_string *classes, struct vm_object *names);

/* The geometry of elements and the window's scrolling (geometry.c, ws074-p031). */
int bind_geometry_install(struct bind_window *window);
int bind_get_bounding_client_rect(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_get_client_rects(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_client_width(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_client_height(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_client_top(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_client_left(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_scroll_width(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_scroll_height(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_scroll_top(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_scroll_left(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_scroll_top_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_scroll_position_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_offset_width(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_offset_height(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_offset_parent(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_offset_top(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_offset_left(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_window_scroll_to(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_window_scroll_by(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_element_scroll_to(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_element_scroll_by(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_scroll_into_view(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_window_scroll_x(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_window_scroll_y(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);

/* The window's environment: navigator, screen, performance, location and Image (environment.c). */
int bind_environment_install(struct bind_window *window);
double bind_epoch_milliseconds(void);
int bind_document_create_event(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_location_part(struct bind_window *window, int part, vm_value *value);

/* The window and the shared helpers (window.c). */
struct bind_window *bind_window_of(struct vm_realm *realm);
int bind_window_install(struct bind_window *window);
int bind_throw_illegal(struct vm_realm *realm);
int bind_illegal_constructor(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_string(struct vm_realm *realm, const char *ascii, vm_value *value);
int bind_units(struct vm_realm *realm, const uint16_t *units, size_t length, vm_value *value);
int bind_to_string(struct vm_realm *realm, vm_value value, struct vm_string **string);
int bind_to_atom(struct vm_realm *realm, vm_value value, int lower, struct vm_string **atom);
int bind_get_option(struct vm_realm *realm, vm_value options, const char *name, int *present, vm_value *value);

/* DOM traversal keeps callback and candidate graphs collectible. */
int bind_traversal_install(struct bind_window *window);
int bind_filter_callback(struct vm_realm *realm, vm_value filter, struct dom_node *node, uint32_t *accepted);
int bind_create_node_iterator(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_create_tree_walker(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);

/* Script-created documents share checked namespace creation and binding snapshots. */
int bind_create_element_ns(struct vm_realm *realm, struct dom_document *document, struct vm_string *uri, struct vm_string *qualified, struct dom_element **created);
int bind_document_implementation(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);

/* Nodes (node.c). */
int bind_wrap(struct bind_window *window, struct dom_node *node, vm_value *value);
int bind_wrap_or_null(struct bind_window *window, struct dom_node *node, vm_value *value);
struct dom_node *bind_node_of(vm_value value);
int bind_this_node(struct vm_realm *realm, vm_value this_value, struct dom_node **node);
int bind_argument_node(struct vm_realm *realm, vm_value value, struct dom_node **node);
int bind_text_content(const struct dom_node *node, struct wb_units *units);
int bind_validate_insert(struct vm_realm *realm, struct dom_node *parent, struct dom_node *node, struct dom_node *reference);
int bind_split_text(struct vm_realm *realm, struct dom_node *node, uint32_t offset, struct dom_node **created);
int bind_clone_node(struct vm_realm *realm, struct dom_node *source, int deep, struct dom_node **created);
int bind_insert(struct vm_realm *realm, struct dom_node *parent, struct dom_node *node, struct dom_node *reference);
int bind_array_create(struct vm_realm *realm, struct vm_object **array);
int bind_array_push_node(struct bind_window *window, struct vm_object *array, struct dom_node *node);
int bind_throw_dom(struct vm_realm *realm, const char *name, const char *message);
struct dom_node *bind_following(const struct dom_node *node, const struct dom_node *root);
struct dom_element *bind_first_element_child(const struct dom_node *node);

/* The mixins' getters and methods shared by several interfaces (node.c). */
int bind_children(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_first_element_child_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_last_element_child_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_child_element_count(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_append(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_prepend(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_remove(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_previous_element_sibling(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_next_element_sibling(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_get_elements_by_tag_name(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_get_elements_by_class_name(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);

/* Elements (element.c). */
int bind_element_has_class(const struct dom_element *element, const struct vm_string *name);

/* Events (event.c). */
struct bind_event *bind_event_of(vm_value value);
int bind_event_create(struct bind_window *window, int interface, struct vm_string *type, vm_value *value, struct bind_event **event);
int bind_submit_event(struct bind_window *window, struct dom_element *form, struct dom_element *submitter, int *canceled);
int bind_click(struct bind_window *window, struct dom_element *element);
int bind_dispatch(struct bind_window *window, vm_value target, vm_value event_value, int *canceled);
int bind_event_prepare(struct bind_window *window, struct dom_node *target, int interface, const char *type, unsigned flags, vm_value *event_value, vm_value *target_value, struct bind_event **event);
int bind_event_construct(struct vm_realm *realm, int interface, const vm_value *args, unsigned count, vm_value *result, struct bind_event **event);
int bind_event_init_mouse(struct vm_realm *realm, vm_value init, struct bind_event *event);

/* The modifiers' getters of MouseEvent and KeyboardEvent, and the modifiers of an event's init (input.c). */
int bind_event_shift_key(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_event_ctrl_key(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_event_alt_key(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_event_meta_key(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_event_init_modifiers(struct vm_realm *realm, vm_value init, struct bind_event *event);
int bind_listeners_of(struct bind_window *window, vm_value target, int create, struct bind_listeners **listeners);
int bind_listeners_add(struct bind_listeners *listeners, const struct bind_listener *listener, size_t position);
int bind_listeners_find(const struct bind_listeners *listeners, const struct vm_string *type, vm_value callback, int capture, int handler, size_t *index);
int bind_listeners_find_handler(const struct bind_listeners *listeners, const struct vm_string *type, size_t *index);

/* Event handlers (handler.c). */
int bind_define_handlers(struct bind_window *window, struct vm_object *prototype);
int bind_handler_prepare(struct bind_window *window, vm_value current, struct vm_string *type);

/* The timers (timer.c). */
void bind_timers_trace(struct vm_heap *heap, struct bind_window *window);
int bind_set_timeout(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_set_interval(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_clear_timer(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
int bind_request_animation_frame(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);

/* Typed native table child operations (table.c). */
int bind_table_caption_get(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
int bind_table_caption_set(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
int bind_table_caption_create(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
int bind_table_caption_delete(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
int bind_table_head_get(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
int bind_table_head_set(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
int bind_table_head_create(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
int bind_table_head_delete(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
int bind_table_foot_get(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
int bind_table_foot_set(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
int bind_table_foot_create(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
int bind_table_foot_delete(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);

/* Native row mutations and pure logical membership (table-row.c, collection.c). */
int bind_table_body_create(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
int bind_table_row_insert(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
int bind_table_row_delete(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
int bind_section_row_insert(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
int bind_section_row_delete(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
int bind_row_index(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
int bind_row_section_index(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
struct dom_node *bind_table_row_next(struct dom_node *table, struct dom_node *previous);

#endif
