<!-- awesome-plan project=zedbsd record=ws019-p003 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws019/phase003/phase.md`

親: [ws019](https://github.com/awemorris/zedBSD/issues/20)

# WS019 Phase 003: userspace partition inspection

Last updated: 2026-09-05

Phase ID: `ws019-p003`

Status: completed in q076, 2026-09-05

Parent: [WS019](https://github.com/awemorris/zedBSD/issues/20)

Tests: [WS019 test index](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/tests/README.md)

## Contract

Install `/sbin/diskpart` through the ordinary base build. No argument or
`list` enumerates whole devices; `show DISK` reads the explicitly named whole
device. `help` distinguishes on-disk tables from current kernel devices.

All GPT/MBR interpretation is userspace, via read/pread and p002 geometry:
checked offsets, MBR signature/primary entries, GPT protective MBR, both
headers and full entry arrays, CRCs, bounds, overlaps/duplicate GUIDs, canonical
GUIDs and bounded UTF-16 names. Preserve sparse one-based slots. Corrupt GPT
never silently falls back to MBR. Unsupported structures fail explicitly;
extended-MBR editing is excluded. Host image adapters specify sector size.

## Verification / boundary

Production-linked parser/CLI tests cover 512/4096 sectors, sparse GPT, primary
MBR, backup beyond 4 GiB, corrupt/truncated metadata, CRC/copy mismatch,
overflow, overlap, unsupported structures and zero writes. Ordinary/sanitizer,
amd64/i386 and shared q076 QEMU gates are required. Review at two active hours.
P011 owns mutations. Installer filesystem/source/use decisions are not inferred
from table bytes or the absence of a visible mount.

## Result

Production userspace parser/writer fixture: 1,722 checks in ordinary and
ASan/UBSan modes; independent CLI fixture: 182 checks in each mode. Corrupt
CRC-valid structures/copy disagreement, sparse slots, 512/4096 and >4-GiB
offsets are covered. amd64/i386 builds and real guest list/show passed.
See [q076 evidence](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/tests/q076-results.md). P011 mutation acceptance remains
separate; no installer provenance or permission-to-write inference was added.
