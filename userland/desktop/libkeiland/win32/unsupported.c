/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The functions of libkeiland that its Windows backend does not have yet:
 * a function returning int returns ENOTSUP, a pointer NULL, another value
 * 0, and a void function does nothing.  Generated from the headers by the
 * stubs' generator of the Windows build's tests; do not edit.
 */

#include <vulkan/vulkan.h>

#include <keiland/keiland.h>
#include "userland/desktop/libkeiland/process.h"
#include "userland/desktop/libkeiland/ui/chooser.h"
#include "userland/desktop/libkeiland/ui/picker.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

/* Not on Windows. */
void
keiui_chooser_accept(
	struct keiui_chooser *chooser)
{
	(void)chooser;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
void
keiui_chooser_accept_path(
	struct keiui_chooser *chooser)
{
	(void)chooser;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
void
keiui_chooser_activate(
	struct keiui_chooser *chooser,
	long index)
{
	(void)chooser;
	(void)index;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
int
keiui_chooser_can_accept(
	const struct keiui_chooser *chooser)
{
	(void)chooser;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
keiui_chooser_can_go_up(
	const struct keiui_chooser *chooser)
{
	(void)chooser;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
void
keiui_chooser_cancel(
	struct keiui_chooser *chooser)
{
	(void)chooser;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
void
keiui_chooser_close_path(
	struct keiui_chooser *chooser)
{
	(void)chooser;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
void
keiui_chooser_go_place(
	struct keiui_chooser *chooser,
	size_t place)
{
	(void)chooser;
	(void)place;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
void
keiui_chooser_go_up(
	struct keiui_chooser *chooser)
{
	(void)chooser;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
const char *
keiui_chooser_home(void)
{
	/* Not on Windows. */
	return NULL;
}

/* Not on Windows. */
int
keiui_chooser_is_place(
	const struct keiui_chooser *chooser,
	size_t place)
{
	(void)chooser;
	(void)place;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
void
keiui_chooser_keep(
	struct keiui_chooser *chooser)
{
	(void)chooser;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
void
keiui_chooser_next_filter(
	struct keiui_chooser *chooser)
{
	(void)chooser;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
void
keiui_chooser_open_path(
	struct keiui_chooser *chooser)
{
	(void)chooser;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
void
keiui_chooser_replace(
	struct keiui_chooser *chooser)
{
	(void)chooser;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
void
keiui_chooser_select(
	struct keiui_chooser *chooser,
	long index)
{
	(void)chooser;
	(void)index;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
void
keiui_chooser_toggle_hidden(
	struct keiui_chooser *chooser)
{
	(void)chooser;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
void
keiui_chooser_type_select(
	struct keiui_chooser *chooser,
	uint32_t character)
{
	(void)chooser;
	(void)character;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
int
keiui_launch_start(
	const struct kl_launch_options *options,
	const char *const *environment)
{
	(void)options;
	(void)environment;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
void
keiui_picker_accept(
	struct keiui_picker *picker)
{
	(void)picker;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
void
keiui_picker_cancel(
	struct keiui_picker *picker)
{
	(void)picker;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
void
keiui_picker_choose(
	struct keiui_picker *picker,
	size_t index)
{
	(void)picker;
	(void)index;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
int
keiui_picker_collect(
	struct keiui_picker *picker,
	size_t first,
	size_t last)
{
	(void)picker;
	(void)first;
	(void)last;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
void
keiui_picker_toggle(
	struct keiui_picker *picker,
	size_t index)
{
	(void)picker;
	(void)index;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
void
keiui_picker_want(
	struct keiui_picker *picker,
	size_t first,
	size_t last)
{
	(void)picker;
	(void)first;
	(void)last;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
int
kl_activate(
	struct wl_display *display,
	struct wl_surface *surface,
	const char *token)
{
	(void)display;
	(void)surface;
	(void)token;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_activation_token(
	struct wl_display *display,
	struct wl_surface *surface,
	const char *app_id,
	char *token,
	size_t size)
{
	(void)display;
	(void)surface;
	(void)app_id;
	(void)token;
	(void)size;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
struct wl_display *
kl_app_display(
	const struct kl_app *app)
{
	(void)app;

	/* Not on Windows. */
	return NULL;
}

/* Not on Windows. */
int
kl_app_notify(
	struct kl_app *app,
	const char *title,
	const char *body)
{
	(void)app;
	(void)title;
	(void)body;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_app_notify_lock(
	struct kl_app *app,
	const char *title,
	const char *body,
	const char *lock_text)
{
	(void)app;
	(void)title;
	(void)body;
	(void)lock_text;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
void
kl_appearance_close(
	struct kl_appearance *appearance)
{
	(void)appearance;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
int
kl_appearance_open(
	struct wl_display *display,
	kl_appearance_fn changed,
	void *data,
	struct kl_appearance **appearance)
{
	(void)display;
	(void)changed;
	(void)data;
	(void)appearance;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
unsigned
kl_audio_stream_capacity(
	const struct kl_audio_stream *stream)
{
	(void)stream;

	/* Not on Windows. */
	return 0;
}

/* Not on Windows. */
void
kl_audio_stream_close(
	struct kl_audio_stream *stream)
{
	(void)stream;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
uint64_t
kl_audio_stream_consumed(
	const struct kl_audio_stream *stream)
{
	(void)stream;

	/* Not on Windows. */
	return 0;
}

/* Not on Windows. */
int
kl_audio_stream_dispatch(
	struct kl_audio_stream *stream,
	unsigned *events)
{
	(void)stream;
	(void)events;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_audio_stream_drain(
	struct kl_audio_stream *stream)
{
	(void)stream;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_audio_stream_flush(
	struct kl_audio_stream *stream)
{
	(void)stream;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_audio_stream_open(
	const struct kl_audio_format *format,
	struct kl_audio_stream **stream)
{
	(void)format;
	(void)stream;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
uint64_t
kl_audio_stream_position(
	const struct kl_audio_stream *stream,
	int64_t *time_ns)
{
	(void)stream;
	(void)time_ns;

	/* Not on Windows. */
	return 0;
}

/* Not on Windows. */
int
kl_audio_stream_start(
	struct kl_audio_stream *stream)
{
	(void)stream;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_audio_stream_stop(
	struct kl_audio_stream *stream)
{
	(void)stream;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
size_t
kl_audio_stream_write(
	struct kl_audio_stream *stream,
	const void *frames,
	size_t count)
{
	(void)stream;
	(void)frames;
	(void)count;

	/* Not on Windows. */
	return 0;
}

/* Not on Windows. */
uint64_t
kl_audio_stream_written(
	const struct kl_audio_stream *stream)
{
	(void)stream;

	/* Not on Windows. */
	return 0;
}

/* Not on Windows. */
void
kl_context_menu_destroy(
	struct kl_context_menu *context_menu)
{
	(void)context_menu;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
void
kl_desktop_ack(
	struct kl_desktop *desktop,
	uint32_t serial)
{
	(void)desktop;
	(void)serial;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
struct kl_desktop *
kl_desktop_create(
	struct wl_display *display,
	struct wl_surface *surface,
	const char *token,
	const struct kl_desktop_listener *listener,
	void *data)
{
	(void)display;
	(void)surface;
	(void)token;
	(void)listener;
	(void)data;

	/* Not on Windows. */
	return NULL;
}

/* Not on Windows. */
void
kl_desktop_destroy(
	struct kl_desktop *desktop)
{
	(void)desktop;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
struct kl_edit *
kl_edit_create(
	struct wl_display *display,
	struct xdg_toplevel *toplevel,
	kl_edit_fn callback,
	void *data)
{
	(void)display;
	(void)toplevel;
	(void)callback;
	(void)data;

	/* Not on Windows. */
	return NULL;
}

/* Not on Windows. */
void
kl_edit_destroy(
	struct kl_edit *edit)
{
	(void)edit;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
void
kl_edit_set_state(
	struct kl_edit *edit,
	uint32_t operations,
	uint32_t state)
{
	(void)edit;
	(void)operations;
	(void)state;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
void
kl_file_chooser_destroy(
	struct kl_file_chooser *chooser)
{
	(void)chooser;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
struct kl_file_chooser *
kl_file_chooser_open(
	struct wl_display *display,
	struct xdg_toplevel *parent,
	const struct kl_file_chooser_options *options,
	const struct kl_file_chooser_listener *listener,
	void *data)
{
	(void)display;
	(void)parent;
	(void)options;
	(void)listener;
	(void)data;

	/* Not on Windows. */
	return NULL;
}

/* Not on Windows. */
struct kl_glass *
kl_glass_create(
	struct wl_display *display,
	struct wl_surface *surface)
{
	(void)display;
	(void)surface;

	/* Not on Windows. */
	return NULL;
}

/* Not on Windows. */
void
kl_glass_destroy(
	struct kl_glass *glass)
{
	(void)glass;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
int
kl_glass_set_blur(
	struct kl_glass *glass,
	int enabled)
{
	(void)glass;
	(void)enabled;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_glass_set_panels(
	struct kl_glass *glass,
	const struct kl_glass_panel *panels,
	size_t count)
{
	(void)glass;
	(void)panels;
	(void)count;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_instance_bind(
	struct kl_instance *instance,
	struct kl_app *app)
{
	(void)instance;
	(void)app;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
void
kl_instance_close(
	struct kl_instance *instance)
{
	(void)instance;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
int
kl_instance_open(
	const char *name,
	const char *request,
	struct kl_instance **instance)
{
	(void)name;
	(void)request;
	(void)instance;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_instance_take(
	struct kl_instance *instance,
	char *request,
	size_t request_size,
	char *token,
	size_t token_size)
{
	(void)instance;
	(void)request;
	(void)request_size;
	(void)token;
	(void)token_size;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
struct kl_keyboard_inset *
kl_keyboard_inset_create(
	struct wl_display *display,
	struct xdg_toplevel *toplevel,
	kl_keyboard_inset_fn callback,
	void *data)
{
	(void)display;
	(void)toplevel;
	(void)callback;
	(void)data;

	/* Not on Windows. */
	return NULL;
}

/* Not on Windows. */
void
kl_keyboard_inset_destroy(
	struct kl_keyboard_inset *inset)
{
	(void)inset;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
void
kl_media_picker_destroy(
	struct kl_media_picker *picker)
{
	(void)picker;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
struct kl_media_picker *
kl_media_picker_open(
	struct wl_display *display,
	struct xdg_toplevel *parent,
	const struct kl_media_picker_options *options,
	const struct kl_media_picker_listener *listener,
	void *data)
{
	(void)display;
	(void)parent;
	(void)options;
	(void)listener;
	(void)data;

	/* Not on Windows. */
	return NULL;
}

/* Not on Windows. */
int
kl_menu_append(
	struct kl_menu *menu,
	uint32_t id,
	uint32_t parent,
	unsigned type,
	const char *label,
	uint32_t action)
{
	(void)menu;
	(void)id;
	(void)parent;
	(void)type;
	(void)label;
	(void)action;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_menu_begin(
	struct kl_menu *menu)
{
	(void)menu;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_menu_commit(
	struct kl_menu *menu)
{
	(void)menu;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
struct kl_menu *
kl_menu_create(
	struct kl_menu_service *service)
{
	(void)service;

	/* Not on Windows. */
	return NULL;
}

/* Not on Windows. */
void
kl_menu_destroy(
	struct kl_menu *menu)
{
	(void)menu;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
int
kl_menu_insert(
	struct kl_menu *menu,
	uint32_t id,
	uint32_t parent,
	uint32_t before,
	unsigned type,
	const char *label,
	uint32_t action)
{
	(void)menu;
	(void)id;
	(void)parent;
	(void)before;
	(void)type;
	(void)label;
	(void)action;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
struct kl_context_menu *
kl_menu_popup(
	struct kl_menu_service *service,
	struct kl_menu *menu,
	struct wl_surface *surface,
	int32_t x,
	int32_t y,
	struct wl_seat *seat,
	uint32_t serial,
	const struct kl_context_menu_listener *listener,
	void *data)
{
	(void)service;
	(void)menu;
	(void)surface;
	(void)x;
	(void)y;
	(void)seat;
	(void)serial;
	(void)listener;
	(void)data;

	/* Not on Windows. */
	return NULL;
}

/* Not on Windows. */
int
kl_menu_remove(
	struct kl_menu *menu,
	uint32_t id)
{
	(void)menu;
	(void)id;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
void
kl_menu_service_close(
	struct kl_menu_service *service)
{
	(void)service;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
struct kl_menu_service *
kl_menu_service_open(
	struct wl_display *display)
{
	(void)display;

	/* Not on Windows. */
	return NULL;
}

/* Not on Windows. */
int
kl_menu_set_action(
	struct kl_menu *menu,
	uint32_t id,
	uint32_t action)
{
	(void)menu;
	(void)id;
	(void)action;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_menu_set_checked(
	struct kl_menu *menu,
	uint32_t id,
	int checked)
{
	(void)menu;
	(void)id;
	(void)checked;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_menu_set_enabled(
	struct kl_menu *menu,
	uint32_t id,
	int enabled)
{
	(void)menu;
	(void)id;
	(void)enabled;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_menu_set_icon_name(
	struct kl_menu *menu,
	uint32_t id,
	const char *icon_name)
{
	(void)menu;
	(void)id;
	(void)icon_name;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_menu_set_label(
	struct kl_menu *menu,
	uint32_t id,
	const char *label)
{
	(void)menu;
	(void)id;
	(void)label;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_menu_set_role(
	struct kl_menu *menu,
	uint32_t id,
	unsigned role)
{
	(void)menu;
	(void)id;
	(void)role;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_menu_set_shortcut(
	struct kl_menu *menu,
	uint32_t id,
	unsigned modifiers,
	uint32_t keysym)
{
	(void)menu;
	(void)id;
	(void)modifiers;
	(void)keysym;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_menu_set_visible(
	struct kl_menu *menu,
	uint32_t id,
	int visible)
{
	(void)menu;
	(void)id;
	(void)visible;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
void
kl_process_close(
	struct kl_process *process)
{
	(void)process;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
int
kl_process_close_input(
	struct kl_process *process)
{
	(void)process;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
void *
kl_process_data(
	const struct kl_process *process)
{
	(void)process;

	/* Not on Windows. */
	return NULL;
}

/* Not on Windows. */
unsigned
kl_process_exit(
	const struct kl_process *process,
	int *code)
{
	(void)process;
	(void)code;

	/* Not on Windows. */
	return 0;
}

/* Not on Windows. */
int
kl_process_kill(
	struct kl_process *process,
	unsigned how)
{
	(void)process;
	(void)how;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
size_t
kl_process_read(
	struct kl_process *process,
	unsigned stream,
	void *buffer,
	size_t size)
{
	(void)process;
	(void)stream;
	(void)buffer;
	(void)size;

	/* Not on Windows. */
	return 0;
}

/* Not on Windows. */
int
kl_process_read_line(
	struct kl_process *process,
	unsigned stream,
	char *line,
	size_t size)
{
	(void)process;
	(void)stream;
	(void)line;
	(void)size;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_process_resize(
	struct kl_process *process,
	unsigned columns,
	unsigned rows)
{
	(void)process;
	(void)columns;
	(void)rows;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_process_start(
	struct kl_app *app,
	const struct kl_process_options *options,
	struct kl_process **process)
{
	(void)app;
	(void)options;
	(void)process;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_process_terminal_mode(
	const struct kl_process *process,
	unsigned *mode)
{
	(void)process;
	(void)mode;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_process_write(
	struct kl_process *process,
	const void *bytes,
	size_t size,
	size_t *written)
{
	(void)process;
	(void)bytes;
	(void)size;
	(void)written;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_recent_add(
	const char *path,
	const char *application)
{
	(void)path;
	(void)application;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_recent_clear(void)
{
	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_recent_keep(
	int *keep)
{
	(void)keep;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_recent_list(
	struct kl_recent_item *items,
	size_t capacity,
	size_t *count)
{
	(void)items;
	(void)capacity;
	(void)count;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_recent_remove(
	const char *path)
{
	(void)path;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_recent_set_keep(
	int keep)
{
	(void)keep;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_recent_stamp(
	uint64_t *stamp)
{
	(void)stamp;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
void
kl_settings_close(
	struct kl_settings *settings)
{
	(void)settings;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
int
kl_settings_dispatch(
	struct kl_settings *settings)
{
	(void)settings;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_settings_get(
	const struct kl_settings *settings,
	const char *key,
	char *value,
	size_t size,
	unsigned *flags)
{
	(void)settings;
	(void)key;
	(void)value;
	(void)size;
	(void)flags;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_settings_get_int(
	const struct kl_settings *settings,
	const char *key,
	int fallback)
{
	(void)settings;
	(void)key;
	(void)fallback;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
struct kl_settings *
kl_settings_open(
	struct wl_display *display,
	const char *app)
{
	(void)display;
	(void)app;

	/* Not on Windows. */
	return NULL;
}

/* Not on Windows. */
int
kl_settings_reset(
	struct kl_settings *settings,
	const char *key,
	uint32_t *request)
{
	(void)settings;
	(void)key;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_settings_set(
	struct kl_settings *settings,
	const char *key,
	const char *value,
	uint32_t *request)
{
	(void)settings;
	(void)key;
	(void)value;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_settings_set_int(
	struct kl_settings *settings,
	const char *key,
	int value,
	uint32_t *request)
{
	(void)settings;
	(void)key;
	(void)value;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_settings_take_result(
	struct kl_settings *settings,
	uint32_t *request,
	int *error)
{
	(void)settings;
	(void)request;
	(void)error;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
void
kl_settings_unwatch(
	struct kl_settings *settings,
	unsigned watch)
{
	(void)settings;
	(void)watch;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
int
kl_settings_watch(
	struct kl_settings *settings,
	const char *prefix,
	kl_settings_watch_fn fn,
	void *data,
	unsigned *watch)
{
	(void)settings;
	(void)prefix;
	(void)fn;
	(void)data;
	(void)watch;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_account_add_key(
	struct kl_system *system,
	const char *password,
	const char *label,
	const char *pin,
	uint32_t *request)
{
	(void)system;
	(void)password;
	(void)label;
	(void)pin;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_account_administer(
	struct kl_system *system,
	const char *password,
	const char *operation,
	uint32_t *request)
{
	(void)system;
	(void)password;
	(void)operation;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_account_enrolled(
	const struct kl_system *system,
	unsigned *pin,
	unsigned *keys)
{
	(void)system;
	(void)pin;
	(void)keys;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_account_key_cancel(
	struct kl_system *system)
{
	(void)system;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_account_key_info(
	struct kl_system *system,
	uint32_t *request)
{
	(void)system;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_account_key_info_get(
	const struct kl_system *system,
	struct kl_system_key_info *info)
{
	(void)system;
	(void)info;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_account_key_options(
	const struct kl_system *system,
	unsigned *key_pin,
	unsigned *key_touch)
{
	(void)system;
	(void)key_pin;
	(void)key_touch;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_account_key_pin(
	struct kl_system *system,
	const char *current,
	const char *pin,
	uint32_t *request)
{
	(void)system;
	(void)current;
	(void)pin;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
unsigned
kl_system_account_key_removed(
	const struct kl_system *system)
{
	(void)system;

	/* Not on Windows. */
	return 0;
}

/* Not on Windows. */
int
kl_system_account_key_reset(
	struct kl_system *system,
	const char *password,
	uint32_t *request)
{
	(void)system;
	(void)password;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
size_t
kl_system_account_keys(
	const struct kl_system *system,
	struct kl_system_key *keys,
	size_t capacity)
{
	(void)system;
	(void)keys;
	(void)capacity;

	/* Not on Windows. */
	return 0;
}

/* Not on Windows. */
int
kl_system_account_methods(
	const struct kl_system *system,
	unsigned *methods)
{
	(void)system;
	(void)methods;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_account_refusal(
	const struct kl_system *system,
	uint32_t request,
	char *reason,
	size_t size)
{
	(void)system;
	(void)request;
	(void)reason;
	(void)size;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_account_remove_key(
	struct kl_system *system,
	const char *password,
	const char *ref,
	uint32_t *request)
{
	(void)system;
	(void)password;
	(void)ref;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_account_replugged(
	struct kl_system *system,
	uint32_t *request)
{
	(void)system;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_account_set_key_options(
	struct kl_system *system,
	const char *password,
	unsigned key_pin,
	unsigned key_touch,
	uint32_t *request)
{
	(void)system;
	(void)password;
	(void)key_pin;
	(void)key_touch;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_account_set_methods(
	struct kl_system *system,
	const char *password,
	unsigned methods,
	uint32_t *request)
{
	(void)system;
	(void)password;
	(void)methods;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_account_set_password(
	struct kl_system *system,
	const char *current,
	const char *fresh,
	uint32_t *request)
{
	(void)system;
	(void)current;
	(void)fresh;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_account_set_pin(
	struct kl_system *system,
	const char *current,
	const char *pin,
	uint32_t *request)
{
	(void)system;
	(void)current;
	(void)pin;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_account_touched(
	struct kl_system *system,
	uint32_t *request)
{
	(void)system;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_audio_feedback(
	struct kl_system *system,
	uint32_t *request)
{
	(void)system;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
void
kl_system_audio_get_state(
	const struct kl_system *system,
	struct kl_audio_state *state)
{
	(void)system;
	(void)state;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
int
kl_system_audio_set_volume(
	struct kl_system *system,
	unsigned left,
	unsigned right,
	unsigned muted,
	uint32_t *request)
{
	(void)system;
	(void)left;
	(void)right;
	(void)muted;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_bluetooth_device(
	struct kl_system *system,
	unsigned action,
	const char *address,
	unsigned type,
	uint32_t *request)
{
	(void)system;
	(void)action;
	(void)address;
	(void)type;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
size_t
kl_system_bluetooth_devices(
	const struct kl_system *system,
	struct kl_bluetooth_device *devices,
	size_t capacity)
{
	(void)system;
	(void)devices;
	(void)capacity;

	/* Not on Windows. */
	return 0;
}

/* Not on Windows. */
int
kl_system_bluetooth_power(
	struct kl_system *system,
	unsigned on,
	uint32_t *request)
{
	(void)system;
	(void)on;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_bluetooth_scan(
	struct kl_system *system,
	unsigned on)
{
	(void)system;
	(void)on;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_bluetooth_state(
	const struct kl_system *system,
	struct kl_bluetooth_state *state)
{
	(void)system;
	(void)state;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_bluetooth_watch(
	struct kl_system *system,
	unsigned on)
{
	(void)system;
	(void)on;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
unsigned
kl_system_capabilities(
	const struct kl_system *system)
{
	(void)system;

	/* Not on Windows. */
	return 0;
}

/* Not on Windows. */
void
kl_system_close(
	struct kl_system *system)
{
	(void)system;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
int
kl_system_devices_busy_program(
	const struct kl_system *system,
	uint32_t request,
	char *program,
	size_t size)
{
	(void)system;
	(void)request;
	(void)program;
	(void)size;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_devices_eject(
	struct kl_system *system,
	const char *id,
	uint32_t *request)
{
	(void)system;
	(void)id;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
size_t
kl_system_devices_get(
	const struct kl_system *system,
	struct kl_device *devices,
	size_t capacity)
{
	(void)system;
	(void)devices;
	(void)capacity;

	/* Not on Windows. */
	return 0;
}

/* Not on Windows. */
int
kl_system_devices_info(
	const struct kl_system *system,
	const char *id,
	struct kl_device_info *info)
{
	(void)system;
	(void)id;
	(void)info;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_devices_mount(
	struct kl_system *system,
	const char *id,
	uint32_t *request)
{
	(void)system;
	(void)id;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_dispatch(
	struct kl_system *system,
	unsigned *changed)
{
	(void)system;
	(void)changed;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_displays_apply(
	struct kl_system *system,
	unsigned mode,
	const struct kl_display_place *places,
	size_t count,
	uint32_t *request)
{
	(void)system;
	(void)mode;
	(void)places;
	(void)count;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
size_t
kl_system_displays_get(
	const struct kl_system *system,
	struct kl_display *displays,
	size_t capacity)
{
	(void)system;
	(void)displays;
	(void)capacity;

	/* Not on Windows. */
	return 0;
}

/* Not on Windows. */
unsigned
kl_system_displays_mode(
	const struct kl_system *system)
{
	(void)system;

	/* Not on Windows. */
	return 0;
}

/* Not on Windows. */
int
kl_system_displays_rotation(
	const struct kl_system *system,
	const char *key,
	unsigned *rotation)
{
	(void)system;
	(void)key;
	(void)rotation;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_displays_rotations(
	const struct kl_system *system,
	const char *key,
	unsigned *mask,
	unsigned *why)
{
	(void)system;
	(void)key;
	(void)mask;
	(void)why;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_displays_set_brightness(
	struct kl_system *system,
	const char *key,
	unsigned percent,
	uint32_t *request)
{
	(void)system;
	(void)key;
	(void)percent;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_displays_set_rotation(
	struct kl_system *system,
	const char *key,
	unsigned rotation,
	uint32_t *request)
{
	(void)system;
	(void)key;
	(void)rotation;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_displays_set_shown(
	struct kl_system *system,
	const char *key,
	unsigned shown,
	uint32_t *request)
{
	(void)system;
	(void)key;
	(void)shown;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_machine_about(
	const struct kl_system *system,
	struct kl_machine_about *about)
{
	(void)system;
	(void)about;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
size_t
kl_system_machine_filesystems(
	const struct kl_system *system,
	struct kl_machine_filesystem *list,
	size_t capacity)
{
	(void)system;
	(void)list;
	(void)capacity;

	/* Not on Windows. */
	return 0;
}

/* Not on Windows. */
unsigned
kl_system_machine_known(
	const struct kl_system *system)
{
	(void)system;

	/* Not on Windows. */
	return 0;
}

/* Not on Windows. */
int
kl_system_machine_login_language(
	const struct kl_system *system,
	char *code,
	size_t size)
{
	(void)system;
	(void)code;
	(void)size;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
size_t
kl_system_machine_mounts(
	const struct kl_system *system,
	struct kl_machine_mount *list,
	size_t capacity)
{
	(void)system;
	(void)list;
	(void)capacity;

	/* Not on Windows. */
	return 0;
}

/* Not on Windows. */
int
kl_system_machine_query(
	struct kl_system *system,
	unsigned what,
	uint32_t *request)
{
	(void)system;
	(void)what;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
uint32_t
kl_system_machine_serial(
	const struct kl_system *system,
	unsigned part)
{
	(void)system;
	(void)part;

	/* Not on Windows. */
	return 0;
}

/* Not on Windows. */
size_t
kl_system_machine_users(
	const struct kl_system *system,
	struct kl_machine_user *list,
	size_t capacity)
{
	(void)system;
	(void)list;
	(void)capacity;

	/* Not on Windows. */
	return 0;
}

/* Not on Windows. */
int
kl_system_mail_allowed(
	const struct kl_system *system)
{
	(void)system;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_mail_arrived(
	struct kl_system *system,
	const struct kl_mail_arrival *arrival,
	uint32_t *request)
{
	(void)system;
	(void)arrival;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_mail_listen(
	struct kl_system *system,
	const char *app,
	uint32_t *request)
{
	(void)system;
	(void)app;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_media_add_paths(
	struct kl_system *system,
	const char *const *paths,
	size_t count,
	int *descriptor)
{
	(void)system;
	(void)paths;
	(void)count;
	(void)descriptor;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_media_add_paths_begin(
	struct kl_system *system,
	const char *const *paths,
	size_t count,
	uint32_t *request)
{
	(void)system;
	(void)paths;
	(void)count;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_media_apply(
	struct kl_system *system,
	int changes,
	int *descriptor)
{
	(void)system;
	(void)changes;
	(void)descriptor;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_media_apply_begin(
	struct kl_system *system,
	int changes,
	uint32_t *request)
{
	(void)system;
	(void)changes;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
void
kl_system_media_cancel(
	struct kl_system *system,
	uint32_t request)
{
	(void)system;
	(void)request;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
int
kl_system_media_list(
	struct kl_system *system,
	int *descriptor)
{
	(void)system;
	(void)descriptor;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_media_list_begin(
	struct kl_system *system,
	uint32_t *request)
{
	(void)system;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_media_notify(
	struct kl_system *system)
{
	(void)system;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_media_remove_ids(
	struct kl_system *system,
	const char *const *ids,
	size_t count,
	int *descriptor)
{
	(void)system;
	(void)ids;
	(void)count;
	(void)descriptor;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_media_remove_ids_begin(
	struct kl_system *system,
	const char *const *ids,
	size_t count,
	uint32_t *request)
{
	(void)system;
	(void)ids;
	(void)count;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_media_take(
	struct kl_system *system,
	uint32_t *request,
	int *error,
	int *descriptor)
{
	(void)system;
	(void)request;
	(void)error;
	(void)descriptor;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_media_watch(
	struct kl_system *system,
	unsigned on)
{
	(void)system;
	(void)on;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
void
kl_system_monitor_close(
	struct kl_system_monitor *monitor)
{
	(void)monitor;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
const struct kl_monitor_info *
kl_system_monitor_info(
	const struct kl_system_monitor *monitor,
	unsigned *changes)
{
	(void)monitor;
	(void)changes;

	/* Not on Windows. */
	return NULL;
}

/* Not on Windows. */
struct kl_system_monitor *
kl_system_monitor_open(
	struct kl_system *system,
	unsigned period_ms)
{
	(void)system;
	(void)period_ms;

	/* Not on Windows. */
	return NULL;
}

/* Not on Windows. */
int
kl_system_monitor_take(
	struct kl_system_monitor *monitor,
	struct kl_monitor_frame *frame)
{
	(void)monitor;
	(void)frame;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_network_configure_wired(
	struct kl_system *system,
	const struct kl_network_wired_config *config,
	uint32_t *request)
{
	(void)system;
	(void)config;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
size_t
kl_system_network_get_dns(
	const struct kl_system *system,
	char (*servers)[KL_NETWORK_ADDRESS_MAX],
	size_t capacity)
{
	(void)system;
	(void)servers;
	(void)capacity;

	/* Not on Windows. */
	return 0;
}

/* Not on Windows. */
size_t
kl_system_network_get_links(
	const struct kl_system *system,
	struct kl_network_link *links,
	size_t capacity)
{
	(void)system;
	(void)links;
	(void)capacity;

	/* Not on Windows. */
	return 0;
}

/* Not on Windows. */
size_t
kl_system_network_get_saved(
	const struct kl_system *system,
	char (*ssids)[KL_NETWORK_SSID_MAX],
	size_t capacity)
{
	(void)system;
	(void)ssids;
	(void)capacity;

	/* Not on Windows. */
	return 0;
}

/* Not on Windows. */
size_t
kl_system_network_get_scan(
	const struct kl_system *system,
	struct kl_network_ap *aps,
	size_t capacity)
{
	(void)system;
	(void)aps;
	(void)capacity;

	/* Not on Windows. */
	return 0;
}

/* Not on Windows. */
void
kl_system_network_get_state(
	const struct kl_system *system,
	struct kl_network_state *state)
{
	(void)system;
	(void)state;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
int
kl_system_network_query_details(
	struct kl_system *system,
	uint32_t *request)
{
	(void)system;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_network_request(
	struct kl_system *system,
	unsigned what,
	const char *ssid,
	uint32_t *request)
{
	(void)system;
	(void)what;
	(void)ssid;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_network_save_key(
	struct kl_system *system,
	const char *ssid,
	const char *key,
	uint32_t *request)
{
	(void)system;
	(void)ssid;
	(void)key;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_network_set_scanning(
	struct kl_system *system,
	unsigned on)
{
	(void)system;
	(void)on;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_notify(
	struct kl_system *system,
	const struct kl_notification *notification,
	uint32_t *request)
{
	(void)system;
	(void)notification;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_notify_lock(
	struct kl_system *system,
	const struct kl_notification *notification,
	const char *lock_text,
	uint32_t *request)
{
	(void)system;
	(void)notification;
	(void)lock_text;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_notify_withdraw(
	struct kl_system *system,
	uint32_t id,
	uint32_t *request)
{
	(void)system;
	(void)id;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
struct kl_system *
kl_system_open(
	struct wl_display *display)
{
	(void)display;

	/* Not on Windows. */
	return NULL;
}

/* Not on Windows. */
int
kl_system_phone_call(
	struct kl_system *system,
	unsigned channel,
	const char *to,
	uint32_t *request)
{
	(void)system;
	(void)channel;
	(void)to;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_phone_calls(
	const struct kl_system *system,
	struct kl_phone_call *calls,
	size_t capacity,
	size_t *count)
{
	(void)system;
	(void)calls;
	(void)capacity;
	(void)count;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_phone_control(
	struct kl_system *system,
	unsigned ask,
	const char *argument,
	uint32_t *request)
{
	(void)system;
	(void)ask;
	(void)argument;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_phone_hf(
	const struct kl_system *system,
	struct kl_phone_hf *hf)
{
	(void)system;
	(void)hf;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_phone_link(
	const struct kl_system *system,
	struct kl_phone_link *link,
	size_t size)
{
	(void)system;
	(void)link;
	(void)size;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_phone_link_set(
	struct kl_system *system,
	const char *address,
	unsigned on,
	unsigned profiles,
	uint32_t *request)
{
	(void)system;
	(void)address;
	(void)on;
	(void)profiles;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_phone_listen(
	struct kl_system *system,
	unsigned on)
{
	(void)system;
	(void)on;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_phone_mark_read(
	struct kl_system *system,
	const char *handle,
	uint32_t *request)
{
	(void)system;
	(void)handle;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_phone_page_end(
	const struct kl_system *system,
	uint32_t request,
	char *cursor,
	size_t cursor_size,
	unsigned *more,
	unsigned *count,
	unsigned *skipped,
	unsigned *capped)
{
	(void)system;
	(void)request;
	(void)cursor;
	(void)cursor_size;
	(void)more;
	(void)count;
	(void)skipped;
	(void)capped;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_phone_send(
	struct kl_system *system,
	unsigned channel,
	const char *to,
	const char *text,
	uint32_t *request)
{
	(void)system;
	(void)channel;
	(void)to;
	(void)text;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_phone_send_mms(
	struct kl_system *system,
	const char *to,
	int descriptor,
	uint32_t *request)
{
	(void)system;
	(void)to;
	(void)descriptor;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_phone_send_text(
	struct kl_system *system,
	unsigned channel,
	const char *to,
	const char *text,
	size_t length,
	uint32_t *request)
{
	(void)system;
	(void)channel;
	(void)to;
	(void)text;
	(void)length;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_phone_sync(
	struct kl_system *system,
	unsigned what,
	int64_t since,
	unsigned limit,
	const char *cursor,
	unsigned count,
	uint32_t *request)
{
	(void)system;
	(void)what;
	(void)since;
	(void)limit;
	(void)cursor;
	(void)count;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_phone_watch_link(
	struct kl_system *system,
	unsigned on)
{
	(void)system;
	(void)on;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_power_action(
	struct kl_system *system,
	unsigned action,
	uint32_t *request)
{
	(void)system;
	(void)action;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
void
kl_system_power_get_state(
	const struct kl_system *system,
	struct kl_power_state *state)
{
	(void)system;
	(void)state;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
int
kl_system_print_cancel(
	struct kl_system *system,
	uint32_t job,
	uint32_t *request)
{
	(void)system;
	(void)job;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_print_job_of(
	const struct kl_system *system,
	uint32_t request,
	uint32_t *job)
{
	(void)system;
	(void)request;
	(void)job;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
size_t
kl_system_print_jobs_get(
	const struct kl_system *system,
	struct kl_print_job *jobs,
	size_t capacity)
{
	(void)system;
	(void)jobs;
	(void)capacity;

	/* Not on Windows. */
	return 0;
}

/* Not on Windows. */
int
kl_system_printers_add(
	struct kl_system *system,
	unsigned protocol,
	const char *host,
	unsigned port,
	const char *path,
	uint32_t *request)
{
	(void)system;
	(void)protocol;
	(void)host;
	(void)port;
	(void)path;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_printers_edit(
	struct kl_system *system,
	uint32_t printer,
	const char *name,
	const char *path,
	uint32_t *request)
{
	(void)system;
	(void)printer;
	(void)name;
	(void)path;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
size_t
kl_system_printers_get(
	const struct kl_system *system,
	struct kl_printer *printers,
	size_t capacity)
{
	(void)system;
	(void)printers;
	(void)capacity;

	/* Not on Windows. */
	return 0;
}

/* Not on Windows. */
int
kl_system_printers_print(
	struct kl_system *system,
	uint32_t printer,
	const char *path,
	const char *title,
	uint32_t *request)
{
	(void)system;
	(void)printer;
	(void)path;
	(void)title;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_printers_remove(
	struct kl_system *system,
	uint32_t printer,
	uint32_t *request)
{
	(void)system;
	(void)printer;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_printers_set_default(
	struct kl_system *system,
	uint32_t printer,
	uint32_t *request)
{
	(void)system;
	(void)printer;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
void
kl_system_sharing_get_state(
	const struct kl_system *system,
	struct kl_sharing_state *state)
{
	(void)system;
	(void)state;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
int
kl_system_sharing_query(
	struct kl_system *system,
	uint32_t *request)
{
	(void)system;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_sharing_set_ssh(
	struct kl_system *system,
	unsigned on,
	uint32_t *request)
{
	(void)system;
	(void)on;
	(void)request;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_take_mail_event(
	struct kl_system *system,
	struct kl_mail_event *event)
{
	(void)system;
	(void)event;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_take_notify_event(
	struct kl_system *system,
	struct kl_notify_event *event)
{
	(void)system;
	(void)event;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_take_phone_event(
	struct kl_system *system,
	struct kl_phone_event *event)
{
	(void)system;
	(void)event;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_take_phone_item(
	struct kl_system *system,
	struct kl_phone_item *item,
	size_t size)
{
	(void)system;
	(void)item;
	(void)size;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_system_take_result(
	struct kl_system *system,
	uint32_t *request,
	int *error)
{
	(void)system;
	(void)request;
	(void)error;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_titlebar_add_control(
	struct kl_titlebar *titlebar,
	uint32_t id,
	unsigned role,
	unsigned priority,
	unsigned group,
	const char *label)
{
	(void)titlebar;
	(void)id;
	(void)role;
	(void)priority;
	(void)group;
	(void)label;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_titlebar_add_tab(
	struct kl_titlebar *titlebar,
	uint32_t id,
	const char *title)
{
	(void)titlebar;
	(void)id;
	(void)title;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_titlebar_begin(
	struct kl_titlebar *titlebar)
{
	(void)titlebar;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_titlebar_commit(
	struct kl_titlebar *titlebar)
{
	(void)titlebar;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
struct kl_titlebar *
kl_titlebar_create(
	struct wl_display *display,
	struct xdg_toplevel *toplevel,
	const struct kl_titlebar_listener *listener,
	void *data)
{
	(void)display;
	(void)toplevel;
	(void)listener;
	(void)data;

	/* Not on Windows. */
	return NULL;
}

/* Not on Windows. */
void
kl_titlebar_destroy(
	struct kl_titlebar *titlebar)
{
	(void)titlebar;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
int
kl_titlebar_focus_control(
	struct kl_titlebar *titlebar,
	uint32_t id,
	unsigned mode)
{
	(void)titlebar;
	(void)id;
	(void)mode;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_titlebar_remove_control(
	struct kl_titlebar *titlebar,
	uint32_t id)
{
	(void)titlebar;
	(void)id;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_titlebar_remove_tab(
	struct kl_titlebar *titlebar,
	uint32_t id)
{
	(void)titlebar;
	(void)id;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_titlebar_set_breadcrumb(
	struct kl_titlebar *titlebar,
	uint32_t id,
	const char *const *segments,
	size_t count)
{
	(void)titlebar;
	(void)id;
	(void)segments;
	(void)count;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_titlebar_set_control_label(
	struct kl_titlebar *titlebar,
	uint32_t id,
	const char *label)
{
	(void)titlebar;
	(void)id;
	(void)label;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_titlebar_set_control_state(
	struct kl_titlebar *titlebar,
	uint32_t id,
	int enabled,
	int checked)
{
	(void)titlebar;
	(void)id;
	(void)enabled;
	(void)checked;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_titlebar_set_control_text(
	struct kl_titlebar *titlebar,
	uint32_t id,
	const char *text,
	const char *placeholder)
{
	(void)titlebar;
	(void)id;
	(void)text;
	(void)placeholder;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_titlebar_set_control_value(
	struct kl_titlebar *titlebar,
	uint32_t id,
	unsigned value)
{
	(void)titlebar;
	(void)id;
	(void)value;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_titlebar_set_mode(
	struct kl_titlebar *titlebar,
	unsigned mode)
{
	(void)titlebar;
	(void)mode;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_titlebar_set_suggestions(
	struct kl_titlebar *titlebar,
	uint32_t id,
	const char *const *labels,
	const char *const *texts,
	size_t count)
{
	(void)titlebar;
	(void)id;
	(void)labels;
	(void)texts;
	(void)count;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_titlebar_set_tab(
	struct kl_titlebar *titlebar,
	uint32_t id,
	const char *title,
	unsigned flags)
{
	(void)titlebar;
	(void)id;
	(void)title;
	(void)flags;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_titlebar_set_tabs_options(
	struct kl_titlebar *titlebar,
	unsigned options)
{
	(void)titlebar;
	(void)options;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
const char *
kl_tr(
	const char *english)
{
	(void)english;

	/* Not on Windows. */
	return NULL;
}

/* Not on Windows. */
void
kl_tr_close(void)
{
	/* Nothing is done on Windows. */
}

/* Not on Windows. */
void
kl_tr_discard(
	struct kl_tr_catalogs *catalogs)
{
	(void)catalogs;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
int
kl_tr_format(
	char *out,
	size_t size,
	const char *pattern,
	...)
{
	(void)out;
	(void)size;
	(void)pattern;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
const char *
kl_tr_language(void)
{
	/* Not on Windows. */
	return NULL;
}

/* Not on Windows. */
const char *
kl_tr_language_code(
	int setting)
{
	(void)setting;

	/* Not on Windows. */
	return NULL;
}

/* Not on Windows. */
int
kl_tr_open(
	const char *domain,
	const char *language)
{
	(void)domain;
	(void)language;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_tr_open_directory(
	const char *directory,
	const char *domain,
	const char *language)
{
	(void)directory;
	(void)domain;
	(void)language;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_tr_read(
	const char *domain,
	const char *language,
	struct kl_tr_catalogs **out)
{
	(void)domain;
	(void)language;
	(void)out;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
void
kl_tr_use(
	struct kl_tr_catalogs *catalogs)
{
	(void)catalogs;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
const char *
kl_trc(
	const char *context,
	const char *english)
{
	(void)context;
	(void)english;

	/* Not on Windows. */
	return NULL;
}

/* Not on Windows. */
const char *
kl_trn(
	const char *singular,
	const char *plural,
	unsigned long count)
{
	(void)singular;
	(void)plural;
	(void)count;

	/* Not on Windows. */
	return NULL;
}

/* Not on Windows. */
int
kl_ui_window_input(
	struct kl_ui *ui,
	const struct kl_window_event *event)
{
	(void)ui;
	(void)event;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
void
kl_ui_window_text(
	struct kl_ui *ui,
	struct kl_window *window)
{
	(void)ui;
	(void)window;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
int
kl_window_accept_drops(
	struct kl_window *window,
	unsigned types)
{
	(void)window;
	(void)types;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_window_accept_tablet(
	struct kl_window *window)
{
	(void)window;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
void
kl_window_answer_drop(
	struct kl_window *window,
	unsigned actions,
	unsigned preferred)
{
	(void)window;
	(void)actions;
	(void)preferred;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
int
kl_window_desktop_place(
	const struct kl_window *window,
	int32_t *x,
	int32_t *y)
{
	(void)window;
	(void)x;
	(void)y;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
const char *
kl_window_device_name(
	const struct kl_window *window)
{
	(void)window;

	/* Not on Windows. */
	return NULL;
}

/* Not on Windows. */
int
kl_window_dispatch(
	struct kl_window *window,
	int timeout_ms)
{
	(void)window;
	(void)timeout_ms;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_window_dispatch_fds(
	struct kl_window *window,
	const int *fds,
	unsigned count,
	int timeout_ms,
	int *ready)
{
	(void)window;
	(void)fds;
	(void)count;
	(void)timeout_ms;
	(void)ready;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
struct wl_display *
kl_window_display(
	const struct kl_window *window)
{
	(void)window;

	/* Not on Windows. */
	return NULL;
}

/* Not on Windows. */
int
kl_window_drag_fill(
	struct kl_window *window,
	const char *type,
	const void *data,
	size_t length)
{
	(void)window;
	(void)type;
	(void)data;
	(void)length;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_window_drag_text(
	struct kl_window *window,
	const char *text,
	size_t length,
	uint32_t serial)
{
	(void)window;
	(void)text;
	(void)length;
	(void)serial;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
void
kl_window_edit_by_keys(
	struct kl_window *window)
{
	(void)window;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
void
kl_window_edit_state(
	struct kl_window *window,
	unsigned state)
{
	(void)window;
	(void)state;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
void
kl_window_finish_drop(
	struct kl_window *window,
	unsigned action)
{
	(void)window;
	(void)action;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
int
kl_window_frame_wait(
	const struct kl_window *window,
	uint64_t now_us)
{
	(void)window;
	(void)now_us;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_window_fullscreen(
	const struct kl_window *window)
{
	(void)window;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
void
kl_window_keyboard_inset(
	const struct kl_window *window,
	int *right,
	int *bottom)
{
	(void)window;
	(void)right;
	(void)bottom;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
int
kl_window_maximized(
	const struct kl_window *window)
{
	(void)window;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
struct kl_window_menu *
kl_window_menu_create(
	struct kl_menu_service *service,
	struct xdg_toplevel *toplevel,
	const struct kl_window_menu_listener *listener,
	void *data)
{
	(void)service;
	(void)toplevel;
	(void)listener;
	(void)data;

	/* Not on Windows. */
	return NULL;
}

/* Not on Windows. */
void
kl_window_menu_destroy(
	struct kl_window_menu *window_menu)
{
	(void)window_menu;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
int
kl_window_menu_set(
	struct kl_window_menu *window_menu,
	struct kl_menu *menu)
{
	(void)window_menu;
	(void)menu;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
void
kl_window_minimize(
	struct kl_window *window)
{
	(void)window;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
void
kl_window_on_edit(
	struct kl_window *window,
	kl_window_edit_fn callback,
	void *data)
{
	(void)window;
	(void)callback;
	(void)data;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
void
kl_window_on_keyboard_inset(
	struct kl_window *window,
	kl_window_keyboard_inset_fn callback,
	void *data)
{
	(void)window;
	(void)callback;
	(void)data;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
struct kl_window *
kl_window_open(
	const struct kl_window_options *options)
{
	(void)options;

	/* Not on Windows. */
	return NULL;
}

/* Not on Windows. */
int
kl_window_output_mode(
	const struct kl_window *window,
	int32_t *width,
	int32_t *height,
	int32_t *refresh)
{
	(void)window;
	(void)width;
	(void)height;
	(void)refresh;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
size_t
kl_window_paste_primary(
	struct kl_window *window,
	char *text,
	size_t size)
{
	(void)window;
	(void)text;
	(void)size;

	/* Not on Windows. */
	return 0;
}

/* Not on Windows. */
int
kl_window_popup_menu(
	struct kl_window *window,
	const struct kl_menu_entry *entries,
	size_t count,
	int x,
	int y)
{
	(void)window;
	(void)entries;
	(void)count;
	(void)x;
	(void)y;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
void
kl_window_present_times(
	const struct kl_window *window,
	struct kl_present_times *times)
{
	(void)window;
	(void)times;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
uint32_t
kl_window_press_serial(
	const struct kl_window *window)
{
	(void)window;

	/* Not on Windows. */
	return 0;
}

/* Not on Windows. */
int
kl_window_receive_drop(
	struct kl_window *window,
	char **data,
	size_t *length,
	unsigned *type)
{
	(void)window;
	(void)data;
	(void)length;
	(void)type;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_window_repeat(
	struct kl_window *window,
	uint64_t now_us)
{
	(void)window;
	(void)now_us;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_window_repeat_wait(
	const struct kl_window *window,
	uint64_t now_us)
{
	(void)window;
	(void)now_us;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
struct wl_seat *
kl_window_seat(
	const struct kl_window *window)
{
	(void)window;

	/* Not on Windows. */
	return NULL;
}

/* Not on Windows. */
int
kl_window_see_through(
	const struct kl_window *window)
{
	(void)window;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
void
kl_window_select(
	struct kl_window *window,
	const char *text,
	size_t length)
{
	(void)window;
	(void)text;
	(void)length;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
int
kl_window_selecting(
	const struct kl_window *window)
{
	(void)window;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_window_selection_own(
	const struct kl_window *window,
	unsigned which)
{
	(void)window;
	(void)which;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
uint32_t
kl_window_serial(
	const struct kl_window *window)
{
	(void)window;

	/* Not on Windows. */
	return 0;
}

/* Not on Windows. */
int
kl_window_set_content_type(
	struct kl_window *window,
	unsigned type)
{
	(void)window;
	(void)type;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_window_set_control_suggestions(
	struct kl_window *window,
	uint32_t id,
	const char *const *labels,
	const char *const *texts,
	size_t count)
{
	(void)window;
	(void)id;
	(void)labels;
	(void)texts;
	(void)count;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
void
kl_window_set_fullscreen(
	struct kl_window *window,
	int fullscreen)
{
	(void)window;
	(void)fullscreen;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
int
kl_window_set_glass(
	struct kl_window *window,
	const struct kl_glass_panel *panels,
	size_t count)
{
	(void)window;
	(void)panels;
	(void)count;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_window_set_glass_blur(
	struct kl_window *window,
	int enabled)
{
	(void)window;
	(void)enabled;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
void
kl_window_set_maximized(
	struct kl_window *window,
	int maximized)
{
	(void)window;
	(void)maximized;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
int
kl_window_set_menu(
	struct kl_window *window,
	const struct kl_menu_entry *entries,
	size_t count)
{
	(void)window;
	(void)entries;
	(void)count;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_window_set_opaque(
	struct kl_window *window,
	int opaque)
{
	(void)window;
	(void)opaque;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_window_set_pacing(
	struct kl_window *window,
	unsigned flags,
	unsigned max_fps)
{
	(void)window;
	(void)flags;
	(void)max_fps;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_window_set_repeat(
	struct kl_window *window,
	int enabled)
{
	(void)window;
	(void)enabled;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
void
kl_window_set_serial(
	struct kl_window *window,
	uint32_t serial)
{
	(void)window;
	(void)serial;

	/* Nothing is done on Windows. */
}

/* Not on Windows. */
int
kl_window_set_tabs(
	struct kl_window *window,
	const struct kl_tab_entry *tabs,
	size_t count,
	unsigned options)
{
	(void)window;
	(void)tabs;
	(void)count;
	(void)options;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_window_start_drag(
	struct kl_window *window,
	const struct kl_drag_data *data,
	size_t count,
	unsigned actions,
	uint32_t serial)
{
	(void)window;
	(void)data;
	(void)count;
	(void)actions;
	(void)serial;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
int
kl_window_start_drag_icon(
	struct kl_window *window,
	const struct kl_drag_data *data,
	size_t count,
	unsigned actions,
	uint32_t serial,
	const struct kl_drag_icon *icon)
{
	(void)window;
	(void)data;
	(void)count;
	(void)actions;
	(void)serial;
	(void)icon;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
struct wl_surface *
kl_window_surface(
	const struct kl_window *window)
{
	(void)window;

	/* Not on Windows. */
	return NULL;
}

/* Not on Windows. */
int
kl_window_take(
	struct kl_window *window,
	struct kl_window_event *event)
{
	(void)window;
	(void)event;

	/* Not on Windows. */
	return ENOTSUP;
}

/* Not on Windows. */
size_t
kl_window_take_drop(
	struct kl_window *window,
	char *text,
	size_t size,
	unsigned *type)
{
	(void)window;
	(void)text;
	(void)size;
	(void)type;

	/* Not on Windows. */
	return 0;
}

/* Not on Windows. */
struct xdg_toplevel *
kl_window_toplevel(
	const struct kl_window *window)
{
	(void)window;

	/* Not on Windows. */
	return NULL;
}
