<!-- awesome-plan project=zedbsd record=ws019-p044 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws019/phase044/phase.md`

親: [ws019](https://github.com/awemorris/zedBSD/issues/20)

# ws019-p044: complete file claim finalization

Status: completed q177
Parent: [WS019](https://github.com/awemorris/zedBSD/issues/20)
Timebox: 90 active minutes

Add a file-aware wrapper to the shared claim finalizer. Validate that the held
file matches the preparing claim, count optional owned metadata, allocate one
canonical array for data plus metadata, fill and validate both, and use existing
self-overlap/registry checks before publication. No logical map changes. Retain
preparing protection on error, reject count overflow/count-fill mismatch and
foreign ranges. Raw finalization keeps its existing interface.

Migrate formatter, swap and loop file consumers together. Test data/metadata
and metadata/metadata collisions, normal metadata protection, provider failure,
wrong identity, count change and cleanup using the production registry. Run
formatter/swap regressions, three builds, and disposable USB/loop QEMU boot.
UFS identity and snapshot exclusion remain before enabling UFS swap.
