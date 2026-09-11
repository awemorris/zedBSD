<!-- awesome-plan project=zedbsd record=ws019-p042 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws019/phase042/phase.md`

親: [ws019](https://github.com/awemorris/zedBSD/issues/20)

# ws019-p042: reject self-overlapping backing ranges

Status: completed q175
Parent: [WS019](https://github.com/awemorris/zedBSD/issues/20)
Timebox: 60 active minutes

Canonicalize a preparing file claim's extents, sort its private physical range
array in-place with O(n log n) worst-case work, and reject overlapping adjacent
ranges with EINVAL before taking the registry spinlock. Adjacent ranges are
valid. Reordered ranges affect ownership only, not the caller's logical I/O map.
On failure retain the preparing claim so retry/release remains explicit. No
new array, filesystem mutation or user ABI. Test physical aliases, duplicates,
partial/nested overlaps, touching and reversed disjoint ranges, and a generated
oracle against exhaustive pair comparisons. Preserve existing claim lifetime,
raw exclusion and formatter/swap tests. Three builds verify integration.

UFS identity, indirect-metadata alias proof and snapshot exclusion remain next;
this prerequisite does not enable UFS swap or complete native installation.
