<!-- awesome-plan project=zedbsd record=ws326-p010 -->

# ws326-p010 CABAC の context の初期値の表

- 親: [WS326](../ws.md)、設計 [design.md](../design.md)。
- 状態: 実装済み・試験 PASS（2026-10-30。cleared は Q1 が受け入れを見て決める）。
- 依存: p001（依存の Phase の実装が main に入ってから始める）。
- 担当の想定: Sonnet 5.5 high の subagent を 1 回起動（ws.md のユーザーの決定 2）。header は p001 の Opus の設計で、変えない。

## 読む物

1. AGENTS.md の §4（担当の共通の規則）・§7・§9・§10、plan/agents/protocol.md
2. plan/coding-style.md の全文（ANSI C: 宣言は関数の先頭、`for` の中で宣言しない、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、など。§14 の checklist で自分の diff を見直す）
3. plan/ws326/design.md の §1（出典の規則）・§2（決定）・§4（所有と寿命）・§5（単体試験の作り方）
4. `userland/desktop/libmedia/h264sw.h`（誤りの値の約束と上限）
5. 実装する header: `userland/desktop/libmedia/h264sw-cabac-init.h`（各関数の comment が仕様。struct の field の意味と単位も header に）。使う側の header（`#include` の先）も読む。
6. 仕様: ITU-T H.264 (08/2021) の 9.3.1.1 の表 9-11（slice の種類ごとの ctxIdx と表）、表 9-12〜9-24（ctxIdx 0〜459 の m と n）。PDF `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf`（SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`、git に入れない）。印刷の頁 N = PDF の頁 N + 22。表と行列は PDF の頁の画で読む（Read の tool の `pages`、1 回 20 頁まで）。`pdftotext -layout` の文字は列が崩れることがある。

## 作る物

- `userland/desktop/libmedia/h264sw-cabac-init.c`: 骨格（各関数の `ENOSYS` など）を header の仕様どおりの実装に置き換える。static の補助関数は自由（規約どおり前方宣言）。
- 単体試験 `plan/ws326/tests/unit/test-cabac_init.c`（`check.h`・`bitwriter.h` を使う。期待の値は仕様の表か、手で計算した値。他の decoder の source・出力を写さない）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- 独立の抜き出しとの照合: `plan/ws326/tests/cabac-init-extract.py`（新しく作る、`python3 -I` で走る）が pdftotext の出力（`pdftotext -layout -f 250 -l 258 <PDF> out.txt`、表 9-12〜9-24 は印刷 228〜236 頁 = PDF 250〜258 頁）から ctxIdx ごとの (m, n) を読み、C の表を出す小さな program（test-cabac_init.c が `--dump` で全 1840 組を印字）の出力と全組で比べる。抜き出しが曖昧な行（列の崩れ）は PDF の頁の画で確かめ、script の中に例外として理由とともに書く。
- 形: 表 9-12・9-17 の範囲（0〜10、60〜69）は 4 組が同じ、I の 11〜59 と 276 は (0, 0)、全値が -128〜127。
- 抜き取り: 各表の最初と最後の ctxIdx の 4 組を PDF の画から手で test に書き、一致。
- 共通: 誤りの入力（範囲外・途中で切れた・巨大な値）で落ちず、header に書かれた誤りを返す。ASan・UBSan の報告が 0。

## 注意

- 1840 組は量が多いので、1 表ずつ入れて 1 表ずつ照合する。block は表ごとに分け、各 block の前に `Values referenced: ITU-T H.264 (08/2021) Table 9-NN` の複数行の comment。照合の記録（抜き出しの例外、目で見た頁）を phase.md の結果に残す。

## 変えてはいけない物

- `userland/desktop/libmedia/h264sw*.h`（全部）。header の不備・矛盾・足りない口を見つけたら、直さずに止めて Q1 に返す: どの header のどの関数か、何が足りないか、案の差分。
- 担当の module 以外の `h264sw-*.c`（他の Phase の物）。既存の libmedia の file（bits.c・h264.c・vkvideo.c・decoder.c など）、Makefile、`plan/ws326/tests/run-unit.sh`・`check.h`・`bitwriter.h`・`check-skeleton.sh`（不備は Q1 へ）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない。
- 上の「注意」がこの Phase に明示に許す物だけは、この節の限りでない。

## 確かめの command（worktree の root で）

- `timeout 600 sh plan/ws326/tests/run-unit.sh cabac_init` → 最後の行 `run-unit: PASS`。前の Phase の試験も `sh plan/ws326/tests/run-unit.sh all` で PASS のまま。
- `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`（header が p001 の物から変わっていない、cc と clang で warning 0）。
- zedBSD amd64: `timeout 1200 make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libmedia.so` → exit 0、出力に warning が 0。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 自分の diff を coding-style の §14 の checklist で見直した。表の block ごとに `Values referenced: ITU-T H.264 (08/2021) Table ...` の複数行の comment がある。source の comment・log に WS・Phase の番号や plan/ への参照が無い。
- `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と結果・未実施・残りを Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、実行した command、PASS の数、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が受け入れを見て決める）。

## 結果

2026-10-30、worktree `/home/awe/zedBSD-worktrees/s-ws326-p010`（branch `agent/s-ws326-p010`）。

- 実装: `h264sw-cabac-init.c` の `h264sw_cabac_init_mn`（460 x 4 x 2）に表 9-12〜9-24 の値を、表ごとの block（各 block の前に `Values referenced: ITU-T H.264 (08/2021) Table 9-NN` の comment）で入れた。header は変えていない。この module には関数が無いので「誤りの入力」の試験は対象が無い（表の形と値だけを確かめた）。
- 値の取り方: 表 9-18〜9-24（行の表、386 件）は `pdftotext -bbox`（語の座標）から scratch の生成 script で取り、表 9-12・9-13・9-15・9-16・9-17 は PDF の画と文字を見て手で入れ、表 9-14 は画から手で入れた。生成 script は commit していない（`build/tmp/p010/gen/`）。
- 独立の照合: `plan/ws326/tests/cabac-init-extract.py`（`pdftotext -layout -f 250 -l 258` を読む。生成と別の方法）。`test-cabac_init --dump`（1840 行）との比較: `cabac-init-extract: PASS (1840 pairs)`。値を 1 つ変えた dump（ctxIdx 5・30・75・399・459）は全て FAIL になることを確かめた。
- 抜き出しの例外: 表 9-14（ctxIdx 24〜39）の m は桁が 2 行に割れた cell（例 30 の `−4` と `6` で -46）があり pdftotext が繋げないため、m は PDF 251 頁の画から手で読んだ値を script の中に理由とともに書いた（n の行は文字から読む）。したがって表 9-14 の m は文字からの独立の照合が無く、画の目視と test の抜き取り（ctxIdx 24・30・31・39）が支え。
- 目で見た頁: PDF 251（表 9-14・9-15・9-16）、252（表 9-17・9-18 の全行）、258（表 9-24 の全行）。他の頁は文字の抜き出しと、各表の最初と最後の ctxIdx の手の抜き取りによる。
- 試験: `timeout 600 sh plan/ws326/tests/run-unit.sh cabac_init` → `test-cabac_init: PASS (4159 checks)`、`run-unit: PASS`（ASan・UBSan 付き、報告 0）。`run-unit.sh all` → test-bitwriter PASS (14 checks)・test-cabac_init PASS・`run-unit: PASS`。
- `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`。`git diff --check` → 出力なし。
- amd64 の `build/amd64/dynamic/libmedia.so`: make の exit 0、出力の warning 0（`-Wall -Wextra -Werror` 付き）。
- 未実施: 実行時の QEMU・実機の試験（この Phase の範囲外）。表 9-14 の m の、文字からの独立の照合。
