/* Actual LP64/ILP32 production UFS mapping and ABI conversion, no host libc.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "src/drivers/fs/ufs/ufs-vfs.c"
static unsigned checks;
static uint64_t seen_lba;
static unsigned seen_count;
static void finish(unsigned status)
{
#if __SIZEOF_POINTER__ == 8
 __asm__ volatile("syscall"::"a"(60UL),"D"((unsigned long)status):"rcx","r11","memory");
#else
 __asm__ volatile("int $0x80"::"a"(1),"b"(status):"memory");
#endif
 __builtin_unreachable();
}
#define REQUIRE(x) do{checks++;if(!(x))finish((checks - 1U) % 255U + 1U);}while(0)
void *memset(void *p,int c,size_t n){unsigned char *b=p;while(n--)*b++=(unsigned char)c;return p;}
void *memcpy(void *d,const void *s,size_t n){unsigned char *a=d;const unsigned char*b=s;while(n--)*a++=*b++;return d;}
void io_stats_record(enum io_stat_event event,uint64_t bytes){(void)event;(void)bytes;}
/* Compiler-runtime integer division only; production supplies all address logic. */
unsigned long long __udivmoddi4(unsigned long long n,unsigned long long d,unsigned long long *r)
{unsigned long long q=0;int i;REQUIRE(d!=0);for(i=63;i>=0;i--)if((n>>i)>=d){n-=d<<i;q|=1ULL<<i;}if(r)*r=n;return q;}
unsigned long long __udivdi3(unsigned long long n,unsigned long long d){return __udivmoddi4(n,d,0);}
unsigned long long __umoddi3(unsigned long long n,unsigned long long d){unsigned long long r;__udivmoddi4(n,d,&r);return r;}
int disk_read(struct disk *disk,uint64_t lba,uint32_t count,void *bytes)
{
 (void)disk;seen_lba=lba;seen_count=count;memset(bytes,0,count*512U);
 ufs_put64(bytes,0,(UINT64_C(1)<<32)+168,0);return 0;
}
static void test(void)
{
 struct ufs_mount_state fs={0};struct ufs_inode_info node={0};struct mount mountp={0};struct disk disk={0};
 uint8_t raw[UFS_DINODE_SIZE]={0};uint64_t size,blocks,fragment;unsigned swapped;
 fs.super.bsize=4096;fs.super.fsize=512;fs.super.frag=8;fs.super.nindir=512;
 fs.super.fpg=512;fs.super.ncg=8388610;fs.super.size=(uint64_t)fs.super.ncg*512;
 fs.super.dblkno=160;mountp.m_data=&fs;mountp.m_disk=&disk;disk.d_block_size=512;node.inode.i_mount=&mountp;
 node.direct[0]=(UINT64_C(1)<<32)+160;
 REQUIRE(bmap(&node.inode,0,&fragment)==0&&fragment==node.direct[0]);
 node.indirect[0]=(UINT64_C(1)<<32)+176;
 REQUIRE(bmap(&node.inode,12,&fragment)==0&&fragment==(UINT64_C(1)<<32)+168);
 REQUIRE(seen_lba==node.indirect[0]&&seen_count==1);
 for(swapped=0;swapped<2;swapped++){
  fs.super.swapped=swapped;ufs_put64(raw,UFS_DI_SIZE,INT32_MAX,swapped);ufs_put64(raw,UFS_DI_BLOCKS,1,swapped);
  REQUIRE(inode_size_values(raw,&fs.super,&size,&blocks)==0&&size==INT32_MAX&&blocks==1);
  ufs_put64(raw,UFS_DI_SIZE,(uint64_t)INT32_MAX+1,swapped);
  REQUIRE(inode_size_values(raw,&fs.super,&size,&blocks)==(sizeof(off_t)==8?0:EFBIG));
  ufs_put64(raw,UFS_DI_SIZE,UINT64_MAX,swapped);REQUIRE(inode_size_values(raw,&fs.super,&size,&blocks)==EFBIG);
  ufs_put64(raw,UFS_DI_SIZE,0,swapped);ufs_put64(raw,UFS_DI_BLOCKS,(uint64_t)INT32_MAX+1,swapped);
  REQUIRE(inode_size_values(raw,&fs.super,&size,&blocks)==(sizeof(blkcnt_t)==8?0:EOVERFLOW));
  ufs_put64(raw,UFS_DI_BLOCKS,UINT64_MAX,swapped);REQUIRE(inode_size_values(raw,&fs.super,&size,&blocks)==EOVERFLOW);
 }
 finish(0);
}
void _start(void){test();}
