# ws019-p028: complete tree enumeration and attribute-preserving copy

Status: completed q161; [acceptance](results.md), 2026-09-09
Parent: [WS019](../ws.md); general utility conformance also belongs to WS001.

Extend existing find and cp to support p007's immutable-tree manifest and
completed-file progress. Do not add an installer-only helper executable.

Find must report directory-read failures, provide filename-safe manifest output
(e.g. -print0/-fprint0), and fail on incomplete enumeration or failed output/
close. Avoid changing existing expression semantics. Test hidden names, spaces,
tabs/newlines, leading dashes, symlink cycles, depth limits and injected errors.

Additional current-source finding: main creates NODE_PRINT when no expression
is supplied, then wraps it with a second NODE_PRINT because has_action remains
false. Thus the default expression can enumerate each path twice. Determine
action presence from the parsed expression, not arbitrary argv text (a path or
-exec argument can equal an action token). Add implicit print exactly once when
the expression lacks an explicit action. Verify no-expression, predicates-only,
explicit print/print0, prune, boolean short-circuit and action-like operands.
The existing final ferror(stdout) check also precedes stdio's implicit exit
flush; explicitly check final flush/close before reporting a complete manifest.

Cp must support recursive attribute-preserving copying sufficient for the
actual rootfs tree: owners, permissions, timestamps, symlinks and hard-link
groups. Audit current syscalls before defining unsupported cases; refuse them
before destructive provisioning rather than silently losing metadata. Preserve
the existing exclusive/attribute-only file-creation options used by p004.

The Noct installer freezes the source manifest and displays its total before
copying, then displays successful/total files. Choose a machine-safe per-file
completion mechanism or explicit manifest batches without breaking hard links.
Directories have separate preparation/final-attribute work; failed metadata
is not a completed file. Reconcile the final destination tree and manifest,
including counts, rather than accepting progress output as proof of success.

Progress contract (user clarification, 2026-09-09): enumerate before invoking
cp, not concurrently with copying. The initial screen reports the fixed number
of non-directory paths; regular files and symbolic/hard-link names each count
once. Show directories as a separate total. An empty file still counts as one
file, and a hard link counts when its destination link and attributes have been
verified, without recopying its data. Do not follow symbolic links during census.

During copy show `Files copied: completed / total` and optionally the current
escaped relative path. Count completion only after that path's content or link,
required attributes, and close operation succeed. Directory metadata is finalized
after children, and the final sync/verification is a separate visible step;
reaching total/total alone must not announce installation success. On failure,
retain the last successful count and name the failed step. A new attempt performs
its own census and validation rather than inheriting an earlier progress count.

Acceptance additionally checks that the census is displayed before the first
destination file is copied, counts are monotonic and never exceed the fixed
total, and read/output/metadata/close failures do not advance completion. Include
zero-length files, multiple hard-link names and a failure on the last file so
neither size-zero shortcuts nor apparent 100 percent hide incomplete work.

Implement focused host fault/conformance tests, build maintained targets, and
exercise target-side copy/count/metadata in QEMU. Actual dedicated installation
and boot remain p006/p007. Enter a finite queue after the active p026/p004 run;
no renewed product approval is needed for these specified utility features.
