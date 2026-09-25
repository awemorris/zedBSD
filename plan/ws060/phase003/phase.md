<!-- awesome-plan project=zedbsd record=ws060p003 -->

# ws060-p003: batch の redo journal（v3）の実装

Phase ID: `ws060-p003`
Parent: [WS060](../ws.md)
Status: cleared
Queue: q441-i01（cleared）

## 目的

ws060-p002 の設計を実装する。tail の journal を持つ volume（`zedimage-host --profile=journal-snapshot`）で動かす。

## 受け入れ

BUG-040: journal の volume の 200 の作成が journal 無しの数倍以内。crash の試験（`plan/tools/ufs/crash-test.sh`）で UFS OK。journal の host 試験（`plan/ws001/tests/` の 2 本）。規約。

## 進行の記録（q441-i01、2026-09-26）

### 実装（設計の ws060-p002 のとおり。違いは下）

| 部分 | 内容 | 場所 |
| --- | --- | --- |
| buffer cache | `buf_write_pinned`（cache に書いて pin、書き戻さない）、`buf_unpin`、書き戻し・`buf_sync`・flusher・追い出しで pin を飛ばす。flusher の hook（`buf_flusher_hook`、登録の lock は `LOCK_RANK_WRITEBACK_CONTROL`） | `include/kern/buf.h`、`src/kern/buf.c` |
| 書き込みの経路 | `write_sectors_impl` に「中身か」の引数。file の中身の 4 か所は `write_content_sectors_context`。v3 が有効なら `j3_write`（metadata は pin して記録、中身は遅延書き込み、走っている transaction で解放した block の中身は pin だけ） | `src/drivers/fs/ufs.c` |
| commit | `j3_commit_locked`: `buf_sync` → slot（sequence の偶奇）に payload と descriptor → `disk_sync` → commit の記録 → `disk_sync` → unpin。契機は flusher の hook（namespace の lock と `ms->lock` の下）、`ufs_sync`（fsync・sync）、unmount、枠が満ちたとき | 同上 |
| 置き場所 | `/.zedjournal`（4 MiB、root の directory の regular file、block は連続でなくてよい。先頭の block の header に全 block の位置）。superblock の予備の word（1220〜1247）の locator（`ZJ3L`、header の fragment、journal の識別子）が header を指す。lookup は `EPERM`、readdir は飛ばす | 同上 |
| replay | mount で superblock の locator から header を読み、2 つの slot の commit の記録のうち最新で未適用のものを検証して元の場所に書き、`disk_sync` の後に header の「適用済み」を更新。root を読む前 | 同上 |
| 既定と option | 書き込み可能な write cached の mount は journal を既定で使い、無ければ作る。`mount -o nojournal` で使わない（replay だけ行う）。`writethru` は journal を使わない。v2 の tail の journal の volume は v2 のまま | `include/uapi/mount.h`（`MNT_NOJOURNAL` 0x8）、`include/kern/mount.h`、`src/kern/syscall.c`、`userland/base/mount/main.c`、`src/drivers/fs/ufs.c` |
| 検査の道具 | 予備の superblock との比較で、実行中に書く locator の word を変わってよい範囲に加えた | `tools/build/check-ufs-image.py` |

WS063-p001 の予定だった既定の有効化・mount の時の作成・`nojournal` はここで入れた（同じ code の経路で、分けると試験が二重になるため）。WS063 には root の強制終了の試験、v2 の tail の volume の扱い、全体の回帰と規約が残る。

host 試験（`plan/ws001/tests/directory-fsync-host-test.mk`・`credential-vfs-ufs-socket-fault-host-test.mk`）はどちらも今回の変更と無関係に build できない（前者は `file.c` と試験の `file_fsync_backend` の二重定義、後者は今は無い `ufs-vfs.c` の断片を要求）。未実施。

### 最初の確認（QEMU、作業 volume 256 MiB を 2 つ）

| 確認 | 結果 |
| --- | --- |
| 起動（root に journal が作られる） | boot test PASS（`build/boot-test-amd64-j3/login.png`）。`ls -a /` に出ない、`ls -l /.zedjournal` は `Operation not permitted` |
| mount の一覧 | 既定 `(rw)`、`-o nojournal` は `(rw,nojournal)` |
| 200 の空 file の作成（BUG-040） | journal 0.364 秒、nojournal 0.361 秒（v2 の journal は 20.6 秒） |
| 強制終了（300 file、3 秒待ち、300 file の作成と 200 の削除の途中で QMP の `quit`） | journal の volume: replay の前の生の volume は辿れない inode 10 個（元の場所が遅れているため、想定どおり）。mount（replay）・unmount の後は **UFS OK**、`a` 300・`b` 270（commit した所まで）。nojournal の volume はこの回はたまたま UFS OK |

### 強制終了の試験（`plan/ws063/tests/crash-test-j3.sh`、journal の無い volume を作り mount で journal を作る、名前を作り続けて QMP で切る）

| 切る時点 | 残った名前 | 途切れない prefix | host の検査（replay・unmount の後） |
| --- | --- | --- | --- |
| 3 秒 | 634 | 634 | UFS OK |
| 5 秒 | 979 | 979 | UFS OK |
| 8 秒 | 1364 | 1364 | UFS OK |
| 13 秒 | 1709 | 1709 | UFS OK |

root: file を作り続けて消し続ける途中で強制終了 → 起動（replay）、fsync した file が残り、`sync` の後の root の partition が UFS OK。

### 計測（root に journal、QEMU 8 GiB 4 vCPU NVMe、host 11.2 秒）

configure（`/root`）13.0〜13.2 秒（journal 無しの write cached 12.3 秒、+6%）、make（直列）20.0 秒。

### 回帰（最終の image）

boot test PASS、make の差分試験 91/91（8 GiB・512 MiB）、`smp-resource-stress` 6/6（8 GiB・512 MiB）、COW、itimer、swaphog 450 MiB（512 MiB、後も network が生きている）、`dir-grow.sh`（root の journal の上）VERIFY-OK、sh の差分試験 1390/1425（前と同じ）。

## 結果（2026-09-26、cleared）

BUG-040 の受け入れ（journal の volume の 200 の作成が journal 無しの数倍以内）: 0.364 秒 / 0.361 秒で達成。crash の試験 4 時点と root で UFS OK。host 試験 2 本は既存の理由で build できず未実施。

残り: 新しい code の規約の指摘（`style-check.py` で約 100 件: 条件の中の呼び出し、閉じ括弧の後の空行、段落の comment、前方宣言の位置）を ws063-p002 の規約の Phase で直す。v2 の tail の journal の volume（`--profile=journal-snapshot`）は v2 のまま（WS063-p001）。QEMU だけ、実機は未実施。
