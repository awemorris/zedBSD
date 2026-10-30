<!-- awesome-plan project=zedbsd record=ws326-p007 -->

# ws326-p007 reference picture list

- 親: [WS326](../ws.md)、設計 [design.md](../design.md)。
- 状態: cleared（2026-10-30 Q1 の判定。実装と試験は Sonnet 5.5 high、ce9b158f1）。
- 依存: p006（依存の Phase の実装が main に入ってから始める）。
- 担当の想定: Sonnet 5.5 high の subagent を 1 回起動（ws.md のユーザーの決定 2）。header は p001 の Opus の設計で、変えない。

## 読む物

1. AGENTS.md の §4（担当の共通の規則）・§7・§9・§10、plan/agents/protocol.md
2. plan/coding-style.md の全文（ANSI C: 宣言は関数の先頭、`for` の中で宣言しない、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、など。§14 の checklist で自分の diff を見直す）
3. plan/ws326/design.md の §1（出典の規則）・§2（決定）・§4（所有と寿命）・§5（単体試験の作り方）
4. `userland/desktop/libmedia/h264sw.h`（誤りの値の約束と上限）
5. 実装する header: `userland/desktop/libmedia/h264sw-reflist.h`（各関数の comment が仕様。struct の field の意味と単位も header に）。使う側の header（`#include` の先）も読む。
6. 仕様: ITU-T H.264 (08/2021) の 8.2.4・8.2.4.1（式 8-27〜8-29）・8.2.4.2.1・8.2.4.2.3・8.2.4.3.1（式 8-34〜8-37）・8.2.4.3.2。PDF `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf`（SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`、git に入れない）。印刷の頁 N = PDF の頁 N + 22。表と行列は PDF の頁の画で読む（Read の tool の `pages`、1 回 20 頁まで）。`pdftotext -layout` の文字は列が崩れることがある。

## 作る物

- `userland/desktop/libmedia/h264sw-reflist.c`: 骨格（各関数の `ENOSYS` など）を header の仕様どおりの実装に置き換える。static の補助関数は自由（規約どおり前方宣言）。
- 単体試験 `plan/ws326/tests/unit/test-reflist.c`（`check.h`・`bitwriter.h` を使う。期待の値は仕様の表か、手で計算した値。他の decoder の source・出力を写さない）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- P の初期: short を PicNum の降順（frame_num の wrap を含む FrameNumWrap）、続いて long を LongTermPicNum の昇順。num_ref_idx_active で切る。
- B の初期: list0 は POC が今より小さい short を降順、大きいのを昇順、long。list1 はその逆。list1 = list0 で 2 枚以上の時に先頭 2 つを入れ替え。
- modification: idc 0・1 の picNumPred の連鎖と wrap、idc 2 の long。同じ picture の重複が後ろから消える（8.2.4.3.1 の shift）。
- missing: list が短い時の位置と、無い picture を指す modification で missing 1 と stand-in。DPB が空の P slice で EINVAL。h264sw_reflist_find。
- 共通: 誤りの入力（範囲外・途中で切れた・巨大な値）で落ちず、header に書かれた誤りを返す。ASan・UBSan の報告が 0。

## 注意

- DPB は h264sw_dpb_store などで組むか、struct h264sw_dpb の entry を test で直に埋める（どちらでもよい、comment に書く）。

## 変えてはいけない物

- `userland/desktop/libmedia/h264sw*.h`（全部）。header の不備・矛盾・足りない口を見つけたら、直さずに止めて Q1 に返す: どの header のどの関数か、何が足りないか、案の差分。
- 担当の module 以外の `h264sw-*.c`（他の Phase の物）。既存の libmedia の file（bits.c・h264.c・vkvideo.c・decoder.c など）、Makefile、`plan/ws326/tests/run-unit.sh`・`check.h`・`bitwriter.h`・`check-skeleton.sh`（不備は Q1 へ）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない。
- 上の「注意」がこの Phase に明示に許す物だけは、この節の限りでない。

## 確かめの command（worktree の root で）

- `timeout 600 sh plan/ws326/tests/run-unit.sh reflist` → 最後の行 `run-unit: PASS`。前の Phase の試験も `sh plan/ws326/tests/run-unit.sh all` で PASS のまま。
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
- 実装: `userland/desktop/libmedia/h264sw-reflist.c`（header は変えていない）。試験: `plan/ws326/tests/unit/test-reflist.c`（278758 checks、うち大半は固定の乱数の 4000 slice）。
  期待の値は 8.2.4・8.2.4.1（8-27〜8-29）・8.2.4.2.1（PicNum 300・302・303 と LongTermPicNum 0・3 の例）・8.2.4.2.3・8.2.4.3.1（8-34〜8-37）・8.2.4.3.2（8-38）から紙で追った値（modification は test の comment に picNumPred・picNumNoWrap・picNum の手順を書いた）。
  項目: P の初期（spec の例、cut・pad、non-existing を含む、未使用の frame を含まない、SP）、frame_num の wrap（MaxFrameNum 16 で FrameNumWrap -2〜1）、B の初期（POC 0〜12 と long 2 枚で list0・list1、cut、pad）、
  POC が現在と等しい frame（list0 は 4,8,12、list1 は 12,8,4）、list1 = list0 の先頭 2 つの入れ替え（cut の前に比べる、1 枚だけ・long だけ・異なる list）、idc 0・1 の picNumPred の連鎖と wrap と重複の除去（同じ picture が 2 回入る場合と短い list）、
  idc 2 と、PicNum と LongTermPicNum が同じ値の short・long を取り違えないこと、list1 の別の picNumL1Pred、missing（無い picture を指す modification、2 連続、list が短い、無い long、B の list0 が全部 missing で list1 から stand-in、long だけの DPB、参照の無い DPB、non-existing より decoded を選ぶ）、
  DPB が空で P・B は EINVAL（counts 0）、I・SI は 0、h264sw_reflist_find（最小の index、missing は見ない、無い・id 0・NULL・count が巨大）、誤りの入力（NULL、slice_type が範囲外、num_ref_idx_active が 0・33・巨大、max_frame_num が 0・65537・巨大、frame_num が max 以上、modification_count が 34・巨大、idc 4・巨大、
  abs_diff が MaxFrameNum 以上・0xFFFFFFFF、long_term_pic_num が 0xFFFFFFFF、list を越える数の operation、33 個の operation を長さ 1 と 32 の list に）、乱数（random の DPB・header・operation・extreme な POC で、EINVAL は空の DPB だけ、list は全位置に DPB の frame を持ち、marking・POC・picNum が entry と一致、operation 無しの P は重複無しで PicNum の降順の後に long の昇順）。各 test の最後に ASan の leak 0。
- 実行した command と結果:
  - `timeout 600 sh plan/ws326/tests/run-unit.sh reflist` → `test-reflist: PASS (278758 checks)`、`run-unit: PASS`。
  - `timeout 900 sh plan/ws326/tests/run-unit.sh all` → 全 PASS、最後の行 `run-unit: PASS`。
  - `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`。
  - zedBSD amd64 の `build/amd64/dynamic/libmedia.so` の make → exit 0、出力に warning 0。
  - `git diff --check` → 問題なし。
- 変異試験（git の外で実装を壊して戻した、23 個のうち下の 2 つ以外の 21 個を検出）: FrameNumWrap の `>` を `>=`、sort の向き、B list0 の `>=` を `>`、list1 の入れ替えの条件を `> 2`、picNum の wrap の `>` を `>=`、short の dedupe で long を区別しない、find が missing を見る、picNumPred を更新しない、
  見つからない時にも dedupe、dedupe の範囲を 1 つ短く、modification_count の上限検査を落とす、stand-in を他の list から取らない・自分の list の missing も取る、non-existing より decoded を選ぶ規則を落とす、B の long の降順、idc > 3 の検査を落とす、pad の境、list1 の `<=` を `<`、
  FrameNumWrap の引き算を落とす、find_long の比較、frame_num の範囲検査を落とす、を FAIL で捕まえた。
  捕まらない物（観測できない同値）: (1) idc 1 の `picNumNoWrap >= MaxPicNum` を `>`（等しい時は wrap しなくても picNum が `NoWrap - MaxPicNum` で 0 に戻り、次の pred も modulo で同じ）、(2) 「list が満杯なら以降の operation を飛ばす」の break を落とす（operation は最大 33、作業 list は 33 要素なので配列を越えず、残す範囲は変わらない。防御として残した）。
- 判断（header が黙っている所・読み方が二つある所）:
  - build は最初に `lists` を全部 0 にする。誤り（EINVAL）の時も counts は 0 のまま。I と SI の slice は counts 0 で 0 を返す（DPB が空でも）。SP は P と同じに作る。slice_type が 4 を越えたら EINVAL。
  - 引数の誤り（header に無いが落とさず EINVAL）: NULL、`max_frame_num` が 0 か 65536 を越える、`frame_num >= max_frame_num`、`num_ref_idx_active` が 0 か 32 を越える（使う list だけ）、`modification_count > H264SW_MAX_MODIFICATIONS`、idc が 4 以上の operation（idc 3 は loop の終わり、それ以後は見ない）。
  - **header の約束と仕様の食い違い**: header は「non-existing の frame は他と同じように list に入る」と書く。8.2.4.2.3 は B の list で pic_order_cnt_type 0 の non-existing の frame を list0・list1 に含めない。build は SPS の type を持たないので header のとおり含めた（P は仕様も含む）。
    type 0 の gap の frame は直前の POC を持つので、gap のある type 0 の B stream では仕様と list の中身が変わる。仕様に合わせるなら build に type を渡す header の変更が要る（Q1 の判断）。conforming な stream は non-existing の frame を参照しないが、list の位置はずれる（encoder は modification を使う、8.2.4.2.3 NOTE 1）。
  - 初期の list の cut（8.2.4.2）は、list1 = list0 の比較と入れ替え（8.2.4.2.3 の 3）の後に行う（8.2.4.2.1〜5 が作った list に対して 8.2.4.2 が切る）。cut の後で比べる実装と、num_ref_idx_active[1] が 1 の時に list1[0] が違う。
  - POC が現在と等しい short-term は、list0 では「現在より小さい」の後の昇順の部分、list1 では「現在より大きい」の後の降順の部分に入る（式の「残り」）。同じ key の frame は DPB の buffer の順（安定）。
  - modification: `abs_diff_pic_num_minus1 >= MaxFrameNum`（7.4.3.1 の範囲外）は picture を指さない扱い（missing）で picNumPred は動かさない。operation は最大でも list の長さまで（list が満杯になったら以降を飛ばす）。picNumLX が DPB の short に無い（non-existing を含む DPB の entry は在れば受ける）時は missing を挿入し、dedupe は何も外さない。
    dedupe は short の operation で long-term の entry を、long の operation で short の entry を外さない（PicNumF・LongTermPicNumF が一致しない値になる）。
  - missing の stand-in: 同じ list で missing でない最初の entry、無ければ他の list の最初、無ければ DPB の frame（non_existing でない frame を先に、無ければ最初の frame）。stand-in の `long_term`・`poc`・`pic_num` はその entry からの写しで `missing` だけ 1。DPB の frame が参照でない（UNUSED）時は `long_term` 0・`pic_num` 0。
    stand-in の選びは両方の list の modification が済んでからで、list0 の選びが list1 の stand-in に影響しない。
  - `h264sw_reflist_find` は missing の位置を数えない（stand-in は stream が指した frame でない）。stand-in だけが持つ frame は -1 を返すので、direct の側は「どの entry にも無い」として concealed に数える。count が配列を越えていれば 32 で打ち切る。
- 未実施: QEMU・実機の試験（範囲外）、decoder との結合、field・MBAFF の list（範囲外）、規約の全文の見直し Phase（WS の最後）。
