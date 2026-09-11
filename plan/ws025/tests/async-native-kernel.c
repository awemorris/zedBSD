/* Link-only native probe: actual scheduler and real USB read, no production ABI.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <kern/bio-async.h>
#include <kern/sched.h>
#include <hal/hal.h>
#include <string.h>
#include <errno.h>
#define ASYNC_TEST_IOCTL 0x57533019UL
#define VERIFY(x) do { if (!(x)) { hal_printf("ASYNC NATIVE FAIL line=%d\n",__LINE__); HAL_FATAL("async native test"); } } while (0)
static atomic_uint_t released;
static atomic_uint_t active;
static unsigned char expected[512];
int __real_disk_ioctl(struct disk *, unsigned long, void *);
int __wrap_disk_ioctl(struct disk *, unsigned long, void *);
static int delayed_read(struct disk *disk,struct bio *bio);
static void disable_endpoint(struct disk *disk);
static const struct disk_ops probe_ops={.submit=delayed_read};

static int delayed_read(struct disk *disk,struct bio *bio)
{
 (void)disk;
 while(!atomic_load_acquire(&released))sched_yield();
 VERIFY(bio->b_op==BIO_READ && bio->b_block_count==1);
 memset(bio->b_data,0x73,512);bio_complete(bio,0,512);return 0;
}
static void disable_endpoint(struct disk *disk)
{
 unsigned attempts=0;int error;
 do { error=bio_async_disable(disk);if(error==EBUSY)sched_yield(); }
 while(error==EBUSY && ++attempts<1000000U);
 VERIFY(error==0);
}
int __wrap_disk_ioctl(struct disk *disk,unsigned long command,void *argument)
{
 struct disk *probe;
 struct bio_async_request *request;
 const void *data;size_t transferred;unsigned was=0;
 if(command!=ASYNC_TEST_IOCTL)return __real_disk_ioctl(disk,command,argument);
 VERIFY(atomic_compare_exchange(&active,&was,1));
 probe=disk_alloc();VERIFY(probe!=NULL);
 strcpy(probe->d_name,"asyncprobe");probe->d_block_size=512;probe->d_block_count=16;
 probe->d_ops=&probe_ops;VERIFY(disk_create(probe)==0);
 VERIFY(bio_async_enable(probe)==0);
 atomic_store_release(&released,0);
 VERIFY(bio_async_prepare(probe,BIO_READ,0,1,NULL,NULL,NULL,NULL,NULL,&request)==0);
 VERIFY(bio_async_submit(request)==0);
 /* A synchronous driver call would never reach this release. */
 atomic_store_release(&released,1);
 VERIFY(bio_async_wait(request)==0);
 VERIFY(bio_async_result(request,&data,&transferred)==0 && transferred==512);
 VERIFY(((const unsigned char *)data)[0]==0x73);
 bio_async_release(request);disable_endpoint(probe);
 VERIFY(disk_gone_if_idle(probe)==0);VERIFY(disk_destroy(probe)==0);
 VERIFY(disk_read_direct(disk,0,1,expected)==0);
 VERIFY(bio_async_enable(disk)==0);
 VERIFY(bio_async_prepare(disk,BIO_READ,0,1,NULL,NULL,NULL,NULL,NULL,&request)==0);
 VERIFY(bio_async_submit(request)==0);VERIFY(bio_async_wait(request)==0);
 VERIFY(bio_async_result(request,&data,&transferred)==0 && transferred==512);
 VERIFY(memcmp(data,expected,512)==0);
 bio_async_release(request);disable_endpoint(disk);
 atomic_store_release(&active,0);
 hal_printf("ASYNC NATIVE KERNEL PASS: independent scheduler dispatch and real USB read\n");
 return 0;
}
