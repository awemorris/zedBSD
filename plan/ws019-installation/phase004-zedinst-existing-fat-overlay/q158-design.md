# q158: public installer integration

Date: 2026-09-09
Phase: ws019-p004
Timebox: 120 active minutes
Authorization: standing autonomous Priority instruction

Prerequisites: q157 native discovery and q149 publication recovery accepted;
q155 process-path/tmpfs cleanup and large seek fixes accepted.

Integrate existing modules through an exclusive 0700 /run workspace,
read-only firmware/configuration mounts, and destination ESP/payload mounts.
Reload the unmounted destination before taking the final device inventory;
never reload after acquiring our mounts. Refuse unrelated existing workspace
state. Track successful mounts and directory identities, unwind in reverse
order, and preserve uncertain or replaced objects. Mount-list observations
are not an atomic reservation or a unique mount generation identifier.

Implementation refinement: use existing `mkdir -m 700` without `-p` for atomic
exclusive directory creation with restricted permissions. Noct's current
makeDirectoryExclusive API passes 0777 and cannot specify the initial mode.
Track pending operations before command invocation, preserve uncertain state,
and stop reverse cleanup before any unresolved ancestor. Directory dev/inode
checks detect replacement but do not provide atomic namespace reservation.

Before publication, inspect source artifacts and all candidate configuration
locations, retain provenance and raw metadata, check capacity and existing
files, and present the six managed paths and selected disk identities. Require
root and actual stdin/stdout terminals. Read an exact bounded confirmation
phrase naming both destination operands; cancel on escape/control input,
mismatch or excessive input. Always restore terminal mode after errors.
Do not use a child process's PTY as evidence of caller interactivity.

Only after confirmation create managed destination directories and stage files.
Connect existing transaction and revalidation operations, config last,
with owned cleanup on every exit. Package the command only after integration
is complete. No partition formatting, NVRAM writes or new private helper CLI.

Validate confirmation with actual PTYs and both Noct execution modes. Cover
mount failure/replacement/cleanup and native cancellation before attempting
publication. Run the real command on disposable QEMU disks for successful
installation, exact rerun, conflict refusal and recovery, retaining protected
hashes. Full p004 remains uncleared until its complete acceptance passes.

## Public orchestration boundary

Require actual caller terminals and effective UID 0 before device operations.
Resolve and reload the explicit destination before acquiring owned mounts, then
retain fresh source/target discovery. Acquire source and destination preparation
under one protected cleanup boundary; failure always attempts owned cleanup.
Display source/destination names and GUIDs, free/required capacity and all six
final filesystem paths. Bound confirmation input/idle waits and cancel without
creating destination directories.

After approval, compare provenance, retained source/target metadata, mount
identities, source contents and competing configuration candidates again.
Recompute existing-file/capacity admission before directory creation. Create
only planned missing directories, recording successful identities; stage paths
remain exclusive. Create the tiny seed with existing cp attributes-only and
no-replace options under the owned 0700 workspace. No image-sized scratch file.

Connect instManagedTransaction with a context revalidation callback; do not
silently weaken its pre-stage, pre-publication or per-file rechecks. Preserve
published final files on failure. Remove only owned empty stage directories and
the owned seed; retain newly created permanent EFI directories if they contain
published files. Unmount independently of reported stage leftovers, and report
leftovers with stable device-relative paths, not only vanished /run paths.

The public launcher forwards exactly DISK PAYLOAD to Noct; no auto-confirm or
test-only switch. Private QEMU invokes the same orchestration with real terminal
input for cancellation, successful installation, idempotent rerun and conflicts.
