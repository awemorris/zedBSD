<!-- awesome-plan project=zedbsd record=ws001-p045 -->

# ws001-p045: POSIX.1-2024 の header の全数の照合と補完

Status: planned（2026-10-09 Q1、ユーザー「WS001に、POSIXのヘッダがすべてそろっているチェックして揃えるPhaseを入れておいてください。」）
Parent: [WS001](../ws.md)
Queue: 未割当（ベータ3、P1 の列）

## 経緯

ws126-p002（Python の cross build）で libc の不足が続けて見つかった: `<sys/types.h>` の POSIX の型（q916 item 1）、`_SC_GETGR_R_SIZE_MAX`・`_SC_TTY_NAME_MAX`、`SOMAXCONN`。個別の package で見つけるのでなく、header を全数で照合する。

## 範囲

- POSIX.1-2024（Base Definitions の Headers の章、82 前後の header）の各 header について、libc（`include/libc/`、uapi 経由を含む）での有無と、規格が求める型・定数・macro・関数の宣言・構造体の member を照合する。XSI の印の物は別の列で数える。
- 表（header ごとに 有・無・不足の名前の数）を `plan/ws001/tests/` に置き、照合の script（規格の名前の一覧と、cross compiler で `#include` と名前の参照を試す C の生成）を再利用できる形で置く。
- 足りない宣言・定数・型を補う。宣言はあるが実装の無い関数は一覧にし、小さい物は実装し、大きい物は Phase を分ける（Q1 に提案）。
- 数字の重複を作らない（kernel と共有の値は uapi を参照するか、2 箇所なら comment で対応を書く。2026-10-09 ユーザーの SOMAXCONN の決定: POSIX の header の定数は libc の header に置く）。

## 受け入れ

- 全 header の照合の表と、不足 0（または残りを理由と移管先つきで列挙）。
- x86_64・i386・aarch64 で、各 header 単独の `#include` が c99 -pedantic・c11 で -Wall -Wextra -Werror を通る。
- libc・kernel・既存 userland の build が warning 0。
- HAL の API・toolchain は変えない。
