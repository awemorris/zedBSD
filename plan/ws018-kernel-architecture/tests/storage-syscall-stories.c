/* Harness for verbatim production syscall functions extracted by the runner.
 * Filesystem/user-copy boundaries are deterministic. SPDX-License-Identifier: Zlib */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define SYSCALL_IO_CHUNK 512U
#define SYSCALL_EXT
#define INODE_REG 1
#define INODE_BLOCK 2
#define HAL_SPACE_READ 1
#define HAL_SPACE_WRITE 2
#define RLIMIT_FSIZE 1
#define SIGXFSZ 25
#define FILE_IO_READ 1
#define FILE_IO_WRITE 2
struct inode { int i_type; };
struct file { struct inode *f_inode; size_t offset; };
struct process { void *fd, *cred; };
struct file_io { struct file *file; int kind; };
struct uaccess_pin { unsigned char *base; };
static struct inode inode = {INODE_REG};
static struct file file = {&inode, 0};
static struct process process;
static unsigned char medium[65536], bytes[65536];
static unsigned calls, copy_calls, fail_copy, fail_alloc, held, allocations, pins;
static void *curthread;
static struct process *current_process(void) { return &process; }
static struct file *filedesc_get_ref(void *fd, int n) { (void)fd; (void)n; return &file; }
static int file_close(struct file *f) { (void)f; return 0; }
static void *kern_malloc(size_t n) { if (fail_alloc) return NULL; allocations++; return malloc(n); }
static void kern_free(void *p) { assert(allocations); allocations--; free(p); }
static int uaccess_pin(uintptr_t base, size_t n, unsigned access, struct uaccess_pin *p)
{ (void)n; (void)access; p->base = (void *)base; pins++; return 0; }
static void uaccess_unpin(struct uaccess_pin *p) { (void)p; assert(pins); pins--; }
static int file_io_begin(struct file *f, int kind, int x, int y, struct file_io *io)
{ (void)x; (void)y; assert(!held); held = 1; io->file = f; io->kind = kind; return 0; }
static int file_io_begin_cred(struct file *f, int kind, int x, int y, void *cred, struct file_io *io)
{ (void)cred; return file_io_begin(f,kind,x,y,io); }
static void file_io_end(struct file_io *io) { (void)io; assert(held); held = 0; }
static void file_io_set_growth_limit(struct file_io *io, uint64_t n) { (void)io; (void)n; }
static uint64_t resource_limit_current(struct process *p, int n) { (void)p; (void)n; return UINT64_MAX; }
static int file_io_take_growth_limit_hit(struct file_io *io) { (void)io; return 0; }
static int signal_send_thread(void *t, int n) { (void)t; (void)n; return 0; }
static ssize_t file_io_transfer(struct file_io *io, void *buffer, size_t n)
{
	assert(held && file.offset + n <= sizeof(medium)); calls++;
	if (io->kind == FILE_IO_WRITE) memcpy(medium + file.offset, buffer, n);
	else memcpy(buffer, medium + file.offset, n);
	file.offset += n; return n;
}
static int copyin_pinned(struct uaccess_pin *p, size_t at, void *buffer, size_t n)
{ if (++copy_calls == fail_copy) return EFAULT; memcpy(buffer,p->base+at,n); return 0; }
static int copyout_pinned(struct uaccess_pin *p, size_t at, void *buffer, size_t n)
{ memcpy(p->base+at,buffer,n); return 0; }
#include "storage-syscall-extracted.h"

int main(void)
{
	uintptr_t args[6] = {0,(uintptr_t)bytes,sizeof(bytes),0,0,0};
	memset(bytes,0x52,sizeof(bytes));
	assert(sys_write_call(args) == sizeof(bytes));
	unsigned writes = calls;
	file.offset = 0; calls = 0; memset(bytes,0,sizeof(bytes));
	assert(sys_read_call(args) == sizeof(bytes) && bytes[65535] == 0x52);
	assert(calls == 65536 / ZEDBSD_SYSCALL_REGULAR_CHUNK && writes == calls);
	printf("S44 CELL chunk=%u IMOD=%u backend_write=%u backend_read=%u IRQ=unmeasured\n",
	    ZEDBSD_SYSCALL_REGULAR_CHUNK, ZEDBSD_XHCI_IMOD, writes, calls);
	file.offset = 0; copy_calls = 0; fail_copy = 3;
	assert(sys_write_call(args) == 2 * ZEDBSD_SYSCALL_REGULAR_CHUNK);
	assert(file.offset == 2 * ZEDBSD_SYSCALL_REGULAR_CHUNK && !held && !pins && !allocations);
	fail_copy = 0; file.offset = 0; assert(sys_write_call(args) == sizeof(bytes));
	puts("S41 PASS injected third user copy fault returns partial length and releases lease");
	fail_alloc = 1; file.offset = 0; calls = 0;
	assert(sys_write_call(args) == sizeof(bytes) && calls == 128);
	fail_alloc = 0; file.offset = 0; assert(sys_read_call(args) == sizeof(bytes));
	assert(!held && !pins && !allocations);
	puts("S42 PASS allocation pressure falls back to small buffer and next I/O succeeds");
	return 0;
}
