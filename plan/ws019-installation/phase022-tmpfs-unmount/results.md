# q155 populated tmpfs acceptance

Date: 2026-09-09
Status: completed

Directory-entry owners now use paired classified/total inode references.
Busy admission still refuses external owners, then sync/prepare failures can
restore the intact namespace. Final destruction pins every mount inode, retires
directory entries without recursion/allocation, releases the pins and performs
normal inode/page/xattr/quota reclamation before freeing tmpfs state. A full
pin pass prevents cache pressure from evicting directories not yet visited.

Evidence:

- Actual tmpfs/inode/mount/namei/ACL fixture: **30320 checks each** ordinary and
  ASan/UBSan, 32 populated cycles. Covers hard links, replacing rename, symlinks,
  25-level directories, data and xattrs, held inode/cwd/child mount EBUSY,
  injected sync/prepare EIO with content preserved, successful retry, inode and
  commit return, LSan, and forced cache exhaustion during namespace retirement.
  `/tmp/zedbsd-q155-tmpfs7.log`.
- Existing path fixture 175 checks each, namespace suite 1303 each plus 21
  threaded admission/rollback races, writeback lifecycle 153 each; ordinary and
  sanitizers pass. Logs `/tmp/zedbsd-q155-path-final.log`,
  `/tmp/zedbsd-q155-mount-final.log`, `/tmp/zedbsd-q155-writeback.log`.
- Explicit CI amd64/PCAT/PC98 `make -j16` and private fixture builds pass:
  `/tmp/zedbsd-q155-amd64.log`, `-pcat.log`, `-pc98.log`, `-fixture.log`.
- `../temp/q155-tmpfs-native/result.json`: PASS source inspection; guest records
  `native nested mount PASS ... populated-tmpfs cleanup`. The private native
  fixture now leaves a closed file with data and a child mountpoint directory
  in the tmpfs being unmounted. It separately verifies an unlinked-open file
  refuses teardown until close, child-mount refusal, read-only and UID checks.
  Actual USB source capture and both source unmounts also pass. Protected GPT/
  FAT boot/sentinel and original production-image hashes remain unchanged.
- Focused `git diff --check` passes. No physical-machine claim is made.

Intermediate host failures were fixture defects: its file lacked f_inode, and
its reused readdir adapter enumerated only the modeled memory filesystem.
The corrected adapter dispatches tmpfs's real readdir. Those failed runs are
retained in `/tmp/zedbsd-q155-tmpfs4.log` through `-tmpfs6.log`.
