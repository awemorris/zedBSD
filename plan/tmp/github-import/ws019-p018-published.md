<!-- awesome-plan project=zedbsd record=ws019-p018 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws019/phase018/phase.md`

親: [ws019](https://github.com/awemorris/zedBSD/issues/20)

# WS019-p018: FAT growth capacity admission

Date: 2026-09-09
Status: completed (q133)
Parent: [WS019](https://github.com/awemorris/zedBSD/issues/20)
Timebox: 120 active minutes

## Evidence and scope

[p017 results](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase017-command-staging/results.md) show fresh 32/64MiB
creation and formatting work, but an impossible 256MiB truncate spends time
allocating/zeroing and returns timeout 124 rather than a prompt ENOSPC.
Implement capacity admission before growing FAT files via truncate. Retain
existing write-failure rollback; no partial-write semantics changes to write.

## Design

Reuse bounded full-chain validation to return the allocated cluster count.
Under the existing FAT mount mutation lock, count additional clusters required
by the new size, including clusters already owned past EOF. Refuse inconsistent
old size/chain as EIO. Scan allocation entries only until enough free clusters
are found or the bounded table ends. ENOSPC/read failure precedes requested
growth allocation, zeroing or directory/size publication. Prior pending closes
and dirty cache flush retain their existing ordering. No unlocked df estimate,
trust in FSInfo hints, new persistent cache or timeout increase.

## Acceptance

Actual consolidated FAT host tests: FAT12/16/32, empty/existing/preallocated
chains, partial cluster, exact fit, insufficient space, full volume, corrupt
chain, table-read error, injected write/barrier errors and rollback. Verify
unchanged original bytes/size/chain on refusal; ordinary and ASan/UBSan.
Re-run p017 fresh QEMU staging including status 1 capacity refusal, swap
activation and generated-data reboot/readback. Complete three architecture
builds with explicit configs, serially, make -j16. Correct time's negative
nanosecond formatting exposed by this measurement without inventing timings.

If essential conditions remain unproved, leave p018/p017 uncleared with concrete
resume conditions; do not call installer acceptance complete.

## Result

All selected gates passed. See [evidence](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase018-fat-growth-capacity/results.md).
