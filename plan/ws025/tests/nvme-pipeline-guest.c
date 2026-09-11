/* Test-image-only native BIO trigger. */
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv)
{
	int fd;
	int error;
	unsigned long request;
	if (argc != 2 || (strcmp(argv[1], "write") != 0 && strcmp(argv[1], "verify") != 0))
		return 2;
	request = strcmp(argv[1], "write") == 0 ? 0x57532701UL : 0x57532702UL;
	fd = open("/dev/nvme0n1", O_RDWR);
	if (fd < 0) { perror("nvme BIO open"); return 1; }
	error = ioctl(fd, request, NULL);
	if (error < 0) perror("nvme BIO test");
	if (close(fd) < 0) return 1;
	return error < 0 ? 1 : 0;
}
