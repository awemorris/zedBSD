<!-- awesome-plan project=zedbsd record=ws054 -->

# WS054: UFS の directory を複数の block に育てる

<!-- awesome-plan-current:start -->
Status: completed（2026-09-25、q416）
Primary Milestone: MG004
Related Milestones: MG002
Objectives: O1, O2
Parent: [Master](../master.md)
Queue: なし
Resume point: なし（完了）。実機の確認はユーザー
<!-- awesome-plan-current:end -->

## 目標

UFS（`src/drivers/fs/ufs.c`）の directory が 1 block（8 KiB）を超えて育ち、多数の entry を作成・探索・readdir・削除・rename でき、journal の group と復旧、再 mount の後も正しいこと。

きっかけ: [BUG-038](../bugs/BUG-038.md)（ws046-p008 で coreutils の `m4/`（492 file）が 370 番目で ENOSPC）。2026-09-24 ユーザー「すべて承認します。」で実装を承認。

## 結果

**directory は直接の 12 block（bsize 8192 で 96 KiB）まで育つ。** 短い名前で約 4000、40 byte の名前で約 1900 の entry。越えると ENOSPC。journal の有無によらない。

- 読む側（`next_dirent()`）は元から複数 block を読めた。書く側を変えた: `dir_find_record()`・`dir_add()`・`dir_remove()`・`dir_replace()` が全ての block を扱い、
  追加は空き → 最後の block の chunk → 新しい block の確保（`dir_add_place()`・`dir_add_finish()`、`struct dir_addition` で巻き戻し）。
- journal: 最初の block を作っていた group を「次の block を足す」group に一般化（`directory_next_block()`）。名前を入れる group（作成・link・rename）は
  `directory_insert_block()` で名前の block を選び、要れば先に block を足す group を単独で commit する。rename は古い名前と新しい名前の block を探し、満杯の同じ directory では古い名前の block を再利用する。
- disk の形式は変えていない（chunk 512 byte、record は chunk をまたがない）。既存の image はそのまま読め、書ける。HAL の変更は無い。縮めない（rmdir は全 block を返す）。
- 検証（QEMU、amd64 の guest 8 GiB、NVMe の volume）: 作成・削除・rename・rmdir・上限・再起動、journal の volume での同じ試験、電源断 3 回の後の replay、
  guest が書いた volume の host での fsck 相当の検査（`tools/build/check-ufs-image.py`）、coreutils の source の展開、sh 1388/1425・make 91/91・対話 41/41、4 platform の boot。

## 制限と移管

- 間接 block まで育てるのは F-010。間接 block を持つ directory（host の道具が作れる）は読めるが編集は EIO。
- journal 無しの `ufs_rename()` は新しい名前を足してから古い名前を消すので、12 block が満杯の同じ directory の中の rename は ENOSPC（journal の側は再利用する）。
- journal の volume では名前の操作が 1 回約 100 ms で、続けると guest の応答が悪い（[BUG-040](../bugs/BUG-040.md)、変更の前からの性質と見ている）。
- UFS の host 試験の土台は壊れている（[BUG-039](../bugs/BUG-039.md)）。試験は guest と host の image の検査で行った。
- 実機は未実施。
- 残した道具: [plan/tools/ufs/](../tools/ufs/)（`dir-grow.sh`・`check-volume.py`・`crash-grow.sh`・`crash-test.sh`）。

## Phase 一覧

| Phase | 内容 | 結果 |
| --- | --- | --- |
| ws054-p001 | 調査と設計 | cleared（q413-i01） |
| ws054-p002 | journal 無しの経路 | cleared（q414-i01。12 block・4031 まで、coreutils の展開） |
| ws054-p003 | journal の経路 | cleared（q415-i01。電源断 3 回の replay も一貫） |
| ws054-p004 | 回帰・coreutils・規約と敵対的なレビュー | cleared（q416-i01。レビューで退行 1 件を直した） |

Phase の記録は WS の完了で削除した（git の履歴に残る。[q413](../history/queue-q413.md)〜[q416](../history/queue-q416.md)）。
