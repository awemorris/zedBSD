/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The appearance on Android: the system's light or dark mode, the accent
 * colour the system gives (taken as the nearest of libkeiland's eight
 * accents: graphite for a grey, otherwise the one of the nearest hue; blue
 * when the system gives none), and the language.  Java reads them when the
 * Activity is made and when the configuration changes, and tells them
 * through the bridge; a change is told to the program with a KL_APP_THEME
 * event, and a change of the language with a KL_APP_LANGUAGE event.
 */

#include "apps/libkeiland/android/android.h"
#include "userland/desktop/libkeiland/ui/internal.h"

#include <string.h>

/*
 * The saturation (0 to 1) below which the system's accent is a grey
 * (graphite).
 */
#define THEME_GREY	0.2f

static unsigned theme_accent_of(uint32_t color);
static float theme_hue(uint32_t color, float *saturation);

/*
 * The appearance (KL_APPEARANCE_*) and the accent (KL_ACCENT_*) told last;
 * written on M, read by any thread.
 */
static unsigned theme_appearance = KL_APPEARANCE_LIGHT;
static unsigned theme_accent = KL_ACCENT_BLUE;

/*
 * Reports the appearance told last (the argument is a Wayland watch, not
 * used here).
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
 * Reports the accent told last.
 */
unsigned
kl_accent_get(void)
{
	/* The accent. */
	return theme_accent;
}

/*
 * Takes what the system told (the mode, the accent and the language of its
 * configuration): libkeiland's theme follows, and the program is told of a
 * change of the colours (KL_APP_THEME) and of the language
 * (KL_APP_LANGUAGE).
 */
void
android_theme_bridge(
	struct kl_app *app,
	const struct keiland_bridge_event *event)
{
	struct kl_app_event posted;
	unsigned appearance;
	unsigned accent;
	char language[KL_TR_NAME_MAX + 1U];
	size_t length;
	int same;

	/* The mode and the accent; none given is blue. */
	appearance = event->a != 0 ? KL_APPEARANCE_DARK : KL_APPEARANCE_LIGHT;
	accent = KL_ACCENT_BLUE;
	if (event->b >= 0)
		accent = theme_accent_of((uint32_t)event->b);

	/* A change of the colours: libkeiland's theme, and the program. */
	if (appearance != theme_appearance || accent != theme_accent) {
		theme_appearance = appearance;
		theme_accent = accent;
		keiui_theme_set(theme_appearance, theme_accent);
		android_titlebar_restyle(app);
		memset(&posted, 0, sizeof(posted));
		posted.kind = KL_APP_THEME;
		(void)android_app_queue(app, &posted);
	}

	/* The language: the locale's first part ("ja-JP" is "ja"). */
	if (event->text == NULL || event->text[0] == '\0')
		return;
	length = strcspn(event->text, "-_");
	if (length == 0U || length > KL_TR_NAME_MAX)
		return;
	memcpy(language, event->text, length);
	language[length] = '\0';
	same = strcmp(language, app->language);
	if (same == 0)
		return;
	memcpy(app->language, language, length + 1U);
	memset(&posted, 0, sizeof(posted));
	posted.kind = KL_APP_LANGUAGE;
	memcpy(posted.language, language, length + 1U);
	(void)android_app_queue(app, &posted);
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

	/* Turns a negative hue into the same angle between 0 and 360. */
	if (hue < 0.0f)
		hue += 360.0f;

	/* The hue. */
	return hue;
}
