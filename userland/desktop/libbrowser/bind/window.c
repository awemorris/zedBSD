/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The window: the realm's global object made into the page's window, the
 * interface objects made from their tables, the console, queueMicrotask,
 * running a script and a microtask checkpoint with their errors reported
 * to the console, and the helpers the interfaces share.
 */

#include "bind/internal.h"
#include "html/html.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The longest console line kept for one call, in bytes (the rest is cut). */
#define WINDOW_CONSOLE_MAX	(64U * 1024U)

static int window_queue_microtask(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int window_post_message(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int window_inner_width(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int window_inner_height(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);

/*
 * The attributes of Window's prototype.  The table is constant for the
 * life of the program.
 */
static const struct bind_attribute window_attributes[] = {
	{ "parent", bind_frame_parent, NULL },
	{ "top", bind_frame_top, NULL },
	{ "frames", bind_frame_self, NULL },
	{ "closed", bind_frame_closed, NULL },
	{ "frameElement", bind_frame_element, NULL },
	{ "innerWidth", window_inner_width, NULL },
	{ "innerHeight", window_inner_height, NULL },
	{ "scrollX", bind_window_scroll_x, NULL },
	{ "scrollY", bind_window_scroll_y, NULL },
	{ "pageXOffset", bind_window_scroll_x, NULL },
	{ "pageYOffset", bind_window_scroll_y, NULL },
	{ "localStorage", bind_local_storage, NULL },
	{ "sessionStorage", bind_session_storage, NULL },
	{ NULL, NULL, NULL }
};

/*
 * The operations of Window's prototype: the timers and queueMicrotask.
 * The table is constant for the life of the program.
 */
static const struct bind_operation window_operations[] = {
	{ "setTimeout", 1, bind_set_timeout },
	{ "setInterval", 1, bind_set_interval },
	{ "clearTimeout", 0, bind_clear_timer },
	{ "clearInterval", 0, bind_clear_timer },
	{ "requestAnimationFrame", 1, bind_request_animation_frame },
	{ "cancelAnimationFrame", 1, bind_clear_timer },
	{ "queueMicrotask", 1, window_queue_microtask },
	{ "postMessage", 1, window_post_message },
	{ "getComputedStyle", 1, bind_get_computed_style },
	{ "scrollTo", 0, bind_window_scroll_to },
	{ "scroll", 0, bind_window_scroll_to },
	{ "scrollBy", 0, bind_window_scroll_by },
	{ NULL, 0, NULL }
};

/*
 * The Window interface, whose prototype the global object gets.
 */
const struct bind_interface bind_window_interface = {
	"Window", BIND_EVENT_TARGET, 0, NULL, window_attributes, window_operations, NULL
};

/*
 * The interface tables in the order of enum bind_interface_index.  The
 * table is constant for the life of the program.
 */
static const struct bind_interface *const window_interfaces[BIND_INTERFACES] = {
	&bind_event_target_interface,
	&bind_node_interface,
	&bind_character_data_interface,
	&bind_text_interface,
	&bind_cdata_interface,
	&bind_pi_interface,
	&bind_comment_interface,
	&bind_document_interface,
	&bind_xml_document_interface,
	&bind_dom_implementation_interface,
	&bind_document_type_interface,
	&bind_document_fragment_interface,
	&bind_element_interface,
	&bind_html_element_interface,
	&bind_window_interface,
	&bind_event_interface,
	&bind_ui_event_interface,
	&bind_mouse_event_interface,
	&bind_custom_event_interface,
	&bind_message_event_interface,
	&bind_keyboard_event_interface,
	&bind_focus_event_interface,
	&bind_wheel_event_interface,
	&bind_navigator_interface,
	&bind_screen_interface,
	&bind_performance_interface,
	&bind_location_interface,
	&bind_html_image_element_interface,
	&bind_html_script_element_interface,
	&bind_dom_token_list_interface,
	&bind_dom_string_map_interface,
	&bind_dom_rect_interface,
	&bind_css_style_declaration_interface,
	&bind_text_encoder_interface,
	&bind_text_decoder_interface,
	&bind_storage_interface,
	&bind_html_template_element_interface,
	&bind_dom_exception_interface,
	&bind_html_iframe_element_interface,
	&bind_html_button_element_interface,
	&bind_html_label_element_interface,
	&bind_html_meta_element_interface,
	&bind_html_object_element_interface,
	&bind_tree_walker_interface,
	&bind_node_iterator_interface,
	&bind_html_collection_interface,
	&bind_html_form_element_interface,
	&bind_html_form_controls_collection_interface,
	&bind_node_list_interface,
	&bind_radio_node_list_interface,
	&bind_html_input_element_interface,
	&bind_submit_event_interface,
	&bind_html_table_element_interface,
	&bind_html_table_section_element_interface,
	&bind_html_table_row_element_interface,
	&bind_html_table_caption_element_interface,
	&bind_html_select_element_interface,
	&bind_html_option_element_interface,
	&bind_html_opt_group_element_interface,
	&bind_html_options_collection_interface,
	&bind_abstract_range_interface,
	&bind_range_interface,
	&bind_html_style_element_interface,
	&bind_style_sheet_interface,
	&bind_css_style_sheet_interface,
	&bind_style_sheet_list_interface,
	&bind_css_rule_list_interface,
	&bind_svg_element_interface,
	&bind_svg_graphics_element_interface,
	&bind_svg_text_content_element_interface,
	&bind_svg_text_positioning_element_interface,
	&bind_svg_text_element_interface,
	&bind_svg_tspan_element_interface,
	&bind_svg_text_path_element_interface,
	&bind_svg_geometry_element_interface,
	&bind_svg_rect_element_interface,
	&bind_svg_animated_length_interface,
	&bind_svg_length_interface,
	&bind_html_media_element_interface,
	&bind_html_video_element_interface,
	&bind_html_audio_element_interface,
	&bind_html_text_area_element_interface
};

static int window_make_interface(struct bind_window *window, int index, struct vm_function **constructors);
static int window_enumerate_dom_member(struct vm_realm *realm, struct vm_object *prototype, const char *name);
static int window_define_globals(struct bind_window *window);
static int window_make_console(struct bind_window *window);
static int window_console_write(struct vm_realm *realm, int level, const vm_value *args, unsigned count, vm_value *result);
static int window_console_log(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int window_console_info(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int window_console_warn(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int window_console_error(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int window_console_debug(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int window_deliver_message(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static void window_report_job(struct vm_realm *realm, vm_value exception, void *context);
static void window_report_exception(struct bind_window *window, vm_value exception, const char *name);
static void window_trace(struct vm_heap *heap, void *context);
static void window_release(void *context);

/*
 * Makes a realm's global object the window of a document: the interfaces,
 * the window's properties, the console and the timers.
 *
 * The realm must have its built-ins.  The window keeps the host's
 * callbacks; the realm and the document must outlive it.
 */
int
bind_window_create(
	struct vm_realm *realm,
	struct dom_document *document,
	const struct bind_host *host,
	struct bind_window **window)
{
	struct bind_window *made;
	int error;

	/* Collectible hosts cannot leave asynchronous callbacks in the embedder. */
	*window = NULL;
	if (realm->managed && host->fetch != NULL)
		return ENOTSUP;

	/* A managed realm and document belong to exactly one live window. */
	if (realm->managed && realm->host != NULL)
		return EINVAL;
	if (realm->managed && document->context != NULL)
		return EINVAL;

	/* Allocates the window's record before publishing any host state. */
	made = calloc(1, sizeof(*made));
	if (made == NULL)
		return ENOMEM;

	/* Its realm, document and host, a time of zero and the state of a document being parsed. */
	made->realm = realm;
	made->document = document;
	made->host = *host;
	made->next_timer_id = 1;
	made->ready_state = "loading";
	wb_vector_init(&made->timers, sizeof(struct bind_timer));

	/* Lets the heap see the cells the window holds. */
	error = vm_heap_add_tracer(realm->heap, window_trace, made);
	if (error != 0) {
		free(made);
		return error;
	}

	/* The natives find the window through the realm. */
	realm->host = made;

	/* The interfaces and the window's own properties. */
	error = bind_window_install(made);
	if (error != 0) {
		bind_window_destroy(made);
		return error;
	}

	/* Transfers permanent tracing to the collectible realm only after install. */
	if (realm->managed) {
		made->owned = 1;
		realm->host_trace = window_trace;
		realm->host_release = window_release;
		document->context = &realm->cell;
		vm_heap_remove_tracer(realm->heap, window_trace, made);
	}

	/* Installs opaque Document view/removal hooks after complete construction. */
	bind_frames_attach(made);

	/* Succeeded: the global object is the document's window. */
	*window = made;
	return 0;
}

/*
 * Destroys a window's record (its objects are left to the collector).
 */
void
bind_window_destroy(
	struct bind_window *window)
{
	/* A missing window has no resources; owned windows are finalized by GC. */
	if (window == NULL || window->owned)
		return;

	/* Retires child contexts while the primary Document cells are still live. */
	bind_frames_release(window);

	/* Releases the explicitly owned primary record. */
	window_release(window);

	/* Succeeded: the primary host no longer retains any cells. */
	return;
}

/*
 * Cancels a detached context's queued work while keeping saved objects usable.
 */
void
bind_window_detach(
	struct bind_window *window)
{
	/* Repeated retirement must not free state twice. */
	if (window == NULL || window->detached)
		return;

	/* Detached contexts no longer deliver tasks, jobs or observer callbacks. */
	window->detached = 1;
	bind_frames_detach(window);

	/* This context no longer delivers its own queued tasks. */
	window->timers.count = 0;
	window->realm->jobs.count = 0;
	window->realm->rejections.count = 0;
	bind_environment_disconnect(window);

	/* Succeeded: saved Window/Document/function references retain quiet state. */
	return;
}

/*
 * Tells the window what time it is, in milliseconds since the page began
 * (the timers and the events' time stamps count from there).
 */
void
bind_window_set_time(
	struct bind_window *window,
	double now)
{
	/* The time only moves forward. */
	if (now > window->now)
		window->now = now;
}

/*
 * Tells the window the size of the viewport, in CSS pixels
 * (innerWidth and innerHeight).
 */
void
bind_window_set_viewport(
	struct bind_window *window,
	int width,
	int height)
{
	/* The two sizes. */
	window->viewport_width = width;
	window->viewport_height = height;
}

/*
 * Sets the document's readiness (document.readyState): "loading",
 * "interactive" or "complete", a static string.
 */
void
bind_window_set_ready_state(
	struct bind_window *window,
	const char *state)
{
	/* The string itself is kept. */
	window->ready_state = state;
}

/*
 * Runs a classic script's source text in the window, then a microtask
 * checkpoint.
 *
 * A syntax error or an exception is reported to the console, as a browser
 * reports an uncaught error, and is not a failure; name says where the
 * script came from.  Returns 0, or ENOMEM.
 */
int
bind_run_script(
	struct bind_window *window,
	const uint16_t *source,
	size_t length,
	const char *name)
{
	struct js_syntax_error syntax;
	struct wb_buffer line;
	vm_value completion;
	int status;

	/* Runs the script. */
	status = js_run_script(window->realm, source, length, 0, &completion, &syntax);

	/* A script that does not parse, or uses what the engine does not support yet, is reported. */
	if (status == EINVAL) {
		wb_buffer_init(&line);
		status = wb_buffer_printf(&line, "Uncaught SyntaxError: %s (%s:%u:%u)", syntax.message, name,
		    (unsigned)syntax.line, (unsigned)syntax.column);
		if (status == 0)
			bind_console(window, BIND_CONSOLE_ERROR, wb_buffer_string(&line));
		wb_buffer_release(&line);
	} else if (status == VM_THROWN) {
		/* An exception is reported and cleared. */
		window_report_exception(window, window->realm->exception, name);
		window->realm->exception = VM_VALUE_UNDEFINED;
		status = 0;
	}

	/* Running out of memory stops the page. */
	if (status == ENOMEM)
		return status;

	/* The microtasks the script queued. */
	status = bind_checkpoint(window);
	if (status != 0)
		return status;

	/* Succeeded: the script ran, or its error was reported. */
	return 0;
}

/*
 * Runs a microtask checkpoint: every queued microtask, each exception
 * reported to the console.
 */
int
bind_checkpoint(
	struct bind_window *window)
{
	int queued;
	int status;

	/* A detached context never delivers work queued by later saved functions. */
	if (window->detached) {
		window->realm->jobs.count = 0;
		window->realm->rejections.count = 0;
		return 0;
	}

	/* Runs the jobs already queued by the script. */
	status = vm_run_jobs(window->realm, window_report_job, window);
	if (status != 0)
		return status;

	/* DOM changes notify their observers as microtasks; a callback may make another change. */
	for (;;) {
		status = bind_environment_checkpoint(window, &queued);
		if (status != 0 || !queued)
			break;
		status = vm_run_jobs(window->realm, window_report_job, window);
		if (status != 0)
			break;
	}

	/* A callback failure ends the checkpoint. */
	if (status != 0)
		return status;

	/* A page may now run tasks queued by DOM changes, but not from a nested script. */
	if (window->realm->depth == 0 && window->host.checkpoint != NULL) {
		status = window->host.checkpoint(window->host.context);
		if (status != 0)
			return status;
	}

	/* Succeeded: the queue is empty. */
	return 0;
}

/*
 * Reports an uncaught exception to the console ("Uncaught " and the
 * exception's string, then "(at LINE:COLUMN)" in the script it was thrown
 * from when that place is known).
 */
void
bind_report_exception(
	struct bind_window *window,
	vm_value exception)
{
	window_report_exception(window, exception, NULL);
}

/* Reports an exception, including the current script's name when it is known. */
static void
window_report_exception(
	struct bind_window *window,
	vm_value exception,
	const char *name)
{
	struct wb_buffer line;
	uint32_t site_line;
	uint32_t site_column;
	int known;
	int error;

	/* The line: the prefix (a promise's rejection nothing handled says so, ws074-p086) and the exception's text. */
	wb_buffer_init(&line);
	if (window->realm->reporting_rejection) {
		error = wb_buffer_append_string(&line, "Uncaught (in promise) ");
	} else {
		error = wb_buffer_append_string(&line, "Uncaught ");
	}

	/* The exception's text. */
	if (error == 0)
		error = js_exception_text(window->realm, exception, &line);

	/* The place the exception was thrown from, which a page's author needs to find the fault. */
	known = vm_throw_site(window->realm, exception, &site_line, &site_column);
	if (error == 0 && known && name != NULL)
		error = wb_buffer_printf(&line, " (at %s:%u:%u)", name, (unsigned)site_line, (unsigned)site_column);
	if (error == 0 && known && name == NULL)
		error = wb_buffer_printf(&line, " (at %u:%u)", (unsigned)site_line, (unsigned)site_column);

	/* Goes to the console as an error. */
	if (error == 0)
		bind_console(window, BIND_CONSOLE_ERROR, wb_buffer_string(&line));
	wb_buffer_release(&line);
}

/*
 * Writes a line of text to the host's console at a level.
 */
void
bind_console(
	struct bind_window *window,
	int level,
	const char *text)
{
	/* A host without a console drops the line. */
	if (window->detached || window->host.console == NULL)
		return;

	/* Hands the line over. */
	window->host.console(window->host.context, level, text, strlen(text));
}

/*
 * Reports the window a realm is the global object of.
 */
struct bind_window *
bind_window_of(
	struct vm_realm *realm)
{
	/* The realm keeps it as its host's data. */
	return realm->host;
}

/*
 * Makes the interfaces and the window's own properties on the global
 * object.
 */
int
bind_window_install(
	struct bind_window *window)
{
	struct vm_function *constructors[BIND_INTERFACES];
	int index;
	int error;

	/* The interfaces, parents first. */
	for (index = 0; index < BIND_INTERFACES; index++) {
		error = window_make_interface(window, index, constructors);
		if (error != 0)
			return error;
	}

	/* The global object is a Window. */
	window->realm->global->prototype = window->prototypes[BIND_WINDOW];

	/* Its own properties: window, self, document and the console. */
	error = window_define_globals(window);
	if (error != 0)
		return error;

	/* Succeeded: the window is installed. */
	return 0;
}

/*
 * Throws the TypeError of a method called on an object of the wrong kind.
 */
int
bind_throw_illegal(
	struct vm_realm *realm)
{
	int status;

	/* The error browsers throw. */
	status = vm_throw_type_error(realm, "Illegal invocation");

	/* Reports the throw. */
	return status;
}

/*
 * Throws the TypeError of an interface that cannot be constructed (and of
 * any interface object called without new).
 */
int
bind_illegal_constructor(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The error browsers throw. */
	*result = VM_VALUE_UNDEFINED;
	status = vm_throw_type_error(realm, "Illegal constructor");

	/* Reports the throw. */
	return status;
}

/*
 * Makes a string value from ASCII text.
 */
int
bind_string(
	struct vm_realm *realm,
	const char *ascii,
	vm_value *value)
{
	struct vm_string *string;

	/* The string's cell. */
	string = vm_string_from_latin1(realm->heap, (const unsigned char *)ascii, strlen(ascii));
	if (string == NULL)
		return ENOMEM;

	/* Succeeded: the value is the string. */
	*value = vm_value_cell(string);
	return 0;
}

/*
 * Makes a string value from UTF-16 code units.
 */
int
bind_units(
	struct vm_realm *realm,
	const uint16_t *units,
	size_t length,
	vm_value *value)
{
	struct vm_string *string;

	/* The string's cell. */
	string = vm_string_from_units(realm->heap, units, length);
	if (string == NULL)
		return ENOMEM;

	/* Succeeded: the value is the string. */
	*value = vm_value_cell(string);
	return 0;
}

/*
 * Converts an argument to a string (WebIDL's DOMString).
 */
int
bind_to_string(
	struct vm_realm *realm,
	vm_value value,
	struct vm_string **string)
{
	int status;

	/* JavaScript's ToString. */
	status = vm_to_string(realm, value, string);
	if (status != 0)
		return status;

	/* Succeeded: the string is converted. */
	return 0;
}

/*
 * Converts an argument to a string and interns it, folded to ASCII lower
 * case when lower is set (an HTML element's name or attribute name).
 */
int
bind_to_atom(
	struct vm_realm *realm,
	vm_value value,
	int lower,
	struct vm_string **atom)
{
	struct vm_string *string;
	struct wb_units units;
	uint16_t unit;
	size_t index;
	int status;

	/* The string. */
	status = vm_to_string(realm, value, &string);
	if (status != 0)
		return status;

	/* Its units, folded when asked. */
	wb_units_init(&units);
	status = vm_string_append_units(string, &units);
	if (status != 0) {
		wb_units_release(&units);
		return status;
	}

	/* Folds A to Z to a to z (other characters stay). */
	for (index = 0; lower && index < units.length; index++) {
		unit = units.data[index];
		if (unit >= 'A' && unit <= 'Z')
			units.data[index] = (uint16_t)(unit + 0x20U);
	}

	/* The atom. */
	*atom = vm_atom_from_units(realm->heap, units.data, units.length);
	wb_units_release(&units);
	if (*atom == NULL)
		return ENOMEM;

	/* Succeeded: the atom is found or made. */
	return 0;
}

/*
 * Reads a member of an options dictionary (an object, or undefined for
 * none): *present says whether it is there and not undefined.
 */
int
bind_get_option(
	struct vm_realm *realm,
	vm_value options,
	const char *name,
	int *present,
	vm_value *value)
{
	vm_value key;
	int is_object;
	int status;

	/* Absent unless the dictionary has it. */
	*present = 0;
	*value = VM_VALUE_UNDEFINED;
	is_object = vm_value_is_object(options);
	if (!is_object)
		return 0;

	/* The member's value. */
	key = vm_key_from_ascii(realm->heap, name);
	if (key == VM_VALUE_EMPTY)
		return ENOMEM;
	status = vm_get(realm, options, key, value);
	if (status != 0)
		return status;

	/* Succeeded: the member is read. */
	if (*value != VM_VALUE_UNDEFINED)
		*present = 1;
	return 0;
}

/* Refines a just-published native IDL member's enumerable flag without changing its native value or other flags. */
static int
window_enumerate_dom_member(
	struct vm_realm *realm,
	struct vm_object *prototype,
	const char *name)
{
	struct vm_property property;
	vm_value key;
	int found;
	int status;

	/* The existing native member owns its actual key and accessor or method value. */
	key = vm_key_from_ascii(realm->heap, name);
	if (key == VM_VALUE_EMPTY)
		return ENOMEM;

	/* A failed lookup cannot publish a partially refined native interface. */
	found = vm_object_get_own(prototype, key, &property);
	if (found < 0)
		return -found;

	/* A missing generated member indicates incomplete native interface publication. */
	if (found == 0)
		return EINVAL;

	/* Preserve genuine function/accessor identities and every descriptor flag except enumerability. */
	status = vm_object_define(realm->heap, prototype, key, *property.value, property.attributes | VM_PROPERTY_ENUMERABLE);
	if (status != 0)
		return status;

	/* Succeeded: the scoped Range member has its required native enumerable descriptor. */
	return 0;
}

/* Makes one interface's object and prototype from its table. */
static int
window_make_interface(
	struct bind_window *window,
	int index,
	struct vm_function **constructors)
{
	const struct bind_interface *table;
	const struct bind_attribute *attribute;
	const struct bind_operation *operation;
	const struct bind_constant *constant;
	struct vm_realm *realm;
	struct vm_object *parent;
	struct vm_object *prototype;
	struct vm_function *constructor;
	vm_native construct;
	int error;

	/* The prototype inherits from the parent's, or from Object.prototype. */
	table = window_interfaces[index];
	realm = window->realm;
	parent = realm->object_prototype;
	if (table->parent != BIND_NO_PARENT)
		parent = window->prototypes[table->parent];

	/* DOMException's inherits from Error.prototype (ws074-p083), when the realm has it. */
	if (index == BIND_DOM_EXCEPTION && realm->intrinsics[VM_INTRINSIC_ERROR_PROTOTYPE] != NULL)
		parent = realm->intrinsics[VM_INTRINSIC_ERROR_PROTOTYPE];
	prototype = vm_object_create(realm->heap, parent);
	if (prototype == NULL)
		return ENOMEM;
	window->prototypes[index] = prototype;

	/* The attributes, accessors on the prototype. */
	for (attribute = table->attributes; attribute != NULL && attribute->name != NULL; attribute++) {
		error = js_builtin_accessor(realm, prototype, attribute->name, attribute->getter, attribute->setter);
		if (error != 0)
			return error;

		/* Range's native readonly IDL attributes enumerate on their relevant prototype. */
		if (index == BIND_ABSTRACT_RANGE || index == BIND_RANGE) {
			error = window_enumerate_dom_member(realm, prototype, attribute->name);
			if (error != 0)
				return error;
		}

	}

	/* The operations, methods on the prototype. */
	for (operation = table->operations; operation != NULL && operation->name != NULL; operation++) {
		error = js_builtin_method(realm, prototype, operation->name, operation->length, operation->method);
		if (error != 0)
			return error;

		/* Range and the concrete Text split use the enumerable writable configurable WebIDL descriptor. */
		if (index == BIND_RANGE || index == BIND_TEXT) {
			error = window_enumerate_dom_member(realm, prototype, operation->name);
			if (error != 0)
				return error;
		}
	}

	/* HTML elements, the document and the window have the event handler attributes. */
	if (index == BIND_HTML_ELEMENT || index == BIND_DOCUMENT || index == BIND_WINDOW) {
		error = bind_define_handlers(window, prototype);
		if (error != 0)
			return error;
	}

	/* The interface object: new runs the interface's constructor, or throws like a call does. */
	construct = table->construct;
	if (construct == NULL)
		construct = bind_illegal_constructor;
	error = js_builtin_constructor(realm, table->name, table->length, bind_illegal_constructor, construct, prototype, &constructor);
	if (error != 0)
		return error;
	constructors[index] = constructor;

	/* The interface object inherits from its parent's (Object.getPrototypeOf(HTMLElement) is Element). */
	if (table->parent != BIND_NO_PARENT)
		constructor->object.prototype = &constructors[table->parent]->object;

	/* The constants, on both the interface object and the prototype. */
	for (constant = table->constants; constant != NULL && constant->name != NULL; constant++) {
		error = js_builtin_value(realm, &constructor->object, constant->name, vm_value_int32(constant->value), VM_PROPERTY_ENUMERABLE);
		if (error != 0)
			return error;
		error = js_builtin_value(realm, prototype, constant->name, vm_value_int32(constant->value), VM_PROPERTY_ENUMERABLE);
		if (error != 0)
			return error;
	}

	/* Succeeded: the interface is on the global object. */
	return 0;
}

/* Defines the global object's own properties: window, self, document and console. */
static int
window_define_globals(
	struct bind_window *window)
{
	struct vm_realm *realm;
	vm_value document;
	int error;

	/* window cannot be changed; self can be replaced. */
	realm = window->realm;
	error = js_builtin_value(realm, realm->global, "window", vm_value_cell(realm->global), VM_PROPERTY_ENUMERABLE);
	if (error != 0)
		return error;
	error = js_builtin_value(realm, realm->global, "self", vm_value_cell(realm->global), VM_PROPERTY_DEFAULT);
	if (error != 0)
		return error;

	/* document cannot be changed either. */
	error = bind_wrap(window, &window->document->node, &document);
	if (error != 0)
		return error;
	error = js_builtin_value(realm, realm->global, "document", document, VM_PROPERTY_ENUMERABLE);
	if (error != 0)
		return error;

	/* The console namespace. */
	error = window_make_console(window);
	if (error != 0)
		return error;

	/* navigator, screen, performance, location, Image and the window's plain properties. */
	error = bind_environment_install(window);
	if (error != 0)
		return error;

	/* DOMRect's members (ws074-p031). */
	error = bind_geometry_install(window);
	if (error != 0)
		return error;

	/* The callback-interface namespace supplies unsigned traversal constants. */
	error = bind_traversal_install(window);
	if (error != 0)
		return error;

	/* The inline style's accessors (ws074-p031). */
	error = bind_style_install(window);
	if (error != 0)
		return error;

	/* Succeeded: the window's properties are defined. */
	return 0;
}

/* Makes the console object with its methods and puts it on the global object. */
static int
window_make_console(
	struct bind_window *window)
{
	struct vm_realm *realm;
	struct vm_object *console;
	int error;

	/* The namespace object. */
	realm = window->realm;
	console = vm_object_create(realm->heap, realm->object_prototype);
	if (console == NULL)
		return ENOMEM;
	window->console = console;

	/* Its methods, one per level. */
	error = js_builtin_method(realm, console, "log", 0, window_console_log);
	if (error == 0)
		error = js_builtin_method(realm, console, "info", 0, window_console_info);
	if (error == 0)
		error = js_builtin_method(realm, console, "warn", 0, window_console_warn);
	if (error == 0)
		error = js_builtin_method(realm, console, "error", 0, window_console_error);
	if (error == 0)
		error = js_builtin_method(realm, console, "debug", 0, window_console_debug);
	if (error != 0)
		return error;

	/* The global property. */
	error = js_builtin_value(realm, realm->global, "console", vm_value_cell(console), JS_BUILTIN_METHOD);
	if (error != 0)
		return error;

	/* Succeeded: the console is defined. */
	return 0;
}

/* Writes a console method's arguments as one line at a level: their strings, a space between them. */
static int
window_console_write(
	struct vm_realm *realm,
	int level,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct wb_buffer line;
	struct vm_string *string;
	unsigned index;
	int status;

	/* Each argument's string, a space between them. */
	*result = VM_VALUE_UNDEFINED;
	window = bind_window_of(realm);
	wb_buffer_init(&line);
	for (index = 0; index < count; index++) {
		status = vm_to_string(realm, args[index], &string);
		if (status == 0 && index > 0)
			status = wb_buffer_append_string(&line, " ");
		if (status == 0 && line.length < WINDOW_CONSOLE_MAX)
			status = vm_string_to_utf8(string, &line);
		if (status != 0) {
			wb_buffer_release(&line);
			return status;
		}
	}

	/* The line goes to the host. */
	bind_console(window, level, wb_buffer_string(&line));
	wb_buffer_release(&line);

	/* Succeeded: the console methods return undefined. */
	return 0;
}

/* Writes a line at the log level (console.log). */
static int
window_console_log(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(this_value);

	/* The line at its level. */
	status = window_console_write(realm, BIND_CONSOLE_LOG, args, count, result);
	if (status != 0)
		return status;

	/* Succeeded: the line is written. */
	return 0;
}

/* Writes a line at the info level (console.info). */
static int
window_console_info(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(this_value);

	/* The line at its level. */
	status = window_console_write(realm, BIND_CONSOLE_INFO, args, count, result);
	if (status != 0)
		return status;

	/* Succeeded: the line is written. */
	return 0;
}

/* Writes a line at the warning level (console.warn). */
static int
window_console_warn(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(this_value);

	/* The line at its level. */
	status = window_console_write(realm, BIND_CONSOLE_WARN, args, count, result);
	if (status != 0)
		return status;

	/* Succeeded: the line is written. */
	return 0;
}

/* Writes a line at the error level (console.error). */
static int
window_console_error(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(this_value);

	/* The line at its level. */
	status = window_console_write(realm, BIND_CONSOLE_ERROR, args, count, result);
	if (status != 0)
		return status;

	/* Succeeded: the line is written. */
	return 0;
}

/* Writes a line at the debug level (console.debug). */
static int
window_console_debug(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(this_value);

	/* The line at its level. */
	status = window_console_write(realm, BIND_CONSOLE_DEBUG, args, count, result);
	if (status != 0)
		return status;

	/* Succeeded: the line is written. */
	return 0;
}

/* Queues a function as a microtask (queueMicrotask). */
static int
window_queue_microtask(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	vm_value callback;
	int callable;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The argument must be a function. */
	*result = VM_VALUE_UNDEFINED;
	callback = js_argument(args, count, 0);
	callable = vm_value_is_callable(callback);
	if (!callable) {
		status = vm_throw_type_error(realm, "Failed to execute 'queueMicrotask': parameter 1 is not of type 'Function'.");
		return status;
	}

	/* The job: the function, called with undefined. */
	status = vm_enqueue_job(realm, callback, VM_VALUE_UNDEFINED);
	if (status != 0)
		return status;

	/* Succeeded: the microtask waits for the next checkpoint. */
	return 0;
}

/*
 * Queues a message for the callee's Window (postMessage).
 * Delivery is a later task; source/origin transport between child contexts
 * remains outside the initial about:blank context interface.
 */
static int
window_post_message(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct bind_event *event;
	struct vm_function *deliver;
	struct vm_string *type;
	struct vm_string *target_origin;
	struct vm_string *origin;
	struct vm_cell *roots[4];
	vm_value option;
	vm_value origin_value;
	vm_value event_value;
	vm_value timer_args[2];
	vm_value ignored;
	int present;
	int is_object;
	int option_cell;
	int allowed;
	int status;
	unsigned registered;
	unsigned index;

	UNUSED_PARAMETER(this_value);

	/* The optional target origin is a string, or the field of the options overload. */
	*result = VM_VALUE_UNDEFINED;
	window = bind_window_of(realm);
	option = js_argument(args, count, 1);
	target_origin = NULL;
	is_object = vm_value_is_object(option);
	if (count >= 2U && option != VM_VALUE_UNDEFINED && is_object) {
		status = bind_get_option(realm, option, "targetOrigin", &present, &option);
		if (status != 0)
			return status;
		if (!present)
			option = VM_VALUE_UNDEFINED;
	}

	/* The options value, origin, event and delivery function survive allocations until the timer owns them. */
	roots[0] = NULL;
	option_cell = vm_value_is_cell(option);
	if (option_cell)
		roots[0] = vm_value_as_cell(option);
	roots[1] = NULL;
	roots[2] = NULL;
	roots[3] = NULL;
	registered = 0;
	for (index = 0; index < 4U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	/* The string overload and the options field use the same origin syntax. */
	if (option != VM_VALUE_UNDEFINED) {
		status = bind_to_string(realm, option, &target_origin);
		if (status != 0)
			goto cleanup;
		roots[0] = &target_origin->cell;
	}

	/* '*' and '/' allow this window; an exact origin does too. */
	status = bind_location_part(window, BIND_LOCATION_ORIGIN, &origin_value);
	if (status != 0)
		goto cleanup;
	origin = (struct vm_string *)vm_value_as_cell(origin_value);
	roots[1] = &origin->cell;
	allowed = target_origin == NULL || vm_string_equal_ascii(target_origin, "*") ||
	    vm_string_equal_ascii(target_origin, "/") || vm_string_equal(target_origin, origin);
	if (!allowed) {
		status = 0;
		goto cleanup;
	}

	/* The message event carries the value in this realm and names this page as its source. */
	type = vm_atom_from_ascii(realm->heap, "message");
	if (type == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* The new event carries this realm's message type. */
	status = bind_event_create(window, BIND_MESSAGE_EVENT, type, &event_value, &event);
	if (status != 0)
		goto cleanup;
	roots[2] = vm_value_as_cell(event_value);
	event->detail = js_argument(args, count, 0);
	event->origin = origin;
	event->source = vm_value_cell(realm->global);
	event->trusted = 1;
	status = js_builtin_array(realm, NULL, 0, &event->ports);
	if (status != 0)
		goto cleanup;

	/* A zero-delay timer supplies the task boundary and keeps the event alive. */
	deliver = vm_function_create_native(realm, "deliver message", 0, window_deliver_message);
	if (deliver == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Hold the callback until the timer retains it. */
	roots[3] = &deliver->object.cell;
	deliver->data = event_value;
	timer_args[0] = vm_value_cell(deliver);
	timer_args[1] = vm_value_int32(0);
	status = bind_set_timeout(realm, vm_value_cell(realm->global), timer_args, 2, &ignored);

cleanup:
	/* Release exactly the temporary roots registered by this call. */
	while (registered > 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Return any error after releasing every temporary root. */
	if (status != 0)
		return status;

	/* Succeeded: the message is waiting in the task queue. */
	return 0;
}

/* Dispatches the MessageEvent kept by a postMessage task. */
static int
window_deliver_message(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	vm_value event_value;
	int canceled;
	int status;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The timer callback's private value is the event made by postMessage. */
	*result = VM_VALUE_UNDEFINED;
	window = bind_window_of(realm);
	event_value = js_builtin_callee(realm)->data;
	status = bind_dispatch(window, vm_value_cell(realm->global), event_value, &canceled);
	if (status != 0)
		return status;

	/* Succeeded: every listener has seen the message. */
	return 0;
}

/* Reports the viewport's width (innerWidth). */
static int
window_inner_width(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	int status;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The size the page gave the window. */
	window = bind_window_of(realm);
	status = bind_frame_viewport(window);
	if (status != 0)
		return status;
	*result = vm_value_int32(window->viewport_width);

	/* Succeeded: the width is reported. */
	return 0;
}

/* Reports the viewport's height (innerHeight). */
static int
window_inner_height(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	int status;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The size the page gave the window. */
	window = bind_window_of(realm);
	status = bind_frame_viewport(window);
	if (status != 0)
		return status;
	*result = vm_value_int32(window->viewport_height);

	/* Succeeded: the height is reported. */
	return 0;
}

/* Reports an exception a microtask threw (the checkpoint's report). */
static void
window_report_job(
	struct vm_realm *realm,
	vm_value exception,
	void *context)
{
	struct bind_window *window;

	UNUSED_PARAMETER(realm);

	/* To the console, as uncaught. */
	window = context;
	bind_report_exception(window, exception);
}

/* Releases host C state without reading any potentially finalized DOM cells. */
static void
window_release(
	void *context)
{
	struct bind_window *window;

	/* Removes registrations while the C realm and heap tables still exist. */
	window = context;
	html_parser_destroy(window->document_parser);
	bind_environment_release(window);
	bind_style_context_release(window);
	if (window->realm->host == window)
		window->realm->host = NULL;
	window->realm->host_trace = NULL;
	window->realm->host_release = NULL;
	vm_heap_remove_tracer(window->realm->heap, window_trace, window);

	/* Timer storage contains values, but releasing it reads none of them. */
	wb_vector_release(&window->timers);
	free(window);

	/* Succeeded: the owner may now release its VM stack. */
	return;
}

/* Marks the cells the window holds: the prototypes, the console, its listeners and the timers. */
static void
window_trace(
	struct vm_heap *heap,
	void *context)
{
	struct bind_window *window;
	int index;

	/* The owned Document remains available even if global.document was removed. */
	window = context;
	vm_heap_mark(heap, &window->document->node.cell);
	html_parser_trace_owned(heap, window->document_parser);
	bind_environment_trace(heap, window);

	/* Initial blank fallback metadata belongs to this managed owner. */
	if (window->base_url != NULL)
		vm_heap_mark(heap, &window->base_url->cell);

	/* Saved child Windows retain their ancestors and their connected frame. */
	if (window->parent_global != NULL)
		vm_heap_mark(heap, &window->parent_global->cell);
	if (window->top_global != NULL)
		vm_heap_mark(heap, &window->top_global->cell);
	if (window->frame != NULL)
		vm_heap_mark(heap, &window->frame->node.cell);

	/* The interfaces keep every saved wrapper's native methods alive. */
	for (index = 0; index < BIND_INTERFACES; index++) {
		if (window->prototypes[index] != NULL)
			vm_heap_mark(heap, &window->prototypes[index]->cell);
	}

	/* The console, the location, the listeners and the timers. */
	if (window->console != NULL)
		vm_heap_mark(heap, &window->console->cell);
	if (window->location != NULL)
		vm_heap_mark(heap, &window->location->cell);
	if (window->listeners != NULL)
		vm_heap_mark(heap, window->listeners);
	bind_timers_trace(heap, window);

	/* The storage objects made so far (ws074-p080). */
	if (window->local_storage != NULL)
		vm_heap_mark(heap, &window->local_storage->cell);
	if (window->session_storage != NULL)
		vm_heap_mark(heap, &window->session_storage->cell);
}
