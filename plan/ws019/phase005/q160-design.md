# Proposed q160: installed NVMe boot acceptance

Executing q160 after p004's q159 public6/conflict2 acceptance.
Timebox: 120 active minutes. Standing autonomous Priority authorization applies.

## Entry contract

Require q159-public6's terminal successful cancel/install/rerun cases plus a
terminal PASS from its corrected conflict-only continuation, including refusal
and restoration. Preserve the public6 aggregate fixture failure without relabeling
it. Record the immutable
source and installed-destination SHA-256, GPT tables, FAT BPBs/labels/sentinels,
and generated configuration. Keep the accepted original target read-only.

UEFI-variable comparison spans the installer operation after guest login;
record the separate preboot digest for visibility, but do not attribute OVMF's
first-boot variable initialization to zedinst. q159-conflict1 passed the guest
case but exposed this measurement-boundary error in the continuation fixture;
its corrected successor captures both moments and must pass before entry.

## Procedure

1. Create disposable reflink/sparse copies under WS019 temp. Supply only the
   installed NVMe and copied OVMF variables; omit source USB, IDE source/root,
   and test-injected rootfs. Capture the exact QEMU argument vector.
2. Boot through the ordinary EFI/BOOT/BOOTX64.EFI fallback. Require kernel
   initialization, login, mounted overlay and active ZEDSWAP2. Inspect the
   generated explicit payload PARTUUID and actual mounted UFS upper. Use the
   public shell's separate-line status checks accepted in p025.
3. Write a unique ordinary file in the writable overlay, sync, halt normally,
   restart the same disposable target, and compare the contents. Require
   ordinary shutdown success instead of treating monitor quit as halt.
4. Run independent disposable copies with no payload configuration, duplicate
   same-disk configuration, and an auxiliary lookalike FAT disk. Record visible
   failure or deterministic same-disk selection as the phase specifies; never
   manufacture success by manually naming a different root or kernel.
5. Reconcile p004's preflight and injected-failure evidence with required cell 4
   explicitly, citing the actual fixtures. Do not equate the host-only matrix
   with the installed-boot checks or infer missing cells from a successful boot.

If installed boot exposes a kernel/loader defect, preserve the last visible
stage and disposable artifacts, define a bounded correction in the same owner
phase or a prerequisite, and rerun only affected acceptance after a real fix.
Do not restart a live QEMU because an observation call timed out.

## Exit contract

Publish a per-cell results table, reusable runner, exact source/target hashes,
and a WS003 candidate only when all required cells pass. Otherwise retain p005
as uncleared with evidence and a concrete resume condition. WS009-p008 follows
the accepted public installation/boot behavior; its guide must not describe an
untested or obsolete UFS1 flow.
