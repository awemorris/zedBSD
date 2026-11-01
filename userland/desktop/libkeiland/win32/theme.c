/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The appearance on Windows: the apps' light or dark mode the user chose
 * (HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Themes\
 * Personalize, the DWORD AppsUseLightTheme; light when it is missing)
 * and the system's accent colour (DwmGetColorizationColor), taken as the
 * nearest of libkeiland's eight accents: graphite for a grey, otherwise
 * the one of the nearest hue (blue when it cannot be read).  They choose
 * libkeiland's light or dark theme with its accent and the frames' dark
 * title bars (DWMWA_USE_IMMERSIVE_DARK_MODE, window.c).  They are read
 * when the application opens and again when the system says the colours
 * changed (WM_SETTINGCHANGE "ImmersiveColorSet",
 * WM_DWMCOLORIZATIONCOLORCHANGED; window.c), and a change is told to the
 * program with a KL_APP_THEME event (app.c).
 */

#include "userland/desktop/libkeiland/ui/internal.h"
#include "userland/desktop/libkeiland/win32/window-win32.h"

#include <dwmapi.h>

/* The key and the value of the apps' mode. */
#define THEME_KEY	L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"
#define THEME_VALUE	L"AppsUseLightTheme"

/*
 * The saturation (0 to 1) below which the system's accent is a grey
 * (graphite).
 */
#define THEME_GREY	0.2f

static unsigned theme_accent_of(uint32_t color);
static float theme_hue(uint32_t color, float *saturation);

/*
 * The appearance read last (KL_APPEARANCE_*), and the accent
 * (KL_ACCENT_*); written on the application's thread when it opens and
 * when the system's colours change, read by any thread after.
 */
static unsigned theme_appearance = KL_APPEARANCE_LIGHT;
static unsigned theme_accent = KL_ACCENT_BLUE;

/*
 * Reports the appearance read last (the argument, a Wayland watch, is not
 * used on Windows).
 */
unsigned
kl_appearance_get(
	const struct kl_appearance *appearance)
{
	(void)appearance;

	/* The appearance. */
	return theme_appearance;
}

/*
 * Reports the accent read last: the nearest of the eight to the system's.
 */
unsigned
kl_accent_get(void)
{
	/* The accent. */
	return theme_accent;
}

/*
 * Reads the apps' mode and the system's accent and makes libkeiland's
 * theme follow them.  Returns 1 for the dark mode, 0 for the light one.
 */
int
keiui_win32_dark(void)
{
	DWORD value;
	DWORD size;
	DWORD colorization;
	BOOL opaque;
	LSTATUS status;
	HRESULT result;

	/* The setting; missing or unreadable is light. */
	value = 1U;
	size = sizeof(value);
	status = RegGetValueW(HKEY_CURRENT_USER, THEME_KEY, THEME_VALUE, RRF_RT_REG_DWORD, NULL, &value, &size);
	theme_appearance = KL_APPEARANCE_LIGHT;
	if (status == ERROR_SUCCESS && value == 0U)
		theme_appearance = KL_APPEARANCE_DARK;

	/* The accent; one that cannot be read is blue. */
	colorization = 0U;
	opaque = FALSE;
	result = DwmGetColorizationColor(&colorization, &opaque);
	theme_accent = KL_ACCENT_BLUE;
	if (SUCCEEDED(result))
		theme_accent = theme_accent_of((uint32_t)colorization);

	/* libkeiland's theme of the appearance and the accent. */
	keiui_theme_set(theme_appearance, theme_accent);

	/* The mode. */
	if (theme_appearance == KL_APPEARANCE_DARK)
		return 1;
	return 0;
}

/*
 * Reads the system's colours again (keiui_win32_dark) into the
 * application.  Returns 1 when the appearance or the accent changed, 0
 * when both are as they were.
 */
int
keiui_win32_theme_reread(
	struct kl_app *app)
{
	unsigned appearance;
	unsigned accent;

	/* The colours before, then now. */
	appearance = theme_appearance;
	accent = theme_accent;
	app->dark = keiui_win32_dark();

	/* Nothing changed. */
	if (appearance == theme_appearance && accent == theme_accent)
		return 0;

	/* Changed. */
	return 1;
}

/*
 * Finds the nearest of libkeiland's accents to a colour (0xAARRGGBB, the
 * alpha not used): graphite for a grey, otherwise the one whose light
 * accent has the nearest hue.
 */
static unsigned
theme_accent_of(
	uint32_t color)
{
	struct kl_accent values;
	float hue;
	float other;
	float saturation;
	float ignored;
	float distance;
	float best_distance;
	unsigned best;
	unsigned accent;

	/* A grey is graphite. */
	hue = theme_hue(color, &saturation);
	if (saturation < THEME_GREY)
		return KL_ACCENT_GRAPHITE;

	/* The coloured accents, by the distance round the hue's circle. */
	best = KL_ACCENT_BLUE;
	best_distance = 360.0f;
	for (accent = 0U; accent < KL_ACCENTS; accent++) {
		if (accent == KL_ACCENT_GRAPHITE)
			continue;

		/* This accent's hue, and how far it is. */
		kl_accent_values(accent, KL_APPEARANCE_LIGHT, &values);
		other = theme_hue((uint32_t)values.accent, &ignored);
		distance = hue - other;
		if (distance < 0.0f)
			distance = -distance;
		if (distance > 180.0f)
			distance = 360.0f - distance;

		/* The nearest so far. */
		if (distance < best_distance) {
			best_distance = distance;
			best = accent;
		}
	}

	/* The nearest. */
	return best;
}

/*
 * Reports a colour's hue in degrees (0 up to 360) and its saturation (0
 * to 1, of the HSV model).
 */
static float
theme_hue(
	uint32_t color,
	float *saturation)
{
	float red;
	float green;
	float blue;
	float high;
	float low;
	float span;
	float hue;

	/* The channels from 0 to 1, and the highest and lowest. */
	red = (float)((color >> 16) & 0xffU) / 255.0f;
	green = (float)((color >> 8) & 0xffU) / 255.0f;
	blue = (float)(color & 0xffU) / 255.0f;
	high = red;
	if (green > high)
		high = green;
	if (blue > high)
		high = blue;
	low = red;
	if (green < low)
		low = green;
	if (blue < low)
		low = blue;
	span = high - low;

	/* Black and the greys have no hue. */
	*saturation = 0.0f;
	if (high <= 0.0f || span <= 0.0f)
		return 0.0f;
	*saturation = span / high;

	/* The hue by the highest channel. */
	if (high == red) {
		hue = 60.0f * ((green - blue) / span);
	} else if (high == green) {
		hue = 60.0f * ((blue - red) / span) + 120.0f;
	} else {
		hue = 60.0f * ((red - green) / span) + 240.0f;
	}
	if (hue < 0.0f)
		hue += 360.0f;

	/* The hue. */
	return hue;
}
