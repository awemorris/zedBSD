<!-- awesome-plan project=zedbsd record=ws060p002 -->

# ws060-p002: batch の redo journal（v3）の設計

Phase ID: `ws060-p002`
Parent: [WS060](../ws.md)
Status: cleared
Queue: q440-i01（cleared）

## 目的

journal を既定にする（WS063、2026-09-26 ユーザー指示）ための前提として、操作ごとに flush する今の 1 枠の redo journal を、複数の操作をまとめて commit する journal にする設計を書く。ws061-p006 の write cached と両立させる。code は変えない。

## 受け入れ

commit の単位と契機、on-disk の形式、読みの重ね合わせ、file の中身の扱い、crash のときの保証と保証しないもの、journal の置き場所（tail と file system の中）、試験の方法が書かれている。

## 前提（調べた事実）

- UFS の書き込みの出口は 2 つ: `write_sectors_impl`（journal の volume では 1 回ずつ `drv_ufs_journal_commit`、無ければ device へ）と `metadata_group_commit`（複数の範囲を 1 group で `drv_ufs_journal_commitv`）。file の中身（`write_content`）も `write_block` → `write_sectors` を通るので、今は journal の volume で中身まで journal に書いている。
- 今の journal（v2、`ZUJ2`）は file system の末尾の外側の 256 sector の 1 枠。1 回の commit で flush 2 回、checkpoint（元の場所への書き込み）と枠の片付けでさらに flush。200 の作成で 20.6 秒（journal 無し 0.9 秒、ws060-p001）。
- 今の native の root は journal 無しで作られ、file system が partition を埋めていて、末尾の外側に場所が無い。
- ws061-p006 で buffer cache に遅延書き込み（`DISK_WRITE_CACHED`、flusher、`buf_sync`）が入った。

## 設計（v3）

### 1. commit の単位: 走っている transaction

journal の volume の metadata の書き込みは、device へも journal へもすぐには書かず、buffer cache に書いて **pin** する（`buf_journal_pin(disk, lba, count)`）。pin した buffer は flusher・`buf_sync`・追い出しの書き戻しから外れる（dirty のまま待つ）。走っている transaction は pin した範囲の一覧（lba、sector 数、重なりは併合）を持つ。読みは buffer cache を読むので、新しい内容が見える（v2 の重ね合わせの読み `drv_ufs_journal_read` と view は要らなくなる）。

file の中身（`write_content` の経路）は journal に入れず、従来の write cached の書き込み（pin しない）。

### 2. commit の契機

- 1 秒ごと（buffer cache の flusher に UFS が登録する hook）。
- `fsync`・`sync`・unmount・`nojournal` への切り替え。
- 走っている transaction が枠の大きさの上限（下記の slot の payload の 3/4）に届く書き込みの前。
- 走っている transaction で解放した block を allocator が再び割り当てようとしたとき（下記 5）。

### 3. commit の手順（journal の lock の下、metadata の書き込みを止めて）

1. `buf_sync`（pin していない dirty = file の中身と前の commit の元の場所の metadata を書く）。
2. 走っている transaction の範囲の中身を buffer cache から読み、slot（N % 2）に descriptor（範囲の一覧、各 checksum）と payload を書く。
3. flush（1 と 2 が device に届く）。
4. commit の記録（sequence N、descriptor の checksum）を書いて flush。
5. pin を外す（元の場所は普通の dirty になり、flusher か次の commit の 1 で書かれる）。

flush は commit 1 回に 2 回。操作ごとではない。

### 4. on-disk の形式と replay

journal の領域は、header（`ZUJ3`、version、slot の大きさ、checksum）と 2 つの slot。slot は descriptor（複数 sector）、payload、commit の記録。commit N は slot N % 2 に書くので、N−1 の slot は N の commit が確定するまで壊れない。N の commit の記録が device に届く前に（手順 3 の flush で）N−1 の元の場所は device に届いている。したがって mount の replay は **最新の有効な commit の 1 つ**（sequence が最大で checksum が合う slot）を元の場所に書いて flush すればよい。replay は冪等。v2 の tail の枠に未完了の group があれば、従来の replay で片付けてから v3 を使う。

### 5. 保証すること・しないこと

- 保証: 電源断の後、metadata は最後に確定した commit の状態で整合（辿れない inode・二重の割り当てが無い）。fsync・`sync` の後の内容は残る。file の中身は、それを指す metadata の commit より前に書かれる（ordered: 手順 1）。
- 走っている transaction で解放した block は commit まで割り当てない（解放した block の集合を持ち、allocator がそれを選んだら先に commit）。電源断で解放が取り消されたとき、元の file の block が別の file の中身で上書きされていることを防ぐ。
- 保証しない: 最後の commit の後の約 1 秒の変更。metadata と file の中身が同じ 4 KiB の line を共有するとき（fragment の混在）の中身の順序。

### 6. journal の置き場所と作成（WS063）

journal は file system の中の予約した file `/.zedjournal`（普通の regular file。block は inode が所有するので host の検査と整合）。UFS は root の directory の readdir・lookup でこの名前を隠し、利用者は開けない・消せない。大きさは 8 MiB（header + 2 × 約 4 MiB の slot）。journal の I/O は file の block map を mount の時に読んで sector に変換し、buffer cache を通して書く（手順の flush で device に届く）。mount（書き込み可能）で journal が見つからず `nojournal` でなければ、その場で file を作る（WS063 p001）。v3 の engine は ws060-p003 で、作成は mount option `journal` のときだけにして試験する。

### 7. 試験

- host 試験: `plan/ws001/tests/directory-fsync-host-test.c`・`credential-vfs-ufs-socket-fault-host-test.c` を v3 に合わせて通す。
- guest: BUG-040（200 の作成、journal と無しの比較）、`dir-grow.sh`、`plan/tools/ufs/crash-test.sh`（途中で QEMU を止めて UFS OK と fsync の内容）、`check-volume.py`。
- configure（`/root`）が journal 無しの write cached と同程度。

## 結果（2026-09-26、cleared）

上の設計。自分で見直した点: pin した line が file の中身を含むときは中身の書き出しも commit まで遅れる（順序の例外として記録）。journal の volume も `DISK_WRITE_CACHED` にするので、cache を通らない run の書き込みは使われない。設計の review のサブエージェントは AGENTS.md の規則（サブエージェントを使わない）で使っていない。
