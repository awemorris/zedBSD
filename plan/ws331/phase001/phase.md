<!-- awesome-plan project=zedbsd record=ws331-p001 -->

# ws331-p001: driver の HAL の利用の一覧と、kernel の置き換え先

Status: in-progress
Parent: [WS331](../ws.md)
Queue: q1713（P2）

## 目的

ユーザーの指示（ws.md の原文）の「違反利用された関数が、どの関数に置き換えられるか、一覧」と「ラッパーがない場合…その一覧」を作る。code は変えない。

## 成果（この phase.md に書く）

1. 違反の全数: `src/drivers/`・`include/drivers/` の HAL の header の include（file・行・header）と、`hal_*` の呼び出し（関数・file・行の数）。HAL の型・定数（`HAL_*`・`hal_*_t` の類）だけを使う所も分けて数える。試験の file（`tests/` の下）は分けて示す。
2. 表 A（置き換え先がある物）: `hal_*` の関数 → kernel の既存の関数（`include/kern/` の宣言の file と行）。意味・引数・失敗の値の違いがあれば書く（例: `hal_space_map_device` → `kern_device_map`、`hal_get_arch_handoff` → `kern_boot_handoff`）。
3. 表 B（wrapper が無い物）: kernel に足す関数の案（名前・宣言・意味・置く file・どの driver が使うか）。HAL の型・定数を driver が使っている所は、kernel の型・定数の案も。
4. 例外の候補: arch・board に固有の HAL の header（i386.h・bsp-pc98/display.h・bsp-rpi-common/cons.h）を使う所は、何を使っているかと、kernel の口にするか driver の中に移すかの案。
5. 書き換えの Phase の分け方の案（driver の群ごと、依存の順）。

## 確かめ

- 数は grep の出力から自分で数え、使った command を書く。
