/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * OmniGrammaton.exe's entry (wWinMain, a program of the Windows
 * subsystem): the DLLs are looked for beside the program and in the
 * system's directory only, the command line is turned into UTF-8, a second
 * start hands its URLs to the first (single.c), and the browser's own
 * entry runs (browser_main, userland/desktop/browser/main.c).
 */

#include "apps/browser-win32/single.h"
#include "userland/desktop/browser/browser-main.h"

#include <windows.h>
#include <shellapi.h>

#include <stdlib.h>

static char **main_arguments(int count, WCHAR **wide);
static void main_arguments_free(int count, char **arguments);

/*
 * Runs the program.
 */
int WINAPI
wWinMain(
	HINSTANCE instance,
	HINSTANCE previous,
	PWSTR line,
	int show)
{
	WCHAR **wide;
	char **arguments;
	int count;
	int handed;
	int status;

	(void)instance;
	(void)previous;
	(void)line;
	(void)show;

	/* DLLs from beside the program and from the system only. */
	(void)SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);

	/* The command line's words, in UTF-8. */
	count = 0;
	wide = CommandLineToArgvW(GetCommandLineW(), &count);
	if (wide == NULL)
		return 1;
	arguments = main_arguments(count, wide);
	(void)LocalFree(wide);
	if (arguments == NULL)
		return 1;

	/* A second start hands its URLs over and ends. */
	handed = single_start(count, arguments);
	if (handed) {
		main_arguments_free(count, arguments);
		return 0;
	}

	/* The browser. */
	status = browser_main(count, arguments);
	main_arguments_free(count, arguments);

	/* Its exit status. */
	return status;
}

/* The words in UTF-8, with a NULL after the last; NULL without memory. */
static char **
main_arguments(
	int count,
	WCHAR **wide)
{
	char **arguments;
	int index;
	int bytes;

	/* The array. */
	arguments = calloc((size_t)count + 1U, sizeof(*arguments));
	if (arguments == NULL)
		return NULL;

	/* Each word. */
	for (index = 0; index < count; index++) {
		bytes = WideCharToMultiByte(CP_UTF8, 0U, wide[index], -1, NULL, 0, NULL, NULL);
		if (bytes == 0) {
			main_arguments_free(index, arguments);
			return NULL;
		}
		arguments[index] = malloc((size_t)bytes);
		if (arguments[index] == NULL) {
			main_arguments_free(index, arguments);
			return NULL;
		}
		(void)WideCharToMultiByte(CP_UTF8, 0U, wide[index], -1, arguments[index], bytes, NULL, NULL);
	}

	/* Succeeded. */
	return arguments;
}

/* Frees the words. */
static void
main_arguments_free(
	int count,
	char **arguments)
{
	int index;

	/* Each word, then the array. */
	for (index = 0; index < count; index++)
		free(arguments[index]);
	free(arguments);
}
