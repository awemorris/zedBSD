/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The harness's calls of the WMI layer and of methods with arguments.
 */

#ifndef AML_HOST_WMI_H
#define AML_HOST_WMI_H

int
host_wmi_attach(void);

int
host_wmi_attach_path(
	const char *path);

int
host_wmi_query(
	const char *request);

int
host_wmi_method(
	const char *request);

int
host_eval_args(
	const char *request);

#endif
