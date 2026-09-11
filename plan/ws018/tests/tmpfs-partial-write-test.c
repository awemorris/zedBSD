/* Production tmpfs partial-write/EOF regression. Memory-only fixture.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <stdio.h>
#include <stdlib.h>
#include "kern/tmpfs.c"
static unsigned checks, allocations, fail_allocation, reservations, fail_reservation;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "tmpfs line %d: %s\n", __LINE__, #x); abort(); } } while (0)
void mutex_lock(struct mutex *lock) { CHECK(!lock->locked); lock->locked = 1; }
void mutex_unlock(struct mutex *lock) { CHECK(lock->locked); lock->locked = 0; }
void *kern_calloc(size_t n, size_t size)
{ if (++allocations == fail_allocation) return NULL; return calloc(n, size); }
void kern_free(void *pointer) { free(pointer); }
int vm_commit_reserve(size_t size)
{ CHECK(size == ZEDBSD_PAGE_SIZE); return ++reservations == fail_reservation ? ENOMEM : 0; }
void vm_commit_release(size_t size) { CHECK(size == ZEDBSD_PAGE_SIZE); }
int main(void)
{
 unsigned char input[8192], output[8192];
 memset(input, 0x5a, sizeof(input));
 for (unsigned failure = 0; failure < 3; failure++) {
  struct tmpfs_state state = { .max_bytes = failure == 0 ? 4096 : 8192 };
  struct tmpfs_node node = { .state = &state };
  struct inode inode = { .i_type = INODE_REG, .i_data = &node };
  struct file file = { .f_inode = &inode };
  allocations = reservations = 0;
  fail_allocation = failure == 1 ? 2 : 0;
  fail_reservation = failure == 2 ? 2 : 0;
  CHECK(tmpfs_write_at(&inode, input, sizeof(input), 0, 0) == 4096);
  CHECK(inode.i_size == 4096 && state.used_bytes == 4096);
  CHECK(tmpfs_pread(&file, output, sizeof(output), 0) == 4096);
  CHECK(memcmp(input, output, 4096) == 0);
  CHECK(tmpfs_write_at(&inode, input, 0, 16384, 0) == 0);
  CHECK(inode.i_size == 4096);
  CHECK(tmpfs_truncate(&inode, 0) == 0);
  CHECK(state.used_bytes == 0 && node.allocated_pages == 0);
 }
 printf("tmpfs partial write/EOF: PASS (%u checks)\n", checks);
 return 0;
}
