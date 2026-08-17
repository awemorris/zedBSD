/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/ufs-consistency.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define SECTORS 64U
struct medium { unsigned char live[SECTORS][512],durable[SECTORS][512];unsigned op,fail,fail_from; };
static int inject(struct medium *m){m->op++;return (m->fail==m->op||
	(m->fail_from!=0&&m->op>=m->fail_from))?EIO:0;}
static int read_io(void *p,uint64_t lba,uint32_t count,void *out)
{struct medium*m=p;if(lba+count>SECTORS)return EIO;memcpy(out,m->live[lba],count*512U);return 0;}
static int write_io(void *p,uint64_t lba,uint32_t count,const void *in)
{struct medium*m=p;if(lba+count>SECTORS)return EIO;if(inject(m))return EIO;memcpy(m->live[lba],in,count*512U);return 0;}
static int flush_io(void *p)
{struct medium*m=p;if(inject(m))return EIO;memcpy(m->durable,m->live,sizeof(m->live));return 0;}
static void crash(struct medium*m){memcpy(m->live,m->durable,sizeof(m->live));m->fail=0;m->fail_from=0;m->op=0;}

static void journal_test(void)
{
	struct ufs_journal_io io;struct ufs_journal journal;struct medium base,m;
	unsigned char payload[1024],old[1024];unsigned cut;int result;
	memset(&base,0,sizeof(base));memset(payload,0xa5,sizeof(payload));
	memset(old,0x3c,sizeof(old));memcpy(base.live[40],old,sizeof(old));
	memcpy(base.durable,base.live,sizeof(base.live));
	io.context=&m;io.read=read_io;io.write=write_io;io.flush=flush_io;
	for(cut=1;cut<20;cut++){
		m=base;m.fail=cut;
		assert(ufs_journal_init(&journal,&io,4,8)==0);
		result=ufs_journal_commit(&journal,40,payload,2);
		if(result==0)assert(!memcmp(m.durable[40],payload,sizeof(payload)));
		crash(&m);assert(ufs_journal_replay(&journal)==0);
		assert(!memcmp(m.durable[40],old,sizeof(old))||
		    !memcmp(m.durable[40],payload,sizeof(payload)));
		assert(!memcmp(m.live[40],m.durable[40],sizeof(payload)));
		if(result==0)break;
	}
	assert(cut<20);
	/* A committed record with corrupt payload is rejected without home I/O. */
	m=base;m.fail_from=8;assert(ufs_journal_init(&journal,&io,4,8)==0);
	assert(ufs_journal_commit(&journal,40,payload,2)==EIO);crash(&m);
	assert(ufs_journal_init(&journal,&io,4,8)==0);
	m.live[5][7]^=1;memcpy(m.durable,m.live,sizeof(m.live));
	assert(ufs_journal_replay(&journal)==EIO);
	assert(!memcmp(m.durable[40],old,sizeof(old)));
	assert(ufs_journal_commit(&journal,5,payload,2)==EINVAL);
	assert(ufs_journal_init(&journal,&io,UINT64_MAX-1U,3)==EINVAL);
	journal.next_sequence=UINT64_MAX;
	assert(ufs_journal_commit(&journal,40,payload,2)==EOVERFLOW);
	/* If recovery itself cannot make the transaction slot definite, further
	 * commits are rejected until the owner remounts and replays it. */
	m=base;m.fail_from=2;
	assert(ufs_journal_init(&journal,&io,4,8)==0);
	assert(ufs_journal_commit(&journal,40,payload,2)==EIO);
	assert(journal.poisoned);
	assert(ufs_journal_commit(&journal,40,payload,2)==EIO);
}

struct order { uint64_t values[8];unsigned count;unsigned fail; };
static int record(void *p,uint64_t block)
{struct order*o=p;if(o->fail==o->count+1U)return EIO;o->values[o->count++]=block;return 0;}
static void softdep_test(void)
{
	struct ufs_softdep dep;struct order order;uint64_t bitmap,inode,dirent;
	ufs_softdep_init(&dep);assert(ufs_softdep_add(&dep,10,&bitmap)==0);
	assert(ufs_softdep_add(&dep,20,&inode)==0);
	assert(ufs_softdep_add(&dep,30,&dirent)==0);
	assert(ufs_softdep_depend(&dep,inode,bitmap)==0);
	assert(ufs_softdep_depend(&dep,dirent,inode)==0);
	memset(&order,0,sizeof(order));assert(ufs_softdep_drain(&dep,record,&order)==0);
	assert(order.count==3&&order.values[0]==10&&order.values[1]==20&&order.values[2]==30);
	ufs_softdep_init(&dep);assert(ufs_softdep_add(&dep,1,&bitmap)==0);
	assert(ufs_softdep_add(&dep,2,&inode)==0);
	assert(ufs_softdep_depend(&dep,bitmap,inode)==0);
	assert(ufs_softdep_depend(&dep,inode,bitmap)==0);
	memset(&order,0,sizeof(order));assert(ufs_softdep_drain(&dep,record,&order)==EDEADLK);
	ufs_softdep_init(&dep);assert(ufs_softdep_add(&dep,10,&bitmap)==0);
	assert(ufs_softdep_add(&dep,20,&inode)==0);
	assert(ufs_softdep_add(&dep,30,&dirent)==0);
	assert(ufs_softdep_depend(&dep,inode,bitmap)==0);
	assert(ufs_softdep_depend(&dep,dirent,inode)==0);
	memset(&order,0,sizeof(order));order.fail=2;
	assert(ufs_softdep_drain(&dep,record,&order)==EIO);
	assert(order.count==1&&order.values[0]==10&&dep.count==2);
	order.fail=0;
	assert(ufs_softdep_drain(&dep,record,&order)==0);
	assert(order.count==3&&order.values[1]==20&&order.values[2]==30);
}

int main(void){journal_test();softdep_test();puts("zedBSD UFS journal/softdep core tests: PASS");return 0;}
