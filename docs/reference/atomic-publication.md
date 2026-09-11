# Atomic file publication

A staging file can be published without replacing an existing pathname:

```sh
sync -- /destination/stage
mv -T --update=none-fail -- /destination/stage /destination/final
sync -- /destination
```

The commands must succeed in order. `mv -T` treats the destination as an exact
pathname, including when a directory already occupies it. `--update=none-fail`
refuses an existing object and returns a nonzero status. `mv -n` also preserves
existing objects, but treats skipped files as success; callers that need to
know whether publication occurred should use `--update=none-fail`.

The no-replace operation runs inside the filesystem namespace transaction.
It does not implement an existence check followed by an ordinary rename.
FAT lookup also rejects an existing case alias. Different mounts return EXDEV;
this mv implementation does not copy and delete across filesystems.

A successful rename changes the visible namespace. If the subsequent directory
sync fails, the file is already published and its durability is unconfirmed.
Do not delete it as though it were an unpublished staging file. These commands
also do not freeze the source contents across separate process invocations;
a caller must arrange ownership of its staging directory and files.

## Interfaces

`renameat2(olddirfd, oldpath, newdirfd, newpath, flags)` is syscall 163. Its libc
declaration and `RENAME_NOREPLACE` flag are available through `<stdio.h>`;
`AT_FDCWD` comes from `<fcntl.h>`. A zero flag preserves renameat semantics.
RENAME_NOREPLACE is 1; unsupported bits fail with EINVAL. Existing destinations,
including the same inode under another name, fail with EEXIST. This is a UNIX
extension, not a POSIX rename requirement. Its interface follows the
[Linux man-pages renameat2 description](https://man7.org/linux/man-pages/man2/rename.2.html).

`sync [--] [file ...]` is installed in /bin. With operands it opens each file or
directory and calls fsync, retaining errors from open, fsync and close. FAT
directory fsync drains the filesystem's pending metadata and disk barrier.
Without operands it uses zedBSD's synchronous mount flush; this libc reports
its errors through errno even though sync has a void return type. Other sync
options are currently unsupported.

Acceptance evidence belongs to [WS019-p015](../../plan/ws019/phase015/phase.md).
