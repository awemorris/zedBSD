<!-- awesome-plan project=zedbsd record=ws326-p021 -->

# ws326-p021 decoder の全体の流れと最初の全体の試験

- 親: [WS326](../ws.md)、設計 [design.md](../design.md)。
- 状態: planned（2026-10-30 p001 で作成）。
- 依存: p002〜p020（依存の Phase の実装が main に入ってから始める）。
- 担当の想定: Sonnet 5.5 high の subagent を 1 回起動（ws.md のユーザーの決定 2）。header は p001 の Opus の設計で、変えない。

## 読む物

1. AGENTS.md の §4（担当の共通の規則）・§7・§9・§10、plan/agents/protocol.md
2. plan/coding-style.md の全文（ANSI C: 宣言は関数の先頭、`for` の中で宣言しない、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、など。§14 の checklist で自分の diff を見直す）
3. plan/ws326/design.md の §1（出典の規則）・§2（決定）・§4（所有と寿命）・§5（単体試験の作り方）
4. `userland/desktop/libmedia/h264sw.h`（誤りの値の約束と上限）
5. 実装する header: `userland/desktop/libmedia/h264sw-decoder.h`（各関数の comment が仕様。struct の field の意味と単位も header に）。使う側の header（`#include` の先）も読む。
6. 仕様: ITU-T H.264 (08/2021) の 7.4.1.2.3・7.4.1.2.4、8.1、8.2（全体）、C.4、design.md §2 の誤りと開始の方針。PDF `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf`（SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`、git に入れない）。印刷の頁 N = PDF の頁 N + 22。表と行列は PDF の頁の画で読む（Read の tool の `pages`、1 回 20 頁まで）。`pdftotext -layout` の文字は列が崩れることがある。

## 作る物

- `userland/desktop/libmedia/h264sw-decoder.c`: 骨格（各関数の `ENOSYS` など）を header の仕様どおりの実装に置き換える。static の補助関数は自由（規約どおり前方宣言）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- 全体の試験の program `plan/ws326/tests/stream/decode-stream.c`（unit の外に置く: run-unit.sh の all は引数なしで流すため）: 引数の Annex B の file を h264sw_nal_find で分けて h264sw_decoder_nal に与え（EBUSY の前に出力を取る）、最後に flush、出力の frame を crop して I420 で frame ごとの MD5 を出す。
- 試料: plan/ws083/tests/streams/*.h264（6 本）、plan/ws236/tests/*.h264（2 本）、host の ffmpeg（libx264）で作る 352x288 の短い stream（CAVLC baseline、CABAC main の B、high の 8x8 と cqm、複数 slice、weighted の P）。
- 期待: host の `ffmpeg -i <file> -f rawvideo -pix_fmt yuv420p` の frame ごとの MD5（副の oracle、design.md §7）。全 frame が一致。
- 誤り: stream を任意の位置で切る・byte を壊す（固定の seed で 100 通り）で落ちない（ASan・UBSan）、次の IDR から正しく戻る。
- 開始: IDR の前の P を捨てる、reset の後。
- 共通: 誤りの入力（範囲外・途中で切れた・巨大な値）で落ちず、header に書かれた誤りを返す。ASan・UBSan の報告が 0。

## 注意

- 最初の全体の試験なので module の間の誤りがここで出る。この Phase に限り、他の module の h264sw-*.c の修正をしてよい（header は不可）。直した file・関数・理由を phase.md の結果に 1 つずつ書く。Q1 は Opus の担当（phase-runner）を推す。
- この試験の runner は `plan/ws326/tests/run-stream.sh`（新しく作る、decode-stream.c を ASan・UBSan で build、出力は build/tmp の fresh の directory、最後の行 `run-stream: PASS`）。ffmpeg が無い時は skip と書く。確かめの command に `timeout 1800 sh plan/ws326/tests/run-stream.sh` を足す。

## 変えてはいけない物

- `userland/desktop/libmedia/h264sw*.h`（全部）。header の不備・矛盾・足りない口を見つけたら、直さずに止めて Q1 に返す: どの header のどの関数か、何が足りないか、案の差分。
- 担当の module 以外の `h264sw-*.c`（他の Phase の物）。既存の libmedia の file（bits.c・h264.c・vkvideo.c・decoder.c など）、Makefile、`plan/ws326/tests/run-unit.sh`・`check.h`・`bitwriter.h`・`check-skeleton.sh`（不備は Q1 へ）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない。
- 上の「注意」がこの Phase に明示に許す物だけは、この節の限りでない。

## 確かめの command（worktree の root で）

- `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`（header が p001 の物から変わっていない、cc と clang で warning 0）。
- zedBSD amd64: `timeout 1200 make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libmedia.so` → exit 0、出力に warning が 0。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 自分の diff を coding-style の §14 の checklist で見直した。表の block ごとに `Values referenced: ITU-T H.264 (08/2021) Table ...` の複数行の comment がある。source の comment・log に WS・Phase の番号や plan/ への参照が無い。
- `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と結果・未実施・残りを Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、実行した command、PASS の数、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が受け入れを見て決める）。

## p020 から引き継ぐ事（2026-10-30 Q1）

deblocking は chroma の QP に `picture->chroma_qp_offset[0]`・`[1]` を全 macroblock で使う（context に slice ごとの PPS の offset が無い）。1 つの picture の slice が chroma_qp_index_offset の違う PPS を指す stream があり得るか（7.4.1.2.1 などの制約）を統合で確かめ、あり得るなら macroblock ごとの値を持たせる直しを Q1 に返す。

## p006 から引き継ぐ事（2026-10-30 Q1）

- memory_management_control_operation 5 の picture は、`h264sw_dpb_store` を呼ぶ前に `frame->frame_num = 0` にする（7.4.3。DPB の項目の frame_num は `current->frame_num` から写る。decoder.h の手順 6 は POC の写しだけを書いている）。
- gap の非存在の frame の POC（type 0）は DPB の中の最も新しい frame の POC か 0（p006 の判断、phase006/phase.md の結果）。適合の試験の gap の stream（MR9_BT_B など）で出力の順を確かめる。
