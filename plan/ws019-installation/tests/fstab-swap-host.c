/* Production fstab/swapon integration with a memory table and control mock. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zedbsd/system.h>

static FILE *table;
static int opens, closes, calls, open_error, close_error, table_error;
static int errors[16];
static char names[16][sizeof(((struct system_swap_control *)0)->source)];

static FILE *test_fopen(const char *path, const char *mode)
{
	FILE *result = table;
	assert(strcmp(path, "/etc/fstab") == 0 && strcmp(mode, "r") == 0);
	if (table_error) { errno = table_error; return NULL; }
	table = NULL;
	return result;
}
static int test_open(const char *path, int flags, ...)
{
	(void)flags;
	assert(strcmp(path, "/dev/system") == 0);
	opens++;
	if (open_error) { errno = open_error; return -1; }
	return 73;
}
static int test_close(int fd)
{
	assert(fd == 73); closes++;
	if (close_error) { errno = close_error; return -1; }
	return 0;
}
static int test_ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	struct system_swap_control *control;
	int at = calls++;
	assert(fd == 73 && request == ZEDBSD_SYSTEM_SWAP_ADD && at < 16);
	va_start(ap, request); control = va_arg(ap, struct system_swap_control *); va_end(ap);
	assert(control->version == ZEDBSD_SYSTEM_SWAP_VERSION);
	assert(control->struct_size == sizeof(*control));
	strcpy(names[at], control->source);
	if (errors[at]) { errno = errors[at]; return -1; }
	return 0;
}
#define fopen test_fopen
#define SWAP_COMMAND_OPEN test_open
#define SWAP_COMMAND_IOCTL test_ioctl
#define SWAP_COMMAND_CLOSE test_close
#define main swapon_program_main
#include "userland/base/swapon/main.c"
#undef main
#undef fopen

static FILE *bytes(const void *data, size_t size)
{
	FILE *stream = tmpfile();
	assert(stream != NULL && fwrite(data, 1, size, stream) == size);
	rewind(stream);
	return stream;
}
static void reset(const char *text)
{
	if (table != NULL) fclose(table);
	table = bytes(text, strlen(text));
	opens = closes = calls = open_error = close_error = table_error = 0;
	memset(errors, 0, sizeof(errors));
	memset(names, 0, sizeof(names));
}
static int run(void)
{
	char *argv[] = {"swapon", "-a", NULL};
	return swapon_program_main(2, argv);
}
static void parser(void)
{
	struct command_fstab entry;
	unsigned line = 0;
	FILE *stream;
	char buffer[4200];
	const char *valid = "# comment\n\n/path\\040name none swap defaults 0 0 # tail\n"
	    "/tab\\011name none swap sw\n/slash\\134name none swap defaults";
	const char *bad[] = {"x y z\n", "a b c d e\n", "a b c d 0 0 extra\n",
	    "/bad\\00 none swap sw\n", "/bad\\000 none swap sw\n",
	    "/bad\\777 none swap sw\n", "/bad\\x none swap sw\n"};
	size_t i;
	stream = bytes(valid, strlen(valid));
	assert(command_fstab_next(stream, &entry, &line) == 1 && line == 3);
	assert(strcmp(entry.source, "/path name") == 0);
	assert(command_fstab_next(stream, &entry, &line) == 1);
	assert(strcmp(entry.source, "/tab\tname") == 0);
	assert(command_fstab_next(stream, &entry, &line) == 1);
	assert(strcmp(entry.source, "/slash\\name") == 0);
	assert(command_fstab_next(stream, &entry, &line) == 0);
	fclose(stream);
	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		stream = bytes(bad[i], strlen(bad[i])); line = 0;
		assert(command_fstab_next(stream, &entry, &line) == -1 && line == 1);
		assert(command_fstab_next(stream, &entry, &line) == 0); fclose(stream);
	}
	memset(buffer, 'x', 4100); buffer[4100] = '\n';
	strcpy(buffer + 4101, "/good none swap sw\n");
	stream = bytes(buffer, strlen(buffer)); line = 0;
	assert(command_fstab_next(stream, &entry, &line) == -1);
	assert(command_fstab_next(stream, &entry, &line) == 1 && line == 2);
	assert(strcmp(entry.source, "/good") == 0); fclose(stream);
	memcpy(buffer, "/x\0 none swap sw\n", 17);
	stream = bytes(buffer, 17); line = 0;
	assert(command_fstab_next(stream, &entry, &line) == -1); fclose(stream);
}
int main(void)
{
	parser();
	reset("# empty\n"); assert(run() == 0 && opens == 0);
	reset("/x none swap noauto\na /mnt ufs defaults\n");
	assert(run() == 0 && opens == 0);
	reset("/one none swap sw 0 0\n/two\\040name none swap defaults\n");
	assert(run() == 0 && calls == 2 && opens == 1 && closes == 1);
	assert(strcmp(names[1], "/two name") == 0);
	reset("/one none swap sw\n/two none swap sw\n"); errors[0] = EEXIST;
	assert(run() == 0 && calls == 2);
	reset("/one none swap nofail\n/two none swap nofail\n");
	errors[0] = ENOENT; errors[1] = ENODEV; assert(run() == 0 && calls == 2);
	reset("/one none swap nofail\n/two none swap sw\n"); errors[0] = EIO;
	assert(run() == 1 && calls == 2 && closes == 1);
	reset("/one none swap sw\n/two none swap sw\n"); errors[0] = EBUSY;
	assert(run() == 1 && calls == 2);
	reset("/one none swap discard\n/two none swap sw\n");
	assert(run() == 1 && calls == 1 && strcmp(names[0], "/two") == 0);
	reset("/one none swap sw,\n/two none swap ,sw\n"); assert(run() == 1 && opens == 0);
	reset("broken line\n/one none swap sw\n"); assert(run() == 1 && calls == 1);
	reset("/one none swap sw\n"); open_error = EACCES;
	assert(run() == 1 && opens == 1 && calls == 0 && closes == 0);
	reset("/one none swap sw\n"); close_error = EIO;
	assert(run() == 1 && calls == 1 && closes == 1);
	reset(""); table_error = ENOENT; assert(run() == 1 && opens == 0);
	if (table != NULL) { fclose(table); table = NULL; }
	puts("fstab parser and swapon -a PASS");
	return 0;
}
