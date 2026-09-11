/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/*
 * Full production input.c contains cdev operation tables retained by ASan.
 * Narrow keymap/subscriber fixtures never invoke those device operations.
 * Strong device-fixture implementations override these fail-fast sentinels.
 */
#include <kern/kmem.h>
#include <kern/uaccess.h>
#include <kern/waitq.h>
#include <kern/poll.h>
#include <stdlib.h>

__attribute__((weak)) void *kern_calloc(size_t count, size_t size)
{ (void)count; (void)size; abort(); }
__attribute__((weak)) void kern_free(void *pointer)
{ (void)pointer; abort(); }
__attribute__((weak)) uint64_t waitq_sequence(const struct wait_queue *queue)
{ (void)queue; abort(); }
__attribute__((weak)) int waitq_sleep(struct wait_queue *queue,
    struct spinlock *lock, uint64_t observed, uint64_t deadline, unsigned flags)
{ (void)queue; (void)lock; (void)observed; (void)deadline; (void)flags; abort(); }
__attribute__((weak)) void waitq_wake_all(struct wait_queue *queue)
{ (void)queue; abort(); }
__attribute__((weak)) int user_address_add(uintptr_t address, size_t delta,
    uintptr_t *result)
{ (void)address; (void)delta; (void)result; abort(); }
__attribute__((weak)) int copyin(uintptr_t source, void *destination, size_t size)
{ (void)source; (void)destination; (void)size; abort(); }
__attribute__((weak)) int copyout(const void *source, uintptr_t destination,
    size_t size)
{ (void)source; (void)destination; (void)size; abort(); }
__attribute__((weak)) void poll_notify(void)
{ abort(); }
__attribute__((weak)) unsigned long spin_lock_irqsave(struct spinlock *lock)
{ (void)lock; abort(); }
__attribute__((weak)) void spin_unlock_irqrestore(struct spinlock *lock,
    unsigned long enabled)
{ (void)lock; (void)enabled; abort(); }
