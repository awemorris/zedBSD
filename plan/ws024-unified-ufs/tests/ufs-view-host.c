/* Exercises production UFS metadata ownership with a deterministic view boundary.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#define UFS_AUDIT_CUSTOM_VIEW
#define main retained_audit_main
#include "ufs-metadata-host.c"
#undef main
static uint64_t media_generation = 1;
static unsigned live_views;
static unsigned enable_views = 1;
void buf_view_release(struct buf_view *view)
{
 if (view->valid) { REQUIRE(live_views != 0); live_views--; }
 memset(view,0,sizeof(*view));
}
int disk_view_matches(struct disk *disk, const struct buf_view *view)
{ return view->valid && view->disk==disk && view->generations[0]==media_generation; }
int disk_read_view(struct disk *disk,uint64_t first,uint32_t count,void *bytes,struct buf_view *view)
{
 int error;
 buf_view_release(view);error=disk_read(disk,first,count,bytes);
 if (error || !enable_views) return error;
 view->disk=disk;view->valid=1;view->generations[0]=media_generation;live_views++;
 return 0;
}
int main(void)
{
 AUDIT_STATE fs; AUDIT_INODE node; struct mount mountp; struct disk disk;
 unsigned before;
 storage_fixture(&fs,&node,&mountp,&disk,0,1);
 /* Zero is an ordinary CG number, not an initialized-cache sentinel. */
 REQUIRE(!fs.cg_valid && fs.active_cg==0);before=storage_reads;
 REQUIRE(load_cg_locked(&mountp,0)==0);REQUIRE(storage_reads==before+1 && live_views==1);
 before=storage_reads;
 REQUIRE(load_cg_locked(&mountp,0)==0);REQUIRE(storage_reads==before);
 media_generation++;
 REQUIRE(load_cg_locked(&mountp,0)==0);REQUIRE(storage_reads==before+1 && live_views==1);
 /* A second valid group replaces the old token and working image. */
 fs.super.ncg=2;fs.super.fpg=256;
 AUDIT_PUT32(storage+32*512,AUDIT_CG_NDBLK,256,0);
 memcpy(storage+288*512,storage+32*512,4096);
 AUDIT_PUT32(storage+288*512,AUDIT_CG_CGX,1,0);
 REQUIRE(load_cg_locked(&mountp,1)==0);REQUIRE(fs.active_cg==1 && live_views==1);
 REQUIRE(load_cg_locked(&mountp,0)==0);REQUIRE(fs.active_cg==0 && live_views==1);
 /* Failure must not expose either the old or partially copied group. */
 media_generation++;failure_read=storage_reads+1;
 REQUIRE(load_cg_locked(&mountp,0)==EIO);REQUIRE(!fs.cg_valid && !live_views);
 failure_read=0;REQUIRE(load_cg_locked(&mountp,0)==0);
 failure_write=storage_writes+1;
 REQUIRE(write_cg(&mountp)==EIO);REQUIRE(!fs.cg_valid && fs.cg_dirty && !live_views);
 failure_write=0;REQUIRE(write_cg(&mountp)==0);REQUIRE(!fs.cg_dirty && !live_views);
 REQUIRE(load_cg_locked(&mountp,0)==0);buf_view_release(&fs.cg_view);free(fs.cg);
 /* Existing allocation/truncate fault oracles cover the immediate adapter. */
 enable_views=0;allocation_and_truncate();
 REQUIRE(live_views==0);
 printf("UFS%d metadata PASS: initial zero, hit, generation, CG switch, read/write failure, allocation rollback\n",UFS_AUDIT_VERSION);
 return 0;
}
