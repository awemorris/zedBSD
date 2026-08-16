/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/ufs1.h"
#include "kern/disk.h"
#include "kern/file.h"
#include "kern/inode.h"
#include "kern/mount.h"
#include "kern/namei.h"
#include "kern/ufs1/ufs1-disk.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef UFS1_TEST_IMAGE
#define UFS1_TEST_IMAGE "build/ufs1-test.img"
#endif

static unsigned char *image;
static size_t image_size;
static unsigned write_operations, fail_write_at, short_write_at;
static unsigned injected_writes;
static uint64_t last_injected_block;
static unsigned get32(size_t offset)
{ return (unsigned)image[offset]|(unsigned)image[offset+1]<<8|
	(unsigned)image[offset+2]<<16|(unsigned)image[offset+3]<<24; }
static void put32(size_t offset,unsigned value)
{ image[offset]=(unsigned char)value;image[offset+1]=(unsigned char)(value>>8);
	image[offset+2]=(unsigned char)(value>>16);image[offset+3]=(unsigned char)(value>>24); }
static unsigned get16(size_t offset)
{ return (unsigned)image[offset]|(unsigned)image[offset+1]<<8; }
static void put16(size_t offset,unsigned value)
{ image[offset]=(unsigned char)value;image[offset+1]=(unsigned char)(value>>8); }
void *kern_malloc(size_t n){return malloc(n);} void *kern_calloc(size_t n,size_t s){return calloc(n,s);} void kern_free(void *p){free(p);}

static int submit(struct disk *disk,struct bio *bio)
{
	size_t offset=(size_t)bio->b_mapped_block*disk->d_block_size;
	size_t length=(size_t)bio->b_block_count*disk->d_block_size;
	if(bio->b_op==BIO_FLUSH){bio_complete(bio,0,0);return 0;}
	if(offset>image_size||length>image_size-offset)return EIO;
	if(bio->b_op==BIO_READ)memcpy(bio->b_data,image+offset,length);
	else if(bio->b_op==BIO_WRITE){
		write_operations++;
		if(write_operations==fail_write_at){injected_writes++;last_injected_block=bio->b_mapped_block;bio_complete(bio,EIO,0);return 0;}
		memcpy(image+offset,bio->b_data,length);
		if(write_operations==short_write_at){injected_writes++;last_injected_block=bio->b_mapped_block;bio_complete(bio,0,length-1U);return 0;}
	}
	else return EOPNOTSUPP;
	bio_complete(bio,0,length);return 0;
}
static const struct disk_ops ops={.submit=submit};

static void
expect_summary(unsigned ndir,unsigned nbfree,unsigned nifree)
{
	assert(get32(UFS1_SBLOCK_OFFSET+UFS1_FS_CSTOTAL_NDIR)==ndir);
	assert(get32(UFS1_SBLOCK_OFFSET+UFS1_FS_CSTOTAL_NBFREE)==nbfree);
	assert(get32(UFS1_SBLOCK_OFFSET+UFS1_FS_CSTOTAL_NIFREE)==nifree);
}

static void
test_sparse_failure_boundary(struct mount *mountp,struct inode *directory,
	off_t offset,int short_io)
{
	static const struct componentname name={"indfail",7,0};
	unsigned attempt;
	int reached_end=0;

	for(attempt=1;attempt<=64U;attempt++) {
		struct inode *inode;
		struct file *file;
		struct path path;
		unsigned ndir=get32(UFS1_SBLOCK_OFFSET+UFS1_FS_CSTOTAL_NDIR);
		unsigned nbfree=get32(UFS1_SBLOCK_OFFSET+UFS1_FS_CSTOTAL_NBFREE);
		unsigned nifree=get32(UFS1_SBLOCK_OFFSET+UFS1_FS_CSTOTAL_NIFREE);
		unsigned before_injected=injected_writes;
		char value=(char)(0x40U+(attempt&31U)),readback=0;
		ssize_t result;

		assert(inode_create(directory,&name,0600,&inode)==0);
		path_init(&path);path_set(&path,mountp,inode);
		assert(file_open_resolved(&path,O_RDWR,&file)==0);
		if(short_io)
			short_write_at=write_operations+attempt;
		else
			fail_write_at=write_operations+attempt;
		result=file_pwrite(file,&value,1,offset);
		fail_write_at=0;short_write_at=0;
		if(injected_writes==before_injected) {
			assert(result==1);
			reached_end=1;
		} else if(result==1) {
			/* Data was accepted before the transient inode metadata error.
			 * fsync must make the in-memory size/pointers durable. */
			assert(file_fsync(file)==0);
		} else {
			assert(result==-EIO);
		}
		if(result==1) {
			assert(inode->i_size==offset+1);
			assert(file_pread(file,&readback,1,offset)==1);
			assert(readback==value);
		}
		assert(file_close(file)==0);path_release(&path);
		assert(inode_unlink(directory,&name)==0);
		inode_release(inode);
		if(get32(UFS1_SBLOCK_OFFSET+UFS1_FS_CSTOTAL_NDIR)!=ndir||
		    get32(UFS1_SBLOCK_OFFSET+UFS1_FS_CSTOTAL_NBFREE)!=nbfree||
		    get32(UFS1_SBLOCK_OFFSET+UFS1_FS_CSTOTAL_NIFREE)!=nifree)
			fprintf(stderr,"UFS1 rollback mismatch: offset=%lld %s attempt=%u result=%lld injected=%u block=%llu counters=%u/%u/%u expected=%u/%u/%u\n",
			    (long long)offset,short_io?"short":"hard",attempt,
			    (long long)result,injected_writes-before_injected,
			    (unsigned long long)last_injected_block,
			    get32(UFS1_SBLOCK_OFFSET+UFS1_FS_CSTOTAL_NDIR),
			    get32(UFS1_SBLOCK_OFFSET+UFS1_FS_CSTOTAL_NBFREE),
			    get32(UFS1_SBLOCK_OFFSET+UFS1_FS_CSTOTAL_NIFREE),
			    ndir,nbfree,nifree);
		expect_summary(ndir,nbfree,nifree);
		if(reached_end)
			break;
	}
	assert(reached_end&&attempt<=64U);
}

static void
test_indirect_failure_matrix(struct mount *mountp,struct inode *directory)
{
	uint64_t bsize=get32(UFS1_SBLOCK_OFFSET+UFS1_FS_BSIZE);
	uint64_t nindir=get32(UFS1_SBLOCK_OFFSET+UFS1_FS_NINDIR);
	uint64_t logical[4];
	unsigned depth,short_io;

	logical[0]=UFS1_NDADDR-1U;
	logical[1]=UFS1_NDADDR;
	logical[2]=UFS1_NDADDR+nindir;
	logical[3]=UFS1_NDADDR+nindir+nindir*nindir;
	for(depth=0;depth<4U;depth++)
		for(short_io=0;short_io<2U;short_io++)
			test_sparse_failure_boundary(mountp,directory,
			    (off_t)(logical[depth]*bsize),short_io!=0);
}

static void
expect_mount_error(int expected)
{
	struct disk *disk;
	struct mount *mountp=NULL;

	disk_registry_reset();mount_reset();disk=disk_alloc();assert(disk);
	strcpy(disk->d_name,"badufs");disk->d_block_size=512;
	disk->d_block_count=image_size/512;disk->d_ops=&ops;
	assert(disk_create(disk)==0);
	assert(filesystem_register(&ufs1_filesystem_type)==0);
	assert(mount_private("ufs1",disk,MOUNT_READ_ONLY,NULL,&mountp)==expected);
	assert(mountp==NULL);
	assert(disk_gone_if_idle(disk)==0);assert(disk_destroy(disk)==0);
}

int main(void)
{
	FILE *fp=fopen(UFS1_TEST_IMAGE,"rb"); struct disk *disk; struct mount *mountp;
	struct componentname etc_name={"etc",3,0}, marker_name={"zedbsd-root",11,0};
	struct componentname fresh_name={"fresh",5,0},moved_name={"moved",5,0};
	struct componentname hard_name={"hard",4,0},sym_name={"sym",3,0};
	struct componentname iofail_name={"iofail",6,0};
	struct componentname full_name={"full",4,0};
	struct componentname sub_name={"sub",3,0},child_name={"child",5,0};
	struct inode *etc,*marker,*fresh,*found,*sym,*sub,*child,*iofail,*full;
	struct path path; struct file *file; char text[32]={0};
	unsigned char *large;size_t index;
	unsigned initial_nbfree,initial_nifree,initial_ndir;
	assert(fp); fseek(fp,0,SEEK_END); image_size=(size_t)ftell(fp); rewind(fp);
	image=malloc(image_size); assert(image&&fread(image,1,image_size,fp)==image_size); fclose(fp);
	/* Kernel mount parser rejects corrupt CG geometry, allocation state, root
	 * type, and root data pointers before publishing a mount. */
	{
		size_t cg=(size_t)get32(8192+12)*1024U;
		size_t root=(size_t)get32(8192+16)*1024U+2U*128U;
		unsigned saved;
		saved=get32(cg+4);put32(cg+4,0);expect_mount_error(EINVAL);put32(cg+4,saved);
		saved=get32(cg+92);put32(cg+92,get32(cg+96)+1U);
		expect_mount_error(EINVAL);put32(cg+92,saved);
		saved=image[cg+get32(cg+92)];image[cg+get32(cg+92)]&=(unsigned char)~(1U<<2);
		expect_mount_error(EINVAL);image[cg+get32(cg+92)]=(unsigned char)saved;
		saved=get16(root);put16(root,0100644);expect_mount_error(EIO);put16(root,saved);
		saved=get32(root+40);put32(root+40,1);expect_mount_error(EIO);put32(root+40,saved);
		{
			size_t root_data=(size_t)get32(root+40)*1024U;
			saved=get16(root_data+4);put16(root_data+4,0);
			expect_mount_error(EIO);put16(root_data+4,saved);
			saved=get32(root_data);put32(root_data,3);
			expect_mount_error(EIO);put32(root_data,saved);
		}
	}
	initial_ndir=get32(8192+192);initial_nbfree=get32(8192+196);
	initial_nifree=get32(8192+200);
	disk_registry_reset(); mount_reset(); disk=disk_alloc(); assert(disk);
	strcpy(disk->d_name,"ufs0"); disk->d_block_size=512; disk->d_block_count=image_size/512; disk->d_ops=&ops;
	assert(disk_create(disk)==0); assert(filesystem_register(&ufs1_filesystem_type)==0);
	assert(mount_private("ufs1",disk,0,NULL,&mountp)==0);
	assert(inode_lookup(mountp->m_root,&etc_name,&etc)==0);
	assert(inode_lookup(etc,&marker_name,&marker)==0);
	path_init(&path); path_set(&path,mountp,marker); assert(file_open_resolved(&path,O_RDWR,&file)==0);
	assert(file_read(file,text,sizeof(text))==20); assert(memcmp(text,"zedBSD ufs1 root v1\n",20)==0);
	assert(file_pwrite(file,"changed",7,0)==7);
	assert(file_pwrite(file,"Z",1,9000)==1);
	assert(file_pread(file,text,1,9000)==1&&text[0]=='Z');
	assert(inode_truncate(marker,7)==0);
	assert(file_close(file)==0); path_release(&path); inode_release(marker);

	/* Allocation and metadata failures must leave a retryable, reachable FS. */
	assert(inode_create(etc,&iofail_name,0644,&iofail)==0);
	path_init(&path);path_set(&path,mountp,iofail);
	assert(file_open_resolved(&path,O_RDWR,&file)==0);
	fail_write_at=write_operations+1U;
	assert(file_write(file,"A",1)==-EIO&&iofail->i_size==0);
	short_write_at=write_operations+1U;
	assert(file_write(file,"B",1)==-EIO&&iofail->i_size==0);
	fail_write_at=write_operations+3U; /* newly allocated block zeroing */
	assert(file_write(file,"C",1)==-EIO&&iofail->i_size==0);
	fail_write_at=write_operations+4U; /* inode pointer publication */
	assert(file_write(file,"D",1)==-EIO&&iofail->i_size==0);
	fail_write_at=write_operations+5U; /* data write after durable allocation */
	assert(file_write(file,"E",1)==-EIO&&iofail->i_size==0);
	assert(file_write(file,"ok",2)==2&&iofail->i_size==2);
	assert(file_close(file)==0);path_release(&path);
	assert(inode_unlink(etc,&iofail_name)==0);
	inode_release(iofail);

	/* Exercise every write boundary while allocating direct through
	 * triple-indirect sparse blocks, for both hard and short BIO writes. */
	test_indirect_failure_matrix(mountp,etc);

	assert(inode_create(etc,&fresh_name,0644,&fresh)==0);
	large=malloc(200000U);assert(large!=NULL);
	for(index=0;index<200000U;index++)large[index]=(unsigned char)(index*37U);
	path_init(&path);path_set(&path,mountp,fresh);
	assert(file_open_resolved(&path,O_RDWR,&file)==0);
	assert(file_write(file,large,200000U)==200000);
	memset(text,0,sizeof(text));
	assert(file_pread(file,text,sizeof(text),150000)==(ssize_t)sizeof(text));
	assert(memcmp(text,large+150000,sizeof(text))==0);
	assert(inode_truncate(fresh,100000)==0);
	assert(file_close(file)==0);path_release(&path);
	assert(inode_link(etc,&hard_name,fresh)==0);
	assert(fresh->i_linkcount==2);
	assert(inode_symlink(etc,&sym_name,"moved",&sym)==0);
	memset(text,0,sizeof(text));
	assert(inode_readlink(sym,text,sizeof(text))==5);
	assert(memcmp(text,"moved",5)==0);
	assert(inode_mkdir(etc,&sub_name,0755,&sub)==0);
	assert(inode_create(sub,&child_name,0600,&child)==0);
	assert(inode_rmdir(etc,&sub_name)==ENOTEMPTY);
	assert(inode_unlink(sub,&child_name)==0);
	inode_release(child);
	assert(inode_rmdir(etc,&sub_name)==0);
	inode_release(sub);
	assert(inode_rename(etc,&fresh_name,etc,&moved_name,0)==0);
	assert(inode_lookup(etc,&fresh_name,&found)==ENOENT);
	assert(inode_lookup(etc,&moved_name,&found)==0);
	assert(found->i_ino==fresh->i_ino);
	inode_release(found);
	assert(inode_unlink(etc,&hard_name)==0);
	assert(fresh->i_linkcount==1);

	/* Exhaust all allocatable blocks, report ENOSPC, then recover every block. */
	assert(inode_create(etc,&full_name,0600,&full)==0);
	path_init(&path);path_set(&path,mountp,full);
	assert(file_open_resolved(&path,O_RDWR,&file)==0);
	{
		unsigned char block[8192];
		ssize_t count=0;
		unsigned attempts;
		memset(block,0xa7,sizeof(block));
		for(attempts=0;attempts<10000U;attempts++) {
			count=file_write(file,block,sizeof(block));
			if(count<0)
				break;
			assert(count>0);
		}
		assert(count==-ENOSPC&&attempts<10000U);
	}
	assert(file_close(file)==0);path_release(&path);
	assert(inode_unlink(etc,&full_name)==0);
	inode_release(full);
	inode_release(sym);
	inode_release(fresh);
	inode_release(etc);
	assert(unmount_private(mountp)==0);
	assert(mount_private("ufs1",disk,MOUNT_READ_ONLY,NULL,&mountp)==0);
	assert(inode_lookup(mountp->m_root,&etc_name,&etc)==0);
	assert(inode_lookup(etc,&marker_name,&marker)==0);
	path_init(&path);path_set(&path,mountp,marker);assert(file_open_resolved(&path,O_RDONLY,&file)==0);
	memset(text,0,sizeof(text));assert(file_read(file,text,sizeof(text))==7);assert(memcmp(text,"changed",7)==0);
	assert(file_close(file)==0);path_release(&path);inode_release(marker);
	assert(inode_lookup(etc,&moved_name,&fresh)==0);
	path_init(&path);path_set(&path,mountp,fresh);
	assert(file_open_resolved(&path,O_RDONLY,&file)==0);
	assert(fresh->i_size==100000);
	memset(text,0,sizeof(text));assert(file_pread(file,text,sizeof(text),90000)==(ssize_t)sizeof(text));
	assert(memcmp(text,large+90000,sizeof(text))==0);
	assert(file_close(file)==0);path_release(&path);inode_release(fresh);
	assert(inode_lookup(etc,&hard_name,&found)==ENOENT);
	assert(inode_lookup(etc,&sub_name,&found)==ENOENT);
	assert(inode_lookup(etc,&sym_name,&sym)==0);
	memset(text,0,sizeof(text));assert(inode_readlink(sym,text,sizeof(text))==5);
	assert(memcmp(text,"moved",5)==0);
	inode_release(sym);inode_release(etc);
	assert(unmount_private(mountp)==0);
	assert(mount_private("ufs1",disk,0,NULL,&mountp)==0);
	assert(inode_lookup(mountp->m_root,&etc_name,&etc)==0);
	assert(inode_unlink(etc,&sym_name)==0);
	assert(inode_unlink(etc,&moved_name)==0);
	inode_release(etc);
	assert(unmount_private(mountp)==0);
	assert(image[8192+209]==1);
	assert(get32(8192+192)==initial_ndir);
	assert(get32(8192+196)==initial_nbfree);
	assert(get32(8192+200)==initial_nifree);
	assert(disk_gone_if_idle(disk)==0); assert(disk_destroy(disk)==0);
	free(large);free(image); puts("zedBSD UFS1 VFS read/write/remount tests: PASS"); return 0;
}
