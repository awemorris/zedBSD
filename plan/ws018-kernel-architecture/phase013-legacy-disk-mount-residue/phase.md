# WS018 Phase 013: remove legacy /diskN mount scaffolding

Last updated: 2026-09-05

WSID: `ws018`

Phase ID: `p013`

Combined ID: `ws018-p013`

Status: completed in finished q077

Parent: [WS018](../ws.md)

Tests: [WS018 test index](../tests/README.md)

## User decision and boundary

The user identifies automatic `/diskN` mounts as obsolete PC-9800 Linux-loader
heritage and requires removal, not a compatibility switch. The current request
initially authorized audit/planning; the subsequent user request explicitly
authorizes Queue selection and execution through [q077](../../queue.md).
Q076 remains finished; this Phase does not reopen its execution budget or clear
its outstanding runtime gates. The required new finite approval was obtained as q077.

The automatic partition-mount loop and `disk_name` helper were already removed
in `d97e21c` by ws019-p012. This Phase removes the obsolete supporting graph
that survived earlier transitions. It does not delete all code named `rootfs`,
all `legacy` branches, or the current mount namespace.

## Audit evidence at d97e21c

Tracked kernel, headers, six platform manifests, bootloaders, userland, build
tools, docs and maintained plan/test files were searched. Current mount/namei/
inode/file and boot call paths were inspected as well as literal/constructed
`/diskN` names. `.internal/`, generated build artifacts, disposable `temp/`
images/logs and downloaded third-party trees were not treated as production
source or deletion targets.

Historical confirmation: `484727a^:src/kern/vfs.c` called `mount_rootfs()`,
generated `/diskN` using `disk_path()`, and mounted each discovered partition.
The `484727a` diff removed `rootfs_add_mountpoint()` and
`rootfs_remove_mountpoint()` calls and `m_mountpoint` assignment from normal
mounting, introducing the path-based mechanism. This establishes the direct
relationship of the following residue to the old layout; the PC-9800/Linux
origin is the user's historical context, not a newly inferred platform claim.

| Remaining item | Current consumers / disposition |
| --- | --- |
| Historical `src/kern/rootfs.c` and `include/kern/rootfs.h` at `d97e21c` | `rootfs_add_mountpoint`/`rootfs_remove_mountpoint` have no callers. `rootfs_type` is used only by the uncalled `mount_rootfs`; `rootfs_reset` is still called at namespace reset but only clears this unused private table. Delete implementation, header, include and reset call together. |
| [mount_rootfs / mount_root_inode](../../../src/kern/mount.c) and declarations | No current callers in the searched maintained tree. Former synthetic-root creation and bare-inode boot-root accessors; remove both. Keep `mount_root_create` and `mount_root_get_ref`. |
| [m_mountpoint](../../../include/kern/mount.h), `mount_follow`, `mount_cross_parent` | No assignment to the field remains. Both old inode-only traversal functions have no callers and only inspect this unset field. Delete field, implementations and declarations. |
| [INODE_MOUNTPOINT](../../../include/kern/inode.h) | Its only producer is uncalled `rootfs_add_mountpoint`. Remaining reads are that dead rootfs removal function and four unlink/rmdir/rename masks in [inode.c](../../../src/kern/inode.c). Remove this obsolete bit and its mask terms after the zero-producer check; preserve all other protections. Do not renumber remaining flag values. |
| All six `platform/*/vmunix.mk` manifests | amd64/arm64/sparcv9/x68k still name `src/kern/rootfs.c`; pcat/pc98 still name its object. Remove these references atomically with the source. The similarly named rootfs image/package targets are unrelated and stay. |
| Literal `/diskN` expectations | No active generating/consuming production path or shipped fstab entry found. Remaining literal mentions describe removal/history or assert absence in the ws019 QEMU fixture; retain these. |

Additional unused convenience functions are not automatically in scope. For
example `mount_for_inode` has no discovered caller, but it has no dependency
on this retired rootfs graph; this Phase does not become general dead-code
cleanup.

## Retained live mechanisms

- `mount_root_create`, private boot mounts, native root, overlay and swap,
  configured `bootN` selectors, filesystem identification and raw `/dev` nodes.
- `m_cover`, `m_children`, `m_parent`, `m_name`, `m_path`, path-based
  `mount_lookup_child` / `mount_cross_path_parent`, `mount_readdir_child`,
  bind mounts and unmount lookup. `namei.c` and `file.c` use these today.
- `INODE_ROOT`, swap/loop claims, read-only checks and mount lifetime rules.
  Removal of a dead marker is not permission to weaken mounted-directory
  unlink/rmdir/rename protection or disk-wide reload EBUSY.
- `rootfs.img`, architecture filesystem images, `rootfs` build/staging targets
  and the `/etc/zedbsd-root` marker. These are not the synthetic `rootfs_type`.
- Non-x86 NULL-source automatic-root compatibility: `VFS_LEGACY_NULL_AUTOROOT`
  is excluded on i386/amd64 (including PC-98). Its current non-x86 consumers
  choose root/overlay, never `/diskN`; removing it requires another requirement.
- Explicit `mount` / fstab configuration. Do not blacklist user-created names
  like `/disk1`, auto-delete directories from an existing writable overlay,
  or broaden the current root-level-only mount syscall in this cleanup.

## Implementation sequence after approval

1. Repeat the tracked source/caller/writer/build audit against the then-current
   revision. An unexpected live consumer is a stop/re-plan condition, not a
   reason to retain the retired interface behind an alias.
2. Delete the synthetic rootfs source/header, `mount_rootfs`,
   `mount_root_inode`, their declarations, the reset call/include, and all six
   manifest entries in one buildable change.
3. Delete `m_mountpoint`, the two inode-only traversal APIs and the obsolete
   `INODE_MOUNTPOINT` producer/reader graph. Preserve numeric values of all
   remaining flags and current path-based traversal/namespace behavior.
4. Add focused production-linked regression and a maintained source/manifest
   audit under this WS's tests. Do not create stubs which make removed APIs
   appear live. Preserve q076's failure records and absence assertions.
5. Run the gates below and synchronize P/W/M results. This Phase is not
   complete merely because source grep no longer finds `/diskN`.

## Verification and proposed timebox

Propose a two-active-hour review timebox for a future Queue, not an allocation
or execution approval in this planning turn.

- KA-T120: no retired definition/declaration/include/object/field/flag remains
  in active source/build/test dependencies. Audit all six platform manifests.
  Allow historical prose and negative regression assertions, never executable
  compatibility. Recheck tracked files so ignored build artifacts cannot hide
  or falsely reintroduce a source dependency.
- KA-T121: focused normal and ASan/UBSan tests retain root/path lookup,
  mount crossing and `..`, mount-list output, explicit ro/rw and bind mounts,
  readdir/unmount, and mounted-directory mutation protections. Verify the
  remaining ROOT/SWAPFILE/LOOPFILE masks and real path-based mount handling.
  Any pre-existing broader defect is recorded separately, not silently fixed
  or waived to finish this Phase.
- Existing WS019 storage foundations, WS016 backing claims, WS018 filesystem
  identity/native FAT and relevant WS003 boot-source regressions pass.
- Build amd64 and i386 PC/AT and PC-98 using `make -j16`, with maintained
  `plan/ws021-llvm-toolchain/tests/config-pcat.mk` / `config-pc98.mk` for the
  latter two. Record non-x86 manifest audit separately from actual build/runtime
  coverage; do not claim unexecuted non-x86 boots. Never `make check`.
- Proposed shared runtime window: at most two amd64 QEMU launches with
  120-second boot / 600-second whole-cell bounds, only disposable copies.
  Explicitly include this window and any shared ws019-p010/p011/p012 acceptance
  in the next Queue before running; it is not an extra budget hidden in p013.
  Verify ordinary configured root/overlay/swap boot, auxiliary device presence
  and absent automatic mounts, explicit `/q076` ro/rw mount/unmount, and the
  same absence after reboot. Use `qemu-system-x86_64`.
- `git diff --check`, plan-link/state consistency and a fresh symbol audit pass.

## Completion conditions / stop rule

The whole retired graph and six build dependencies are gone with no shim,
the live mechanisms above retain their behavior, and the declared tests and
builds have evidence. Q076 residuals are cleared only by their own acceptance,
even if the same guest cell supplies both results. If a new live consumer,
namespace redesign, unavailable required gate or exhausted timebox prevents
completion, record uncleared with a concrete resume condition and stop.

Historical planning result on 2026-09-05: residue found and bounded removal scope recorded.
No production code, tests, runtime state, images or Queue authorization changed.

## Execution update

Current continuation entry: [2026-09-05 resume preparation](resume.md), including
fresh focused host results, working-tree assumptions and remaining gates.

Approved q077 removed the retired graph and six manifest references. KA-T120,
ordinary traversal/flag tests, storage/claims/identity/FAT regressions and three
maintained builds passed. KA-T121's mandatory covered-directory probe fails
both before and after removal (`rmdir=0 expected=17 callbacks=1`), normal and
sanitized. This is a baseline live-namespace bug, not proof of regression-free
completion. The user explicitly authorized [p014](../phase014-mounted-namespace-protection/phase.md)
to audit and fix it; the earlier stop rule is superseded only for that scope.
No q077 QEMU launch has yet occurred at this update.

## Final functional continuation result — 2026-09-05

Completed in q077. [Findings and coverage](../tests/q077-filesystem-audit.md)
and [exact results](../tests/q077-results.md) retain the original failures and
record final-source normal/sanitizer gates, amd64/PCAT/PC98 builds and the
passing second shared QEMU cell. No acceptance criterion was waived.

The working tree includes the inherited p013/p014 edits and this continuation's
UFS1/UFS2, overlay and tmpfs corrections plus maintained regressions. All remain
uncommitted. Q077 consumed two of two permitted runtime launches. No third
launch, real-media operation or installer/formatter work belongs to this Queue.
The bounded functional audit does not assert exhaustive concurrency, physical
power-loss, non-x86 runtime or security coverage.
