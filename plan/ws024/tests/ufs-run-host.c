/* Production UFS data runs with deterministic block mapping and hook boundaries.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#define UFS_AUDIT_CUSTOM_IO
#define main retained_audit_main
#include "ufs-metadata-host.c"
#undef main
#include "../../ws025/tests/journal-view-stubs-host.inc"
#define CONTENT_READ IO_UFS_CONTENT_READ
#define CONTENT_WRITE IO_UFS_CONTENT_WRITE
static unsigned journal_calls, snapshot_calls;
static unsigned hook_failure;
int drv_ufs_snapshot_preserve(struct ufs_snapshot *snapshot,uint64_t first,uint32_t count)
{ (void)snapshot;(void)first;(void)count;snapshot_calls++;return hook_failure?EIO:0; }
int drv_ufs_journal_read(struct ufs_journal *journal, uint64_t first,
    uint32_t count, void *buffer)
{ (void)journal; (void)first; (void)count; (void)buffer; abort(); }
int drv_ufs_journal_commitv(struct ufs_journal *journal,
    const struct ufs_journal_extent *extents, unsigned count)
{ (void)journal; (void)extents; (void)count; abort(); }
int drv_ufs_journal_commit(struct ufs_journal *j,uint64_t target,const void *payload,uint32_t sectors)
{
 REQUIRE(snapshot_calls==journal_calls+1);REQUIRE(sectors<=j->sector_count-2);
 journal_calls++;return disk_write(NULL,target,sectors,payload);
}

int main(void)
{
 AUDIT_STATE fs;AUDIT_INODE node;struct mount mountp;struct disk disk;
 struct io_stats before,after;struct file writer;unsigned writes_before;
 unsigned char input[65536],output[65536],expected[65536];
 int mapping_error;
 unsigned i;off_t offset=12*4096;
 storage_fixture(&fs,&node,&mountp,&disk,0,0);
 disk.d_block_size=512;disk.d_block_count=512;
 node.inode.i_size=28*4096;node.blocks=28*8+8;
 for(i=0;i<12;i++)node.direct[i]=160+i*8;
 node.indirect[0]=400;
 for(i=0;i<16;i++)AUDIT_PUTPTR(storage+400*512,i*AUDIT_STRIDE,256+i*8,0);
 for(i=0;i<sizeof(input);i++)input[i]=(unsigned char)(i*13+i/4096);
 memcpy(storage+256*512,input,sizeof(input));
 REQUIRE(persist_inode(&node.inode)==0);
 /* Delayed admission proves existing allocation without mutating the disk. */
 memset(&writer,0,sizeof(writer));writer.f_inode=&node.inode;
 writes_before=storage_writes;
 REQUIRE(ufs_writeback_range(&writer,offset,sizeof(input))==1);
 REQUIRE(ufs_writeback_range(&writer,1,4096)==1);
 REQUIRE(ufs_writeback_range(&writer,node.inode.i_size-1,2)==0);
 REQUIRE(ufs_writeback_range(&writer,-1,1)==0);
 REQUIRE(ufs_writeback_range(&writer,0,0)==0);
 fs.writable=0;REQUIRE(ufs_writeback_range(&writer,0,1)==0);fs.writable=1;
 REQUIRE(storage_writes==writes_before);

 io_stats_snapshot(&before);
 REQUIRE(pread_inode(&node.inode,output,sizeof(output),offset)==sizeof(output));
 io_stats_snapshot(&after);REQUIRE(after.events[CONTENT_READ].calls-before.events[CONTENT_READ].calls==1);
 REQUIRE(!memcmp(input,output,sizeof(input)));
 before=after;memset(input,0x62,sizeof(input));
 REQUIRE(pwrite_inode(&node.inode,input,sizeof(input),offset)==sizeof(input));
 io_stats_snapshot(&after);REQUIRE(after.events[CONTENT_WRITE].calls-before.events[CONTENT_WRITE].calls==1);
 REQUIRE(after.events[CONTENT_READ].calls==before.events[CONTENT_READ].calls);
 REQUIRE(!memcmp(storage+256*512,input,sizeof(input)));
 /* Direct/indirect boundary, partial edges and exact EOF retain logical contents. */
 before=after;REQUIRE(pread_inode(&node.inode,output,sizeof(output),0)==sizeof(output));
 io_stats_snapshot(&after);REQUIRE(after.events[CONTENT_READ].calls-before.events[CONTENT_READ].calls==2);
 REQUIRE(pwrite_inode(&node.inode,input,65534,offset+1)==65534);
 REQUIRE(pread_inode(&node.inode,output,sizeof(output),offset)==sizeof(output));
 REQUIRE(!memcmp(input,output,sizeof(input)));
 REQUIRE(pread_inode(&node.inode,output,sizeof(output),node.inode.i_size-17)==17);
 REQUIRE(pread_inode(&node.inode,output,sizeof(output),node.inode.i_size)==0);
 /* Holes and noncontiguous addresses split the physical run without losing bytes. */
 memcpy(expected,input,sizeof(expected));memset(expected+4*4096,0,4096);
 AUDIT_PUTPTR(storage+400*512,4*AUDIT_STRIDE,0,0);
 REQUIRE(ufs_writeback_range(&writer,offset,sizeof(input))==0);
 REQUIRE(pread_inode(&node.inode,output,sizeof(output),offset)==sizeof(output));
 REQUIRE(!memcmp(expected,output,sizeof(output)));
 memset(storage+416*512,0x39,4096);memset(expected+4*4096,0x39,4096);
 AUDIT_PUTPTR(storage+400*512,4*AUDIT_STRIDE,416,0);
 before=after;io_stats_snapshot(&before);
 REQUIRE(pread_inode(&node.inode,output,sizeof(output),offset)==sizeof(output));
 io_stats_snapshot(&after);REQUIRE(after.events[CONTENT_READ].calls-before.events[CONTENT_READ].calls==3);
 REQUIRE(!memcmp(expected,output,sizeof(output)));
 /* A bad later mapping returns only the valid earlier run. */
 AUDIT_PUTPTR(storage+400*512,4*AUDIT_STRIDE,520,0);
 REQUIRE(pread_inode(&node.inode,output,sizeof(output),offset)==4*4096);
 AUDIT_PUTPTR(storage+400*512,4*AUDIT_STRIDE,288,0);
 REQUIRE(content_run_bytes(&node.inode,12+fs.super.nindir-1,160,65536,0,&mapping_error)==4096);
 /* A failed lookahead is retained after the known prefix, without silent retry. */
 failure_read=storage_reads+2;
 REQUIRE(pread_inode(&node.inode,output,sizeof(output),offset)==4096);
 REQUIRE(storage_reads==failure_read+1);failure_read=0;
 failure_read=storage_reads+2;
 REQUIRE(pwrite_inode(&node.inode,input,sizeof(input),offset)==4096);
 failure_read=0;
 /* Failed data transfer cannot publish a completed byte count. */
 failure_write=storage_writes+1;
 REQUIRE(pwrite_inode(&node.inode,input,sizeof(input),offset)==-EIO);failure_write=0;
 /* Runs retain snapshot-before-journal routing and fit the existing journal payload. */
 fs.journal_enabled=1;fs.snapshot_available=1;fs.journal.sector_count=18;
 journal_calls=snapshot_calls=0;
 REQUIRE(pwrite_inode(&node.inode,input,sizeof(input),offset)==sizeof(input));
 REQUIRE(journal_calls==9&&snapshot_calls==9); /* Eight 8 KiB data commits plus dinode. */
 hook_failure=1;
 REQUIRE(pwrite_inode(&node.inode,input,sizeof(input),offset)==-EIO);
 REQUIRE(journal_calls==9);hook_failure=0;
 free(fs.cg);
 printf("UFS%u runs PASS: 64 KiB, indirect/partial/EOF, holes/fragments, error prefix, journal hooks\n",UFS_AUDIT_VERSION);
 return 0;
}
