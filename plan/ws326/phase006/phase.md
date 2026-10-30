<!-- awesome-plan project=zedbsd record=ws326-p006 -->

# ws326-p006 DPB（marking・gap・出力の順）

- 親: [WS326](../ws.md)、設計 [design.md](../design.md)。
- 状態: 実装済み・試験 PASS（2026-10-30、cleared は Q1 が受け入れを見て決める）。
- 依存: p005（依存の Phase の実装が main に入ってから始める）。
- 担当の想定: Sonnet 5.5 high の subagent を 1 回起動（ws.md のユーザーの決定 2）。header は p001 の Opus の設計で、変えない。

## 読む物

1. AGENTS.md の §4（担当の共通の規則）・§7・§9・§10、plan/agents/protocol.md
2. plan/coding-style.md の全文（ANSI C: 宣言は関数の先頭、`for` の中で宣言しない、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、など。§14 の checklist で自分の diff を見直す）
3. plan/ws326/design.md の §1（出典の規則）・§2（決定）・§4（所有と寿命）・§5（単体試験の作り方）
4. `userland/desktop/libmedia/h264sw.h`（誤りの値の約束と上限）
5. 実装する header: `userland/desktop/libmedia/h264sw-dpb.h`（各関数の comment が仕様。struct の field の意味と単位も header に）。使う側の header（`#include` の先）も読む。
6. 仕様: ITU-T H.264 (08/2021) の 8.2.5・8.2.5.1〜8.2.5.4（式 8-39 など）、8.2.5.2、C.4.1〜C.4.5.3。PDF `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf`（SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`、git に入れない）。印刷の頁 N = PDF の頁 N + 22。表と行列は PDF の頁の画で読む（Read の tool の `pages`、1 回 20 頁まで）。`pdftotext -layout` の文字は列が崩れることがある。

## 作る物

- `userland/desktop/libmedia/h264sw-dpb.c`: 骨格（各関数の `ENOSYS` など）を header の仕様どおりの実装に置き換える。static の補助関数は自由（規約どおり前方宣言）。
- 単体試験 `plan/ws326/tests/unit/test-dpb.c`（`check.h`・`bitwriter.h` を使う。期待の値は仕様の表か、手で計算した値。他の decoder の source・出力を写さない）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- sliding window: max_num_ref_frames 2 で 3 枚目の参照が最古の short を外す。
- MMCO 1〜6 を各々（1: picNumX の short を外す、2: long を外す、3: short → long、4: MaxLongTermFrameIdx を下げて超える long を外す、5: 全部外す、6: 今の picture を long）、名前の無い picture を指す op は飛ばす。
- IDR: 全部 unused、no_output_of_prior_pics 1 で出力なしに空、0 で bumping で全部出力。changed 1 でも flag 0 なら出力（C.4.4 NOTE 3、header の決定）。
- make_room: 全 buffer が出力済みの参照（long-term を含む、MMCO 3・6 で作る）の時、FrameNumWrap 最小の short、無ければ LongTermFrameIdx 最小の long が外れて loop が終わる。store の output 0（leading の picture）。
- 出力の順: POC 0, 8, 4, 2, 6（B の pyramid の decode の順）を DPB 大きさ 3 で入れ、queue に出る順が POC の昇順で、bumping の時機が C.4.5.3 どおり（手で追った表を test に書く）。非参照の picture が全部より小さい POC の時は格納せずすぐ出力。reorder（max_num_reorder_frames）1 で遅れが 1 枚。
- gap: frame_num 0, 1, 4（gaps 許可、max_num_ref_frames 2）で non-existing が 2 と 3 の 2 枚（3 枚目以降は窓で外れるので最後の 2 枚だけ）、出力されないこと、POC type 2 での各 frame の POC（手で計算）、motion が全部 intra。
- flush: 残りが POC 順に全部。clear: 出力なし。queue 満杯で ENOBUFS（-ENOBUFS）。全 frame の参照の数が最後に 0（ASan）。
- 共通: 誤りの入力（範囲外・途中で切れた・巨大な値）で落ちず、header に書かれた誤りを返す。ASan・UBSan の報告が 0。

## 注意

- frame は h264sw-frame の pool から作る（p005 の実装を使う）。

## 変えてはいけない物

- `userland/desktop/libmedia/h264sw*.h`（全部）。header の不備・矛盾・足りない口を見つけたら、直さずに止めて Q1 に返す: どの header のどの関数か、何が足りないか、案の差分。
- 担当の module 以外の `h264sw-*.c`（他の Phase の物）。既存の libmedia の file（bits.c・h264.c・vkvideo.c・decoder.c など）、Makefile、`plan/ws326/tests/run-unit.sh`・`check.h`・`bitwriter.h`・`check-skeleton.sh`（不備は Q1 へ）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない。
- 上の「注意」がこの Phase に明示に許す物だけは、この節の限りでない。

## 確かめの command（worktree の root で）

- `timeout 600 sh plan/ws326/tests/run-unit.sh dpb` → 最後の行 `run-unit: PASS`。前の Phase の試験も `sh plan/ws326/tests/run-unit.sh all` で PASS のまま。
- `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`（header が p001 の物から変わっていない、cc と clang で warning 0）。
- zedBSD amd64: `timeout 1200 make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libmedia.so` → exit 0、出力に warning が 0。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 自分の diff を coding-style の §14 の checklist で見直した。表の block ごとに `Values referenced: ITU-T H.264 (08/2021) Table ...` の複数行の comment がある。source の comment・log に WS・Phase の番号や plan/ への参照が無い。
- `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と結果・未実施・残りを Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、実行した command、PASS の数、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が受け入れを見て決める）。

## p005 から引き継ぐ事（2026-10-30 Q1、p005 の実装の担当の報告）

frame_num の gap で作る non-existing の frame を POC の type 0 で `h264sw_poc_finish` に通すと、`prev_msb`・`prev_lsb` が合成の header の `poc_lsb`（0）で上書きされる。8.2.1.1 は non-existing の frame を前の参照の picture にしない。type 0 では finish を飛ばすか、前後で `prev_msb`・`prev_lsb` を保存して戻す（type 1・2 は普通に finish を通す）。

## 結果

- 日付: 2026-10-30（host の `date`）。
- 実装: `userland/desktop/libmedia/h264sw-dpb.c`（header は変えていない）。試験: `plan/ws326/tests/unit/test-dpb.c`（129447 checks、うち大半は固定の乱数 8 系列の 4000 picture ずつ）。
  期待の値は 8.2.5・8.2.5.2・8.2.5.4.1〜6・8-27〜8-29・8-39・C.4.2・C.4.4・C.4.5.1〜3 から紙で追った値（出力の順は test の comment に trace を書いた）。
  項目: sliding window（wrap と max_num_ref_frames 0 を含む）、MMCO 1〜6 を 11 枚の picture の連続で（名前の無い picNumX は飛ばす、op3 と op6 による LongTermFrameIdx の奪い合い、op4、op5）、op5 の出力、IDR（no_output 0/1、changed 1、long_term_reference_flag）、
  make_room（short の後に long、long 同士は最小の index、参照だけで埋まった DPB への store）、出力の順（B pyramid POC 0,8,4,2,6 を参照 3 枚と 2 枚で、reorder 1 と 0、同じ POC）、gap（frame_num 0,1,4 / 0,1,6 / 13 から 2 の wrap / type 0 / gap 無しの各場合 / 最長の gap / DPB 満杯での bump / ENOMEM / ENOBUFS と再試行 / op1 は non-existing を外せて op3 は飛ばす）、
  queue（ring の wrap、満杯、ENOBUFS で状態が変わらない）、NULL・範囲外・巨大な値、各 test の最後に全 frame が pool の free list に戻ること（参照 0、ASan の leak 0）。
- 実行した command と結果:
  - `timeout 600 sh plan/ws326/tests/run-unit.sh dpb` → `test-dpb: PASS (129447 checks)`、`run-unit: PASS`。
  - `timeout 900 sh plan/ws326/tests/run-unit.sh all` → 17 本 PASS、最後の行 `run-unit: PASS`。
  - `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`。
  - zedBSD amd64 の `build/amd64/dynamic/libmedia.so` の make → exit 0、出力に warning 0。
  - `git diff --check` → 問題なし。
- 変異試験（git の外で実装を壊して戻した、24 個のうち 23 を検出）: window の >= と >、FrameNumWrap の wrap 無し、picNumX の +1 落とし、bump が最大 POC、IDR の no_output 無視、make_room が long 先、op4 の境、op3 の既存 index を外さない、reorder の >=、gap の non-existing が出力待ち、
  gap で飛ばす分を飛ばさない、op5 の全出力無し、op6 で既存 index を外さない、op2 の対応のずれ、type 0 で finish を通す、IDR で gap、IDR の long の印落とし、「最後に格納した frame」の選び違い、非参照 output 0 の格納、
  同じ POC の非参照の即出力（`<=` を `<`）、bump で空にしない、op3 が non-existing を受ける、remove で sweep しない、の 23 個が FAIL。最初の版で通った 4 つ（同じ POC・bump の即時の空け・op3 の non-existing・remove の sweep）は試験を足して捕まえた。
  捕まらない物: queue の head の `% H264SW_OUTPUT_MAX` を take の側で省く変異（使う側で常に `%` を取るので観測できない同値）。
- 判断（header が黙っている所・読み方が二つある所）:
  - 引数が NULL は EINVAL（bump は -EINVAL）。store の marking が 0〜2 の外も EINVAL。header に無いが、呼び手の誤りを落ちずに返す。
  - gap: `gaps_in_frame_num_value_allowed_flag` は見ない（0 でも同じ frame を作る、header のとおり）。gap の frame の作り方は frame_get → fill_grey → make_room → order count → 格納の順で、make_room が ENOBUFS の時に poc の状態を進めない（再試行できる）。
    ENOMEM の時は window の marking だけが済んでいる。frame_num が max_frame_num 以上、prev_ref_frame_num が max_frame_num 以上、max_frame_num 0 は gap でない（何も作らず 0）。IDR は gap でない。
  - gap の type 0（引き継ぎ事項）: `h264sw_poc_finish` を通さず、`prev_ref_frame_num` だけを自分で進める（prev_msb・prev_lsb は動かない、試験で確認）。POC は「最後に格納した frame」= DPB の entry の中で `frame->id` が最大の物の poc（id は pool が出す度に増える。DPB が空なら 0）。
    type 1・2 で `h264sw_poc_compute` が EINVAL（溢れ）の時も同じ扱い（POC は直前の値、state は `prev_ref_frame_num` のみ）。header の comment は gaps の誤りに EINVAL を持たないので、EINVAL で返さずこの形にした。
  - remove の CurrPicNum は `current->frame_num` でなく `header->frame_num`（operation 5 の picture の frame->frame_num を decoder が先に 0 にしても CurrPicNum が崩れない）。`sps`・`current`・`changed` は使わない（DPB の field と header で足りる。changed は header の comment どおり出力の可否に関与しない）。
    **decoder の Phase への注意**: operation 5 の picture は `frame->frame_num` を 0 にしてから `h264sw_dpb_store` に渡すこと（7.4.3、DPB の entry の frame_num は store が `current->frame_num` から写す）。decoder.h の手順 6 は POC の写しだけを書いている。
  - sliding window は `参照の数 >= Max( max_num_ref_frames, 1 )` の間、FrameNumWrap 最小の short を外す（正しい stream では 1 回だけ。壊れた stream が窓を越えていても窓に戻す）。short が無ければ何も外さない。
  - MMCO: operation 0 と 6 を越える値は飛ばす。op3 は non-existing の frame を指す時は飛ばす（8.2.5.4.3 の制約）、op1 は non-existing も外せる（8.2.5.2 NOTE 4）。long_term_frame_idx が MaxLongTermFrameIdx を越えていても受ける（制約違反だが参照を失わない方を採った）。
    op4 の plus1 が int32 を越える時は MaxLongTermFrameIdx を INT32_MAX に丸める。op6 の後に op4（新しい上限が index 以下）か op5 が来た時は picture の long の印を戻して short にする。op5 を見た時だけ全 frame を bumping で出力する（header の has_mmco5 は見ない）。
    mmco_count が H264SW_MAX_MMCO を越えていれば MAX で打ち切る。
  - IDR: 参照を全部 unused にして、no_output なら clear（出力なし）、そうでなければ全部出力。IDR でも nal_ref_idc が 0 なら marking は UNUSED のまま。`max_long_term_frame_idx` は IDR で必ず -1 か 0 に決める。clear は `max_long_term_frame_idx` を -1 に戻す。
  - store: 参照 picture は make_room → 格納。非参照で output 0 なら DPB を変えず、reorder の bump も行わない前に抜けず（count は変わらないので何も起きない）そのまま続く。非参照の即出力は PicOrderCnt が全ての出力待ちより「小さい」時だけ（同じなら先に bump）。
    reorder の bump は格納（または即出力）の後に 1 回だけ回し、出力待ちが `reorder` を越える間 bump する。
  - init: dpb_frames は 1..16 に丸め、reorder は VUI の値（無ければ size）を size に丸める。
  - flush は残りの参照も含めて DPB を空にする（clear を呼ぶ）。queue が満杯なら ENOBUFS で DPB は途中の状態のまま（呼び手が queue を空けて呼び直せる）。
- 未実施: QEMU・実機の試験（範囲外）、decoder 全体との結合、field・MBAFF・PAFF の DPB（範囲外）、規約の全文の見直し Phase（WS の最後）。
