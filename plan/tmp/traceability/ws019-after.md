<!-- awesome-plan project=zedbsd record=ws019 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws019/ws.md`

親: [master](https://github.com/awemorris/zedBSD/issues/1)

# WS019: installation and disk administration

<!-- traceability:start -->

## Goal traceability

- Primary Milestone: **MG003 — 対象機へ導入して単独起動できる**
- Related Milestones: MG006, MG004
- Objectives: O1, O2, O4
- 貢献する成果: 共存/専用/PC98のインストールと対象単独起動を成立させる。
- 上位定義: [MasterのObjectives / Milestone Goals](https://github.com/awemorris/zedBSD/issues/1)

既存Phaseは本WSを親として上位成果に接続する。Primaryは分類と責任の所在であり、
各PhaseがRelatedすべてを満たすという意味ではない。成果・検証・限界は各Phaseの
現行記録を根拠とする。今回の対応付けは状態変更・未定義作業の追加・実行許可ではない。

<!-- traceability:end -->


Last updated: 2026-09-10
Status: completed / cleared q186

The text installer is accepted in both modes. [p049/q182](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase049-native-installer-integration/results.md)
completed the actual source/mode/disk/review/copy/boot flow, coexistence regression,
source-free native boots and three supported builds. [p007/q183](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase007-native-root-installation/results.md)
completed real UFS swap paging, 513-extent fragmentation, lifecycle and halt.
p006/p007/p029/p049 are complete. The amd64 installer is accepted; PC98 FAT-only installation and target-only login passed in p050/q186.
Earlier codec, claim, command and copy evidence is indexed in the phase registry.

User addition: finish the current text UI, then implement
[p029 BeUI frontend](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase029-graphic-installer/phase.md) with shared installer
logic and precomposed 640x480 RGB24 screen assets. Final entry points are
/sbin/zedinst and /sbin/zedinst-graphic; /bin/noct remains the interpreter.
[q184 results](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase029-graphic-installer/results.md): graphical native and FAT
coexistence installation passed. User accepts normal-path completion.
Additional PC98 graphical FAT-only acceptance: [p050](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase050-pc98-graphic-fat/phase.md).

User request 2026-09-09: capture and show the installer screen on its next QEMU
execution. Preserve actual framebuffer screenshots with the acceptance artifacts
and display them to the user; do not substitute a mockup or console transcript.

## Earlier execution history (superseded by the completed phase registry)

q156: [retained GPT/BPB and live identity](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase004-zedinst-existing-fat-overlay/q156-results.md)
pass host/native capture and revalidation. Public installer integration remains.

q154: [p021](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase021-nested-mount/results.md) completed process-path
mount/unmount. Actual source artifact capture passes QEMU, including both
read-only USB source mounts and cleanup. [q154 evidence](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase004-zedinst-existing-fat-overlay/q154-results.md).
Populated tmpfs teardown is fixed in [p022/q155](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase022-tmpfs-unmount/results.md).
[p023](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase023-noct-large-seek/results.md) completed the upstream Noct update:
host/native large files and a 5-GiB disk's backup GPT read pass. P004 no longer
needs a dd workaround for large metadata offsets; public integration remains.

q152: source-config/manifest/capacity components pass host and QEMU;
[p004 remains uncleared](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase004-zedinst-existing-fat-overlay/q152-results.md)
for real source/mount/confirmation integration and public command acceptance.

[p020](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase020-df-capacity/phase.md) completed in q151. df capacity
arithmetic/output failures are checked; host/sanitizer and three builds pass.
The full public installer remains incomplete.

q148: [p019](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase019-pristine-verification/phase.md) completed
read-only pristine verification in existing mkfs/mkswap commands, closing the
q134 canonical-image comparison prerequisite. p004 transaction and p005
installed-boot acceptance remain incomplete; no installation is claimed.

q149: Noct managed-file transaction passes 102 host/native policy scenarios and
real FAT command publication/recovery. [Result and full-admission resume point](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase004-zedinst-existing-fat-overlay/q149-results.md).
The public installer launcher remains uninstalled pending complete integration.

WSID: `ws019`

Status: active; Noct 2.0.1 is integrated and the temporary implementation-
language block is released. The user selected target-side creation through new
`mkfs` and `mkswap` commands rather than installer templates. P002 is
completed with p003 userspace inspection in finished q076. P010 reload, p011
editing and user-added p012 explicit mounts completed their final mounted/reboot
runtime gates in q077. Installer-v1 remains
separately non-table-writing.

Parent: [master plan](https://github.com/awemorris/zedBSD/issues/1)

Last verified Phase: `ws019-p020`, q151; p014–p019 prerequisites complete.

Resume point: p004's [admission design](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase004-zedinst-existing-fat-overlay/admission-design.md)
is the current implementation entry. Provenance, commands, publication,
capacity, pristine comparison and file transaction prerequisites have evidence;
public discovery/confirmation/packaging and full installation remain. P005
follows p004. Earlier q077–q079 evidence below remains historical.

Current filesystem integration: [WS024](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws024-unified-ufs/ws.md), q101,
implements the user's single 64-bit UFS decision. The installer consumes
`mkfs -t ufs FILE` for ordinary images. The explicit
`mkfs -t ufs --profile=journal-snapshot FILE` profile reserves persistence regions
inside the existing file size; it is not the installer's default. Keep the
same descriptor reservation, fixed size, identity and flush/readback boundary.
P008/q079 remain historical evidence for the former UFS1 baseline. WS024 retirement acceptance is complete; installer-specific current-format
acceptance is tracked separately in p014.

Shared tests: [WS019 test index](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/tests/README.md)

## Goals

- Install zedBSD from the ordinary USB-booted system with `/bin/zedinst`
  rather than maintaining a separate installer image.
- Make the first install path intentionally small: an existing GPT disk, its
  existing ESP, and one explicitly selected existing FAT32 payload partition.
- Preserve partition-table and existing partition filesystem formats, labels,
  and all unrelated files in installer v1. Newly created `data.img` and
  `swapfile` are formatted as files inside the selected FAT32.
- Provide a truthful read-only `/sbin/diskpart` before adding any table writer.
- Boot the resulting immutable-root/writable-overlay installation from QEMU
  NVMe and then the Latitude 5320 internal NVMe.
- Keep native-root installation, whole-disk initialization, and partition
  filesystem formatting in p006/p007, now authorized by the user. Installer v1
  does create the two file-backed runtime objects with target-side tools.

## Objective

Create the smallest safe installation path for the current product maturity.
The first installer does not pretend to be a general partition editor and does
not create a dual-boot layout. It verifies an existing GPT/ESP, lets the user
select one existing FAT32 on the same disk, and copies one bounded set of
zedBSD files. A user who does not already have such a destination should keep
using the USB system until the whole-disk/native work in p006/p007 is implemented
and accepted. Those modes were authorized and designed on 2026-09-09.

## Fixed installer-v1 layout

The two roles are distinct partitions on one existing GPT disk:

```text
ESP (existing FAT32)
  /EFI/BOOT/BOOTX64.EFI

ZEDBSD payload (user-selected existing FAT32)
  /vmunix
  /zedbsd.cfg
  /rootfs.img
  /data.img
  /swapfile
```

`ZEDBSD` is a role name in this contract, not a requirement to change the GPT
partition name or FAT volume label. Installer v1 performs no GPT write, no
partition formatting, no resize, no label change, and no native-root
installation. It does invoke target-side `mkfs` for the new UFS `data.img` and
`mkswap` for the new swap file.

## Fixed product and safety decisions

- The commands are `/bin/zedinst` and `/sbin/diskpart`.
- Installation runs from the ordinary USB image; no installer-only image or
  base system is introduced.
- The selected disk must already contain a valid GPT and exactly one usable
  GPT EFI System Partition with a writable FAT32 filesystem.
- The user explicitly selects a different, writable FAT32 partition on the
  same physical disk. Enumeration order is never consent or identity.
- The installer displays the disk identity, ESP PARTUUID, payload PARTUUID,
  capacity/free-space checks, source image identity, and exact managed paths
  before one confirmation.
- Unrelated partitions and files are never touched. A managed destination
  path that is already byte-identical is accepted; a non-identical existing
  object is refused rather than silently overwritten in v1.
- Each copied or generated managed file is staged on the destination filesystem, flushed,
  checked for size and digest, and published by same-filesystem rename. A
  failure removes only the unpublished temporary file and reports the exact
  incomplete path.
- Secure Boot remains disabled for the initial Latitude installation.
- Existing one-based partition naming remains authoritative: the first GPT
  partition is `/dev/nvme0n1p1`, never `p0`.
- Diskpart's existing-table edits belong to p011; kernel p010 only reloads.
  Any mounted partition (including read-only/unchanged/root) makes its whole
  disk EBUSY. Writes and reload results are separate; reboot is acceptable.
  No force, whole-disk initialization, formatting or partition data movement.
- Discovery publishes auxiliary partition devices but never auto-mounts their
  filesystems at `/diskN`. Only configured boot/root/overlay/swap sources and
  explicit runtime mount requests select filesystem mounts (p012).

## Boot and payload discovery contract

Installer v1 does not create, reorder, delete, or depend on zedBSD-owned UEFI
`Boot####` variables. It installs the fallback/recovery pathname
`/EFI/BOOT/BOOTX64.EFI`. QEMU and Latitude acceptance first allow firmware
automatic discovery; if that is absent, one explicit firmware menu or
boot-from-file selection is sufficient for this milestone. Portable
unattended fixed-disk boot may add a separately reviewed Boot entry later.

The loader is physically on the ESP while all remaining files are on the
payload FAT32. Therefore loader origin cannot continue to mean `boot0`.
WS013 p002/p003 completed this discovery/parsing contract in q031:

1. enumerate SimpleFS handles on the same physical GPT disk as the loaded ESP;
2. accept same-disk FAT16/FAT32, including the loaded filesystem when it owns
   the configuration;
3. use `/zedbsd.cfg` as the candidate marker;
4. fail visibly if zero candidates match, and warn then use the deterministic
   first candidate if multiple match;
5. read required `kernel=` and load that relative file from the same FAT; and
6. bind an omitted `boot0` and bare image paths to the selected config FAT.

The installer deliberately applies a stricter uniqueness rule before copying:
it refuses another same-disk `/zedbsd.cfg`, ensuring the installed payload is
the loader's first and only candidate. No
FAT label, GPT name, extra ESP locator file, or new CPAR handoff ABI is needed.

The initial generated configuration is direct and contains:

```ini
kernel=vmunix
overlay-root=rootfs.img
overlay-data=data.img
swap0=swapfile
```

WS013 p003 removes the loader-only `kernel=` directive, adds the selected FAT
identity as `boot0` when omitted, prefixes the three bare file values with
`boot0:`, and hands the existing kernel parser a space-separated record. A
future config may instead use direct `rootpart=` for native UFS; no menu or
section syntax is implemented now.

## Source artifact contract

`zedinst` must resolve its boot/config source using a separately designed
provenance interface before p004 implementation; p002's minimal disk/mount
queries do not expose that provenance. It mounts the verified source read-only
in a private temporary location and
uses its verified `BOOTX64.EFI`, amd64 kernel, and read-only `rootfs.img`.
It creates the destination `data.img` and `swapfile` at explicitly bounded
sizes and invokes the target `/sbin/mkfs` and `/sbin/mkswap`; it never copies
the running overlay upper or active swap. It generates the direct
`/zedbsd.cfg` above rather than copying mutable live configuration.

## Foundations and current gaps

- WS013 p002/p003 completed same-disk config-volume discovery and configured
  kernel loading in q031; the former own-filesystem-only `/VMUNIX.X64` path is
  historical, not an outstanding prerequisite. Q032 completed the configured
  BIOS paths. P004 still needs retained selected boot/config filesystem
  provenance without confusing it with the firmware-loaded ESP.
- The selected config FAT's synthesized `boot0` remains the filesystem
  identity contract. Current versioned boot metadata distinguishes GPT from
  legacy one-based MBR indices; p004 must not infer missing physical-loader
  provenance from an MBR index, enumeration order, or the running root.
- WS004 p024 provides kernel GPT discovery; diskpart p003/p011 parses raw
  tables in userspace. P002 exposes only basic block information and mounts.
- Current devfs uses checked 64-bit offsets and exactly-once full-sector
  writes. The old >4-GiB/duplicate-write debt note is stale; p002 extended its
  block path to 4096-byte sectors and revalidated both in q076.
- Installer source/boot provenance formerly assigned to p002 remains an
  explicit p004 pre-implementation gap; it is not part of the minimal UAPI.
- Target-side UFS1-in-file and swap-in-file initializers completed p008/p009
  in q079; their descriptor reservation and runtime acceptance are recorded. FAT32 formatting and
  block-device formatting remain outside installer v1.

## Phase registry

| Combined ID | Phase | Status | Required result |
| --- | --- | --- | --- |
| `ws019-p001` | [overlay installer-v1 contract](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase001-installer-v1-contract/phase.md) | Completed by design, 2026-08-29 | The existing-ESP/existing-FAT32, no-format, no-Boot-variable contract and Phase map are fixed |
| `ws019-p002` | [basic disk information and mounts](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase002-readonly-block-gpt-administration/phase.md) | Completed in q076 | Minimal fixed-width geometry and mount snapshot; no GPT query |
| `ws019-p003` | [userspace diskpart inspection](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase003-diskpart-readonly/phase.md) | Completed in q076 | GPT/MBR analysis using raw reads |
| `ws019-p004` | [existing-FAT overlay `/bin/zedinst`](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase004-zedinst-existing-fat-overlay/phase.md) | Completed q159 | Public cancel/install/rerun and conflict refusal/restoration passed |
| `ws019-p005` | [QEMU NVMe overlay-install acceptance](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase005-qemu-nvme-overlay-install/phase.md) | Completed q160 | Installed boot/persistence, selection and fault-model reconciliation passed; WS003 candidate recorded |
| `ws019-p006` | [Mode selection and whole-disk provisioning](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase006-whole-disk-provisioning/phase.md) | Completed q182 | Coexistence/dedicated choice, NO/YES confirmation, automatic GPT/FAT32/UFS; shell escape, no partition editor |
| `ws019-p007` | [Native-root installation](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase007-native-root-installation/phase.md) | Completed q183 | ESP loader/kernel/config, private source mount and attribute-preserving cp to UFS, UFS file swap and disk-only boot |
| `ws019-p008` | [target `/sbin/mkfs`](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase008-target-mkfs/phase.md) | Completed q079 | Create a bounded UFS1 filesystem in a newly created regular file without formatting its containing partition |
| `ws019-p009` | [target `/sbin/mkswap`](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase009-target-mkswap/phase.md) | Completed q079 | Create the existing ZEDSWAP2 format in a newly created regular file with bounded size and publication |

| `ws019-p010` | [conservative partition reload](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase010-conservative-partition-reload/phase.md) | Complete q077 | Explicit ro/rw/root EBUSY and reboot acceptance pass |
| `ws019-p011` | [userspace existing-table editing](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase011-userspace-partition-editing/phase.md) | Complete q077 | Mounted-add exit 3, no live replacement and reboot discovery pass |
| `ws019-p012` | [explicit auxiliary mounts](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase012-explicit-auxiliary-mounts/phase.md) | Complete q077 | Explicit mounts and reboot/no-auto-mount acceptance pass |
| `ws019-p013` | [prerequisite contracts](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase013-installer-prerequisite-contracts/phase.md) | Complete q129 | Separate observable commands, provenance and publication requirements |
| `ws019-p014` | [current UFS formatter](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase014-current-ufs-formatter/phase.md) | Complete q129 | Single 64-bit UFS replaces the p008 historical UFS1 producer |
| `ws019-p015` | [atomic publication](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase015-atomic-publication/phase.md) | Complete q130 | No-replace rename and directory durability |
| `ws019-p016` | [boot provenance](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase016-boot-source-provenance/phase.md) | Complete q131 | Independent firmware/configuration origin |
| `ws019-p017` | [command staging](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase017-command-staging/phase.md) | Complete q133 | Existing command operations on target FAT |
| `ws019-p018` | [FAT capacity](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase018-fat-growth-capacity/phase.md) | Complete q133 | Growth, capacity refusal and real image creation |
| `ws019-p019` | [pristine verification](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase019-pristine-verification/phase.md) | Complete q148 | Read-only exact initial-image checks |
| `ws019-p020` | [df capacity](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase020-df-capacity/phase.md) | Complete q151 | Checked capacity arithmetic and complete-output errors |
| `ws019-p021` | [process-path mount/unmount](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase021-nested-mount/phase.md) | Completed q154 | Host/races/sanitizers, three builds and native path/source acceptance |
| `ws019-p022` | [populated tmpfs teardown](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase022-tmpfs-unmount/phase.md) | Completed q155 | Distinguish namespace owners from external busy users and reclaim populated tmpfs safely |
| `ws019-p023` | [Noct large File.seek update](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase023-noct-large-seek/phase.md) | Completed q155 | Immutable upstream revision and host/native large-offset acceptance |
| `ws019-p024` | [Noct terminal input progress](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase024-noct-terminal-input/phase.md) | Completed q158 | Partial-input progress, host PTY/EOF and eight native confirmation cases accepted |
| `ws019-p025` | [Public runtime contracts](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase025-public-runtime-contracts/phase.md) | Completed q159 | /bin/noct, shell exit status, null/zero; host/native/build acceptance passed |
| `ws019-p026` | [Noct image copy and progress](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase026-noct-image-copy/phase.md) | Completed q159 | Host faults and native copying/cancel/install/rerun passed; p004 conflict tracked separately |
| `ws019-p027` | [Installation source selection](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase027-installation-source-selection/phase.md) | Completed q162 | Live private-root admission, disk-only source screen, native/no-image refusal and unchanged-target acceptance |
| `ws019-p028` | [Native tree copy tools](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase028-native-tree-copy-tools/phase.md) | Completed q161 | Census, archive cp, Noct progress, independent verification and native nanosecond persistence accepted |
| `ws019-p029` | [BeUI graphic installer](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase029-graphic-installer/phase.md) | Completed q184 | Shared backend, /sbin entry points, precomposed 640x480 RGB24 UI using user artwork |
| `ws019-p030` | [GPT initialization codec](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase030-gpt-initialization-codec/phase.md) | Completed q163 | Both GPT copies/PMBR, old-byte checks, fault/sanitizer/independent inspection and three builds pass; p006 command admission follows |
| `ws019-p031` | [Exclusive block administration](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase031-exclusive-block-administration/phase.md) | Completed q164 | FD-owned gate/claim, owner writes/reload, dup/final-close/exit, QEMU and three builds pass |
| `ws019-p032` | [Public diskpart GPT init](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase032-diskpart-gpt-init/phase.md) | Completed q165 | Host faults/sanitizers, blank/existing QEMU, empty/full GPT and live child geometry, three builds pass |
| `ws019-p033` | [Partition administration](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase033-partition-administration/phase.md) | Completed q166 | Host/sanitizers, native read-only/writable siblings, whole-disk regression and three builds pass |
| `ws019-p034` | [Portable FAT32 formatter](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase034-fat32-formatter/phase.md) | Completed q167 | Four sector sizes, faults/sanitizers, independent mtools/readback, preserved free data and three builds pass |
| `ws019-p035` | [Reserved FAT32 command](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase035-fat32-command/phase.md) | Completed q168 | Public mkfs/QEMU mount and copy, host faults, stale shell cp retired, three builds pass |
| `ws019-p036` | [Native UFS codec](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase036-native-ufs-codec/phase.md) | Completed q169 | 4-GiB image, 5-TiB header geometry, independent bitmap/root accounting, legacy regressions and three builds |
| `ws019-p037` | [Native UFS command](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase037-native-ufs-command/phase.md) | Completed q170 | Shared frontend, 4-GiB native QEMU/attribute copy, FAT regression and three builds |
| `ws019-p038` | [Root image source view](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase038-root-image-view/phase.md) | Completed q171 | Read-only source mount identity, repeated cleanup and actual UI capture pass |
| `ws019-p039` | [Owned Noct source view](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase039-owned-root-view/phase.md) | Completed q172 | Shared source view, host refusal/lifecycle and public QEMU review/cleanup pass |
| `ws019-p040` | [File extent capability](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase040-file-extent-capability/phase.md) | Completed q173 | Common extent dispatch; format/swap host tests, three builds and real FAT/loop QEMU pass |
| `ws019-p041` | [UFS file extents](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase041-ufs-file-extents/phase.md) | Completed q174 | UFS mapping iterator, 117 checks in both host modes and three builds; backing admission remains closed |
| `ws019-p042` | [Backing self-overlap](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase042-backing-self-overlap/phase.md) | Completed q175 | 512-layout oracle, sanitizer/lifecycle regressions and three builds pass |
| `ws019-p043` | [File-owned metadata](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase043-owned-file-metadata/phase.md) | Completed q176 | Owned metadata provider; 157 host checks in both modes and three builds |
| `ws019-p044` | [Complete file claims](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase044-file-claim-finalization/phase.md) | Completed q177 | Complete file claims; metadata faults, format/swap regressions, three builds and FAT/loop QEMU pass |
| `ws019-p045` | [UFS backing admission](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase045-ufs-backing-admission/phase.md) | Complete q178 | Native swap lifecycle, snapshot exclusion, owner writes and truncate refusal verified |
| `ws019-p046` | [Native layout plan](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase046-native-layout-plan/phase.md) | Complete q179 | 6 layouts / 29 refusals in Noct JIT and interpreter; packaged |
| `ws019-p047` | [Native swap startup](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase047-native-swap-startup/phase.md) | Complete q180 | Two source-free UFS boots, automatic swap, missing-entry recovery and halt |
| `ws019-p048` | [Native formatter preflight](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase048-native-format-preflight/phase.md) | Complete q181 | Actual codec capacity, no-media-I/O CLI and Noct byte/inode admission |
| `ws019-p049` | [Native installer integration](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase049-native-installer-integration/phase.md) | Completed q182 | Actual mode/disk/review flow, provision/copy/swap/config transaction and QEMU |
| `ws019-p050` | [PC98 graphical FAT installation](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase050-pc98-graphic-fat/phase.md) | Completed q186 | Two IDE disks, PC98 boot chain, FAT only; no GPT/native provisioning |

## Installer-v1 completion conditions

- p002--p005 satisfy their completion conditions without importing external
  base-system code.
- A disposable QEMU NVMe with pre-existing GPT, ESP, and payload FAT32 is
  installed from the ordinary USB system and boots through the installed
  fallback loader into the overlay root.
- Before and after partition tables, filesystem formats/labels, unmanaged
  files, and UEFI variables compare unchanged.
- Zero/multiple ESPs, zero payload markers, an already-present second config,
  non-FAT32 selection,
  insufficient space, existing conflicting files, copy/flush/verify failure,
  and source/target aliasing fail visibly without a success claim.
- The public guide recommends USB trial use and states that the initial
  installer requires suitable existing partition filesystems, does not format
  them, and creates only the contained UFS `data.img` and ZEDSWAP2 `swapfile`.

## Reconsideration boundaries

Return to planning if the Latitude cannot launch the fallback path even by an
explicit firmware selection, if current FAT rename/flush behavior cannot make
publication bounded, if the selected FAT cannot be rediscovered by PARTUUID,
or if implementation would need to format, resize, or rewrite GPT.

The ordinary USB runtime currently uses `DATA.IMG` as its writable overlay
upper and `SWAPFILE` as active swap. They are not installation inputs and must
never be copied live. The selected contract is target generation through
p008/p009; immutable installer templates are not part of this WS.

## Standards references

- UEFI 2.10 defines `Boot####`/`BootOrder` and fixed-media boot-manager
  behavior:
  <https://uefi.org/specs/UEFI/2.10/03_Boot_Manager.html>.
- UEFI 2.10 defines ESP FAT, `EFI/BOOT/BOOT{machine}.EFI`, partition
  discovery, and SimpleFS:
  <https://uefi.org/specs/UEFI/2.10/13_Protocols_Media_Access.html>.

## Q078 continuation

Q078 implements p008/p009 using a descriptor-owned FAT-file reservation and
shared production parsers. The old comprehensive storage-snapshot prerequisite
is superseded by p002's diagnostic-only result; it is not restored. Formatter
completion is established by [q079 final acceptance](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/tests/q079-results.md).
P004 source provenance and bounded publication remain independent prerequisites
for the later installer Queue; its Phase records the concrete readiness gaps.

## Priority自走 / q129

p004/p005の前提を現ソースで確認し、[p013 prerequisite contracts](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase013-installer-prerequisite-contracts/phase.md)を追加して実行中。旧UFS1表記は現行単一UFSへ読み替え、installer媒体契約の変更とはしない。

### Current UFS formatter follow-up (2026-09-09)

ユーザー指摘を受け、[p014](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase014-current-ufs-formatter/phase.md)をp004/p005の前提へ追加。mkfs本体の単一64bit UFS対応は実装済み。p008/q079の旧UFS1証跡、現行formatter、installer側fixtureを区別して再検証し、必要な改修を行う。

### Noct command boundary (2026-09-09)

p004は正常化済みNoctからコマンドを実行する。専用native helperコマンドの新設は行わない。既存コマンドへの機能追加、未実装のPOSIXまたはUNIXで一般的なコマンドのuserland/base/への追加はユーザー承認済み。それでも不足する機能は明記し、依存Phaseをunclearedとして他の作業を続行する。

### Installer prerequisite phases

- [WS019-p015](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase015-atomic-publication/phase.md): completed q130
- [WS019-p016](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase016-boot-source-provenance/phase.md): completed q131
- [WS019-p017](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase017-command-staging/phase.md): planned

p004 depends on p014–p017; p005 follows p004. Noct itself is available.

q129: p013 prerequisite decomposition and p014 current-UFS formatter acceptance completed. See p014/results.md for host, target and reboot evidence; combined IDE boot-config publication remains affected by existing BUG001.

q130 completed p015 atomic publication and directory durability; p016 boot-source provenance and p017 command staging remain prerequisites for p004.

q131 completed p016 retained boot-source provenance. p017 command staging remains before p004/p005; Noct is available and no private helper command is planned.

## q132 command staging result

[p017](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase017-command-staging/phase.md) uncleared: cp ownership tests and target
Noct command checks passed; fresh data/swap growth and formatting passed. FAT
capacity refusal returned timeout 124; bounded capacity/rollback correction and
remaining acceptance/build gates are required before p004/p005. See
[results](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase017-command-staging/results.md).

[p018](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase018-fat-growth-capacity/phase.md) / q133: FAT truncate容量判定とp017受け入れ残件を実行。

## q133 completion

[p018](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase018-fat-growth-capacity/results.md) and p017 completed. FAT容量不足を
拡張前に拒否し、fresh data/swapの作成・公開・再起動後保持と3構成ビルドを確認。
次はp004 Noctインストーラ本体の詳細化・実装、続いてp005受け入れ。

## q134 in progress

p004の既存コマンド出力・Noct識別処理を実装し、host/実QEMUの照合試験が通過。
[進捗](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase004-zedinst-existing-fat-overlay/progress.md)。インストーラ本体の
選択・確認・公開・回収とp004受け入れは未完了。

### q134 bounded result

p004 remains uncleared: command extensions and Noct selection implemented and
verified; complete transaction/installed command/acceptance remain. FAT-only
format reservation rejects the proposed tmpfs scratch route. See
[progress and resume condition](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase004-zedinst-existing-fat-overlay/progress.md).
Continue independent WS002 work before resuming this implementation.
