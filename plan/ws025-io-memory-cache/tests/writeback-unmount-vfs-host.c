/* Real VFS teardown with a controlled retained-policy endpoint. */
#define disk_open retained_disk_open
#define disk_close retained_disk_close
#define main retained_mount_main
#include "../../ws018-kernel-architecture/tests/legacy-disk-mount-test.c"
#undef main
#undef disk_open
#undef disk_close
#include <kern/writeback.h>
#include <kern/readahead.h>
static struct mount *policy_mount;
static unsigned policy_aborts, policy_commits;
static unsigned disk_opens;
static unsigned read_active, read_begins, read_ends;
static int read_fail;
int readahead_boundary_begin(struct readahead_boundary *token, struct mount *mountp)
{
 CHECK(!token->active && mountp!=NULL);
 if(read_fail)return EAGAIN;
 token->active=1;token->mount=mountp;read_active++;read_begins++;return 0;
}
void readahead_boundary_end(struct readahead_boundary *token)
{
 if(!token->active)return;
 CHECK(read_active>0 && token->mount!=NULL);read_active--;read_ends++;
 memset(token,0,sizeof(*token));
}
int disk_open(struct disk *disk) { CHECK(disk!=NULL);disk_opens++;return 0; }
void disk_close(struct disk *disk) { CHECK(disk!=NULL && disk_opens);disk_opens--; }
int writeback_unmount_begin(struct mount *mountp, struct writeback_unmount *token)
{
 CHECK(read_active>0);
 CHECK(token->mount==NULL && token->worker==NULL);
 if((!mount_is_private(mountp) && strcmp(mountp->m_path,"/held")) || mountp->m_bind_source!=NULL)return 0;
 if(policy_mount==NULL) { policy_mount=mountp;mount_ref(mountp); }
 CHECK(policy_mount==mountp);
 token->mount=mountp;token->worker=mountp;
 return 0;
}
void writeback_unmount_finish(struct writeback_unmount *token, int committed)
{
 if(token->mount==NULL)return;
 CHECK(read_active>0);
 CHECK(token->mount==policy_mount);
 if(committed) {
  CHECK(policy_mount->m_state==MOUNT_STATE_DYING);
  policy_commits++;mount_release(policy_mount);policy_mount=NULL;
 }else {
  CHECK(policy_mount->m_state==MOUNT_STATE_LIVE);
  CHECK(refcount_load(&policy_mount->m_refs)>=2);
  policy_aborts++;
 }
 memset(token,0,sizeof(*token));
}
int main(void)
{
 struct mount *root,*private_mount;
 struct disk disk={0};
 struct memory_control control={0};
 struct path root_path;
 mount_reset();CHECK(filesystem_register(&memory_type)==0);
 CHECK(mount_root_create("memory",0,NULL,&root)==0);
 path_set(&root_path,root,root->m_root);
 publication_and_rollback(root,&root_path);
 CHECK(policy_aborts==2 && policy_commits==1 && policy_mount==NULL);
 /* Private teardown uses its one base reference plus the retained policy. */
 CHECK(mount_private("memory",&disk,0,&control,&private_mount)==0 && disk_opens==1);
 read_fail=1;CHECK(unmount_private(private_mount)==EAGAIN && read_active==0);
 CHECK(private_mount->m_state==MOUNT_STATE_LIVE && disk_opens==1);read_fail=0;
 control.sync_error=EIO;CHECK(unmount_private(private_mount)==EIO);
 CHECK(private_mount->m_state==MOUNT_STATE_LIVE && control.destroys==0);
 control.sync_error=0;control.prepare_error=EIO;
 CHECK(unmount_private(private_mount)==EIO && control.destroys==0);
 control.prepare_error=0;CHECK(unmount_private(private_mount)==0);
 CHECK(policy_aborts==4 && policy_commits==2 && policy_mount==NULL && disk_opens==0);
 CHECK(read_active==0 && read_begins==read_ends && read_begins>=6);
 path_release(&root_path);namecache_reset();
 printf("writeback VFS rollback PASS: sync failure, prepare-unmount failure, retry, retained policy refs (%u checks)\n",checks);
 return 0;
}
