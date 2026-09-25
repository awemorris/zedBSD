<!-- awesome-plan project=zedbsd record=ws067p002 -->

# ws067-p002: 規約の適合

Phase ID: `ws067-p002`
Parent: [WS067](../ws.md)
Status: in-progress
Queue: q455-i01
Disposition: normal

## 範囲

WS067 で変えた source（`src/kern/devfs.c`・`syscall.c`、`userland/base/diff/`）を [coding-style.md](../../coding-style.md) の全文で見直し、style-check の指摘を 0 にする。

## 受け入れ

style-check の指摘 0、見直しの記録、build（warning 0）、boot test。
