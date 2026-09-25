<!-- awesome-plan project=zedbsd record=ws061p004 -->

# ws061-p004: root の overlay（USB の boot disk 上の loop の UFS）への書き込みの同期を減らす

Phase ID: `ws061-p004`
Parent: [WS061](../ws.md)
Status: planned
Queue: none

## 目的

expat の configure は tmpfs で 35〜38 秒、root の overlay で 59〜64 秒（ws061-p002）。差の 22〜26 秒は、overlay の upper（`boot0:data.img` を loop で UFS）への書き込みが同期になっていること。標本（p002）: UFS の metadata の書き（`write_cg`・`free_block`・`ufs_truncate`）が `write_sectors_context` → `loop_submit` → `fat_pwrite_context` → USB の転送で待ち、loop が request ごとに `file_fsync_backend` → `fat_fsync` → `bio_flush`（USB の SYNCHRONIZE CACHE）を出す。

調べること: loop が flush を出す条件（request の FUA/flush か、write-through の設定か）、UFS の metadata の書きが同期である理由（soft updates・journal の有無、BUG-040 との関係）、FAT の file の書きが buffer cache を経るか。Linux の ext4（journal、非同期の metadata）と同じ水準の「fsync の要求があるときだけ待つ」を目標にする。耐障害性の意味（電源断で何を失うか）が変わるなら、変える前にユーザーの判断を求める。

## 受け入れ

root の overlay の上の configure が tmpfs の上の +20% 以内。`fsync`/`sync` を要求した file は従来どおり永続化される（試験で確かめる）。回帰: boot、sh・make の差分試験、UFS の試験、`SMP-STRESS.ELF`。規約。
