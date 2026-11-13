/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Stand-ins for the parts of the compositor that objects.c calls, for the
 * host test of the pace of the gone clients' releases (retire-test.c).  They
 * are declared here without kwl.h: each does nothing and gives 0, except the
 * clocks (the real monotonic clock in milliseconds, so the timer thread and
 * the releases count on one clock).  kwl_timer_set, which arms the real
 * timers, is retire-test.c's.
 */

#include "threads.h"

#include <stdint.h>

uint64_t kwl_milliseconds(void);
uint64_t kwl_microseconds(void);
uint64_t kwl_cycles(void);
void kwl_fatal(const char *format, ...);
long kl_backend_gpu_resource_free(void);
long kwl_audio_object_gone(void);
long kwl_bluetooth_gone(void);
long kwl_client_fail(void);
long kwl_compose_client_gone(void);
long kwl_content_type_object_gone(void);
long kwl_cursor_default(void);
long kwl_cursor_shape_object_gone(void);
long kwl_data_object_gone(void);
long kwl_decoration_object_gone(void);
long kwl_delete_id(void);
long kwl_desktop_object_gone(void);
long kwl_displays_object_gone(void);
long kwl_edit_object_gone(void);
long kwl_emit(void);
long kwl_game_object_gone(void);
long kwl_glass_forget(void);
long kwl_gpu_host(void);
long kwl_gpu_resource(void);
long kwl_ime_client_gone(void);
long kwl_ime_object_gone(void);
long kwl_import_destroy_with(void);
long kwl_inset_object_gone(void);
long kwl_keyboard_client_told(void);
long kwl_log_us(void);
long kwl_machine_gone(void);
long kwl_media_library_gone(void);
long kwl_menu_object_gone(void);
long kwl_notify_client_gone(void);
long kwl_packet_free(void);
long kwl_panels_object_gone(void);
long kwl_perf_vk_outside(void);
long kwl_phone_gone(void);
long kwl_pool_put(void);
long kwl_popup_object_gone(void);
long kwl_primary_object_gone(void);
long kwl_scanout_buffer_gone(void);
long kwl_scanout_client_gone(void);
long kwl_scanout_surface_gone(void);
long kwl_seat_surface_gone(void);
long kwl_shm_image_destroy(void);
long kwl_subsurface_object_gone(void);
long kwl_system_network_gone(void);
long kwl_tablet_object_gone(void);
long kwl_titlebar_object_gone(void);
long kwl_title_tap_forget(void);
long kwl_toplevel_surface_gone(void);
long kwl_touch_object_gone(void);
long kwl_viewport_object_gone(void);

/* The real clock, the one the timers count on. */
uint64_t
kwl_milliseconds(
	void)
{
	/* The monotonic clock in milliseconds. */
	return kwl_timers_now();
}

/* The real clock in microseconds (only differences are used). */
uint64_t
kwl_microseconds(
	void)
{
	/* The milliseconds, finer is not needed. */
	return kwl_timers_now() * 1000U;
}

/* No cycle counter on the host. */
uint64_t
kwl_cycles(
	void)
{
	/* Nothing counted. */
	return 0U;
}

/* A fatal line goes nowhere in the test. */
void
kwl_fatal(
	const char *format,
	...)
{
	/* Nothing to print. */
	(void)format;
}

/* Does nothing in the test. */
long
kl_backend_gpu_resource_free(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_audio_object_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_bluetooth_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_compose_client_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_content_type_object_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_cursor_default(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_cursor_shape_object_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_data_object_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_decoration_object_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_delete_id(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_desktop_object_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_displays_object_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_edit_object_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_client_fail(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_emit(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_game_object_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_glass_forget(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_gpu_host(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_gpu_resource(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_ime_client_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_ime_object_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_import_destroy_with(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_inset_object_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_keyboard_client_told(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_log_us(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_machine_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_media_library_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_menu_object_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_notify_client_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_packet_free(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_panels_object_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_perf_vk_outside(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_phone_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_pool_put(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_popup_object_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_primary_object_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_scanout_buffer_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_scanout_client_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_scanout_surface_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_seat_surface_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_shm_image_destroy(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_subsurface_object_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_system_network_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_tablet_object_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_titlebar_object_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_title_tap_forget(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_toplevel_surface_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_touch_object_gone(
	void)
{
	/* Nothing done. */
	return 0;
}

/* Does nothing in the test. */
long
kwl_viewport_object_gone(
	void)
{
	/* Nothing done. */
	return 0;
}
