/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The window's environment: navigator, screen, performance
 * and location, the Image constructor, and the window's other plain
 * properties (devicePixelRatio, the outer size, top and the like) that
 * page scripts read before they do anything else.
 *
 * navigator, screen, performance and location are one object each per
 * window, made from the prototypes of their interfaces.  What never
 * changes (the navigator's names and flags, the screen's depth) is a
 * data property of the prototype, so an own-property test on the object
 * fails as it does in other browsers; what changes (the screen's size,
 * the clock, the location) is an accessor.  The location is read from the
 * host (the page's URL); navigating it (the setters, history) is the
 * prelude's session history, which scrolls with bind_scroll_to_fragment.
 * navigator.sendBeacon accepts a beacon without sending it.
 */

#include "bind/internal.h"
#include "js/builtin.h"
#include "net/net.h"

#include <errno.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The processor's name in navigator.platform, as the compiler targets it. */
#if defined(__x86_64__)
#define ENVIRONMENT_MACHINE	"x86_64"
#elif defined(__aarch64__)
#define ENVIRONMENT_MACHINE	"aarch64"
#elif defined(__riscv) && __riscv_xlen == 64
#define ENVIRONMENT_MACHINE	"riscv64"
#elif defined(__i386__)
#define ENVIRONMENT_MACHINE	"i686"
#else
#define ENVIRONMENT_MACHINE	"unknown"
#endif

/* The colour depth of the screen, in bits per pixel (the surfaces are 32-bit, with 24 bits of colour). */
#define ENVIRONMENT_COLOR_DEPTH	24

/*
 * A string property of an interface's prototype that does not change for
 * the life of the program.
 */
struct environment_string {
	const char *name;
	const char *value;
};

/*
 * A number property of an interface's prototype that does not change for
 * the life of the program.
 */
struct environment_number {
	const char *name;
	double value;
};

/*
 * A Boolean property of an interface's prototype that does not change for
 * the life of the program.
 */
struct environment_flag {
	const char *name;
	int value;
};

/*
 * One accessor whose getter reads one part of something (the part is the
 * getter function's data): its name and the part.
 */
struct environment_part {
	const char *name;
	int part;
};

/* One MutationObserver and its current target and pending records. */
struct environment_mutation_observer {
	struct vm_cell *observer;
	struct vm_cell *callback;
	struct vm_cell *target;
	struct vm_cell *records;
	int child_list;
	int subtree;
	int attributes;
	int attribute_old_value;
	struct vm_cell *attribute_filter;
	struct environment_mutation_observer *next;
};

/* Unpublished observe options borrow a caller-rooted, independently converted name filter. */
struct environment_mutation_settings {
	int child_list;
	int subtree;
	int attributes;
	int attribute_old_value;
	struct vm_cell *attribute_filter;
};

/* One ancestor selected before a beforematch handler can change the DOM. */
struct environment_reveal_entry {
	struct vm_cell *node;
	int details;
	int hidden;
};

/* A performance wrapper owns this managed buffer; its entries retain no native Window pointer. */
struct environment_timeline {
	struct vm_cell cell;
	struct wb_vector entries;
	struct vm_object *owner_global;
	struct vm_object *mark_prototype;
	struct vm_object *list_prototype;
	struct wb_vector observers;
	vm_value dispatch;
	int queued;
};

/* A saved mark independently retains its immutable name, primitive detail and original timestamp. */
struct environment_mark {
	struct vm_cell cell;
	struct vm_string *name;
	vm_value detail;
	double start;
};

/* A genuine observer owns callback and pending mark references through its independently collectible wrapper. */
struct environment_performance_observer {
	struct vm_cell cell;
	struct environment_timeline *timeline;
	struct vm_object *wrapper;
	vm_value callback;
	struct wb_vector records;
	int mode;
	int active;
	int dropped_required;
};

/* A delivered EntryList owns detached actual mark identities independently of its former observer. */
struct environment_performance_list {
	struct vm_cell cell;
	struct wb_vector entries;
};

static void environment_performance_observer_trace(struct vm_heap *heap, struct vm_cell *cell);
static void environment_performance_observer_finalize(struct vm_heap *heap, struct vm_cell *cell);
static void environment_performance_list_trace(struct vm_heap *heap, struct vm_cell *cell);
static void environment_performance_list_finalize(struct vm_heap *heap, struct vm_cell *cell);
static int environment_performance_observer_construct(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int environment_performance_observe(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int environment_performance_disconnect(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int environment_performance_take_records(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int environment_performance_list_entries(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int environment_performance_list_by_type(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int environment_performance_list_by_name(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static void environment_timeline_trace(struct vm_heap *heap, struct vm_cell *cell);
static void environment_timeline_finalize(struct vm_heap *heap, struct vm_cell *cell);
static void environment_mark_trace(struct vm_heap *heap, struct vm_cell *cell);
static int environment_mark_construct(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int environment_performance_mark(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int environment_mark_json(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int environment_performance_entries(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int environment_performance_by_type(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int environment_performance_by_name(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int environment_performance_clear_marks(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int environment_install_navigator(struct bind_window *window);
static int environment_install_screen(struct bind_window *window);
static int environment_install_location(struct bind_window *window);
static int environment_navigator_app_version(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int environment_navigator_user_agent(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int environment_navigator_languages(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int environment_navigator_list(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int environment_navigator_java_enabled(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int environment_navigator_send_beacon(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int environment_performance_now(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int environment_performance_time_origin(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int environment_performance_nothing(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int environment_location_to_string(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);

/*
 * The screen's sizes (the part is 0 for the width and 1 for the height).
 * The table is constant for the life of the program.
 */
static const struct environment_part environment_screen_sizes[] = {
	{ "width", 0 },
	{ "height", 1 },
	{ "availWidth", 0 },
	{ "availHeight", 1 },
	{ NULL, 0 }
};

/*
 * The parts of the location, each a BIND_LOCATION_* the host reads.  The
 * table is constant for the life of the program.
 */
static const struct environment_part environment_location_parts[] = {
	{ "href", BIND_LOCATION_HREF },
	{ "origin", BIND_LOCATION_ORIGIN },
	{ "protocol", BIND_LOCATION_PROTOCOL },
	{ "host", BIND_LOCATION_HOST },
	{ "hostname", BIND_LOCATION_HOSTNAME },
	{ "port", BIND_LOCATION_PORT },
	{ "pathname", BIND_LOCATION_PATHNAME },
	{ "search", BIND_LOCATION_SEARCH },
	{ "hash", BIND_LOCATION_HASH },
	{ NULL, 0 }
};

/*
 * The navigator's names that never change.  The table is constant for the
 * life of the program.
 */
static const struct environment_string environment_navigator_strings[] = {
	{ "appCodeName", "Mozilla" },
	{ "appName", "Netscape" },
	{ "platform", "Kei " ENVIRONMENT_MACHINE },
	{ "product", "Gecko" },
	{ "productSub", "20030107" },
	{ "vendor", "" },
	{ "vendorSub", "" },
	{ "language", "en-US" },
	{ NULL, NULL }
};

/*
 * The navigator's flags: cookies are kept, the network is taken to be
 * there, and nothing drives the browser.  The table is constant for the
 * life of the program.
 */
static const struct environment_flag environment_navigator_flags[] = {
	{ "cookieEnabled", 1 },
	{ "onLine", 1 },
	{ "webdriver", 0 },
	{ "pdfViewerEnabled", 0 },
	{ NULL, 0 }
};

/*
 * The navigator's numbers: one processor for the page's scripts, and no
 * touch points.  The table is constant for the life of the program.
 */
static const struct environment_number environment_navigator_numbers[] = {
	{ "hardwareConcurrency", 1.0 },
	{ "maxTouchPoints", 0.0 },
	{ NULL, 0.0 }
};

/*
 * The screen's numbers that never change.  The table is constant for the
 * life of the program.
 */
static const struct environment_number environment_screen_numbers[] = {
	{ "colorDepth", ENVIRONMENT_COLOR_DEPTH },
	{ "pixelDepth", ENVIRONMENT_COLOR_DEPTH },
	{ "availLeft", 0.0 },
	{ "availTop", 0.0 },
	{ NULL, 0.0 }
};

/*
 * The names of the moments of performance.timing, which are all the
 * moment the window was made in this pass (the loader does not report its
 * moments).  The table is constant for the life of the program.
 */
static const char *const environment_timing_names[] = {
	"navigationStart", "unloadEventStart", "unloadEventEnd", "redirectStart", "redirectEnd", "fetchStart",
	"domainLookupStart", "domainLookupEnd", "connectStart", "connectEnd", "secureConnectionStart", "requestStart",
	"responseStart", "responseEnd", "domLoading", "domInteractive", "domContentLoadedEventStart",
	"domContentLoadedEventEnd", "domComplete", "loadEventStart", "loadEventEnd", NULL
};

/*
 * The operations of Navigator.  The table is constant for the life of the
 * program.
 */
static const struct bind_operation environment_navigator_operations[] = {
	{ "javaEnabled", 0, environment_navigator_java_enabled },
	{ "sendBeacon", 1, environment_navigator_send_beacon },
	{ NULL, 0, NULL }
};

/*
 * The attributes of Navigator that depend on the host.  The table is
 * constant for the life of the program.
 */
static const struct bind_attribute environment_navigator_attributes[] = {
	{ "userAgent", environment_navigator_user_agent, NULL },
	{ "appVersion", environment_navigator_app_version, NULL },
	{ "languages", environment_navigator_languages, NULL },
	{ "plugins", environment_navigator_list, NULL },
	{ "mimeTypes", environment_navigator_list, NULL },
	{ NULL, NULL, NULL }
};

/*
 * The Navigator interface (the names and flags are added to its
 * prototype by environment_install_navigator).
 */
const struct bind_interface bind_navigator_interface = {
	"Navigator", BIND_NO_PARENT, 0, NULL, environment_navigator_attributes, environment_navigator_operations, NULL
};

/*
 * The Screen interface (its sizes and depths are added to its prototype
 * by environment_install_screen).
 */
const struct bind_interface bind_screen_interface = {
	"Screen", BIND_NO_PARENT, 0, NULL, NULL, NULL, NULL
};

/* Observer payloads retain their actual callback, relevant timeline and pending records until collection. */
static const struct vm_cell_type environment_performance_observer_type = {
	"performance-observer", environment_performance_observer_trace, environment_performance_observer_finalize
};

/* Delivered snapshots retain and release their own detached native mark vector. */
static const struct vm_cell_type environment_performance_list_type = {
	"performance-entry-list", environment_performance_list_trace, environment_performance_list_finalize
};

/* Only genuine native observer wrappers carry this private operation identity. */
static const struct vm_native_operations environment_performance_observer_native = {
	NULL, NULL, NULL, NULL, NULL
};

/* EntryList identity is independent of mutable JS prototypes and constructor properties. */
static const struct vm_native_operations environment_performance_list_native = {
	NULL, NULL, NULL, NULL, NULL
};

/* Real native registrations consume genuine produced marks and dispatch asynchronously. */
static const struct bind_operation environment_performance_observer_operations[] = {
	{ "observe", 0, environment_performance_observe },
	{ "disconnect", 0, environment_performance_disconnect },
	{ "takeRecords", 0, environment_performance_take_records },
	{ NULL, 0, NULL }
};

/* Every query returns a fresh array without exposing the delivered snapshot's native storage. */
static const struct bind_operation environment_performance_list_operations[] = {
	{ "getEntries", 0, environment_performance_list_entries },
	{ "getEntriesByType", 1, environment_performance_list_by_type },
	{ "getEntriesByName", 1, environment_performance_list_by_name },
	{ NULL, 0, NULL }
};

/* Constructing alone retains a callback without registering or manufacturing pending entries. */
const struct bind_interface bind_performance_observer_interface = {
	"PerformanceObserver", BIND_NO_PARENT, 1, environment_performance_observer_construct, NULL, environment_performance_observer_operations, NULL
};

/* Only actual asynchronous delivery can create a genuine EntryList. */
const struct bind_interface bind_performance_observer_entry_list_interface = {
	"PerformanceObserverEntryList", BIND_NO_PARENT, 0, NULL, NULL, environment_performance_list_operations, NULL
};

/* The private timeline's type traces buffered wrappers and releases native vector storage. */
static const struct vm_cell_type environment_timeline_type = {
	"performance-timeline", environment_timeline_trace, environment_timeline_finalize
};

/* A mark's independently managed payload remains readable after its owner frame retires. */
static const struct vm_cell_type environment_mark_type = {
	"performance-mark", environment_mark_trace, NULL
};

/* Distinct private native-operation identities brand ordinary extensible wrappers without virtual properties. */
static const struct vm_native_operations environment_timeline_native = {
	NULL, NULL, NULL, NULL, NULL
};

/* Entry branding cannot be obtained through a mutable JS prototype or constructor property. */
static const struct vm_native_operations environment_mark_native = {
	NULL, NULL, NULL, NULL, NULL
};

/* Getter part order is shared only by native installation and the immutable entry reader. */
static const char *const environment_entry_parts[] = {
	"name", "entryType", "startTime", "duration", "detail"
};

/* Entry serialization reports common fields without exposing mark detail or mutable native storage. */
static const struct bind_operation environment_entry_operations[] = {
	{ "toJSON", 0, environment_mark_json },
	{ NULL, 0, NULL }
};

/* PerformanceEntry instances originate from concrete native timeline producers. */
const struct bind_interface bind_performance_entry_interface = {
	"PerformanceEntry", BIND_NO_PARENT, 0, NULL, NULL, environment_entry_operations, NULL
};

/* Direct construction makes a genuine mark without adding it to any performance buffer. */
const struct bind_interface bind_performance_mark_interface = {
	"PerformanceMark", BIND_PERFORMANCE_ENTRY, 1, environment_mark_construct, NULL, NULL, NULL
};

/*
 * The attributes of Performance.  The table is constant for the life of
 * the program.
 */
static const struct bind_attribute environment_performance_attributes[] = {
	{ "timeOrigin", environment_performance_time_origin, NULL },
	{ NULL, NULL, NULL }
};

/*
 * Native mark production, selection and clearing share the real timeline.
 * Other entry producers remain outside this implementation increment.
 * The table is constant for the life of the program.
 */
static const struct bind_operation environment_performance_operations[] = {
	{ "now", 0, environment_performance_now },
	{ "getEntries", 0, environment_performance_entries },
	{ "getEntriesByType", 1, environment_performance_by_type },
	{ "getEntriesByName", 1, environment_performance_by_name },
	{ "mark", 1, environment_performance_mark },
	{ "measure", 1, environment_performance_nothing },
	{ "clearMarks", 0, environment_performance_clear_marks },
	{ "clearMeasures", 0, environment_performance_nothing },
	{ "clearResourceTimings", 0, environment_performance_nothing },
	{ "setResourceTimingBufferSize", 1, environment_performance_nothing },
	{ NULL, 0, NULL }
};

/*
 * The Performance interface.
 */
const struct bind_interface bind_performance_interface = {
	"Performance", BIND_NO_PARENT, 0, NULL, environment_performance_attributes, environment_performance_operations, NULL
};

/*
 * The operations of Location (the parts are accessors added by
 * environment_install_location).  The table is constant for the life of
 * the program.
 */
static const struct bind_operation environment_location_operations[] = {
	{ "toString", 0, environment_location_to_string },
	{ NULL, 0, NULL }
};

/*
 * The Location interface.
 */
const struct bind_interface bind_location_interface = {
	"Location", BIND_NO_PARENT, 0, NULL, NULL, environment_location_operations, NULL
};

/* Helpers not referenced by constant initializers follow the file-scope tables. */
static int environment_performance_observer_of(struct vm_realm *realm, vm_value receiver, struct environment_performance_observer **observer);
static int environment_performance_list_of(struct vm_realm *realm, vm_value receiver, struct environment_performance_list **list);
static int environment_performance_entry_types(struct vm_realm *realm, vm_value sequence, int *has_mark);
static int environment_performance_schedule(struct environment_timeline *timeline);
static int environment_performance_queue_mark(struct environment_timeline *timeline, vm_value mark);
static void environment_performance_list_sort(struct wb_vector *buffer);
static int environment_performance_list_create(struct vm_realm *realm, struct environment_timeline *timeline, struct wb_vector *records, vm_value *result);
static int environment_performance_dispatch(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int environment_performance_list_snapshot(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, int mode, vm_value *result);
static int environment_performance_supported_types(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int environment_performance_enumerate(struct vm_realm *realm, struct vm_object *prototype, const struct bind_operation *operation);
static int environment_install_performance_observer(struct bind_window *window, struct environment_timeline *timeline);
static int environment_entries_snapshot(struct vm_realm *realm, const struct wb_vector *buffer, const vm_value *args, unsigned count, int mode, vm_value *result);
static int environment_timeline_of(struct vm_realm *realm, vm_value receiver, struct environment_timeline **timeline);
static int environment_mark_of(struct vm_realm *realm, vm_value receiver, struct environment_mark **mark);
static int environment_mark_create(struct vm_realm *realm, const vm_value *args, unsigned count, struct environment_timeline *timeline, int construct, vm_value *result);
static int environment_mark_part(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int environment_performance_snapshot(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, int mode, vm_value *result);
static int environment_install_entry_tag(struct vm_realm *realm, struct vm_object *prototype, const char *name);
static int environment_install_mark_fields(struct bind_window *window);
static int environment_install_performance(struct bind_window *window);
static int environment_install_image(struct bind_window *window);
static int environment_install_observers(struct bind_window *window);
static int environment_install_window(struct bind_window *window);
static int environment_instance(struct bind_window *window, int interface, const char *global, struct vm_object **object);
static int environment_strings(struct vm_realm *realm, struct vm_object *object, const struct environment_string *table);
static int environment_numbers(struct vm_realm *realm, struct vm_object *object, const struct environment_number *table);
static int environment_flags(struct vm_realm *realm, struct vm_object *object, const struct environment_flag *table);
static int environment_part_accessor(struct vm_realm *realm, struct vm_object *object, const char *name, vm_native getter, vm_native setter, int part);
static int environment_empty_array(struct vm_realm *realm, vm_value *value);
static int environment_screen_size(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int environment_location_part(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int environment_fragment_target(struct bind_window *window, const struct wb_buffer *fragment, struct dom_node **target);
static int environment_reveal_target(struct bind_window *window, struct dom_node *target);
static int environment_hidden_until_found(const struct vm_string *value);
static int environment_open_details(struct vm_realm *realm, struct dom_element *element);
static int environment_image_construct(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int environment_outer_size(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int environment_atob(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int environment_btoa(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int environment_observer_construct(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int environment_observer_observe(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int environment_observer_disconnect(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int environment_observer_records(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int environment_observer_entry(struct bind_window *window, vm_value target, int type, vm_value *entry);
static int environment_observer_rect(struct vm_realm *realm, double x, double y, double width, double height, vm_value *result);
static int environment_observer_size(struct vm_realm *realm, double width, double height, vm_value *result);
static int environment_mutation_add(struct bind_window *window, struct vm_object *observer, vm_value callback);
static struct environment_mutation_observer *environment_mutation_find(struct bind_window *window, vm_value observer);
static void environment_mutation_clear(struct bind_window *window, struct environment_mutation_observer *observer, int discard_records);
static int environment_root(struct bind_window *window, struct vm_cell **slot);
static int environment_mutation_options(struct vm_realm *realm, vm_value options, struct environment_mutation_settings *settings, struct vm_cell **filter);
static int environment_mutation_boolean(struct vm_realm *realm, vm_value options, const char *name, int *present, int *enabled);
static int environment_mutation_filter(struct vm_realm *realm, vm_value source, struct vm_cell **filter);
static int environment_attribute_record(struct bind_window *window, struct environment_mutation_observer *observer, struct dom_element *element, struct vm_string *name, struct vm_string *old_value);
static int environment_mutation_record(struct bind_window *window, struct environment_mutation_observer *observer, struct dom_node *parent, struct dom_node *const *added, size_t added_count, struct dom_node *const *removed, size_t removed_count, struct dom_node *previous, struct dom_node *next);
static int environment_node_list(struct bind_window *window, struct dom_node *const *nodes, size_t count, vm_value *result);
static int environment_base64_value(uint16_t unit);

/*
 * Marks observer state retained by a window rather than by permanent roots.
 */
void
bind_environment_trace(
	struct vm_heap *heap,
	struct bind_window *window)
{
	struct environment_mutation_observer *observer;

	/* Managed observer cycles are collectible with their detached context. */
	for (observer = window->mutation_observers;
	     observer != NULL;
	     observer = observer->next) {
		vm_heap_mark(heap, observer->observer);
		vm_heap_mark(heap, observer->callback);
		vm_heap_mark(heap, observer->target);
		vm_heap_mark(heap, observer->records);
		vm_heap_mark(heap, observer->attribute_filter);
	}

	/* Succeeded: every registered observer's references are visible. */
	return;
}

/*
 * Disconnects pending observer deliveries without invalidating saved objects.
 */
void
bind_environment_disconnect(
	struct bind_window *window)
{
	struct environment_mutation_observer *observer;

	/* Saved observer objects can still inspect or change their backend later. */
	for (observer = window->mutation_observers;
	     observer != NULL;
	     observer = observer->next) {
		environment_mutation_clear(window, observer, 1);
	}

	/* Succeeded: there are no pending targets or records to deliver. */
	return;
}

/*
 * Makes the window's environment: navigator, screen, performance,
 * location, the Image constructor and the window's plain properties.
 */
int
bind_environment_install(
	struct bind_window *window)
{
	int error;

	/* The moment the window was made, which performance.timeOrigin reports. */
	window->time_origin = bind_epoch_milliseconds();

	/* navigator. */
	error = environment_install_navigator(window);
	if (error != 0)
		return error;

	/* screen. */
	error = environment_install_screen(window);
	if (error != 0)
		return error;

	/* performance. */
	error = environment_install_performance(window);
	if (error != 0)
		return error;

	/* location, which the document reports too. */
	error = environment_install_location(window);
	if (error != 0)
		return error;

	/* Image. */
	error = environment_install_image(window);
	if (error != 0)
		return error;

	/* The observation APIs used to reveal and resize page components. */
	error = environment_install_observers(window);
	if (error != 0)
		return error;

	/* The window's own plain properties. */
	error = environment_install_window(window);
	if (error != 0)
		return error;

	/* The pieces written in the page's own language (AbortController, URL, Intl, ...). */
	error = bind_install_prelude(window);
	if (error != 0)
		return error;

	/* Succeeded: the environment is on the global object. */
	return 0;
}

/*
 * Queues ordinary attribute snapshots for the active Document's native observer registrations.
 */
int
bind_environment_attribute_mutation(
	struct dom_document *document,
	struct dom_element *element,
	struct vm_string *name,
	struct vm_string *old_value)
{
	struct bind_window *window;
	struct vm_heap *heap;
	struct environment_mutation_observer *observer;
	struct vm_object *filter;
	struct vm_string *wanted;
	struct dom_node *target;
	struct dom_node *ancestor;
	struct vm_cell *roots[3];
	size_t position;
	unsigned index;
	unsigned registered;
	int matched;
	int allowed;
	int same;
	int status;

	/* Detached or standalone Documents have no active native callback owner. */
	window = document->view;
	if (window == NULL || window->detached)
		return 0;

	/* The DOM commit lent these retired strings without any intervening VM allocation. */
	heap = window->realm->heap;
	roots[0] = &element->node.cell;
	roots[1] = &name->cell;
	roots[2] = NULL;
	if (old_value != NULL)
		roots[2] = &old_value->cell;
	registered = 0;
	status = 0;
	for (index = 0; index < 3U; index++) {
		status = vm_heap_add_root(heap, &roots[index]);
		if (status != 0)
			break;
		registered++;
	}

	/* A single native mutation produces independent records in normal registration order. */
	if (status == 0) {
		for (observer = window->mutation_observers;
		     observer != NULL;
		     observer = observer->next) {
			/* Disabled kinds and disconnected targets cannot observe this ordinary attribute. */
			if (!observer->attributes || observer->target == NULL)
				continue;

			/* Match the current inclusive ancestors without crossing a shadow root boundary. */
			target = bind_node_of(vm_value_cell(observer->target));
			matched = 0;
			for (ancestor = &element->node;
			     ancestor != NULL;
			     ancestor = ancestor->parent) {
				/* The registered target is either this element or a permitted subtree ancestor. */
				if (ancestor == target) {
					matched = 1;
					break;
				}

				/* A non-subtree registration never consults the element's parents. */
				if (!observer->subtree)
					break;
			}

			/* A registration in a different branch has no matching record. */
			if (!matched)
				continue;

			/* A copied filter uses exact names; an empty list intentionally observes none. */
			if (observer->attribute_filter != NULL) {
				filter = (struct vm_object *)observer->attribute_filter;
				allowed = 0;
				for (position = 0; position < filter->length; position++) {
					wanted = (struct vm_string *)vm_value_as_cell(filter->elements[position]);
					same = vm_string_equal(wanted, name);
					if (same) {
						allowed = 1;
						break;
					}
				}

				/* Excluded attribute names never allocate or enqueue a record. */
				if (!allowed)
					continue;
			}

			/* Only a complete payload enters the already owned microtask record array. */
			status = environment_attribute_record(window, observer, element, name, old_value);
			if (status != 0)
				break;
		}
	}

	/* Borrowed retired strings leave temporary ownership only after every observer attempt. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(heap, &roots[registered]);
	}

	/* An allocation failure retains its precise native status after the committed mutation. */
	if (status != 0)
		return status;

	/* Succeeded: all matching ordinary attribute registrations own complete snapshots. */
	return 0;
}

/* Makes the three observation constructors used by responsive pages. */
static int
environment_install_observers(
	struct bind_window *window)
{
	static const char *const names[] = { "MutationObserver", "IntersectionObserver", "ResizeObserver" };
	struct vm_realm *realm;
	struct vm_function *constructor;
	size_t index;
	int error;

	/* Each constructor records its observer kind in its native function. */
	realm = window->realm;
	for (index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
		error = js_builtin_function(realm, names[index], 1, bind_illegal_constructor,
		    environment_observer_construct, &constructor);
		if (error != 0)
			return error;
		constructor->data = vm_value_int32((int32_t)index);
		error = js_builtin_value(realm, realm->global, names[index], vm_value_cell(constructor), JS_BUILTIN_METHOD);
		if (error != 0)
			return error;
	}

	/* Succeeded: every observer constructor is global. */
	return 0;
}

/*
 * Reads the real-time clock in milliseconds since 1970 (a whole number;
 * 0 when there is no clock).
 */
double
bind_epoch_milliseconds(void)
{
	struct timespec now;
	int status;

	/* The system's clock. */
	status = clock_gettime(CLOCK_REALTIME, &now);
	if (status != 0)
		return 0.0;

	/* Succeeded: the whole milliseconds. */
	return (double)now.tv_sec * 1000.0 + (double)(now.tv_nsec / 1000000L);
}

/*
 * Makes a string of one part of the document's location (a
 * BIND_LOCATION_*), as the host reads it from the page's URL; the empty
 * string when the host has no location.
 */
int
bind_location_part(
	struct bind_window *window,
	int part,
	vm_value *value)
{
	struct wb_buffer text;
	struct vm_string *string;
	int error;

	/* The host writes the part. */
	wb_buffer_init(&text);
	error = 0;
	if (window->owned) {
		error = bind_frame_location(window, part, &text);
	} else if (window->host.location != NULL) {
		error = window->host.location(window->host.context, part, &text);
	}

	/* A failed owned URL or host component cannot publish partial text. */
	if (error != 0) {
		wb_buffer_release(&text);
		return error;
	}

	/* The string, from the UTF-8 text. */
	string = vm_string_from_utf8(window->realm->heap, wb_buffer_string(&text), text.length);
	wb_buffer_release(&text);
	if (string == NULL)
		return ENOMEM;

	/* Succeeded: the part's string. */
	*value = vm_value_cell(string);
	return 0;
}

/*
 * Scrolls to the element a fragment names, revealing its hidden ancestors.
 *
 * The fragment is UTF-8 text with or without its leading number sign; *found
 * says whether an element has the decoded fragment as its id.
 */
int
bind_scroll_to_fragment(
	struct bind_window *window,
	const struct wb_buffer *fragment,
	int *found)
{
	struct dom_node *target;
	struct vm_cell *target_root;
	struct bind_box box;
	int error;

	*found = 0;

	/* The element the fragment names. */
	error = environment_fragment_target(window, fragment, &target);
	if (error != 0 || target == NULL)
		return error;

	/* The target stays alive while revealing runs scripts' toggle handlers. */
	*found = 1;
	target_root = &target->cell;
	error = vm_heap_add_root(window->realm->heap, &target_root);
	if (error != 0)
		return error;

	/* Reveals the target, then scrolls to it when it is still in the document and has a box. */
	error = environment_reveal_target(window, target);
	if (error == 0 &&
	    dom_is_inclusive_ancestor(&window->document->node, target) &&
	    window->host.node_box != NULL && window->host.scroll_to != NULL &&
	    window->host.node_box(window->host.context, target, &box))
		window->host.scroll_to(window->host.context, 0.0, box.y);
	vm_heap_remove_root(window->realm->heap, &target_root);
	if (error != 0)
		return error;

	/* Succeeded: the target is shown. */
	return 0;
}

/* Makes navigator with its names, flags and numbers. */
static int
environment_install_navigator(
	struct bind_window *window)
{
	struct vm_realm *realm;
	struct vm_object *prototype;
	struct vm_object *navigator;
	int error;

	/* The prototype carries what never changes. */
	realm = window->realm;
	prototype = window->prototypes[BIND_NAVIGATOR];
	error = environment_strings(realm, prototype, environment_navigator_strings);
	if (error != 0)
		return error;
	error = environment_flags(realm, prototype, environment_navigator_flags);
	if (error != 0)
		return error;
	error = environment_numbers(realm, prototype, environment_navigator_numbers);
	if (error != 0)
		return error;

	/* doNotTrack is null: the user has not said. */
	error = js_builtin_value(realm, prototype, "doNotTrack", VM_VALUE_NULL, VM_PROPERTY_ENUMERABLE | VM_PROPERTY_CONFIGURABLE);
	if (error != 0)
		return error;

	/* The window's navigator. */
	error = environment_instance(window, BIND_NAVIGATOR, "navigator", &navigator);
	if (error != 0)
		return error;

	/* Succeeded: navigator is on the global object. */
	return 0;
}

/* Makes screen with its sizes and depths. */
static int
environment_install_screen(
	struct bind_window *window)
{
	const struct environment_part *size;
	struct vm_realm *realm;
	struct vm_object *prototype;
	struct vm_object *screen;
	int error;

	/* The depths never change. */
	realm = window->realm;
	prototype = window->prototypes[BIND_SCREEN];
	error = environment_numbers(realm, prototype, environment_screen_numbers);
	if (error != 0)
		return error;

	/* The sizes follow the window's viewport, which is the whole screen the page can have. */
	for (size = environment_screen_sizes; size->name != NULL; size++) {
		error = environment_part_accessor(realm, prototype, size->name, environment_screen_size, NULL, size->part);
		if (error != 0)
			return error;
	}

	/* The window's screen. */
	error = environment_instance(window, BIND_SCREEN, "screen", &screen);
	if (error != 0)
		return error;

	/* Succeeded: screen is on the global object. */
	return 0;
}

/* Makes performance with a real managed mark buffer and the existing legacy timing records. */
static int
environment_install_performance(
	struct bind_window *window)
{
	struct vm_realm *realm;
	struct vm_object *performance;
	struct vm_object *timing;
	struct vm_object *navigation;
	struct environment_timeline *timeline;
	struct vm_cell *root;
	vm_value origin;
	size_t index;
	int error;

	/* Install readonly native entry fields after their interface prototypes exist. */
	realm = window->realm;
	error = environment_install_mark_fields(window);
	if (error != 0)
		return error;

	/* The ordinary global performance property retains this wrapper during initialization. */
	error = environment_instance(window, BIND_PERFORMANCE, "performance", &performance);
	if (error != 0)
		return error;

	/* Allocate one empty, independently collectible native timeline per Window. */
	timeline = vm_heap_alloc(realm->heap, &environment_timeline_type, sizeof(*timeline));
	if (timeline == NULL)
		return ENOMEM;

	/* Initialize all traced storage before publishing the payload to the rooted wrapper. */
	wb_vector_init(&timeline->entries, sizeof(vm_value));
	timeline->owner_global = realm->global;
	timeline->mark_prototype = window->prototypes[BIND_PERFORMANCE_MARK];
	timeline->list_prototype = window->prototypes[BIND_PERFORMANCE_OBSERVER_ENTRY_LIST];
	wb_vector_init(&timeline->observers, sizeof(vm_value));
	timeline->dispatch = VM_VALUE_UNDEFINED;
	timeline->queued = 0;
	performance->kind = VM_KIND_PLATFORM;
	performance->internal = vm_value_cell(timeline);
	performance->native_operations = &environment_timeline_native;

	/* Legacy record objects need temporary ownership while their first property key is allocated. */
	root = NULL;
	error = vm_heap_add_root(realm->heap, &root);
	if (error != 0)
		return error;

	/* Preserve the existing legacy timing record without changing the page clock. */
	timing = vm_object_create(realm->heap, realm->object_prototype);
	if (timing == NULL) {
		error = ENOMEM;
		goto cleanup;
	}

	/* Retain the new record while publishing its first owning reference. */
	root = &timing->cell;

	/* Publish the record before defining its allocating string keys. */
	error = js_builtin_value(realm, performance, "timing", vm_value_cell(timing), VM_PROPERTY_DEFAULT);
	if (error != 0)
		goto cleanup;

	/* Legacy moments retain their existing shared time-origin values. */
	origin = vm_value_number(window->time_origin);
	for (index = 0; environment_timing_names[index] != NULL; index++) {
		error = js_builtin_value(realm, timing, environment_timing_names[index], origin, VM_PROPERTY_DEFAULT);
		if (error != 0)
			goto cleanup;
	}

	/* Navigation remains the existing ordinary type with no redirects. */
	navigation = vm_object_create(realm->heap, realm->object_prototype);
	if (navigation == NULL) {
		error = ENOMEM;
		goto cleanup;
	}

	/* The timing record is globally owned; the temporary root now retains navigation. */
	root = &navigation->cell;

	/* Publish before defining fields so collector pressure cannot orphan this record. */
	error = js_builtin_value(realm, performance, "navigation", vm_value_cell(navigation), VM_PROPERTY_DEFAULT);
	if (error != 0)
		goto cleanup;

	/* Preserve the established navigation type. */
	error = js_builtin_value(realm, navigation, "type", vm_value_int32(0), VM_PROPERTY_DEFAULT);
	if (error != 0)
		goto cleanup;

	/* No navigation entry producer or resource timing feature is implied. */
	error = js_builtin_value(realm, navigation, "redirectCount", vm_value_int32(0), VM_PROPERTY_DEFAULT);
	if (error != 0)
		goto cleanup;

	/* Bind real observer construction and delivery to this already owned native timeline. */
	error = environment_install_performance_observer(window, timeline);
	if (error != 0)
		goto cleanup;

cleanup:
	/* Preserve no temporary collector roots after either partial or complete installation. */
	vm_heap_remove_root(realm->heap, &root);

	/* A partially initialized environment remains an installation failure. */
	if (error != 0)
		return error;

	/* Succeeded: each Window owns an actual native mark buffer. */
	return 0;
}

/* Makes location, whose parts are read from the host. */
static int
environment_install_location(
	struct bind_window *window)
{
	const struct environment_part *part;
	struct vm_realm *realm;
	struct vm_object *prototype;
	int error;

	/* Each part is an accessor that asks the host. */
	realm = window->realm;
	prototype = window->prototypes[BIND_LOCATION];
	for (part = environment_location_parts; part->name != NULL; part++) {
		error = environment_part_accessor(realm, prototype, part->name, environment_location_part, NULL, part->part);
		if (error != 0)
			return error;
	}

	/* The window's location, which document.location reports as well. */
	error = environment_instance(window, BIND_LOCATION, "location", &window->location);
	if (error != 0)
		return error;

	/* Succeeded: location is on the global object. */
	return 0;
}

/* Makes the Image constructor, whose objects are img elements. */
static int
environment_install_image(
	struct bind_window *window)
{
	struct vm_realm *realm;
	struct vm_function *image;
	vm_value prototype;
	int error;

	/* The constructor, which throws when called without new. */
	realm = window->realm;
	error = js_builtin_function(realm, "Image", 0, bind_illegal_constructor, environment_image_construct, &image);
	if (error != 0)
		return error;

	/* Its prototype is HTMLImageElement's (which keeps its own constructor). */
	prototype = vm_value_cell(window->prototypes[BIND_HTML_IMAGE_ELEMENT]);
	error = js_builtin_value(realm, &image->object, "prototype", prototype, 0);
	if (error != 0)
		return error;

	/* The global. */
	error = js_builtin_value(realm, realm->global, "Image", vm_value_cell(image), JS_BUILTIN_METHOD);
	if (error != 0)
		return error;

	/* Succeeded: Image is on the global object. */
	return 0;
}

/* structuredClone (the clone of the plain data a page keeps): written in the page's own language. */
static const char environment_structured_clone[] =
	"(function () {\n"
	"  function fail(what) { return new DOMException(what + ' could not be cloned.', 'DataCloneError'); }\n"
	"  return function structuredClone(value) {\n"
	"    var seen = new Map();\n"
	"    function copy(x) {\n"
	"      var r, tag, i, keys;\n"
	"      if (typeof x === 'function') throw fail(String(x));\n"
	"      if (typeof x === 'symbol') throw fail('Symbol()');\n"
	"      if (x === null || typeof x !== 'object') return x;\n"
	"      if (seen.has(x)) return seen.get(x);\n"
	"      tag = Object.prototype.toString.call(x);\n"
	"      if (Array.isArray(x)) {\n"
	"        r = new Array(x.length); seen.set(x, r);\n"
	"        for (i = 0; i < x.length; i++) if (i in x) r[i] = copy(x[i]);\n"
	"        return r;\n"
	"      }\n"
	"      if (tag === '[object Date]') { r = new Date(x.getTime()); seen.set(x, r); return r; }\n"
	"      if (tag === '[object RegExp]') { r = new RegExp(x.source, x.flags); seen.set(x, r); return r; }\n"
	"      if (tag === '[object Boolean]' || tag === '[object Number]' || tag === '[object String]') {\n"
	"        r = Object(x.valueOf()); seen.set(x, r); return r;\n"
	"      }\n"
	"      if (x instanceof Map) {\n"
	"        r = new Map(); seen.set(x, r);\n"
	"        x.forEach(function (v, k) { r.set(copy(k), copy(v)); });\n"
	"        return r;\n"
	"      }\n"
	"      if (x instanceof Set) {\n"
	"        r = new Set(); seen.set(x, r);\n"
	"        x.forEach(function (v) { r.add(copy(v)); });\n"
	"        return r;\n"
	"      }\n"
	"      if (x instanceof Error) {\n"
	"        r = Object.create(Object.getPrototypeOf(x)); seen.set(x, r);\n"
	"        Object.defineProperty(r, 'message', { value: x.message, writable: true, configurable: true });\n"
	"        if (x.stack !== undefined) Object.defineProperty(r, 'stack', { value: x.stack, writable: true, configurable: true });\n"
	"        return r;\n"
	"      }\n"
	"      if (tag !== '[object Object]') throw fail('#<' + tag.slice(8, -1) + '>');\n"
	"      r = {}; seen.set(x, r);\n"
	"      keys = Object.keys(x);\n"
	"      for (i = 0; i < keys.length; i++) r[keys[i]] = copy(x[keys[i]]);\n"
	"      return r;\n"
	"    }\n"
	"    if (arguments.length === 0) throw new TypeError(\"Failed to execute 'structuredClone' on 'Window': 1 argument required, but only 0 present.\");\n"
	"    return copy(value);\n"
	"  };\n"
	"})()";

/* crypto.getRandomValues(typedArray): fills the array with random bytes and returns it. */
static int
environment_get_random_values(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(this_value);
	status = js_typed_random_fill(js_argument(args, count, 0));
	if (status == EINVAL)
		return vm_throw_type_error(realm, "Failed to execute 'getRandomValues' on 'Crypto': The provided value is not of type '(ArrayBufferView)'.");
	if (status != 0)
		return status;
	*result = js_argument(args, count, 0);
	return 0;
}

/* crypto.randomUUID(): a version 4 UUID. */
static int
environment_random_uuid(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	static const char hex[] = "0123456789abcdef";
	unsigned char bytes[16];
	char text[37];
	struct vm_string *string;
	FILE *file;
	size_t got;
	unsigned i;
	unsigned at;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	file = fopen("/dev/urandom", "rb");
	if (file == NULL)
		return EIO;
	got = fread(bytes, 1U, sizeof(bytes), file);
	fclose(file);
	if (got != sizeof(bytes))
		return EIO;
	bytes[6] = (unsigned char)((bytes[6] & 0x0fU) | 0x40U);
	bytes[8] = (unsigned char)((bytes[8] & 0x3fU) | 0x80U);
	at = 0;
	for (i = 0; i < 16U; i++) {
		if (i == 4U || i == 6U || i == 8U || i == 10U)
			text[at++] = '-';
		text[at++] = hex[bytes[i] >> 4];
		text[at++] = hex[bytes[i] & 15U];
	}
	string = vm_string_from_utf8(realm->heap, text, at);
	if (string == NULL)
		return ENOMEM;
	*result = vm_value_cell(string);
	return 0;
}

/* Defines the global crypto object (random values only; there is no SubtleCrypto). */
static int
environment_install_crypto(
	struct vm_realm *realm)
{
	struct vm_object *crypto;
	int error;

	crypto = vm_object_create(realm->heap, realm->object_prototype);
	if (crypto == NULL)
		return ENOMEM;
	error = js_builtin_method(realm, crypto, "getRandomValues", 1, environment_get_random_values);
	if (error == 0)
		error = js_builtin_method(realm, crypto, "randomUUID", 0, environment_random_uuid);
	if (error == 0)
		error = js_builtin_value(realm, realm->global, "crypto", vm_value_cell(crypto), VM_PROPERTY_DEFAULT);
	return error;
}

/* Defines structuredClone on the global object. */
static int
environment_install_structured_clone(
	struct vm_realm *realm)
{
	struct vm_string *source;
	vm_value function;
	int status;

	/* The function made by running its source. */
	source = vm_string_from_utf8(realm->heap, environment_structured_clone, sizeof(environment_structured_clone) - 1U);
	if (source == NULL)
		return ENOMEM;
	status = js_builtin_evaluate(realm, source, 0, &function);
	if (status != 0) {
		/* A failure to build it leaves the global without it. */
		realm->exception = VM_VALUE_UNDEFINED;
		return 0;
	}
	status = js_builtin_value(realm, realm->global, "structuredClone", function, VM_PROPERTY_DEFAULT);
	return status;
}

/*
 * Defines the window's plain properties: the pixel ratio, the outer size
 * and position, and the window's relatives (a page without frames is its
 * own top and parent).
 */
static int
environment_install_window(
	struct bind_window *window)
{
	struct vm_realm *realm;
	struct vm_object *global;
	vm_value self;
	int error;

	/* One device pixel is one CSS pixel. */
	realm = window->realm;
	global = realm->global;
	error = js_builtin_value(realm, global, "devicePixelRatio", vm_value_int32(1), VM_PROPERTY_DEFAULT);
	if (error != 0)
		return error;

	/* Binary string conversion used by resource loaders. */
	error = js_builtin_method(realm, global, "atob", 1, environment_atob);
	if (error == 0)
		error = js_builtin_method(realm, global, "btoa", 1, environment_btoa);
	if (error == 0)
		error = environment_install_structured_clone(realm);
	if (error == 0)
		error = environment_install_crypto(realm);
	if (error != 0)
		return error;

	/* The outer size is the viewport's (the window's frame is not counted). */
	error = environment_part_accessor(realm, global, "outerWidth", environment_outer_size, NULL, 0);
	if (error != 0)
		return error;
	error = environment_part_accessor(realm, global, "outerHeight", environment_outer_size, NULL, 1);
	if (error != 0)
		return error;

	/* The window's place on the screen is its corner. */
	error = js_builtin_value(realm, global, "screenX", vm_value_int32(0), VM_PROPERTY_DEFAULT);
	if (error == 0)
		error = js_builtin_value(realm, global, "screenY", vm_value_int32(0), VM_PROPERTY_DEFAULT);
	if (error == 0)
		error = js_builtin_value(realm, global, "screenLeft", vm_value_int32(0), VM_PROPERTY_DEFAULT);
	if (error == 0)
		error = js_builtin_value(realm, global, "screenTop", vm_value_int32(0), VM_PROPERTY_DEFAULT);
	if (error != 0)
		return error;

	/* Browsing-context relatives are live accessors on Window's prototype. */
	error = js_builtin_value(realm, global, "opener", VM_VALUE_NULL, VM_PROPERTY_DEFAULT);
	if (error != 0)
		return error;

	/* Frame enumeration remains outside the initial-context interface. */
	error = js_builtin_value(realm, global, "length", vm_value_int32(0), VM_PROPERTY_DEFAULT);
	if (error != 0)
		return error;

	/* The window has no name and is open. */
	error = bind_string(realm, "", &self);
	if (error == 0)
		error = js_builtin_value(realm, global, "name", self, VM_PROPERTY_DEFAULT);

	/* A failed name allocation or definition ends environment installation. */
	if (error != 0)
		return error;

	/* Succeeded: the window's properties are defined. */
	return 0;
}

/*
 * Makes the window's object of an interface (from its prototype) and puts
 * it on the global object under a name that cannot be replaced.
 */
static int
environment_instance(
	struct bind_window *window,
	int interface,
	const char *global,
	struct vm_object **object)
{
	struct vm_realm *realm;
	struct vm_cell *root;
	int error;

	/* The wrapper must survive allocating its first global property key. */
	realm = window->realm;
	root = NULL;
	error = vm_heap_add_root(realm->heap, &root);
	if (error != 0)
		return error;

	/* Register temporary ownership before constructing the ordinary native instance. */
	*object = vm_object_create(realm->heap, window->prototypes[interface]);
	if (*object == NULL) {
		vm_heap_remove_root(realm->heap, &root);
		return ENOMEM;
	}

	/* A reentrant collection during global publication cannot orphan the newborn wrapper. */
	root = &(*object)->cell;
	error = js_builtin_value(realm, realm->global, global, vm_value_cell(*object), VM_PROPERTY_ENUMERABLE | VM_PROPERTY_CONFIGURABLE);
	vm_heap_remove_root(realm->heap, &root);

	/* Failed publication leaves environment installation incomplete. */
	if (error != 0)
		return error;

	/* Succeeded: the actual global object owns the wrapper before temporary roots end. */
	return 0;
}

/* Defines each string of a table on an object. */
static int
environment_strings(
	struct vm_realm *realm,
	struct vm_object *object,
	const struct environment_string *table)
{
	vm_value value;
	int error;

	/* Each entry, a string that cannot be assigned. */
	for (; table->name != NULL; table++) {
		error = bind_string(realm, table->value, &value);
		if (error != 0)
			return error;
		error = js_builtin_value(realm, object, table->name, value, VM_PROPERTY_ENUMERABLE | VM_PROPERTY_CONFIGURABLE);
		if (error != 0)
			return error;
	}

	/* Succeeded: the strings are defined. */
	return 0;
}

/* Defines each number of a table on an object. */
static int
environment_numbers(
	struct vm_realm *realm,
	struct vm_object *object,
	const struct environment_number *table)
{
	int error;

	/* Each entry, a number that cannot be assigned. */
	for (; table->name != NULL; table++) {
		error = js_builtin_value(realm, object, table->name, vm_value_number(table->value), VM_PROPERTY_ENUMERABLE | VM_PROPERTY_CONFIGURABLE);
		if (error != 0)
			return error;
	}

	/* Succeeded: the numbers are defined. */
	return 0;
}

/* Defines each flag of a table on an object. */
static int
environment_flags(
	struct vm_realm *realm,
	struct vm_object *object,
	const struct environment_flag *table)
{
	vm_value value;
	int error;

	/* Each entry, a Boolean that cannot be assigned. */
	for (; table->name != NULL; table++) {
		value = VM_VALUE_FALSE;
		if (table->value)
			value = VM_VALUE_TRUE;
		error = js_builtin_value(realm, object, table->name, value, VM_PROPERTY_ENUMERABLE | VM_PROPERTY_CONFIGURABLE);
		if (error != 0)
			return error;
	}

	/* Succeeded: the flags are defined. */
	return 0;
}

/*
 * Defines an accessor whose getter reads one part of something; the optional
 * setter updates that part. The getter finds the part in its function data.
 */
static int
environment_part_accessor(
	struct vm_realm *realm,
	struct vm_object *object,
	const char *name,
	vm_native getter,
	vm_native setter,
	int part)
{
	struct vm_function *function;
	struct vm_function *set_function;
	struct vm_accessor *accessor;
	char getter_name[80];
	char setter_name[80];
	struct vm_cell *roots[3];
	vm_value set_value;
	unsigned registered;
	int error;

	/* The getter, named "get NAME", which knows its part. */
	snprintf(getter_name, sizeof(getter_name), "get %s", name);
	error = js_builtin_function(realm, getter_name, 0, getter, NULL, &function);
	if (error != 0)
		return error;
	function->data = vm_value_int32(part);
	roots[0] = &function->object.cell;
	registered = 0U;
	error = vm_heap_add_root(realm->heap, &roots[0]);
	if (error != 0)
		return error;
	registered++;

	/* A second function allocation must retain the first until publication. */
	set_value = VM_VALUE_UNDEFINED;
	if (setter != NULL) {
		snprintf(setter_name, sizeof(setter_name), "set %s", name);
		error = js_builtin_function(realm, setter_name, 1, setter, NULL, &set_function);
		if (error != 0)
			goto cleanup;
		roots[1] = &set_function->object.cell;
		error = vm_heap_add_root(realm->heap, &roots[1]);
		if (error != 0)
			goto cleanup;
		registered++;
		set_value = vm_value_cell(set_function);
	}

	/* The accessor, with a setter only when this part changes. */
	accessor = vm_accessor_create(realm->heap, vm_value_cell(function), set_value);
	if (accessor == NULL) {
		error = ENOMEM;
		goto cleanup;
	}
	roots[registered] = &accessor->cell;
	error = vm_heap_add_root(realm->heap, &roots[registered]);
	if (error != 0)
		goto cleanup;
	registered++;
	error = js_builtin_value(realm, object, name, vm_value_cell(accessor), VM_PROPERTY_ACCESSOR | VM_PROPERTY_ENUMERABLE | VM_PROPERTY_CONFIGURABLE);

cleanup:
	while (registered > 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Succeeded: the accessor is defined. */
	return error;
}

/* Makes a new empty array (a list with nothing in it). */
static int
environment_empty_array(
	struct vm_realm *realm,
	vm_value *value)
{
	struct vm_object *array;
	int error;

	/* The array of the realm. */
	error = bind_array_create(realm, &array);
	if (error != 0)
		return error;

	/* Succeeded: the array. */
	*value = vm_value_cell(array);
	return 0;
}

/* Reports navigator.userAgent: the User-Agent the host sends. */
static int
environment_navigator_user_agent(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	const char *agent;
	int error;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* A host that names no agent leaves it empty. */
	window = bind_window_of(realm);
	agent = window->host.user_agent;
	if (agent == NULL)
		agent = "";

	/* The string. */
	error = bind_string(realm, agent, result);
	if (error != 0)
		return error;

	/* Succeeded: the agent is reported. */
	return 0;
}

/* Reports navigator.appVersion: the User-Agent after its first slash. */
static int
environment_navigator_app_version(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	const char *agent;
	const char *slash;
	int error;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The agent, from after the product's name. */
	window = bind_window_of(realm);
	agent = window->host.user_agent;
	if (agent == NULL)
		agent = "";
	slash = strchr(agent, '/');
	if (slash != NULL)
		agent = slash + 1;

	/* The string. */
	error = bind_string(realm, agent, result);
	if (error != 0)
		return error;

	/* Succeeded: the version is reported. */
	return 0;
}

/* Reports navigator.languages: the one language, in a new array. */
static int
environment_navigator_languages(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	vm_value language;
	int error;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The language navigator.language names. */
	error = bind_string(realm, "en-US", &language);
	if (error != 0)
		return error;

	/* The array of it. */
	error = js_builtin_array(realm, &language, 1, result);
	if (error != 0)
		return error;

	/* Succeeded: the languages are reported. */
	return 0;
}

/* Reports navigator.plugins and navigator.mimeTypes: there are none. */
static int
environment_navigator_list(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* An empty list. */
	error = environment_empty_array(realm, result);
	if (error != 0)
		return error;

	/* Succeeded: the list is reported. */
	return 0;
}

/* Reports navigator.javaEnabled(): there is no Java. */
static int
environment_navigator_java_enabled(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	UNUSED_PARAMETER(realm);
	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Succeeded: false. */
	*result = VM_VALUE_FALSE;
	return 0;
}

/*
 * Takes navigator.sendBeacon(url, data): the beacon is accepted (true)
 * and not sent, since the page's measurements are for the site alone.
 */
static int
environment_navigator_send_beacon(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *url;
	int error;

	UNUSED_PARAMETER(this_value);

	/* The URL is converted as the method's argument is, which may throw. */
	error = bind_to_string(realm, js_argument(args, count, 0), &url);
	if (error != 0)
		return error;

	/* Succeeded: the beacon is taken. */
	*result = VM_VALUE_TRUE;
	return 0;
}

/* Reports one of the screen's sizes (the function's data: 0 for a width, 1 for a height). */
static int
environment_screen_size(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct vm_function *callee;
	int part;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Which size. */
	callee = js_builtin_callee(realm);
	part = vm_value_as_int32(callee->data);
	window = bind_window_of(realm);

	/* The height. */
	if (part == 1) {
		*result = vm_value_int32(window->viewport_height);
		return 0;
	}

	/* Succeeded: the width. */
	*result = vm_value_int32(window->viewport_width);
	return 0;
}

/* Reports performance.now(): the page's clock in milliseconds. */
static int
environment_performance_now(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The time the page gave the window. */
	window = bind_window_of(realm);
	*result = vm_value_number(window->now);

	/* Succeeded: the time is reported. */
	return 0;
}

/* Reports performance.timeOrigin: when the window was made, in milliseconds since 1970. */
static int
environment_performance_time_origin(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The moment kept when the window was made. */
	window = bind_window_of(realm);
	*result = vm_value_number(window->time_origin);

	/* Succeeded: the origin is reported. */
	return 0;
}

/* Retains the actual relevant global and buffered marks through managed VM references. */
static void
environment_timeline_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct environment_timeline *timeline;
	vm_value *entries;
	size_t index;

	/* Every buffered wrapper owns its immutable entry state. */
	timeline = (struct environment_timeline *)cell;
	vm_heap_mark(heap, &timeline->owner_global->cell);
	vm_heap_mark(heap, &timeline->mark_prototype->cell);
	vm_heap_mark(heap, &timeline->list_prototype->cell);
	vm_heap_mark_value(heap, timeline->dispatch);
	entries = timeline->entries.items;
	for (index = 0; index < timeline->entries.count; index++) {
		vm_heap_mark_value(heap, entries[index]);
	}

	/* Active registrations retain their genuine callback-owning observer wrappers. */
	entries = timeline->observers.items;
	for (index = 0; index < timeline->observers.count; index++) {
		vm_heap_mark_value(heap, entries[index]);
	}

	/* Succeeded: the timeline's relevant realm and current marks remain reachable. */
	return;
}

/* Releases the native buffer when its performance wrapper becomes unreachable. */
static void
environment_timeline_finalize(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct environment_timeline *timeline;

	UNUSED_PARAMETER(heap);

	/* No Window or DOM ownership is borrowed by the buffer. */
	timeline = (struct environment_timeline *)cell;
	wb_vector_release(&timeline->entries);
	wb_vector_release(&timeline->observers);

	/* Succeeded: no native vector allocation survives the dead timeline. */
	return;
}

/* Retains the name and primitive detail of a separately saved native mark. */
static void
environment_mark_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct environment_mark *mark;

	/* Saved entries remain readable after clearMarks and frame retirement. */
	mark = (struct environment_mark *)cell;
	vm_heap_mark(heap, &mark->name->cell);
	vm_heap_mark_value(heap, mark->detail);

	/* Succeeded: a saved entry retains its immutable primitive payload. */
	return;
}

/* Refuses prototype lookalikes, proxies and unrelated native wrappers. */
static int
environment_timeline_of(
	struct vm_realm *realm,
	vm_value receiver,
	struct environment_timeline **timeline)
{
	struct vm_object *object;
	struct vm_cell *cell;
	int is_object;
	int is_cell;
	int status;

	/* Only a genuine performance object exposes its private timeline. */
	is_object = vm_value_is_object(receiver);
	if (!is_object) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Native operations provide an unforgeable wrapper brand. */
	object = (struct vm_object *)vm_value_as_cell(receiver);
	if (object->native_operations != &environment_timeline_native) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* The managed payload must belong to the same native representation. */
	is_cell = vm_value_is_cell(object->internal);
	if (!is_cell) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* A malformed wrapper cannot become a native state pointer. */
	cell = vm_value_as_cell(object->internal);
	if (cell->type != &environment_timeline_type) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Succeeded: the caller borrows the receiver-owned timeline. */
	*timeline = (struct environment_timeline *)cell;
	return 0;
}

/* Obtains the immutable native state behind an actual PerformanceMark. */
static int
environment_mark_of(
	struct vm_realm *realm,
	vm_value receiver,
	struct environment_mark **mark)
{
	struct vm_object *object;
	struct vm_cell *cell;
	int is_object;
	int is_cell;
	int status;

	/* Ordinary objects cannot impersonate entries by changing their prototype. */
	is_object = vm_value_is_object(receiver);
	if (!is_object) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Entry and timeline brands remain distinct even after prototype mutation. */
	object = (struct vm_object *)vm_value_as_cell(receiver);
	if (object->native_operations != &environment_mark_native) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* The payload's managed type independently validates its layout. */
	is_cell = vm_value_is_cell(object->internal);
	if (!is_cell) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* A foreign managed cell is never interpreted as entry storage. */
	cell = vm_value_as_cell(object->internal);
	if (cell->type != &environment_mark_type) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Succeeded: the receiver keeps the entry alive throughout the call. */
	*mark = (struct environment_mark *)cell;
	return 0;
}

/* Converts mark arguments and creates an immutable wrapper without buffering it. */
static int
environment_mark_create(
	struct vm_realm *realm,
	const vm_value *args,
	unsigned count,
	struct environment_timeline *timeline,
	int construct,
	vm_value *result)
{
	struct bind_window *window;
	struct vm_string *name;
	struct vm_object *prototype;
	struct vm_object *object;
	struct environment_mark *mark;
	struct vm_cell *roots[5];
	vm_value options;
	vm_value detail;
	vm_value timestamp;
	double start;
	size_t index;
	unsigned registered;
	int present;
	int is_object;
	int is_cell;
	int equal;
	int status;

	/* Required arguments are distinct from an explicit undefined name. */
	*result = VM_VALUE_UNDEFINED;
	if (count == 0U) {
		status = vm_throw_type_error(realm, "A mark name is required");
		return status;
	}

	/* All slots exist before allocation or user conversion can run the collector. */
	registered = 0U;
	for (index = 0; index < 5U; index++) {
		roots[index] = NULL;
	}

	/* Preserve intermediate dictionary values across reentrant getters. */
	for (index = 0; index < 5U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;

		/* Cleanup releases only slots successfully registered with the heap. */
		registered++;
	}

	/* DOMString conversion precedes the dictionary's member reads. */
	status = vm_to_string(realm, args[0], &name);
	if (status != 0)
		goto cleanup;

	/* A newly converted name survives option getters and prototype lookup. */
	roots[0] = &name->cell;
	options = VM_VALUE_UNDEFINED;
	if (count > 1U)
		options = args[1];

	/* Null and undefined are empty dictionaries; other primitives are rejected. */
	if (options != VM_VALUE_UNDEFINED && options != VM_VALUE_NULL) {
		is_object = vm_value_is_object(options);
		if (!is_object) {
			status = vm_throw_type_error(realm, "Mark options must be a dictionary");
			goto cleanup;
		}
	}

	/* WebIDL reads dictionary members in lexical order, including absent values. */
	status = bind_get_option(realm, options, "detail", &present, &detail);
	if (status != 0)
		goto cleanup;

	/* The default detail is null, including an explicitly undefined member. */
	if (!present)
		detail = VM_VALUE_NULL;

	/* A getter-produced primitive cell needs its own root before the next getter. */
	is_cell = vm_value_is_cell(detail);
	if (is_cell)
		roots[1] = vm_value_as_cell(detail);

	/* Read the optional timestamp without keeping a pointer into a mutable buffer. */
	status = bind_get_option(realm, options, "startTime", &present, &timestamp);
	if (status != 0)
		goto cleanup;

	/* Explicit times use finite nonnegative DOMHighResTimeStamp values. */
	window = bind_window_of(realm);
	if (timeline != NULL) {
		/* Borrowed methods use the actual receiver's retained relevant global and clock. */
		window = bind_window_of((struct vm_realm *)vm_value_as_cell(timeline->owner_global->internal));
	}

	/* The receiver's managed global keeps a child host alive across conversions. */
	start = window->now;
	if (present) {
		status = vm_to_number(realm, timestamp, &start);
		if (status != 0)
			goto cleanup;

		/* NaN and infinities cannot become an entry timestamp. */
		if (start != start ||
		    start > DBL_MAX ||
		    start < 0.0) {
			status = vm_throw_type_error(realm, "A mark timestamp must be finite and nonnegative");
			goto cleanup;
		}
	}

	/* Legacy PerformanceTiming member names remain reserved for both creation APIs. */
	for (index = 0; environment_timing_names[index] != NULL; index++) {
		equal = vm_string_equal_ascii(name, environment_timing_names[index]);
		if (equal) {
			status = bind_throw_dom(realm, "SyntaxError", "A legacy timing name cannot name a mark");
			goto cleanup;
		}
	}

	/* This increment supports clone-free primitives; complex cloning remains explicit residual work. */
	is_object = vm_value_is_object(detail);
	if (is_object) {
		status = bind_throw_dom(realm, "DataCloneError", "Complex mark detail cloning is not supported yet");
		goto cleanup;
	}

	/* Symbols are not structured-cloneable even though they are primitive values. */
	if (is_cell && roots[1]->type == &vm_symbol_type) {
		status = bind_throw_dom(realm, "DataCloneError", "Symbol mark detail cannot be cloned");
		goto cleanup;
	}

	/* Only construction consults new.target; performance.mark uses the relevant realm prototype. */
	prototype = window->prototypes[BIND_PERFORMANCE_MARK];
	if (timeline != NULL)
		prototype = timeline->mark_prototype;

	/* Direct construction resolves the explicit new.target prototype in its own realm. */
	if (construct) {
		status = vm_construct_prototype(realm, realm->new_target, prototype, &prototype);
		if (status != 0)
			goto cleanup;
	}

	/* A custom prototype survives the following state and wrapper allocations. */
	roots[2] = &prototype->cell;
	mark = vm_heap_alloc(realm->heap, &environment_mark_type, sizeof(*mark));
	if (mark == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Initialize the complete traceable payload before another allocation can occur. */
	mark->name = name;
	mark->detail = detail;
	mark->start = start;
	roots[3] = &mark->cell;

	/* A separately saved wrapper owns its state rather than borrowing a Window pointer. */
	object = vm_object_create(realm->heap, prototype);
	if (object == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Native branding remains private while ordinary own-property semantics remain available. */
	roots[4] = &object->cell;
	object->kind = VM_KIND_PLATFORM;
	object->internal = vm_value_cell(mark);
	object->native_operations = &environment_mark_native;
	*result = vm_value_cell(object);
	status = 0;

cleanup:
	/* Release construction roots in reverse order after publication or failure. */
	while (registered > 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Preserve conversion, allocation or construction failures for the caller. */
	if (status != 0)
		return status;

	/* Succeeded: the caller receives an unbuffered, independently owned native mark. */
	return 0;
}

/* Constructs a mark without adding it to the Window's performance buffer. */
static int
environment_mark_construct(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(receiver);

	/* Preserve the ordinary new.target protocol through the shared mark builder. */
	status = environment_mark_create(realm, args, count, NULL, 1, result);
	if (status != 0)
		return status;

	/* Succeeded: constructing alone did not publish a buffered entry. */
	return 0;
}

/* Creates a real mark and inserts it stably into its receiver's timeline. */
static int
environment_performance_mark(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct environment_timeline *timeline;
	struct environment_mark *mark;
	struct environment_mark *previous;
	struct vm_object *object;
	vm_value *entries;
	vm_value value;
	size_t index;
	int status;

	/* Validate the receiver before invoking user argument conversions. */
	status = environment_timeline_of(realm, receiver, &timeline);
	if (status != 0)
		return status;

	/* Conversion may reenter mark or clearMarks; insertion uses the resulting current buffer. */
	status = environment_mark_create(realm, args, count, timeline, 0, result);
	if (status != 0)
		return status;

	/* Native vector growth cannot invoke script or collect the freshly returned wrapper. */
	value = *result;
	status = wb_vector_push(&timeline->entries, &value);
	if (status != 0)
		return status;

	/* Preserve insertion order among equal timestamps without calling a JS comparator. */
	object = (struct vm_object *)vm_value_as_cell(value);
	mark = (struct environment_mark *)vm_value_as_cell(object->internal);
	entries = timeline->entries.items;
	index = timeline->entries.count - 1U;
	while (index > 0U) {
		object = (struct vm_object *)vm_value_as_cell(entries[index - 1U]);
		previous = (struct environment_mark *)vm_value_as_cell(object->internal);

		/* An equal or earlier timestamp already belongs before the new entry. */
		if (previous->start <= mark->start)
			break;

		/* Shift only references; entry payloads and their JS identities remain unchanged. */
		entries[index] = entries[index - 1U];
		index--;
	}

	/* Publish the entry after every older timestamp and every equal-time predecessor. */
	entries[index] = value;

	/* Queue this exact producer entry through the actual relevant timeline's observers. */
	status = environment_performance_queue_mark(timeline, value);
	if (status != 0)
		return status;

	/* Succeeded: the returned mark is the actual buffered object. */
	return 0;
}

/* Reads one immutable entry field using the native getter's private part index. */
static int
environment_mark_part(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct environment_mark *mark;
	struct vm_function *callee;
	int part;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Shared prototype getters still require a genuine native entry brand. */
	status = environment_mark_of(realm, receiver, &mark);
	if (status != 0)
		return status;

	/* Select the field recorded when this accessor was installed. */
	callee = js_builtin_callee(realm);
	part = vm_value_as_int32(callee->data);

	/* Name and detail preserve their independently traced primitive identities. */
	if (part == 0) {
		*result = vm_value_cell(mark->name);
	} else if (part == 1) {
		status = bind_string(realm, "mark", result);
		if (status != 0)
			return status;
	} else if (part == 2) {
		*result = vm_value_number(mark->start);
	} else if (part == 3) {
		*result = vm_value_int32(0);
	} else {
		*result = mark->detail;
	}

	/* Succeeded: the readonly native field is reported. */
	return 0;
}

/* Serializes the common PerformanceEntry fields as ordinary own data properties. */
static int
environment_mark_json(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct environment_mark *mark;
	struct vm_object *object;
	struct vm_cell *roots[2];
	unsigned registered;
	vm_value type;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* toJSON cannot serialize an ordinary prototype lookalike as an entry. */
	status = environment_mark_of(realm, receiver, &mark);
	if (status != 0)
		return status;

	/* Reserve the output root before allocation can collect native temporaries. */
	roots[0] = NULL;
	roots[1] = NULL;
	registered = 0U;
	status = vm_heap_add_root(realm->heap, &roots[0]);
	if (status != 0)
		return status;

	/* The output owns one root and the transient type string owns another. */
	registered++;
	status = vm_heap_add_root(realm->heap, &roots[1]);
	if (status != 0)
		goto cleanup;

	/* Cleanup owns both registered slots before the first object allocation. */
	registered++;

	/* The common entry dictionary does not serialize mark detail. */
	object = vm_object_create(realm->heap, realm->object_prototype);
	if (object == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Each fallible property definition sees a rooted, partially constructed dictionary. */
	roots[0] = &object->cell;
	status = js_builtin_value(realm, object, "name", vm_value_cell(mark->name), VM_PROPERTY_DEFAULT);
	if (status != 0)
		goto cleanup;

	/* Construct the fixed entry type before immediately publishing its value. */
	status = bind_string(realm, "mark", &type);
	if (status != 0)
		goto cleanup;

	/* A generated string remains rooted while its property key is allocated. */
	roots[1] = vm_value_as_cell(type);

	/* The type string remains owned by this property before the next VM allocation. */
	status = js_builtin_value(realm, object, "entryType", type, VM_PROPERTY_DEFAULT);
	if (status != 0)
		goto cleanup;

	/* Explicit times preserve their exact finite numeric value. */
	status = js_builtin_value(realm, object, "startTime", vm_value_number(mark->start), VM_PROPERTY_DEFAULT);
	if (status != 0)
		goto cleanup;

	/* A mark's duration is always zero, independently of subsequent clock changes. */
	status = js_builtin_value(realm, object, "duration", vm_value_int32(0), VM_PROPERTY_DEFAULT);
	if (status != 0)
		goto cleanup;

	/* Publish the complete dictionary before releasing its construction root. */
	*result = vm_value_cell(object);

cleanup:
	/* Release temporary ownership on both partial and complete dictionaries. */
	while (registered > 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Propagate an incomplete serialization without claiming success. */
	if (status != 0)
		return status;

	/* Succeeded: the caller owns a common-entry snapshot. */
	return 0;
}

/* Selects a snapshot after all filter conversions have finished reentering script. */
static int
environment_entries_snapshot(
	struct vm_realm *realm,
	const struct wb_vector *buffer,
	const vm_value *args,
	unsigned count,
	int mode,
	vm_value *result)
{
	struct environment_mark *mark;
	struct vm_object *object;
	struct vm_object *entry;
	struct vm_string *name;
	struct vm_string *type;
	struct vm_cell *roots[3];
	vm_value *entries;
	vm_value value;
	size_t index;
	uint32_t output;
	unsigned registered;
	int matches;
	int status;

	/* The calling native method already validated and retains the real source receiver. */
	*result = VM_VALUE_UNDEFINED;

	/* Filtered variants require an argument even when it is explicitly undefined. */
	if (mode != 0 && count == 0U) {
		status = vm_throw_type_error(realm, "An entry filter is required");
		return status;
	}

	/* Names and types returned by conversion survive later conversions and allocation. */
	registered = 0U;
	for (index = 0; index < 3U; index++) {
		roots[index] = NULL;
	}

	/* Register every slot before constructing either filter or the output array. */
	for (index = 0; index < 3U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;

		/* The unwind count records ownership of each registered slot. */
		registered++;
	}

	/* getEntriesByName converts its required name before its optional type. */
	name = NULL;
	type = NULL;
	if (mode == 2) {
		status = vm_to_string(realm, args[0], &name);
		if (status != 0)
			goto cleanup;

		/* The type getter can collect a newly converted name. */
		roots[0] = &name->cell;
	}

	/* getEntriesByType always converts its first argument, including undefined. */
	if (mode == 1) {
		status = vm_to_string(realm, args[0], &type);
		if (status != 0)
			goto cleanup;

		/* Keep this filter through snapshot construction. */
		roots[1] = &type->cell;
	}

	/* An omitted or explicitly undefined optional type leaves name matching unfiltered. */
	if (mode == 2 &&
	    count > 1U &&
	    args[1] != VM_VALUE_UNDEFINED) {
		status = vm_to_string(realm, args[1], &type);
		if (status != 0)
			goto cleanup;

		/* A second converted string has independent temporary ownership. */
		roots[1] = &type->cell;
	}

	/* A real array remains rooted until the native snapshot is complete. */
	object = vm_array_create(realm->heap, realm->array_prototype);
	if (object == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* No later snapshot step calls script or keeps a pointer across user conversion. */
	roots[2] = &object->cell;
	entries = buffer->items;
	output = 0U;
	for (index = 0; index < buffer->count; index++) {
		value = entries[index];
		entry = (struct vm_object *)vm_value_as_cell(value);
		mark = (struct environment_mark *)vm_value_as_cell(entry->internal);

		/* Only marks have a producer in this increment. */
		if (type != NULL) {
			matches = vm_string_equal_ascii(type, "mark");
			if (!matches)
				continue;
		}

		/* Name matching observes complete converted strings rather than object identity. */
		if (name != NULL) {
			matches = vm_string_equal(name, mark->name);
			if (!matches)
				continue;
		}

		/* Own data definitions cannot invoke an inherited Array index setter. */
		status = vm_object_define(realm->heap, object, vm_value_int32((int32_t)output), value, VM_PROPERTY_DEFAULT);
		if (status != 0)
			goto cleanup;

		/* Advance the output only after its current element has been published. */
		output++;
	}

	/* The source supplies chronological query storage or raw takeRecords order as appropriate. */
	*result = vm_value_cell(object);
	status = 0;

cleanup:
	/* Every registered root is released even after a user conversion throws. */
	while (registered > 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* An incomplete snapshot never becomes a successful result. */
	if (status != 0)
		return status;

	/* Succeeded: the caller owns an independent array of real entry identities. */
	return 0;
}

/* Keeps performance methods branded separately from delivered EntryList query methods. */
static int
environment_performance_snapshot(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	int mode,
	vm_value *result)
{
	struct environment_timeline *timeline;
	int status;

	/* Only an actual performance receiver can supply this native timeline. */
	status = environment_timeline_of(realm, receiver, &timeline);
	if (status != 0)
		return status;

	/* Common conversion and snapshot logic still observes the latest buffer after reentrant getters. */
	status = environment_entries_snapshot(realm, &timeline->entries, args, count, mode, result);
	if (status != 0)
		return status;

	/* Succeeded: the actual performance buffer supplied a fresh array of genuine entries. */
	return 0;
}

/* Returns all currently recorded entries in stable timestamp order. */
static int
environment_performance_entries(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The unfiltered snapshot requires no arguments. */
	status = environment_performance_snapshot(realm, receiver, args, count, 0, result);
	if (status != 0)
		return status;

	/* Succeeded: all native buffered marks were selected. */
	return 0;
}

/* Returns native entries matching one required type string. */
static int
environment_performance_by_type(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* Type selection shares the same real timeline snapshot as getEntries. */
	status = environment_performance_snapshot(realm, receiver, args, count, 1, result);
	if (status != 0)
		return status;

	/* Succeeded: the type-filtered snapshot is complete. */
	return 0;
}

/* Returns native entries matching a required name and optional type string. */
static int
environment_performance_by_name(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* Name selection preserves conversion order and optional undefined semantics. */
	status = environment_performance_snapshot(realm, receiver, args, count, 2, result);
	if (status != 0)
		return status;

	/* Succeeded: the name-filtered snapshot is complete. */
	return 0;
}

/* Removes buffered references without invalidating separately saved entries. */
static int
environment_performance_clear_marks(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct environment_timeline *timeline;
	struct environment_mark *mark;
	struct vm_object *object;
	struct vm_string *name;
	vm_value *entries;
	size_t input;
	size_t output;
	int matches;
	int status;

	/* clearMarks requires the actual performance receiver even with no name. */
	*result = VM_VALUE_UNDEFINED;
	status = environment_timeline_of(realm, receiver, &timeline);
	if (status != 0)
		return status;

	/* An absent or undefined optional argument clears all buffered marks. */
	if (count == 0U || args[0] == VM_VALUE_UNDEFINED) {
		wb_vector_clear(&timeline->entries);
		return 0;
	}

	/* Conversion can reenter clear or append; only the resulting current buffer is edited. */
	status = vm_to_string(realm, args[0], &name);
	if (status != 0)
		return status;

	/* Compaction allocates nothing and cannot collect the converted name. */
	entries = timeline->entries.items;
	output = 0U;
	for (input = 0; input < timeline->entries.count; input++) {
		object = (struct vm_object *)vm_value_as_cell(entries[input]);
		mark = (struct environment_mark *)vm_value_as_cell(object->internal);
		matches = vm_string_equal(name, mark->name);
		if (matches)
			continue;

		/* Keep unmatched entries in their existing stable order. */
		entries[output] = entries[input];
		output++;
	}

	/* The collector traces only the compacted live portion of the vector. */
	timeline->entries.count = output;

	/* Succeeded: matching marks no longer belong to the timeline buffer. */
	return 0;
}

/* Defines the standard descriptive tag without replacing genuine native brands. */
static int
environment_install_entry_tag(
	struct vm_realm *realm,
	struct vm_object *prototype,
	const char *name)
{
	struct vm_cell *root;
	vm_value value;
	vm_value key;
	int status;

	/* Keep the generated tag string through property-storage allocation. */
	root = NULL;
	status = vm_heap_add_root(realm->heap, &root);
	if (status != 0)
		return status;

	/* A tag describes the real native interface rather than supplying its brand. */
	status = bind_string(realm, name, &value);
	if (status != 0) {
		vm_heap_remove_root(realm->heap, &root);
		return status;
	}

	/* The well-known symbol itself remains owned by the actual realm. */
	root = vm_value_as_cell(value);
	key = vm_symbol_key(realm, VM_SYMBOL_TO_STRING_TAG);
	status = vm_object_define(realm->heap, prototype, key, value, VM_PROPERTY_CONFIGURABLE);
	vm_heap_remove_root(realm->heap, &root);

	/* A failed definition leaves interface installation incomplete. */
	if (status != 0)
		return status;

	/* Succeeded: Object.prototype.toString can report this native interface. */
	return 0;
}

/* Publishes real readonly entry fields after the native interface prototypes exist. */
static int
environment_install_mark_fields(
	struct bind_window *window)
{
	struct vm_realm *realm;
	struct vm_object *prototype;
	struct vm_property property;
	vm_value key;
	const char *name;
	int part;
	int found;
	int status;

	/* Part selection stays private to each native getter's function data. */
	realm = window->realm;
	for (part = 0; part < 5; part++) {
		prototype = window->prototypes[BIND_PERFORMANCE_ENTRY];
		name = environment_entry_parts[part];

		/* Only mark detail belongs to the more specific prototype. */
		if (part == 4)
			prototype = window->prototypes[BIND_PERFORMANCE_MARK];

		/* Existing accessor construction roots getter and accessor allocations. */
		status = environment_part_accessor(realm, prototype, name, environment_mark_part, NULL, part);
		if (status != 0)
			return status;

		/* WebIDL attributes are enumerable as well as configurable and readonly. */
		key = vm_key_from_ascii(realm->heap, name);
		if (key == VM_VALUE_EMPTY)
			return ENOMEM;

		/* Read the genuine accessor object without replacing its native getter identity. */
		found = vm_object_get_own(prototype, key, &property);
		if (found < 0)
			return -found;

		/* A missing newly installed field indicates incomplete publication. */
		if (found == 0)
			return EINVAL;

		/* Refining one flag preserves the accessor's getter and absent setter. */
		status = vm_object_define(realm->heap, prototype, key, *property.value, property.attributes | VM_PROPERTY_ENUMERABLE);
		if (status != 0)
			return status;
	}

	/* The common entry serializer is also an enumerable WebIDL operation. */
	prototype = window->prototypes[BIND_PERFORMANCE_ENTRY];
	key = vm_key_from_ascii(realm->heap, "toJSON");
	if (key == VM_VALUE_EMPTY)
		return ENOMEM;

	/* Preserve the serializer installed from the actual native interface table. */
	found = vm_object_get_own(prototype, key, &property);
	if (found < 0)
		return -found;

	/* An absent serializer cannot be exposed as a complete native interface. */
	if (found == 0)
		return EINVAL;

	/* WebIDL methods retain writable and configurable flags while becoming enumerable. */
	status = vm_object_define(realm->heap, prototype, key, *property.value, property.attributes | VM_PROPERTY_ENUMERABLE);
	if (status != 0)
		return status;

	/* Tags describe the actual interface prototypes while native invocation still checks private state. */
	status = environment_install_entry_tag(realm, window->prototypes[BIND_PERFORMANCE_ENTRY], "PerformanceEntry");
	if (status != 0)
		return status;

	/* A concrete mark reports its own tag through the inherited native object protocol. */
	status = environment_install_entry_tag(realm, window->prototypes[BIND_PERFORMANCE_MARK], "PerformanceMark");
	if (status != 0)
		return status;

	/* Succeeded: genuine entry prototypes expose their readonly native data. */
	return 0;
}

/* Takes a performance method that records something: nothing is recorded in this pass. */
static int
environment_performance_nothing(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	UNUSED_PARAMETER(realm);
	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Succeeded: undefined. */
	*result = VM_VALUE_UNDEFINED;
	return 0;
}

/* Reports one part of the location (the function's data is a BIND_LOCATION_*). */
static int
environment_location_part(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_function *callee;
	int part;
	int error;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Which part. */
	callee = js_builtin_callee(realm);
	part = vm_value_as_int32(callee->data);

	/* The part's text from the host. */
	error = bind_location_part(bind_window_of(realm), part, result);
	if (error != 0)
		return error;

	/* Succeeded: the part is reported. */
	return 0;
}

/* Matches the special hidden state without changing the stored spelling. */
static int
environment_hidden_until_found(
	const struct vm_string *value)
{
	static const char keyword[] = "until-found";
	size_t index;
	uint16_t unit;

	if (value == NULL || value->length != sizeof(keyword) - 1U)
		return 0;
	for (index = 0U; index < sizeof(keyword) - 1U; index++) {
		unit = vm_string_at(value, index);
		if (unit >= 'A' && unit <= 'Z')
			unit = (uint16_t)(unit + 0x20U);
		if (unit != (uint16_t)keyword[index])
			return 0;
	}
	return 1;
}

/* Adds open to one closed details element while its caller roots the node. */
static int
environment_open_details(
	struct vm_realm *realm,
	struct dom_element *element)
{
	struct vm_cell *root;
	struct vm_string *name;
	struct vm_string *value;
	int error;

	name = vm_atom_from_ascii(realm->heap, "open");
	if (name == NULL)
		return ENOMEM;
	root = &name->cell;
	error = vm_heap_add_root(realm->heap, &root);
	if (error != 0)
		return error;
	value = vm_string_from_utf8(realm->heap, "", 0U);
	if (value == NULL)
		error = ENOMEM;
	else {
		error = dom_element_set_attribute(element, name, value);
		if (error == 0)
			error = bind_details_queue_toggle(bind_window_of(realm), element);
	}
	vm_heap_remove_root(realm->heap, &root);
	return error;
}

/* Finds the first connected element with the decoded fragment's id. */
static int
environment_fragment_target(
	struct bind_window *window,
	const struct wb_buffer *fragment,
	struct dom_node **target)
{
	struct dom_node *walk;
	struct dom_element *element;
	struct vm_string *id;
	struct wb_buffer decoded;
	struct wb_buffer text;
	const char *hash;
	size_t length;
	int error;

	*target = NULL;
	if (window->document == NULL || fragment->length == 0U)
		return 0;
	hash = wb_buffer_string(fragment);
	length = fragment->length;
	if (hash[0] == '#') {
		hash++;
		length--;
	}
	if (length == 0U)
		return 0;
	wb_buffer_init(&decoded);
	error = net_percent_decode(hash, length, &decoded);
	if (error != 0) {
		wb_buffer_release(&decoded);
		return error;
	}
	for (walk = bind_following(&window->document->node, &window->document->node);
	     walk != NULL;
	     walk = bind_following(walk, &window->document->node)) {
		if (walk->type != DOM_ELEMENT)
			continue;
		element = (struct dom_element *)walk;
		id = dom_attribute_ascii(element, "id");
		if (id == NULL)
			continue;
		wb_buffer_init(&text);
		error = vm_string_to_utf8(id, &text);
		if (error == 0 && text.length == decoded.length &&
		    memcmp(text.data, decoded.data, text.length) == 0)
			*target = walk;
		wb_buffer_release(&text);
		if (error != 0 || *target != NULL)
			break;
	}
	wb_buffer_release(&decoded);
	return error;
}

/* Reveals the target's original ancestors from the target outward. */
static int
environment_reveal_target(
	struct bind_window *window,
	struct dom_node *target)
{
	struct environment_reveal_entry *entries;
	struct dom_node *walk;
	struct dom_element *element;
	struct vm_string *hidden;
	struct vm_string *name;
	size_t count;
	size_t index;
	size_t rooted;
	int error;
	int canceled;

	/* Freeze the ancestor path before any event handler can reparent a node. */
	count = 0U;
	for (walk = target; walk != NULL && walk != &window->document->node; walk = walk->parent) {
		if (walk->type == DOM_ELEMENT)
			count++;
	}
	if (count == 0U)
		return 0;
	entries = calloc(count, sizeof(*entries));
	if (entries == NULL)
		return ENOMEM;
	index = 0U;
	for (walk = target; walk != NULL && walk != &window->document->node; walk = walk->parent) {
		if (walk->type != DOM_ELEMENT)
			continue;
		element = (struct dom_element *)walk;
		entries[index].node = &walk->cell;
		entries[index].details = element->ns == DOM_NS_HTML && element->tag == DOM_TAG_DETAILS &&
		    dom_attribute_ascii(element, "open") == NULL;
		hidden = dom_attribute_ascii(element, "hidden");
		entries[index].hidden = environment_hidden_until_found(hidden);
		index++;
	}

	/* The snapshot itself holds each node alive during script callbacks. */
	error = 0;
	rooted = 0U;
	for (index = 0U; index < count; index++) {
		error = vm_heap_add_root(window->realm->heap, &entries[index].node);
		if (error != 0)
			break;
		rooted++;
	}
	for (index = 0U; error == 0 && index < count; index++) {
		if (!entries[index].details && !entries[index].hidden)
			continue;
		walk = (struct dom_node *)entries[index].node;
		if (!dom_is_inclusive_ancestor(&window->document->node, walk))
			break;
		element = (struct dom_element *)walk;
		if (entries[index].details && dom_attribute_ascii(element, "open") == NULL) {
			error = environment_open_details(window->realm, element);
			if (error != 0)
				break;
		}
		if (!entries[index].hidden)
			continue;
		if (!environment_hidden_until_found(dom_attribute_ascii(element, "hidden")))
			break;
		canceled = 0;
		error = bind_fire_event(window, walk, "beforematch", BIND_EVENT_BUBBLES, &canceled);
		if (error != 0)
			break;
		if (!dom_is_inclusive_ancestor(&window->document->node, walk) ||
		    !environment_hidden_until_found(dom_attribute_ascii(element, "hidden")))
			break;
		name = vm_atom_from_ascii(window->realm->heap, "hidden");
		if (name == NULL) {
			error = ENOMEM;
			break;
		}
		error = dom_element_remove_attribute(element, name);
		if (error != 0)
			break;
	}
	for (index = rooted; index != 0U; index--)
		vm_heap_remove_root(window->realm->heap, &entries[index - 1U].node);
	free(entries);
	return error;
}

/* Reports location.toString(): the whole URL. */
static int
environment_location_to_string(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The href. */
	error = bind_location_part(bind_window_of(realm), BIND_LOCATION_HREF, result);
	if (error != 0)
		return error;

	/* Succeeded: the URL is reported. */
	return 0;
}

/* Makes new Image(width, height): an img element with the sizes given as its attributes. */
static int
environment_image_construct(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct dom_element *element;
	struct vm_string *name;
	struct vm_string *value;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The element, in the HTML namespace of the window's document. */
	window = bind_window_of(realm);
	name = vm_atom_from_ascii(realm->heap, "img");
	if (name == NULL)
		return ENOMEM;
	element = dom_element_create(window->document, DOM_NS_HTML, name, NULL);
	if (element == NULL)
		return ENOMEM;

	/* The width, when given. */
	if (count > 0) {
		status = bind_to_string(realm, args[0], &value);
		if (status != 0)
			return status;
		name = vm_atom_from_ascii(realm->heap, "width");
		if (name == NULL)
			return ENOMEM;
		status = dom_element_set_attribute(element, name, value);
		if (status != 0)
			return status;
	}

	/* The height, when given. */
	if (count > 1) {
		status = bind_to_string(realm, args[1], &value);
		if (status != 0)
			return status;
		name = vm_atom_from_ascii(realm->heap, "height");
		if (name == NULL)
			return ENOMEM;
		status = dom_element_set_attribute(element, name, value);
		if (status != 0)
			return status;
	}

	/* Its object. */
	status = bind_wrap(window, &element->node, result);
	if (status != 0)
		return status;

	/* Succeeded: the image element is made. */
	return 0;
}

/* Decodes a Base64 string into a string whose code units are bytes (atob). */
static int
environment_atob(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *input;
	struct vm_string *decoded;
	struct wb_buffer clean;
	struct wb_buffer bytes;
	uint32_t bits;
	uint16_t unit;
	size_t index;
	int value;
	int held;
	int space;
	int status;

	UNUSED_PARAMETER(this_value);

	/* Converts the argument and removes the ASCII whitespace Base64 permits. */
	status = bind_to_string(realm, js_argument(args, count, 0), &input);
	if (status != 0)
		return status;
	wb_buffer_init(&clean);
	wb_buffer_init(&bytes);
	for (index = 0; index < input->length && status == 0; index++) {
		unit = vm_string_at(input, index);
		space = unit == 0x09U || unit == 0x0aU || unit == 0x0cU || unit == 0x0dU || unit == 0x20U;
		if (!space && unit <= 0x7fU)
			status = wb_buffer_append_byte(&clean, (unsigned char)unit);
		if (!space && unit > 0x7fU)
			status = EINVAL;
	}

	/* One or two padding characters at a quartet's end are optional. */
	if (status == 0 && clean.length % 4U == 0U && clean.length != 0 && clean.data[clean.length - 1U] == '=') {
		clean.length--;
		if (clean.length != 0 && clean.data[clean.length - 1U] == '=')
			clean.length--;
	}

	/* A lone character after whole groups cannot make a byte. */
	if (status == 0 && clean.length % 4U == 1U)
		status = EINVAL;

	/* Six bits per character make each output byte. */
	bits = 0;
	held = 0;
	for (index = 0; index < clean.length && status == 0; index++) {
		value = environment_base64_value(clean.data[index]);
		if (value < 0) {
			status = EINVAL;
			continue;
		}

		/* The six bits join those held from the preceding character. */
		bits = (bits << 6) | (uint32_t)value;
		held += 6;
		if (held >= 8) {
			held -= 8;
			status = wb_buffer_append_byte(&bytes, (unsigned char)(bits >> held));
		}
	}

	/* Invalid input throws the DOM exception browsers use. */
	if (status == EINVAL) {
		wb_buffer_release(&clean);
		wb_buffer_release(&bytes);
		return bind_throw_dom(realm, "InvalidCharacterError", "The string is not correctly encoded.");
	}

	/* Other failures are allocation failures. */
	if (status != 0) {
		wb_buffer_release(&clean);
		wb_buffer_release(&bytes);
		return status;
	}

	/* A binary string preserves every decoded byte. */
	decoded = vm_string_from_latin1(realm->heap, bytes.data, bytes.length);
	wb_buffer_release(&clean);
	wb_buffer_release(&bytes);
	if (decoded == NULL)
		return ENOMEM;
	*result = vm_value_cell(decoded);
	return 0;
}

/* Encodes a binary string as Base64 (btoa). */
static int
environment_btoa(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	struct vm_string *input;
	struct vm_string *encoded;
	struct wb_buffer text;
	uint32_t group;
	uint16_t units[3];
	uint16_t unit;
	unsigned char third;
	unsigned char fourth;
	size_t index;
	size_t left;
	size_t take;
	int status;

	UNUSED_PARAMETER(this_value);

	/* Every input code unit must fit one byte. */
	status = bind_to_string(realm, js_argument(args, count, 0), &input);
	if (status != 0)
		return status;
	for (index = 0; index < input->length; index++) {
		unit = vm_string_at(input, index);
		if (unit > 0xffU)
			return bind_throw_dom(realm, "InvalidCharacterError", "The string contains a character outside of the Latin1 range.");
	}

	/* Encodes three bytes into four alphabet characters. */
	wb_buffer_init(&text);
	status = 0;
	for (index = 0; index < input->length && status == 0; index += take) {
		left = input->length - index;
		take = 3U;
		if (left < take)
			take = left;
		units[0] = vm_string_at(input, index);
		units[1] = 0;
		units[2] = 0;
		if (take > 1U)
			units[1] = vm_string_at(input, index + 1U);
		if (take > 2U)
			units[2] = vm_string_at(input, index + 2U);
		group = ((uint32_t)units[0] << 16) | ((uint32_t)units[1] << 8) | units[2];
		third = '=';
		fourth = '=';
		if (take > 1U)
			third = (unsigned char)alphabet[(group >> 6) & 63U];
		if (take > 2U)
			fourth = (unsigned char)alphabet[group & 63U];
		status = wb_buffer_append_byte(&text, (unsigned char)alphabet[(group >> 18) & 63U]);
		if (status == 0)
			status = wb_buffer_append_byte(&text, (unsigned char)alphabet[(group >> 12) & 63U]);
		if (status == 0)
			status = wb_buffer_append_byte(&text, third);
		if (status == 0)
			status = wb_buffer_append_byte(&text, fourth);
	}

	/* An allocation failure stops the encoding. */
	if (status != 0) {
		wb_buffer_release(&text);
		return status;
	}

	/* The encoding is ASCII. */
	encoded = vm_string_from_latin1(realm->heap, text.data, text.length);
	wb_buffer_release(&text);
	if (encoded == NULL)
		return ENOMEM;
	*result = vm_value_cell(encoded);
	return 0;
}

/* Drops unfinished script requests' roots after their host requests were cancelled. */
void
bind_environment_release(
	struct bind_window *window)
{
	struct environment_mutation_observer *observer_next;
	struct environment_mutation_observer *observer;

	/* Each remaining host request has already been cancelled by the page. */
	bind_requests_release(window);

	/* Mutation observers retain their objects, callbacks, targets and pending records. */
	observer = window->mutation_observers;
	while (observer != NULL) {
		observer_next = observer->next;
		environment_mutation_clear(window, observer, 1);
		vm_heap_remove_root(window->realm->heap, &observer->callback);
		vm_heap_remove_root(window->realm->heap, &observer->observer);
		free(observer);
		observer = observer_next;
	}

	/* The window no longer owns an observer. */
	window->mutation_observers = NULL;
}

/* Makes an observer whose callback is kept by its observe method. */
static int
environment_observer_construct(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_function *callee;
	struct vm_function *method;
	struct vm_object *observer;
	vm_value data_values[2];
	vm_value data;
	vm_value callback;
	int callable;
	int type;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The first argument is the callable notified by observe. */
	callback = js_argument(args, count, 0);
	callable = vm_value_is_callable(callback);
	if (!callable)
		return vm_throw_type_error(realm, "The observer callback is not a function.");
	callee = js_builtin_callee(realm);
	type = vm_value_as_int32(callee->data);
	data_values[0] = callback;
	data_values[1] = vm_value_int32(type);
	status = js_builtin_array(realm, data_values, 2, &data);
	if (status != 0)
		return status;

	/* The observer's methods; observe keeps the callback and kind in its function data. */
	observer = vm_object_create(realm->heap, realm->object_prototype);
	if (observer == NULL)
		return ENOMEM;
	status = js_builtin_function(realm, "observe", 1, environment_observer_observe, NULL, &method);
	if (status == 0) {
		method->data = data;
		status = js_builtin_value(realm, observer, "observe", vm_value_cell(method), JS_BUILTIN_METHOD);
	}

	/* The other methods do not need per-observer state. */
	if (status == 0)
		status = js_builtin_method(realm, observer, "unobserve", 1, environment_observer_disconnect);
	if (status == 0)
		status = js_builtin_method(realm, observer, "disconnect", 0, environment_observer_disconnect);
	if (status == 0)
		status = js_builtin_method(realm, observer, "takeRecords", 0, environment_observer_records);
	if (status == 0 && type == 0)
		status = environment_mutation_add(bind_window_of(realm), observer, callback);
	if (status != 0)
		return status;

	/* Succeeded: new reports the observer object. */
	*result = vm_value_cell(observer);
	return 0;
}

/* Begins observing one target and queues its initial layout observation. */
static int
environment_observer_observe(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct environment_mutation_observer *observer;
	struct environment_mutation_settings settings;
	struct bind_window *window;
	struct dom_node *node;
	struct vm_function *callee;
	struct vm_object *data;
	struct vm_cell *roots[4];
	vm_value entry;
	vm_value entries;
	vm_value callback;
	vm_value options;
	vm_value target;
	unsigned index;
	unsigned registered;
	int is_cell;
	int type;
	int status;

	/* Constructor-owned data distinguishes mutation registration from layout observation. */
	*result = VM_VALUE_UNDEFINED;
	callee = js_builtin_callee(realm);
	data = (struct vm_object *)vm_value_as_cell(callee->data);
	callback = data->elements[0];
	type = vm_value_as_int32(data->elements[1]);
	window = bind_window_of(realm);
	if (type == 0) {
		/* Reject a borrowed receiver before running any option getter. */
		observer = environment_mutation_find(window, this_value);
		if (observer == NULL) {
			status = bind_throw_illegal(realm);
			if (status != VM_THROWN)
				return status;

			/* Succeeded: the invalid receiver is refused by its native exception. */
			return VM_THROWN;
		}

		/* Attribute and child-list registration both require an actual Node. */
		target = js_argument(args, count, 0);
		node = bind_node_of(target);
		if (node == NULL) {
			status = vm_throw_type_error(realm, "The observation target is not a Node.");
			if (status != VM_THROWN)
				return status;

			/* Succeeded: a non-Node target is refused. */
			return VM_THROWN;
		}

		/* Getter/iterator calls retain the receiver, target, dictionary and unpublished filter. */
		options = js_argument(args, count, 1);
		memset(&settings, 0, sizeof(settings));
		memset(roots, 0, sizeof(roots));
		roots[0] = vm_value_as_cell(target);
		is_cell = vm_value_is_cell(options);
		if (is_cell)
			roots[1] = vm_value_as_cell(options);
		roots[3] = observer->observer;
		registered = 0;
		status = 0;
		for (index = 0; index < 4U; index++) {
			status = vm_heap_add_root(realm->heap, &roots[index]);
			if (status != 0)
				break;
			registered++;
		}

		/* No invalid conversion can discard an earlier registration or its pending records. */
		if (status == 0) {
			do {
				status = environment_mutation_options(realm, options, &settings, &roots[2]);
				if (status != 0)
					break;

				/* This native implementation updates its one registration without deleting queued records. */
				environment_mutation_clear(window, observer, 0);
				observer->target = roots[0];
				status = environment_root(window, &observer->target);
				if (status != 0) {
					observer->target = NULL;
					break;
				}

				/* Only a completely copied filter enters persistent or managed ownership. */
				observer->attribute_filter = settings.attribute_filter;
				if (observer->attribute_filter != NULL) {
					status = environment_root(window, &observer->attribute_filter);
					if (status != 0) {
						observer->attribute_filter = NULL;
						environment_mutation_clear(window, observer, 0);
						break;
					}
				}

				/* All flags become visible together after conversion and ownership succeed. */
				observer->child_list = settings.child_list;
				observer->subtree = settings.subtree;
				observer->attributes = settings.attributes;
				observer->attribute_old_value = settings.attribute_old_value;
			} while (0);
		}

		/* Every temporary native slot is retired before this C invocation returns. */
		while (registered != 0) {
			registered--;
			vm_heap_remove_root(realm->heap, &roots[registered]);
		}

		/* Allocation, conversion or property failures keep their original status. */
		if (status != 0)
			return status;

		/* Succeeded: the requested ordinary mutation flags and copied filter are registered. */
		return 0;
	}

	/* Layout observations retain their existing initial-entry delivery path. */
	target = js_argument(args, count, 0);
	status = environment_observer_entry(window, target, type, &entry);
	if (status != 0)
		return status;

	/* The initial entry is the one argument of the ordinary queued layout callback. */
	status = js_builtin_array(realm, &entry, 1, &entries);
	if (status != 0)
		return status;

	/* The existing callback remains asynchronous rather than entering the observer now. */
	status = vm_enqueue_job(realm, callback, entries);
	if (status != 0)
		return status;

	/* Succeeded: the initial layout-observer callback is queued. */
	return 0;
}

/* Stops a MutationObserver; the layout observers have no retained target. */
static int
environment_observer_disconnect(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct environment_mutation_observer *observer;
	struct bind_window *window;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Layout observers retain their existing no-op disconnect; mutation observers release registrations. */
	*result = VM_VALUE_UNDEFINED;
	window = bind_window_of(realm);
	observer = environment_mutation_find(window, this_value);
	if (observer != NULL)
		environment_mutation_clear(window, observer, 1);

	/* Succeeded: any mutation target, filter and undelivered records are disconnected. */
	return 0;
}

/* Takes a MutationObserver's pending records, or an empty list. */
static int
environment_observer_records(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct environment_mutation_observer *observer;
	struct vm_cell *records;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	observer = environment_mutation_find(bind_window_of(realm), this_value);
	if (observer != NULL && observer->records != NULL) {
		records = observer->records;
		vm_heap_remove_root(realm->heap, &observer->records);
		observer->records = NULL;
		*result = vm_value_cell(records);
		return 0;
	}

	/* An observer without pending records returns an empty sequence. */
	return js_builtin_array(realm, NULL, 0, result);
}

/* Keeps primary observers rooted and managed observers in their window graph. */
static int
environment_root(
	struct bind_window *window,
	struct vm_cell **slot)
{
	int status;

	/* The owned window's trace supplies reachability without a permanent root. */
	if (window->realm->managed)
		return 0;

	/* Explicit primary ownership retains its existing observer-root contract. */
	status = vm_heap_add_root(window->realm->heap, slot);
	if (status != 0)
		return status;

	/* Succeeded: the primary observer reference remains globally rooted. */
	return 0;
}

/* Retains one newly constructed MutationObserver and its callback. */
static int
environment_mutation_add(
	struct bind_window *window,
	struct vm_object *observer,
	vm_value callback)
{
	struct environment_mutation_observer *made;
	struct environment_mutation_observer **link;
	int status;

	/* The registered observer remains alive even when the script drops its last reference. */
	made = calloc(1, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	made->observer = &observer->cell;
	made->callback = vm_value_as_cell(callback);
	status = environment_root(window, &made->observer);
	if (status == 0)
		status = environment_root(window, &made->callback);
	if (status != 0) {
		if (made->observer != NULL)
			vm_heap_remove_root(window->realm->heap, &made->observer);
		free(made);
		return status;
	}

	/* Registration order is callback delivery order. */
	link = &window->mutation_observers;
	while (*link != NULL)
		link = &(*link)->next;
	*link = made;
	return 0;
}

/* Finds the state belonging to a MutationObserver object. */
static struct environment_mutation_observer *
environment_mutation_find(
	struct bind_window *window,
	vm_value observer)
{
	struct environment_mutation_observer *entry;
	struct vm_cell *cell;
	int is_object;

	/* Only the exact object made by this constructor has state. */
	is_object = vm_value_is_object(observer);
	if (!is_object)
		return NULL;
	cell = vm_value_as_cell(observer);
	for (entry = window->mutation_observers; entry != NULL; entry = entry->next) {
		if (entry->observer == cell)
			return entry;
	}

	/* No registered observer owns this object. */
	return NULL;
}

/* Drops a MutationObserver's target and any records not delivered yet. */
static void
environment_mutation_clear(
	struct bind_window *window,
	struct environment_mutation_observer *observer,
	int discard_records)
{
	/* The old target is no longer an active observation edge. */
	if (observer->target != NULL) {
		vm_heap_remove_root(window->realm->heap, &observer->target);
		observer->target = NULL;
	}

	/* A copied name filter has the same owner policy as the registered target. */
	if (observer->attribute_filter != NULL) {
		vm_heap_remove_root(window->realm->heap, &observer->attribute_filter);
		observer->attribute_filter = NULL;
	}

	/* Disconnect discards records; updating one registration preserves already queued snapshots. */
	if (discard_records && observer->records != NULL) {
		vm_heap_remove_root(window->realm->heap, &observer->records);
		observer->records = NULL;
	}

	/* A later observe call supplies a complete new set of flags. */
	observer->child_list = 0;
	observer->subtree = 0;
	observer->attributes = 0;
	observer->attribute_old_value = 0;
}

/* Reads the MutationObserver options implemented by this pass. */
static int
environment_mutation_options(
	struct vm_realm *realm,
	vm_value options,
	struct environment_mutation_settings *settings,
	struct vm_cell **filter)
{
	vm_value value;
	int filter_present;
	int old_present;
	int attributes_present;
	int character_present;
	int character_old_present;
	int character_data;
	int character_old_value;
	int present;
	int status;

	/* Dictionary members are read in WebIDL lexicographic order, treating undefined as absent. */
	memset(settings, 0, sizeof(*settings));
	status = bind_get_option(realm, options, "attributeFilter", &filter_present, &value);
	if (status != 0)
		return status;

	/* Sequence conversion happens before any later dictionary getter can run. */
	if (filter_present) {
		status = environment_mutation_filter(realm, value, filter);
		if (status != 0)
			return status;
	}

	/* A present old-value option also affects the default attributes flag. */
	status = environment_mutation_boolean(realm, options, "attributeOldValue", &old_present, &settings->attribute_old_value);
	if (status != 0)
		return status;

	/* An explicit attributes value must not be confused with its omitted default. */
	status = environment_mutation_boolean(realm, options, "attributes", &attributes_present, &settings->attributes);
	if (status != 0)
		return status;

	/* Character data remains a recognized but separately unimplemented notification kind. */
	status = environment_mutation_boolean(realm, options, "characterData", &character_present, &character_data);
	if (status != 0)
		return status;

	/* Its dictionary validation follows the same optional-boolean default rule. */
	status = environment_mutation_boolean(realm, options, "characterDataOldValue", &character_old_present, &character_old_value);
	if (status != 0)
		return status;

	/* Existing child-list registration keeps its ordinary default and scope. */
	status = environment_mutation_boolean(realm, options, "childList", &present, &settings->child_list);
	if (status != 0)
		return status;

	/* Subtree applies independently to the enabled ordinary mutation kinds. */
	status = environment_mutation_boolean(realm, options, "subtree", &present, &settings->subtree);
	if (status != 0)
		return status;

	/* Presence, including false oldValue and an empty filter, enables an omitted attributes flag. */
	if (!attributes_present && (old_present || filter_present))
		settings->attributes = 1;

	/* Character-data default inference validates options without claiming record support. */
	if (!character_present && character_old_present)
		character_data = 1;

	/* A dictionary with no enabled mutation kind cannot establish an observation. */
	if (!settings->child_list && !settings->attributes && !character_data) {
		status = vm_throw_type_error(realm, "MutationObserver has no enabled mutation type.");
		if (status != VM_THROWN)
			return status;

		/* Succeeded: the empty observation request is refused. */
		return VM_THROWN;
	}

	/* Explicitly disabling attributes is incompatible with requesting old attribute values. */
	if (settings->attribute_old_value && !settings->attributes) {
		status = vm_throw_type_error(realm, "attributeOldValue requires attributes.");
		if (status != VM_THROWN)
			return status;

		/* Succeeded: the contradictory old-value request is refused. */
		return VM_THROWN;
	}

	/* A present filter likewise requires attribute observation, even when it is empty. */
	if (filter_present && !settings->attributes) {
		status = vm_throw_type_error(realm, "attributeFilter requires attributes.");
		if (status != VM_THROWN)
			return status;

		/* Succeeded: the contradictory filter request is refused. */
		return VM_THROWN;
	}

	/* The recognized character-data options cannot contradict their own enabled flag. */
	if (character_old_value && !character_data) {
		status = vm_throw_type_error(realm, "characterDataOldValue requires characterData.");
		if (status != VM_THROWN)
			return status;

		/* Succeeded: the contradictory character-data dictionary is refused. */
		return VM_THROWN;
	}

	/* Succeeded: every flag is valid and the caller still roots the copied sequence. */
	settings->attribute_filter = *filter;
	return 0;
}

/* Converts one optional Boolean without conflating undefined with a present false value. */
static int
environment_mutation_boolean(
	struct vm_realm *realm,
	vm_value options,
	const char *name,
	int *present,
	int *enabled)
{
	vm_value value;
	int status;

	/* Dictionary lookup runs the actual inherited getter exactly once. */
	*enabled = 0;
	status = bind_get_option(realm, options, name, present, &value);
	if (status != 0)
		return status;

	/* Only a defined member contributes its Boolean conversion. */
	if (*present)
		*enabled = vm_to_boolean(value);

	/* Succeeded: absence and the converted value remain independent outputs. */
	return 0;
}

/* Copies an iterable sequence while preserving conversion and abrupt completion ordering. */
static int
environment_mutation_filter(
	struct vm_realm *realm,
	vm_value source,
	struct vm_cell **filter)
{
	struct vm_cell *roots[4];
	struct vm_object *array;
	struct vm_string *name;
	vm_value iterator;
	vm_value member;
	vm_value made;
	unsigned index;
	unsigned registered;
	int object;
	int cell;
	int done;
	int status;

	/* WebIDL sequences require an object before consulting its iterator. */
	object = vm_value_is_object(source);
	if (!object) {
		status = vm_throw_type_error(realm, "attributeFilter must be an iterable object.");
		if (status != VM_THROWN)
			return status;

		/* Succeeded: a primitive filter is refused by the required native exception. */
		return VM_THROWN;
	}

	/* The source, iterator, yielded value and converted string survive callbacks. */
	memset(roots, 0, sizeof(roots));
	roots[0] = vm_value_as_cell(source);
	registered = 0;
	status = 0;
	for (index = 0; index < 4U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			break;
		registered++;
	}

	/* Conversion publishes only a private caller-rooted array, never the original sequence. */
	if (status == 0) {
		do {
			status = vm_iter_start(realm, source, &iterator);
			if (status != 0)
				break;
			roots[1] = vm_value_as_cell(iterator);

			/* The copied sequence becomes rooted before converting its first member. */
			status = js_builtin_array(realm, NULL, 0, &made);
			if (status != 0)
				break;
			*filter = vm_value_as_cell(made);
			array = (struct vm_object *)*filter;

			/* Each member becomes a string before the next observable iterator step. */
			for (;;) {
				roots[2] = NULL;
				roots[3] = NULL;
				status = vm_iter_next(realm, iterator, &member, &done);
				if (status != 0 || done)
					break;
				cell = vm_value_is_cell(member);
				if (cell)
					roots[2] = vm_value_as_cell(member);

				/* A user conversion may allocate, mutate the original sequence, or throw. */
				status = vm_to_string(realm, member, &name);
				if (status != 0)
					break;
				roots[3] = &name->cell;

				/* The private array uses the VM's bounded integer-index representation. */
				if (array->length > INT32_MAX) {
					status = EOVERFLOW;
					break;
				}

				/* Store the converted string, not the original mutable member. */
				status = vm_object_define(realm->heap, array, vm_value_int32((int32_t)array->length), vm_value_cell(name), VM_PROPERTY_DEFAULT);
				if (status != 0)
					break;
			}
		} while (0);
	}

	/* Every root has stable storage until the iteration or conversion has ended. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Preserve conversion, iteration and allocation failures precisely. */
	if (status != 0)
		return status;

	/* Succeeded: the caller owns an independent sequence of converted names. */
	return 0;
}

/* Publishes one ordinary attribute snapshot with genuine immutable empty NodeLists. */
static int
environment_attribute_record(
	struct bind_window *window,
	struct environment_mutation_observer *observer,
	struct dom_element *element,
	struct vm_string *name,
	struct vm_string *old_value)
{
	struct vm_realm *realm;
	struct vm_object *record;
	struct vm_object *array;
	struct vm_cell *roots[5];
	vm_value target;
	vm_value added;
	vm_value removed;
	vm_value type;
	vm_value value;
	vm_value pending;
	unsigned index;
	unsigned registered;
	int new_pending;
	int status;

	/* The caller retains the native target and retired strings through all allocations. */
	realm = window->realm;
	memset(roots, 0, sizeof(roots));
	registered = 0;
	new_pending = 0;
	status = 0;
	for (index = 0; index < 5U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			break;
		registered++;
	}

	/* No incomplete record reaches the observer's pending sequence. */
	if (status == 0) {
		do {
			status = bind_wrap(window, &element->node, &target);
			if (status != 0)
				break;
			roots[0] = vm_value_as_cell(target);

			/* Independent native lists preserve their real NodeList prototype and owner. */
			status = bind_empty_node_list(window, &added);
			if (status != 0)
				break;
			roots[1] = vm_value_as_cell(added);

			/* Removed nodes are independently empty for an attribute mutation. */
			status = bind_empty_node_list(window, &removed);
			if (status != 0)
				break;
			roots[2] = vm_value_as_cell(removed);

			/* The type string remains rooted while property names are allocated. */
			status = bind_string(realm, "attributes", &type);
			if (status != 0)
				break;
			roots[3] = vm_value_as_cell(type);

			/* Construct the complete snapshot before publishing it. */
			record = vm_object_create(realm->heap, realm->object_prototype);
			if (record == NULL) {
				status = ENOMEM;
				break;
			}

			/* Keep the unpublished payload alive while each property key is allocated. */
			roots[4] = &record->cell;

			/* Identify the ordinary attribute mutation. */
			status = js_builtin_value(realm, record, "type", type, VM_PROPERTY_DEFAULT);
			if (status != 0)
				break;

			/* Retain the actual changed element wrapper. */
			status = js_builtin_value(realm, record, "target", target, VM_PROPERTY_DEFAULT);
			if (status != 0)
				break;

			/* Attribute changes add no children. */
			status = js_builtin_value(realm, record, "addedNodes", added, VM_PROPERTY_DEFAULT);
			if (status != 0)
				break;

			/* Attribute changes remove no children. */
			status = js_builtin_value(realm, record, "removedNodes", removed, VM_PROPERTY_DEFAULT);
			if (status != 0)
				break;

			/* Attribute changes have no preceding child boundary. */
			status = js_builtin_value(realm, record, "previousSibling", VM_VALUE_NULL, VM_PROPERTY_DEFAULT);
			if (status != 0)
				break;

			/* Attribute changes have no following child boundary. */
			status = js_builtin_value(realm, record, "nextSibling", VM_VALUE_NULL, VM_PROPERTY_DEFAULT);
			if (status != 0)
				break;

			/* Preserve the committed ordinary attribute name. */
			status = js_builtin_value(realm, record, "attributeName", vm_value_cell(name), VM_PROPERTY_DEFAULT);
			if (status != 0)
				break;

			/* The ordinary mutation path has no attribute namespace. */
			status = js_builtin_value(realm, record, "attributeNamespace", VM_VALUE_NULL, VM_PROPERTY_DEFAULT);
			if (status != 0)
				break;

			/* Each observer independently requests or suppresses the retired value. */
			value = VM_VALUE_NULL;
			if (observer->attribute_old_value && old_value != NULL)
				value = vm_value_cell(old_value);
			status = js_builtin_value(realm, record, "oldValue", value, VM_PROPERTY_DEFAULT);
			if (status != 0)
				break;

			/* Allocate pending ownership only after every payload field is complete. */
			if (observer->records == NULL) {
				status = js_builtin_array(realm, NULL, 0, &pending);
				if (status != 0)
					break;
				observer->records = vm_value_as_cell(pending);
				status = environment_root(window, &observer->records);
				if (status != 0) {
					observer->records = NULL;
					break;
				}

				/* A failed first append must undo this newly established pending ownership. */
				new_pending = 1;
			}

			/* Guard the VM's integer-index limit before appending the completed payload. */
			array = (struct vm_object *)observer->records;
			if (array->length > INT32_MAX) {
				status = EOVERFLOW;
				break;
			}

			/* The observer now retains the complete record and both independent native lists. */
			status = vm_object_define(realm->heap, array, vm_value_int32((int32_t)array->length), vm_value_cell(record), VM_PROPERTY_DEFAULT);
		} while (0);
	}

	/* A failed first publication must not schedule an empty observer callback. */
	if (status != 0 && new_pending) {
		vm_heap_remove_root(realm->heap, &observer->records);
		observer->records = NULL;
	}

	/* Temporary payload roots end only after successful publication or complete failure. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Preserve the exact allocation or property-definition failure. */
	if (status != 0)
		return status;

	/* Succeeded: a matching observer owns its requested attribute snapshot. */
	return 0;
}

/*
 * Records an existing single-node notification through the sequence implementation.
 */
int
bind_environment_child_mutation(
	struct bind_window *window,
	struct dom_node *parent,
	struct dom_node *added,
	struct dom_node *removed)
{
	size_t added_count;
	size_t removed_count;
	int status;

	/* Preserve the old zero-or-one collection shape and sibling fields for existing callers. */
	added_count = 0;
	if (added != NULL)
		added_count = 1;

	/* Removal is independently optional for the existing single-node interface. */
	removed_count = 0;
	if (removed != NULL)
		removed_count = 1;

	/* The sequence API owns every native participant during record construction. */
	status = bind_environment_children_mutation(window, parent, &added, added_count, &removed, removed_count, NULL, NULL);
	if (status != 0)
		return status;

	/* Succeeded: matching observers retain this single-node change. */
	return 0;
}

/*
 * Records one complete child-list mutation with independent lists for each observer.
 * Input arrays are borrowed for this call; native roots retain all detached participants.
 */
int
bind_environment_children_mutation(
	struct bind_window *window,
	struct dom_node *parent,
	struct dom_node *const *added,
	size_t added_count,
	struct dom_node *const *removed,
	size_t removed_count,
	struct dom_node *previous,
	struct dom_node *next)
{
	struct environment_mutation_observer *observer;
	struct dom_node *target;
	struct vm_cell **roots;
	struct vm_heap *heap;
	size_t total;
	size_t index;
	size_t registered;
	int matches;
	int status;

	/* No observer can retain a record, so no allocation is needed in the common case. */
	if (window->mutation_observers == NULL)
		return 0;

	/* Reject native slot-count overflow before computing the allocation size. */
	if (added_count > SIZE_MAX - 3U)
		return ENOMEM;

	/* Parent and both sibling boundaries receive fixed slots before the two sequences. */
	total = added_count + 3U;
	if (removed_count > SIZE_MAX - total)
		return ENOMEM;

	/* calloc checks multiplication overflow and keeps unused sibling slots empty. */
	total += removed_count;
	roots = calloc(total, sizeof(*roots));
	if (roots == NULL)
		return ENOMEM;

	/* All arrays have stable storage until the last matching observer finishes. */
	heap = window->realm->heap;
	roots[0] = &parent->cell;
	if (previous != NULL)
		roots[1] = &previous->cell;

	/* The next boundary can be absent independently of the previous one. */
	if (next != NULL)
		roots[2] = &next->cell;

	/* Retain every addition even when the source fragment has already been emptied. */
	for (index = 0; index < added_count; index++)
		roots[index + 3U] = &added[index]->cell;

	/* Removed nodes no longer have a destination parent edge. */
	for (index = 0; index < removed_count; index++)
		roots[index + 3U + added_count] = &removed[index]->cell;

	/* Publish all ownership slots before any VM object or property key allocation. */
	registered = 0;
	for (index = 0; index < total; index++) {
		status = vm_heap_add_root(heap, &roots[index]);
		if (status != 0)
			goto cleanup;

		/* The cleanup count tracks successful registrations rather than input nodes. */
		registered++;
	}

	/* Each observer receives exactly one record for this complete mutation in its scope. */
	for (observer = window->mutation_observers; observer != NULL; observer = observer->next) {
		/* Disconnected and non-child-list observers do not match this mutation kind. */
		if (!observer->child_list || observer->target == NULL)
			continue;

		/* Match the registered target or its requested subtree without invoking script. */
		target = bind_node_of(vm_value_cell(observer->target));
		matches = 0;
		if (target == parent)
			matches = 1;

		/* A subtree registration also observes descendant parents. */
		if (!matches && observer->subtree)
			matches = dom_is_inclusive_ancestor(target, parent);

		/* Unrelated targets have no record for this operation. */
		if (!matches)
			continue;

		/* Build independent complete snapshots before publishing to this observer. */
		status = environment_mutation_record(window, observer, parent, added, added_count, removed, removed_count, previous, next);
		if (status != 0)
			goto cleanup;
	}

	/* Every matching observer has accepted the record, including the no-match case. */
	status = 0;

cleanup:
	/* Native node ownership lasts through the final record-publication attempt. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(heap, &roots[registered]);
	}

	/* Stable native storage leaves only after every heap slot has been unregistered. */
	free(roots);

	/* Allocation or publication failure retains the precise error for the caller. */
	if (status != 0)
		return status;

	/* Succeeded: every interested observer owns an independent complete record. */
	return 0;
}

/* Queues callbacks for MutationObservers that have accumulated records. */
int
bind_environment_checkpoint(
	struct bind_window *window,
	int *queued)
{
	struct environment_mutation_observer *observer;
	struct vm_cell *records;
	int status;

	/* Each pending array becomes one callback job. */
	*queued = 0;
	for (observer = window->mutation_observers; observer != NULL; observer = observer->next) {
		if (observer->records == NULL)
			continue;
		records = observer->records;
		status = vm_enqueue_job(window->realm, vm_value_cell(observer->callback), vm_value_cell(records));
		if (status != 0)
			return status;
		vm_heap_remove_root(window->realm->heap, &observer->records);
		observer->records = NULL;
		*queued = 1;
	}

	/* All pending arrays are now jobs. */
	return 0;
}

/* Appends a completely constructed childList record while retaining every native participant. */
static int
environment_mutation_record(
	struct bind_window *window,
	struct environment_mutation_observer *observer,
	struct dom_node *parent,
	struct dom_node *const *added,
	size_t added_count,
	struct dom_node *const *removed,
	size_t removed_count,
	struct dom_node *previous,
	struct dom_node *next)
{
	struct vm_realm *realm;
	struct vm_object *array;
	struct vm_object *record;
	struct vm_cell *roots[7];
	vm_value added_nodes;
	vm_value removed_nodes;
	vm_value target;
	vm_value type;
	vm_value records;
	vm_value sibling;
	unsigned index;
	unsigned registered;
	int status;

	/* Retain inputs before even the pending-array allocation can trigger collection. */
	realm = window->realm;
	memset(roots, 0, sizeof(roots));
	roots[0] = &parent->cell;
	/* Empty newborn slots acquire objects immediately after each successful allocation. */
	registered = 0;
	status = 0;
	for (index = 0; index < 7U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			break;
		registered++;
	}

	/* Every construction failure follows the same bounded root-release path. */
	if (status == 0) {
		do {
			/* The pending array retains the existing primary or managed observer ownership. */
			if (observer->records == NULL) {
				status = js_builtin_array(realm, NULL, 0, &records);
				if (status != 0)
					break;
				observer->records = vm_value_as_cell(records);
				status = environment_root(window, &observer->records);
				if (status != 0) {
					observer->records = NULL;
					break;
				}
			}

			/* The rooted native parent retains the target wrapper produced here. */
			status = bind_wrap(window, parent, &target);
			if (status != 0)
				break;

			/* Retain the added list before construction of the independent removed list. */
			status = environment_node_list(window, added, added_count, &added_nodes);
			if (status != 0)
				break;
			roots[3] = vm_value_as_cell(added_nodes);

			/* Retain the removed list before the type string and record allocations. */
			status = environment_node_list(window, removed, removed_count, &removed_nodes);
			if (status != 0)
				break;
			roots[4] = vm_value_as_cell(removed_nodes);

			/* Keep the type string alive while the record object is allocated. */
			status = bind_string(realm, "childList", &type);
			if (status != 0)
				break;
			roots[5] = vm_value_as_cell(type);

			/* The newborn record remains unpublished through all fallible property definitions. */
			record = vm_object_create(realm->heap, realm->object_prototype);
			if (record == NULL) {
				status = ENOMEM;
				break;
			}

			/* Property-key allocation may collect before the complete record reaches the array. */
			roots[6] = &record->cell;

			/* Describe the existing child-list mutation kind. */
			status = js_builtin_value(realm, record, "type", type, VM_PROPERTY_DEFAULT);
			if (status != 0)
				break;

			/* Retain the actual changed parent wrapper. */
			status = js_builtin_value(realm, record, "target", target, VM_PROPERTY_DEFAULT);
			if (status != 0)
				break;

			/* Publish the independent completed added-node sequence. */
			status = js_builtin_value(realm, record, "addedNodes", added_nodes, VM_PROPERTY_DEFAULT);
			if (status != 0)
				break;

			/* Publish the independent completed removed-node sequence. */
			status = js_builtin_value(realm, record, "removedNodes", removed_nodes, VM_PROPERTY_DEFAULT);
			if (status != 0)
				break;

			/* Preserve the actual boundary captured before source removal. */
			status = bind_wrap_or_null(window, previous, &sibling);
			if (status != 0)
				break;

			/* The source boundary node retains its wrapper through property publication. */
			status = js_builtin_value(realm, record, "previousSibling", sibling, VM_PROPERTY_DEFAULT);
			if (status != 0)
				break;

			/* The following boundary is independent of the previous sibling. */
			status = bind_wrap_or_null(window, next, &sibling);
			if (status != 0)
				break;

			/* Publish the complete following boundary on the record. */
			status = js_builtin_value(realm, record, "nextSibling", sibling, VM_PROPERTY_DEFAULT);
			if (status != 0)
				break;

			/* Child-list records carry no attribute name. */
			status = js_builtin_value(realm, record, "attributeName", VM_VALUE_NULL, VM_PROPERTY_DEFAULT);
			if (status != 0)
				break;

			/* Child-list records carry no attribute namespace. */
			status = js_builtin_value(realm, record, "attributeNamespace", VM_VALUE_NULL, VM_PROPERTY_DEFAULT);
			if (status != 0)
				break;

			/* Child-list records carry no previous character or attribute value. */
			status = js_builtin_value(realm, record, "oldValue", VM_VALUE_NULL, VM_PROPERTY_DEFAULT);
			if (status != 0)
				break;

			/* Only the complete record is appended to the observer-owned array. */
			array = (struct vm_object *)observer->records;
			status = vm_object_define(
			    realm->heap,
			    array,
			    vm_value_int32((int32_t)array->length),
			    vm_value_cell(record),
			    VM_PROPERTY_DEFAULT);
		} while (0);
	}

	/* Temporary ownership ends whether construction or final publication failed or succeeded. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Preserve the precise allocation or property-publication failure. */
	if (status != 0)
		return status;

	/* Succeeded: the observer owns the complete record and its independent lists. */
	return 0;
}

/* Makes a complete static NodeList-shaped sequence under explicit temporary ownership. */
static int
environment_node_list(
	struct bind_window *window,
	struct dom_node *const *nodes,
	size_t count,
	vm_value *result)
{
	struct vm_realm *realm;
	struct vm_object *constructor;
	struct vm_object *list;
	struct vm_cell *roots[4];
	vm_value sequence;
	vm_value name;
	size_t position;
	unsigned index;
	unsigned registered;
	int status;

	/* Native ownership retains a newly wrapped node without relying on caller stack scanning. */
	realm = window->realm;
	memset(roots, 0, sizeof(roots));

	/* Empty slots retain every newborn before the next VM allocation. */
	registered = 0;
	status = 0;
	for (index = 0; index < 4U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			break;
		registered++;
	}

	/* List publication is delayed until every constructor decoration succeeds. */
	if (status == 0) {
		do {
			/* Start with an empty sequence while the caller retains all input native nodes. */
			status = js_builtin_array(realm, NULL, 0, &sequence);
			if (status != 0)
				break;

			/* Retain the sequence before constructing any node wrapper. */
			list = (struct vm_object *)vm_value_as_cell(sequence);
			roots[1] = &list->cell;

			/* Snapshot every native node in order without exposing a partially constructed list. */
			for (position = 0; position < count; position++) {
				status = bind_array_push_node(window, list, nodes[position]);
				if (status != 0)
					break;
			}

			/* A failed wrapper or array property leaves the collection unpublished. */
			if (status != 0)
				break;

			/* The constructor object is protected before its name is allocated. */
			constructor = vm_object_create(realm->heap, realm->object_prototype);
			if (constructor == NULL) {
				status = ENOMEM;
				break;
			}

			/* A fallible name or property-key allocation cannot reclaim either collection object. */
			roots[2] = &constructor->cell;
			status = bind_string(realm, "NodeList", &name);
			if (status != 0)
				break;
			roots[3] = vm_value_as_cell(name);

			/* The existing observer collection exposes its constructor's name. */
			status = js_builtin_value(realm, constructor, "name", name, VM_PROPERTY_DEFAULT);
			if (status != 0)
				break;

			/* Attach the complete constructor before publishing the decorated list. */
			status = js_builtin_value(realm, list, "constructor", vm_value_cell(constructor), VM_PROPERTY_DEFAULT);
			if (status != 0)
				break;
			*result = sequence;
		} while (0);
	}

	/* Release every slot actually registered, including allocation-failure paths. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Report why no complete collection could be published. */
	if (status != 0)
		return status;

	/* Succeeded: the caller receives one fully decorated independent collection. */
	return 0;
}

/* Makes the initial IntersectionObserver or ResizeObserver entry for a target. */
static int
environment_observer_entry(
	struct bind_window *window,
	vm_value target,
	int type,
	vm_value *entry)
{
	struct vm_realm *realm;
	struct vm_object *record;
	struct dom_node *node;
	struct bind_box box;
	vm_value rect;
	vm_value root;
	vm_value sizes;
	double scroll_x;
	double scroll_y;
	double x;
	double y;
	int visible;
	int found;
	int status;

	/* Observers only accept elements. */
	realm = window->realm;
	node = bind_node_of(target);
	if (node == NULL || node->type != DOM_ELEMENT)
		return vm_throw_type_error(realm, "The observation target is not an Element.");

	/* Its border box in viewport coordinates. */
	memset(&box, 0, sizeof(box));
	found = 0;
	if (window->host.node_box != NULL)
		found = window->host.node_box(window->host.context, node, &box);
	scroll_x = 0.0;
	scroll_y = 0.0;
	if (window->host.scroll != NULL)
		window->host.scroll(window->host.context, &scroll_x, &scroll_y);
	x = box.x - scroll_x;
	y = box.y - scroll_y;
	status = environment_observer_rect(realm, x, y, box.width, box.height, &rect);
	if (status != 0)
		return status;

	/* Every record names the observed element. */
	record = vm_object_create(realm->heap, realm->object_prototype);
	if (record == NULL)
		return ENOMEM;
	status = js_builtin_value(realm, record, "target", target, VM_PROPERTY_DEFAULT);
	if (status != 0)
		return status;

	/* Intersection records include the viewport root and the visible state. */
	if (type == 1) {
		visible = found && box.width > 0.0 && box.height > 0.0 && x < window->viewport_width && y < window->viewport_height &&
		    x + box.width > 0.0 && y + box.height > 0.0;
		status = environment_observer_rect(realm, 0.0, 0.0, window->viewport_width, window->viewport_height, &root);
		if (status == 0)
			status = js_builtin_value(realm, record, "time", vm_value_number(window->now), VM_PROPERTY_DEFAULT);
		if (status == 0)
			status = js_builtin_value(realm, record, "rootBounds", root, VM_PROPERTY_DEFAULT);
		if (status == 0)
			status = js_builtin_value(realm, record, "boundingClientRect", rect, VM_PROPERTY_DEFAULT);
		if (status == 0)
			status = js_builtin_value(realm, record, "intersectionRect", rect, VM_PROPERTY_DEFAULT);
		if (status == 0)
			status = js_builtin_value(realm, record, "isIntersecting", vm_value_boolean(visible), VM_PROPERTY_DEFAULT);
		if (status == 0)
			status = js_builtin_value(realm, record, "intersectionRatio", vm_value_int32(visible), VM_PROPERTY_DEFAULT);
	}

	/* Resize records include the content rectangle and all three size arrays. */
	if (type == 2) {
		status = js_builtin_value(realm, record, "contentRect", rect, VM_PROPERTY_DEFAULT);
		if (status == 0)
			status = environment_observer_size(realm, box.width, box.height, &sizes);
		if (status == 0)
			status = js_builtin_value(realm, record, "contentBoxSize", sizes, VM_PROPERTY_DEFAULT);
		if (status == 0)
			status = js_builtin_value(realm, record, "borderBoxSize", sizes, VM_PROPERTY_DEFAULT);
		if (status == 0)
			status = js_builtin_value(realm, record, "devicePixelContentBoxSize", sizes, VM_PROPERTY_DEFAULT);
	}

	/* An allocation or property failure stops delivery. */
	if (status != 0)
		return status;

	/* Succeeded: the microtask owns the record through its array. */
	*entry = vm_value_cell(record);
	return 0;
}

/* Makes a DOMRect-shaped plain object. */
static int
environment_observer_rect(
	struct vm_realm *realm,
	double x,
	double y,
	double width,
	double height,
	vm_value *result)
{
	struct vm_object *rect;
	int status;

	/* Each DOMRect field is an ordinary number. */
	rect = vm_object_create(realm->heap, realm->object_prototype);
	if (rect == NULL)
		return ENOMEM;
	status = js_builtin_value(realm, rect, "x", vm_value_number(x), VM_PROPERTY_DEFAULT);
	if (status == 0)
		status = js_builtin_value(realm, rect, "y", vm_value_number(y), VM_PROPERTY_DEFAULT);
	if (status == 0)
		status = js_builtin_value(realm, rect, "width", vm_value_number(width), VM_PROPERTY_DEFAULT);
	if (status == 0)
		status = js_builtin_value(realm, rect, "height", vm_value_number(height), VM_PROPERTY_DEFAULT);
	if (status == 0)
		status = js_builtin_value(realm, rect, "top", vm_value_number(y), VM_PROPERTY_DEFAULT);
	if (status == 0)
		status = js_builtin_value(realm, rect, "right", vm_value_number(x + width), VM_PROPERTY_DEFAULT);
	if (status == 0)
		status = js_builtin_value(realm, rect, "bottom", vm_value_number(y + height), VM_PROPERTY_DEFAULT);
	if (status == 0)
		status = js_builtin_value(realm, rect, "left", vm_value_number(x), VM_PROPERTY_DEFAULT);
	if (status != 0)
		return status;
	*result = vm_value_cell(rect);
	return 0;
}

/* Makes the one-item ResizeObserverSize sequence. */
static int
environment_observer_size(
	struct vm_realm *realm,
	double width,
	double height,
	vm_value *result)
{
	struct vm_object *size;
	vm_value item;
	int status;

	/* One size record is enough for a single CSS box. */
	size = vm_object_create(realm->heap, realm->object_prototype);
	if (size == NULL)
		return ENOMEM;
	status = js_builtin_value(realm, size, "inlineSize", vm_value_number(width), VM_PROPERTY_DEFAULT);
	if (status == 0)
		status = js_builtin_value(realm, size, "blockSize", vm_value_number(height), VM_PROPERTY_DEFAULT);
	if (status != 0)
		return status;
	item = vm_value_cell(size);
	return js_builtin_array(realm, &item, 1, result);
}

/* Reports a Base64 alphabet character's six-bit value, or -1. */
static int
environment_base64_value(
	uint16_t unit)
{
	if (unit >= 'A' && unit <= 'Z')
		return unit - 'A';
	if (unit >= 'a' && unit <= 'z')
		return unit - 'a' + 26;
	if (unit >= '0' && unit <= '9')
		return unit - '0' + 52;
	if (unit == '+')
		return 62;
	if (unit == '/')
		return 63;
	return -1;
}

/* Reports outerWidth or outerHeight (the function's data: 0 for the width, 1 for the height). */
static int
environment_outer_size(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	/* The window's outer size is the screen's in this pass. */
	error = environment_screen_size(realm, this_value, args, count, result);
	if (error != 0)
		return error;

	/* Succeeded: the size is reported. */
	return 0;
}

/* Retains a registered observer's callback, real pending records and actual relevant timeline. */
static void
environment_performance_observer_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct environment_performance_observer *observer;
	vm_value *records;
	size_t index;

	/* A saved observer owns its callback and relevant global even after a frame retires. */
	observer = (struct environment_performance_observer *)cell;
	vm_heap_mark(heap, &observer->timeline->cell);
	vm_heap_mark_value(heap, observer->callback);

	/* The callback's receiver is the genuine wrapper, never a reconstructed lookalike. */
	if (observer->wrapper != NULL)
		vm_heap_mark(heap, &observer->wrapper->cell);

	/* Pending marks retain their independently managed entry payloads. */
	records = observer->records.items;
	for (index = 0; index < observer->records.count; index++) {
		vm_heap_mark_value(heap, records[index]);
	}

	/* Succeeded: the observer graph remains owned through real VM references. */
	return;
}

/* Releases only native vector storage when an observer's managed state becomes unreachable. */
static void
environment_performance_observer_finalize(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct environment_performance_observer *observer;

	UNUSED_PARAMETER(heap);

	/* Finalization never dereferences another possibly finalized owner object. */
	observer = (struct environment_performance_observer *)cell;
	wb_vector_release(&observer->records);

	/* Succeeded: no native pending-record allocation survives the dead observer. */
	return;
}

/* Retains the immutable entry references owned by a delivered native EntryList. */
static void
environment_performance_list_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct environment_performance_list *list;
	vm_value *entries;
	size_t index;

	/* Saved lists remain readable after callbacks drain or disconnect their source observer. */
	list = (struct environment_performance_list *)cell;
	entries = list->entries.items;
	for (index = 0; index < list->entries.count; index++) {
		vm_heap_mark_value(heap, entries[index]);
	}

	/* Succeeded: every actual entry in this independent snapshot remains reachable. */
	return;
}

/* Releases an EntryList's detached native vector without consulting its former observer. */
static void
environment_performance_list_finalize(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct environment_performance_list *list;

	UNUSED_PARAMETER(heap);

	/* Snapshot ownership ends independently of Window and observer finalization order. */
	list = (struct environment_performance_list *)cell;
	wb_vector_release(&list->entries);

	/* Succeeded: the dead snapshot owns no native storage. */
	return;
}

/* Validates the native observer brand before reading any private state. */
static int
environment_performance_observer_of(
	struct vm_realm *realm,
	vm_value receiver,
	struct environment_performance_observer **observer)
{
	struct vm_object *object;
	struct vm_cell *cell;
	int is_object;
	int is_cell;
	int status;

	/* A mutable prototype cannot turn an ordinary value into a native observer. */
	is_object = vm_value_is_object(receiver);
	if (!is_object) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* The wrapper brand is distinct from entries, timelines and delivered lists. */
	object = (struct vm_object *)vm_value_as_cell(receiver);
	if (object->native_operations != &environment_performance_observer_native) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Only a managed payload can own pending records. */
	is_cell = vm_value_is_cell(object->internal);
	if (!is_cell) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* A foreign payload cannot be interpreted using the observer representation. */
	cell = vm_value_as_cell(object->internal);
	if (cell->type != &environment_performance_observer_type) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Succeeded: the receiver owns the actual native observer state. */
	*observer = (struct environment_performance_observer *)cell;
	return 0;
}

/* Requires a genuine delivered EntryList before exposing its independent buffer. */
static int
environment_performance_list_of(
	struct vm_realm *realm,
	vm_value receiver,
	struct environment_performance_list **list)
{
	struct vm_object *object;
	struct vm_cell *cell;
	int is_object;
	int is_cell;
	int status;

	/* EntryList methods never accept an arbitrary array or prototype lookalike. */
	is_object = vm_value_is_object(receiver);
	if (!is_object) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Native operations identify the real list wrapper. */
	object = (struct vm_object *)vm_value_as_cell(receiver);
	if (object->native_operations != &environment_performance_list_native) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* The snapshot's storage must be an independently managed cell. */
	is_cell = vm_value_is_cell(object->internal);
	if (!is_cell) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Distinct managed types prevent accidental cross-interpretation of native payloads. */
	cell = vm_value_as_cell(object->internal);
	if (cell->type != &environment_performance_list_type) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Succeeded: the caller borrows only this receiver's real snapshot. */
	*list = (struct environment_performance_list *)cell;
	return 0;
}

/* Constructs an observer bound to its constructor's private relevant timeline. */
static int
environment_performance_observer_construct(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_function *callee;
	struct environment_timeline *timeline;
	struct environment_performance_observer *observer;
	struct vm_object *prototype;
	struct vm_object *object;
	struct vm_cell *roots[4];
	vm_value callback;
	unsigned index;
	unsigned registered;
	int callable;
	int status;

	UNUSED_PARAMETER(receiver);

	/* WebIDL callback conversion rejects both missing and non-callable arguments. */
	*result = VM_VALUE_UNDEFINED;
	callback = js_argument(args, count, 0);
	callable = vm_value_is_callable(callback);
	if (!callable) {
		status = vm_throw_type_error(realm, "A callable PerformanceObserver callback is required");
		return status;
	}

	/* A mutable global performance property never supplies the constructor's owner. */
	callee = js_builtin_callee(realm);
	timeline = (struct environment_timeline *)vm_value_as_cell(callee->data);
	registered = 0U;
	for (index = 0; index < 4U; index++) {
		roots[index] = NULL;
	}

	/* Protect the custom prototype, managed payload and wrapper before allocations or getters. */
	for (index = 0; index < 4U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;

		/* Unwind only successfully registered native root slots. */
		registered++;
	}

	/* The callback can be a newborn function passed directly to the constructor. */
	roots[0] = vm_value_as_cell(callback);

	/* Native constructors preserve an explicit subclass new.target prototype. */
	prototype = bind_window_of(realm)->prototypes[BIND_PERFORMANCE_OBSERVER];
	status = vm_construct_prototype(realm, realm->new_target, prototype, &prototype);
	if (status != 0)
		goto cleanup;

	/* The prototype getter may have returned a newborn object. */
	roots[1] = &prototype->cell;
	observer = vm_heap_alloc(realm->heap, &environment_performance_observer_type, sizeof(*observer));
	if (observer == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Complete the traceable state before constructing another VM object. */
	observer->timeline = timeline;
	observer->callback = callback;
	observer->wrapper = NULL;
	observer->mode = 0;
	observer->active = 0;
	observer->dropped_required = 0;
	wb_vector_init(&observer->records, sizeof(vm_value));
	roots[2] = &observer->cell;

	/* Construct a genuine extensible platform wrapper with no public substitute state. */
	object = vm_object_create(realm->heap, prototype);
	if (object == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* The callback receives this exact identity, including when constructed by a subclass. */
	roots[3] = &object->cell;
	object->kind = VM_KIND_PLATFORM;
	object->internal = vm_value_cell(observer);
	object->native_operations = &environment_performance_observer_native;
	observer->wrapper = object;
	*result = vm_value_cell(object);
	status = 0;

cleanup:
	/* End temporary ownership only after a complete wrapper is published or construction fails. */
	while (registered > 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* A failed prototype or allocation step cannot masquerade as successful construction. */
	if (status != 0)
		return status;

	/* Succeeded: the observer exists independently of whether observe is ever called. */
	return 0;
}

/* Converts the entire WebIDL sequence while retaining every collectible iterator input. */
static int
environment_performance_entry_types(
	struct vm_realm *realm,
	vm_value sequence,
	int *has_mark)
{
	struct vm_cell *roots[3];
	struct vm_string *name;
	vm_value iterator;
	vm_value value;
	unsigned index;
	unsigned registered;
	int is_object;
	int is_cell;
	int done;
	int equal;
	int status;

	/* WebIDL sequences require an object, even though primitive JS strings are iterable. */
	*has_mark = 0;
	is_object = vm_value_is_object(sequence);
	if (!is_object) {
		status = vm_throw_type_error(realm, "entryTypes must be an iterable object");
		return status;
	}

	/* A getter may have produced a sequence with no other owning reference. */
	roots[0] = vm_value_as_cell(sequence);
	roots[1] = NULL;
	roots[2] = NULL;
	registered = 0U;
	for (index = 0; index < 3U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;

		/* Cleanup releases only slots that the heap actually registered. */
		registered++;
	}

	/* Use the engine's real iterator protocol rather than treating arrays as the only sequence. */
	status = vm_iter_start(realm, sequence, &iterator);
	if (status != 0)
		goto cleanup;

	/* Iterator state survives next methods and each element's string conversion. */
	roots[1] = vm_value_as_cell(iterator);
	for (;;) {
		status = vm_iter_next(realm, iterator, &value, &done);
		if (status != 0)
			goto cleanup;

		/* Exhaustion ends conversion without invoking an iterator return method. */
		if (done)
			break;

		/* Preserve a newborn element while ToString invokes arbitrary getters or calls. */
		roots[2] = NULL;
		is_cell = vm_value_is_cell(value);
		if (is_cell)
			roots[2] = vm_value_as_cell(value);

		/* Every element is converted, even after the supported mark type was already found. */
		status = vm_to_string(realm, value, &name);
		if (status != 0) {
			/* WebIDL sequence conversion propagates this abrupt completion without IteratorClose. */
			goto cleanup;
		}

		/* Unsupported types are filtered without being advertised or silently manufactured. */
		equal = vm_string_equal_ascii(name, "mark");
		if (equal)
			*has_mark = 1;
	}

	/* Every iterator element and side effect completed successfully. */
	status = 0;

cleanup:
	/* Sequence, iterator and element temporary ownership ends on every path. */
	while (registered > 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* A partial conversion cannot register a partially observed type set. */
	if (status != 0)
		return status;

	/* Succeeded: the complete sequence determines whether real marks are supported. */
	return 0;
}

/* Converts options, checks the sticky registration mode and registers actual mark production. */
static int
environment_performance_observe(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct environment_performance_observer *observer;
	struct environment_timeline *timeline;
	struct vm_cell *roots[2];
	struct vm_string *type;
	vm_value options;
	vm_value value;
	vm_value *entries;
	unsigned registered;
	unsigned index;
	size_t position;
	int present;
	int types_present;
	int type_present;
	int is_object;
	int is_cell;
	int buffered;
	int has_mark;
	int supported;
	int mode;
	int status;

	/* Receiver validation precedes any user option getters. */
	*result = VM_VALUE_UNDEFINED;
	observer = NULL;
	status = environment_performance_observer_of(realm, receiver, &observer);
	if (status != 0)
		return status;

	/* Null and undefined become empty dictionaries; other primitives are rejected. */
	options = js_argument(args, count, 0);
	if (options != VM_VALUE_UNDEFINED && options != VM_VALUE_NULL) {
		is_object = vm_value_is_object(options);
		if (!is_object) {
			status = vm_throw_type_error(realm, "Observer options must be a dictionary");
			return status;
		}
	}

	/* Temporary sequence and type getter results survive later dictionary conversions. */
	roots[0] = NULL;
	roots[1] = NULL;
	registered = 0U;
	for (index = 0; index < 2U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;

		/* Only registered slots belong to the final unwind. */
		registered++;
	}

	/* WebIDL dictionary conversion reads buffered before entryTypes and type. */
	status = bind_get_option(realm, options, "buffered", &present, &value);
	if (status != 0)
		goto cleanup;

	/* Boolean conversion does not call user code. */
	buffered = vm_to_boolean(value);
	status = bind_get_option(realm, options, "entryTypes", &types_present, &value);
	if (status != 0)
		goto cleanup;

	/* An iterable object's complete element conversion precedes reading the type member. */
	has_mark = 0;
	if (types_present) {
		is_cell = vm_value_is_cell(value);
		if (is_cell)
			roots[0] = vm_value_as_cell(value);

		/* Use the native sequence converter, including custom iterators and abrupt completion. */
		status = environment_performance_entry_types(realm, value, &has_mark);
		if (status != 0)
			goto cleanup;
	}

	/* The optional single-type DOMString is converted after the entire sequence. */
	status = bind_get_option(realm, options, "type", &type_present, &value);
	if (status != 0)
		goto cleanup;

	/* Only a real mark producer can be selected by a single-type registration. */
	supported = 0;
	if (type_present) {
		is_cell = vm_value_is_cell(value);
		if (is_cell)
			roots[1] = vm_value_as_cell(value);

		/* Type getters may return a newborn object whose conversion runs script. */
		status = vm_to_string(realm, value, &type);
		if (status != 0)
			goto cleanup;

		/* No later user conversion borrows the returned string. */
		supported = vm_string_equal_ascii(type, "mark");
	}

	/* One registration cannot combine the multiple and single-type forms. */
	if (types_present && type_present) {
		status = vm_throw_type_error(realm, "entryTypes and type cannot be combined");
		goto cleanup;
	}

	/* A dictionary with neither form does not select an observer registration. */
	if (!types_present && !type_present) {
		status = vm_throw_type_error(realm, "entryTypes or type is required");
		goto cleanup;
	}

	/* The mode persists across disconnect and unsupported-type attempts. */
	mode = 2;
	if (types_present)
		mode = 1;

	/* Reentrant option conversions are complete before checking the current mode. */
	if (observer->mode != 0 && observer->mode != mode) {
		status = bind_throw_dom(realm, "InvalidModificationError", "An observer cannot switch registration mode");
		goto cleanup;
	}

	/* Refresh dropped-entry metadata even when unsupported types abort the new registration. */
	observer->mode = mode;
	observer->dropped_required = 1;
	if (types_present)
		supported = has_mark;

	/* An empty supported set leaves an existing registration unchanged. */
	if (!supported) {
		status = 0;
		goto cleanup;
	}

	/* Publish the actual observer identity only after all dictionary and mode checks succeeded. */
	timeline = observer->timeline;
	if (!observer->active) {
		value = vm_value_cell(observer->wrapper);
		status = wb_vector_push(&timeline->observers, &value);
		if (status != 0)
			goto cleanup;

		/* The relevant timeline now retains this active observer and its callback. */
		observer->active = 1;
	}

	/* Fixed-target and Chrome compatibility ignores buffered with entryTypes for every caller. */
	if (mode == 2 && buffered) {
		entries = timeline->entries.items;
		for (position = 0; position < timeline->entries.count; position++) {
			status = wb_vector_push(&observer->records, &entries[position]);
			if (status != 0)
				goto cleanup;
		}

		/* Existing mark records become asynchronous notifications through the same actual task source. */
		status = environment_performance_schedule(timeline);
		if (status != 0)
			goto cleanup;
	}

	/* Registration with no buffered delivery still succeeds without a synchronous callback. */
	status = 0;

cleanup:
	/* Converted dictionary temporaries never become permanent observer state. */
	while (registered > 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Conversion, mode or allocation errors remain visible to the original caller. */
	if (status != 0)
		return status;

	/* Succeeded: only genuine mark registrations and requested replay records were published. */
	return 0;
}

/* Removes a registration and pending records without changing its sticky observation mode. */
static int
environment_performance_disconnect(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct environment_performance_observer *observer;
	struct environment_timeline *timeline;
	struct vm_object *object;
	vm_value *items;
	size_t input;
	size_t output;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Disconnect accepts only an actual native observer receiver. */
	*result = VM_VALUE_UNDEFINED;
	observer = NULL;
	status = environment_performance_observer_of(realm, receiver, &observer);
	if (status != 0)
		return status;

	/* Compact registration order without allocating or calling script. */
	timeline = observer->timeline;
	items = timeline->observers.items;
	output = 0U;
	for (input = 0; input < timeline->observers.count; input++) {
		object = (struct vm_object *)vm_value_as_cell(items[input]);

		/* The exact native wrapper is unregistered rather than a prototype-related value. */
		if (object == observer->wrapper)
			continue;

		/* Other observers retain their existing dispatch order. */
		items[output] = items[input];
		output++;
	}

	/* Pending task dispatch may remain queued, but it no longer retains this registration. */
	timeline->observers.count = output;
	observer->active = 0;
	wb_vector_clear(&observer->records);

	/* Succeeded: future marks no longer queue records for this observer. */
	return 0;
}

/* Queues one private, uncancellable task in the actual owner's ordinary Window task backend. */
static int
environment_performance_schedule(
	struct environment_timeline *timeline)
{
	struct vm_realm *realm;
	struct bind_window *window;
	struct bind_timer task;
	int status;

	/* The retained real global supplies the owner even when callers borrow another realm's methods. */
	realm = (struct vm_realm *)vm_value_as_cell(timeline->owner_global->internal);
	window = bind_window_of(realm);
	if (window == NULL || window->detached)
		return 0;

	/* One queued task batches all current interested observers and mark records. */
	if (timeline->queued)
		return 0;

	/* Private identifier zero cannot be canceled by the public positive-id timer API. */
	memset(&task, 0, sizeof(task));
	task.id = 0U;
	task.due = window->now;
	task.sequence = window->next_sequence;
	task.callback = timeline->dispatch;
	task.arguments = VM_VALUE_UNDEFINED;

	/* The ordinary timer tracer and retirement path retain and cancel the actual callback. */
	status = wb_vector_push(&window->timers, &task);
	if (status != 0)
		return status;

	/* Successful publication consumes only the internal task generation, never a public timer id. */
	window->next_sequence++;
	timeline->queued = 1;

	/* Succeeded: delivery waits for the next ordinary Window task round. */
	return 0;
}

/* Appends a real mark to every active interested observer before scheduling asynchronous delivery. */
static int
environment_performance_queue_mark(
	struct environment_timeline *timeline,
	vm_value mark)
{
	struct environment_performance_observer *observer;
	struct vm_object *object;
	vm_value *items;
	size_t index;
	int status;

	/* Registration retains only observers that actually selected the supported mark producer. */
	items = timeline->observers.items;
	for (index = 0; index < timeline->observers.count; index++) {
		object = (struct vm_object *)vm_value_as_cell(items[index]);
		observer = (struct environment_performance_observer *)vm_value_as_cell(object->internal);
		status = wb_vector_push(&observer->records, &mark);
		if (status != 0)
			return status;
	}

	/* The shared flag batches consecutive marks, including marks created before registration. */
	status = environment_performance_schedule(timeline);
	if (status != 0)
		return status;

	/* Succeeded: actual native entry identities wait in their observers' own queues. */
	return 0;
}

/* Sorts a private EntryList snapshot stably without changing the observer's raw takeRecords order. */
static void
environment_performance_list_sort(
	struct wb_vector *buffer)
{
	struct vm_object *object;
	struct environment_mark *mark;
	struct environment_mark *previous;
	vm_value *entries;
	vm_value entry;
	size_t index;
	size_t position;

	/* Only detached, already rooted mark references participate in this allocation-free sort. */
	entries = buffer->items;
	for (index = 1U; index < buffer->count; index++) {
		entry = entries[index];
		object = (struct vm_object *)vm_value_as_cell(entry);
		mark = (struct environment_mark *)vm_value_as_cell(object->internal);
		position = index;

		/* Equal timestamps preserve their original queued order. */
		while (position > 0U) {
			object = (struct vm_object *)vm_value_as_cell(entries[position - 1U]);
			previous = (struct environment_mark *)vm_value_as_cell(object->internal);
			if (previous->start <= mark->start)
				break;

			/* Moving references cannot collect or reconstruct the underlying entry objects. */
			entries[position] = entries[position - 1U];
			position--;
		}

		/* The genuine entry retains its identity at the stable sorted position. */
		entries[position] = entry;
	}

	/* Succeeded: all EntryList query snapshots can reuse chronological native storage. */
	return;
}

/* Detaches a pending queue into a genuine independently owned delivered EntryList. */
static int
environment_performance_list_create(
	struct vm_realm *realm,
	struct environment_timeline *timeline,
	struct wb_vector *records,
	vm_value *result)
{
	struct environment_performance_list *list;
	struct vm_object *object;
	struct vm_cell *roots[2];
	unsigned index;
	unsigned registered;
	int status;

	/* Register empty roots before allocating either the payload or wrapper. */
	*result = VM_VALUE_UNDEFINED;
	roots[0] = NULL;
	roots[1] = NULL;
	registered = 0U;
	for (index = 0; index < 2U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;

		/* Cleanup owns only acquired registry entries. */
		registered++;
	}

	/* Allocation failure leaves the observer's original queue untouched. */
	list = vm_heap_alloc(realm->heap, &environment_performance_list_type, sizeof(*list));
	if (list == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* An empty initialized payload is safe to trace while its wrapper is allocated. */
	wb_vector_init(&list->entries, sizeof(vm_value));
	roots[0] = &list->cell;
	object = vm_object_create(realm->heap, timeline->list_prototype);
	if (object == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Only the actual wrapper exposes the native EntryList brand and relevant prototype. */
	roots[1] = &object->cell;
	object->kind = VM_KIND_PLATFORM;
	object->internal = vm_value_cell(list);
	object->native_operations = &environment_performance_list_native;

	/* Move ownership atomically before a callback can append a new observer batch. */
	list->entries = *records;
	wb_vector_init(records, sizeof(vm_value));
	environment_performance_list_sort(&list->entries);
	*result = vm_value_cell(object);
	status = 0;

cleanup:
	/* Temporary roots end only after complete publication or a non-destructive allocation failure. */
	while (registered > 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Preserve a failed snapshot construction for the ordinary task error path. */
	if (status != 0)
		return status;

	/* Succeeded: later observer mutations cannot alter this delivered EntryList. */
	return 0;
}

/* Delivers a registration-order task snapshot while allowing real callback and microtask reentry. */
static int
environment_performance_dispatch(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct environment_timeline *timeline;
	struct environment_performance_observer *observer;
	struct vm_function *callee;
	struct bind_window *window;
	struct vm_object *notify;
	struct vm_object *options;
	struct vm_cell *roots[3];
	vm_value snapshot;
	vm_value list;
	vm_value wrapper;
	vm_value callback_args[3];
	vm_value returned;
	unsigned index;
	unsigned registered;
	uint32_t position;
	int status;

	UNUSED_PARAMETER(receiver);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The private callback data retains the exact relevant timeline and Window graph. */
	*result = VM_VALUE_UNDEFINED;
	callee = js_builtin_callee(realm);
	timeline = (struct environment_timeline *)vm_value_as_cell(callee->data);
	window = bind_window_of(realm);

	/* Clear the shared flag before callbacks can schedule a distinct next delivery task. */
	timeline->queued = 0;

	/* Retired contexts deliver no saved observer tasks. */
	if (window == NULL || window->detached)
		return 0;

	/* Snapshot construction and callback dictionaries need explicit temporary roots. */
	registered = 0U;
	for (index = 0; index < 3U; index++) {
		roots[index] = NULL;
	}

	/* No collector depends on this native task's conservative C stack. */
	for (index = 0; index < 3U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;

		/* Unwind only root slots acquired by this invocation. */
		registered++;
	}

	/* Later callbacks can reallocate or replace the original registration vector. */
	status = js_builtin_array(realm, timeline->observers.items, (unsigned)timeline->observers.count, &snapshot);
	if (status != 0)
		goto cleanup;

	/* A genuine private array retains every original observer throughout dispatch. */
	roots[0] = vm_value_as_cell(snapshot);
	notify = (struct vm_object *)roots[0];
	for (position = 0U; position < notify->length; position++) {
		/* An earlier callback or microtask can retire the actual owning frame. */
		if (window->detached)
			break;

		/* This snapshot contains only genuine wrappers published by native registration. */
		wrapper = notify->elements[position];
		observer = NULL;
		status = environment_performance_observer_of(realm, wrapper, &observer);
		if (status != 0)
			goto cleanup;

		/* Disconnected, drained or already delivered queues require no callback. */
		if (observer->records.count == 0U)
			continue;

		/* Detach only this observer's current records before invoking its callback. */
		status = environment_performance_list_create(realm, timeline, &observer->records, &list);
		if (status != 0)
			goto cleanup;

		/* A callback or one of its microtasks may collect while this exact list remains in use. */
		roots[1] = vm_value_as_cell(list);
		options = vm_object_create(realm->heap, realm->object_prototype);
		if (options == NULL) {
			status = ENOMEM;
			goto cleanup;
		}

		/* Only the first delivery after observe reports the real mark-only dropped count of zero. */
		roots[2] = &options->cell;
		if (observer->dropped_required) {
			status = js_builtin_value(realm, options, "droppedEntriesCount", vm_value_int32(0), VM_PROPERTY_DEFAULT);
			if (status != 0)
				goto cleanup;

			/* Reentrant observe during the callback can independently request future drop metadata. */
			observer->dropped_required = 0;
		}

		/* The callback receives the actual list, observer identity and ordinary options dictionary. */
		callback_args[0] = list;
		callback_args[1] = wrapper;
		callback_args[2] = vm_value_cell(options);
		status = vm_call(realm, observer->callback, wrapper, callback_args, 3U, &returned);
		if (status == VM_THROWN) {
			bind_report_exception(window, realm->exception);
			realm->exception = VM_VALUE_UNDEFINED;
			status = 0;
		}

		/* Allocation or interpreter failures cannot silently discard the remaining native task work. */
		if (status != 0)
			goto cleanup;

		/* WebIDL callback cleanup performs a microtask checkpoint before the next observer. */
		status = bind_checkpoint(window);
		if (status != 0)
			goto cleanup;

		/* Saved JS references retain any list or options that outlive this completed callback. */
		roots[1] = NULL;
		roots[2] = NULL;
	}

	/* Any reentrant mark already scheduled the next ordinary task using the cleared shared flag. */
	status = 0;

cleanup:
	/* Release every temporary even when callback execution or allocation stops dispatch. */
	while (registered > 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Preserve task failures for the existing Window timer reporting path. */
	if (status != 0)
		return status;

	/* Succeeded: registered observers received genuine asynchronous mark snapshots. */
	return 0;
}

/* Returns a copy of the observer's raw queue and drains it without changing entry identities or order. */
static int
environment_performance_take_records(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct environment_performance_observer *observer;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Only the actual observer may consume its pending records. */
	observer = NULL;
	status = environment_performance_observer_of(realm, receiver, &observer);
	if (status != 0)
		return status;

	/* The common array builder preserves this unsorted native queue's insertion order. */
	status = environment_entries_snapshot(realm, &observer->records, NULL, 0U, 0, result);
	if (status != 0)
		return status;

	/* Clearing native references cannot collect the completed returned array. */
	wb_vector_clear(&observer->records);

	/* Succeeded: drained records no longer produce a later callback by themselves. */
	return 0;
}

/* Validates the native EntryList receiver before sharing the real entry-array query implementation. */
static int
environment_performance_list_snapshot(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	int mode,
	vm_value *result)
{
	struct environment_performance_list *list;
	int status;

	/* EntryList methods cannot accept a performance object or a prototype lookalike. */
	list = NULL;
	status = environment_performance_list_of(realm, receiver, &list);
	if (status != 0)
		return status;

	/* Delivered storage is already stably sorted and independent of its former observer. */
	status = environment_entries_snapshot(realm, &list->entries, args, count, mode, result);
	if (status != 0)
		return status;

	/* Succeeded: the query returns a fresh real array of genuine native entries. */
	return 0;
}

/* Returns all entries in a delivered native snapshot in stable timestamp order. */
static int
environment_performance_list_entries(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The unfiltered list query shares the same native snapshot semantics as performance queries. */
	status = environment_performance_list_snapshot(realm, receiver, args, count, 0, result);
	if (status != 0)
		return status;

	/* Succeeded: every actual entry in this delivered list was selected. */
	return 0;
}

/* Returns delivered native entries matching one required DOMString type. */
static int
environment_performance_list_by_type(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* Type conversion and required-argument checks use the common real entry query. */
	status = environment_performance_list_snapshot(realm, receiver, args, count, 1, result);
	if (status != 0)
		return status;

	/* Succeeded: the delivered type-filtered snapshot is complete. */
	return 0;
}

/* Returns delivered native entries matching a required name and optional type. */
static int
environment_performance_list_by_name(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* Name and optional type conversion retain the existing reentrant-GC guarantees. */
	status = environment_performance_list_snapshot(realm, receiver, args, count, 2, result);
	if (status != 0)
		return status;

	/* Succeeded: the caller receives an independent name-filtered array. */
	return 0;
}

/* Returns the same genuinely frozen supported-type array retained by this relevant realm's native getter. */
static int
environment_performance_supported_types(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_function *callee;

	UNUSED_PARAMETER(receiver);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Native function data cannot be replaced by assigning a JS constructor property. */
	callee = js_builtin_callee(realm);
	*result = callee->data;

	/* Succeeded: only the implemented real mark producer is advertised. */
	return 0;
}

/* Applies the enumerable WebIDL descriptor to methods already installed from native interface tables. */
static int
environment_performance_enumerate(
	struct vm_realm *realm,
	struct vm_object *prototype,
	const struct bind_operation *operation)
{
	struct vm_property property;
	vm_value key;
	int found;
	int status;

	/* Preserve actual native method identities and their writable/configurable flags. */
	for (; operation->name != NULL; operation++) {
		key = vm_key_from_ascii(realm->heap, operation->name);
		if (key == VM_VALUE_EMPTY)
			return ENOMEM;

		/* The just-installed method must exist as an own prototype property. */
		found = vm_object_get_own(prototype, key, &property);
		if (found < 0)
			return -found;

		/* Missing native members make interface publication incomplete. */
		if (found == 0)
			return EINVAL;

		/* Enumerability is the only refined flag. */
		status = vm_object_define(realm->heap, prototype, key, *property.value, property.attributes | VM_PROPERTY_ENUMERABLE);
		if (status != 0)
			return status;
	}

	/* Succeeded: every native operation has its WebIDL prototype descriptor. */
	return 0;
}

/* Publishes real constructor ownership, a frozen supported array and a private asynchronous dispatcher. */
static int
environment_install_performance_observer(
	struct bind_window *window,
	struct environment_timeline *timeline)
{
	struct vm_realm *realm;
	struct vm_object *prototype;
	struct vm_object *array;
	struct vm_function *constructor;
	struct vm_function *function;
	struct vm_accessor *accessor;
	struct vm_property property;
	struct vm_cell *roots[3];
	vm_value key;
	vm_value value;
	unsigned index;
	unsigned registered;
	int found;
	int done;
	int status;

	/* Read the pristine native constructor through its private installed prototype, never a mutable global lookup. */
	realm = window->realm;
	prototype = window->prototypes[BIND_PERFORMANCE_OBSERVER];
	key = vm_key_from_ascii(realm->heap, "constructor");
	if (key == VM_VALUE_EMPTY)
		return ENOMEM;

	/* Window interface installation already published this exact real constructor. */
	found = vm_object_get_own(prototype, key, &property);
	if (found < 0)
		return -found;

	/* An absent native constructor cannot provide a working observer. */
	if (found == 0)
		return EINVAL;

	/* Constructor-native data binds future instances to their relevant timeline even after global replacement. */
	constructor = (struct vm_function *)vm_value_as_cell(*property.value);
	constructor->data = vm_value_cell(timeline);
	registered = 0U;
	for (index = 0; index < 3U; index++) {
		roots[index] = NULL;
	}

	/* Array, native getter and accessor construction retain every intermediate VM cell. */
	for (index = 0; index < 3U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;

		/* The unwind count tracks only roots accepted by the heap. */
		registered++;
	}

	/* A real array contains only entry types backed by an actual native producer and delivery path. */
	array = vm_array_create(realm->heap, realm->array_prototype);
	if (array == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Array ownership precedes allocating its sole supported-type string. */
	roots[0] = &array->cell;
	status = bind_string(realm, "mark", &value);
	if (status != 0)
		goto cleanup;

	/* Define an immutable enumerable own index without invoking inherited setters. */
	status = vm_object_define(realm->heap, array, vm_value_int32(0), value, VM_PROPERTY_ENUMERABLE);
	if (status != 0)
		goto cleanup;

	/* FrozenArray length is also immutable and nonconfigurable. */
	key = vm_key_from_ascii(realm->heap, "length");
	if (key == VM_VALUE_EMPTY) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Preserve the actual single-element array while making its native length readonly. */
	status = vm_object_define(realm->heap, array, key, vm_value_int32(1), 0U);
	if (status != 0)
		goto cleanup;

	/* Prevent all further own-property additions through the ordinary object implementation. */
	status = vm_object_prevent_extensions(array, &done);
	if (status != 0)
		goto cleanup;

	/* Refusal to freeze a native array is an installation failure, never a fake supported capability. */
	if (!done) {
		status = EINVAL;
		goto cleanup;
	}

	/* The stable readonly getter retains this real array through private function data. */
	status = js_builtin_function(realm, "get supportedEntryTypes", 0, environment_performance_supported_types, NULL, &function);
	if (status != 0)
		goto cleanup;

	/* Getter construction is complete before its accessor allocates. */
	roots[1] = &function->object.cell;
	function->data = vm_value_cell(array);
	accessor = vm_accessor_create(realm->heap, vm_value_cell(function), VM_VALUE_UNDEFINED);
	if (accessor == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* WebIDL static attributes are enumerable/configurable accessors with no setter. */
	roots[2] = &accessor->cell;
	status = js_builtin_value(realm, &constructor->object, "supportedEntryTypes", vm_value_cell(accessor), VM_PROPERTY_ACCESSOR | VM_PROPERTY_ENUMERABLE | VM_PROPERTY_CONFIGURABLE);
	if (status != 0)
		goto cleanup;

	/* The private task callback retains its actual timeline without becoming a user-visible timer handler. */
	status = js_builtin_function(realm, "notify performance observers", 0, environment_performance_dispatch, NULL, &function);
	if (status != 0)
		goto cleanup;

	/* Publish callback ownership before any subsequent prototype metadata allocation. */
	function->data = vm_value_cell(timeline);
	timeline->dispatch = vm_value_cell(function);
	status = environment_performance_enumerate(realm, prototype, environment_performance_observer_operations);
	if (status != 0)
		goto cleanup;

	/* Native list query methods expose the same standard enumerable operation descriptors. */
	prototype = window->prototypes[BIND_PERFORMANCE_OBSERVER_ENTRY_LIST];
	status = environment_performance_enumerate(realm, prototype, environment_performance_list_operations);
	if (status != 0)
		goto cleanup;

	/* Standard tags describe real privately branded observer and snapshot instances. */
	status = environment_install_entry_tag(realm, window->prototypes[BIND_PERFORMANCE_OBSERVER], "PerformanceObserver");
	if (status != 0)
		goto cleanup;

	/* A delivered list identifies its actual native interface. */
	status = environment_install_entry_tag(realm, prototype, "PerformanceObserverEntryList");
	if (status != 0)
		goto cleanup;

cleanup:
	/* Getter and array temporaries are now owned by the real constructor or discarded on failure. */
	while (registered > 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Failed metadata publication leaves Window initialization incomplete. */
	if (status != 0)
		return status;

	/* Succeeded: real native mark observers replace the unused Prelude fallback. */
	return 0;
}
