/* Any target I/O from a capacity-only command is a test failure. */
#include <stdlib.h>
#include <sys/types.h>
#include <stddef.h>
int __wrap_open(const char *path, int flags, ...) { (void)path; (void)flags; abort(); }
ssize_t __wrap_pread(int fd, void *p, size_t n, off_t o) { (void)fd; (void)p; (void)n; (void)o; abort(); }
ssize_t __wrap_pwrite(int fd, const void *p, size_t n, off_t o) { (void)fd; (void)p; (void)n; (void)o; abort(); }
int __wrap_fsync(int fd) { (void)fd; abort(); }
int __wrap_ioctl(int fd, unsigned long request, ...) { (void)fd; (void)request; abort(); }
