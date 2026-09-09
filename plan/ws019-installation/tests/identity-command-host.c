/* Actual stat/blkid command parsers and output, with deterministic blkid ioctl. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#if defined(TEST_STAT)
#define main stat_command_main
#include "userland/base/stat/main.c"
#undef main
void command_error(const char *command, const char *operand)
{ fprintf(stderr, "%s: %s: %s\n", command, operand, strerror(errno)); }
int main(int argc, char **argv) { return stat_command_main(argc, argv); }
#else
#include <zedbsd/blkid.h>
static int fake_open(const char *path, int flags, ...) { (void)path; (void)flags; return 42; }
static int fake_close(int fd) {
	(void)fd;
	if (getenv("CLOSE_ERROR")) { errno = EIO; return -1; }
	return 0;
}
static int fake_ioctl(int fd, unsigned long op, ...) {
	struct block_identity *id;
	va_list args;
	(void)fd;
	if (op != BLKGETIDENTITY) abort();
	if (getenv("QUERY_ERROR")) { errno = EIO; return -1; }
	if (getenv("UNKNOWN")) { errno = ENOENT; return -1; }
	va_start(args, op); id = va_arg(args, struct block_identity *); va_end(args);
	memset(id, 0, sizeof(*id));
	id->flags = ZEDBSD_BLKID_TYPE | ZEDBSD_BLKID_LABEL | ZEDBSD_BLKID_PARTUUID;
	strcpy(id->type, "vfat"); strcpy(id->label, "a b\nc=d\\e");
	strcpy(id->partuuid, "12345678-1234-4321-8765-123456789abc");
	return 0;
}
#define open fake_open
#define close fake_close
#define ioctl fake_ioctl
#define main blkid_command_main
#include "userland/base/blkid/main.c"
#undef main
int main(int argc, char **argv) { return blkid_command_main(argc, argv); }
#endif
