/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef KERN_KERN_TTY_H
#define KERN_KERN_TTY_H

#include <stddef.h>
#include <stdint.h>
#include <uapi/types.h>

struct file;
struct process;
struct file_ops;

int
tty_console_init(void);

void
tty_console_input_event(
	uint32_t event);

/*
 * Holds the keyboard away from the console while a graphical display owns
 * the screen: a GPU display lease holds it from its claim to its
 * release, so what is typed into the display (a password at the graphical
 * login) is not also read, or echoed, by the text console.  Holds nest.
 * The serial console's input is not held.
 */
void
tty_console_input_hold(void);

void
tty_console_input_unhold(void);

/*
 * Feeds one character to the active console.
 *
 * A keyboard reports which key moved and the console works out what that
 * means; a serial line carries the character itself, already decided by the
 * terminal at the other end.  This is the way in for the second kind, and
 * it runs the same line discipline the first kind ends up in, so that what
 * arrives over a cable and what arrives from a keyboard are read alike.
 *
 * Safe to call from an interrupt.
 */
void
tty_console_input_byte(
	uint8_t byte);

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

/*
 * The controlling terminal, which /dev/tty names.
 *
 * A process without one gets ENXIO rather than a descriptor that never
 * answers, so that a caller can fall back to standard input.
 */
int
tty_controlling_open(
	struct file *file);

ssize_t
tty_controlling_read(
	struct file *file,
	void *buffer,
	size_t size);

ssize_t
tty_controlling_write(
	struct file *file,
	const void *buffer,
	size_t size);

int
tty_controlling_ioctl(
	struct file *file,
	unsigned long request,
	uintptr_t argument);

int
tty_controlling_poll(
	struct file *file,
	short events,
	short *revents);

/*
 * The most pseudo terminals at once.  A desktop with many Terminal windows
 * and tabs, each with an SSH session, needs more than 8, or a new Terminal
 * fails with ENOSPC.  Each pair is about 7 KiB of the
 * kernel's static memory, so the 32-bit boards with little memory (PC-98,
 * X68000, i386) keep fewer.
 */
#ifndef TTY_PTY_MAX
#if defined(KERN_USER_ABI_LP64)
#define TTY_PTY_MAX 64U
#else
#define TTY_PTY_MAX 16U
#endif
#endif

int
tty_pty_register(void);

int
tty_pty_exists(
	unsigned index);

unsigned
tty_pty_snapshot(
	unsigned *indices,
	unsigned capacity);

/*
 * Who a pseudo terminal belongs to.  The node under /dev/pts is made afresh
 * on every lookup, so its ownership is kept with the terminal and read back
 * through these rather than stored in the node.
 */
int
tty_pty_attr_get(
	unsigned index,
	uid_t *uid,
	gid_t *gid,
	mode_t *mode);

int
tty_pty_attr_set(
	unsigned index,
	const uid_t *uid,
	const gid_t *gid,
	const mode_t *mode);

extern const struct file_ops tty_pty_slave_file_ops;

#ifdef KERN_TTY_TEST
int
tty_test_vlnext_ixon(void);
#endif

#endif
