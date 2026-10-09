<!-- awesome-plan project=zedbsd record=ws001-p045 -->

# ws001-p045: POSIX.1-2024 の header の全数の照合と補完

Status: planned（2026-10-09 Q1、P1 q916 に割当。ユーザー「WS001に、POSIXのヘッダがすべてそろっているチェックして揃えるPhaseを入れておいてください。」）
Parent: [WS001](../ws.md)
Queue: 未割当（ベータ3、P1 の列）

## 経緯

ws126-p002（Python の cross build）で libc の不足が続けて見つかった: `<sys/types.h>` の POSIX の型（q916 item 1）、`_SC_GETGR_R_SIZE_MAX`・`_SC_TTY_NAME_MAX`、`SOMAXCONN`。個別の package で見つけるのでなく、header を全数で照合する。

## 範囲

- POSIX.1-2024（Base Definitions の Headers の章、82 前後の header）の各 header について、libc（`include/libc/`、uapi 経由を含む）での有無と、規格が求める型・定数・macro・関数の宣言・構造体の member を照合する。XSI の印の物は別の列で数える。
- 表（header ごとに 有・無・不足の名前の数）を `plan/ws001/tests/` に置き、照合の script（規格の名前の一覧と、cross compiler で `#include` と名前の参照を試す C の生成）を再利用できる形で置く。
- 足りない宣言・定数・型を補う。宣言はあるが実装の無い関数は一覧にし、小さい物は実装し、大きい物は Phase を分ける（Q1 に提案）。
- 数字の重複を作らない（kernel と共有の値は uapi を参照するか、2 箇所なら comment で対応を書く。2026-10-09 ユーザーの SOMAXCONN の決定: POSIX の header の定数は libc の header に置く）。

## 含める既知の不足

- wint_t（2026-10-09 ユーザー、クリック「WS001 p045 で直す」）: libc の `<wchar.h>` の wint_t は uint32_t、clang の zedBSD target の `__WINT_TYPE__` は int。大きさは同じだが int にすると libc++.so.1 の `basic_streambuf<wchar_t>::overflow`・`pbackfail` の mangled 名が Ej→Ei に変わるので、libcxx（toolchain、main の許可で unlock）の作り直しと一緒に直す。P1 の調べは ws126-p002 の phase.md。

## 受け入れ

- 全 header の照合の表と、不足 0（または残りを理由と移管先つきで列挙）。
- x86_64・i386・aarch64 で、各 header 単独の `#include` が c99 -pedantic・c11 で -Wall -Wextra -Werror を通る。
- libc・kernel・既存 userland の build が warning 0。
- HAL の API・toolchain は変えない。

## 2026-10-09 P1 q916: 着手と中断（WS192 の割り込み、Q1）

- 規格の取得: POSIX.1-2024（Issue 8）の header の索引 `https://pubs.opengroup.org/onlinepubs/9799919799/idx/head.html` から 86 の header の頁を取得（P1 の worktree の build/ws001-posix-ref/pages、git に入れない）。
- 頁の形（照合の script の設計の材料）: 記号の定数は `<dt>NAME</dt>`（limits は `{NAME}`）、型と構造体の名前は `<b>name</b>`（「`<b>ipc_perm</b>` structure」）、構造体の member は `<pre><tt>型 名前 </tt>説明` の行、関数は DESCRIPTION の `<pre><tt>` の prototype。option の印は `<sup>[<a …>XSI</a>]</sup>` の後の `opt-start.gif`〜`opt-end.gif` の範囲（数: CX 199、XSI 114、OB 16、ADV 11 ほか）。
- 再開点: `plan/ws001/tests/posix-headers/` に extract（頁 → 名前の一覧の JSON、option の印つき）と check（header ごとに C を生成し x86_64・i386・aarch64 の clang で compile、未宣言の名前を数えて表を作る）を作る所から。wint_t は最後（toolchain の判断を Q1 へ）。
