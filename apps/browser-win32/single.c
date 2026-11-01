/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * One OmniGrammaton at a time.  The first start holds the named mutex
 * "Local\OmniGrammaton-SingleInstance" for as long as it runs.  A later
 * start finds it held, looks for the first one's window (a libkeiland
 * frame, class "KeilandWindow", with the property "OmniGrammaton") and
 * sends it the URLs with WM_COPYDATA (dwData 0x4F474D31: the URLs in
 * UTF-8, each ended by a NUL), brings it to the front, and ends.
 */

#include "apps/browser-win32/single.h"

#include <windows.h>

#include <stdlib.h>
#include <string.h>

/* The mutex, the window's class and property, and the data's kind. */
#define SINGLE_MUTEX		L"Local\\OmniGrammaton-SingleInstance"
#define SINGLE_CLASS		L"KeilandWindow"
#define SINGLE_PROPERTY		L"OmniGrammaton"
#define SINGLE_URLS		0x4F474D31U

/* How long a window may take to answer, in milliseconds. */
#define SINGLE_TIMEOUT_MS	5000U

/* The longest path a file's word is made absolute in. */
#define SINGLE_PATH_MAX		32768U

static HWND single_window(void);
static char *single_urls(int argc, char **argv, size_t *length);
static char *single_absolute(const char *word);

/*
 * The mutex, held until the program ends (the system closes it); NULL
 * before single_start.  Only the program's main thread touches it.
 */
static HANDLE single_mutex;

/*
 * Takes the program's place, or hands the URLs to the program already
 * running.
 */
int
single_start(
	int argc,
	char **argv)
{
	COPYDATASTRUCT copy;
	DWORD_PTR answer;
	LRESULT sent;
	DWORD error;
	HWND window;
	char *urls;
	size_t length;

	/* The program's place. */
	single_mutex = CreateMutexW(NULL, FALSE, SINGLE_MUTEX);
	error = GetLastError();
	if (single_mutex == NULL || error != ERROR_ALREADY_EXISTS)
		return 0;

	/* The first start's window; without one this start goes on by itself. */
	window = single_window();
	if (window == NULL)
		return 0;

	/* The URLs, handed over. */
	urls = single_urls(argc, argv, &length);
	if (urls == NULL)
		return 0;
	memset(&copy, 0, sizeof(copy));
	copy.dwData = SINGLE_URLS;
	copy.cbData = (DWORD)length;
	copy.lpData = urls;
	answer = 0U;
	sent = SendMessageTimeoutW(window, WM_COPYDATA, 0, (LPARAM)&copy, SMTO_ABORTIFHUNG, SINGLE_TIMEOUT_MS, &answer);
	free(urls);
	if (sent == 0 || answer == 0U)
		return 0;

	/* The first start's window in front. */
	(void)SetForegroundWindow(window);

	/* Succeeded: handed over. */
	return 1;
}

/* Finds a libkeiland frame with the program's property; NULL when there is none. */
static HWND
single_window(void)
{
	HWND window;
	HANDLE mark;

	/* Each window of the class, until one has the property. */
	window = NULL;
	for (;;) {
		window = FindWindowExW(NULL, window, SINGLE_CLASS, NULL);
		if (window == NULL)
			break;
		mark = GetPropW(window, SINGLE_PROPERTY);
		if (mark != NULL)
			return window;
	}

	/* None. */
	return NULL;
}

/* The words that are not options, each ended by a NUL (a file's path made absolute); NULL without memory. */
static char *
single_urls(
	int argc,
	char **argv,
	size_t *length)
{
	char *urls;
	char *grown;
	char *word;
	size_t size;
	size_t taken;
	int index;

	/* Nothing yet (an empty list is one byte, its NUL). */
	urls = malloc(1U);
	if (urls == NULL)
		return NULL;
	urls[0] = '\0';
	size = 0U;

	/* Each word that is not an option. */
	for (index = 1; index < argc; index++) {
		if (argv[index][0] == '-')
			continue;
		word = single_absolute(argv[index]);
		if (word == NULL) {
			free(urls);
			return NULL;
		}
		taken = strlen(word) + 1U;
		grown = realloc(urls, size + taken + 1U);
		if (grown == NULL) {
			free(word);
			free(urls);
			return NULL;
		}
		urls = grown;
		memcpy(urls + size, word, taken);
		size += taken;
		urls[size] = '\0';
		free(word);
	}

	/* Succeeded: the list and its length. */
	*length = size;
	if (size == 0U)
		*length = 1U;
	return urls;
}

/* A word as it is, or a file's path that exists made absolute; NULL without memory. */
static char *
single_absolute(
	const char *word)
{
	WCHAR *wide;
	WCHAR *full;
	DWORD attributes;
	DWORD made;
	char *text;
	int units;
	int bytes;

	/* The word in UTF-16. */
	units = MultiByteToWideChar(CP_UTF8, 0U, word, -1, NULL, 0);
	wide = calloc((size_t)units + 1U, sizeof(*wide));
	full = calloc(SINGLE_PATH_MAX, sizeof(*full));
	if (wide == NULL || full == NULL || units == 0) {
		free(wide);
		free(full);
		return NULL;
	}
	(void)MultiByteToWideChar(CP_UTF8, 0U, word, -1, wide, units);

	/* A file that exists: its full path. */
	made = 0U;
	attributes = GetFileAttributesW(wide);
	if (attributes != INVALID_FILE_ATTRIBUTES)
		made = GetFullPathNameW(wide, SINGLE_PATH_MAX, full, NULL);
	if (made == 0U || made >= SINGLE_PATH_MAX)
		memcpy(full, wide, ((size_t)units) * sizeof(*wide));
	free(wide);

	/* Back to UTF-8. */
	bytes = WideCharToMultiByte(CP_UTF8, 0U, full, -1, NULL, 0, NULL, NULL);
	text = calloc((size_t)bytes + 1U, 1U);
	if (text == NULL || bytes == 0) {
		free(text);
		free(full);
		return NULL;
	}
	(void)WideCharToMultiByte(CP_UTF8, 0U, full, -1, text, bytes, NULL, NULL);
	free(full);

	/* Succeeded. */
	return text;
}
