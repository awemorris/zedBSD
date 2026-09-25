<!-- awesome-plan project=zedbsd record=ws067p001 -->

# ws067-p001: `/dev/fd` の一覧・lookup・stat と diff

Phase ID: `ws067-p001`
Parent: [WS067](../ws.md)
Status: in-progress
Queue: q454-i01
Disposition: normal

## 範囲

- `src/kern/devfs.c`: `/dev/fd` の readdir は呼んだ process が開いている descriptor だけを並べる。`/dev/fd/N` と `/dev/stdin`・`stdout`・`stderr` の lookup は、呼んだ process が N を開いていなければ ENOENT（devfs の固定の directory は name cache に入れないので呼び手ごとの答えでよい）。
- `src/kern/syscall.c`: `stat`・`lstat`・`fstatat` で行き着いた node が descriptor の別名なら、その descriptor の `fstat` の結果を返す。
- `userland/base/diff/`: operand が FIFO か文字 device なら中身を読んで比べる（GNU diff と同じ）。

## 受け入れ

WS067 の受け入れ。回帰: build（warning 0）、guest の BUG-054 の再現手順、sh の process substitution の case、boot test。
