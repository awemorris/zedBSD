<!-- awesome-plan project=zedbsd record=ws334-p015 -->

# ws334-p015 custom frame の幾何: frame

- 親: [WS334](../ws.md)、設計 [design.md](../design.md)。
- 状態: cleared（2026-11-01 03:09 Q1、main a448d9343）
- 依存: p004（依存の Phase の実装が main に merge され cleared になってから始める）。
- 担当の想定: Sonnet 5.5 high（phase-runner-low）を 1 回起動（標準廉価実装手順、ws.md の決定 22）。header は p002 の設計で、p004 で確定した物を変えない。

## 読む物

1. AGENTS.md の §4・§7・§9・§10、plan/agents/protocol.md、plan/coding-style.md の全文（§14 の checklist）、docs/agent/spec-module-delegation.md の §5・§7、plan/guardrail.md の「出典」「開発の管理情報」。
2. [design.md](../design.md) の §4.4 の N1・N3、§4.5（titlebar の controls）。
3. 実装する header（各関数の comment が仕様）: `userland/desktop/libkeiland/win32/frame.h`。それが include する header も読む。
4. 仕様: Microsoft Learn の `WM_NCHITTEST`（戻り値の HT* の値）、「Custom Window Frame Using DWM」（`WM_NCCALCSIZE` と hit test の考え方）、「Support snap layouts for desktop apps on Windows 11」（`HTMAXBUTTON` を返すこと）。手元の file は無い。値と規則は header に書いた。

## 作る物

- `userland/desktop/libkeiland/win32/frame.c`（今は骨格。全部の関数を書く）。
- 単体試験 `plan/ws334/tests/unit/test-frame.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- `keiui_win32_scale`: dpi 96・120・144・192・0 で 8・46・16、丸め（8 × 120 / 96 = 10、46 × 144 / 96 = 69、1 × 144 / 96 = 1.5 → 2）、UINT32_MAX × 192 の飽和。
- `keiui_win32_frame_buttons`: 幅 800・caption 32・dpi 96 で close が x 754・幅 46・高さ 32、maximize 708、minimize 662。dpi 144 で大きさが 1.5 倍。全画面と caption 0 で空の 3 つ。幅 100（3 つより狭い）で x が負。NULL で何も書かない。
- `keiui_win32_frame_hit`: header の 1〜7 の各段の代表の点と境の点（band の内と外の 1 px、corner の reach の内と外、button の端、control の端、caption の下の 1 px）を dpi 96 と 144、通常・最大化・全画面で。最大化では端の band が無く上の端の点が button か caption。top band が button を横切る所では TOP_BORDER（4 px）だけ。範囲の外（-1、width、height）は NOWHERE。control が caption の外にあっても 5 の段で CLIENT。
- 乱数: 固定の種で 100000 点と枠（幅・高さ 0〜5000、dpi 72〜480、control 0〜8 個）を作り、答えが header の 15 の値のどれかで、全画面なら CLIENT か NOWHERE、範囲の外なら NOWHERE であることだけを確かめる（落ちない、UB なし）。

## 注意

- 比較は 64 bit で（`rect.x + width` が 32 bit を超えうる）。scale の結果の飽和。
- 期待の値は header の規則から手で求める（Windows の実際の hit test の挙動を真似た他の実装を見ない）。
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

- `timeout 1800 sh plan/ws334/tests/run-unit.sh frame` → 最後の行 `run-unit: PASS`（posix と win32 の両方が流れたことを出力で見る。win32 は初回に Wine の prefix を作るので数分かかる）。
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

- 日付: 2026-11-01 03:10（host の `date`）。担当: Sonnet 5.5（phase-runner-low）。状態: 実装済み・試験 PASS（cleared は Q1）。
- 実装: `userland/desktop/libkeiland/win32/frame.c`（`keiui_win32_scale`・`keiui_win32_frame_buttons`・`keiui_win32_frame_hit` と static の補助 4 つ）。試験: `plan/ws334/tests/unit/test-frame.c`。header は変えていない。
- 実行した command と最後の行（`LC_ALL=C.UTF-8` は Q1 の指示。frame の試験に非 ASCII は無い）:
  - `timeout 1800 sh plan/ws334/tests/run-unit.sh frame` → `test-frame: PASS (1079182 checks) (posix)`・`test-frame: PASS (1079798 checks) (win32)`・`run-unit: PASS`。
  - `LC_ALL=C.UTF-8 timeout 3600 sh plan/ws334/tests/run-unit.sh all` → frame・os-zones・tools が posix・win32 とも PASS、`run-unit: PASS`。
  - `BASE=d393d3267 timeout 1800 sh plan/ws334/tests/check-skeleton.sh` → `check-skeleton: PASS`。
  - `git diff --check` → 出力なし。
- 期待の値は header の規則と手の計算（dpi 96・120・144・192 の band・corner・button の x を表にして点ごとに求めた）。乱数の部分（固定の種、100000 点）は性質だけ（15 の値のどれか、範囲の外は NOWHERE・中は NOWHERE でない、全画面は CLIENT、最大化は端の値が無い、band の深さより内は端の値が無い、close の x）。
- 自分で決めた点（header が黙っている所）:
  - button の x は int32_t に収まらない時、範囲に clamp する（幅が UINT32_MAX 近い、dpi が極端に大きい時だけ）。大きさ（幅・高さ）は scale の飽和のまま。
  - 全画面・caption 0 の空の矩形は x・y・width・height を全部 0 にする。
  - 窓が小さく反対側の band が重なる時（幅・高さが band の 2 倍未満）は、点に近い方の端が決める。同じ距離なら低い側（左・上）。corner の reach が両端に届く時も同じ。
  - top band を 4 px にするのは、点の x が（空でない）3 つの button のどれかの x の範囲にある時（y は見ない。button は y 0 から）。左右の band は thin にならない。
  - 縦の band（左右）の点は、y が corner の reach の中なら corner（TOP_・BOTTOM_）、横の band の点は x が reach の中なら corner。最大化では band 自体が無い。
  - button の判定は close・maximize・minimize の順（重ならないので結果は同じ）。`controls` が NULL で `control_count` が 0 でない時は controls なしと見る。
  - dpi は band・corner・button・caption の全部に `keiui_win32_scale` で同じ dpi を使う。caption の判定は `y < scale(caption_height)`。
- 壊して確かめた（git の外、scratchpad の複写で、posix の ASan 版。原本は変えていないことを `cmp` で確認）: 丸めを半分未満に（FAIL 12）、飽和の上限をずらす（3）、band の深さ +1（622）、top band の thin を無くす（27）、corner の境の比較 > にする（28）、band の下側の比較 <= にする（164）、MAXIMIZE を MINIMIZE に（19）、caption の境 <=（20）、button の x の段を 1 つずらす（5625）、最大化でも band（1751）、全画面の分岐を無効に（1397）、controls を読まない（10）、近い端の同点の向き（1）、clamp の上限を 1 つ大きく・下限を 1 つ小さく（初めは見逃した。int32 の端ちょうどの試験を足して捕まえた、各 1）、x の範囲の比較 >（122）、矩形の端を 32 bit に切る（1）、buttons NULL の検査の削除（crash）、dpi 0 の扱い（69）、corner の reach を 1 つ小さく（8）、close の判定の削除（22）、button の幅に 96 dpi 固定（5533）。最終は見逃し無し。
- 未実施: 実機の Windows（Wine のみ）。aarch64 の Windows での実行（compile のみ）。
