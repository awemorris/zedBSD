<!-- awesome-plan project=zedbsd record=ws034p041 -->

# ws034-p041: curses の termcap API（`tgetent`・`tgetstr`・`tgoto`・`tputs`）

Phase ID: `ws034-p041`
Parent: [WS034](../ws.md)
Status: **cleared**（q333-i01、2026-09-23）
Phase disposition: normal
Queue: q333（q333-i01）
実行: メインセッション

## なぜ

ws034-p005（libc・カーネル是正の受け皿）に集めた不足のうち、termcap API を切り出した。vim は `tgetent` が必須、
emacs は `tputs` が必須、gdb の readline も使う（inventory §3.8）。
**2026-09-23 のユーザー決定: termcap API は base の curses に足す。ncurses は使わない。**

## 入れたもの

| 場所 | 内容 |
| --- | --- |
| `userland/base/curses/termcap.c` | `tgetent`・`tgetflag`・`tgetnum`・`tgetstr`・`tgoto`・`tputs`、それに `tparm`・`tiparm`。大域の `PC`・`UP`・`BC`・`ospeed`。2文字の termcap の code を terminfo の名前へ引く表（約120件）を持ち、`setupterm()` が読んだ terminfo の記述から答える。termcap の database は無い |
| `include/libc/termcap.h`・`include/libc/term.h` | termcap の宣言と、terminfo 側（`setupterm`・`tigetstr`・`tparm`・`tiparm`・`putp`）の宣言 |
| `userland/base/curses/Makefile` | `termcap.c` を curses library に加えた |
| `platform/amd64/vmunix.mk` | **`/lib/libcurses.a` を position independent な object から作る**（下記） |

仕様の要点:
- `tgoto(cm, 列, 行)` は termcap の順（列が先）で受け、terminfo の `cup`（行が先）へ入れ替えて展開する。
  展開できない文字列には termcap の慣習どおり `"OOPS"` を返す。
- `tgetstr(id, &area)` は area へ写して `*area` を進める。area が無ければ library の持つ文字列を返す。
- `tputs` は `$<…>` の padding を書かずに飛ばす（このシステムが扱う端末は padding を要らない）。
- 表に無い code、記述に無い capability は「無い」（flag 0、数 -1、文字列 NULL）。

### 見つけて直した問題: `/lib/libcurses.a` が PIE から使えなかった

amd64 の `libcurses.a` は base の静的な program 用の object（`-fno-pic`）から作られていた。cross toolchain
（WS032）が作る program は PIE なので、**外部 package が `-lcurses` で link すると relocation の error になる**。
termcap API を足しても vim などは使えなかったことになる。`libcurses.a` は image に data として入るだけで
base の program は link していないので、共有 library と同じ `-fPIC` の object（`$(BUILD)/dynamic/obj`）から作るようにした。
i386 など他の platform の `libcurses.a` はそのまま（WS034 の受入は QEMU amd64 だけ）。

## 検証

| 検証 | 結果 |
| --- | --- |
| amd64 `world`・`disk-image` | PASS。新しい warning なし |
| cross toolchain（PIE）で `libcurses.a` を link | 修正前は `R_X86_64_32 cannot be used against local symbol` で失敗、修正後は link できる |
| ゲスト（QEMU、KVM）で `termcap-target`（`xterm-256color`） | `TERMCAP PASS`: 無い端末は 0、`co`/`li`/`Co`/`am`、表に無い code、area への写し、`tgoto(cm, 5, 2)` = `\E[3;6H`、`mr`、記述に無い `so` は NULL、`tparm`/`tiparm` の setaf、`tputs` の padding 除去、`UP` |

証拠: `evidence/guest.txt`。

## 残したこと

- **base の terminfo の記述が小さい。** `xterm-256color.zti` には `smso`・`smul`・`csr`・`il1`・`dl1` などが無い。
  vim や emacs は無い capability を避けて動くが、表示が遅く・貧しくなる。記述を足すのは vim・emacs の Phase
  （ws034-p008・p010）で、使われ方を見てからにする。
- 他の platform の `libcurses.a` は PIC でない。
- 試験の image に clang が無いので、ターゲット上での compile（`cc x.c -lcurses`）は試していない。
