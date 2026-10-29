<!-- awesome-plan project=zedbsd record=ws326-p011 -->

# ws326-p011 CABAC の engine

- 親: [WS326](../ws.md)、設計 [design.md](../design.md)。
- 状態: 実装済み・試験 PASS（2026-10-30、cleared は Q1 が受け入れを見て決める）。
- 依存: p002、p010（依存の Phase の実装が main に入ってから始める）。
- 担当の想定: Sonnet 5.5 high の subagent を 1 回起動（ws.md のユーザーの決定 2）。header は p001 の Opus の設計で、変えない。

## 読む物

1. AGENTS.md の §4（担当の共通の規則）・§7・§9・§10、plan/agents/protocol.md
2. plan/coding-style.md の全文（ANSI C: 宣言は関数の先頭、`for` の中で宣言しない、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、など。§14 の checklist で自分の diff を見直す）
3. plan/ws326/design.md の §1（出典の規則）・§2（決定）・§4（所有と寿命）・§5（単体試験の作り方）
4. `userland/desktop/libmedia/h264sw.h`（誤りの値の約束と上限）
5. 実装する header: `userland/desktop/libmedia/h264sw-cabac.h`（各関数の comment が仕様。struct の field の意味と単位も header に）。使う側の header（`#include` の先）も読む。
6. 仕様: ITU-T H.264 (08/2021) の 9.3.1.1（式 9-5、表 9-11）・9.3.1.2、9.3.3.2.1〜9.3.3.2.4（表 9-44・9-45）、9.3.4（informative の符号化、試験で使う）。PDF `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf`（SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`、git に入れない）。印刷の頁 N = PDF の頁 N + 22。表と行列は PDF の頁の画で読む（Read の tool の `pages`、1 回 20 頁まで）。`pdftotext -layout` の文字は列が崩れることがある。

## 作る物

- `userland/desktop/libmedia/h264sw-cabac.c`: 骨格（各関数の `ENOSYS` など）を header の仕様どおりの実装に置き換える。static の補助関数は自由（規約どおり前方宣言）。
- 単体試験 `plan/ws326/tests/unit/test-cabac.c`（`check.h`・`bitwriter.h` を使う。期待の値は仕様の表か、手で計算した値。他の decoder の source・出力を写さない）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- 初期化: SliceQPY 0・26・51 と I・cabac_init_idc 0〜2 で、手で計算した ctxIdx 数個（各表から 1 つ以上）の pStateIdx と valMPS が式 9-5 どおり。276 は初期化しない。
- 往復: 9.3.4（informative の符号化 engine）を共有の試験の header `plan/ws326/tests/unit/cabacwriter.h`（新しく作る、後の Phase の試験も使う。decoder の code を流用しない）に書き、乱数（固定の seed）の bin 列を決まった ctxIdx・bypass・terminate の混ざりで符号化し、decoder で同じ列が戻る（10 万 bin 以上、状態の遷移の全域）。
- PCM: terminate で 1 の後の h264sw_cabac_pcm が正しい byte 位置から 384 byte を読み、engine を再開して続く bin が往復で一致。
- 誤り: 終わりを越えた読みで error、codIOffset 510・511 の開始で EINVAL。
- 表の形: 表 9-44 の rangeTabLPS は各列で pStateIdx とともに減らず（増えない）、各行で qCodIRangeIdx とともに増える、pStateIdx 63 の行は 2,2,2,2。表 9-45 の transIdxMPS は min( pStateIdx + 1, 62 )（62 と 63 は自分）、transIdxLPS は pStateIdx 以下、0 の LPS は 0。
- 共通: 誤りの入力（範囲外・途中で切れた・巨大な値）で落ちず、header に書かれた誤りを返す。ASan・UBSan の報告が 0。

## 注意

- (m, n) の表は p010 の h264sw-cabac-init.c（h264sw_cabac_init_mn）。表 9-44（rangeTabLPS 64×4）と 9-45 はこの Phase で入れ、PDF の画と照合した記録を phase.md に残す。

## 変えてはいけない物

- `userland/desktop/libmedia/h264sw*.h`（全部）。header の不備・矛盾・足りない口を見つけたら、直さずに止めて Q1 に返す: どの header のどの関数か、何が足りないか、案の差分。
- 担当の module 以外の `h264sw-*.c`（他の Phase の物）。既存の libmedia の file（bits.c・h264.c・vkvideo.c・decoder.c など）、Makefile、`plan/ws326/tests/run-unit.sh`・`check.h`・`bitwriter.h`・`check-skeleton.sh`（不備は Q1 へ）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない。
- 上の「注意」がこの Phase に明示に許す物だけは、この節の限りでない。

## 確かめの command（worktree の root で）

- `timeout 600 sh plan/ws326/tests/run-unit.sh cabac` → 最後の行 `run-unit: PASS`。前の Phase の試験も `sh plan/ws326/tests/run-unit.sh all` で PASS のまま。
- `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`（header が p001 の物から変わっていない、cc と clang で warning 0）。
- zedBSD amd64: `timeout 1200 make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libmedia.so` → exit 0、出力に warning が 0。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 自分の diff を coding-style の §14 の checklist で見直した。表の block ごとに `Values referenced: ITU-T H.264 (08/2021) Table ...` の複数行の comment がある。source の comment・log に WS・Phase の番号や plan/ への参照が無い。
- `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と結果・未実施・残りを Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、実行した command、PASS の数、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が受け入れを見て決める）。

## 結果

2026-10-30、worktree `/home/awe/zedBSD-worktrees/s-ws326-p011`（branch `agent/s-ws326-p011`）。

- 実装: `h264sw-cabac.c` に `h264sw_cabac_start`・`_decision`・`_bypass`・`_terminate`・`_pcm` と表 9-44（rangeTabLPS）・表 9-45（transIdxLPS・transIdxMPS）を入れた。header は変えていない。
  - engine は codIOffset を scaled に持つ: `offset` = 9 bit の codIOffset の下に `bits_left`（1 byte ずつ読み足す、8 未満で 1 byte）bit の先読み。比較は `offset >= range << bits_left`、renormalization は `bits_left` を減らすだけ。
  - `position` は「次に読む byte」で、終わりを越えた zero の補い（padding）の byte も数える。model（9 bit の式）が読んだ bit は `8 * position - bits_left`。`error` は padding の byte の bit が `bits_left` を越えて consume された時（`8 * (position - size) > bits_left`）に EINVAL。先読みだけの padding では立てない。
  - `h264sw_cabac_pcm` は terminate が 1 を返した後、model の読んだ bit の直後から byte 境界まで（pcm_alignment_zero_bit）を飛ばし、count byte を複写し、続く byte で engine を初期化し直す。context は保つ。
  - 初期化の context の一覧は表 9-11 の列（I: 3..10・60..275・277..459、P: 11..23・40..275・277..459、B: 24..275・277..459）。一覧に無い ctxIdx は pStateIdx 0・valMPS 0。式 9-5 の `>> 4`（負の積の切り下げ）は、積に 8192 を足して割る方法で右 shift を避けた。
- 判断（header が書いていない所）:
  - `h264sw_cabac_start` は、alignment の後の data が 2 byte 未満（9 bit の codIOffset が読めない）の時も EINVAL（header は 510・511 だけを書く）。失敗の時は `memset` で全 field を 0 にし `error` を EINVAL にして返す（呼び手が戻り値を見なくても次の error の確認で止まる）。
  - `h264sw_cabac_pcm` は `error` が立っている engine・data に収まらない count・NULL の samples（count > 0）を EINVAL とし、`error` を立てる。engine の再初期化に 2 byte 足りない時も EINVAL。
  - 規約違反の stream（codIOffset が codIRange 以上のまま）は、header が決めていないので garbage の bin を返し続けるだけで、全て unsigned の演算・範囲内の table の添字（未定義の動作なし）。
  - 表 9-44・9-45 は、表の形の試験が値に触れるよう、header に宣言の無い非 static の `h264sw_cabac_range_lps`・`h264sw_cabac_trans_lps`・`h264sw_cabac_trans_mps` として定義した（libmedia.so の exports.map の外には出ない）。試験側が extern で宣言する。
  - 表 9-11 の SI の列は使わない（slice_type は I・P・B だけ、他は EINVAL）。cabac_init_idc は I では見ず、P・B で 3 以上は EINVAL。qp は 0..51 に clip。
- 表 9-44・9-45 の照合: PDF 296・297 頁（印刷 274・275 頁）の頁の画を読んで形と行の値を目で確かめ、さらに `pdftotext -layout` の出力から scratch の script（`python3 -I`、commit していない）で 64 行の rangeTabLPS と 2 x 64 の transIdx を抜き出し、C の表と全 256 + 128 個が一致することを確かめた。試験側（`cabacwriter.h`）は別に同じ script の出力から作った自分の写しを持ち、decoder の表と全部の値を比べる。
- 試験 `plan/ws326/tests/unit/test-cabac.c` と共有の符号化 engine `plan/ws326/tests/unit/cabacwriter.h`（9.3.4 の InitEncoder・EncodeDecision・EncodeBypass・EncodeTerminate と flush・PutBit・RenormE を図 9-8〜9-12 の画から書いた。decoder の code を使っていない）:
  - 表の形（行は増え 63 行目は 2,2,2,2、列は増えない、transIdxMPS は Min(p+1, 62)、transIdxLPS は p 以下・0 は 0・単調）と頁から読んだ値の抜き取り。
  - 初期化: 式 9-5 を手で計算した 40 個の (ctxIdx, 種類, idc, qp, 状態)（各表から。丸めが負になる場合・preCtxState の 1 と 126 の端・64 を含む）、全 slice 型・全 idc・qp -3〜54 で、一覧の ctxIdx が式 9-5 の別実装（cabacwriter）と一致し、他は 0、276 は 0。qp の範囲外は端と同じ。
  - 往復: 28 本の stream（I・P の idc 3 通り・B の idc 3 通り、各 4 つの qp）の 24000 操作（decision 70%・bypass 26%・terminate 0 が 4%）、合計 672000 操作。slice header の 0〜7 bit の後の alignment、最後は end_of_slice_flag 1 で error 0。状態の網羅: LPS で出た pStateIdx 63 状態中 63、(pStateIdx, qCodIRangeIdx) の組 252 中 252。
  - PCM: 400 本の短い stream（1〜40 操作、30% が I_PCM で連続も含む）で 384 byte が一致し、続く bin が一致（1000 回以上の I_PCM、終わりの bit 位置の全ての byte 内の位置を通る）。最初の操作が I_PCM の 1 本。
  - 誤り: 2 byte の 0 で 7 回の bypass は error 0・8 回目で error、decision の renormalization での error、510・511 の開始で EINVAL（509 は 0）、data 不足、alignment の 0 bit、壊れた reader、slice 型・idc の範囲外、ctx 460・276・0xffffffff で 0 と error、PCM の count 超過・NULL・error 後。
  - 乱数 3000 本（0〜39 byte、0 と 0xff の偏り含む）に 200 回ずつの乱数の呼び出し（PCM を含む）で ASan・UBSan の報告 0。
  - mutation の確認: rangeTabLPS の 1 値の変更・PCM の byte 位置の 1 bit のずれ・`>> 4` の切り下げを切り捨てに変更・error の判定の 1 bit のずれ・初期化の範囲の 1 つのずれ、の 5 つの全てで test-cabac が FAIL になった（その後元に戻した）。
- 確かめた command と結果:
  - `timeout 600 sh plan/ws326/tests/run-unit.sh cabac` → `test-cabac: PASS (195654 checks)`、`run-unit: PASS`。
  - `timeout 900 sh plan/ws326/tests/run-unit.sh all` → test-bits PASS (169)・test-bitwriter PASS (14)・test-cabac PASS (195654)・test-cabac_init PASS (4159)・test-nal PASS (95)、`run-unit: PASS`。
  - `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`。
  - amd64 の `build/amd64/dynamic/libmedia.so`（Phase の command どおり）→ exit 0、warning 0（`-Wall -Wextra -Werror`、h264sw-cabac.c は compile された）。
  - `git diff --check` → 出力なし。clang でも test-cabac.c は `-std=c89 -Wall -Wextra -Werror -Wpedantic` で通る。
- 未実施: 実行時の QEMU・実機の試験（この Phase の範囲外）、CABAC の syntax element との結合（p012 以降）、速さの測定（ここは 1 bit ずつ数える renormalization の loop と 1 byte ずつの先読みで、速さの Phase で見直す）。
