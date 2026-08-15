/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/ufs1.h"
#include "kern/disk.h"
#include "kern/file.h"
#include "kern/inode.h"
#include "kern/mount.h"
#include "kern/namei.h"
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
static unsigned get32(size_t offset)
{ return (unsigned)image[offset]|(unsigned)image[offset+1]<<8|
	(unsigned)image[offset+2]<<16|(unsigned)image[offset+3]<<24; }
void *kern_malloc(size_t n){return malloc(n);} void *kern_calloc(size_t n,size_t s){return calloc(n,s);} void kern_free(void *p){free(p);}

static int submit(struct disk *disk,struct bio *bio)
{
	size_t offset=(size_t)bio->b_mapped_block*disk->d_block_size;
	size_t length=(size_t)bio->b_block_count*disk->d_block_size;
	if(bio->b_op==BIO_FLUSH){bio_complete(bio,0,0);return 0;}
	if(offset>image_size||length>image_size-offset)return EIO;
	if(bio->b_op==BIO_READ)memcpy(bio->b_data,image+offset,length);
	else if(bio->b_op==BIO_WRITE)memcpy(image+offset,bio->b_data,length);
	else return EOPNOTSUPP;
	bio_complete(bio,0,length);return 0;
}
static const struct disk_ops ops={.submit=submit};

int main(void)
{
	FILE *fp=fopen(UFS1_TEST_IMAGE,"rb"); struct disk *disk; struct mount *mountp;
	struct componentname etc_name={"etc",3,0}, marker_name={"zedbsd-root",11,0};
	struct componentname fresh_name={"fresh",5,0},moved_name={"moved",5,0};
	struct componentname hard_name={"hard",4,0},sym_name={"sym",3,0};
	struct componentname sub_name={"sub",3,0},child_name={"child",5,0};
	struct inode *etc,*marker,*fresh,*found,*sym,*sub,*child;
	struct path path; struct file *file; char text[32]={0};
	unsigned char *large;size_t index;
	unsigned initial_nbfree,initial_nifree,initial_ndir;
	assert(fp); fseek(fp,0,SEEK_END); image_size=(size_t)ftell(fp); rewind(fp);
	image=malloc(image_size); assert(image&&fread(image,1,image_size,fp)==image_size); fclose(fp);
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
