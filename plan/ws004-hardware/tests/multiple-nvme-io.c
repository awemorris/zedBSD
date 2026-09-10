/* Two processes exercise distinct controllers with different persistent patterns. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int exercise(const char *path, int raw, unsigned tag)
{
 static unsigned char sent[65536],readback[65536];
 unsigned round,i;
 int fd;
 off_t offset;
 fd=open(path,raw ? O_RDWR : O_RDWR|O_CREAT|O_TRUNC,0600);
 if(fd<0)return 1;
 for(round=0;round<64;round++) {
  for(i=0;i<sizeof(sent);i++)sent[i]=(unsigned char)(tag ^ round ^ (i*17U));
  offset=(raw ? 8*1024*1024 : 0)+(off_t)round*sizeof(sent);
  if(pwrite(fd,sent,sizeof(sent),offset)!=(ssize_t)sizeof(sent) || fsync(fd)!=0 ||
     pread(fd,readback,sizeof(readback),offset)!=(ssize_t)sizeof(readback) ||
     memcmp(sent,readback,sizeof(sent))!=0) {
   fprintf(stderr,"multi-nvme: %s round=%u errno=%d\n",path,round,errno);
   close(fd);return 1;
  }
 }
 return close(fd)!=0;
}
int main(int argc,char **argv)
{
 int gate[2],status,failed;
 pid_t child;
 char start=1;
 if(argc!=3 || pipe(gate)!=0)return 2;
 child=fork();
 if(child<0)return 2;
 if(child==0) {
  close(gate[1]);
  if(read(gate[0],&start,1)!=1)_exit(2);
  close(gate[0]);_exit(exercise(argv[1],1,0x3d));
 }
 close(gate[0]);
 failed=write(gate[1],&start,1)!=1;close(gate[1]);
 failed|=exercise(argv[2],0,0xa7);
 if(waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status)!=0)failed=1;
 if(failed)return 1;
 puts("MULTIPLE NVME concurrent write/flush/readback PASS");return 0;
}
