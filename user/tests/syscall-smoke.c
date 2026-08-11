#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv, char **envp)
{
	const char message[] = "BOOT_USER_SYSCALL_OK\n";
	(void)argc; (void)argv; (void)envp;
	return write(1, message, strlen(message)) == (ssize_t)(sizeof(message) - 1U) ? 37 : 1;
}
