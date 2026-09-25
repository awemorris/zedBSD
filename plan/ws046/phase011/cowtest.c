#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#define PG 4096
#define N 64
static int fails;
static void check(int ok, const char *what){ if(!ok){ printf("FAIL %s\n", what); fails++; } }
int main(void){
	char buf[PG]; int fd, i; char *m; pid_t c; int st;
	fd = open("/tmp/cow.dat", O_RDWR|O_CREAT|O_TRUNC, 0644);
	for (i = 0; i < N; i++) { memset(buf, 'A' + (i % 26), PG); write(fd, buf, PG); }
	/* private read-write mapping */
	m = mmap(0, N*PG, PROT_READ|PROT_WRITE, MAP_PRIVATE, fd, 0);
	check(m != MAP_FAILED, "mmap");
	check(m[5*PG] == 'F', "read before write");
	m[5*PG] = 'z';
	check(m[5*PG] == 'z', "private write visible");
	pread(fd, buf, 1, 5*PG); check(buf[0] == 'F', "file unchanged by private write");
	check(m[6*PG] == 'G', "neighbour still file content");
	/* fork: each side keeps its own write */
	m[7*PG] = 'p';
	c = fork();
	if (c == 0) { m[7*PG] = 'c'; m[8*PG] = 'c'; _exit(m[7*PG]=='c' && m[5*PG]=='z' ? 0 : 1); }
	waitpid(c, &st, 0);
	check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "child sees its own writes");
	check(m[7*PG] == 'p', "parent keeps its write after child");
	check(m[8*PG] == 'I', "parent unaffected by child write");
	/* write() to the file shows through pages not yet written */
	memset(buf, '#', PG); pwrite(fd, buf, PG, 20*PG);
	check(m[20*PG] == '#', "unwritten page sees later file write");
	check(m[5*PG] == 'z', "written page keeps private copy");
	munmap(m, N*PG);
	/* read-only mapping made writable */
	m = mmap(0, N*PG, PROT_READ, MAP_PRIVATE, fd, 0);
	check(m[3*PG] == 'D', "ro read");
	check(mprotect(m, N*PG, PROT_READ|PROT_WRITE) == 0, "mprotect");
	m[3*PG] = 'q';
	pread(fd, buf, 1, 3*PG); check(buf[0] == 'D', "file unchanged after mprotect write");
	check(m[3*PG] == 'q', "mprotect write visible");
	munmap(m, N*PG);
	/* exec-like: map, touch all, unmap many times */
	for (i = 0; i < 50; i++) { m = mmap(0, N*PG, PROT_READ, MAP_PRIVATE, fd, 0); volatile char s = 0; for (int j = 0; j < N; j++) s += m[j*PG]; (void)s; munmap(m, N*PG); }
	close(fd); unlink("/tmp/cow.dat");
	printf(fails ? "COWTEST FAIL %d\n" : "COWTEST OK\n", fails);
	return fails != 0;
}
