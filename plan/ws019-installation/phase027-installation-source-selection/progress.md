# q162 progress

Status: completed q162; [final acceptance](results.md). Below is historical progress.

The new read-only kern.boot.root_image record reports the current root's
overlay lower mount, attached loop's referenced backing file, flags and numeric
device/inode/size identity. Referenced mount/path/file owners protect inspection;
native root returns version 1 with zero flags. Noct resolves rootfs.img on the
retained physical source, compares its inode/device/size to that record, and
rechecks during preparation and transaction revalidation. Boot text and a
lookalike filename alone cannot satisfy admission.

The first source screen offers disk only; HTTP is visible but unavailable.
Escape/EOF cancels before discovery/mounts. Enter proceeds to the live query
and existing coexistence preparation. Future mode/disk and BeUI flows remain
p006/p007/p029. Fixed console CSI consumption of unsupported private markers
and parameter separators, which formerly leaked 4;2m into the display.

installer-rootimage.noct: 30 cases PASS in JIT and interpreter (malformed,
native/readonly/loop flags, differing source identity, changed observations,
source keys). console-csi-test.py: 77 production-parser split-write cases PASS.
amd64 source2 build and pcat/pc98 source builds passed. Initial compile errors
were actual path API field/function names; corrected before runtime.

temp/q162-source1 proved the live record (1:15:8:4:22982:33554432) and denied
sysctl mutation, then failed in the harness's unsupported '%' key. Added that
key mapping and launched fresh source2 as session 66269. Its source record
matches /bin/stat on the read-only FAT source's rootfs.img; first-screen cancel
works. The actual source screen is captured as installer-source.png, inspected
without the old escape artifact. The same run is processing the continue-to-
review case; poll that handle, do not start another VM until terminal.

Native-root/no-mounted-image refusal, final target hashes, screenshots and
complete q162 acceptance remain. No production edits/builds while QEMU is live.

source2 terminated PASS with unchanged target/production and captured source/
review screens. native1 supplied a whole disk to the partition-only rootpart
selector, explicitly failed VFS init and was stopped/reaped as a failed fixture.
native2 uses a UFS partition and terminated PASS: zero live-image flags, source
screen cancellation and no-image refusal despite rootfs.img existing on USB;
selected target unchanged. All handles terminal; p027/q162 completed.
