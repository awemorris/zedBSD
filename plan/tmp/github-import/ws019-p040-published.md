<!-- awesome-plan project=zedbsd record=ws019-p040 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws019/phase040/phase.md`

親: [ws019](https://github.com/awemorris/zedBSD/issues/20)

# ws019-p040: filesystem file-extent capability

Status: completed q173
Timebox: 90 active minutes
Parent: [WS019](https://github.com/awemorris/zedBSD/issues/20)

Replace direct FAT extent calls in file format reservations, swap and loop with
one filesystem capability dispatch. Filesystem type advertises an ordered
512-byte-sector extent callback; FAT initially supplies its existing validated
provider. A common checked wrapper refuses missing file/inode/callback and
unsupported providers without inventing mappings. The caller retains its
prepared backing claim across both count/fill traversals; provider callbacks
must not mutate layout or retain callback state. No user ABI changes.

Retain existing FAT-only admission and identity behavior until the UFS provider
and canonical identity are implemented together. This is a prerequisite, not
UFS swap completion. The FAT loop-map optimization is unchanged.

Verify production formatter reservation and swap-source focused fixtures with
new capability registrations, independent dispatch refusal tests, and all three
architecture builds. Existing callback fault and coverage tests remain active.
Follow with UFS extent/identity provider, claim mutation coverage, actual mkswap /
swapon / swapoff and native I/O acceptance in a subsequent finite queue.
