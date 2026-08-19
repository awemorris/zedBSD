/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_TTY_H
#define ZEDBSD_KERN_TTY_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct file;
struct process;
struct file_ops;

int tty_console_init(void);
void tty_console_input_event(uint32_t);
unsigned tty_vt_count(void);
unsigned tty_vt_active(void);
int tty_vt_activate(unsigned);
ssize_t tty_vt_read(unsigned, struct file *, void *, size_t);
ssize_t tty_vt_write(unsigned, struct file *, const void *, size_t);
int tty_vt_ioctl(unsigned, struct file *, unsigned long, uintptr_t);
int tty_vt_poll(unsigned, struct file *, short, short *);
ssize_t tty_console_read(struct file *, void *, size_t);
ssize_t tty_console_write(struct file *, const void *, size_t);
int tty_console_ioctl(struct file *, unsigned long, uintptr_t);
int tty_console_poll(struct file *, short, short *);
void tty_attach_console(struct process *);
void tty_detach_process(struct process *);
int tty_pty_register(void);
int tty_pty_exists(unsigned);
unsigned tty_pty_snapshot(unsigned *, unsigned);
extern const struct file_ops tty_pty_slave_file_ops;

#endif
