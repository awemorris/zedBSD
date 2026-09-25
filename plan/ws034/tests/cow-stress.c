/* Fork and copy-on-write stress for ws034-p043: many children write to pages
 * shared with the parent, some while the parent pins them in a blocking
 * pipe write; every child checks what it read. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
int main(void) {
	size_t size = 256 * 1024, i;
	unsigned char *b = malloc(size);
	int round, bad = 0;
	for (i = 0; i < size; i++) b[i] = (unsigned char)(i * 13);
	for (round = 0; round < 40; round++) {
		int fd[2]; pipe(fd);
		pid_t p = fork();
		if (p == 0) {
			/* The child reads the pipe slowly, then writes the shared pages. */
			unsigned char *in = malloc(size); size_t got = 0; ssize_t n;
			close(fd[1]);
			usleep(20000);
			while ((n = read(fd[0], in + got, size - got)) > 0) got += (size_t)n;
			for (i = 0; i < size; i += 4096) b[i] ^= 0xff;
			for (i = 0; i < got; i++) if (in[i] != (unsigned char)(i * 13)) _exit(2);
			_exit(got == size ? 0 : 3);
		}
		close(fd[0]);
		/* The parent writes from the shared buffer (pinned while blocked)... */
		size_t sent = 0; ssize_t n;
		while (sent < size && (n = write(fd[1], b + sent, size - sent)) > 0) sent += (size_t)n;
		close(fd[1]);
		int status; waitpid(p, &status, 0);
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) bad++;
		/* ...and its own view must be unchanged by the child's writes. */
		for (i = 0; i < size; i++) if (b[i] != (unsigned char)(i * 13)) { bad++; break; }
	}
	printf("COWSTRESS %s bad=%d\n", bad ? "FAIL" : "PASS", bad);
	return bad != 0;
}
