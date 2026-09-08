/* Legacy nonjournal fixtures have no immutable redo owner; real-core tests omit these. */
int drv_ufs_journal_drain(struct ufs_journal *journal)
{ if(journal->pending_sequence || journal->poisoned)abort();return 0; }
int drv_ufs_journal_publishv(struct ufs_journal *journal,const struct ufs_journal_extent *extents,unsigned count)
{ (void)journal;(void)extents;(void)count;abort(); }
#ifndef UFS_AUDIT_WRITEBACK_STUB_DEFINED
int writeback_mount_active(struct mount *mountp)
{ (void)mountp;return 0; }
#endif
int drv_ufs_journal_views_busy(const struct ufs_journal *journal)
{ (void)journal;return 0; }
int drv_ufs_journal_view_acquire(struct ufs_journal *journal,struct ufs_journal_view *view)
{ (void)journal;(void)view;return ENOENT; }
int drv_ufs_journal_view_copy(const struct ufs_journal_view *view,uint64_t first,uint32_t count,void *buffer)
{ (void)view;(void)first;(void)count;(void)buffer;abort(); }
void drv_ufs_journal_view_release(struct ufs_journal_view *view)
{ (void)view;abort(); }
