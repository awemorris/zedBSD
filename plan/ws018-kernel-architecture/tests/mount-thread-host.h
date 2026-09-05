/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef MOUNT_THREAD_HOST_H
#define MOUNT_THREAD_HOST_H
void *host_thread_start(void (*run)(void *), void *argument);
void host_thread_join(void *thread);
void host_thread_yield(void);
void host_gate_reset(unsigned gate);
void host_gate_signal(unsigned gate);
void host_gate_pause(unsigned gate);
void host_gate_wait(unsigned gate);
void host_gate_release(unsigned gate);
#endif
