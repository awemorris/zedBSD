<!-- awesome-plan project=zedbsd record=ws063p001 -->

# ws063-p001: journal の大きさを mkfs で記録し、mount で journal の file を再利用・確保し直し・作成する

Phase ID: `ws063-p001`
Parent: [WS063](../ws.md)
Status: cleared
Queue: q443-i01（cleared）

## 目的（2026-09-26 ユーザー指示）

- 「ジャーナルはメタデータのみにして、ジャーナルサイズはmkfs時に可変にして、デフォルトでは最大128MB、手動の指定で1GBまで。ext4の設定化バランスを真似します。」
- 「mkfsではジャーナルサイズだけ記録して、ジャーナルファイルはマウント時に再利用・再確保・作成などしましょう。ファイル名は.ufs-journalがいいです。将来、ディスクの種類によって移動可能にします。HDDなら外周がいいですし。ファイル名は特殊扱いにして、vfsから作成できないようにしましょう。FreeBSDのUFSとは互換がないので、別のOSで消されることは考えなくてもいいかも。ブロックは連続でなくても、extentを保存すればいいですね。」

## 設計

| # | 内容 |
| --- | --- |
| 1 | mkfs（`zedimage-host ufs`、guest の `mkfs -t ufs`）は superblock の予備の word 1248〜1263 に大きさの記録 `ZJ3R`（MiB、0 は journal なし、checksum）を書く（予備の superblock にも同じ内容）。指定 `--journal-size=MIB`（0〜1024）。指定が無ければ ext4（e2fsprogs）の既定の表で決める: 8 MiB 未満はなし、128 MiB 未満 4、1 GiB 未満 16、2 GiB 未満 32、16 GiB 未満 64、それ以上 128 MiB（既定の上限）。 |
| 2 | 記録の無い古い image は kernel が同じ表で決める。どちらも空き領域の 1/8 を超えるときは縮める（1 MiB 未満ならなし）。 |
| 3 | journal の file は root の directory の `.ufs-journal`。header（先頭の block）は extent の一覧（disk の sector、長さ）を持ち、block は連続でなくてよい（将来の移動も同じ形）。superblock の locator（`ZJ3L`、1220〜1247）が header を指す。header の形式は version 2（v1 の block ごとの一覧は読まない）。 |
| 4 | mount: 有効な journal があり大きさが記録と合えば再利用。大きさが違えば file を 0 に切り詰めて確保し直す。無い・壊れている（locator・header の検査に落ちる）なら作る。壊れていても mount は失敗させない。 |
| 5 | 名前 `.ufs-journal` は root の directory で特別扱い: lookup は `EPERM`（作成・open・unlink・rename の先・link が VFS から全部できない）、readdir は飛ばす。 |
| 6 | 1 回の commit の範囲の数の上限を journal の大きさで決める（slot の payload / 16、2,048〜65,536）。 |
| 7 | 検査の道具（`check-ufs-image.py`）: `ZJ3R` は mkfs が予備にも書くので比較の対象のまま。 |

## 受け入れ

- 4 GiB の root を mkfs（既定）で作ると記録は 64 MiB、mount で 64 MiB の `.ufs-journal` ができる。`--journal-size=1024` の volume で 1 GiB、`--journal-size=0` で journal なし。記録を変えた volume（host で書き換え）で確保し直される。
- 名前: `ls -a /` に出ない、`touch /.ufs-journal`・`rm`・`mv x /.ufs-journal` が `EPERM`。
- 強制終了の試験（`plan/ws063/tests/crash-test-j3.sh`）UFS OK。configure の時間が変わらない。

## 進行の記録（q443-i01、2026-09-26）

### 実装

| 部分 | 内容 | 場所 |
| --- | --- | --- |
| mkfs（host） | `zedimage-host ufs ... --journal-size=MIB`（0〜1024）。指定が無ければ ext4 の表の値。`superblock()` で `ZJ3R` を書くので予備の superblock にも入る。あわせて 8 GiB 以上の image が壊れる既存の不具合（cylinder group の byte 位置を 32 bit で計算して桁あふれ）を直した | `tools/build/zedimage-host.c` |
| mkfs（guest） | `mkfs -t ufs [--journal-size=MIB] FILE`。`make_super` で記録するので書き込みと pristine の検査の両方に効く | `userland/base/mkfs/main.c`・`ufs-format.c`・`ufs-format.h` |
| kernel | header の形式 version 2（extent の一覧: disk の sector と長さ、先頭の block に最大 508 個）。大きさは `ZJ3R` → 無ければ ext4 の表（128 MiB まで）→ 空き＋今の journal の 1/8 まで。mount で再利用（大きさが合う）・確保し直し（切り詰めて再確保）・作成。locator や header が壊れていたら journal なしとして作り直す（mount は失敗させない）。1 回の commit の範囲の上限は slot の大きさで 2,048〜65,536。名前は `.ufs-journal` | `src/drivers/fs/ufs.c` |
| 作成の速さ | journal の file の確保の間は write cached にして最後に 1 回同期。**block を確保するたびの 0 埋めを、metadata の経路（journal に記録されていた）から中身の経路に移し、journal の file の確保では省いた**（0 埋めは commit の前に書かれるので古い中身は見えない） | 同上 |

### 確認（QEMU）

| 確認 | 結果 |
| --- | --- |
| 4 GiB の root（mkfs の既定） | 記録 64 MiB、最初の mount で 64 MiB・13 extent の `.ufs-journal` |
| 10 GiB の volume `--journal-size=1024` | 1024 MiB・145 extent。作成の mount 76 → 3.2 秒（0 埋めを省いた後）。2 回目の mount（再利用）0.35 秒。UFS OK |
| 256 MiB の volume（既定 16 MiB）の記録を host で 32 MiB に書き換え | 確保し直され 30 MiB（空きの 1/8 の制限）、13 extent、中身は保たれ、UFS OK |
| `--journal-size=0` | journal なし、UFS OK |
| 名前 | `ls -a /` に出ない。`touch`・`rm`・`ln -s`・`mkdir`・`cat`・`mv /root/y /.ufs-journal` が `Operation not permitted` |
| 強制終了（`crash-test-j3.sh` 3・7 秒） | 2078・3182 の名前が途切れず、UFS OK |
| configure（`/root`） | 11.29〜11.34 秒（host 10.9〜11.2 秒）。make（直列）17.9 秒 |
| 回帰（8 GiB・512 MiB） | make の差分試験 91/91、SMP 6/6、COW、itimer、swaphog 450 MiB、`dir-grow.sh` VERIFY-OK、後も network が生きている |
| boot test・sh の差分試験 | PASS（`build/boot-test-amd64-ws063/login.png`）、sh 1389/1425（落ちる 1 件 `noclobber on &> >` は `echo baz &` の背景の job の出力の順が scheduling で変わる case。ws061-p005 で新しい子をすぐ他の CPU に移さないようにしてから後に回りやすい。単独では期待どおり） |

## 結果（2026-09-26、cleared）

受け入れを満たした。残り: 新しい code の規約の指摘（ws063-p002）。`--profile=journal-snapshot` の volume は tail の v2 の journal（中身も記録する）のまま。QEMU だけ、実機は未実施。

