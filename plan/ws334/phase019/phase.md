<!-- awesome-plan project=zedbsd record=ws334-p019 -->

# ws334-p019 ICO の writer: ico（旧 T3）

- 親: [WS334](../ws.md)、設計 [design.md](../design.md)。
- 状態: cleared（2026-11-01 03:26 Q1、main 875c16efb）
- 依存: p004（依存の Phase の実装が main に merge され cleared になってから始める）。
- 担当の想定: Sonnet 5.5 high（phase-runner-low）を 1 回起動（標準廉価実装手順、ws.md の決定 22）。header は p002 の設計で、p004 で確定した物を変えない。

## 読む物

1. AGENTS.md の §4・§7・§9・§10、plan/agents/protocol.md、plan/coding-style.md の全文（§14 の checklist）、docs/agent/spec-module-delegation.md の §5・§7、plan/guardrail.md の「出典」「開発の管理情報」。
2. [design.md](../design.md) の §6.4（icon の大きさ）、§8 の T3。
3. 実装する header（各関数の comment が仕様）: `apps/browser-win32/tools/ico.h`。それが include する header も読む。
4. 仕様: Microsoft「Icons」（MSDN Library, Win32 の icon の file の形、ICONDIR・ICONDIRENTRY・画像の形）: `/home/awe/zedBSD-claude1/build/sources/specs/ws334/icons-ico.html`（SHA-256 `44922354f299da5650bc6a5b067c94cc2c2b1e8952d170963106af48635fca64`、2026-11-01 取得、https://learn.microsoft.com/en-us/previous-versions/ms997538(v=msdn.10)）。BITMAPINFOHEADER は Microsoft Learn の `BITMAPINFOHEADER` の頁。PNG の signature は PNG（ISO/IEC 15948）の §5.2 の 8 byte。git に入れない。

## 作る物

- `apps/browser-win32/tools/ico.c`（今は骨格。全部の関数を書く）。
- 単体試験 `plan/ws334/tests/unit/test-ico.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- 1 枚の 16 × 16 の DIB: 出力の bytes を header の形から手で組み立てた期待と全部比べる（ICONDIR 6 byte、ICONDIRENTRY 16 byte、BITMAPINFOHEADER 40 byte（biHeight 32、biBitCount 32、biCompression 0、biSizeImage は XOR と AND の合計）、下の行からの BGRA、AND の mask は 1 行 4 byte（16 bit を 32 bit に詰める）で alpha 0 の所が 1）。
- 幅が 32 bit の倍数でない大きさ（20 × 20・24 × 24・40 × 40・48 × 48）の AND の mask の行の詰め。stride が width より大きい入力。
- 256 × 256 の PNG の entry（試験が作った最小の PNG の bytes、signature の後は何でもよい）が directory で幅・高さ 0 で bytes がそのまま。8 つの大きさ（16・20・24・32・40・48・64・256）の組で offset が隙間なく並ぶ。
- 誤り: count 0・256、大きさ 0・257、pixels と png の両方・どちらも無い、stride < width、PNG の signature の違い → EINVAL と `*out` NULL・`*length` 0。
- 試験の中に独立の小さな読み手（directory と BITMAPINFOHEADER を読み、pixel を戻す）を書き、往復で pixel が戻る（alpha 0 の pixel も）。

## 注意

- この module は host の道具（p031 が `tools/make-ico.c` で使う）。Windows の header を使わない。整数は little-endian で byte ごとに。
- POSIX の file（`*-posix.c`）と OS に依らない file に `#ifdef _WIN32`・`<windows.h>` を書かない（`check-skeleton.sh` が見る）。Windows の file（`*-win32.c`）は `<windows.h>`・`<winsock2.h>` などを include してよいが、W の API だけを使い（A の API を使わない）、`<jpeglib.h>` を include しない。
- 他の実装（Chromium・Firefox・Wine・curl・libuv・glib・Qt など）の code・comment・構造・名前を写さない。Win32・POSIX の API の使い方は API の reference から書く。
- source の comment に WS・Phase の番号・plan/ への参照・日付を書かない。各関数は coding-style の形（multi-line の定義、宣言の群、comment の付いた段落、成功の return を最後に）。
- 試験は header の約束を全部の誤りの場合まで確かめる。期待の値は header・API の reference・手の計算から作り、他の実装の出力を使わない。Windows の側の期待を Windows の API で独立に求めてよい（試験の file の中で、実装と別の道で）。
- 試験の file は `plan/ws334/tests/unit/test-<名前>.c`。`#include "check.h"` の `CHECK`・`CHECK_EQ`・`check_finish("<名前>")` を使い、`main(argc, argv)` の `argv[1]` が "posix" か "win32"。今の directory は試験ごとの空の directory で、file を作ってよい。

## 変えてはいけない物

- 全部の module の header（`plan/ws334/tests/modules.sh` の `ws334_headers` の全部）。header の不備・矛盾・足りない口を見つけたら直さずに止めて Q1 に返す（どの header のどの関数か、何が足りないか、案の差分）。header が黙っている所は自分で決め、「結果」に列挙する。
- 担当の外の `.c`（他の Phase の module、libbrowser・libmedia の既存の file）、`apps/browser-win32/Makefile`・一番上の `Makefile`、試験の道具（`plan/ws334/tests/modules.sh`・`run-unit.sh`・`check-skeleton.sh`・`unit/check.h`・`unit/test-tools.c`）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない（消す物は path と理由を Q1 へ）。push しない。

## 確かめの command（worktree の root で）

- `timeout 1800 sh plan/ws334/tests/run-unit.sh ico` → 最後の行 `run-unit: PASS`（posix と win32 の両方が流れたことを出力で見る。win32 は初回に Wine の prefix を作るので数分かかる）。
- `timeout 3600 sh plan/ws334/tests/run-unit.sh all` → `run-unit: PASS`。
- `BASE=d393d3267 timeout 1800 sh plan/ws334/tests/check-skeleton.sh` → `check-skeleton: PASS`（header と試験の道具が変わっていない、全部の .c が cc・clang・x86_64 と aarch64 の llvm-mingw で warning 0、`make browser-win64`・`browser-winarm64` が通る）。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 試験が通った後、実装をわざと数通りに壊して（git の外で。例: 誤りの写しを 1 つ違える、境界の比較を 1 つずらす、Windows の側の変換の長さを 1 つ違える）試験が FAIL することを確かめ、元に戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。
- 自分の diff を coding-style の §14 の checklist で見直した。
- 新しい file を `git add` してから `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と最後の行・自分で決めた点・未実施を Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、command、PASS の件数、自分で決めた点、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1）。

## 結果

- 2026-11-01 03:20 実装済み・試験 PASS（cleared は Q1 の判定）。担当 Sonnet 5.5（phase-runner-low）、worktree `s-ws334-p019`。
- 作った物: `apps/browser-win32/tools/ico.c`（全部の関数）、`plan/ws334/tests/unit/test-ico.c`。header・他の module・試験の道具は変えていない。
- 実行: `LC_ALL=C.UTF-8 timeout 1800 sh plan/ws334/tests/run-unit.sh ico` → `test-ico: PASS (39722 checks) (posix)`、`test-ico: PASS (39722 checks) (win32)`、`run-unit: PASS`。
  `run-unit.sh all` と `check-skeleton.sh` の結果は下の「全体の確認」。
- 試験の中身: 16 × 16 の DIB の全 byte を header の形から手で書いた期待（ICONDIR 6・ICONDIRENTRY 16・BITMAPINFOHEADER 40 の literal、下の行からの BGRA、AND の mask）と比べる。
  幅 1・5・20・24・32・33・40・48・65 の mask の行の詰め（長さは形から手で計算）、stride が幅より大きい入力（幅の外の画素を目印の値にして、tight と同じ出力）、
  256 × 256 の PNG（幅・高さ 0、bytes がそのまま、offset）、8 つの大きさ（16〜64 の DIB と 256 の PNG）の offset の隙間なし、count 255 と 2 つの同じ大きさ、
  全部の誤り（NULL・count 0/256・大きさ 0/257・pixels と png の両方/無し・stride 不足・signature の 8 byte を 1 つずつ変える・総量が 32 bit を超える）の EINVAL と `*out` NULL・`*length` 0、
  試験の中の独立の読み手（directory と BITMAPINFOHEADER を読み、mask が alpha 0 の所だけ 1 で詰めが 0 であることも確かめる）による往復、alpha 0 の画素が色を保つこと。
- わざと壊した試験（git の外、posix、毎回 FAIL を確かめて `cmp` で元に戻した）: mask の bit の向き、高さを 2 倍にしない、mask の行の詰めの式、stride の比較（`<=`）、
  count の上限、count 0 の拒否、行の順（上から）、signature の比較の長さ、総量の比較、biSizeImage、alpha 0 の判定、BGR の順、offset の進み、
  pixels/png の無し・両方の拒否、PNG の最小の長さ、PNG の幅の byte、大きさの上限・0 の拒否、stride の桁あふれの拒否、`*out`・`*length` の初期化、
  256 を 255 にする（幅・高さ）、biBitCount。`if (width == 256) width_byte = 0` を `== 0` に壊した物だけ PASS したが、
  `(unsigned char)256` が 0 になるので同じ動作の変異で、見逃しではない。
- 自分で決めた点（header が黙っている所）:
  - PNG の entry の directory の幅・高さは、image の `width`・`height` の値（256 は 0）。PNG の中身は読まない。PNG の entry では `stride` は見ない。
  - PNG の `png_length` が 8 未満、または 32 bit を超えるものは EINVAL。signature 以外の bytes は読まず、そのまま複写する。
  - BITMAPINFOHEADER は biSize 40・biWidth・biHeight（高さの 2 倍）・biPlanes 1・biBitCount 32・biCompression 0・biSizeImage（XOR と AND の合計）、他の field（解像度・色数）は 0。仕様の「使うのは biSize・biWidth・biHeight・biPlanes・biBitCount・biSizeImage、他は 0」に従う。
  - AND の mask は下の行から、左の画素が byte の最上位 bit、行は 4 byte の倍数に詰め（詰めは 0）。mask が 1 なのは alpha がちょうど 0 の画素。alpha 0 の画素の RGB はそのまま書く（premultiply しない）。
  - header の誤りの一覧に無いが、`(height - 1) * stride + width` が size_t に収まらない stride（読み出しの位置が数えられない）も EINVAL にした。
  - NULL 引数（images・out・length のどれか）は EINVAL。`out`・`length` が NULL でなければ、誤りの時に `*out` NULL・`*length` 0 にする（NULL の引数自身には書かない）。
  - 検査は全部の image について malloc の前に行う。割り当ては calloc（mask の詰めの 0 を保つため）。
- header・phase.md と仕様の食い違い: 無し。
- 未実施: `*length` が 32 bit を超える現実の大きさの割り当て（試験は長さを言うだけで 8 byte の signature だけ本物の PNG で、拒否が割り当ての前であることを見る）、ENOMEM の経路の試験、
  Windows の実際の icon 読み出し（LoadImage など）での往復。aarch64 の Windows は compile だけ（check-skeleton.sh）。

### 全体の確認

- `LC_ALL=C.UTF-8 sh plan/ws334/tests/run-unit.sh all` → 6 本の試験が posix と win32 の両方で PASS、最後の行 `run-unit: PASS`（test-ico は各 39722 checks）。
  1 回目は test-tools の win32 だけが「wine server に繋がらない」で FAIL（別の担当の Wine が同時に動いていた為の競合で、ico と無関係。変更なしの再実行で PASS）。
- `BASE=d393d3267 sh plan/ws334/tests/check-skeleton.sh` → `check-skeleton: PASS`（headers: 19、sources: 22）。`git diff --cached --check` 問題なし。
