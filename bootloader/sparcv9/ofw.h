/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Minimal IEEE 1275 client interface used by the SPARC V9 loaders.
 */

#ifndef BOOTLOADER_SPARCV9_OFW_H
#define BOOTLOADER_SPARCV9_OFW_H

/* One cell of the client interface's argument array, and its signed view. */
typedef unsigned long ofw_cell_t;
typedef long ofw_scell_t;

/* The firmware's client interface entry, called with the argument array. */
typedef ofw_cell_t (*ofw_client_t)(ofw_cell_t *);

void
ofw_init(
	ofw_client_t client);

ofw_scell_t
ofw_finddevice(
	const char *path);

ofw_scell_t
ofw_child(
	ofw_cell_t node);

ofw_scell_t
ofw_peer(
	ofw_cell_t node);

ofw_scell_t
ofw_parent(
	ofw_cell_t node);

int
ofw_getprop(
	ofw_cell_t node,
	const char *name,
	void *buffer,
	unsigned long size);

ofw_scell_t
ofw_instance_to_package(
	ofw_cell_t handle);

int
ofw_call_method(
	const char *method,
	ofw_cell_t handle,
	unsigned int argument_count,
	const ofw_cell_t *arguments,
	unsigned int result_count,
	ofw_cell_t *results);

int
ofw_translate(
	unsigned long virtual_address,
	unsigned long long *physical);

ofw_scell_t
ofw_open(
	const char *path);

int
ofw_close(
	ofw_cell_t handle);

long
ofw_read(
	ofw_cell_t handle,
	void *buffer,
	unsigned long size);

int
ofw_seek(
	ofw_cell_t handle,
	unsigned long long offset);

void *
ofw_claim(
	void *address,
	unsigned long size,
	unsigned long align);

void *
ofw_claim_fixed(
	void *address,
	unsigned long long physical,
	unsigned long size);

long
ofw_write(
	ofw_cell_t handle,
	const void *buffer,
	unsigned long size);

void
ofw_puts(
	const char *text);

ofw_cell_t
ofw_stdout_handle(
	void);

int
ofw_bootpath(
	char *buffer,
	unsigned long size);

void
ofw_exit(
	void)
__attribute__((noreturn));

#endif
