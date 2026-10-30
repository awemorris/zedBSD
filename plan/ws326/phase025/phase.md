<!-- awesome-plan project=zedbsd record=ws326-p025 -->

# ws326-p025 規約の全文の見直し

- 親: [WS326](../ws.md)、設計 [design.md](../design.md)。
- 状態: planned（2026-10-30 p001 で作成）。
- 依存: p023・p026（と速さの Phase）（依存の Phase の実装が main に入ってから始める）。

## 読む物

1. AGENTS.md の §4（担当の共通の規則）・§7・§9・§10、plan/agents/protocol.md
2. plan/coding-style.md の全文（ANSI C: 宣言は関数の先頭、`for` の中で宣言しない、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、など。§14 の checklist で自分の diff を見直す）
3. plan/ws326/design.md の §1（出典の規則）・§2（決定）・§4（所有と寿命）・§5（単体試験の作り方）
4. `userland/desktop/libmedia/h264sw.h`（誤りの値の約束と上限）
5. 仕様: ITU-T H.264 (08/2021) の plan/coding-style.md の全文、guardrail の出典と開発の管理情報の節。PDF `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf`（SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`、git に入れない）。印刷の頁 N = PDF の頁 N + 22。表と行列は PDF の頁の画で読む（Read の tool の `pages`、1 回 20 頁まで）。`pdftotext -layout` の文字は列が崩れることがある。

## 作る物

- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- WS が足した C（h264sw*.c・h264sw*.h・vcodec*.c・vcodec*.h・decoder.c の変更・試験の C）を全文と照らし、書き方だけを直す（動作を変えない）。表の block ごとの出典の comment の形を確かめる。
- 共通: 誤りの入力（範囲外・途中で切れた・巨大な値）で落ちず、header に書かれた誤りを返す。ASan・UBSan の報告が 0。

## 変えてはいけない物

- `userland/desktop/libmedia/h264sw*.h`・`vcodec*.h`（全部）。header の不備・矛盾・足りない口を見つけたら、直さずに止めて Q1 に返す: どの header のどの関数か、何が足りないか、案の差分。
- 担当の module 以外の `h264sw-*.c`（他の Phase の物）。既存の libmedia の file（bits.c・h264.c・vkvideo.c・decoder.c など）、Makefile、`plan/ws326/tests/run-unit.sh`・`check.h`・`bitwriter.h`・`check-skeleton.sh`（不備は Q1 へ）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない。
- 上の「注意」がこの Phase に明示に許す物だけは、この節の限りでない。

## 確かめの command（worktree の root で）

- `BASE=92ca2d0e8 timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`（header が p026 の物から変わっていない、cc と clang で warning 0。WS329 p002 が header を移したらその commit）。
- zedBSD amd64: `timeout 1200 make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libmedia.so` → exit 0、出力に warning が 0。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 自分の diff を coding-style の §14 の checklist で見直した。表の block ごとに `Values referenced: ITU-T H.264 (08/2021) Table ...` の複数行の comment がある。source の comment・log に WS・Phase の番号や plan/ への参照が無い。
- `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と結果・未実施・残りを Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、実行した command、PASS の数、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が受け入れを見て決める）。

## p003 から持ち越した物（2026-10-30 Q1）

- `h264sw-ps.h` の `struct h264sw_ps_store` の comment の「約 50 KiB」は実測 128768 byte（sps 1424・pps 320 × 個数）。header は実装の Phase の間は変えない（check-skeleton の BASE の照合）ので、この見直しの Phase で直す。

## 結果（2026-10-31 P1、q1778-i01）

実装済み・試験 PASS（cleared は Q1 が受け入れを見て決める）。直したのは書き方だけ（動作・API・ABI・error の値は同じ）。header は変えていない。

- 進め方: `plan/tools/style-check.py` を WS326 の source 全部（h264sw-*.c・vcodec-*.c と decoder.c の WS326 の部分）と試験の C に走らせ、出た行を直し、conformance-reviewer の helper 8 人（source 3・試験 5、file が重ならない組）が全文を読んで機械で見えない規則（`return f(...)`、引数の中の入れ子の呼び出し、条件の中の呼び出し、`?:`、comment の言い換え）を直した。
- source の直し（style-check は 0 件になった。前は blank-after-brace 約 50・paragraph-comment 約 70・call-in-condition 7）: decoder.c 7、h264sw-backend.c 9、cabac.c 1、cabac-se.c 10、cavlc.c 4、deblock.c 8、decoder.c(h264sw) 3、direct.c 3（入れ子の呼び出しを `ref_bc` に）、dpb.c 5（条件の中の呼び出し 2 を変数に、`h264sw_dpb_make_room` を成功の return で終わる形に）、intra.c 7、macroblock.c 約 25、mb.c 3、mvpred.c 2、poc.c 約 5（`poc_fits` を条件の外に）、ps.c 3、recon.c 約 25（入れ子の呼び出し 1 を `luma_qp` に）、reflist.c 約 15（`return f(...)` 3 を変数経由に、条件の中の呼び出し 1）、slice.c 2、transform.c 約 14（入れ子の呼び出し 12 を `half` に）、weight.c 2、vcodec-cabac.c 1。nal・cabac-init・frame・interp・tables・vcodec-bitreader・vcodec-nal は直す所なし。
- 試験の C（style-check は残り `?:` の短い対称な選択だけ）: unit 全 file と backend・conformance・speed・stream の C。前方宣言・段落 comment・閉じ括弧の後の空行・条件の中の呼び出し（`CHECK(f(...))` の多くを含む）・`main` の `return check_finish(...)` を直した。`CHECK(` の数と `PASS (N checks)` の文言は同じ。
- 開発の管理情報: source・試験の comment・log の文字列に WS・Phase・Bug・Queue の番号や plan/ への参照は 0（grep）。表の block の出典の comment は既にあった。
- 引数の評価順が未規定だった所（test-dpb.c の `header_mmco` の 4 引数、test-cabac_se.c の `h264sw_cabac_residual_block` の 2 引数）は、gcc の順に合わせて固定した。試験の出力は同じ。clang で乱数の列が変わりうる（試験の期待値は乱数に依らない）。
- 持ち越し（h264sw-ps.h の `struct h264sw_ps_store` の comment の「約 50 KiB」、実測 128768 byte）: 直していない。header を変えると BASE の照合（check-skeleton）が FAIL になるため、Q1 に返す。案: 「(about 50 KiB)」を「(about 126 KiB: 2 SPS 1424 byte x 件数、PPS 320 byte x 件数)」の実測の値に直す、BASE も同じ commit で移す。
- 動作に見える欠陥: なし。注意 1 つ: h264sw-ps.c の `ps_dpb_frames` は `size_mbs` が 0 でない事を呼び出し元の `unsupported` の検査に頼っている（今は 0 で割らない）。
- 実行した確かめ（全部 worktree の root）:
  - `sh plan/ws326/tests/run-unit.sh all` → run-unit: PASS（24 本、各 checks 数は前と同じ。test-reflist 281194 は HEAD の file でも同じ）。
  - `BASE=92ca2d0e8 sh plan/ws326/tests/check-skeleton.sh` → PASS。
  - `sh plan/ws326/tests/run-stream.sh` → PASS（22 本 408 frame が ffmpeg と一致）。
  - `sh plan/ws326/tests/backend/run-host-decode.sh` → PASS。`sh plan/ws326/tests/backend/run-backend.sh` → PASS（test-switch 112 checks）。
  - `H264SW_CONFORMANCE_DIR=/tmp/o-ws326-p022/conformance sh plan/ws326/tests/conformance/run.sh` → `conform-run: PASS (101 in, 103 out)`。
  - `sh plan/tools/media/run-host-codec.sh` → PASS。
  - libmedia.so の build（上の command）→ exit 0・warning 0。`nm -D` の symbol 名（種類と名前）は前後で同じ（アドレスだけ違う）。
  - `git diff --check` → 出力なし。
  - `sh plan/ws326/tests/speed/run-speed.sh` → PASS（high-1080-grain 22.47 fps、high-1080-clean 34.42、main-1080-cavlc 26.65。p024 の記録 24.13 より約 7% 低いが、host は他の担当の試験と同時で、静かな host での再計測は未実施）。
- 未実施: 静かな host での速さの再計測、clang での試験、QEMU・実機の回帰、ASan の無い構成。helper は全 file を 1 行ずつは読んでいない（comment の言い換えと「1 段落に判断 1 つ」の細かい点は、機械の検査と grep の範囲）。
