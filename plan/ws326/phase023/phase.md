<!-- awesome-plan project=zedbsd record=ws326-p023 -->

# ws326-p023 libmedia の backend と backend の切り替え

- 親: [WS326](../ws.md)、設計 [design.md](../design.md)。
- 状態: planned（2026-10-30 p001 で作成）。
- 依存: p021（p022 を推す）（依存の Phase の実装が main に入ってから始める）。
- 担当の想定: Sonnet 5.5 high の subagent を 1 回起動（ws.md のユーザーの決定 2）。header は p001 の Opus の設計で、変えない。

## 読む物

1. AGENTS.md の §4（担当の共通の規則）・§7・§9・§10、plan/agents/protocol.md
2. plan/coding-style.md の全文（ANSI C: 宣言は関数の先頭、`for` の中で宣言しない、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、など。§14 の checklist で自分の diff を見直す）
3. plan/ws326/design.md の §1（出典の規則）・§2（決定）・§4（所有と寿命）・§5（単体試験の作り方）
4. `userland/desktop/libmedia/h264sw.h`（誤りの値の約束と上限）
5. 実装する header: `userland/desktop/libmedia/h264sw-backend.h`（各関数の comment が仕様。struct の field の意味と単位も header に）。使う側の header（`#include` の先）も読む。
6. 仕様: ITU-T H.264 (08/2021) の design.md §2（backend の順・遅れた拒否の切り替え）、h264sw-backend.h の comment。PDF `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf`（SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`、git に入れない）。印刷の頁 N = PDF の頁 N + 22。表と行列は PDF の頁の画で読む（Read の tool の `pages`、1 回 20 頁まで）。`pdftotext -layout` の文字は列が崩れることがある。

## 作る物

- `userland/desktop/libmedia/h264sw-backend.c`: 骨格（各関数の `ENOSYS` など）を header の仕様どおりの実装に置き換える。static の補助関数は自由（規約どおり前方宣言）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- h264sw-backend.c の全 op（h264sw-backend.h の comment どおり）。
- decoder.c: 表を { vkvideo, h264sw, aac } にし（libavcodec は WS318 p002 で h264sw の後ろに入る）、open の DEVICE・PROFILE・BUSY で次へ、send の ENOTSUP・ENODEV・EBUSY で切り替え（受け入れ 0 なら同じ packet から、後なら drain して key frame まで捨て、数を counter と media_log に）。key frame が来ないまま終わった時だけ誤り。
- host 試験: plan/tools/media の host-codec の形で、mediafile から MP4・MKV・TS の H.264 を libmedia の decoder で decode（Vulkan Video の無い host では vkvideo が DEVICE で拒み、h264sw が開く）。frame 数と最初・最後の frame の MD5 を ffmpeg と比べる。
- 切り替えの試験: 偽の backend の表（試験の build だけ）で、受け入れ 0 の拒否・受け入れ後の拒否と key frame までの捨て・key frame が来ない終わりの 3 通り。
- QEMU: Video Player で H.264 を再生（boot-test と guest の操作、Q1 の割り当て）。
- 共通: 誤りの入力（範囲外・途中で切れた・巨大な値）で落ちず、header に書かれた誤りを返す。ASan・UBSan の報告が 0。

## 注意

- この Phase で変えてよい既存の file: decoder.c（表と切り替え）、media-private.h（要れば）、h264sw-backend.c、host の試験の file（plan/ws326/tests/ の下に新しく）。変更は最小に。既存の run-host-codec.sh を壊さない。

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

## 結果（2026-10-31、S-p023）

実装: `userland/desktop/libmedia/h264sw-backend.c`（header の仕様どおり。出た frame を確保済みの pool の media_picture へ即複写、queue は H264SW_OUTPUT_MAX、queue が溢れる packet の残りは backend が所有する複写に溜めて receive の中で decoder に渡す、`refused`（ENOTSUP）は send が 1 回だけ返す、ENOMEM は flush まで続く）と `userland/desktop/libmedia/decoder.c`（表は vkvideo・h264sw・aac。open の DEVICE・PROFILE・BUSY で次へ、全部だめなら最初の問題。send の ENOTSUP・ENODEV・EBUSY で次の backend が開けば切り替え: 受け入れ 0 なら同じ packet を新しい backend へ、後なら旧 backend を drain して held の picture に溜め、key frame まで捨て、数を media_log に。key frame が来ないまま終端なら元の誤りを返す）。header は変えていない。
補足: `h264sw_decoder_create` が ENOSYS（骨格）の間は backend の open が MEDIA_PROBLEM_FORMAT を返し、次の backend へ回る（既存の host 試験 plan/tools/media/run-host-codec.sh の libavcodec への経路が壊れない）。run-host-codec.sh には `h264sw-*.c` を link に足した（decoder.c が media_h264sw_ops を参照するため）。

実行した command と結果（worktree の root）:

- `sh plan/ws326/tests/backend/run-backend.sh` → test-backend PASS（202 checks）、test-switch PASS（112 checks）、run-backend: PASS（ASan・UBSan 報告 0）。test-backend は decoder の代役（scripted）、test-switch は 3 つの偽の backend の表で decoder.c を試験: open の順と最初の問題、受け入れ 0 の拒否（ENOTSUP・ENODEV・EBUSY、他の誤りは切り替えない、後続が無ければ拒否のまま）、受け入れ後の拒否（drain した picture が先に出る、key frame まで捨てる、捨てた数の log、拒否された packet が key frame の場合）、key frame が来ない終端、flush、2 段の切り替え。
- `BASE=fd1d6f88c sh plan/ws326/tests/check-skeleton.sh` → check-skeleton: PASS。
- amd64 の libmedia.so（確かめの command の make）→ exit 0、log に warning 0（build/tmp/libmedia-build.log）。
- `git diff --check` → 出力なし。
- `sh plan/tools/media/run-host-codec.sh` → run-host-codec: PASS（8 file、H.264 は骨格の間 libavcodec へ回る）。

未実施:

- 実 stream の decode（`sh plan/ws326/tests/backend/run-host-decode.sh`: MP4・MKV・TS の H.264 を ffmpeg の出力と比べる）。道具は書いて ffmpeg の参照の生成まで確かめたが、decoder が骨格のため open が失敗する（DEVICE）。p021 の取り込み後に流す。
- QEMU の Video Player での再生（Q1 の割り当て）。
- 規約の §14 の checklist は自分で見直したが、試験の C は production の規約ほど厳密に整えていない。
