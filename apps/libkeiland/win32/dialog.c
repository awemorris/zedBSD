/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Windows file-open dialog (IFileOpenDialog) for libkeiland.
 */

#define COBJMACROS

#include "apps/libkeiland/win32/window-win32.h"

#include <shobjidl.h>

#include <errno.h>
#include <stddef.h>

static int dialog_run(HWND hwnd, char *path, size_t size);
static int dialog_read_item(IFileOpenDialog *dialog, char *path, size_t size);

/*
 * Shows a modal Windows File Open dialog owned by owner and writes the
 * selected UTF-8 filesystem path into path (up to size bytes).
 *
 * Returns 0 on selection, ECANCELED when cancelled, or an errno value.
 */
int
kl_file_open_dialog(
	struct kl_window *owner,
	char *path,
	size_t size)
{
	HWND hwnd;
	HRESULT hr;
	int com_initialized;
	int status;

	/* Validates output buffer before touching COM. */
	if (path == NULL || size == 0U)
		return EINVAL;
	path[0] = '\0';

	/* Resolves the optional owner frame handle. */
	hwnd = NULL;
	if (owner != NULL)
		hwnd = owner->frame;

	/* Initializes STA COM for the Shell item dialog. */
	com_initialized = 0;
	hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	if (hr == S_OK || hr == S_FALSE) {
		com_initialized = 1;
	} else if (hr != RPC_E_CHANGED_MODE) {
		return EIO;
	}

	/* Runs the modal dialog and converts the chosen path. */
	status = dialog_run(hwnd, path, size);
	if (com_initialized)
		CoUninitialize();

	/* Returns 0, ECANCELED, or an errno value. */
	return status;
}

/*
 * Creates, configures, and shows IFileOpenDialog, returning the selected path.
 */
static int
dialog_run(
	HWND hwnd,
	char *path,
	size_t size)
{
	IFileOpenDialog *dialog;
	DWORD options;
	HRESULT hr;
	HRESULT cancelled;
	int failed;
	int succeeded;
	int status;

	/* Creates the FileOpenDialog COM object. */
	dialog = NULL;
	hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
	    &IID_IFileOpenDialog, (void **)&dialog);
	failed = FAILED(hr);
	if (failed || dialog == NULL)
		return EIO;

	/* Restricts selection to existing filesystem files without changing cwd. */
	options = 0U;
	hr = IFileOpenDialog_GetOptions(dialog, &options);
	succeeded = SUCCEEDED(hr);
	if (succeeded) {
		options |= FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR | FOS_FORCEFILESYSTEM;
		(void)IFileOpenDialog_SetOptions(dialog, options);
	}

	/* Displays the modal dialog and checks for user cancellation. */
	hr = IFileOpenDialog_Show(dialog, hwnd);
	cancelled = HRESULT_FROM_WIN32(ERROR_CANCELLED);
	if (hr == cancelled) {
		IFileOpenDialog_Release(dialog);
		return ECANCELED;
	}

	/* Checks whether showing the dialog failed. */
	failed = FAILED(hr);
	if (failed) {
		IFileOpenDialog_Release(dialog);
		return EIO;
	}

	/* Extracts the selected UTF-8 path and releases the dialog. */
	status = dialog_read_item(dialog, path, size);
	IFileOpenDialog_Release(dialog);
	return status;
}

/*
 * Reads the chosen IShellItem from dialog and converts its path to UTF-8.
 */
static int
dialog_read_item(
	IFileOpenDialog *dialog,
	char *path,
	size_t size)
{
	IShellItem *item;
	PWSTR wide_path;
	HRESULT hr;
	int failed;
	int converted;
	DWORD last;

	/* Obtains the selected shell item. */
	item = NULL;
	hr = IFileOpenDialog_GetResult(dialog, &item);
	failed = FAILED(hr);
	if (failed || item == NULL)
		return EIO;

	/* Queries the Win32 filesystem path in UTF-16. */
	wide_path = NULL;
	hr = IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &wide_path);
	failed = FAILED(hr);
	if (failed || wide_path == NULL) {
		IShellItem_Release(item);
		return EIO;
	}

	/* Converts the UTF-16 path to UTF-8 and releases COM resources. */
	converted = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
	    wide_path, -1, path, (int)size, NULL, NULL);
	CoTaskMemFree(wide_path);
	IShellItem_Release(item);
	if (converted <= 0) {
		path[0] = '\0';
		last = GetLastError();
		if (last == ERROR_INSUFFICIENT_BUFFER)
			return ENAMETOOLONG;
		return EINVAL;
	}

	/* Succeeded. */
	return 0;
}
