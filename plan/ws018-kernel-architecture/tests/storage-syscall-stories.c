/* Harness for verbatim production syscall functions extracted by the runner.
 * Filesystem/user-copy boundaries are deterministic. SPDX-License-Identifier: Zlib */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "kern/io-stats.h"
#include "kern/io-pool.h"
#define ZEDBSD_SYSCALL_REGULAR_CHUNK KERN_IO_BATCH_MAX
#include "storage-syscall-config.h"
#ifndef SSIZE_MAX
#define SSIZE_MAX INTPTR_MAX
#endif
#define KERN_PIPE_BUF 512U
#define SYSCALL_EXT
#define INODE_REG 1
#define INODE_BLOCK 2
#define HAL_SPACE_READ 1
#define HAL_SPACE_WRITE 2
#define RLIMIT_FSIZE 1
#define SIGXFSZ 25
enum file_io_kind { FILE_IO_READ = 1, FILE_IO_WRITE, FILE_IO_PREAD, FILE_IO_PWRITE };
struct ucred;
struct syscall_iovec { uint64_t base, length; };
struct inode { int i_type; };
struct file { struct inode *f_inode; size_t offset; };
struct process { void *fd, *cred; };
struct file_io { struct file *file; int kind; size_t offset; };
struct uaccess_pin { unsigned char *base; };
static struct inode inode = {INODE_REG};
static struct file file = {&inode, 0};
static struct process process;
static unsigned char medium[1024 * 1024], bytes[1024 * 1024];
static unsigned calls, copy_calls, fail_copy, fail_alloc, held, allocations, pins;
static size_t alloc_ceiling = SIZE_MAX, allocated_size, short_count = SIZE_MAX;
static unsigned fail_backend, short_backend, pipe_mode, limit_hit, signals;
static int begin_error;
static void *curthread;
static struct process *current_process(void) { return &process; }
static struct file *filedesc_get_ref(void *fd, int n) { (void)fd; (void)n; return &file; }
static int file_close(struct file *f) { (void)f; return 0; }
/* This syscall-boundary model controls reserve availability; the WS025 pool
 * fixture separately tests the production owner and absence of warm allocation. */
void *io_pool_borrow(size_t wanted, size_t *capacity)
{
	void *p;
	size_t n = wanted <= KERN_IO_SMALL_SIZE ? KERN_IO_SMALL_SIZE : KERN_IO_BATCH_MAX;
	assert(!held);
	if (fail_alloc) return NULL;
	if (n > alloc_ceiling) n = KERN_IO_SMALL_SIZE;
	if (n > alloc_ceiling) return NULL;
	p = malloc(n);
	if (p) { allocations++; allocated_size = n; *capacity = n; }
	return p;
}
void io_pool_release(void *p)
{ assert(allocations); allocations--; free(p); }
static int uaccess_pin(uintptr_t base, size_t n, unsigned access, struct uaccess_pin *p)
{ (void)n; (void)access; p->base = (void *)base; pins++; return 0; }
static void uaccess_unpin(struct uaccess_pin *p) { (void)p; assert(pins); pins--; }
static int file_io_begin(struct file *f, int kind, off_t x, int y, struct file_io *io)
{ (void)y; assert(!held); if (begin_error) return begin_error; held = 1; io->file = f; io->kind = kind; io->offset = kind >= FILE_IO_PREAD ? (size_t)x : f->offset; return 0; }
static int file_io_begin_cred(struct file *f, int kind, off_t x, int y, void *cred, struct file_io *io)
{ (void)cred; return file_io_begin(f,kind,x,y,io); }
static void file_io_end(struct file_io *io) { (void)io; assert(held); held = 0; }
static int completion_error;
static unsigned completions;
static ssize_t file_io_complete(struct file_io *io, ssize_t result)
{
 file_io_end(io);completions++;
 return completion_error && result>=0 ? -completion_error : result;
}
static void file_io_set_growth_limit(struct file_io *io, uint64_t n) { (void)io; (void)n; }
static uint64_t resource_limit_current(struct process *p, int n) { (void)p; (void)n; return UINT64_MAX; }
static int file_io_take_growth_limit_hit(struct file_io *io) { (void)io; return limit_hit; }
static int signal_send_thread(void *t, int n) { (void)t; (void)n; signals++; return 0; }
static int pipe_file_is_pipe(struct file *f) { (void)f; return pipe_mode; }
static int copyin(uintptr_t p, void *b, size_t n) { memcpy(b, (void *)p, n); return 0; }
static ssize_t file_io_transfer(struct file_io *io, void *buffer, size_t n)
{
	assert(held && io->offset + n <= sizeof(medium)); calls++;
	if (calls == fail_backend) return -EIO;
	if (calls == short_backend && n > short_count) n = short_count;
	if (io->kind == FILE_IO_WRITE || io->kind == FILE_IO_PWRITE) memcpy(medium + io->offset, buffer, n);
	else memcpy(buffer, medium + io->offset, n);
	io->offset += n;
	if (io->kind < FILE_IO_PREAD) file.offset = io->offset;
	return n;
}
static int copyin_pinned(struct uaccess_pin *p, size_t at, void *buffer, size_t n)
{ if (++copy_calls == fail_copy) return EFAULT; memcpy(buffer,p->base+at,n); return 0; }
static int copyout_pinned(struct uaccess_pin *p, size_t at, void *buffer, size_t n)
{ if (++copy_calls == fail_copy) return EFAULT; memcpy(p->base+at,buffer,n); return 0; }
#include "storage-syscall-extracted.h"

int main(void)
{
	process.fd = &process;
	uintptr_t args[6] = {0,(uintptr_t)bytes,65536,0,0,0};
	memset(bytes,0x52,sizeof(bytes));
	assert(sys_write_call(args) == 65536);
	unsigned writes = calls;
	file.offset = 0; calls = 0; memset(bytes,0,sizeof(bytes));
	assert(sys_read_call(args) == 65536 && bytes[65535] == 0x52);
	assert(calls == (65536 + ZEDBSD_SYSCALL_REGULAR_CHUNK - 1) / ZEDBSD_SYSCALL_REGULAR_CHUNK && writes == calls);
	printf("S44 CELL chunk=%u IMOD=%u backend_write=%u backend_read=%u IRQ=unmeasured\n",
	    ZEDBSD_SYSCALL_REGULAR_CHUNK, ZEDBSD_XHCI_IMOD, writes, calls);
	args[2] = 3 * ZEDBSD_SYSCALL_REGULAR_CHUNK;
	file.offset = 0; copy_calls = 0; fail_copy = 3;
	assert(sys_write_call(args) == 2 * ZEDBSD_SYSCALL_REGULAR_CHUNK);
	assert(file.offset == 2 * ZEDBSD_SYSCALL_REGULAR_CHUNK && !held && !pins && !allocations);
	fail_copy = 0; file.offset = 0; assert(sys_write_call(args) == (intptr_t)args[2]);
	puts("S41 PASS injected third user copy fault returns partial length and releases lease");
	args[2] = 65536; fail_alloc = 1; file.offset = 0; calls = 0;
	assert(sys_write_call(args) == 65536 && calls == 128);
	fail_alloc = 0; file.offset = 0; assert(sys_read_call(args) == 65536);
	assert(!held && !pins && !allocations);
	puts("S42 PASS pool exhaustion falls back to stack buffer and next I/O succeeds");

	/* All entry paths use the actual production helper, including current default. */
	unsigned expected = (65536 + ZEDBSD_SYSCALL_REGULAR_CHUNK - 1) / ZEDBSD_SYSCALL_REGULAR_CHUNK;
	file.offset = 99; calls = 0; args[3] = 17;
	assert(sys_positional_call(args, 1) == 65536 && calls == expected && file.offset == 99);
	calls = 0; assert(sys_positional_call(args, 0) == 65536 && calls == expected && file.offset == 99);
	struct syscall_iovec v[3] = {{(uintptr_t)bytes,65536},{(uintptr_t)bytes,0},{(uintptr_t)bytes+65536,777}};
	uintptr_t va[6] = {0,(uintptr_t)v,1,0,0,0};
	file.offset = 0; calls = 0;
	assert(sys_vector_call(va, 1) == 65536 && calls == expected);
	file.offset = 0; calls = 0;
	assert(sys_vector_call(va, 0) == 65536 && calls == expected);
	va[2] = 3; file.offset = 0; calls = 0;
	assert(sys_vector_call(va, 1) == 65536+777 && calls == expected + (777 + ZEDBSD_SYSCALL_REGULAR_CHUNK - 1) / ZEDBSD_SYSCALL_REGULAR_CHUNK);
	file.offset = 0; assert(sys_vector_call(va, 0) == 65536+777);
	puts("q087 PASS six syscall directions; positional offset; vector element and empty boundaries");

	const size_t lengths[] = {0,1,511,512,513,4095,4096,4097,65536,
	    ZEDBSD_SYSCALL_REGULAR_CHUNK, ZEDBSD_SYSCALL_REGULAR_CHUNK+1};
	for (size_t i = 0; i < sizeof(lengths)/sizeof(lengths[0]); i++) {
		args[2] = lengths[i]; file.offset = 0; calls = 0;
		assert(sys_write_call(args) == (intptr_t)args[2]);
		assert(calls == (args[2]+ZEDBSD_SYSCALL_REGULAR_CHUNK-1)/ZEDBSD_SYSCALL_REGULAR_CHUNK);
		assert(!held && !pins && !allocations);
	}
	args[2] = 65536; alloc_ceiling = 4096; file.offset = 0; calls = 0;
	assert(sys_write_call(args) == 65536);
	assert(allocated_size == 4096 && calls == 16);
	alloc_ceiling = SIZE_MAX;
	puts("q087/WS025 PASS request/cap boundaries and small-pool fallback");

	args[2] = ZEDBSD_SYSCALL_REGULAR_CHUNK + 1;
	for (unsigned mode = 0; mode < 6; mode++) {
		for (unsigned fault = 0; fault < 6; fault++) {
			file.offset = 0; calls = copy_calls = 0;
			v[0].length = args[2]; va[2] = 1; args[3] = 0;
			fail_backend = fault < 2 ? fault+1 : 0;
			short_backend = fault == 2 || fault == 3 ? 1 : 0;
			short_count = fault == 2 ? 37 : 0;
			begin_error = fault == 4 ? EBUSY : 0;
			fail_copy = fault == 5 ? 1 : 0;
			intptr_t got = mode == 0 ? sys_read_call(args) : mode == 1 ? sys_write_call(args) :
			    mode < 4 ? sys_positional_call(args, mode == 3) : sys_vector_call(va, mode == 5);
			intptr_t want = fault == 0 ? -EIO : fault == 1 ? (intptr_t)ZEDBSD_SYSCALL_REGULAR_CHUNK :
			    fault == 2 ? 37 : fault == 3 ? 0 : fault == 4 ? -EBUSY : -EFAULT;
			assert(got == want && !held && !pins && !allocations);
		}
	}
	fail_backend = short_backend = fail_copy = 0; begin_error = 0;
	args[2] = 65536; file.offset = 0; calls = 0; short_backend = limit_hit = 1; short_count = 37;
	assert(sys_write_call(args) == 37 && calls == 1 && signals == 1);
	short_backend = limit_hit = 0;
	puts("q087 PASS short/EOF/backend/copy/begin failures on six paths; growth-limit signal");

	/* The public write variants preserve the checked completion result. */
	completion_error = EIO;
	for (unsigned mode = 0; mode < 3; mode++) {
		file.offset = 0; calls = completions = 0;
		args[2] = 65536; args[3] = 0; v[0].length = 65536; va[2] = 1;
		intptr_t got = mode == 0 ? sys_write_call(args) :
		    mode == 1 ? sys_positional_call(args,1) : sys_vector_call(va,1);
		assert(got == -EIO && completions == 1 && !held && !pins && !allocations);
	}
	completion_error = 0;
	puts("WS025 PASS scalar/positional/vector checked completion errors");

	inode.i_type = 3; file.offset = 0; calls = 0;
	assert(sys_read_call(args) == 512 && calls == 1);
	pipe_mode = 1; v[0].length = 255; v[1].length = 257; va[2] = 2;
	file.offset = 0; calls = 0;
	assert(sys_vector_call(va, 1) == 512 && calls == 1);
	assert(!held && !pins && !allocations);
	puts("q087 PASS stream read termination and PIPE_BUF single transfer preserved");
	return 0;
}
