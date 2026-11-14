/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A small program for a check under Wine that changes the apps' mode of
 * the current user while a program runs, as the Settings of Windows do:
 *   theme-set light|dark
 * It writes AppsUseLightTheme (1 for light, 0 for dark) and tells every
 * top-level window that the colours changed (WM_SETTINGCHANGE with
 * "ImmersiveColorSet").  It prints one line, "theme-set: light" or
 * "theme-set: dark", or why it failed; the exit status is 0 when it did
 * both.
 */

#include <windows.h>

#include <stdio.h>
#include <string.h>

/* The key and the value of the apps' mode. */
#define THEME_KEY	L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"
#define THEME_VALUE	L"AppsUseLightTheme"

/* How long each window may take to answer the broadcast, in milliseconds. */
#define THEME_ANSWER_MS	5000U

/*
 * Reads the mode asked for, writes it and tells the windows.
 */
int
main(
	int argc,
	char **argv)
{
	DWORD value;
	DWORD_PTR answer;
	LSTATUS status;
	LRESULT sent;
	const char *name;
	int differs;
	int light;

	/* The mode: light or dark. */
	if (argc != 2) {
		fprintf(stderr, "usage: theme-set light|dark\n");
		return 2;
	}

	/* The theme asked for: dark unless the name is light. */
	light = 0;
	name = "dark";
	differs = strcmp(argv[1], "light");
	if (differs == 0) {
		light = 1;
		name = "light";
	} else {
		differs = strcmp(argv[1], "dark");
		if (differs != 0) {
			fprintf(stderr, "usage: theme-set light|dark\n");
			return 2;
		}
	}

	/* The value of the current user. */
	value = 0U;
	if (light)
		value = 1U;
	status = RegSetKeyValueW(HKEY_CURRENT_USER, THEME_KEY, THEME_VALUE, REG_DWORD, &value, sizeof(value));
	if (status != ERROR_SUCCESS) {
		printf("theme-set: cannot write the value (%ld)\n", (long)status);
		return 1;
	}

	/* Every top-level window hears of it. */
	answer = 0;
	sent = SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet",
	    SMTO_ABORTIFHUNG, THEME_ANSWER_MS, &answer);
	if (sent == 0) {
		printf("theme-set: the broadcast failed (%lu)\n", (unsigned long)GetLastError());
		return 1;
	}

	/* Succeeded: the mode written and told. */
	printf("theme-set: %s\n", name);
	return 0;
}
