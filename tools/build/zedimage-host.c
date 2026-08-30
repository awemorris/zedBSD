/* Host-side binary-format backend for the Noct build scripts. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

enum { FRAG=1024, BLOCK=8192, FRAGS=8, IPG=256, INODE_SIZE=128,
       SBLK=8, CBLK=16, IBLK=24, DBLK=56, MAX_INODES=768 };
enum { IFDIR=0040000, IFREG=0100000, IFLNK=0120000 };
struct child { char *name; uint32_t ino; uint8_t type; };
struct node { struct child *v; size_t n, cap; unsigned mode; };
struct image {
    uint8_t *data; size_t size; uint32_t fragments, ncg, fpg, cgsize;
    uint32_t cg_next[8], next_ino; uint8_t *used_frag[8], used_ino[8][IPG];
    struct node nodes[MAX_INODES];
};

static void die(const char *s) { perror(s); exit(1); }
static void fail(const char *s) { fprintf(stderr,"zedimage-host: %s\n",s); exit(1); }
static void p16(uint8_t *p,uint16_t v){p[0]=v;p[1]=v>>8;}
static void p32(uint8_t *p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
static void p64(uint8_t *p,uint64_t v){p32(p,(uint32_t)v);p32(p+4,(uint32_t)(v>>32));}
static uint32_t g32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint32_t cg_base(struct image *im,uint32_t cg){return cg*im->fpg;}
static uint32_t cg_ndblk(struct image *im,uint32_t cg){uint32_t b=cg_base(im,cg);return im->fragments-b<im->fpg?im->fragments-b:im->fpg;}
static uint8_t *inode_at(struct image *im,uint32_t ino){uint32_t cg=ino/IPG,index=ino%IPG;if(cg>=im->ncg)fail("inode table full");return im->data+(cg_base(im,cg)+IBLK)*FRAG+index*INODE_SIZE;}
static void add_child(struct image *im,uint32_t parent,const char *name,uint32_t ino,uint8_t type){
    struct node *n=&im->nodes[parent]; if(n->n==n->cap){n->cap=n->cap?n->cap*2:8;n->v=realloc(n->v,n->cap*sizeof(*n->v));if(!n->v)die("realloc");}
    n->v[n->n].name=strdup(name);n->v[n->n].ino=ino;n->v[n->n].type=type;n->n++;
}
static uint32_t alloc_ino(struct image *im){while(im->next_ino<im->ncg*IPG){uint32_t i=im->next_ino++,cg=i/IPG,x=i%IPG;if(!im->used_ino[cg][x]){im->used_ino[cg][x]=1;return i;}}fail("inode table full");return 0;}
static uint32_t alloc_block(struct image *im,const uint8_t *p,size_t n){
    for(uint32_t cg=0;cg<im->ncg;cg++){uint32_t s=(im->cg_next[cg]+7)&~7U,nd=cg_ndblk(im,cg);if(s+8>nd)continue;im->cg_next[cg]=s+8;for(unsigned j=0;j<8;j++)im->used_frag[cg][s+j]=1;uint32_t f=cg_base(im,cg)+s;if(n)memcpy(im->data+(size_t)f*FRAG,p,n);return f;}fail("UFS1 image full");return 0;
}
static uint32_t indirect(struct image *im,const uint32_t *blocks,size_t n,unsigned depth,uint32_t *meta){
    uint8_t raw[BLOCK]={0};size_t count=0;if(depth==1){for(size_t i=0;i<n;i++)p32(raw+4*count++,blocks[i]);}
    else {size_t span=1;for(unsigned d=1;d<depth;d++)span*=BLOCK/4;for(size_t pos=0;pos<n;pos+=span){size_t take=n-pos<span?n-pos:span;uint32_t q=indirect(im,blocks+pos,take,depth-1,meta);p32(raw+4*count++,q);}}
    (*meta)++;return alloc_block(im,raw,sizeof(raw));
}
static void write_inode(struct image *im,uint32_t ino,unsigned mode,unsigned links,uint64_t size,const uint32_t *blocks,size_t n){
    uint8_t *r=inode_at(im,ino);memset(r,0,INODE_SIZE);p16(r,mode);p16(r+2,links);p64(r+8,size);size_t direct=n<12?n:12;for(size_t i=0;i<direct;i++)p32(r+40+4*i,blocks[i]);
    size_t pos=direct;uint32_t meta=0;for(unsigned depth=1;depth<=3&&pos<n;depth++){size_t cap=1;for(unsigned d=0;d<depth;d++)cap*=BLOCK/4;size_t take=n-pos<cap?n-pos:cap;p32(r+88+4*(depth-1),indirect(im,blocks+pos,take,depth,&meta));pos+=take;}if(pos<n)fail("file exceeds triple indirect range");p32(r+104,(uint32_t)((n+meta)*16));p32(r+108,ino);
}
static uint32_t add_dir(struct image *im,uint32_t parent,const char *name,unsigned mode){uint32_t ino=alloc_ino(im);im->nodes[ino].mode=mode;write_inode(im,ino,IFDIR|mode,2,0,NULL,0);add_child(im,parent,name,ino,4);return ino;}
static uint32_t child_dir(struct image *im,uint32_t parent,const char *name){for(size_t i=0;i<im->nodes[parent].n;i++)if(im->nodes[parent].v[i].type==4&&!strcmp(im->nodes[parent].v[i].name,name))return im->nodes[parent].v[i].ino;return 0;}
static void add_file_data(struct image *im,uint32_t parent,const char *name,const uint8_t *data,size_t size,unsigned mode){
    uint32_t ino=alloc_ino(im);size_t nb=(size+BLOCK-1)/BLOCK;uint32_t *blocks=calloc(nb?nb:1,sizeof(*blocks));if(!blocks)die("calloc");for(size_t i=0;i<nb;i++){size_t n=size-i*BLOCK<BLOCK?size-i*BLOCK:BLOCK;blocks[i]=alloc_block(im,data+i*BLOCK,n);}write_inode(im,ino,IFREG|mode,1,size,blocks,nb);free(blocks);add_child(im,parent,name,ino,8);
}
static void add_symlink(struct image *im,uint32_t parent,const char *name,const char *target){size_t n=strlen(target);if(n>60)fail("inline symlink too long");uint32_t ino=alloc_ino(im);write_inode(im,ino,IFLNK|0777,1,n,NULL,0);memcpy(inode_at(im,ino)+40,target,n);add_child(im,parent,name,ino,10);}
static int namecmp(const void *a,const void *b){const char *const *x=a,*const *y=b;return strcmp(*x,*y);}
static void add_tree(struct image *im,const char *path,uint32_t parent){
    DIR *d=opendir(path);if(!d)die(path);char **names=NULL;size_t n=0,cap=0;struct dirent *e;while((e=readdir(d)))if(strcmp(e->d_name,".")&&strcmp(e->d_name,"..")){if(n==cap){cap=cap?cap*2:16;names=realloc(names,cap*sizeof(*names));if(!names)die("realloc");}names[n++]=strdup(e->d_name);}closedir(d);qsort(names,n,sizeof(*names),namecmp);
    for(size_t i=0;i<n;i++){size_t z=strlen(path)+strlen(names[i])+2;char *p=malloc(z);snprintf(p,z,"%s/%s",path,names[i]);struct stat st;if(lstat(p,&st))die(p);if(S_ISLNK(st.st_mode)){char target[256];ssize_t m=readlink(p,target,sizeof(target)-1);if(m<0)die(p);target[m]=0;add_symlink(im,parent,names[i],target);}else if(S_ISDIR(st.st_mode)){uint32_t ino=child_dir(im,parent,names[i]);if(!ino)ino=add_dir(im,parent,names[i],st.st_mode&07777);add_tree(im,p,ino);}else if(S_ISREG(st.st_mode)){FILE *f=fopen(p,"rb");if(!f)die(p);uint8_t *buf=malloc(st.st_size?st.st_size:1);if(st.st_size&&fread(buf,1,st.st_size,f)!=(size_t)st.st_size)die(p);fclose(f);add_file_data(im,parent,names[i],buf,st.st_size,st.st_mode&07777);free(buf);}free(p);free(names[i]);}free(names);
}
static void superblock(struct image *im){uint8_t *s=im->data+8192;struct {int o;uint32_t v;} v[]={{8,SBLK},{12,CBLK},{16,IBLK},{20,DBLK},{36,im->fragments},{40,0},{44,im->ncg},{48,BLOCK},{52,FRAG},{56,FRAGS},{80,13},{84,10},{96,3},{100,1},{104,1376},{116,BLOCK/4},{120,64},{160,im->cgsize},{184,IPG},{188,im->fpg},{1320,60},{1324,2},{1372,0x011954}};uint32_t dsize=0;for(uint32_t cg=0;cg<im->ncg;cg++){uint32_t n=cg_ndblk(im,cg);if(n>DBLK)dsize+=n-DBLK;}v[5].v=dsize;for(size_t i=0;i<sizeof(v)/sizeof(v[0]);i++)p32(s+v[i].o,v[i].v);p64(s+1328,0x7fffffffffffffffULL);s[144]=0x7a;s[145]=0x65;s[146]=0x64;s[147]=0x42;s[148]=0x53;s[149]=0x44;s[150]=1;s[151]=0;s[209]=1;}
static void finish_dirs(struct image *im){
    uint32_t parent[MAX_INODES]={0};parent[2]=2;for(uint32_t ino=2;ino<MAX_INODES;ino++)for(size_t j=0;j<im->nodes[ino].n;j++)if(im->nodes[ino].v[j].type==4)parent[im->nodes[ino].v[j].ino]=ino;
    for(uint32_t ino=2;ino<MAX_INODES;ino++){struct node *node=&im->nodes[ino];if(!node->mode)continue;size_t cap=1024,len=0,last=(size_t)-1;uint8_t *dir=calloc(1,cap);size_t total=node->n+2;for(size_t j=0;j<total;j++){const char *name=j==0?".":j==1?"..":node->v[j-2].name;uint32_t target=j==0?ino:j==1?parent[ino]:node->v[j-2].ino;uint8_t type=j<2?4:node->v[j-2].type;size_t nl=strlen(name),minimum=(8+nl+3)&~3U,within=len%512;if(within+minimum>512){p16(dir+last+4,512-(last%512));size_t pad=512-within;if(len+pad>cap){cap*=2;dir=realloc(dir,cap);memset(dir+len,0,cap-len);}len+=pad;last=(size_t)-1;}while(len+minimum>cap){size_t old=cap;cap*=2;dir=realloc(dir,cap);memset(dir+old,0,cap-old);}memset(dir+len,0,minimum);p32(dir+len,target);p16(dir+len+4,minimum);dir[len+6]=type;dir[len+7]=nl;memcpy(dir+len+8,name,nl);last=len;len+=minimum;}if(last!=(size_t)-1){p16(dir+last+4,512-(last%512));if(len%512){size_t end=(len+511)&~511U;while(end>cap){size_t old=cap;cap*=2;dir=realloc(dir,cap);memset(dir+old,0,cap-old);}memset(dir+len,0,end-len);len=end;}}
        size_t nb=(len+BLOCK-1)/BLOCK;uint32_t *blocks=calloc(nb,sizeof(*blocks));for(size_t j=0;j<nb;j++){size_t z=len-j*BLOCK<BLOCK?len-j*BLOCK:BLOCK;blocks[j]=alloc_block(im,dir+j*BLOCK,z);}unsigned sub=0;for(size_t j=0;j<node->n;j++)if(node->v[j].type==4)sub++;write_inode(im,ino,IFDIR|node->mode,2+sub,len,blocks,nb);free(blocks);free(dir);
    }
}
static void finish_cg(struct image *im){uint32_t totals[4]={0};for(uint32_t ci=0;ci<im->ncg;ci++){uint32_t base=cg_base(im,ci),nd=cg_ndblk(im,ci);uint8_t *cg=im->data+(base+CBLK)*FRAG;unsigned ndir=0,nifree=0,nbfree=0,nffree=0;for(unsigned i=0;i<IPG;i++){if(im->used_ino[ci][i]){if(ci*IPG+i<MAX_INODES&&im->nodes[ci*IPG+i].mode)ndir++;cg[168+i/8]|=1U<<(i&7);}else nifree++;}for(uint32_t f=0;f<nd;f++)if(!im->used_frag[ci][f])cg[200+f/8]|=1U<<(f&7);for(uint32_t f=0;f+8<=nd;f+=8){int all=1;for(unsigned j=0;j<8;j++)if(im->used_frag[ci][f+j])all=0;if(all)nbfree++;}unsigned freec=0;for(uint32_t f=0;f<nd;f++)if(!im->used_frag[ci][f])freec++;nffree=freec-nbfree*8;struct{int o;uint32_t v;}v[]={{4,0x090255},{12,ci},{20,nd},{24,ndir},{28,nbfree},{32,nifree},{36,nffree},{92,168},{96,200},{100,im->cgsize},{116,IPG},{120,IPG}};for(size_t i=0;i<sizeof(v)/sizeof(v[0]);i++)p32(cg+v[i].o,v[i].v);totals[0]+=ndir;totals[1]+=nbfree;totals[2]+=nifree;totals[3]+=nffree;}
    uint8_t *s=im->data+8192;for(int i=0;i<4;i++)p32(s+192+4*i,totals[i]);for(uint32_t ci=1;ci<im->ncg;ci++)memcpy(im->data+(cg_base(im,ci)+SBLK)*FRAG,s,8192);
}
static void create_ufs(const char *root,const char *out,size_t size){struct image im={0};if(size<4*1024*1024||size%FRAG)fail("bad UFS1 size");im.size=size;im.data=calloc(1,size);if(!im.data)die("calloc image");im.fragments=size/FRAG;im.ncg=1;im.fpg=(im.fragments+7)&~7U;im.cgsize=200+(im.fpg+7)/8;if(im.cgsize>BLOCK)fail("cylinder group bitmap too large");im.cg_next[0]=DBLK;im.used_frag[0]=calloc(im.fpg,1);for(uint32_t i=0;i<DBLK&&i<im.fpg;i++)im.used_frag[0][i]=1;im.used_ino[0][0]=im.used_ino[0][1]=im.used_ino[0][2]=1;im.next_ino=3;im.nodes[2].mode=0755;superblock(&im);write_inode(&im,2,IFDIR|0755,2,0,NULL,0);add_tree(&im,root,2);if(!child_dir(&im,2,"etc")){uint32_t etc=add_dir(&im,2,"etc",0755);static const uint8_t marker[]="zedBSD ufs1 root v1\n";add_file_data(&im,etc,"zedbsd-root",marker,sizeof(marker)-1,0644);}finish_dirs(&im);finish_cg(&im);FILE*f=fopen(out,"wb");if(!f)die(out);if(fwrite(im.data,1,size,f)!=size)die(out);if(fclose(f))die(out);}

static uint32_t crc32_more(uint32_t crc,const uint8_t *p,size_t n){crc^=0xffffffffU;while(n--){crc^=*p++;for(int j=0;j<8;j++)crc=(crc>>1)^((crc&1)?0xedb88320U:0);}return crc^0xffffffffU;}
static uint8_t *read_all(const char *path,size_t *size){struct stat st;if(stat(path,&st))die(path);FILE*f=fopen(path,"rb");if(!f)die(path);uint8_t*p=malloc(st.st_size?st.st_size:1);if(!p)die("malloc");if(st.st_size&&fread(p,1,st.st_size,f)!=(size_t)st.st_size)die(path);fclose(f);*size=st.st_size;return p;}
static void seek_write(FILE*f,uint64_t at,const void*p,size_t n){if(fseeko(f,(off_t)at,SEEK_SET)||fwrite(p,1,n,f)!=n)die("disk image write");}
static void shell_copy(const char *image,uint64_t offset,const char *source,const char *dest){char cmd[16384];snprintf(cmd,sizeof(cmd),"mcopy -o -i '%s'@@%llu '%s' '::%s'",image,(unsigned long long)offset,source,dest);if(system(cmd))fail("mcopy failed");}
static void shell_mkdir(const char *image,uint64_t offset,const char *dest){char cmd[16384];snprintf(cmd,sizeof(cmd),"mmd -i '%s'@@%llu '::%s' >/dev/null 2>&1 || true",image,(unsigned long long)offset,dest);if(system(cmd))fail("mmd failed");}
static uint32_t random_serial(void){uint32_t value=0;int fd=open("/dev/urandom",O_RDONLY);if(fd>=0){ssize_t n=read(fd,&value,sizeof(value));close(fd);if(n==(ssize_t)sizeof(value)&&value)return value;}struct timespec now;if(clock_gettime(CLOCK_REALTIME,&now))die("clock_gettime");value=(uint32_t)now.tv_nsec^(uint32_t)now.tv_sec^(uint32_t)getpid()^(uint32_t)(uintptr_t)&value;return value?value:1;}
static void random_guid(uint8_t guid[16]){for(size_t i=0;i<4;i++)p32(guid+i*4,random_serial()^(uint32_t)(0x9e3779b9U*i));guid[7]=(guid[7]&0x0f)|0x40;guid[8]=(guid[8]&0x3f)|0x80;}
static int guid_matches_any(const uint8_t guid[16],const uint8_t *others,size_t count){for(size_t i=0;i<count;i++)if(!memcmp(guid,others+i*16,16))return 1;return 0;}
static void shell_format_pcat(const char *image,uint64_t offset,uint64_t start,uint64_t blocks,int fat32,const char *label,uint32_t serial){char cmd[16384];if(fat32)snprintf(cmd,sizeof(cmd),"mformat -i '%s'@@%llu -F -N 0x%08x -R 32 -H %llu -T %llu -v %s ::",image,(unsigned long long)offset,serial,(unsigned long long)start,(unsigned long long)blocks,label);else snprintf(cmd,sizeof(cmd),"mformat -i '%s'@@%llu -N 0x%08x -R 4 -H %llu -T %llu -v %s ::",image,(unsigned long long)offset,serial,(unsigned long long)start,(unsigned long long)blocks,label);if(system(cmd))fail("mformat failed");}
static uint32_t fat32_entry(FILE *f,uint64_t at,uint32_t cluster){uint8_t raw[4];if(fseeko(f,(off_t)(at+(uint64_t)cluster*4),SEEK_SET)||fread(raw,1,4,f)!=4)die("read FAT32 entry");return g32(raw)&0x0fffffff;}
static void fat32_set_entry(FILE *f,uint64_t at,uint32_t sectors,uint8_t copies,uint32_t cluster,uint32_t value){for(uint8_t i=0;i<copies;i++){uint64_t pos=at+(uint64_t)i*sectors*512+(uint64_t)cluster*4;uint8_t raw[4];if(fseeko(f,(off_t)pos,SEEK_SET)||fread(raw,1,4,f)!=4)die("read FAT32 entry");p32(raw,(g32(raw)&0xf0000000)|(value&0x0fffffff));if(fseeko(f,(off_t)pos,SEEK_SET)||fwrite(raw,1,4,f)!=4)die("write FAT32 entry");}}
static void fragment_fat32_root_file(const char *image,uint64_t offset,const char name[11]){FILE*f=fopen(image,"rb+");if(!f)die(image);uint8_t b[512];if(fseeko(f,(off_t)offset,SEEK_SET)||fread(b,1,512,f)!=512)die("read FAT32 BPB");uint16_t reserved=(uint16_t)b[14]|(uint16_t)b[15]<<8;uint8_t copies=b[16],spc=b[13];uint32_t fat_sectors=g32(b+36),root=g32(b+44)&0x0fffffff,total=g32(b+32),metadata=reserved+(uint32_t)copies*fat_sectors;if(((uint16_t)b[11]|(uint16_t)b[12]<<8)!=512||!reserved||!copies||!spc||!fat_sectors||root<2||metadata>=total)fail("invalid FAT32 fragmentation geometry");uint64_t fat=offset+(uint64_t)reserved*512,data=offset+(uint64_t)metadata*512;uint32_t data_sectors=total-metadata,limit=data_sectors/spc+2,first=0;if(root>=limit||limit<18||(uint64_t)limit*4>(uint64_t)fat_sectors*512)fail("invalid FAT32 fragmentation bounds");uint64_t root_at=data+(uint64_t)(root-2)*spc*512;for(uint32_t i=0;i<(uint32_t)spc*16;i++){if(fseeko(f,(off_t)(root_at+(uint64_t)i*32),SEEK_SET)||fread(b,1,32,f)!=32)die("read FAT32 root");if(!b[0])break;if(b[0]!=0xe5&&b[11]!=0x0f&&!memcmp(b,name,11)){first=(uint32_t)b[26]|(uint32_t)b[27]<<8|(uint32_t)b[20]<<16|(uint32_t)b[21]<<24;first&=0x0fffffff;break;}}if(first<2||first>=limit)fail("fragmentation target absent");uint32_t victim=fat32_entry(f,fat,first);if(victim<2||victim>=limit)fail("fragmentation target is too short");uint32_t next=fat32_entry(f,fat,victim),replacement=limit-16;if(!((next>=2&&next<limit)||next>=0x0ffffff8))fail("invalid fragmentation target chain");while(replacement>2&&fat32_entry(f,fat,replacement)!=0)replacement--;if(replacement<=2||replacement==first||replacement==victim)fail("no FAT32 fragmentation cluster");size_t bytes=(size_t)spc*512;uint8_t*cluster=malloc(bytes);if(!cluster)die("malloc fragmentation cluster");uint64_t from=data+(uint64_t)(victim-2)*bytes,to=data+(uint64_t)(replacement-2)*bytes;if(fseeko(f,(off_t)from,SEEK_SET)||fread(cluster,1,bytes,f)!=bytes)die("read fragmentation cluster");if(fseeko(f,(off_t)to,SEEK_SET)||fwrite(cluster,1,bytes,f)!=bytes)die("write fragmentation cluster");free(cluster);fat32_set_entry(f,fat,fat_sectors,copies,first,replacement);fat32_set_entry(f,fat,fat_sectors,copies,replacement,next);fat32_set_entry(f,fat,fat_sectors,copies,victim,0);if(fclose(f))die("close fragmented FAT32 image");}
static void pc98_chs(uint8_t out[4],uint32_t lba,uint32_t heads){uint32_t cyl=lba/(heads*17),rem=lba%(heads*17),head=rem/17,sec=rem%17;if(cyl>65535)fail("PC-98 CHS overflow");out[0]=sec;out[1]=head;p16(out+2,cyl);}
static void gpt_entry(uint8_t *e,const uint8_t type[16],const uint8_t unique[16],uint64_t first,uint64_t last,const char *name){memcpy(e,type,16);memcpy(e+16,unique,16);p64(e+32,first);p64(e+40,last);for(size_t i=0;name[i]&&i<36;i++)p16(e+56+2*i,(uint8_t)name[i]);}
static void gpt_header(uint8_t h[512],uint64_t cur,uint64_t backup,uint64_t entries,uint64_t first,uint64_t last,const uint8_t diskid[16],uint32_t ecrc){memset(h,0,512);memcpy(h,"EFI PART",8);p32(h+8,0x10000);p32(h+12,92);p64(h+24,cur);p64(h+32,backup);p64(h+40,first);p64(h+48,last);memcpy(h+56,diskid,16);p64(h+72,entries);p32(h+80,128);p32(h+84,128);p32(h+88,ecrc);p32(h+16,crc32_more(0,h,92));}
struct diskopt {const char *machine,*stage1,*stage2,*pbr,*bootzbsd,*kernel,*bootx64,*zedbsd_config,*arch,*data,*swap,*ufs_root,*output,*layout;int gpt,force,fragment_kernel,size_mib,fat_mib;};
static void disk_create_variant(struct diskopt *o);
static void disk_create(struct diskopt *o){
    if(o->layout){disk_create_variant(o);return;}
    if(!o->machine||!o->stage1||!o->stage2||!o->pbr||!o->bootzbsd||!o->kernel||!o->output){fail("incomplete disk arguments");}
    size_t s1n,s2n,pbrn,kn,ufsn=0;uint8_t*s1=read_all(o->stage1,&s1n),*s2=read_all(o->stage2,&s2n),*pbr=read_all(o->pbr,&pbrn),*kernel=read_all(o->kernel,&kn),*ufs=o->ufs_root?read_all(o->ufs_root,&ufsn):NULL;if(s1n!=512||s1[510]!=0x55||s1[511]!=0xaa||s2n%512||pbrn!=2048||ufsn%512)fail("invalid bootloader input");
    uint64_t sectors=(uint64_t)o->size_mib*2048,esp_start=2048,esp_blocks=o->gpt?64U*2048U:0,start=o->gpt?esp_start+esp_blocks:2048,blocks=(uint64_t)o->fat_mib*2048,offset=start*512,esp_offset=esp_start*512,stage_lba=o->gpt?34:!strcmp(o->machine,"pc98")?2:1;if(!o->gpt&&!o->ufs_root&&o->fat_mib==128&&o->size_mib==129)blocks=sectors-start;if(o->gpt){uint64_t minimum=start+blocks+33;if(o->ufs_root)fail("GPT overlay image cannot include a native UFS root");if(sectors<minimum)sectors=(minimum+2047)&~(uint64_t)2047;}uint64_t root_start=start+blocks,root_blocks=ufsn/512;if(root_blocks&&root_start+root_blocks>sectors)fail("UFS root exceeds disk image");if(stage_lba+s2n/512>(o->gpt?esp_start:start))fail("stage2 too large");char tmp[4096];snprintf(tmp,sizeof(tmp),"%s.tmp",o->output);FILE*f=fopen(tmp,"wb+");if(!f)die(tmp);if(ftruncate(fileno(f),(off_t)(sectors*512)))die("truncate image");
    if(strcmp(o->machine,"pc98")){
        uint32_t sig=crc32_more(0,s2,s2n);sig=crc32_more(sig,kernel,kn);if(!sig)sig=1;p32(s1+0x1b8,sig);
        uint8_t *e=s1+0x1be;e[0]=0x80;e[1]=0xfe;e[2]=0xff;e[3]=0xff;e[4]=o->gpt?0x0c:0x0e;e[5]=0xfe;e[6]=0xff;e[7]=0xff;p32(e+8,start);p32(e+12,blocks);
        if(o->gpt){e=s1+0x1ce;e[2]=2;e[4]=0xee;e[5]=0xfe;e[6]=0xff;e[7]=0xff;p32(e+8,1);p32(e+12,sectors-1>0xffffffffU?0xffffffffU:(uint32_t)(sectors-1));}
        else if(root_blocks){e=s1+0x1ce;e[4]=0xa5;e[5]=0xfe;e[6]=0xff;e[7]=0xff;p32(e+8,root_start);p32(e+12,root_blocks);}
    }
    seek_write(f,0,s1,512);
    if(!strcmp(o->machine,"pc98")){uint32_t heads=o->size_mib<=20?4:8;uint8_t pe[64]={0};pe[0]=0xa1;pe[1]=0x91;pc98_chs(pe+4,start,heads);pc98_chs(pe+8,start,heads);pc98_chs(pe+12,(root_blocks?root_start:sectors)-1,heads);memset(pe+16,' ',16);memcpy(pe+16,"BOOT",4);if(root_blocks){pe[32]=0x21;pe[33]=0x01;pc98_chs(pe+36,root_start,heads);pc98_chs(pe+40,root_start,heads);pc98_chs(pe+44,root_start+root_blocks-1,heads);memset(pe+48,' ',16);memcpy(pe+48,"ROOT",4);}seek_write(f,512,pe,root_blocks?64:32);}seek_write(f,stage_lba*512,s2,s2n);if(root_blocks)seek_write(f,root_start*512,ufs,ufsn);
    if(o->gpt){static const uint8_t esp[16]={0x28,0x73,0x2a,0xc1,0x1f,0xf8,0xd2,0x11,0xba,0x4b,0x00,0xa0,0xc9,0x3e,0xc9,0x3b};static const uint8_t basic[16]={0xa2,0xa0,0xd0,0xeb,0xe5,0xb9,0x33,0x44,0x87,0xc0,0x68,0xb6,0xb7,0x26,0x99,0xc7};static const uint8_t bios[16]={0x48,0x61,0x68,0x21,0x49,0x64,0x6f,0x6e,0x74,0x4e,0x65,0x65,0x64,0x45,0x46,0x49};uint8_t*entries=calloc(1,128*128),h[512],disk_guid[16],partition_guids[3][16];if(!entries)die("calloc GPT entries");random_guid(disk_guid);for(size_t i=0;i<3;i++){do random_guid(partition_guids[i]);while(!memcmp(partition_guids[i],disk_guid,16)||guid_matches_any(partition_guids[i],partition_guids[0],i));}gpt_entry(entries,esp,partition_guids[0],esp_start,esp_start+esp_blocks-1,"zedBSD EFI System");gpt_entry(entries+128,basic,partition_guids[1],start,start+blocks-1,"zedBSD Payload");gpt_entry(entries+256,bios,partition_guids[2],34,esp_start-1,"zedBSD BIOS loader");uint32_t ec=crc32_more(0,entries,128*128);gpt_header(h,1,sectors-1,2,34,sectors-34,disk_guid,ec);seek_write(f,512,h,512);seek_write(f,1024,entries,128*128);seek_write(f,(sectors-33)*512,entries,128*128);gpt_header(h,sectors-1,1,sectors-33,34,sectors-34,disk_guid,ec);seek_write(f,(sectors-1)*512,h,512);free(entries);}if(fclose(f))die("close disk image before formatting");
    char cmd[16384];uint32_t boot_serial=random_serial(),esp_serial=random_serial();if(esp_serial==boot_serial)esp_serial^=0x80000000U;if(!esp_serial)esp_serial=1;if(o->gpt)shell_format_pcat(tmp,esp_offset,esp_start,esp_blocks,1,"ESP",esp_serial);if(!strcmp(o->machine,"pc98")){uint32_t heads=o->size_mib<=20?4:8,logical=blocks/2,cluster=1;while(logical/cluster>=65525)cluster*=2;snprintf(cmd,sizeof(cmd),"mformat -i '%s'@@%llu -S 3 -N 0x%08x -R 4 -c %u -h %u -s 17 -H 2048 -T %llu -v BOOT ::",tmp,(unsigned long long)offset,boot_serial,cluster,heads,(unsigned long long)logical);if(system(cmd))fail("mformat failed");}else shell_format_pcat(tmp,offset,start,blocks,o->gpt,"BOOT",boot_serial);
    f=fopen(tmp,"rb+");if(!f)die(tmp);uint8_t bpb[2048];if(fseeko(f,(off_t)offset,SEEK_SET)||fread(bpb,1,2048,f)!=2048)die("read BPB");int fat32=bpb[22]==0&&bpb[23]==0;memcpy(pbr+3,bpb+3,(fat32?0x5a:0x3e)-3);if(fat32){uint16_t reserved=(uint16_t)bpb[14]|(uint16_t)bpb[15]<<8;uint16_t backup=(uint16_t)bpb[50]|(uint16_t)bpb[51]<<8;if(!backup||backup>=reserved)fail("invalid FAT32 backup boot sector");seek_write(f,offset,pbr,512);seek_write(f,offset+(uint64_t)backup*512,pbr,512);seek_write(f,offset+1024,pbr+512,1536);}else seek_write(f,offset,pbr,2048);if(fclose(f))die("close installed partition PBR");shell_copy(tmp,offset,o->bootzbsd,"/BOOTZBSD.EXE");shell_copy(tmp,offset,o->kernel,o->gpt?"/vmunix":"/VMUNIX");if(o->zedbsd_config)shell_copy(tmp,offset,o->zedbsd_config,!strcmp(o->machine,"pc98")?"/BOOTZBSD.CFG":o->gpt?"/zedbsd.cfg":"/ZEDBSD.CFG");if(o->gpt){if(!o->bootx64)fail("GPT image requires BOOTX64.EFI");shell_mkdir(tmp,esp_offset,"/EFI");shell_mkdir(tmp,esp_offset,"/EFI/BOOT");shell_copy(tmp,esp_offset,o->bootx64,"/EFI/BOOT/BOOTX64.EFI");}if(o->arch)shell_copy(tmp,offset,o->arch,o->gpt?"/rootfs.img":"/ROOTFS.IMG");if(o->data)shell_copy(tmp,offset,o->data,o->gpt?"/data.img":"/DATA.IMG");if(o->swap)shell_copy(tmp,offset,o->swap,o->gpt?"/swapfile":"/SWAPFILE");if(o->fragment_kernel){if(!fat32)fail("kernel fragmentation requires FAT32");fragment_fat32_root_file(tmp,offset,"VMUNIX     ");}if(rename(tmp,o->output))die("publish disk image");free(s1);free(s2);free(pbr);free(kernel);free(ufs);
}

static uint64_t parse_u64(const char *text,const char *what){
    char *end=NULL;unsigned long long value;
    if(!text[0])fail(what);
    for(const char *p=text;*p;p++)if(*p<'0'||*p>'9')fail(what);
    errno=0;value=strtoull(text,&end,10);
    if(errno||!end||*end)fail(what);
    return (uint64_t)value;
}
static int parse_positive_int(const char *text,const char *what){
    uint64_t value=parse_u64(text,what);
    if(value==0U||value>(uint64_t)INT_MAX)fail(what);
    return (int)value;
}
static void disk_create_uefi(struct diskopt *o){
    static const uint8_t esp_type[16]={0x28,0x73,0x2a,0xc1,0x1f,0xf8,0xd2,0x11,0xba,0x4b,0x00,0xa0,0xc9,0x3e,0xc9,0x3b};
    static const uint8_t basic_type[16]={0xa2,0xa0,0xd0,0xeb,0xe5,0xb9,0x33,0x44,0x87,0xc0,0x68,0xb6,0xb7,0x26,0x99,0xc7};
    const uint64_t esp_start=2048U,esp_blocks=64U*2048U;
    const uint64_t payload_start=esp_start+esp_blocks,payload_blocks=(uint64_t)o->fat_mib*2048U;
    uint64_t sectors,last,bytes;
    uint8_t mbr[512]={0},entries[128U*128U]={0},header[512],disk_guid[16],partition_guids[2][16];
    size_t kernel_size,bootx64_size;uint8_t *kernel,*bootx64;char temporary[4096];FILE *file;
    uint32_t entry_crc,boot_serial,esp_serial;

    if(!o->machine||strcmp(o->machine,"pcat")||!o->kernel||!o->bootx64||
        !o->zedbsd_config||!o->arch||!o->data||!o->swap||!o->output)
        fail("incomplete UEFI-only disk arguments");
    if(o->gpt||o->ufs_root||o->fragment_kernel)fail("UEFI-only disk received an incompatible legacy option");
    if(o->fat_mib<=0)fail("invalid payload FAT size");
    if(payload_start>UINT64_MAX-payload_blocks)fail("payload extent overflow");
    sectors=payload_start+payload_blocks+33U;
    last=sectors-1U;
    if(sectors>UINT64_MAX/512U)fail("UEFI-only image size overflow");
    bytes=sectors*512U;
    if(bytes>(uint64_t)INT64_MAX)fail("UEFI-only image exceeds host offset range");

    kernel=read_all(o->kernel,&kernel_size);bootx64=read_all(o->bootx64,&bootx64_size);
    if(kernel_size==0||bootx64_size==0)fail("empty UEFI-only payload input");
    snprintf(temporary,sizeof(temporary),"%s.tmp",o->output);
    file=fopen(temporary,"wb+");if(!file)die(temporary);
    if(ftruncate(fileno(file),(off_t)bytes))die("truncate UEFI-only image");

    mbr[0x1beU+2U]=2U;mbr[0x1beU+4U]=0xeeU;
    mbr[0x1beU+5U]=0xffU;mbr[0x1beU+6U]=0xffU;mbr[0x1beU+7U]=0xffU;
    p32(mbr+0x1beU+8U,1U);
    p32(mbr+0x1beU+12U,(uint32_t)last);mbr[510]=0x55;mbr[511]=0xaa;
    seek_write(file,0,mbr,sizeof(mbr));

    random_guid(disk_guid);
    for(size_t i=0;i<2;i++){
        do random_guid(partition_guids[i]);
        while(!memcmp(partition_guids[i],disk_guid,16)||guid_matches_any(partition_guids[i],partition_guids[0],i));
    }
    gpt_entry(entries,esp_type,partition_guids[0],esp_start,esp_start+esp_blocks-1U,"zedBSD EFI System");
    gpt_entry(entries+128U,basic_type,partition_guids[1],payload_start,payload_start+payload_blocks-1U,"zedBSD Payload");
    entry_crc=crc32_more(0,entries,sizeof(entries));
    gpt_header(header,1U,last,2U,34U,last-33U,disk_guid,entry_crc);
    seek_write(file,512U,header,sizeof(header));seek_write(file,1024U,entries,sizeof(entries));
    if(fclose(file))die("close compact UEFI image before formatting");

    boot_serial=random_serial();esp_serial=random_serial();
    if(esp_serial==boot_serial)esp_serial^=0x80000000U;
    if(!esp_serial)esp_serial=1;
    shell_format_pcat(temporary,esp_start*512U,esp_start,esp_blocks,1,"ESP",esp_serial);
    shell_format_pcat(temporary,payload_start*512U,payload_start,payload_blocks,1,"BOOT",boot_serial);
    shell_mkdir(temporary,esp_start*512U,"/EFI");shell_mkdir(temporary,esp_start*512U,"/EFI/BOOT");
    shell_copy(temporary,esp_start*512U,o->bootx64,"/EFI/BOOT/BOOTX64.EFI");
    shell_copy(temporary,payload_start*512U,o->kernel,"/vmunix");
    if(o->zedbsd_config)shell_copy(temporary,payload_start*512U,o->zedbsd_config,"/zedbsd.cfg");
    if(o->arch)shell_copy(temporary,payload_start*512U,o->arch,"/rootfs.img");
    if(o->data)shell_copy(temporary,payload_start*512U,o->data,"/data.img");
    if(o->swap)shell_copy(temporary,payload_start*512U,o->swap,"/swapfile");
    if(rename(temporary,o->output))die("publish compact UEFI disk image");
    free(kernel);free(bootx64);
}
static int stage1_loads_lba(const char *path,uint32_t expected){
    const size_t at=0x1a0U;size_t size;uint8_t *data;
    if(!path)return 0;
    data=read_all(path,&size);
    int matches=size==512U&&g32(data+at)==0x314c425aU&&
        data[at+4U]==1U&&data[at+5U]==0U&&
        data[at+6U]==16U&&data[at+7U]==0U&&g32(data+at+8U)==expected&&
        data[at+12U]==1U&&data[at+13U]==0U&&
        data[at+14U]==0U&&data[at+15U]==0U;
    for(size_t i=at+16U;matches&&i<440U;i++)if(data[i]!=0U)matches=0;
    free(data);return matches;
}
static void disk_create_variant(struct diskopt *o){
    const char *layout=o->layout;
    if(!o->machine||strcmp(o->machine,"pcat"))
        fail("selected amd64 disk layout requires machine pcat");
    if(o->size_mib!=129||o->fat_mib!=128||o->ufs_root||o->fragment_kernel)
        fail("selected disk layout received a legacy geometry or mode override");
    if(!strcmp(layout,"uefi")){disk_create_uefi(o);return;}
    if(strcmp(layout,"hybrid")&&strcmp(layout,"bios"))fail("unsupported disk layout");
    if(!o->stage1||!o->stage2||!o->pbr||!o->bootzbsd||!o->kernel||
        !o->zedbsd_config||!o->arch||!o->data||!o->swap||!o->output)
        fail("incomplete selected disk layout arguments");
    if(!strcmp(layout,"hybrid")&&!o->bootx64)
        fail("Hybrid disk layout requires BOOTX64.EFI");
    if(!strcmp(layout,"bios")&&o->gpt)fail("BIOS-only layout cannot use GPT");
    if(!stage1_loads_lba(o->stage1,!strcmp(layout,"hybrid")?34U:1U))
        fail("invalid Stage 1 layout metadata or Stage 2 LBA");
    o->layout=NULL;o->gpt=!strcmp(layout,"hybrid");disk_create(o);o->layout=layout;
}
static void parse_disk(int argc,char **argv){struct diskopt o={.size_mib=129,.fat_mib=128};for(int i=2;i<argc;i++){char*a=argv[i];if(!strcmp(a,"--gpt"))o.gpt=1;else if(!strcmp(a,"--force"))o.force=1;else if(!strcmp(a,"--fragment-kernel"))o.fragment_kernel=1;else if(!strcmp(a,"--machine")&&++i<argc)o.machine=argv[i];else if(!strcmp(a,"--stage1")&&++i<argc)o.stage1=argv[i];else if(!strcmp(a,"--stage2")&&++i<argc)o.stage2=argv[i];else if(!strcmp(a,"--partition-pbr")&&++i<argc)o.pbr=argv[i];else if(!strcmp(a,"--bootzbsd")&&++i<argc)o.bootzbsd=argv[i];else if(!strcmp(a,"--kernel")&&++i<argc)o.kernel=argv[i];else if(!strcmp(a,"--bootx64")&&++i<argc)o.bootx64=argv[i];else if(!strcmp(a,"--zedbsd-config")&&++i<argc)o.zedbsd_config=argv[i];else if(!strcmp(a,"--arch-image")&&++i<argc)o.arch=argv[i];else if(!strcmp(a,"--data-image")&&++i<argc)o.data=argv[i];else if(!strcmp(a,"--swapfile")&&++i<argc)o.swap=argv[i];else if(!strcmp(a,"--ufs-root")&&++i<argc)o.ufs_root=argv[i];else if(!strcmp(a,"--layout")&&++i<argc)o.layout=argv[i];else if(!strcmp(a,"--size-mib")&&++i<argc)o.size_mib=parse_positive_int(argv[i],"invalid disk size");else if(!strcmp(a,"--fat-size-mib")&&++i<argc)o.fat_mib=parse_positive_int(argv[i],"invalid FAT size");else if((!strcmp(a,"--checker")||!strcmp(a,"--arch-profile")||!strcmp(a,"--arch-format"))&&++i<argc){}else if(a[0]!='-')o.output=a;else fail("unsupported disk argument");}disk_create(&o);}
int main(int argc,char **argv){if(argc>=2&&!strcmp(argv[1],"ufs")){if(argc!=5)fail("usage: zedimage-host ufs SIZE ROOT OUTPUT");char *end;unsigned long long size=strtoull(argv[2],&end,0);if(*end)fail("invalid size");create_ufs(argv[3],argv[4],(size_t)size);return 0;}if(argc>=2&&!strcmp(argv[1],"disk")){parse_disk(argc,argv);return 0;}fail("usage: zedimage-host ufs|disk ...");return 1;}
