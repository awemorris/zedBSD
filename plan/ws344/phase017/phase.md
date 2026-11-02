<!-- awesome-plan project=zedbsd record=ws344-p017 -->

# ws344-p017 VP8 の decoder（vp8.c）

- Status: in-progress（P2、q1966-i01。実装済み・試験 PASS、cleared は Q1 が決める）
- 親: [ws.md](../ws.md)、設計: [design.md](../design.md)。担当の model: Opus を推す（phase-runner）。依存: p002, p011, p012, p013, p014, p015, p016（fixture-check は riff.c を使う）。
- 目的: key frame 全体: macroblock の loop、edge の値、再構成、frame 全体の loop filter。
- 基点の SHA（header と道具の凍結）: `aa2426a5763975c358cf667aff3e17bfb841b7e2`。

## 1. 読む物

- AGENTS.md §4（担当の共通の規則）・§7（禁止）・§9（検証）・§10（コード）、plan/agents/protocol.md、docs/agent/spec-module-delegation.md §5.3・§7（出典と license）。
- plan/coding-style.md の全文（ANSI C、宣言は関数の先頭、段落ごとの comment、if・loop・return の comment、Review checklist）。
- plan/guardrail.md の「MIT・GPL の出典のデータの引用」（block ごとの複数行の comment「Values referenced: <仕様と版> <節・表>」「Procedure referenced: ...」、file の先頭に書かない）。
- plan/ws344/design.md の §1（出典と RFC の既知の誤り）・§4（決定）・§7（上限と信用できない入力）・§8（試験の道具）と、§5 のこの module の行。
- `userland/desktop/libwebp-compat/common.h`（誤りの値・算術の規則。全 module の約束）。
- 仕様（git の外、著作物）: RFC 9649 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc9649.txt`（SHA-256 `f4d7cd92b015c0b340f10a932bb03e564a758a17df50cbb992c16d8459d60e58`）、RFC 6386 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc6386.txt`（SHA-256 `c044997ae5e26f74dc28569f8866937412b2783e887c96e0c09372af78be411c`）、RFC 1951 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc1951.txt`（SHA-256 `5ebf4b5b7fe1c3a0c0ab9aa3ac8c0f3853a7dc484905e76e03b0b0f301350009`）。text なので節は `sed -n '/^13\.3\./,/^13\.4\./p' <file>` のように読む（頁の区切りの行 `Bankoski, et al.`・`RFC 6386 ...` は無視）。
- `vp8.h`（仕様）と vp8-*.h の全部、`bool.h`。
- RFC 6386 §5、§12（edge の値と above-right の規則）、§13、§14、§15.1、§19.3、§20.4（dixie.c の decode_frame の流れ）。

## 2. 作る物

- `vp8.c`、`unit/test-vp8.c`。
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 試験の項目

- boolwriter で組んだ小さな frame: 16x16 の DC_PRED・係数無し・filter level 0 → 全画素 128（手で）、2x2 macroblock の B_PRED で frame の端の edge の値（127・129・角）が予測に効くこと、skip の macroblock、partition 2 で行が交互の partition から読まれること、partition の終わりを越えると ENOBUFS。
- fixture: `run-fixtures.sh lossy` が全部 PASS（全ての非可逆の fixture で dwebp の -yuv と -nofilter -yuv に全 sample 一致。segment・simple と normal の filter・sharpness・partition 1〜8・大きさ 1x1〜1024x683 を含む）。
- 全ての単体試験は ASan・UBSan で落ちず（run-unit.sh）、途中で切れた入力・範囲の外の値・乱数の入力で決められた誤りを返す。
- 期待の値は仕様の表・式と手の計算、test の側の逆の処理（lbitwriter・boolwriter で組んだ入力）だけから作る。他の実装の出力を期待の値にしない（fixture の全体の試験は道具が libwebp の出力と比べる）。

## 4. 注意

- intra 予測は filter 前の画素から（frame 全体を再構成した後で filter する、§15.1）。
- edge: 上の行の外は 127（角も、上の行の macroblock）、左の列の外は 129（上の行より下の左の列の macroblock の角も 129）。subblock の above-right: 右の列の subblock の行 1〜3 は macroblock の above-right（行 −1 の列 16〜19）、右端の macroblock（上の行より下）は行 −1 の列 15 の値 4 つ、上の行は 127。
- plane は macroblock の整数倍（右と下の余りも decode と filter をし、出力で切る）。
- segment は map を更新しない frame では 0。frame の loop_filter_level 0 は filter 無し。bypass_filtering。
- ended の確かめは macroblock の token ごとと header の行ごと（少なくとも）。

## 5. 変えてはいけない物

- 全ての header: `userland/desktop/libwebp-compat/*.h`、`include/libc/compat/webp/*.h`。header の comment が誤り・不足と思えば直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、結果の節に書く。
- この Phase が名指す以外の .c、`common.c`、`Makefile`・`sources.mk`・`exports.map`、`platform/`・`apps/`・`config/`。
- 試験の道具: `plan/ws344/tests/` の `*.sh`・`*.py`・`*.c`・`fixtures.list`・`fixtures.sha256`、`unit/check.h`・`unit/lbitwriter.h`・`unit/boolwriter.h`・`unit/test-helpers.c`・`unit/test-common.c`。道具の不具合（補助の header の未定義の動作など）は Q1 に返す。
- plan/ws344/design.md（判断や食い違いは結果の節に書く）、HAL・toolchain・共有の `build/`、main の checkout。
- 他の実装（libwebp・libvpx・FFmpeg など）の source を読まない・写さない。RFC 6386 §20 の参照 decoder は仕様として読んでよいが code・comment・構造・名前は写さない（自分の名前と構造で書く）。
- この Phase は vp8-*.c の誤りを見つけたら直してよい（その module の単体試験を足し、結果の節に書く）。header は変えない。

## 6. 確かめの command

```sh
cd <worktree>
sh plan/ws344/tests/run-unit.sh vp8
sh plan/ws344/tests/run-unit.sh all
sh plan/ws344/tests/make-fixtures.sh
sh plan/ws344/tests/run-fixtures.sh lossy
BASE=aa2426a5763975c358cf667aff3e17bfb841b7e2 sh plan/ws344/tests/check-skeleton.sh
sh plan/ws344/tests/target-build.sh
git diff --check
```

## 7. 終わりの条件

- 上の command が全部 PASS（最後の行を自分で読んで確かめる）。`check-skeleton` は `BASE=aa2426a5763975c358cf667aff3e17bfb841b7e2` で header と道具が変わっていないこと。
- 試験が通った後、実装をわざと数通りに壊し（git の外の複写で。例: 境界の比較を 1 つずらす、符号を 1 つ逆にする、表の値を 1 つ変える）、単体試験が FAIL することを確かめ、元に戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。
- 規約の checklist（coding-style.md §14）で自分の diff を見直した。表・定数の block ごとに出典の comment がある。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）が無い。
- commit: 新しい file は先に `git add`、`git commit -m WIP -- <作った path>...`。push しない。rm しない。
- 結果の節（下）: 日付（host の `date`）、実行した command と最後の行、件数、未実施、自分で決めた点、仕様と phase.md・header の食い違い。ws.md のこの行を「実装済み・試験 PASS（担当、日付）」に（cleared は Q1 が決める）。

## 結果

（2026-11-02 15:10、P2、q1966-i01）

- 作った物: `userland/desktop/libwebp-compat/vp8.c`（frame header → macroblock ごとに modes・residue（partition は行 % 数）・WHT・予測と IDCT → frame 全体の loop filter）、`plan/ws344/tests/unit/test-vp8.c`（80 check）、`plan/ws344/tests/conformance/`（make-conformance.sh・ivf-key-frames.py・sha256.txt、下の追加の試験の画像）。
- 確かめた事（command と最後の行、PATH=/usr/bin:$PATH）:
  - `sh plan/ws344/tests/run-unit.sh vp8` → `test-vp8: PASS (80 checks)`、`run-unit.sh all` → `run-unit: PASS`。
  - `sh plan/ws344/tests/run-fixtures.sh lossy` → `run-fixtures: PASS (lossy, 352 checks)`（176 の非可逆の fixture × filter あり・-nofilter、dwebp 1.5.0 と全 sample 一致）。
  - `WEBPC_FIXTURES=build/tmp/ws344-conformance sh plan/ws344/tests/run-fixtures.sh lossy` → `run-fixtures: PASS (lossy, 342 checks)`（libwebp-test-data の非可逆、libvpx の vp80-00-comprehensive-001〜017 の表示される key frame 29 枚、2・4・8 partition の 54 枚を含む）。同じ集合の lossless → `PASS (lossless, 43 checks)`。
  - `BASE=aa2426a5763975c358cf667aff3e17bfb841b7e2 sh plan/ws344/tests/check-skeleton.sh` → `check-skeleton: PASS`。`sh plan/ws344/tests/target-build.sh` → `target-build: PASS (build/amd64/dynamic/libwebp-compat.so, 0 warnings)`。`git diff --check` は出力無し。
  - わざと壊す確かめ（vp8.c を一時に書き換え、cmp で戻した）: 左の列の角を 127 に → unit FAIL・fixture 128 件 FAIL、右端の above-right を列 12〜15 に → fixture 50 件 FAIL（unit は通る）、全部の行を partition 0 から → unit FAIL（fixture は通った: 下の欠陥）、inner の判定を block 0 の bit に → fixture 46 件 FAIL、subblock の左の列の角を macroblock の角に → unit・fixture FAIL。
  - ASan・UBSan: test-vp8 の全ての切り詰め（ちょうどの大きさの複写）と乱数の後半、scratch の program で 335 の非可逆の file × 200 の byte の置き換え（約 67000 入力）と bitstream の切り詰め約 33600 個。報告無し（scratch の program は tree に入れていない）。
- Q1 の判断の 2 点（libwebp と fixture で照合）:
  - (a) vp8-tokens.c の `coded`: vp8.c は block ごとの bit を使わない（IDCT は WHT の後の 16 係数が全部 0 の block だけ飛ばす）。bit 31 は inner edge の filter にだけ使う（§20.16 の eob_mask の bit 31 と同じ値: どれかの block の t が 1）。上の全 fixture で libwebp と一致。
  - (b) WHT の 1 段目の 16 bit の wrap: 全 fixture と vp80 の key frame で libwebp と一致（正しい stream では値が範囲の中）。header の comment の直しは p022 へ。
- 見つけた試験の道具の欠陥（Q1 へ、凍結の道具なので直していない）: `fixtures.list` の `enc ... partitions=N` の行は全部 1 partition の file になる。libwebp の encoder は method 3 以上（既定 4）で token buffer を使い、partition を 1 にする（src/enc/webp_enc.c）。fixture-encoder に `method=0..2` を足せば直る。代わりに conformance の集合に method 0〜2 の 2・4・8 partition の 54 枚を入れ、全部 PASS。
- 追加の試験の画像（ws.md の「Q1 の判断」§11-5 の許可）: `/home/awe/zedBSD-claude1/build/sources/webp-conformance/`（git の外）に libwebp-test-data（`https://chromium.googlesource.com/webm/libwebp-test-data` の depth 1 の clone、commit 06ddd96e276c2c638a72d39d3c0f340afd61978c、131 の .webp）と vp80-00-comprehensive-001〜017.ivf。SHA-256 は `plan/ws344/tests/conformance/sha256.txt`（148 行）。`sh plan/ws344/tests/conformance/make-conformance.sh [fetch]` → `make-conformance: PASS (214 files, 214 decoded by dwebp, downloads recorded; build/tmp/ws344-conformance)`。alpha の 27 件は alpha.c（p008）が骨格のため未確かめ。compare・mutate・fuzz は decode.c（p019）の後。
- 自分で決めた点: plane は border 無しの macroblock の整数倍で、edge は毎回 struct webpc_vp8_edges に集める。filter の情報（segment・B_PRED・inner）を macroblock ごとに持ち、制御値は filter の時に webpc_vp8_filter_params で求める。first partition の ended は行ごと、token の ended は macroblock ごとに確かめる。
- 未実施: API（decode.c）経由の compare・mutate・fuzz（p019）、alpha の conformance（p008）、QEMU・実機。
