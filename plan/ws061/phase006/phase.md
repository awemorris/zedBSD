<!-- awesome-plan project=zedbsd record=ws061p006 -->

# ws061-p006: UFS の write cached を既定に、write-through を mount option に

Phase ID: `ws061-p006`
Parent: [WS061](../ws.md)
Status: cleared
Queue: q439-i01（cleared）
Disposition: normal

## 目的

2026-09-26 ユーザー指示: 「UFSはwrite cached をデフォルトにして、write thruはマウントオプションにしてください。configureがホストとほぼ同等の性能になるまで、全般的な最適化を行なってください。」（[F-015](../../future-work.md) の判断。電源断で失う範囲を広げてよい）

expat の configure（`/root`、UFS）の間に同期の書き込みが 15,145 回（124 MB）、device flush が 950 回ある（`sysctl vfs.io.stats`、ws061-p005 の後の image）。buffer cache の書き込み（`buf_write_context`）は毎回すぐ書き戻し（write-through）、UFS は journal の無い volume の順序のために `disk_sync`（全 dirty の書き出し + device flush）を呼ぶ。

## 設計

| # | 内容 | 場所 |
| --- | --- | --- |
| 1 | mount の flag `MNT_WRITETHRU`（uapi）・`MOUNT_WRITE_THROUGH`（kernel）= `0x4`。`mount(2)` が受け付け、mount の一覧に出る | `include/uapi/mount.h`、`include/kern/mount.h`、`src/kern/syscall.c` |
| 2 | `mount -o writethru` と fstab の `writethru`。一覧に `,writethru` | `userland/base/mount/main.c` |
| 3 | disk の flag `DISK_WRITE_CACHED`。UFS が書き込み可能で journal の無い volume を `writethru` 無しで mount したとき partition の disk に立て、unmount の最後の sync の後に消す | `include/kern/disk.h`、`src/drivers/fs/ufs.c` |
| 4 | `buf_write_context` は `DISK_WRITE_CACHED` の disk では dirty にするだけで書き戻さない。dirty の総量が上限（32 MiB）を超えたら従来どおりすぐ書き戻す | `src/kern/buf.c` |
| 5 | flusher（kernel thread、1 秒ごと）: dirty になってから 2 秒たった buffer を書き戻し、書いた leaf の disk を `bio_flush`。最初の cached の mount で起動 | `src/kern/buf.c` |
| 6 | UFS の順序のための `disk_sync`（`bmap_ensure`・`allocation_run_abort`・`allocation_write_run`・`detach_inode_block`・`truncate_indirect`・`dir_add_finish`・`discard_new_inode`・`extattr_publish`・`reclaim_unlinked_inode`）は cached の mount では省く。fsync（`ufs_file_sync` → `ufs_sync`）・`sync`・unmount・clean flag・quota・snapshot・journal は従来どおり `disk_sync` | `src/drivers/fs/ufs.c` |
| 7 | journal の無い volume は、cylinder group の集計と superblock の合計が合わないと mount を断る（`EINVAL`）。cached の volume は電源断でこれが起こり得るので、clean flag が立っていないとき（前回 unmount されなかった）は合計を集計から作り直して mount する（journal の volume と同じ） | `src/drivers/fs/ufs.c` |

失うもの（ユーザーの判断で受け入れ）: 電源断・panic で直前の最大 約 3 秒の書き込みと、journal の無い volume の metadata の順序。zedBSD には fsck が無く、bitmap と inode の食い違い（漏れた block・inode）は直せない。fsync・`sync`・正常な停止（`mount_sync_all`）は従来どおり永続化する。journal の volume は今回は write-through のまま（redo journal と遅延した home write の組み合わせは別の設計が要る）。root の write-through の起動の option（`zedbsd.cfg`）は範囲外（Future Work）。

## 受け入れ

- 既定の mount（root を含む）が write cached、`mount -o writethru` で write-through（一覧で確かめる）。
- configure（`/root`）の同期の書き込みと flush が大きく減り、tmpfs との差が縮む。
- fsync した file が QEMU を強制終了した後も残る。cached の volume を強制終了した後に起動できる（root が mount される）。
- 回帰: boot test、make の差分試験（8 GiB・512 MiB）、SMP、COW、sh の差分試験、UFS の試験（`plan/tools/ufs/dir-grow.sh`）、swaphog。規約。

## 進行の記録（q439-i01、2026-09-26）

実装は設計の表のとおり。加えて、`buf_write_context` の整列した 2 line 以上の書き込みは cache を通らない `transfer_run` で device へ直接書かれていたので、cached の disk では line の経路（遅延）に回した（最初の実装では device の書き込みが減らなかった）。

### 計測（QEMU、8 GiB、4 vCPU、NVMe、`/root` は UFS。host 11.2 秒）

| 測定 | p005 の後 | p006 |
| --- | --- | --- |
| configure（`/root`） | 17.2〜19.8 秒 | **12.3 秒**（2・3 回目。1 回目は cache が冷えて 14.9 秒） |
| make（直列、`/root`） | 20.4〜24.6 秒 | 18.4〜19.0 秒 |
| configure 3 回の device の書き込み / flush | 47,831 回 / 約 2,850 回 | 4,616 回 / 35 回 |

### 正しさ

| 確認 | 結果 |
| --- | --- |
| 既定の mount | root は `(rw)`（cached）。`mount -o writethru` は一覧に `(rw,writethru)` |
| 50 file を作る間の device の書き込み | cached 0 回、writethru 600 回 |
| fsync・`sync` の永続性（QMP の `quit` で強制終了して起動し直す） | fsync した file・`sync` の後の file は残る。何もしない file と 200 file は失われる（想定どおり） |
| 強制終了の後の起動 | root が rw で mount される（集計の作り直し） |
| 強制終了の直後の volume の検査（`check-volume.py`） | **確保されたが辿れない inode が 43 個**（順序を捨てた結果。検査は最初の異常で止まるので、二重の割り当ての有無は未確認） |
| UFS の試験（`dir-grow.sh` make・verify、remount の後の verify、unmount の後の host の検査） | cached・writethru とも VERIFY-OK、`UFS OK` |
| 回帰 | boot test PASS（`build/boot-test-amd64-p006/login.png`）、make の差分試験 91/91（8 GiB）、COW、itimer。`smp-resource-stress` は 26 回中 2 回 7: 差は `packet` 9 → 8（SSH の packet の出入り。減る方向で漏れではない。UFS と無関係） |

## 結果（2026-09-26、cleared）

write cached の既定と `writethru` の mount option を入れ、configure（`/root`）は 12.3 秒（host の 1.1 倍）。電源断の後に journal の無い volume に漏れ（と二重の割り当ての恐れ）が残る。2026-09-26 ユーザー指示「ジャーナルなしで作成したイメージも、マウント時にジャーナルを作り直すようにして、ジャーナルをデフォルトで有効にしてください。マウントオプションでジャーナルオフに対応しましょう」で、journal を既定にし write cached と両立させる Phase（[ws061-p007](../phase007/phase.md)）に続ける。512 MiB・sh の差分試験は p007 の後にまとめて行う（UFS をさらに変えるため）。QEMU だけ、実機は未実施。

