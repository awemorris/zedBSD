<!-- awesome-plan project=zedbsd record=ws067p001 -->

# ws067-p001: `/dev/fd` の一覧・lookup・stat と diff

Phase ID: `ws067-p001`
Parent: [WS067](../ws.md)
Status: cleared
Queue: q454-i01
Disposition: normal

## 範囲

- `src/kern/devfs.c`: `/dev/fd` の readdir は呼んだ process が開いている descriptor だけを並べる。`/dev/fd/N` と `/dev/stdin`・`stdout`・`stderr` の lookup は、呼んだ process が N を開いていなければ ENOENT（devfs の固定の directory は name cache に入れないので呼び手ごとの答えでよい）。
- `src/kern/syscall.c`: `stat`・`lstat`・`fstatat` で行き着いた node が descriptor の別名なら、その descriptor の `fstat` の結果を返す。
- `userland/base/diff/`: operand が FIFO か文字 device なら中身を読んで比べる（GNU diff と同じ）。

## 受け入れ

WS067 の受け入れ。回帰: build（warning 0）、guest の BUG-054 の再現手順、sh の process substitution の case、boot test。

## 実装（2026-09-26）

- `src/kern/devfs.c`: `devfs_descriptor_held()`（呼んだ process が descriptor を持つか、持つ file の型）。`/dev/fd` の readdir は持つ descriptor だけを、その file の型で並べる。`/dev/fd/N`・`/dev/stdin`・`stdout`・`stderr` の lookup は持たない N なら ENOENT（node を作らない）。node の説明の comment を fdescfs と同じ方式に書き直した。
- `src/kern/syscall.c`: `descriptor_getattr()` を `fstat` から分け、`stat`・`lstat`・`fstatat` で行き着いた node が descriptor の別名（`i_descriptor_alias`）なら、その descriptor の属性を返す。
- `userland/base/diff/`: operand の一方でも FIFO か文字 device なら、両方を `$TMPDIR`（既定 `/tmp`）の一時 file に写して比べ、出力には operand の名前を出す（text の callback に label を足した）。

## 検証（2026-09-26）

| 確認 | 結果 |
| --- | --- |
| build（`-Werror`） | warning 0 |
| host の diff（Linux で build） | `diff f1 <(..)`・`diff <(echo 1) <(echo 1)` は 0、`<(echo 1) <(echo 2)` は差と 1、`-q`、`/dev/null`。一時 file は残らない |
| guest の BUG-054 の再現手順（QEMU 8 GiB 4 vCPU NVMe） | `ls /dev/fd` は 0 1 2 だけ、`ls -l /dev/fd/ 3</etc/passwd` は 0〜2 が `p`、3 が regular。`ls -lL /dev/fd/0 </etc/passwd` は regular、pipe は `p`、`/dev/stdin` も同じ。開いていない `/dev/fd/7` は ENOENT、`exec 7<` の後は在る。`diff <(echo 1) <(echo 1)` は 0、`<(echo 2)` は差と 1、`diff f1 <(..)` は 0。`ls -l /dev/fd` の 30 回の繰り返しの後も SSH は生きている。`/dev/fd/0`・`/dev/stdin` の open（dup）は今まで通り |
| sh の差分試験（guest、1458 件） | 1412/1458、失敗の一覧は ws065-p003 と同じ。`bash-extensions.sh` に `diff <(..) <(..)` を戻した（host 32/32） |
| boot test（`BOOT_MODE=uefi-nvme`） | PASS（`build/boot-test-amd64-ws067p001/login.png`） |

実機は未実施。制限: `ls /dev/fd` は ls が開いた directory の descriptor を出さない（一覧は directory の open の時に作るため。Linux は出す）。

## 結果（2026-09-26、cleared）

`/dev/fd` は呼んだ process の descriptor だけを見せ、`stat` は descriptor の file を報告し、`diff` は pipe を中身で比べる。BUG-054 は QEMU で resolved。

