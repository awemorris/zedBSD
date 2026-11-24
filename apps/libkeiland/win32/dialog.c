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
	IFileOpenDialog *dialog;
	DWORD options;
	IShellItem *item;
	PWSTR wide_path;
	int converted;
	int status;

	if (path == NULL || size == 0U)
		return EINVAL;
	path[0] = '\0';

	hwnd = (owner != NULL) ? owner->frame : NULL;
	dialog = NULL;
	item = NULL;
	wide_path = NULL;
	com_initialized = 0;
	status = 0;

	hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	if (hr == S_OK || hr == S_FALSE) {
		com_initialized = 1;
	} else if (hr != RPC_E_CHANGED_MODE) {
		return EIO;
	}

	hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
	    &IID_IFileOpenDialog, (void **)&dialog);
	if (FAILED(hr) || dialog == NULL) {
		status = EIO;
		goto out;
	}

	options = 0U;
	hr = IFileOpenDialog_GetOptions(dialog, &options);
	if (SUCCEEDED(hr)) {
		options |= FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR | FOS_FORCEFILESYSTEM;
		(void)IFileOpenDialog_SetOptions(dialog, options);
	}

	hr = IFileOpenDialog_Show(dialog, hwnd);
	if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
		status = ECANCELED;
		goto out;
	}
	if (FAILED(hr)) {
		status = EIO;
		goto out;
	}

	hr = IFileOpenDialog_GetResult(dialog, &item);
	if (FAILED(hr) || item == NULL) {
		status = EIO;
		goto out;
	}

	hr = IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &wide_path);
	if (FAILED(hr) || wide_path == NULL) {
		status = EIO;
		goto out;
	}

	converted = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
	    wide_path, -1, path, (int)size, NULL, NULL);
	if (converted <= 0) {
		path[0] = '\0';
		status = (GetLastError() == ERROR_INSUFFICIENT_BUFFER) ? ENAMETOOLONG : EINVAL;
	}

out:
	if (wide_path != NULL)
		CoTaskMemFree(wide_path);
	if (item != NULL)
		IShellItem_Release(item);
	if (dialog != NULL)
		IFileOpenDialog_Release(dialog);
	if (com_initialized)
		CoUninitialize();

	return status;
}
