/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_TTY_H
#define ZEDBSD_KERN_TTY_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct file;
struct process;
struct file_ops;

int
tty_console_init(void);

void
tty_console_input_event(
	uint32_t event);

unsigned
tty_vt_count(void);

unsigned
tty_vt_active(void);

int
tty_vt_activate(
	unsigned vt);

ssize_t
tty_vt_read(
	unsigned vt,
	struct file *file,
	void *buffer,
	size_t size);

ssize_t
tty_vt_write(
	unsigned vt,
	struct file *file,
	const void *buffer,
	size_t size);

int
tty_vt_ioctl(
	unsigned vt,
	struct file *file,
	unsigned long request,
	uintptr_t argument);

int
tty_vt_poll(
	unsigned vt,
	struct file *file,
	short events,
	short *revents);

ssize_t
tty_console_read(
	struct file *f,
	void *b,
	size_t n);

ssize_t
tty_console_write(
	struct file *f,
	const void *b,
	size_t n);

int
tty_console_ioctl(
	struct file *file,
	unsigned long request,
	uintptr_t argument);

int
tty_console_poll(
	struct file *file,
	short events,
	short *revents);

void
tty_attach_console(
	struct process *process);

void
tty_detach_process(
	struct process *process);

int
tty_pty_register(void);

int
tty_pty_exists(
	unsigned index);

unsigned
tty_pty_snapshot(
	unsigned *indices,
	unsigned capacity);

extern const struct file_ops tty_pty_slave_file_ops;

#ifdef ZEDBSD_TTY_TEST
int
tty_test_vlnext_ixon(void);
#endif

#endif
