/* Production journal recovery across volatile media and interrupted slot reuse. */
#include <errno.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../temp/p031-driver-fragments/src/drivers/fs/ufs/ufs-consistency.h"
#define SECTOR 512U
#define SECTORS 64U
#define FIRST 40U
#define COUNT 12U
#define COMMIT_MAGIC 0x434a4655U
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"journal-crash:%d: %s (cut=%u mode=%u op=%u)\n",__LINE__,#x,cut,mode,operations); exit(1); } } while (0)
static unsigned use_image, read_calls;
static unsigned char redo_image[UFS_JOURNAL_IMAGE_BYTES];
static unsigned checks,cut,mode,operations,commit_written,commit_durable,stop_commit,lose_commit,fail_write_at,fail_flush_at,home_reads,max_read;
static unsigned char stable[SECTORS][SECTOR],cache[SECTORS][SECTOR];
static unsigned char saved[SECTORS][SECTOR],replay_base[SECTORS][SECTOR],payload[3*SECTOR],empty[SECTOR];
static jmp_buf power_loss;
static struct ufs_journal journal;
static uint32_t get32(const unsigned char *p)
{ return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static int read_media(void *context,uint64_t first,uint32_t count,void *data)
{
 (void)context;CHECK(first<SECTORS && count<=SECTORS-first);
 read_calls++;
 if(first<FIRST)home_reads++;
 if(count>max_read)max_read=count;
 memcpy(data,cache[first],count*SECTOR);return 0;
}
static int write_media(void *context,uint64_t first,uint32_t count,const void *data)
{
 (void)context;CHECK(first<SECTORS && count<=SECTORS-first);
 if(fail_write_at && operations+1==fail_write_at){operations++;return EIO;}
 memcpy(cache[first],data,count*SECTOR);
 if(first>=FIRST && first<FIRST+COUNT && count==1 && get32(data)==COMMIT_MAGIC)
  commit_written=1;
 if(lose_commit && count==1 && get32(data)==COMMIT_MAGIC) {
  memset(cache[first],0,SECTOR);lose_commit=0;
 }
 operations++;
 if(mode==1)memcpy(stable[first],data,count*SECTOR);
 if(operations==cut) {
  /* A power cut may preserve only half of the final issued sector. */
  if(mode>=2)memcpy(stable[first],data,mode==2?8U:(mode==3?20U:SECTOR/2));
  longjmp(power_loss,1);
 }
 return 0;
}
static int flush_media(void *context)
{
 (void)context;
 memcpy(stable,cache,sizeof(stable));
 if(commit_written)commit_durable=1;
 operations++;
 if(fail_flush_at && operations==fail_flush_at)return EIO;
 if(operations==cut || (stop_commit && commit_written))longjmp(power_loss,1);
 return 0;
}
static struct ufs_journal_io io={NULL,read_media,write_media,flush_media};
static int test_init(struct ufs_journal *j,const struct ufs_journal_io *callbacks,
 uint64_t first,uint32_t count,uint64_t homes)
{
 int error=drv_ufs_journal_init(j,callbacks,first,count,homes);
 if(!error && use_image)error=drv_ufs_journal_bind_image(j,redo_image,sizeof(redo_image));
 return error;
}
static void remount(void)
{
 memcpy(cache,stable,sizeof(cache));
 CHECK(test_init(&journal,&io,FIRST,COUNT,FIRST-1)==0);
}
static void run_cut(unsigned old_count,unsigned new_count,unsigned stop,unsigned persistence)
{
 unsigned committed,written;int error;
 /* Start with an empty descriptor but retained old payload and commit record. */
 cut=0;mode=0;operations=0;commit_written=0;commit_durable=0;
 memset(stable,0,sizeof(stable));memset(cache,0,sizeof(cache));
 CHECK(test_init(&journal,&io,FIRST,COUNT,FIRST-1)==0);
 CHECK(drv_ufs_journal_commit(&journal,4,payload,old_count)==0);
 memcpy(saved,stable,sizeof(saved));
 remount();CHECK(drv_ufs_journal_replay(&journal)==0);
 cut=stop;mode=persistence;operations=0;commit_written=0;commit_durable=0;
 if(setjmp(power_loss)==0)CHECK(drv_ufs_journal_commit(&journal,12,payload,new_count)==0);
 committed=commit_durable;written=commit_written;
 /* Recovery has reliable I/O, and a second remount must be idempotent. */
 cut=0;mode=0;remount();error=drv_ufs_journal_replay(&journal);
 CHECK(error==0 || error==EIO);
 if(error==0) {
  if(!written)CHECK(memcmp(stable[12],saved[12],new_count*SECTOR)==0);
  if(committed)CHECK(memcmp(stable[12],payload,new_count*SECTOR)==0);
  memcpy(saved,stable,sizeof(saved));remount();CHECK(drv_ufs_journal_replay(&journal)==0);
  CHECK(memcmp(stable,saved,sizeof(stable))==0);
 }
 /* A committed home target from the previous mount is never rolled back. */
 CHECK(memcmp(stable[4],payload,old_count*SECTOR)==0);
}
/* Repeated power loss during home replay must converge to the committed payload. */
static void prepare_replay(void)
{
 cut=0;mode=0;operations=0;commit_written=0;commit_durable=0;stop_commit=1;
 memset(stable,0,sizeof(stable));memset(cache,0,sizeof(cache));
 CHECK(test_init(&journal,&io,FIRST,COUNT,FIRST-1)==0);
 if(setjmp(power_loss)==0) {
  (void)drv_ufs_journal_commit(&journal,12,payload,3);
  CHECK(0);
 }
 stop_commit=0;CHECK(commit_durable);memcpy(replay_base,stable,sizeof(stable));
}
static void replay_cut(unsigned stop,unsigned persistence)
{
 memcpy(stable,replay_base,sizeof(stable));remount();
 cut=stop;mode=persistence;operations=0;commit_written=0;commit_durable=0;
 if(setjmp(power_loss)==0)CHECK(drv_ufs_journal_replay(&journal)==0);
 cut=0;mode=0;remount();CHECK(drv_ufs_journal_replay(&journal)==0);
 CHECK(memcmp(stable[12],payload,3*SECTOR)==0);
 memcpy(saved,stable,sizeof(saved));remount();CHECK(drv_ufs_journal_replay(&journal)==0);
 CHECK(memcmp(stable,saved,sizeof(stable))==0);
}
static struct ufs_journal_extent group[3];
static void group_cut(unsigned stop,unsigned persistence)
{
 unsigned committed,written,index,installed;int error;
 cut=0;mode=0;operations=0;commit_written=0;commit_durable=0;stop_commit=0;
 memset(stable,0,sizeof(stable));remount();
 cut=stop;mode=persistence;
 if(setjmp(power_loss)==0)CHECK(drv_ufs_journal_commitv(&journal,group,3)==0);
 committed=commit_durable;written=commit_written;
 cut=0;mode=0;remount();error=drv_ufs_journal_replay(&journal);
 CHECK(error==0 || error==EIO);
 if(error==0) {
  installed=memcmp(stable[group[0].target],group[0].payload,SECTOR)==0;
  for(index=0;index<3;index++) {
   if(!written)CHECK(get32(stable[group[index].target])==0);
   if(committed)CHECK(memcmp(stable[group[index].target],group[index].payload,SECTOR)==0);
   CHECK(memcmp(stable[group[index].target],installed?group[index].payload:empty,SECTOR)==0);
  }
  memcpy(saved,stable,sizeof(saved));remount();CHECK(drv_ufs_journal_replay(&journal)==0);
  CHECK(memcmp(stable,saved,sizeof(stable))==0);
 }
}
static void group_validation(void)
{
 unsigned before,index;int error;
 cut=0;mode=0;operations=0;commit_written=0;commit_durable=0;
 memset(stable,0,sizeof(stable));remount();before=operations;
 CHECK(drv_ufs_journal_commitv(&journal,group,0)==EINVAL && operations==before);
 CHECK(drv_ufs_journal_commitv(&journal,group,UFS_JOURNAL_EXTENTS+1)==EINVAL && operations==before);
 journal.next_sequence=UINT64_MAX;
 CHECK(drv_ufs_journal_commitv(&journal,group,3)==EOVERFLOW && operations==before);
 journal.next_sequence=1;group[0].sectors=10;
 CHECK(drv_ufs_journal_commitv(&journal,group,3)==EINVAL && operations==before);
 group[0].sectors=1;
 group[1].target=group[0].target;
 CHECK(drv_ufs_journal_commitv(&journal,group,3)==EINVAL && operations==before);
 group[1].target=12;group[2].target=UINT64_MAX;
 CHECK(drv_ufs_journal_commitv(&journal,group,3)==EINVAL && operations==before);
 group[2].target=20;group[1].sectors=UFS_JOURNAL_GROUP_SECTORS+1;
 CHECK(drv_ufs_journal_commitv(&journal,group,3)==EINVAL && operations==before);
 group[1].sectors=1;group[1].payload=NULL;
 CHECK(drv_ufs_journal_commitv(&journal,group,3)==EINVAL && operations==before);
 group[1].payload=payload+SECTOR;
 /* Keep a valid committed group pending without installing any home sector. */
 stop_commit=1;
 if(setjmp(power_loss)==0){(void)drv_ufs_journal_commitv(&journal,group,3);CHECK(0);}
 stop_commit=0;memcpy(replay_base,stable,sizeof(stable));remount();before=operations;
 CHECK(drv_ufs_journal_commitv(&journal,group,3)==EBUSY && operations==before);
 /* Corruption in even the last payload prevents all home installation. */
 stable[FIRST+3][73]^=1;remount();error=drv_ufs_journal_replay(&journal);CHECK(error==EIO);
 for(index=0;index<3;index++)CHECK(get32(stable[group[index].target])==0);
 memcpy(stable,replay_base,sizeof(stable));
 /* A corrupt home target is covered by the descriptor checksum. */
 stable[FIRST][32]^=1;remount();CHECK(drv_ufs_journal_replay(&journal)==EIO);
 for(index=0;index<3;index++)CHECK(get32(stable[group[index].target])==0);
 memcpy(stable,replay_base,sizeof(stable));remount();CHECK(drv_ufs_journal_replay(&journal)==0);
 for(index=0;index<3;index++)CHECK(memcmp(stable[group[index].target],group[index].payload,SECTOR)==0);
}
static void group_replay_cut(unsigned stop,unsigned persistence)
{
 unsigned index;
 memcpy(stable,replay_base,sizeof(stable));remount();
 cut=stop;mode=persistence;operations=0;commit_written=0;commit_durable=0;
 if(setjmp(power_loss)==0)CHECK(drv_ufs_journal_replay(&journal)==0);
 cut=0;mode=0;remount();CHECK(drv_ufs_journal_replay(&journal)==0);
 for(index=0;index<3;index++)CHECK(memcmp(stable[group[index].target],group[index].payload,SECTOR)==0);
 memcpy(saved,stable,sizeof(saved));remount();CHECK(drv_ufs_journal_replay(&journal)==0);
 CHECK(memcmp(stable,saved,sizeof(stable))==0);
}
static void pending_reads(void)
{
 unsigned char output[24*SECTOR],original[sizeof(payload)];
 unsigned index,part,before,home_before;uint64_t witness;
 cut=0;mode=0;operations=0;commit_written=0;commit_durable=0;
 memset(stable,0,sizeof(stable));remount();
 /* The directory order need not match physical redo order. */
 group[0].target=20;group[1].target=4;group[2].target=12;
 memcpy(original,payload,sizeof(payload));
 CHECK(drv_ufs_journal_publishv(&journal,group,3)==0);
 CHECK(journal.pending_sequence!=0 && journal.pending_ready && !journal.pending_clearing);
 witness=journal.pending_sequence;before=operations;
 CHECK(drv_ufs_journal_publishv(&journal,group,3)==EBUSY && operations==before);
 CHECK(drv_ufs_journal_commitv(&journal,group,3)==EBUSY && operations==before);
 /* No caller buffer reference or home installation survives publication. */
 memset(payload,0xcd,sizeof(payload));
 for(index=0;index<3;index++)CHECK(memcmp(stable[group[index].target],empty,SECTOR)==0);
 home_before=home_reads;
 CHECK(drv_ufs_journal_read(&journal,0,24,output)==0 && operations==before);
 CHECK(home_reads-home_before==4);
 for(index=0;index<24;index++) {
  part=3;
  if(index==20)part=0;else if(index==4)part=1;else if(index==12)part=2;
  CHECK(memcmp(output+index*SECTOR,part==3?empty:original+part*SECTOR,SECTOR)==0);
 }
 /* A failed home write retains verified redo for reads and a later retry. */
 fail_write_at=operations+1;
 CHECK(drv_ufs_journal_checkpoint(&journal)==EIO);
 fail_write_at=0;CHECK(journal.pending_sequence==witness && journal.pending_ready);
 CHECK(drv_ufs_journal_read(&journal,20,1,output)==0 && memcmp(output,original,SECTOR)==0);
 /* Three home writes + home flush + descriptor clear + clear flush. */
 fail_flush_at=operations+6;
 CHECK(drv_ufs_journal_checkpoint(&journal)==EIO);
 fail_flush_at=0;CHECK(journal.pending_clearing && journal.pending_sequence==witness);
 CHECK(drv_ufs_journal_read(&journal,20,1,output)==0 && memcmp(output,original,SECTOR)==0);
 before=operations;CHECK(drv_ufs_journal_checkpoint(&journal)==0 && operations==before+2);
 CHECK(journal.pending_sequence==0 && !journal.pending_ready && !journal.pending_clearing);
 for(index=0;index<3;index++)CHECK(memcmp(stable[group[index].target],original+index*SECTOR,SECTOR)==0);
 memcpy(payload,original,sizeof(payload));
 /* An uncertain failed publication blocks visibility until explicit recovery. */
 lose_commit=1;
 CHECK(drv_ufs_journal_publishv(&journal,group,3)==EIO);
 CHECK(journal.pending_sequence!=0 && !journal.pending_ready);
 CHECK(drv_ufs_journal_read(&journal,20,1,output)==EBUSY);
 CHECK(drv_ufs_journal_checkpoint(&journal)==EBUSY);
 CHECK(drv_ufs_journal_replay(&journal)==0 && journal.pending_sequence==0);
 CHECK(drv_ufs_journal_publishv(&journal,group,3)==0);
 /* Losing proof of a known committed group cannot become successful boot replay. */
 memset(cache[FIRST+COUNT-1],0,SECTOR);
 CHECK(drv_ufs_journal_replay(&journal)==EIO && journal.pending_ready);
 memcpy(cache[FIRST+COUNT-1],stable[FIRST+COUNT-1],SECTOR);
 CHECK(drv_ufs_journal_checkpoint(&journal)==0);
 /* One contiguous redo range is returned with one multi-sector data read. */
 group[0].sectors=3;
 CHECK(drv_ufs_journal_publishv(&journal,group,1)==0);max_read=0;
 CHECK(drv_ufs_journal_read(&journal,20,3,output)==0 && max_read==3);
 CHECK(memcmp(output,payload,sizeof(payload))==0);
 CHECK(drv_ufs_journal_checkpoint(&journal)==0);group[0].sectors=1;
}
static void recovery_outcome(void)
{
 uint64_t sequence,previous;uint32_t digest,previous_digest;unsigned index;
 cut=0;mode=0;operations=0;commit_written=0;commit_durable=0;
 memset(stable,0,sizeof(stable));remount();
 CHECK(!drv_ufs_journal_committed(&journal,0,0));
 /* A commit flush can report an error after the record has reached stable media. */
 fail_flush_at=9;
 CHECK(drv_ufs_journal_publishv(&journal,group,3)==EIO);
 fail_flush_at=0;sequence=journal.pending_sequence;digest=journal.pending_digest;
 CHECK(sequence!=0 && !journal.pending_ready);
 CHECK(!drv_ufs_journal_committed(&journal,sequence,digest));
 CHECK(drv_ufs_journal_replay(&journal)==0 && journal.pending_sequence==0);
 CHECK(drv_ufs_journal_committed(&journal,sequence,digest));
 CHECK(!drv_ufs_journal_committed(&journal,sequence,digest^1U));
 for(index=0;index<3;index++)CHECK(memcmp(stable[group[index].target],group[index].payload,SECTOR)==0);
 previous=sequence;previous_digest=digest;
 /* A later missing commit cannot inherit the preceding transaction's proof. */
 lose_commit=1;
 CHECK(drv_ufs_journal_publishv(&journal,group,3)==EIO);
 sequence=journal.pending_sequence;digest=journal.pending_digest;
 CHECK(sequence!=previous && !drv_ufs_journal_committed(&journal,sequence,digest));
 CHECK(drv_ufs_journal_replay(&journal)==0 && journal.pending_sequence==0);
 CHECK(!drv_ufs_journal_committed(&journal,sequence,digest));
 CHECK(drv_ufs_journal_committed(&journal,previous,previous_digest));
 /* A newly verified group replaces the bounded proof; init ends its lifetime. */
 CHECK(drv_ufs_journal_publishv(&journal,group,3)==0);
 sequence=journal.pending_sequence;digest=journal.pending_digest;
 CHECK(drv_ufs_journal_committed(&journal,sequence,digest));
 CHECK(!drv_ufs_journal_committed(&journal,previous,previous_digest));
 CHECK(drv_ufs_journal_checkpoint(&journal)==0);
 CHECK(drv_ufs_journal_committed(&journal,sequence,digest));
 remount();CHECK(!drv_ufs_journal_committed(&journal,sequence,digest));
}
static void image_reads(void)
{
 unsigned before;
 unsigned char result[3*SECTOR],expected[3*SECTOR];
 struct ufs_journal_extent run={12,3,payload};
 cut=0;mode=0;operations=0;commit_written=0;commit_durable=0;
 memset(stable,0,sizeof(stable));remount();
 CHECK(drv_ufs_journal_bind_image(&journal,redo_image,1)==EINVAL);
 CHECK(drv_ufs_journal_publishv(&journal,group,3)==0);
 CHECK(journal.image_valid);
 CHECK(drv_ufs_journal_bind_image(&journal,NULL,0)==EBUSY);
 memcpy(expected,payload,sizeof(expected));memset(payload,0,sizeof(payload));
 before=read_calls;
 CHECK(drv_ufs_journal_read(&journal,group[0].target,1,result)==0);
 CHECK(read_calls==before && memcmp(result,expected,SECTOR)==0);
 /* Checkpoint consumes exactly three extent writes and two flush boundaries. */
 before=operations;
 CHECK(drv_ufs_journal_checkpoint(&journal)==0);
 CHECK(operations-before==6 && read_calls>0 && !journal.image_valid);
 CHECK(memcmp(stable[group[0].target],expected,SECTOR)==0);
 memcpy(payload,expected,sizeof(payload));
 CHECK(drv_ufs_journal_publishv(&journal,&run,1)==0);
 before=read_calls;
 CHECK(drv_ufs_journal_read(&journal,12,3,result)==0);
 CHECK(read_calls==before && memcmp(result,payload,sizeof(result))==0);
 before=operations;
 CHECK(drv_ufs_journal_checkpoint(&journal)==0);
 CHECK(operations-before==4); /* One three-sector home write, flush, clear, flush. */
 CHECK(memcmp(stable[12],payload,sizeof(payload))==0);
 CHECK(drv_ufs_journal_bind_image(&journal,NULL,0)==0 && journal.image==NULL);
}
int main(void)
{
 unsigned old_count,new_count,stop,persistence,index;
 for(index=0;index<sizeof(payload);index++)payload[index]=(unsigned char)(index%251U+1U);
 for(old_count=1;old_count<=3;old_count++)
  for(new_count=1;new_count<=3;new_count++)
   for(persistence=0;persistence<5;persistence++)
    for(stop=1;stop<=14;stop++)run_cut(old_count,new_count,stop,persistence);
 prepare_replay();
 for(persistence=0;persistence<5;persistence++)
  for(stop=1;stop<=8;stop++)replay_cut(stop,persistence);
 for(index=0;index<3;index++){group[index].target=4+8*index;group[index].sectors=1;group[index].payload=payload+SECTOR*index;}
 for(persistence=0;persistence<5;persistence++)
  for(stop=1;stop<=20;stop++)group_cut(stop,persistence);
 group_validation();
 for(persistence=0;persistence<5;persistence++)
  for(stop=1;stop<=8;stop++)group_replay_cut(stop,persistence);
 cut=0;mode=0;operations=0;commit_written=0;commit_durable=0;
 memset(stable,0,sizeof(stable));remount();lose_commit=1;
 CHECK(drv_ufs_journal_commitv(&journal,group,3)==EIO);
 for(index=0;index<3;index++)CHECK(memcmp(stable[group[index].target],empty,SECTOR)==0);
 pending_reads();
 recovery_outcome();
 use_image=1;
 image_reads();
 for(index=0;index<3;index++)group[index].target=4+8*index;
 group_validation();
 for(persistence=0;persistence<5;persistence++)
  for(stop=1;stop<=8;stop++)group_replay_cut(stop,persistence);
 for(persistence=0;persistence<5;persistence++)
  for(stop=1;stop<=20;stop++)group_cut(stop,persistence);
 prepare_replay();
 for(persistence=0;persistence<5;persistence++)
  for(stop=1;stop<=8;stop++)replay_cut(stop,persistence);
 recovery_outcome();
 printf("UFS journal crash/reuse: %u checks PASS\n",checks);return 0;
}
