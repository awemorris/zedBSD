/* Production private cache and generation helpers, deterministic disk boundary.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <stdio.h>
#include <stdlib.h>
#include "src/drivers/fs/fat.c"
#define REQUIRE(x) do {if(!(x)){fprintf(stderr,"line %u: %s\n",__LINE__,#x);abort();}}while(0)
static uint8_t media[8192];
static unsigned reads,writes,read_error,write_error;
int disk_read(struct disk *disk,uint64_t first,uint32_t count,void *bytes)
{(void)disk;reads++;if(read_error)return EIO;REQUIRE((first+count)*512<=sizeof(media));memcpy(bytes,media+first*512,count*512);return 0;}
int disk_read_direct(struct disk *disk,uint64_t first,uint32_t count,void *bytes)
{return disk_read(disk,first,count,bytes);}
int disk_write_filesystem(struct disk *disk,uint64_t first,uint32_t count,const void *bytes)
{(void)disk;writes++;if(write_error)return EIO;REQUIRE((first+count)*512<=sizeof(media));memcpy(media+first*512,bytes,count*512);return 0;}
void mutex_lock(struct mutex *lock) { (void)lock; }
void mutex_unlock(struct mutex *lock) { (void)lock; }
int main(void)
{
 struct fat_mount_state fs={0};struct disk disk={0};
 struct mount mountp={0};struct fat_inode_info node={0};struct file writer={0};
 struct fat_file_state file={0};struct fat_chain_cursor cursor={0,2};
 const uint8_t *sector;uint8_t *mutable;unsigned before,i;
 fs.disk=&disk;fs.total_sectors=16;
 for(i=0;i<sizeof(media);i++)media[i]=(uint8_t)(i/512);
 REQUIRE(sizeof(fs.clean_sectors)<=4*(512+8));
 REQUIRE(fat_engine_read_sector_result(&fs,1,&sector)==0&&sector[0]==1);
 REQUIRE(fat_engine_read_sector_result(&fs,2,&sector)==0&&sector[0]==2);
 before=reads;
 for(i=0;i<100;i++){
  REQUIRE(fat_engine_read_sector_result(&fs,1,&sector)==0&&sector[0]==1);
  REQUIRE(fat_engine_read_sector_result(&fs,2,&sector)==0&&sector[0]==2);
 }
 REQUIRE(reads==before);
 REQUIRE(fat_engine_write_sector_result(&fs,1,&mutable)==0);mutable[0]=91;
 REQUIRE(fat_engine_mark_sector_dirty(&fs)==0);
 write_error=1;REQUIRE(fat_engine_read_sector_result(&fs,2,&sector)==EIO);
 REQUIRE(fs.sector_cache_dirty&&fs.sector_cache_lba==1);
 write_error=0;REQUIRE(fat_engine_read_sector_result(&fs,2,&sector)==0);
 REQUIRE(media[512]==91&&!fs.sector_cache_dirty);
 REQUIRE(fat_engine_read_sector_result(&fs,1,&sector)==0&&sector[0]==91);
 read_error=1;REQUIRE(fat_engine_read_sector_result(&fs,3,&sector)==EIO);
 REQUIRE(!fs.sector_cache_valid);read_error=0;
 REQUIRE(fat_engine_read_sector_result(&fs,3,&sector)==0&&sector[0]==3);
 fat_engine_invalidate(&fs);before=reads;
 REQUIRE(fat_engine_read_sector_result(&fs,1,&sector)==0&&reads==before+1);
 for(i=0;i<FAT_CLEAN_SLOTS;i++)REQUIRE(!fs.clean_sectors[i].valid);
 file.mount=&fs;file.first_cluster=2;
 fat_file_save_cursor(&file,&cursor,512,fs.chain_generation,2);
 REQUIRE(file.cursor_valid);
 fat_chain_invalidate(&fs);REQUIRE(file.cursor_generation!=fs.chain_generation);
 fs.chain_generation=UINT64_MAX;fat_chain_invalidate(&fs);
 REQUIRE(fs.chain_generation==UINT64_MAX);
 fat_file_save_cursor(&file,&cursor,512,UINT64_MAX,2);REQUIRE(!file.cursor_valid);
 /* Prove a complete cluster chain; a short tail cannot admit delayed growth. */
 memset(media,0,sizeof(media));fat_engine_invalidate(&fs);
 fs.type=ZEDBSD_FAT32;fs.fat_start=1;fs.fat_sectors=1;fs.cluster_count=8;
 fs.sectors_per_cluster=1;fs.number_of_fats=1;
 mountp.m_data=&fs;node.fi_inode.i_mount=&mountp;node.fi_inode.i_type=INODE_REG;
 node.fi_inode.i_size=1024;node.fi_first_cluster=2;
 writer.f_inode=&node.fi_inode;writer.f_data=&file;file.mount=&fs;
 media[512+8]=3;media[512+12]=0xff;media[512+13]=0xff;
 media[512+14]=0xff;media[512+15]=0x0f;
 before=writes;
 REQUIRE(fat_writeback_range(&writer,0,1024)==1);
 REQUIRE(fat_writeback_range(&writer,1023,2)==0);
 REQUIRE(fat_writeback_range(&writer,-1,1)==0);
 REQUIRE(fat_writeback_range(&writer,0,0)==0);
 fs.read_only=1;REQUIRE(fat_writeback_range(&writer,0,1)==0);fs.read_only=0;
 node.fi_inode.i_size=1536;REQUIRE(fat_writeback_range(&writer,0,1536)==0);
 media[512+12]=2;media[512+13]=media[512+14]=media[512+15]=0;
 fat_engine_invalidate(&fs);REQUIRE(fat_writeback_range(&writer,0,512)==-EIO);
 REQUIRE(writes==before);
 puts("FAT private cache PASS: bounded clean slots, alias update, dirty/read failure, invalidation, generation saturation");
 return 0;
}

/* Synchronous media adapter retains the production context validation. */
int
disk_write_filesystem_context(struct disk *disk, uint64_t block, uint32_t count,
    const void *data, const struct io_context *context)
{
	int error = io_context_validate(context);
	return error != 0 ? error : disk_write_filesystem(disk, block, count, data);
}
