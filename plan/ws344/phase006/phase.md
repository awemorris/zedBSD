<!-- awesome-plan project=zedbsd record=ws344-p006 -->

# ws344-p006 VP8L の逆変換（vp8l-transform.c）

- Status: 実装済み・試験 PASS（Sonnet 5.5 q1956-i01、2026-11-02。cleared は Q1）
- 親: [ws.md](../ws.md)、設計: [design.md](../design.md)。担当の model: Sonnet 5.5 high（phase-runner-low）。依存: p001。
- 目的: predictor・color・subtract green・color indexing の逆変換。
- 基点の SHA（header と道具の凍結）: `aa2426a5763975c358cf667aff3e17bfb841b7e2`。

## 1. 読む物

- AGENTS.md §4（担当の共通の規則）・§7（禁止）・§9（検証）・§10（コード）、plan/agents/protocol.md、docs/agent/spec-module-delegation.md §5.3・§7（出典と license）。
- plan/coding-style.md の全文（ANSI C、宣言は関数の先頭、段落ごとの comment、if・loop・return の comment、Review checklist）。
- plan/guardrail.md の「MIT・GPL の出典のデータの引用」（block ごとの複数行の comment「Values referenced: <仕様と版> <節・表>」「Procedure referenced: ...」、file の先頭に書かない）。
- plan/ws344/design.md の §1（出典と RFC の既知の誤り）・§4（決定）・§7（上限と信用できない入力）・§8（試験の道具）と、§5 のこの module の行。
- `userland/desktop/libwebp-compat/common.h`（誤りの値・算術の規則。全 module の約束）。
- 仕様（git の外、著作物）: RFC 9649 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc9649.txt`（SHA-256 `f4d7cd92b015c0b340f10a932bb03e564a758a17df50cbb992c16d8459d60e58`）、RFC 6386 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc6386.txt`（SHA-256 `c044997ae5e26f74dc28569f8866937412b2783e887c96e0c09372af78be411c`）、RFC 1951 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc1951.txt`（SHA-256 `5ebf4b5b7fe1c3a0c0ab9aa3ac8c0f3853a7dc484905e76e03b0b0f301350009`）。text なので節は `sed -n '/^13\.3\./,/^13\.4\./p' <file>` のように読む（頁の区切りの行 `Bankoski, et al.`・`RFC 6386 ...` は無視）。
- `vp8l-transform.h`（仕様）。
- RFC 9649 §3.5、§3.5.1（Table 2、Figure 19、境界の規則）、§3.5.2、§3.5.3、§3.5.4（Table 3、bundling）。

## 2. 作る物

- `vp8l-transform.c`、`unit/test-vp8l_transform.c`。
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 試験の項目

- subsample: (1,0)、(16,2)、(17,2)、(16384,9)。
- predictor: 14 の mode と 14・15 を、3x3 と 5x4 の画像で手計算の期待値と比べる（mode の画像を 1 block と 2x2 block で）。境界: (0,0) は 0xff000000、上の行は L、左の列は T、右端の列の TR は今の行の左端の画素。Average2 は channel ごと。Select の pL == pT は T。ClampAddSubtractHalf の (a - b) が負で 0 への切り捨てと floor が違う例（a = 10, b = 13 → 9）。
- color: t・c が負（0x80・0xff）の delta、red_to_blue が新しい red を使うこと、mod 256。
- add_green: 繰り上がり（red 200 + green 100 → 44）。
- indexing: width_bits 0〜3 で bit の位置（下位から）、幅が pack の倍数でない（幅 5 を 8 個ずつ）、table の範囲の外の index → 0x00000000、packed と out が同じ buffer。
- 往復: test の中に仕様の正方向（ColorTransform、predictor の残差 = 実際 − 予測）を書き、乱数の画像を変換してから逆変換で戻す（各変換、size_bits 2〜5）。
- 全ての単体試験は ASan・UBSan で落ちず（run-unit.sh）、途中で切れた入力・範囲の外の値・乱数の入力で決められた誤りを返す。
- 期待の値は仕様の表・式と手の計算、test の側の逆の処理（lbitwriter・boolwriter で組んだ入力）だけから作る。他の実装の出力を期待の値にしない（fixture の全体の試験は道具が libwebp の出力と比べる）。

## 4. 注意

- mode は green の下位 4 bit（14・15 は mode 0）。
- 予測は復元済みの画素から（in place、scan 順）。
- ClampAddSubtractHalf の除算は C の「/」（0 へ切り捨て）。Average2 は非負なので floor と同じ。

## 5. 変えてはいけない物

- 全ての header: `userland/desktop/libwebp-compat/*.h`、`include/libc/compat/webp/*.h`。header の comment が誤り・不足と思えば直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、結果の節に書く。
- この Phase が名指す以外の .c、`common.c`、`Makefile`・`sources.mk`・`exports.map`、`platform/`・`apps/`・`config/`。
- 試験の道具: `plan/ws344/tests/` の `*.sh`・`*.py`・`*.c`・`fixtures.list`・`fixtures.sha256`、`unit/check.h`・`unit/lbitwriter.h`・`unit/boolwriter.h`・`unit/test-helpers.c`・`unit/test-common.c`。道具の不具合（補助の header の未定義の動作など）は Q1 に返す。
- plan/ws344/design.md（判断や食い違いは結果の節に書く）、HAL・toolchain・共有の `build/`、main の checkout。
- 他の実装（libwebp・libvpx・FFmpeg など）の source を読まない・写さない。RFC 6386 §20 の参照 decoder は仕様として読んでよいが code・comment・構造・名前は写さない（自分の名前と構造で書く）。

## 6. 確かめの command

```sh
cd <worktree>
sh plan/ws344/tests/run-unit.sh vp8l_transform
sh plan/ws344/tests/run-unit.sh all
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

- 日付: 2026-11-02 14:12（host の `date`）。担当: Sonnet 5.5（q1956-i01）。実装の commit: 5659aedbbe99c89fe5ce5dbde127ca05a652b616。
- 作った物: `userland/desktop/libwebp-compat/vp8l-transform.c`（5 つの公開関数と static の補助）、`plan/ws344/tests/unit/test-vp8l_transform.c`。header・試験の道具・他の module は変えていない。
- 確かめ（全部 worktree で実行、最後の行を読んだ）:
  - `sh plan/ws344/tests/run-unit.sh vp8l_transform` → `test-vp8l_transform: PASS (457604 checks)`、`run-unit: PASS`。
  - `sh plan/ws344/tests/run-unit.sh all` → 11 件全部 PASS、最後の行 `run-unit: PASS`。
  - `BASE=aa2426a5763975c358cf667aff3e17bfb841b7e2 sh plan/ws344/tests/check-skeleton.sh` → `check-skeleton: PASS`。
  - `sh plan/ws344/tests/target-build.sh` → `target-build: PASS (build/amd64/dynamic/libwebp-compat.so, 0 warnings)`。
  - `git diff --check` → 出力なし（終了 0）。
- 単体試験の中身: subsample（(1,0)、(16,2)、(17,2)、(16384,9) ほか）。predictor は 3x3 で mode 0〜15 を (1,1) と rightmost の (2,1) について手計算の値と比べ（mode 7・11・12・13 は試験の comment に手の計算を書いた）、mode は green の下位 4 bit（0x12 → 2、0xf1 → 1、0x1e → 14 → 黒）、境界（(0,0) = 0xff000000 + residual の alpha の繰り上がり、上の行は L、左の列は T、右端の TR は今の行の左端）、1 画素幅・1 行、5x5（2x2 block、mode 3・2・1・3）と 5x4（2x1 block）で block ごとの mode、Average2 の floor と channel の混ざらなさ、Select（L・T・tie → T・alpha が効く）、ClampAddSubtractFull/Half の clamp と、(a - b) が負で奇数（a=10, b=13 → 9）。color は t・c が負（0xff・0x80）の delta、floor（t=-1, c=1）、-508 → 4、512 → 0、mod 256、red_to_blue が新しい red を使うこと、alpha と green が変わらないこと。add_green は繰り上がり（red 200 + green 100 → 44）。indexing は width_bits 0〜3 の bit の位置、幅 5 と 9（8 個ずつ）、2 行、table の範囲の外 → 0、alpha・red・blue を見ないこと、packed と out が同じ buffer（乱数 200 通りで別 buffer と一致）。往復は test の中の正方向（predictor の残差、ColorTransform、green の引き算、index の pack）で乱数 400 seed × 各変換（size_bits 2〜5）。ASan・UBSan 下、画像の buffer は大きさちょうどの malloc。乱数の入力（size_bits 2〜9）で全関数を流し範囲の外を見る。
- わざと壊す確かめ（git の外、`build/tmp/mut-p006/` の複写で 73 通り: 境界の比較・符号・mode ごとの式・表の値・shift・stride・loop の始点・channel の抜け・clamp・mask）: 70 通りが FAIL、生き残り 3 通りは全て等価（`block row stride` と `color element row` は式を恒等に書き換えただけの no-op、`average2 mask` は `(a & 0xfefefefe) >> 1` と `(a >> 1) & 0x7f7f7f7f` が同じ値）。見逃しは無く、試験は足していない。複写は変更前の source から作り、元の file は変わっていない（`git status` が clean）。
- 自分で決めた点（header が黙っている所）:
  - size_bits・width_bits は header の範囲（2..9、0..3）を前提とし、範囲の外の値を検査しない（関数は void で誤りを返せず、呼び手の vp8l.c が読んだ値を範囲で検査する）。範囲の外では shift が未定義になる。
  - width または height が 0 の呼び出しは何もしない。
  - ClampAddSubtractHalf の負の差の除算は、実装定義の負の整数の除算に頼らず、大きさを 2 で割って符号を戻す（0 への切り捨て）。
  - Average2 は 4 channel を 1 語で floor の平均（(a & ~1)/2 + (b & ~1)/2 + (a & b & 1)）、足し算は 16 bit の lane（色の加算の繰り上がりが隣へ行かない）。
  - indexing は in place かどうかに関わらず常に最後の画素から前へ展開する（別 buffer でも結果は同じ）。table の index は 8 bit 以内に mask され、table は 256 entry なので範囲の外を読まない。
- 仕様と phase.md・header の食い違い: 無い。phase.md の「Average2 は非負なので floor と同じ」「mode は green の下位 4 bit」は仕様と合う。header の Select の記述（pL < pT で L、そうでなければ T）は RFC 9649 §3.5.1 のとおりに実装した。
- 未実施: fixture の全体の試験（`run-fixtures.sh lossless` は p007 の後）、QEMU・実機（この Phase の範囲外）。
- Q1 が消す物（path）: `/home/awe/zedBSD-worktrees/s-ws344-p006/build/tmp/mut-p006/`（わざと壊した複写と script）、`/home/awe/zedBSD-worktrees/s-ws344-p006/build/tmp/calc/`（手計算の確認用 script）、`/home/awe/zedBSD-worktrees/s-ws344-p006/build/tmp/ws344-unit*`（run-unit の出力）。
