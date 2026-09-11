# p004 implementation contract (q134)

## q149 bounded implementation

q148 completed pristine comparison in the existing commands; the historical
/run canonical-image generation proposal below is superseded. Implement the
Noct managed-file transaction first, then attach full selection/confirmation
and production packaging. A finite 120-minute cycle owns this body and its
host/native failure cases. Keep p004 uncleared if the complete launcher and
destination preflight cannot be accepted in this cycle.

The transaction takes validated source/target records from preflight. It
revalidates before staging and before each publication. All six objects must
be staged/verified before any final path is created. Existing final files are
verified in place (copy/config exact content, data/swap pristine) and never
reformatted. Newly created staging paths retain inode/device ownership records.
Prepare all staged outputs, flush and verify, then publish in fixed order with
configuration last and atomic no-replace mv. A failed or uncertain rename must
inspect the final/staging identities; never remove final names during cleanup.
Only an owned, still-unpublished stage with matching inode identity may be
removed. Unknown/replaced stages are reported and preserved. Child commands
must finish before cleanup. Revalidate final content and directory durability
before reporting success. No public launcher is installed until its complete
preflight/confirmation and actual invocation are tested.

Separate command adapters from the transaction state machine so independent
host fault cases exercise the real policy without fake filesystem success.
Native Noct acceptance must exercise the actual command adapters on disposable
FAT, including exact rerun, conflict refusal and publication/cleanup failures.

The command adapter uses complete `ls -a1` output only when stat failed, to
distinguish a missing name from an uninspectable existing object. The actual ls
source previously ignored readdir failure and delayed stdout errors. Correct
those existing-command errors and add independent failure cases before using
its listing as evidence. Do not use Noct's best-effort listDirectory for this
decision. FAT rename can change st_ino; final content and identity are re-read,
while cleanup compares only the original unpublished staging identity.

The destination FAT representation supports 0755 writable or 0555 read-only,
not arbitrary POSIX mode bits. The ordinary writable managed files use 0755,
as the existing staging contract requires; do not request unsupported 0644.
Create a tiny seed inside the private tmpfs workspace and chmod it to 0755
before attributes-only exclusive cp. Host build umask can make /bin/sh or
/sbin/mkfs 0775, so those artifacts are not a reliable mode seed. Copy payload
bytes into the already owned stage without preserving unsupported source mode,
then apply the explicit destination mode. No image-sized scratch is needed.

2026-09-09. Prerequisites p014–p018 completed. This supersedes old UFS1 and
native-helper assumptions in phase.md. Preserve its complete installer outcome.

## Interface and execution

`zedinst DISK PAYLOAD_PARTITION` on amd64, root, interactive terminal only.
No force/yes, partition format/edit, native-root or whole-disk mode. A small
installed shell launcher execs Noct plus installed source modules; all installer
logic is Noct. No new native helper or Noct ioctl binding.

Use the already accepted bounded argv runner; extend existing command output:
`diskpart --machine list/show` for actual registered devices, parent/ranges and
validated GPT metadata spans; `blkid -o export -s TAG` for selected identity;
`stat -c FORMAT` for regular/no-follow inode identity and sizes. Human defaults
remain available. Reject malformed or incomplete output and child errors.
Prefer existing commands for mounts, capacity, copy, truncate, format, sync,
comparison and no-replace publication. Source/destination paths passed as argv.

## Selection and provenance

Require retained firmware/config PARTUUIDs and exactly one boot configuration
match from sysctl. Resolve each GUID uniquely in current block inventory;
never infer origins from boot0. Reject same source/destination parent or
ambiguous cloned GUIDs. Validate live partition ranges against on-disk GPT.
Destination must be explicitly named whole GPT disk plus distinct same-disk
FAT32 payload and exactly one usable FAT32 ESP. Preflight rejects mounted/root/
swap aliases (including hidden claims via existing reload refusal), read-only
media, unsupported/degraded GPT, other same-disk boot configuration markers.

Operation is a single-administrator installation, consistent with diskpart.
Revalidate identities, GPT bytes, media signatures and files after confirmation
and before publication. Do not claim snapshots or a device-wide reservation
against arbitrary concurrent raw writes. If required identity cannot be retained
across commands, report that exact limitation rather than assume safety.

Use mount listings only to reuse/inspect paths, never as boot provenance.
Mount source read-only if not already mounted; bind/read-only views may be
used when needed. New temporary mount directories use exclusive creation.
Unmount only mounts created by this run. Keep descriptors/hash records for
GPT metadata spans; compare on-disk bytes before/after the transaction.

## Transaction

Six final files: ESP `/EFI/BOOT/BOOTX64.EFI`; payload `/vmunix`, `/rootfs.img`,
`/data.img`, `/swapfile`, `/zedbsd.cfg`. Copy only verified immutable artifacts
from proved boot sources. Fresh data=32MiB UFS, swap=64MiB ZEDSWAP2; formatters
produce deterministic initial bytes. Fixed configuration uses the selected
payload PARTUUID and explicit root/data/swap paths, not source boot0 values.

Display source/destination identities, capacities and all managed paths, then
one exact device-specific confirmation. Recheck after the interactive pause.
Prepare and verify all staging objects before publishing any final file.
Create via exclusive attributes-only cp from a known 0755 artifact, record
ownership only after success, then copy/generate/format, sync and hash/readback.
Every failed command must finish/reap before cleanup; timeout is not proof that
a kernel mutation was cancelled. Use System.pcall to retain cleanup state.

Byte-identical existing final files are accepted. Differing or wrong-type files
refuse; never overwrite. Publish with atomic mv --update=none-fail and sync
directory. Configuration publishes last. A rename followed by sync failure
means published with uncertain durability: never remove that final file.
Cleanup only known, unpublished staging objects with matching recorded inode
identity; uncertain artifacts remain named in the failure report. Recovery from
interruption accepts exact final files and refuses unknown conflicts.

## Verification and limits

Host command output/error fixtures and actual Noct policy/orchestration tests,
including negative sources/targets, cancellation, stale identity, conflicting
files, publication failure and partial prior publication. Then disposable QEMU
executes the installed production command against existing NVMe GPT ESP/FAT32,
compares protected bytes/sentinels/firmware variables, reruns idempotently and
refuses a conflicting file. p005 additionally boots installed NVMe without the
source USB and verifies data/swap/reboot. No test-only installer bypass switches.

Timebox q134: 180 active minutes. p004 may remain in-progress between turns;
mark uncleared only with concrete remaining work and resume condition.

## Canonical generated images and command capabilities

Current built Noct does not register the unorganized Binary/Hash sources.
Use explicit Long-based BPB readers in Noct and extend existing cksum with
`-a sha256` streaming output. The userland digest source is an independent
copy; no kernel source/header or Noct binding dependency is introduced.

For rerun space accounting, generate fresh canonical 32/64MiB images in the
private /run workspace with the real formatters. Compare existing final files
against these outputs, and stage on destination FAT only files that are absent.
Never reformat an existing destination. This avoids requiring a second full
installation's capacity on an already installed payload. Scratch allocation
failure aborts before confirmation; no prebuilt image/template is shipped or
used, and live data/swap are never copy sources. Verify tmpfs formatter support
and byte equality against the earlier real FAT-created canonical images before
using this route. Source/canonical records are rechecked after confirmation.

The /run proposal above failed its QEMU probe (EOPNOTSUPP from the FAT-only
format reservation). It is not an approved working implementation; see
[bounded result and resume condition](progress.md).
