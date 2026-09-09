# q155 implementation progress

Date: 2026-09-09
Status: in-progress; no build/native completion claimed

Implemented paired inode namespace ownership in create/link/unlink/replacing
rename, generic busy allowance and infallible tmpfs directory-entry retirement
after all unmount refusal points. The retirement walker pins all mount members
before callbacks, preventing cache-pressure eviction of directories not yet
visited, then drops all pins before normal root/inode purge.

Actual tmpfs/inode/mount/namei/ACL host fixture passes 1903 checks each in ordinary
and ASan/UBSan modes: 32 repeated populated mounts with hard links/directories,
held-inode EBUSY, content/link retention, successful close-and-retry, inode count
return and leak checking. `/tmp/zedbsd-q155-tmpfs3.log`. Earlier fixture links
omitted the required real ACL object; they were compile/link failures, not
runtime evidence. Existing path suite passed before the all-member pin refinement
(`/tmp/zedbsd-q155-mount.log`); rerun after final edits.

Remaining: deepen tests for data/xattrs/symlinks/deep directories, unlinked-open
and cwd/child owners, replacing rename, failed sync/prepare and cache pressure
during retirement; review invariants, run maintained builds and native populated
tmpfs acceptance. Do not close p022 from the initial host subset.

User requested new upstream Noct File.seek integration during this work. It is
queued independently as p023 after p022, with upstream bb23981 inspected; package
pin/builds have not yet changed. No running fixture remains after this checkpoint.
