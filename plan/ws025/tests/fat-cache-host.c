/* Production FAT with maintained VFS fixture and counter-based reuse oracles.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#define FAT_CURSOR_MAIN retained_cursor_main
#include "../../ws011/tests/fat-write-cursor-host-test.c"
#include <kern/io-stats.h>
static uint64_t event_count(enum io_stat_event event)
{ struct io_stats stats;io_stats_snapshot(&stats);return stats.events[event].calls; }
int main(void)
{
 static const uint32_t chain[]={100,400,200,500,300,600,700,800};
 struct memory_image image;struct mount mountp;struct inode *inode;
 struct file reader,writer;uint8_t bytes[512];
 uint64_t hits,steps;unsigned i;uint32_t cluster_bytes;
 prepare_cursor_file(&image,ZEDBSD_FAT16,1,chain,ARRAY_COUNT(chain));
 cluster_bytes=image.sectors_per_cluster*512U;
 open_cursor_file(&image,&mountp,&inode,&reader);
 CHECK_ERROR(host_file_open(inode,O_RDWR,&writer),0);
 hits=event_count(IO_FAT_CURSOR_HIT);
 CHECK(reader.f_ops->pread(&reader,bytes,sizeof(bytes),0)==sizeof(bytes));
 steps=event_count(IO_FAT_CHAIN_STEP);
 CHECK(reader.f_ops->pread(&reader,bytes,sizeof(bytes),512)==sizeof(bytes));
 CHECK(event_count(IO_FAT_CURSOR_HIT)==hits+1);
 CHECK(event_count(IO_FAT_CHAIN_STEP)-steps<=1);
 for(i=0;i<sizeof(bytes);i++)CHECK(bytes[i]==0x35);
 /* Backward seek validates anew instead of using the later cluster. */
 hits=event_count(IO_FAT_CURSOR_HIT);steps=event_count(IO_FAT_CHAIN_STEP);
 CHECK(reader.f_ops->pread(&reader,bytes,sizeof(bytes),0)==sizeof(bytes));
 CHECK(event_count(IO_FAT_CURSOR_HIT)==hits);
 CHECK(event_count(IO_FAT_CHAIN_STEP)-steps>=ARRAY_COUNT(chain));
 /* Another open changes only payload; cached sectors must expose the new bytes. */
 memset(bytes,0x76,sizeof(bytes));
 CHECK(writer.f_ops->pwrite(&writer,bytes,sizeof(bytes),512)==sizeof(bytes));
 CHECK(reader.f_ops->pread(&reader,bytes,sizeof(bytes),512)==sizeof(bytes));
 for(i=0;i<sizeof(bytes);i++)CHECK(bytes[i]==0x76);
 /* Shared truncate/free and subsequent growth retire every cursor. */
 CHECK_ERROR(inode->i_op->truncate(inode,cluster_bytes),0);
 memset(bytes,0x42,sizeof(bytes));
 CHECK(writer.f_ops->pwrite(&writer,bytes,sizeof(bytes),cluster_bytes)==sizeof(bytes));
 hits=event_count(IO_FAT_CURSOR_HIT);
 CHECK(reader.f_ops->pread(&reader,bytes,sizeof(bytes),cluster_bytes)==sizeof(bytes));
 CHECK(event_count(IO_FAT_CURSOR_HIT)==hits);
 for(i=0;i<sizeof(bytes);i++)CHECK(bytes[i]==0x42);
 CHECK_ERROR(host_file_close(&writer),0);
 close_cursor_file(&mountp,inode,&reader);destroy_image(&image);
 printf("FAT cache PASS: sequential reuse, seek validation, other-open overwrite, truncate/free/growth (%u checks)\n",checks);
 return 0;
}
