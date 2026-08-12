#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv, char **envp)
{
	const char message[] = "BOOT_USER_SYSCALL_OK\n";
	unsigned char *allocation;
	size_t offset;
	(void)argc; (void)argv; (void)envp;
	allocation = malloc(256U * 1024U);
	if (allocation == NULL)
		return 2;
	for (offset = 0; offset < 256U * 1024U; offset += 4096U)
		allocation[offset] = (unsigned char)(offset >> 12);
	if (allocation[63U * 4096U] != 63U) {
		free(allocation);
		return 3;
	}
	free(allocation);
	return write(1, message, strlen(message)) ==
		(ssize_t)(sizeof(message) - 1U) ? 37 : 1;
}
