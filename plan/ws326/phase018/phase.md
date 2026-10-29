<!-- awesome-plan project=zedbsd record=ws326-p018 -->

# ws326-p018 slice data と macroblock の syntax

- 親: [WS326](../ws.md)、設計 [design.md](../design.md)。
- 状態: planned（2026-10-30 p001 で作成）。
- 依存: p002、p008、p009、p012（依存の Phase の実装が main に入ってから始める）。
- 担当の想定: Sonnet 5.5 high の subagent を 1 回起動（ws.md のユーザーの決定 2）。header は p001 の Opus の設計で、変えない。

## 読む物

1. AGENTS.md の §4（担当の共通の規則）・§7・§9・§10、plan/agents/protocol.md
2. plan/coding-style.md の全文（ANSI C: 宣言は関数の先頭、`for` の中で宣言しない、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、など。§14 の checklist で自分の diff を見直す）
3. plan/ws326/design.md の §1（出典の規則）・§2（決定）・§4（所有と寿命）・§5（単体試験の作り方）
4. `userland/desktop/libmedia/h264sw.h`（誤りの値の約束と上限）
5. 実装する header: `userland/desktop/libmedia/h264sw-macroblock.h`（各関数の comment が仕様。struct の field の意味と単位も header に）。使う側の header（`#include` の先）も読む。
6. 仕様: ITU-T H.264 (08/2021) の 7.3.4・7.4.4、7.3.5・7.3.5.1・7.3.5.2・7.3.5.3・7.3.5.3.1・7.4.5〜7.4.5.3.3（式 7-36・7-37）。PDF `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf`（SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`、git に入れない）。印刷の頁 N = PDF の頁 N + 22。表と行列は PDF の頁の画で読む（Read の tool の `pages`、1 回 20 頁まで）。`pdftotext -layout` の文字は列が崩れることがある。

## 作る物

- `userland/desktop/libmedia/h264sw-macroblock.c`: 骨格（各関数の `ENOSYS` など）を header の仕様どおりの実装に置き換える。static の補助関数は自由（規約どおり前方宣言）。
- 単体試験 `plan/ws326/tests/unit/test-macroblock.c`（`check.h`・`bitwriter.h` を使う。期待の値は仕様の表か、手で計算した値。他の decoder の source・出力を写さない）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- CAVLC: bitwriter で組んだ I slice（I_4x4・I_16x16・I_PCM の各 1 macroblock）、P slice（mb_skip_run、P_L0_16x16、P_8x8 の sub 4 種、P_8x8ref0）、B slice（B_Direct_16x16、B_8x8 の direct sub）を h264sw_macroblock_parse で読み、ctx->syntax と mb の field が期待どおり。
- transform_size_8x8_flag の読む条件（I_NxN、inter の cbp の後、direct_8x8_inference の条件）と CAVLC の 8x8 の 4 つの 4x4 の交互の並び（7.3.5.3.2 の lumaList）。
- CABAC: macroblock 全体の符号化を test に書くのは大きすぎるので、ここでは I_PCM の macroblock 1 つと end_of_slice_flag だけを `cabacwriter.h` で組んで読む。CABAC の macroblock の syntax の全体は p021 の stream の試験（CABAC の stream）と p022 の適合で確かめる。
- QP: mb_qp_delta の wrap（51 + 1 → 0、0 - 1 → 51）、範囲外 26 は EINVAL。I_PCM の qp 0。
- h264sw_slice_data は recon が骨格（ENOSYS）の間は p019 の後に全体で試す。この Phase では parse だけ（slice_data の loop は recon を stub にできないので、p018 の試験に回す）。
- 共通: 誤りの入力（範囲外・途中で切れた・巨大な値）で落ちず、header に書かれた誤りを返す。ASan・UBSan の報告が 0。

## 注意

- h264sw_slice_data は h264sw_recon_mb を呼ぶ。recon が未実装の間は ENOSYS で止まるので、loop の試験は p019 の受け入れに含める。

## 変えてはいけない物

- `userland/desktop/libmedia/h264sw*.h`（全部）。header の不備・矛盾・足りない口を見つけたら、直さずに止めて Q1 に返す: どの header のどの関数か、何が足りないか、案の差分。
- 担当の module 以外の `h264sw-*.c`（他の Phase の物）。既存の libmedia の file（bits.c・h264.c・vkvideo.c・decoder.c など）、Makefile、`plan/ws326/tests/run-unit.sh`・`check.h`・`bitwriter.h`・`check-skeleton.sh`（不備は Q1 へ）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない。
- 上の「注意」がこの Phase に明示に許す物だけは、この節の限りでない。

## 確かめの command（worktree の root で）

- `timeout 600 sh plan/ws326/tests/run-unit.sh macroblock` → 最後の行 `run-unit: PASS`。前の Phase の試験も `sh plan/ws326/tests/run-unit.sh all` で PASS のまま。
- `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`（header が p001 の物から変わっていない、cc と clang で warning 0）。
- zedBSD amd64: `timeout 1200 make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libmedia.so` → exit 0、出力に warning が 0。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 自分の diff を coding-style の §14 の checklist で見直した。表の block ごとに `Values referenced: ITU-T H.264 (08/2021) Table ...` の複数行の comment がある。source の comment・log に WS・Phase の番号や plan/ への参照が無い。
- `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と結果・未実施・残りを Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、実行した command、PASS の数、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が受け入れを見て決める）。

## p012 から引き継ぐ事（2026-10-30 Q1、p012 の実装の担当の報告）

CABAC の syntax element の文脈（ctxIdxInc）は今の macroblock の状態を読むので、macroblock の解析はそれぞれを、それを使う次の要素を読む前に書く:
`kind`、`coded_flags`（16 = luma DC、17・18 = Cb・Cr の DC、`19 + 4*(component-1) + chroma4x4BlkIdx` = chroma AC）、`mvd`、`motion.ref_idx[list][quadrant]`、`direct`（ref_idx と mvd の前）、I_16X16 の `cbp`（mb_type から）。
