<!-- awesome-plan project=zedbsd record=ws326-p009 -->

# ws326-p009 CAVLC

- 親: [WS326](../ws.md)、設計 [design.md](../design.md)。
- 状態: 実装済み・試験 PASS（2026-10-30、cleared は Q1 の受け入れ待ち）。
- 依存: p002、p008（依存の Phase の実装が main に入ってから始める）。
- 担当の想定: Sonnet 5.5 high の subagent を 1 回起動（ws.md のユーザーの決定 2）。header は p001 の Opus の設計で、変えない。

## 読む物

1. AGENTS.md の §4（担当の共通の規則）・§7・§9・§10、plan/agents/protocol.md
2. plan/coding-style.md の全文（ANSI C: 宣言は関数の先頭、`for` の中で宣言しない、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、など。§14 の checklist で自分の diff を見直す）
3. plan/ws326/design.md の §1（出典の規則）・§2（決定）・§4（所有と寿命）・§5（単体試験の作り方）
4. `userland/desktop/libmedia/h264sw.h`（誤りの値の約束と上限）
5. 実装する header: `userland/desktop/libmedia/h264sw-cavlc.h`（各関数の comment が仕様。struct の field の意味と単位も header に）。使う側の header（`#include` の先）も読む。
6. 仕様: ITU-T H.264 (08/2021) の 7.3.5.3.2、9.2・9.2.1（表 9-5）・9.2.2・9.2.2.1（表 9-6）・9.2.3（表 9-7〜9-10）・9.2.4、9.1.2（表 9-4）。PDF `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf`（SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`、git に入れない）。印刷の頁 N = PDF の頁 N + 22。表と行列は PDF の頁の画で読む（Read の tool の `pages`、1 回 20 頁まで）。`pdftotext -layout` の文字は列が崩れることがある。

## 作る物

- `userland/desktop/libmedia/h264sw-cavlc.c`: 骨格（各関数の `ENOSYS` など）を header の仕様どおりの実装に置き換える。static の補助関数は自由（規約どおり前方宣言）。
- 単体試験 `plan/ws326/tests/unit/test-cavlc.c`（`check.h`・`bitwriter.h` を使う。期待の値は仕様の表か、手で計算した値。他の decoder の source・出力を写さない）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- coeff_token: 表 9-5 の 5 列（0 <= nC < 2、2 <= nC < 4、4 <= nC < 8、8 <= nC、nC = -1）の各々から 10 個以上の符号語（最短と最長、TrailingOnes 0〜3、TotalCoeff の小と大を含む）を PDF の表から手で選んで bitwriter で書き、TotalCoeff・TrailingOnes が表どおり。実装の表を test に写して比べることはしない（同じ写し間違いが通る）。
- level: suffixLength 0 からの増え方、level_prefix 14・15（escape）・16 以上、TrailingOnes < 3 の +2、手で計算した block 3 つ（係数の列 → bit 列は 9.2 を逆に追って作る）。
- total_zeros（表 9-7・9-8・9-9 a）と run_before（表 9-10）、zerosLeft が 0 で止まる、maxNumCoeff 15（AC）と 4（chroma DC）の配置。
- nC: 隣が無い・片方・両方・skip・I_PCM（16）。cbp: 表 9-4 の codeNum 0〜47 の intra と inter の全部、48 は EINVAL。
- 誤り: 表に無い符号、TotalCoeff が maxNumCoeff を超える、run が start より前、終わりで切れた入力。
- 表の形（写し間違いを捕まえる、実装の表を読む小さな検査を test に書く）: 表 9-5 の各列、表 9-7・9-8 の各 tzVlcIndex、表 9-9 a の各 tzVlcIndex、表 9-10 の各 zerosLeft の符号語の集合が prefix-free で Kraft の和 sum 2^-len が 1（9-5 の nC >= 8 の固定長の列も 1）。符号語の数が行の数どおり（9-5 は列ごとに 62、ただし nC = -1 は 14）。
- 共通: 誤りの入力（範囲外・途中で切れた・巨大な値）で落ちず、header に書かれた誤りを返す。ASan・UBSan の報告が 0。

## 注意

- 表 9-5 の符号語は長さと値の組で持つ（peek して長さで飛ばす方式を推す、h264sw_bits_peek）。block ごとに `Values referenced: ITU-T H.264 (08/2021) Table 9-5`。

## 変えてはいけない物

- `userland/desktop/libmedia/h264sw*.h`（全部）。header の不備・矛盾・足りない口を見つけたら、直さずに止めて Q1 に返す: どの header のどの関数か、何が足りないか、案の差分。
- 担当の module 以外の `h264sw-*.c`（他の Phase の物）。既存の libmedia の file（bits.c・h264.c・vkvideo.c・decoder.c など）、Makefile、`plan/ws326/tests/run-unit.sh`・`check.h`・`bitwriter.h`・`check-skeleton.sh`（不備は Q1 へ）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない。
- 上の「注意」がこの Phase に明示に許す物だけは、この節の限りでない。

## 確かめの command（worktree の root で）

- `timeout 600 sh plan/ws326/tests/run-unit.sh cavlc` → 最後の行 `run-unit: PASS`。前の Phase の試験も `sh plan/ws326/tests/run-unit.sh all` で PASS のまま。
- `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`（header が p001 の物から変わっていない、cc と clang で warning 0）。
- zedBSD amd64: `timeout 1200 make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libmedia.so` → exit 0、出力に warning が 0。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 自分の diff を coding-style の §14 の checklist で見直した。表の block ごとに `Values referenced: ITU-T H.264 (08/2021) Table ...` の複数行の comment がある。source の comment・log に WS・Phase の番号や plan/ への参照が無い。
- `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と結果・未実施・残りを Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、実行した command、PASS の数、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が受け入れを見て決める）。

## 結果

- 日付: 2026-10-30（host の `date`）。
- 作った物: `userland/desktop/libmedia/h264sw-cavlc.c`（全関数と表。表 9-5 の 5 列、表 9-7・9-8・9-9 (a)・9-10、表 9-4 (a) を block ごとの `Values referenced:` の comment で出典付き）、単体試験 `plan/ws326/tests/unit/test-cavlc.c`。header は変えていない。
- 表の値の取り方: PDF の `pdftotext -layout` の出力を script で行ごとに抜き出して C の表を作り（script は repo に入れない）、PDF の頁の画（表 9-4 の 2 頁、表 9-5 の 3 頁、表 9-7〜9-10 の 3 頁）を読んで抜き出しと突き合わせた。表 9-4 は画から手で読んだ値（test に書いた）と pdftotext からの抜き出し（実装）が一致。表 9-5 の符号語は長さの短い順に並べ、`peek` した 16 bit と各符号語を比べて長さで飛ばす（h264sw_bits_peek と h264sw_bits_skip）。
- 実行した command と結果:
  - `timeout 600 sh plan/ws326/tests/run-unit.sh cavlc` → `test-cavlc: PASS (87868 checks)`、`run-unit: PASS`（ASan・UBSan の報告 0）。
  - `timeout 600 sh plan/ws326/tests/run-unit.sh all` → test-bits 169・test-bitwriter 14・test-cabac_init 4159・test-cavlc 87868・test-mb 1938・test-nal 95 の各 PASS、`run-unit: PASS`。
  - `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`。
  - zedBSD amd64 の `make ... build/amd64/dynamic/libmedia.so` → exit 0、出力の warning 0（`grep -ci warning` が 0）。
  - `git diff --check` → 問題なし。
  - 試験の確かめ（変異）: 実装に 7 通りの誤り（suffixLength の上限、+2 の規則、+15 の規則、TotalCoeff > 10 の閾値、total_zeros の行の長さ、表 9-5 の値と長さの各 1 つ）を一時的に入れ、どれも test-cavlc が FAIL になることを確かめて元に戻した。
- 試験の内容: 表 9-5 の 5 列から 13〜15 個ずつの符号語を PDF の画から手で選び（最短・最長、TrailingOnes 0〜3、TotalCoeff の小と大）、符号語の後ろを 0 と 1 にして長さを確かめる。nC の境（0・1・2・3・4・7・8・20・-1・-2）で列が替わること。表 9-7・9-8・9-9 (a)・9-10 の符号語を各 tzVlcIndex・zerosLeft から選ぶ。手で計算した block 13 個（9.2.2〜9.2.4 を逆に追った。level_prefix 14・15・16、suffixLength 1 始まりと +2 の規則、zerosLeft が途中で 0 になる、chroma DC、15 係数の AC、TotalCoeff 0）。9.2 を逆に書いた encoder（試験の中）で 32 個の level の値（±32767、-32768 を含む）を 3 つの形で、ランダムの block 6000 個（start/end/max_coeff の 9 通りの形）を往復。途中で切れた入力（各 block を各 byte 長で切る）。誤り。ランダムの入力 40000 個（EINVAL か成功のみ、level の範囲、total の上限）。nC は 3 x 3 の picture で A・B の有無・別の slice・skip（0）・I_PCM（16）・current の中の block・Cb と Cr の AC。cbp は表 9-4 (a) の 48 x 2 の全部と 48 以上の EINVAL。
- 試験は実装の .c を `#include` して static の表・補助関数を読む（public の関数は macro で名を替えて取り込み、public の経路は link した本物を試験する）。
- 仕様の表と phase.md の食い違い（要確認）: phase.md の「表の形」は「符号語の集合が prefix-free で Kraft の和が 1（9-5 の nC >= 8 の固定長の列も 1）」と書くが、表 9-5 の 0 <= nC < 2・2 <= nC < 4・4 <= nC < 8 の列と nC >= 8 の列、表 9-7 の tzVlcIndex 1、表 9-10 の zerosLeft > 6 は Kraft の和が 1 未満（仕様の表が全 0 の符号語などを使わない）。PDF から抜き出した値の和は: 0 <= nC < 2 が 1 - 2^-15、2 <= nC < 4 が 1 - 2^-13、4 <= nC < 8 が 1 - 2^-10、8 <= nC が 62/64（6 bit の固定長で 62 個）、tzVlcIndex 1 が 1 - 2^-9、zerosLeft > 6 が 1 - 2^-11。他（nC = -1、tzVlcIndex 2〜15、表 9-9 (a)、zerosLeft 1〜6）は 1。試験はこの値を厳密に確かめている（prefix-free と、符号語の数（62・14・17 - tzVlcIndex など）を含む）。
- header が決めていない点で担当が決めた物（Q1 の確認用）:
  - `h264sw_cavlc_residual_block` は max_coeff が 4・15・16 以外、start > end、end >= max_coeff、bits・levels・total_coeff が NULL、nC が -2 以下なら EINVAL（levels は触らない）。max_coeff と nC の組は検査しない（nC -1 と max_coeff 16 も読む）。
  - header の誤りの節の「total_zeros or a run that puts a level before start」は、9.2.4 の置き方では level が endIdx の後ろに出る場合しか無い（start より前には出ない）。実装は total_zeros が endIdx - startIdx + 1 - TotalCoeff を超える時と、run が zerosLeft を超える時を EINVAL にし、置く位置が end を超える時も EINVAL にする。header の文の「before start」は「past endIdx」の意味と解した。
  - 誤りの時の levels は 0 で埋めた後の途中の値、`*total_coeff` は coeff_token を読めた後なら TotalCoeff（それ以前は 0）。header は「levels then unspecified」。
  - 成功の時、levels の max_coeff 以降の要素は触らない。
  - level_prefix は 32 bit を覗き、1 が無い（32 個以上の 0）か 25 を超えれば EINVAL、1 が RBSP の外なら skip の誤りで EINVAL。prefix 16〜25 は受け、level が -2^15 .. 2^15 - 1 の外なら EINVAL。
  - `h264sw_cavlc_nc` は ctx が NULL か block >= 24 なら 0（header に誤りの値が無いため。nC は 0 以上の値で、負の誤りは返さない）。隣の総数は `total_coeff[base + 隣の block]` をそのまま読む（16 は I_PCM としてすでに入っている前提、clamp しない）。
  - `h264sw_cavlc_cbp` は intra が 0 以外なら intra の列、cbp が NULL なら EINVAL。
- 未実施: QEMU・実機の試験（この Phase の範囲外）。規約の全文の見直しの Phase（WS の最後）。表 9-5 の nC = -2 の列と表 9-9 (b)（4:2:2）は範囲外で実装していない。

