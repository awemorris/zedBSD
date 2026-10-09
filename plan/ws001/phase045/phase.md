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

## 2026-10-09 P1 q916: 照合の道具と最初の表

- 道具（`plan/ws001/tests/posix-headers/`）:
  - `fetch.sh OUT`: 規格の header の頁（86）を取る（git に入れない、OUT は build の下）。
  - `extract.py PAGES OUT.json`: 頁の DESCRIPTION から header ごとの名前（types・structs と members・constants・limits（limits.h の省いてよい 3 節は may_omit）・function-like macros・functions・variables・others（人が分ける table の名前））を、option の印（CX・XSI・ADV・OB…）つきで取り出す。markup の読み違いは OVERRIDES・ADDITIONS・NOISE で直す。結果は `posix-2024.json`（git に入れる、86 header、types 342・structs 83・constants 1140・limits 185・macros 19・functions 1219・variables 9）。
  - `check.py OUT [--target …] [--header …]`: header ごとに `_XOPEN_SOURCE=800` で単独に include して全部の名前を参照する C を作り、tree の clang で x86_64・i386・aarch64 に compile（include/libc の後ろに compiler の include だけ）し、probe の行の error を「無い名前」、header の中の error を「単独で通らない」と数える。`table.md`・`results.json`。
- 最初の表（2026-10-09、`table-2026-10-09.md`）: 無い名前 438（base と CX 355、XSI 64、他の option 19。limits.h の省いてよい値 37 は別）。3 arch で同じ。無い header 5（complex.h・cpio.h・monetary.h・tar.h・wordexp.h。tgmath.h は complex.h が無いので通らない）。
  - 種類: constant 239、function 93、limit（XSI）51、member 27、type 14、struct 9、macro 4、variable 1。
  - function 93 のうち、libc に実装があって宣言だけ無い物 5（pthread_kill・pthread_sigmask（signal.h にも要る）、ctermid、tcgetpgrp・tcsetpgrp（unistd.h））、実装の無い物 88（`check.py` の results と libc.so の symbol の照合）。

## 2026-10-09 Q1 の判定: 範囲と分けた Phase

- この Phase: (A) 定数・limits（XSI を含む）・型・macro・構造体と member、(B) cpio.h・tar.h、(C) 小さい関数（宣言の足し 5 と、string・wchar・getsubopt・dprintf・vdprintf・strerror_l・*_l の locale 版・posix_fadvise・if_nameindex・if_freenameindex）。wint_t は最後（着手の前に Q1 へ）。
- 分けた Phase（planning、Q1 の割当まで着手しない）: [p046](../phase046/phase.md) memory stream の関数、[p047](../phase047/phase.md) scheduling の関数と時計、[p048](../phase048/phase.md) netdb の network・protocol の database、[p049](../phase049/phase.md) monetary.h と wordexp.h、[p050](../phase050/phase.md) complex.h と tgmath.h、[p051](../phase051/phase.md) mlock 系と typed memory。
