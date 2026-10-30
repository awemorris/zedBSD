<!-- awesome-plan project=zedbsd record=ws326-p019 -->

# ws326-p019 macroblock の再構成

- 親: [WS326](../ws.md)、設計 [design.md](../design.md)。
- 状態: cleared（2026-10-31 Q1、053bafc0b。実装と試験 S-p019）。
- 依存: p013〜p018（依存の Phase の実装が main に入ってから始める）。
- 担当の想定: Sonnet 5.5 high の subagent を 1 回起動（ws.md のユーザーの決定 2）。header は p001 の Opus の設計で、変えない。

## 読む物

1. AGENTS.md の §4（担当の共通の規則）・§7・§9・§10、plan/agents/protocol.md
2. plan/coding-style.md の全文（ANSI C: 宣言は関数の先頭、`for` の中で宣言しない、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、など。§14 の checklist で自分の diff を見直す）
3. plan/ws326/design.md の §1（出典の規則）・§2（決定）・§4（所有と寿命）・§5（単体試験の作り方）
4. `userland/desktop/libmedia/h264sw.h`（誤りの値の約束と上限）
5. 実装する header: `userland/desktop/libmedia/h264sw-recon.h`（各関数の comment が仕様。struct の field の意味と単位も header に）。使う側の header（`#include` の先）も読む。
6. 仕様: ITU-T H.264 (08/2021) の 8.3・8.4（8.4.1・8.4.1.4・8.4.2）・8.5（8.5.1〜8.5.4・8.5.8・8.5.14）。PDF `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf`（SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`、git に入れない）。印刷の頁 N = PDF の頁 N + 22。表と行列は PDF の頁の画で読む（Read の tool の `pages`、1 回 20 頁まで）。`pdftotext -layout` の文字は列が崩れることがある。

## 作る物

- `userland/desktop/libmedia/h264sw-recon.c`: 骨格（各関数の `ENOSYS` など）を header の仕様どおりの実装に置き換える。static の補助関数は自由（規約どおり前方宣言）。
- 単体試験 `plan/ws326/tests/unit/test-recon.c`（`check.h`・`bitwriter.h` を使う。期待の値は仕様の表か、手で計算した値。他の decoder の source・出力を写さない）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- I_PCM の複写、I_16x16 DC の 1 macroblock（residual 無し → 128 の平ら、DC の level 1 つ → 手で計算した値）。
- I_4x4 の 16 block の順（後の block が前の block の再構成の sample を端に使う）。
- P_L0_16x16 の整数 MV で参照の複写、residual の加算、weighted の単。
- B の双予測の平均、direct の quadrant。
- QP'C（chroma_qp_index_offset と second の Cb・Cr 別、表 8-15）。
- p018 と合わせて h264sw_slice_data の loop: 2x2 macroblock の CAVLC の slice を bitwriter で組み、性質で確かめる: 全部 I_16x16 DC で residual 無しの I slice は全 sample 128、I_PCM の macroblock は送った sample のまま、1 枚目の I の後の P_Skip だけの P slice は参照と同じ、P_L0_16x16 の整数 MV (16, 0) は参照を 4 sample ずらした物（端は clamp）。全 sample の手計算はしない。
- 共通: 誤りの入力（範囲外・途中で切れた・巨大な値）で落ちず、header に書かれた誤りを返す。ASan・UBSan の報告が 0。

## 変えてはいけない物

- `userland/desktop/libmedia/h264sw*.h`（全部）。header の不備・矛盾・足りない口を見つけたら、直さずに止めて Q1 に返す: どの header のどの関数か、何が足りないか、案の差分。
- 担当の module 以外の `h264sw-*.c`（他の Phase の物）。既存の libmedia の file（bits.c・h264.c・vkvideo.c・decoder.c など）、Makefile、`plan/ws326/tests/run-unit.sh`・`check.h`・`bitwriter.h`・`check-skeleton.sh`（不備は Q1 へ）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない。
- 上の「注意」がこの Phase に明示に許す物だけは、この節の限りでない。

## 確かめの command（worktree の root で）

- `timeout 600 sh plan/ws326/tests/run-unit.sh recon` → 最後の行 `run-unit: PASS`。前の Phase の試験も `sh plan/ws326/tests/run-unit.sh all` で PASS のまま。
- `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`（header が p001 の物から変わっていない、cc と clang で warning 0）。
- zedBSD amd64: `timeout 1200 make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libmedia.so` → exit 0、出力に warning が 0。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 自分の diff を coding-style の §14 の checklist で見直した。表の block ごとに `Values referenced: ITU-T H.264 (08/2021) Table ...` の複数行の comment がある。source の comment・log に WS・Phase の番号や plan/ への参照が無い。
- `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と結果・未実施・残りを Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、実行した command、PASS の数、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が受け入れを見て決める）。

## 結果

- 日付: 2026-10-31（host の `date`）。状態: 実装済み・試験 PASS（cleared は Q1 が受け入れを見て決める）。
- 作った物: `userland/desktop/libmedia/h264sw-recon.c`（`h264sw_recon_mb`: I_PCM、Intra_4x4・Intra_8x8・Intra_16x16 と chroma、P_Skip、P の 16x16・16x8・8x16・8x8（sub 4 種）、B_Skip・B_Direct_16x16・B_8x8（direct sub を含む）、重み付き予測、4x4・8x8 の residual）、単体試験 `plan/ws326/tests/unit/test-recon.c`。header・他の module・Makefile・試験の道具は変えていない。
- 実行した command と結果:
  - `timeout 600 sh plan/ws326/tests/run-unit.sh recon` → `test-recon: PASS (33584 checks)`、`run-unit: PASS`（ASan・UBSan の報告 0）。
  - `timeout 900 sh plan/ws326/tests/run-unit.sh all` → 全 test PASS（test-macroblock 1107981・test-recon 33584 ほか 24 本）、`run-unit: PASS`。
  - `BASE=fd1d6f88c sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`。
  - zedBSD amd64 の `make ... build/amd64/dynamic/libmedia.so` → exit 0、出力の warning 0（h264sw-recon.o を含めて link）。cc と clang の `-std=c89 -Wall -Wextra -Werror -Wpedantic` で .c を単独 compile して OK。
  - `git diff --check` → 問題なし。
- 試験の内容: I_PCM の複写（3 plane、周りの sample が不変、nonzero 0xffff）、Intra_16x16 DC（residual 無しで 128、DC level 1 を QPY 28 で 129・36 で 131、-1 で 127、scan 位置 1 の level で左右の block が 136・120）、AC level、chroma DC の位置（c01・c10 で 148・108）、QP'C（Cb と Cr 別の offset、Table 8-15 の写しで 12 通り、clip 0 と 51）、Intra_4x4 の 16 block の順（block 0 の residual を後の block が端に使い全体 132）、Intra_8x8（130 と 4 bit の nonzero）、使えない mode の概念上の隠蔽（1 macroblock で concealed 1）、scaling list の intra・inter・成分（Intra Cb 16・Cr 32・Inter Cb 32・Cr 16、8x8 の inter）、P_L0_16x16 の整数 MV（右端の clamp 込み）、分数 MV（半 sample の 6 tap と負の MV の floor）、explicit weight、P_Skip（median の vector と、B が無い時の 0）、16x8・8x16 の mvp の連鎖、P_8x8 の 4x4 sub の連鎖（C が未導出で D に替わる）、sub ごとの ref_idx、B の双予測（別 MV）、implicit weight（w0 48・w1 16）、B_L0_L1_16x8、spatial・temporal の direct（mvL0 8・mvL1 -8）、B_8x8（direct・L1・Bi・L0 の sub）、vector の wrap、誤り（context・frame・範囲外の macroblock・範囲外の ref_idx・NULL の frame・大きさの違う frame・co-located 無し・不正な type と sub）、ランダムの syntax 30000 本。slice_data の loop（bitwriter の CAVLC、2x2 macroblock）: I_16x16 DC だけの I slice は全 sample 128、I_PCM の 4 macroblock は送った sample のまま、skip だけの P slice は参照と同じ、P_L0_16x16 の整数 MV (16, 0) は参照を 4 sample（chroma は 2）ずらした物（端は clamp）、B_Skip だけの B slice は参照 2 枚の平均、途中で切れた stream とランダムの stream 3000 本。期待の値は仕様の式と表から手で出した物。
- 試験の確かめ（一時的な突然変異、git の外）: 18 通りの誤りを入れ、17 通りで test-recon が FAIL（Intra_4x4 の residual 抜け、Cr に Cb の offset、Table 8-15 無し、inter の luma に intra の list、floor 除算が 0 への丸め、ref_id の書き忘れ、decoded_4x4 の更新忘れ、chroma の weight が luma の物、luma DC の転置、chroma DC の index の取り違え、P_Skip の ref_id、8x8 の nonzero が 1 bit、concealed を event ごとに数える、intra4x4_mode の書き忘れ、PCM の Cb と Cr の plane の取り違え、一様性の判定が ref_idx を見ない、8x8 の inter に intra の list）。PASS のまま残ったのは vector の wrap の「下側を戻さない」だけで、int16_t への変換が host の compiler で同じ結果になるため等価（wrap を書く理由は変換が実装定義だから）。
- header が決めていない点で担当が決めた物（Q1 の確認用）:
  - concealed は 1 macroblock につき最大 1 だけ足す（使えない mode・missing の参照が複数あっても 1）。
  - 動きは partition ごとに導出して各 4x4 block に書き、予測は動き（vector と ref_idx）が同じ 16x16・8x8・4x4 の最大の正方形にまとめる。サンプル単位の演算なので partition の形で結果は変わらない。
  - Intra_16x16 の nonzero は DC level が 1 つでもあれば 16 bit 全部（struct h264sw_mb の規則）、AC は block ごと。
  - 参照の frame の大きさが現在の frame と違う、plane が NULL、ref_idx が list の外・負、partition が macroblock の外・4 の倍数でない、list を 1 つも使わない partition は EINVAL。
  - I_NxN のままの kind（parser は解決する）と未知の kind は EINVAL。
- 未実施: CABAC の slice を recon まで通す試験（p021 の stream の試験と p022 の適合で）、実 stream での比較、QEMU・実機。
