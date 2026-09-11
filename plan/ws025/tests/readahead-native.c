/* Measures real optional file I/O through public sysctl and POSIX file calls. */
#include <sys/sysctl.h>
#include <zedbsd/readahead.h>
#include <zedbsd/io-stats.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define REQUIRE(x) do { if(!(x)){printf("READ FAIL line=%u errno=%d: %s\n",__LINE__,errno,#x);return 1;} } while(0)
#define PAGES 256U
static unsigned char buffer[65536];
static uint64_t latency[PAGES];
static struct io_stats io_before,io_after;
static const char sequential_path[]="/ws025-wb/read-sequential";
static const char random_path[]="/ws025-wb/read-random";
static int snapshot(struct readahead_report *report)
{
 size_t length=sizeof(*report);
 if(sysctlbyname("vfs.readahead.stats",report,&length,NULL,0)!=0)return -1;
 return length==sizeof(*report) && report->version==READAHEAD_REPORT_VERSION?0:-1;
}
static uint64_t now(void)
{
 struct timespec time;
 if(clock_gettime(CLOCK_MONOTONIC,&time)!=0)return UINT64_MAX;
 return (uint64_t)time.tv_sec*1000000000U+(uint64_t)time.tv_nsec;
}
/* This fixture is amd64/QEMU only; report raw cycles as well as calibrated estimates. */
static uint64_t cycles(void)
{
 unsigned low,high;
 __asm__ __volatile__("lfence; rdtsc; lfence" : "=a"(low),"=d"(high) : : "memory");
 return ((uint64_t)high<<32)|low;
}
static int compare(const void *left,const void *right)
{
 uint64_t a=*(const uint64_t *)left,b=*(const uint64_t *)right;
 return a<b?-1:a>b?1:0;
}
int main(int argc,char **argv)
{
 struct readahead_report before,after;
 unsigned char unaligned[sizeof(after)+2];size_t length;
 uint64_t begin,end,total,cal_start,cal_end,cal_cycles,rate;
 struct timespec pause={0,250000000},resolution;unsigned page,index,offset,random,baseline;int fd;
 REQUIRE(argc==2 && sizeof(after)==88);
 length=0;REQUIRE(sysctlbyname("vfs.readahead.stats",NULL,&length,NULL,0)==0 && length==sizeof(after));
 memset(unaligned,0xa5,sizeof(unaligned));length=sizeof(after)-1;errno=0;
 REQUIRE(sysctlbyname("vfs.readahead.stats",unaligned+1,&length,NULL,0)==-1 && errno==ENOMEM);
 REQUIRE(length==sizeof(after) && unaligned[1]==0xa5);
 length=sizeof(after);REQUIRE(sysctlbyname("vfs.readahead.stats",unaligned+1,&length,NULL,0)==0);
 memcpy(&after,unaligned+1,sizeof(after));
 REQUIRE(after.version==READAHEAD_REPORT_VERSION && unaligned[0]==0xa5 && unaligned[sizeof(unaligned)-1]==0xa5);
 errno=0;REQUIRE(sysctlbyname("vfs.readahead.stats",NULL,NULL,&after,sizeof(after))==-1 && errno==EPERM);
 if(strcmp(argv[1],"prepare")==0) {
  /* Validate offline fixtures without populating the guest's data-buffer cache. */
  fd=open(sequential_path,O_RDONLY);REQUIRE(fd>=0);
  REQUIRE(lseek(fd,0,SEEK_END)==PAGES*4096 && close(fd)==0);
  fd=open(random_path,O_RDONLY);REQUIRE(fd>=0);
  REQUIRE(lseek(fd,0,SEEK_END)==PAGES*4096 && close(fd)==0);
  puts("READ PREPARE PASS");return 0;
 }
 random=strcmp(argv[1],"random")==0;
 baseline=strcmp(argv[1],"sequential-baseline")==0;
 REQUIRE(random || baseline || strcmp(argv[1],"sequential")==0);
 REQUIRE(clock_getres(CLOCK_MONOTONIC,&resolution)==0);
 cal_start=now();cal_cycles=cycles();REQUIRE(nanosleep(&pause,NULL)==0);
 cal_cycles=cycles()-cal_cycles;cal_end=now();REQUIRE(cal_end>cal_start);
 rate=cal_cycles/((cal_end-cal_start)/1000U);REQUIRE(rate!=0);
 printf("READ CLOCK wall_resolution_ns=%llu cycles_per_us_est=%llu\n",
  (unsigned long long)((uint64_t)resolution.tv_sec*1000000000U+resolution.tv_nsec),
  (unsigned long long)rate);
 fd=open(random?random_path:sequential_path,O_RDONLY);REQUIRE(fd>=0);REQUIRE(snapshot(&before)==0);
 length=sizeof(io_before);REQUIRE(sysctlbyname("vfs.io.stats",&io_before,&length,NULL,0)==0);
 total=0;
 for(page=0;page<PAGES;page++) {
  /* The odd multiplier permutes every page without adjacent sequential accesses. */
  offset=(random?((page*149U+73U)%PAGES):page)*4096U;
  begin=cycles();
  REQUIRE(pread(fd,buffer,4096,(off_t)offset)==4096);
  end=cycles();REQUIRE(end>=begin);latency[page]=end-begin;total+=latency[page];
  for(index=0;index<4096;index++)REQUIRE(buffer[index]==(unsigned char)((offset+index)%251U));
 }
 REQUIRE(close(fd)==0);REQUIRE(snapshot(&after)==0);
 length=sizeof(io_after);REQUIRE(sysctlbyname("vfs.io.stats",&io_after,&length,NULL,0)==0);
 printf("READ IO syscall_calls=%llu syscall_bytes=%llu content_calls=%llu content_bytes=%llu driver_calls=%llu driver_bytes=%llu\n",
  (unsigned long long)(io_after.events[IO_SYSCALL_READ].calls-io_before.events[IO_SYSCALL_READ].calls),
  (unsigned long long)(io_after.events[IO_SYSCALL_READ].bytes-io_before.events[IO_SYSCALL_READ].bytes),
  (unsigned long long)(io_after.events[IO_UFS_CONTENT_READ].calls-io_before.events[IO_UFS_CONTENT_READ].calls),
  (unsigned long long)(io_after.events[IO_UFS_CONTENT_READ].bytes-io_before.events[IO_UFS_CONTENT_READ].bytes),
  (unsigned long long)(io_after.events[IO_DRIVER_READ].calls-io_before.events[IO_DRIVER_READ].calls),
  (unsigned long long)(io_after.events[IO_DRIVER_READ].bytes-io_before.events[IO_DRIVER_READ].bytes));
 qsort(latency,PAGES,sizeof(latency[0]),compare);
 printf("READ METRIC workload=%s reads=%u total_us_est=%llu p95_us_est=%llu p95_cycles=%llu requested=%llu started=%llu published=%llu confirmed_useful=%llu retired_uncredited=%llu discarded_fill=%llu errors=%llu jobs=%u\n",
  argv[1],PAGES,(unsigned long long)(total/rate),(unsigned long long)(latency[(PAGES*95U)/100U]/rate),
  (unsigned long long)latency[(PAGES*95U)/100U],
  (unsigned long long)(after.requested_bytes-before.requested_bytes),
  (unsigned long long)(after.started_bytes-before.started_bytes),
  (unsigned long long)(after.published_bytes-before.published_bytes),
  (unsigned long long)(after.confirmed_useful_bytes-before.confirmed_useful_bytes),
  (unsigned long long)(after.retired_uncredited_bytes-before.retired_uncredited_bytes),
  (unsigned long long)(after.discarded_fill_bytes-before.discarded_fill_bytes),
  (unsigned long long)(after.errors-before.errors),after.jobs);
 REQUIRE(io_after.events[IO_DRIVER_READ].bytes-io_before.events[IO_DRIVER_READ].bytes>=PAGES*4096U);
 REQUIRE(after.errors==before.errors);
 if(!random && !baseline)REQUIRE(after.confirmed_useful_bytes>before.confirmed_useful_bytes);
 if(baseline)REQUIRE(after.started_bytes==before.started_bytes && after.confirmed_useful_bytes==before.confirmed_useful_bytes);
 puts("READ MEASURE PASS");return 0;
}
