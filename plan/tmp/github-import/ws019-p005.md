<!-- awesome-plan project=zedbsd record=ws019-p005 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws019/phase005/phase.md`

親: [ws019](https://github.com/awemorris/zedBSD/issues/20)

# WS019 Phase 005: QEMU NVMe overlay-install acceptance

Last updated: 2026-09-09

Phase ID: `ws019-p005`

Status: completed; [q160 results](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase005-qemu-nvme-overlay-install/q160-results.md), public install and NVMe-only boot accepted

Parent: [WS019](https://github.com/awemorris/zedBSD/issues/20)

Tests: [WS019 test index](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/tests/README.md)

## Objective

Prove the complete partition-preserving installer-v1 transaction, target-side
data/swap file creation, and installed boot on a disposable QEMU NVMe before
any physical Latitude write.

## Fixture

- ordinary amd64 USB image as installer source;
- one NVMe image prepared in advance with valid GPT, exactly one FAT32 ESP,
  one separate FAT32 payload, and unmanaged sentinel content on both;
- fresh OVMF variable store captured before installation;
- no zedBSD managed file present before the transaction.

## Required cells

1. install succeeds and preserves GPT, labels, unmanaged files, and NVRAM;
2. installed fallback loader discovers the same-disk `/zedbsd.cfg`, consumes
   `kernel=vmunix`, resolves the generated explicit `boot0=PARTUUID=...` to
   the selected payload and uses its `boot0:` image paths,
   mounts
   `rootfs.img` plus `data.img`, activates `swapfile`, and reaches login;
3. an absent config fails visibly; duplicate configs warn and choose the first
   deterministic same-disk candidate, while the installer preflight refuses to
   create such an ambiguous installed layout;
4. conflicting file, insufficient space, wrong filesystem, source/target
   alias, and injected copy/generation/format/flush/verify failures never
   report success;
5. an auxiliary FAT disk with matching-looking files cannot steal selection
   from the ESP's physical GPT disk;
6. the generated 32-MiB `data.img` is recognized and mounted as the current
   single 64-bit UFS, the
   generated 64-MiB `swapfile` is recognized as ZEDSWAP2, and neither source
   live object is opened as an installation input.

QEMU may supply a firmware-created boot choice for the disk; the installer
itself must not change the variable store. A successful manual/fallback file
selection is sufficient and must be distinguished from a zedBSD-created
`Boot####` entry.

## Completion conditions

- One reusable WS019 test runner records every cell and immutable artifact
  digest.
- The production `make -j16` build and the full installed overlay boot pass.
- The exact candidate image for WS003 p018 is recorded.

## Reconsideration boundary

Return to design if OVMF cannot exercise the no-installer-Boot-variable path,
if payload selection is nondeterministic, or if any unmanaged byte changes.

## Unified formatter dependency (q101)

Use `mkfs -t ufs FILE` and the ordinary profile under the existing reservation
transaction. WS024 is implementing the driver/producer migration and retains
old-media rejection and fresh-file initialization boundaries. Do not infer
in-place conversion from the new public name. This update does not start the
installer phase; its existing prerequisites still apply.

## Current acceptance preparation (q158)

The q101 paragraph is historical: WS024 and WS019-p014 are complete, and UFS1
is no longer an accepted generated format. Current source configuration may
omit boot0, but the installer generates an explicit selected-payload PARTUUID;
do not weaken selection to test an obsolete generated profile.

After p004 acceptance, detach the source USB completely and boot a disposable
copy of the accepted destination with the normal UEFI fallback path. Record
the loader/kernel, root mount, UFS upper and active swap evidence. Create a
small file in the installed writable overlay, shut down normally, boot the
same installed copy again and verify persistence. Retain the original accepted
installer output separately from these boot-time modifications.

The p004 cancel/install/rerun fixture runs unchanged orchestration in a private
test rootfs. That alone does not satisfy the production package or installed
boot gates. Record the final packaged production source and destination hashes
before naming a WS003 candidate. No physical-disk write is implied by this plan.
