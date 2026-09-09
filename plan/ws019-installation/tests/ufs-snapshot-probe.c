/* Disposable native acceptance fixture; not installed as an OS command. */
#include <sys/snapshot.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <zedbsd/fcntl.h>

int
main(int argc, char **argv)
{
	struct snapshot_control request;
	int result;
	int busy;

	if (argc != 3)
		return 2;
	if (strcmp(argv[1], "diagnose") == 0) {
		struct stat st;
		struct zedbsd_file_format_reserve lease;
		char block[512] = {0};
		int fd = open(argv[2], O_RDWR | O_NOFOLLOW | O_CLOEXEC);
		printf("diagnose open=%d errno=%d\n", fd, errno);
		if (fd < 0 || fstat(fd, &st) != 0)
			return 1;
		memset(&lease, 0, sizeof(lease));
		lease.version = ZEDBSD_FILE_FORMAT_VERSION;
		lease.struct_size = sizeof(lease);
		lease.size_bytes = st.st_size;
		result = ioctl(fd, ZEDBSD_FILE_FORMAT_RESERVE, &lease);
		printf("diagnose reserve=%d errno=%d\n", result, errno);
		if (result != 0) {
			close(fd);
			return 1;
		}
		if (result == 0) {
			result = pwrite(fd, block, sizeof(block), 0);
			printf("diagnose write=%d errno=%d\n", result, errno);
			if (result != sizeof(block)) {
				close(fd);
				return 1;
			}
			result = fsync(fd);
			printf("diagnose fsync=%d errno=%d\n", result, errno);
			if (result != 0) {
				close(fd);
				return 1;
			}
		}
		result = close(fd);
		printf("diagnose close=%d errno=%d\n", result, errno);
		return result != 0;
	}
	memset(&request, 0, sizeof(request));
	request.size = sizeof(request);
	request.version = ZEDBSD_SNAPSHOT_VERSION;
	busy = strcmp(argv[1], "create-busy") == 0;
	if (busy || strcmp(argv[1], "create") == 0)
		request.command = ZEDBSD_SNAPSHOT_CREATE;
	else if (strcmp(argv[1], "delete") == 0)
		request.command = ZEDBSD_SNAPSHOT_DELETE;
	else
		return 2;
	result = snapshotctl(argv[2], &request);
	if (busy && result == -1 && errno == EBUSY) {
		puts("snapshot active-backing refusal PASS");
		return 0;
	}
	if (result != 0 || busy) {
		fprintf(stderr, "snapshot: result=%d errno=%d\n", result, errno);
		return 1;
	}
	puts("snapshot operation PASS");
	return 0;
}
