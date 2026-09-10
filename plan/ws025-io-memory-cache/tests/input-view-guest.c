#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <zedbsd/io-stats.h>
static unsigned char input[65536] __attribute__((aligned(4096)));
static unsigned char output[65536];
#define CHECK(x) do { if(!(x)){printf("INPUT-VIEW FAIL line=%d errno=%d\n",__LINE__,errno);return 1;} } while(0)
static uint64_t ns(struct timespec t) { return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
static uint64_t cpu(struct rusage r) { return (uint64_t)(r.ru_utime.tv_sec+r.ru_stime.tv_sec)*1000000+r.ru_utime.tv_usec+r.ru_stime.tv_usec; }
static void size_signal(int number) { (void)number; }
int main(void)
{
 struct io_stats before,after;
 struct timespec start,end;
 struct rusage first,last;
 struct rlimit saved,limit;
 size_t size;
 unsigned i,round;
 int fd;
 for(i=0;i<sizeof(input);i++)input[i]=(unsigned char)(i*13+7);
 fd=open("/root/input-view-data",O_CREAT|O_TRUNC|O_RDWR,0600);CHECK(fd>=0);
 CHECK(write(fd,input,sizeof(input))==sizeof(input));CHECK(fsync(fd)==0);
 CHECK(pread(fd,output,sizeof(output),0)==sizeof(output));CHECK(memcmp(input,output,sizeof(input))==0);
 for(round=0;round<8;round++) {
 size=sizeof(before);CHECK(sysctlbyname("vfs.io.stats",&before,&size,NULL,0)==0);
 CHECK(size==sizeof(before) && before.version==IO_STATS_VERSION && before.count==IO_STAT_COUNT);
 CHECK(getrusage(RUSAGE_SELF,&first)==0);CHECK(clock_gettime(CLOCK_MONOTONIC,&start)==0);
 for(i=0;i<256;i++)CHECK(pwrite(fd,input,sizeof(input),0)==sizeof(input));
 CHECK(clock_gettime(CLOCK_MONOTONIC,&end)==0);CHECK(getrusage(RUSAGE_SELF,&last)==0);
 size=sizeof(after);CHECK(sysctlbyname("vfs.io.stats",&after,&size,NULL,0)==0);
 CHECK(fsync(fd)==0);CHECK(pread(fd,output,sizeof(output),0)==sizeof(output));CHECK(memcmp(input,output,sizeof(input))==0);
 printf("INPUT-VIEW METRIC copy=%llu view=%llu cpu_us=%llu wall_ns=%llu\n",
  (unsigned long long)(after.events[IO_SCALAR_INPUT_COPY].bytes-before.events[IO_SCALAR_INPUT_COPY].bytes),
  (unsigned long long)(after.events[IO_SCALAR_INPUT_VIEW].bytes-before.events[IO_SCALAR_INPUT_VIEW].bytes),
  (unsigned long long)(cpu(last)-cpu(first)),(unsigned long long)(ns(end)-ns(start)));
 }
 /* Actual CPU store after lease release must refault with writable access. */
 input[0]^=0x5a;
 CHECK(pwrite(fd,input,sizeof(input),0)==sizeof(input));
 CHECK(pread(fd,output,sizeof(output),0)==sizeof(output));CHECK(memcmp(input,output,sizeof(input))==0);
 CHECK(pwrite(fd,input+1,4095,4096)==4095);CHECK(pread(fd,output,4095,4096)==4095);CHECK(memcmp(input+1,output,4095)==0);
 CHECK(close(fd)==0);
 fd=open("/root/input-view-data",O_WRONLY|O_APPEND);CHECK(fd>=0);
 CHECK(lseek(fd,0,SEEK_SET)==0);CHECK(write(fd,input,sizeof(input))==sizeof(input));CHECK(close(fd)==0);
 fd=open("/root/input-view-data",O_RDONLY);CHECK(fd>=0);CHECK(lseek(fd,0,SEEK_END)==131072);
 CHECK(pread(fd,output,sizeof(output),65536)==sizeof(output));CHECK(memcmp(input,output,sizeof(input))==0);CHECK(close(fd)==0);
 CHECK(getrlimit(RLIMIT_FSIZE,&saved)==0);limit=saved;limit.rlim_cur=37;
 CHECK(signal(SIGXFSZ,size_signal)!=SIG_ERR);
 fd=open("/root/input-view-limit",O_CREAT|O_TRUNC|O_RDWR,0600);CHECK(fd>=0);
 CHECK(setrlimit(RLIMIT_FSIZE,&limit)==0);CHECK(write(fd,input,sizeof(input))==37);
 CHECK(setrlimit(RLIMIT_FSIZE,&saved)==0);CHECK(fsync(fd)==0);
 CHECK(pread(fd,output,sizeof(output),0)==37);CHECK(memcmp(input,output,37)==0);CHECK(close(fd)==0);
 CHECK(unlink("/root/input-view-data")==0);CHECK(unlink("/root/input-view-limit")==0);
 puts("INPUT-VIEW PASS write pwrite append unaligned limit fsync readback");
 return 0;
}
