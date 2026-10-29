<!-- awesome-plan project=zedbsd record=ws326-p020 -->

# ws326-p020 deblocking

- 親: [WS326](../ws.md)、設計 [design.md](../design.md)。
- 状態: cleared（2026-10-30 Q1 の判定。実装と試験は Sonnet 5.5 high、139d91884）。
- 依存: p003、p008（依存の Phase の実装が main に入ってから始める）。
- 担当の想定: Sonnet 5.5 high の subagent を 1 回起動（ws.md のユーザーの決定 2）。header は p001 の Opus の設計で、変えない。

## 読む物

1. AGENTS.md の §4（担当の共通の規則）・§7・§9・§10、plan/agents/protocol.md
2. plan/coding-style.md の全文（ANSI C: 宣言は関数の先頭、`for` の中で宣言しない、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、など。§14 の checklist で自分の diff を見直す）
3. plan/ws326/design.md の §1（出典の規則）・§2（決定）・§4（所有と寿命）・§5（単体試験の作り方）
4. `userland/desktop/libmedia/h264sw.h`（誤りの値の約束と上限）
5. 実装する header: `userland/desktop/libmedia/h264sw-deblock.h`（各関数の comment が仕様。struct の field の意味と単位も header に）。使う側の header（`#include` の先）も読む。
6. 仕様: ITU-T H.264 (08/2021) の 8.7・8.7.1・8.7.2・8.7.2.1〜8.7.2.4（式 8-453 以下、表 8-16・8-17）。PDF `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf`（SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`、git に入れない）。印刷の頁 N = PDF の頁 N + 22。表と行列は PDF の頁の画で読む（Read の tool の `pages`、1 回 20 頁まで）。`pdftotext -layout` の文字は列が崩れることがある。

## 作る物

- `userland/desktop/libmedia/h264sw-deblock.c`: 骨格（各関数の `ENOSYS` など）を header の仕様どおりの実装に置き換える。static の補助関数は自由（規約どおり前方宣言）。
- 単体試験 `plan/ws326/tests/unit/test-deblock.c`（`check.h`・`bitwriter.h` を使う。期待の値は仕様の表か、手で計算した値。他の decoder の source・出力を写さない）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- luma の edge: bS 1・2・3（tC・ap・aq、式 8-463 以下）と bS 4（強い filter と弱い filter の分かれ目）の 1 行ずつ手で計算した 6 sample、alpha・beta の閾値の直下と直上、indexA の clip（offset ±12）。
- chroma の edge: bS < 4 で p0・q0 だけ、bS 4 の 3 tap。
- bS の導出: intra の macroblock 境界 4 と内部 3、非 0 係数 2、参照の frame の違い・mv の差 4 の境（3 と 4）・双予測の組み合わせ 1/0。
- picture: disable_deblocking_filter_idc 1（何もしない）と 2（slice の境を越えない）、transform_size_8x8 の内部の edge 1・3 を飛ばす、chroma の edge 4 は常に。
- 表 8-16・8-17 の値は PDF の画から入れ、indexA 0・15・16・51 などの行を照合した記録を phase.md に。
- 共通: 誤りの入力（範囲外・途中で切れた・巨大な値）で落ちず、header に書かれた誤りを返す。ASan・UBSan の報告が 0。

## 変えてはいけない物

- `userland/desktop/libmedia/h264sw*.h`（全部）。header の不備・矛盾・足りない口を見つけたら、直さずに止めて Q1 に返す: どの header のどの関数か、何が足りないか、案の差分。
- 担当の module 以外の `h264sw-*.c`（他の Phase の物）。既存の libmedia の file（bits.c・h264.c・vkvideo.c・decoder.c など）、Makefile、`plan/ws326/tests/run-unit.sh`・`check.h`・`bitwriter.h`・`check-skeleton.sh`（不備は Q1 へ）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない。
- 上の「注意」がこの Phase に明示に許す物だけは、この節の限りでない。

## 確かめの command（worktree の root で）

- `timeout 600 sh plan/ws326/tests/run-unit.sh deblock` → 最後の行 `run-unit: PASS`。前の Phase の試験も `sh plan/ws326/tests/run-unit.sh all` で PASS のまま。
- `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`（header が p001 の物から変わっていない、cc と clang で warning 0）。
- zedBSD amd64: `timeout 1200 make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libmedia.so` → exit 0、出力に warning が 0。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 自分の diff を coding-style の §14 の checklist で見直した。表の block ごとに `Values referenced: ITU-T H.264 (08/2021) Table ...` の複数行の comment がある。source の comment・log に WS・Phase の番号や plan/ への参照が無い。
- `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と結果・未実施・残りを Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、実行した command、PASS の数、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が受け入れを見て決める）。

## 結果

- 日付: 2026-10-30（host の `date`）。状態: 実装済み・試験 PASS（cleared は Q1 が受け入れを見て決める）。
- 作った物: `userland/desktop/libmedia/h264sw-deblock.c`（4 つの公開関数と static の補助。header は変えていない）、`plan/ws326/tests/unit/test-deblock.c`。
- 実行した command と結果:
  - `timeout 600 sh plan/ws326/tests/run-unit.sh deblock` → `test-deblock: PASS (3313 checks)`、`run-unit: PASS`（ASan・UBSan 黙る）。
  - `timeout 900 sh plan/ws326/tests/run-unit.sh all` → 全 12 試験 PASS、最後の行 `run-unit: PASS`。
  - `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`。
  - zedBSD amd64 の `make ... build/amd64/dynamic/libmedia.so` → exit 0、出力の `warning` は 0 件（h264sw-deblock.o が link されたことを確認）。
  - `git diff --check` → 問題なし。
- 表 8-16・8-17 の照合の記録: PDF（SHA-256 d6e09bcc…）の PDF 頁 228・229（印刷 206・207 頁）を 110 dpi の画で読み、`pdftotext -layout` の文字と全 52 列（indexA 0..51）が一致することを確かめた（alpha'、beta'、tC0' の bS 1・2・3）。
  .c の表は 13 個ずつ 4 行（indexA 0..12・13..25・26..38・39..51）、試験の `spec_alpha`・`spec_beta`・`spec_tc0` は仕様の頁の分け方（0..25・26..51）で別に打ち直した。
  indexA 0・15・16・17・23・25・26・51 などの境の値は、試験が全 index の掃引（alpha: step = alpha' - 1 は filter され alpha' は filter されない、beta: 差 beta' - 1 と beta'、tC0: tC が tC0 になる標本で p0 の動きが tC0 と等しい）で filter の結果として確かめる。
- 試験の中身: 表の掃引（52 index × alpha・beta の両側・tC0 の bS 1〜3）、luma bS 1〜3 の手計算 5 例（両側が flat・鏡像・p 側だけ flat でない・q 側だけ flat でない・負の奇数の >> 1 が負の無限大向き）と bS 4 の手計算 5 例（両側が強い filter・p 側だけ弱い・q 側だけ弱い・step 14 は弱い・step 13 は強い）と alpha 直下・直上、
  chroma の bS 1〜4（p0・q0 だけ、bS 4 は flat でも 3 tap、2 行ごとの bS、横の edge）、indexA・indexB の clip（offset ±12、INT_MAX・INT_MIN）、segment ごとの bS と横の edge、
  bS の導出（intra の macroblock 境界 4 と内部 3、picture の左・上の境、concealment の kind だけの intra、PCM、非 0 係数の p 側・q 側・縦横・8x8 の 4 bit、参照 frame の違い・ref_idx だけの違い・list 1 経由、mv 差 3 と 4（水平・垂直・正負）、mv の数の違い、双予測の list の順の違い、同じ frame 2 回の straight と cross、内部の edge、誤りの入力）、
  picture（idc 1、idc 0 の強い filter の手計算、idc 2 の slice の境と同じ slice、q0 の slice の offset、上の境の idc 2、横・縦の ramp の edge の順の手計算と縦の後に横の 2 次元の例、transform_size_8x8 の edge 1・3 の飛ばしと chroma edge 4、Cb・Cr の別の offset、qPav の +1 の丸め、上の macroblock の qp、I_PCM の qp、inter の segment ごとの bS、chroma が luma の edge 2 の bS を使う事、誤りの picture）。
- 突然変異の確認（commit 後ではなく作業の file を直接書き換え、戻して diff が 0 を確認）: 表の 3 つの値の書き換え、qPav の +1 の欠け、PCM の qp、縦横の順の入れ替え、8x8 の飛ばしの欠け、idc 2 の欠け、強い filter の閾値の `<` を `<=`、tC の ap・aq 項の欠け、chroma の tC + 1 の欠け、mv の限度 4 を 5、内部の intra の bS 4、chroma が luma の qp を使う、q0 でない slice の offset、負の値の >> の切り捨て、参照の並びの対応、同じ frame 2 回の && を ||、Cr に Cb の offset、chroma edge 4 を 8x8 で飛ばす、chroma が luma の edge 1 の bS、chroma の行の対応、nonzero の p 側の無視、chroma の上の edge が左の macroblock の qp を使う、の全てを試験が検出した。
  最初に生き残った 2 件（chroma が luma の edge 1 の bS を使う、chroma の上の edge の p 側 macroblock）は試験を足して検出させた。残る生き残りは「bS が 4 を超える時の clamp」の 1 件で、clamp が無くても bS 4 の分岐に入るため結果が同じ（等価）。
- 判断（header が書かない所）:
  - header の公開関数は `void` なので、誤りの入力は「何もしない」か「bs を 0 にする」で扱う。`h264sw_deblock_picture` は picture・frame・planes・mbs・slices が無い、grid が 0 か `size_mbs` と合わない、frame か stride が grid より小さい、slice_count が slice_capacity より大きい時、planes を触らず戻る。
    `h264sw_deblock_bs` は存在しない edge（picture の左・上の境、macroblock address か edge が範囲外）で bs の 4 つを 0 にして戻り、picture か mbs が NULL でも bs が有れば 0 にする。edge の関数は q0 か bs が NULL なら戻る。
  - slice の番号が -1 か slice_count 以上の macroblock は filter せず、その macroblock と接する edge（左か上が未 decode か slice 不明）も filter しない（実 decode の経路では concealment が全 macroblock に slice を与えるので起きない）。
  - intra の判定は `kind`（I_NXN・I_4X4・I_8X8・I_16X16・I_PCM）か `motion.intra` のどちらかが intra なら intra（header が「concealment は kind だけを与える」「parser は motion.intra を書く」と書くため）。
  - 強度（bs の値）が 4 を超える時は 4 とみなす（tC0 の表の添字に使わない）。qPav・offset は int64_t で足してから clip し、どの int でも溢れない。
  - 負の値の右 shift（8-467 の >> 3、8-470・8-472 の >> 1）は実装定義なので、`shift_right` を算術で書いた（負は -((-v - 1) >> n) - 1、負の無限大向きの丸め、h264sw.h の算術の規則）。
  - 色の qp は header の通り picture の `chroma_qp_offset[0]`（Cb）・`[1]`（Cr）を使い、全 macroblock で共通（slice ごとに PPS が変わる場合の個別の offset は context が持たない）。I_PCM は QPY 0 に対する QPC（Table 8-15）。
  - bS の motion は `ref_idx[list][quadrant] >= 0` で使用中を決め、参照 frame は `ref_id`、mv は 4x4 block の `mv[list][blk]`、quadrant は blk / 4。参照の数が 0 同士（intra でも skip でもない壊れた状態）は 0。
  - chroma の edge は luma の edge 0 と 2 の bS を使う（edge 4 は luma の edge 8 に対応、8.7.2 の「luma sample (2x, 2y)」）。Cb・Cr は仕様の順（成分ごとに縦の後に横）で filter する。
  - `h264sw_deblock_bs` は SP・SI の規則を適用しない（decoder が拒む）。MBAFF・field の規則は範囲外。
- 仕様と phase.md の食い違い: 無し。
- 試験の書き方: 新しい試験の loop は既存の試験（test-intra・test-mb）と同じく、補助の中の単純な loop に目的の comment を付けていない所がある（source の .c は規約の全文に沿わせた）。
- 未実施: 実機・QEMU の試験は対象外（host の単体試験のみ）。実 stream との比較（conformance）は decoder の Phase 以降。
