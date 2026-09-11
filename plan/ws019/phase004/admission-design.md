# Installer admission and integration follow-up

q149 source inventory, 2026-09-09. Transaction component acceptance does not
replace the following complete public-command boundary.

q152 adds `admission.noct`: fixed source-config validation, six-file manifest,
explicit payload PARTUUID configuration and bounded df capacity arithmetic.
`files.noct` consumes real df output. These do not yet replace the discovery,
owned mounts, artifact-format checks or interactive public entry below.

The current loader configuration grammar has no spaces, tabs, comments,
quoting or escapes. Admission must agree with that grammar rather than treating
a friendly-looking but unbootable file as a supported source profile.

## Existing interfaces

- `Term.isTTY()` checks the Noct process's actual stdin and stdout. Do not run
  tty inside Process.spawn: that API gives the child a PTY and would falsely
  accept a noninteractive caller. `id -u` can check inherited effective UID.
- The existing Term API supports bounded interactive confirmation without a
  Noct ioctl extension. Term.open uses raw input and an alternate screen;
  display the complete selection after opening, and always Term.close through
  a protected-call cleanup. Require one exact `INSTALL DISK PARTITION` line,
  handle backspace/cancel and a bounded wait, and restore the console on error.
- diskpart machine output plus blkid and explicit BPB reads already implement
  GUID/live-map/FAT32 selection. A successful `diskpart reload DISK` before
  destination mounts proves no current mount/hidden claim blocked admission;
  refresh live device records afterward because child registration can change.
- Mount source partitions read-only by their independently resolved provenance
  identity. Do not use inferred boot0 as origin. Record only successful mounts
  as owned. A failed command with an uncertain side effect leaves its path
  reported; cleanup cannot assume a mount was never created.
- `FileUtil.makeDirectoryExclusive` plus chmod 0700 on tmpfs can own one small
  `/run/zedinst` workspace. An existing workspace is a conflict, not permission
  to delete it. It holds only metadata/seed/config bookkeeping, never a second
  full data/swap image. Use actual command errors, not best-effort Noct directory
  enumeration, for filesystem decisions.

## Before confirmation

Resolve unique firmware/config PARTUUIDs and require config_matches=1; reject
source/destination parent aliases. Require healthy supported GPT, distinct FAT32
payload and one usable FAT32 ESP. Retain bounded GPT metadata bytes and each BPB,
not only parsed fields. Reconcile all live children and revalidate after the
interactive pause. Inspect every relevant same-disk boot-config candidate;
refuse an uninspectable layout rather than assume no competing marker exists.

Noct's amd64 File.seek limit was lifted and verified in
[p023/q155](../phase023/results.md). Use the production bounded
reader directly for raw GPT/BPB ranges, including backup GPT above 4 GiB;
retain exact bytes for later comparison. Keep reads at most 65536 bytes and
check offsets/counts against signed-64 limits before multiplication/addition.
The planned dd staging workaround is superseded. Native acceptance read the
real backup GPT of a 5-GiB QEMU disk; full discovery/revalidation integration
must still establish disk identity and compare the retained metadata.

Read the source configuration and accept only the supported fixed artifact
paths, proving that vmunix/rootfs.img are its immutable inputs. Capture source
regular-file identities, sizes and SHA-256. Do not read live upper/swap objects.
Validate loader/kernel/image formats and target architecture before staging.

Preflight existing final contents using the q148 pristine checks and exact
source/config comparisons. Compute required space only for missing files,
rounded to actual FAT clusters, plus bounded directory/metadata allowance.
Require complete successful df output. Source and destination paths in command
records are controlled absolute paths; no shell string interpolation is needed.
Creating target `/EFI/BOOT` and private stage directories waits until explicit
confirmation; preserve existing directories and record ownership of new ones.

## Publication and completion

Build exactly the six entries consumed by transaction.noct, with a fresh 0755
seed and FAT-representable modes. Configuration names the selected payload
PARTUUID explicitly and publishes last. Revalidation checks current inventory,
retained raw GPT/BPB bytes, source identities/content and configuration
uniqueness without attempting reload while the run's own mounts are active.
Close retained readers and unmount only owned mounts. Cleanup never recursively
deletes an unknown workspace, final file or replacement stage.

Install the shell-to-Noct launcher and package modules only when this boundary
is implemented and the actual public command passes disposable USB/NVMe
acceptance: cancellation, missing/conflicting targets, capacity, partial
publication recovery, exact rerun, unchanged GPT/labels/sentinels/NVRAM. p005
then tests installed NVMe-only boot with no source USB and data/swap persistence.
