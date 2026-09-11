<!-- awesome-plan project=zedbsd record=ws019-p045 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws019/phase045/phase.md`

親: [ws019](https://github.com/awemorris/zedBSD/issues/20)

# ws019-p045: UFS backing admission and native swap

Status: completed q178; see [results](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase045-ufs-backing-admission/results.md)
Parent: [WS019](https://github.com/awemorris/zedBSD/issues/20)
Timebox: 120 active minutes

Register canonical file identity as a filesystem capability: FAT dirent location
and UFS inode number, qualified by canonical volume in the registry. Preserve
legacy host FAT fallback only when no capability exists. UFS identity is pure;
ordinary writes with active snapshots must remain possible.

Guard snapshot CREATE with a whole-volume backing mutation from before mount
locks until publication/rollback finishes. Existing/preparing claims refuse the
guard; new claims refuse the guard; an already active snapshot is refused by
UFS data/metadata iterators. No registry spinlock across I/O. Admit UFS format
reservations and swap via the common extent/identity capabilities. Preserve UFS
loop creation by dropping the optional FAT cache-map optimization on non-FAT
backends while retaining its complete ownership claim.

Verify identity/canonical aliases, snapshot guard admission orders and cleanup,
all existing backing/format/swap regressions, UFS provider tests, three builds.
QEMU disposable native UFS: fully allocate file, mkswap, swapon, mutation refusal,
swapoff and mutation recovery; malformed/holey file refusal. Check source and
unrelated bytes. Record partial failures as uncleared instead of enabling
unverified native installation. Broader swap stress/boot activation and full
installer transaction remain p007 acceptance.
