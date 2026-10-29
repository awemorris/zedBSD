<!-- awesome-plan project=zedbsd record=ws326-p002 -->

# ws326-p002 bit reader と NAL

- 親: [WS326](../ws.md)、設計 [design.md](../design.md)。
- 状態: cleared（2026-10-30 Q1 の判定。実装と試験は Sonnet 5.5 high、8fc7866ec）。
- 依存: p001（依存の Phase の実装が main に入ってから始める）。
- 担当の想定: Sonnet 5.5 high の subagent を 1 回起動（ws.md のユーザーの決定 2）。header は p001 の Opus の設計で、変えない。

## 読む物

1. AGENTS.md の §4（担当の共通の規則）・§7・§9・§10、plan/agents/protocol.md
2. plan/coding-style.md の全文（ANSI C: 宣言は関数の先頭、`for` の中で宣言しない、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、など。§14 の checklist で自分の diff を見直す）
3. plan/ws326/design.md の §1（出典の規則）・§2（決定）・§4（所有と寿命）・§5（単体試験の作り方）
4. `userland/desktop/libmedia/h264sw.h`（誤りの値の約束と上限）
5. 実装する header: `userland/desktop/libmedia/h264sw-bits.h`、`userland/desktop/libmedia/h264sw-nal.h`（各関数の comment が仕様。struct の field の意味と単位も header に）。使う側の header（`#include` の先）も読む。
6. 仕様: ITU-T H.264 (08/2021) の 7.2（descriptor）、7.3.1・7.4.1（NAL unit、emulation prevention）、7.3.2.11（rbsp_trailing_bits）、9.1・9.1.1（表 9-2・9-3）、Annex B（B.1・B.2）。PDF `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf`（SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`、git に入れない）。印刷の頁 N = PDF の頁 N + 22。表と行列は PDF の頁の画で読む（Read の tool の `pages`、1 回 20 頁まで）。`pdftotext -layout` の文字は列が崩れることがある。

## 作る物

- `userland/desktop/libmedia/h264sw-bits.c`: 骨格（各関数の `ENOSYS` など）を header の仕様どおりの実装に置き換える。static の補助関数は自由（規約どおり前方宣言）。
- `userland/desktop/libmedia/h264sw-nal.c`: 骨格（各関数の `ENOSYS` など）を header の仕様どおりの実装に置き換える。static の補助関数は自由（規約どおり前方宣言）。
- 単体試験 `plan/ws326/tests/unit/test-bits.c`（`check.h`・`bitwriter.h` を使う。期待の値は仕様の表か、手で計算した値。他の decoder の source・出力を写さない）。
- 単体試験 `plan/ws326/tests/unit/test-nal.c`（`check.h`・`bitwriter.h` を使う。期待の値は仕様の表か、手で計算した値。他の decoder の source・出力を写さない）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- bits: u(n) の 0・1・17・32 bit、境界をまたぐ読み、終わりを越えた読みで error が立ち 0 を返し位置が動かない、error の後の読みも 0。
- bits: ue・se を bitwriter で作った値（0、1、2、254、255、2^16、2^31 - 1、2^32 - 2）で往復。leadingZeroBits 32 以上の符号は error。se の 2^32 - 2 は -(2^31 - 1)。
- bits: te の range 1（1 bit を反転）と range 2 以上（ue）、range 0 は error。peek は位置を動かさず、終わりの先は 0 で error にしない。
- bits: more_rbsp_data が stop bit の直前まで 1、stop の位置で 0（末尾に 0x00 の byte が続く RBSP でも）。trailing の正しい形と、stop bit の後に 1 がある形（EINVAL）。
- nal: find が 3 byte と 4 byte の start code、先頭の zero、末尾の trailing_zero_8bits、start code 2 つ続き（長さ 0 の NAL は飛ばす）、start code の無い入力（0 を返し cursor = size）。
- nal: parse の header（ref_idc・type）、0x000003 の除去、末尾の 0x000003（cabac_zero_word の後、正しい）、forbidden_zero_bit 1・0x000002・0x000003 04（EINVAL）、capacity 不足（ENOBUFS）、type 14・20 は rbsp_size 0。
- 共通: 誤りの入力（範囲外・途中で切れた・巨大な値）で落ちず、header に書かれた誤りを返す。ASan・UBSan の報告が 0。

## 注意

- `h264sw_nal_parse` は `media_rbsp_unescape`（bits.h）を使い、末尾の 0x03 を先に落とす（bits.c は変えない）。

## 変えてはいけない物

- `userland/desktop/libmedia/h264sw*.h`（全部）。header の不備・矛盾・足りない口を見つけたら、直さずに止めて Q1 に返す: どの header のどの関数か、何が足りないか、案の差分。
- 担当の module 以外の `h264sw-*.c`（他の Phase の物）。既存の libmedia の file（bits.c・h264.c・vkvideo.c・decoder.c など）、Makefile、`plan/ws326/tests/run-unit.sh`・`check.h`・`bitwriter.h`・`check-skeleton.sh`（不備は Q1 へ）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない。
- 上の「注意」がこの Phase に明示に許す物だけは、この節の限りでない。

## 確かめの command（worktree の root で）

- `timeout 600 sh plan/ws326/tests/run-unit.sh bits nal` → 最後の行 `run-unit: PASS`。前の Phase の試験も `sh plan/ws326/tests/run-unit.sh all` で PASS のまま。
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
- 実装: `h264sw-bits.c`、`h264sw-nal.c`（header は変えていない）。試験: `plan/ws326/tests/unit/test-bits.c`（169 checks）、`test-nal.c`（95 checks）。
  どちらも仕様の表・手で計算した値・bitwriter での往復で期待を作り、固定の乱数列の入力（bits 2000 回、nal 3000 回）で境界を越えないことも確かめた。
- `timeout 600 sh plan/ws326/tests/run-unit.sh bits nal` → `test-bits: PASS (169 checks)`、`test-nal: PASS (95 checks)`、`run-unit: PASS`。
- `timeout 600 sh plan/ws326/tests/run-unit.sh all` → bits 169・bitwriter 14・nal 95、`run-unit: PASS`。
- `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`。
- zedBSD amd64 の `build/amd64/dynamic/libmedia.so` の make → exit 0、warning 0。
- `git diff --check` → 問題なし。
- 判断: `h264sw_bits_peek` は error 後と count 33 以上で 0 を返す（header は error 後の peek を述べないので sticky に合わせた）。
  `h264sw_bits_trailing` が EINVAL の時は position を動かさず error も立てない。`h264sw_bits_left` は size * 8 - position（error の有無に依らない）。
- 未実施: QEMU・実機の試験（この Phase の範囲外）。
