#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <zedbsd/io-stats.h>
static unsigned char input[65536];
static unsigned char output[65536] __attribute__((aligned(4096)));
#define CHECK(x) do { if(!(x)){printf("OUTPUT-VIEW FAIL line=%d errno=%d\n",__LINE__,errno);return 1;} } while(0)
static uint64_t ns(struct timespec t) { return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
static uint64_t cpu(struct rusage r) { return (uint64_t)(r.ru_utime.tv_sec+r.ru_stime.tv_sec)*1000000+r.ru_utime.tv_usec+r.ru_stime.tv_usec; }
int main(void)
{
 struct io_stats before,after;
 struct timespec start,end;
 struct rusage first,last;
 size_t size;
 unsigned i,round;
 int fd,status;
 pid_t child;
 for(i=0;i<sizeof(input);i++)input[i]=(unsigned char)(i*13+7);
 fd=open("/root/output-view-data",O_CREAT|O_TRUNC|O_RDWR,0600);CHECK(fd>=0);
 CHECK(write(fd,input,sizeof(input))==sizeof(input));CHECK(fsync(fd)==0);
 memset(output,0x66,sizeof(output));
 CHECK(pread(fd,output,sizeof(output),0)==sizeof(output));CHECK(memcmp(input,output,sizeof(input))==0);
 for(round=0;round<8;round++) {
  size=sizeof(before);CHECK(sysctlbyname("vfs.io.stats",&before,&size,NULL,0)==0);
  CHECK(size==sizeof(before) && before.version==IO_STATS_VERSION && before.count==IO_STAT_COUNT);
  CHECK(getrusage(RUSAGE_SELF,&first)==0);CHECK(clock_gettime(CLOCK_MONOTONIC,&start)==0);
  for(i=0;i<256;i++) {
   if(round&1) { CHECK(lseek(fd,0,SEEK_SET)==0);CHECK(read(fd,output,sizeof(output))==sizeof(output)); }
   else CHECK(pread(fd,output,sizeof(output),0)==sizeof(output));
  }
  CHECK(clock_gettime(CLOCK_MONOTONIC,&end)==0);CHECK(getrusage(RUSAGE_SELF,&last)==0);
  size=sizeof(after);CHECK(sysctlbyname("vfs.io.stats",&after,&size,NULL,0)==0);
  CHECK(memcmp(input,output,sizeof(input))==0);
  printf("OUTPUT-VIEW METRIC copy=%llu view=%llu cpu_us=%llu wall_ns=%llu\n",
   (unsigned long long)(after.events[IO_SCALAR_OUTPUT_COPY].bytes-before.events[IO_SCALAR_OUTPUT_COPY].bytes),
   (unsigned long long)(after.events[IO_SCALAR_OUTPUT_VIEW].bytes-before.events[IO_SCALAR_OUTPUT_VIEW].bytes),
   (unsigned long long)(cpu(last)-cpu(first)),(unsigned long long)(ns(end)-ns(start)));
 }
 /* User stores/read faults after retirement, and EOF must preserve the suffix. */
 memset(output,0x66,sizeof(output));
 CHECK(pread(fd,output,sizeof(output),65536-37)==37);
 CHECK(memcmp(output,input+65536-37,37)==0);
 for(i=37;i<sizeof(output);i++)CHECK(output[i]==0x66);
 CHECK(pread(fd,output,sizeof(output),65536)==0);
 for(i=37;i<sizeof(output);i++)CHECK(output[i]==0x66);
 CHECK(pread(fd,output+1,4095,1)==4095);CHECK(memcmp(output+1,input+1,4095)==0);
 memset(output,0x66,sizeof(output));
 child=fork();CHECK(child>=0);
 if(child==0) {
  CHECK(pread(fd,output,sizeof(output),0)==sizeof(output));
  CHECK(memcmp(input,output,sizeof(input))==0);
  output[0]^=0x5a;
  _exit(0);
 }
 CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
 for(i=0;i<sizeof(output);i++)CHECK(output[i]==0x66);
 CHECK(close(fd)==0);CHECK(unlink("/root/output-view-data")==0);
 puts("OUTPUT-VIEW PASS read pread EOF suffix unaligned fork COW exit");
 return 0;
}
