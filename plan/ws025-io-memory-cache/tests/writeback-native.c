/* Exercises opt-in writeback through the public sysctl and file interfaces. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/uio.h>
#include <sys/sysctl.h>
#include <zedbsd/writeback.h>
#include <zedbsd/io-stats.h>

#define REQUIRE(x) do { if(!(x)){printf("WRITEBACK FAIL line=%u errno=%d: %s\n",__LINE__,errno,#x);return 1;} } while(0)
static unsigned char bytes[65536],observed[65536];
static struct writeback_report report;
static struct io_stats before,after;
static const char path[]="/ws025-wb/content";
static int status(void)
{
 size_t size=sizeof(report);
 if(sysctlbyname("vfs.writeback.stats",&report,&size,NULL,0)!=0)return -1;
 if(size!=sizeof(report)||report.header.version!=WRITEBACK_REPORT_VERSION){errno=EINVAL;return -1;}
 return 0;
}
static int control(unsigned enabled)
{
 struct writeback_control request;
 memset(&request,0,sizeof(request));request.version=WRITEBACK_REPORT_VERSION;
 request.enabled=enabled;strcpy(request.path,"/ws025-wb");
 return sysctlbyname("vfs.writeback.control",NULL,NULL,&request,sizeof(request));
}
static int io_status(struct io_stats *stats)
{ size_t size=sizeof(*stats);return sysctlbyname("vfs.io.stats",stats,&size,NULL,0); }
static uint64_t cycles(void)
{
 unsigned low,high;
 __asm__ __volatile__("lfence; rdtsc; lfence" : "=a"(low),"=d"(high) : : "memory");
 return ((uint64_t)high<<32)|low;
}
int main(int argc,char **argv)
{
 int fd,syncfd;struct iovec vectors[2];unsigned round;unsigned char *mapping;uint64_t passes,calls;
 struct timespec start,end;uint64_t tick_start,tick_end;
 if(argc==2 && strcmp(argv[1],"verify")==0) {
  fd=open(path,O_RDONLY);REQUIRE(fd>=0);
  REQUIRE(pread(fd,observed,64,3)==64);
  for(round=0;round<64;round++)REQUIRE(observed[round]==0xab);
  REQUIRE(pread(fd,observed,2,8193)==2 && observed[0]==0x9a && observed[1]==0x5b);
  REQUIRE(close(fd)==0);printf("WRITEBACK VERIFY PASS\n");return 0;
 }
 if(argc==2 && strcmp(argv[1],"unmount")==0) {
  REQUIRE(control(1)==0);fd=open(path,O_RDWR);REQUIRE(fd>=0);
  memset(bytes,0xab,64);REQUIRE(pwrite(fd,bytes,64,3)==64);
  errno=0;REQUIRE(unmount("/ws025-wb",0)==-1 && errno==EBUSY);
  REQUIRE(status()==0 && report.header.count==1 && report.mounts[0].state==WRITEBACK_STATE_LIVE);
  REQUIRE(pwrite(fd,bytes,64,3)==64);REQUIRE(close(fd)==0);
  REQUIRE(unmount("/ws025-wb",0)==0);
  REQUIRE(status()==0 && report.header.count==0 && report.header.workers==0);
  REQUIRE(report.header.dirty==0 && report.header.reserved==0 && report.header.tickets==0);
  printf("WRITEBACK UNMOUNT PASS\n");return 0;
 }
 if(argc==2 && strcmp(argv[1],"shutdown")==0) {
  REQUIRE(control(1)==0);fd=open(path,O_RDWR);REQUIRE(fd>=0);
  memset(bytes,0xcd,64);REQUIRE(pwrite(fd,bytes,64,3)==64);
  REQUIRE(close(fd)==0);REQUIRE(status()==0 && report.header.count==1);
  printf("WRITEBACK SHUTDOWN ARMED dirty=%llu\n",(unsigned long long)report.header.dirty);
  return 0;
 }
 REQUIRE(argc==1);
 REQUIRE(status()==0 && report.header.count==1 && report.header.workers==1);
 REQUIRE(control(1)==0); /* CLI enable followed by idempotent API enable. */
 fd=open(path,O_CREAT|O_TRUNC|O_RDWR,0600);REQUIRE(fd>=0);
 for(round=0;round<sizeof(bytes);round++)bytes[round]=(unsigned char)(round%251U);
 REQUIRE(write(fd,bytes,sizeof(bytes))==sizeof(bytes));REQUIRE(fsync(fd)==0);
 REQUIRE(pread(fd,observed,sizeof(observed),0)==sizeof(observed));
 REQUIRE(memcmp(bytes,observed,sizeof(bytes))==0);
 /* Measure one full overwrite without shell input contaminating the window. */
 REQUIRE(io_status(&before)==0);
 REQUIRE(clock_gettime(CLOCK_MONOTONIC,&start)==0);tick_start=cycles();
 REQUIRE(pwrite(fd,bytes,sizeof(bytes),0)==sizeof(bytes));REQUIRE(fsync(fd)==0);
 tick_end=cycles();REQUIRE(clock_gettime(CLOCK_MONOTONIC,&end)==0);
 REQUIRE(io_status(&after)==0);
 printf("WRITEBACK DMA TIME elapsed_ns=%llu tsc_delta=%llu\n",
  (unsigned long long)(((int64_t)end.tv_sec-start.tv_sec)*1000000000+end.tv_nsec-start.tv_nsec),
  (unsigned long long)(tick_end-tick_start));
 printf("WRITEBACK DMA usb_copy=%llu hcd_copy=%llu shared=%llu sg_td=%llu trbs=%llu trb_bytes=%llu\n",
  (unsigned long long)(after.events[IO_USB_STAGING_COPY].bytes-before.events[IO_USB_STAGING_COPY].bytes),
  (unsigned long long)(after.events[IO_XHCI_BOUNCE_COPY].bytes-before.events[IO_XHCI_BOUNCE_COPY].bytes),
  (unsigned long long)(after.events[IO_XHCI_SHARED_STAGING].bytes-before.events[IO_XHCI_SHARED_STAGING].bytes),
  (unsigned long long)(after.events[IO_XHCI_SG_TD].calls-before.events[IO_XHCI_SG_TD].calls),
  (unsigned long long)(after.events[IO_XHCI_DATA_TRB].calls-before.events[IO_XHCI_DATA_TRB].calls),
  (unsigned long long)(after.events[IO_XHCI_DATA_TRB].bytes-before.events[IO_XHCI_DATA_TRB].bytes));
 REQUIRE(io_status(&before)==0);
 for(round=0;round<30;round++) {
  memset(bytes,0x30+round,64);REQUIRE(pwrite(fd,bytes,64,3)==64);
 }
 REQUIRE(pread(fd,observed,64,3)==64 && memcmp(bytes,observed,64)==0);
 REQUIRE(fsync(fd)==0 && io_status(&after)==0);
 calls=after.events[IO_UFS_CONTENT_WRITE].calls-before.events[IO_UFS_CONTENT_WRITE].calls;
 printf("WRITEBACK BATCH writes=%llu bytes=%llu\n",(unsigned long long)calls,
  (unsigned long long)(after.events[IO_UFS_CONTENT_WRITE].bytes-before.events[IO_UFS_CONTENT_WRITE].bytes));
 REQUIRE(calls>0 && calls<30);
 before=after;
 for(round=0;round<30;round++) {
  bytes[0]=(unsigned char)round;REQUIRE(pwrite(fd,bytes,64,3)==64);REQUIRE(fsync(fd)==0);
 }
 REQUIRE(io_status(&after)==0);
 calls=after.events[IO_UFS_CONTENT_WRITE].calls-before.events[IO_UFS_CONTENT_WRITE].calls;
 printf("WRITEBACK EACH_FSYNC writes=%llu\n",(unsigned long long)calls);REQUIRE(calls>=30);

 /* Ordinary delayed writes and CPU stores share one revoked/captured object. */
 mapping=mmap(NULL,sizeof(observed),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
 REQUIRE(mapping!=MAP_FAILED);mapping[8193]=0x9a;bytes[0]=0x5b;
 REQUIRE(pwrite(fd,bytes,1,8194)==1);
 REQUIRE(pread(fd,observed,2,8193)==2 && observed[0]==0x9a && observed[1]==0x5b);
 REQUIRE(msync(mapping,sizeof(observed),MS_SYNC)==0);
 REQUIRE(munmap(mapping,sizeof(observed))==0);

 /* Age alone must trigger the real independent kernel filesystem worker. */
 REQUIRE(status()==0);passes=report.header.passes;
 bytes[0]=0x77;REQUIRE(pwrite(fd,bytes,1,9000)==1);
 sleep(3);REQUIRE(status()==0);
 REQUIRE(report.header.passes>passes && report.header.dirty==0);
 printf("WRITEBACK AGE passes=%llu dirty=%llu\n",(unsigned long long)report.header.passes,
  (unsigned long long)report.header.dirty);
 /* Scalar, positional and vector syscalls report synchronous completion. */
 for(round=0;round<2;round++) {
  syncfd=open(path,O_RDWR|(round==0?O_SYNC:O_DSYNC));REQUIRE(syncfd>=0);
  memset(bytes,0xce,64);vectors[0].iov_base=bytes;vectors[0].iov_len=64;
  vectors[1]=vectors[0];
  REQUIRE(lseek(syncfd,16384,SEEK_SET)==16384);
  REQUIRE(io_status(&before)==0);
  REQUIRE(write(syncfd,bytes,64)==64);
  REQUIRE(io_status(&after)==0);
  REQUIRE(after.events[IO_DRIVER_FLUSH].calls>before.events[IO_DRIVER_FLUSH].calls);
  before=after;REQUIRE(pwrite(syncfd,bytes,64,16512)==64);
  REQUIRE(io_status(&after)==0);
  REQUIRE(after.events[IO_DRIVER_FLUSH].calls>before.events[IO_DRIVER_FLUSH].calls);
  before=after;REQUIRE(writev(syncfd,vectors,2)==128);
  REQUIRE(io_status(&after)==0);
  REQUIRE(after.events[IO_DRIVER_FLUSH].calls>before.events[IO_DRIVER_FLUSH].calls);
  REQUIRE(status()==0 && report.header.dirty==0);REQUIRE(close(syncfd)==0);
 }
 /* Overlay forwards synchronous status through its resolved real-file open. */
 for(round=0;round<2;round++) {
  syncfd=open("/tmp/ws025-sync",O_CREAT|O_TRUNC|O_RDWR|(round==0?O_SYNC:O_DSYNC),0600);
  REQUIRE(syncfd>=0);REQUIRE(write(syncfd,bytes,64)==64);
  REQUIRE(pread(syncfd,observed,64,0)==64 && memcmp(bytes,observed,64)==0);
  REQUIRE(close(syncfd)==0);REQUIRE(unlink("/tmp/ws025-sync")==0);
 }
 printf("WRITEBACK OVERLAY SYNCHRONOUS PASS\n");
 printf("WRITEBACK SYNCHRONOUS PASS\n");
 memset(bytes,0xab,64);REQUIRE(pwrite(fd,bytes,64,3)==64);
 REQUIRE(close(fd)==0);REQUIRE(control(0)==0);
 REQUIRE(status()==0 && report.header.count==0 && report.header.workers==0);
 REQUIRE(report.header.dirty==0 && report.header.reserved==0 && report.header.tickets==0);
 printf("WRITEBACK NATIVE PASS\n");return 0;
}
