/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The appearance on Windows: the apps' light or dark mode the user chose
 * (HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Themes\
 * Personalize, the DWORD AppsUseLightTheme; light when it is missing),
 * read when the application opens.  It chooses libkeiland's light or dark
 * theme (with the blue accent) and the frames' dark title bars
 * (DWMWA_USE_IMMERSIVE_DARK_MODE, window.c).  A change of the setting
 * while the program runs is not followed.
 */

#include "userland/desktop/libkeiland/ui/internal.h"
#include "userland/desktop/libkeiland/win32/window-win32.h"

/* The key and the value of the apps' mode. */
#define THEME_KEY	L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"
#define THEME_VALUE	L"AppsUseLightTheme"

/*
 * The appearance read last (KL_APPEARANCE_*), and the accent; written on
 * the application's thread when it opens, read by any thread after.
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
 * Reports the accent: blue (the Windows backend does not read the system's
 * accent yet).
 */
unsigned
kl_accent_get(void)
{
	/* The accent. */
	return theme_accent;
}

/*
 * Reads the apps' mode and makes libkeiland's theme follow it.  Returns 1
 * for the dark mode, 0 for the light one.
 */
int
keiui_win32_dark(void)
{
	DWORD value;
	DWORD size;
	LSTATUS status;

	/* The setting; missing or unreadable is light. */
	value = 1U;
	size = sizeof(value);
	status = RegGetValueW(HKEY_CURRENT_USER, THEME_KEY, THEME_VALUE, RRF_RT_REG_DWORD, NULL, &value, &size);
	theme_appearance = KL_APPEARANCE_LIGHT;
	if (status == ERROR_SUCCESS && value == 0U)
		theme_appearance = KL_APPEARANCE_DARK;

	/* libkeiland's theme of the appearance. */
	keiui_theme_set(theme_appearance, theme_accent);

	/* The mode. */
	if (theme_appearance == KL_APPEARANCE_DARK)
		return 1;
	return 0;
}
