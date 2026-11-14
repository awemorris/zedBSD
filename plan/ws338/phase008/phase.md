<!-- awesome-plan project=zedbsd record=ws338-p08 -->

# ws338-p08 段の分離（1 thread のまま）

- 親: [WS338](../ws.md)、設計 [design.md](../design.md)。
- 状態: planned（2026-11-15 p001 で作成）
- 依存: p004〜p007 が main に
- 担当の想定: Opus 5.5（phase-runner-high を推す）

## 目的と範囲（design §6）

- decoder の流れを P（parse・動きの導出・検査）、R（画素の再構成、誤りを返さない）、D（行の範囲の deblock）に分け、1 thread で P → R → D を行の ring で回す。
- P に移す物（design §6 の表）: `recon_motion`（mvpred・P_Skip・direct）、intra の mode の導出、`mb->nonzero`・`intra4x4_mode` の記録、R が今返す誤りの検査の全部（recon_predict_block・recon_vector・recon_partition）、隠蔽の数（`ctx->concealed`）。
- macroblock の仕事の記録: 係数（int16）と mask、PCM、partition ごとに解いた参照（frame の pointer・missing）と weight の係数の値、intra の mode。slice header・`struct h264sw_weights` への pointer を持たない。
- picture の終わり: R の全部 → `decoder_conceal` → 残りの D → DPB。slice の誤りでは ring を捨てず、誤った entry だけ −1。ring を捨てるのは drop の時だけ。
- 口の互換: `h264sw_recon_mb`（動き＋画素）・`h264sw_deblock_picture`・`struct h264sw_slice_ctx` の今の field を残す（WS329 の encoder が使う）。互換で残せない所が出たら止めて Q1 に返す。
- p005・p006 で残した recon.c の中間の buffer（予測を weight の出力へ直に、chroma の residual）もここで。
- P の段の時間の割合を計測して結果に書く（p009 の目標の計算に使う）。

## 受け入れ

- 出力が bit 単位で同じ（下の確かめ全部と p002 の outputs.sh）。1 thread の速さが前の 0.95 倍以上（host）。header の変更の一覧と理由を Q1 に送り、新しい BASE で check-skeleton PASS。

## 変えてよい物

- `userland/desktop/libmedia/h264sw-*.c`・`h264sw-*.h`（backend を除く）、単体試験の追従（Q1 の許可、件数を減らさない）、この phase.md と ws.md の行。

## 読む物

1. AGENTS.md の §4・§7・§9・§10、plan/agents/protocol.md
2. plan/coding-style.md の全文（ANSI C、宣言は関数の先頭、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、§14 の checklist で自分の diff を見直す）。拡張は `__inline`・`__attribute__` の綴りで。
3. [design.md](../design.md) の §1（決まった事・出典）と、この Phase の節。[../../ws326/design.md](../../ws326/design.md) の §1（出典の規則: FFmpeg・openh264・JM・x264 などの code・comment・構造・名前を写さない。仕様と自分の profile から導く）・§4（所有と寿命）
4. 仕様: ITU-T H.264 (08/2021)。PDF は ITU の公開の URL（`T-REC-H.264-202108-S!!PDF-E`）から自分の worktree の `build/sources/specs/` へ（git に入れない）、SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`。印刷の頁 N = PDF の頁 N + 22。

## 確かめ（worktree の root で。各 command に時間の上限）

- 前（この Phase の変更の前の tree）と後で同じ: `sh plan/tools/h264sw/run-unit.sh all`（各 test の最後の行、件数）、`timeout 3600 sh plan/tools/h264sw/run-stream.sh`、`timeout 1200 sh plan/tools/h264sw/backend/run-host-decode.sh`、p002 の出力の照合（`plan/ws338/tests/` の壊した stream を含む全出力の MD5 の一覧、前後で diff 0）。
- 適合: 初回は `H264SW_CONFORMANCE_DIR=$PWD/build/tmp/h264sw-conformance/zips timeout 7200 sh plan/tools/h264sw/conformance/fetch.sh`、それから `H264SW_CONFORMANCE_DIR=$PWD/build/tmp/h264sw-conformance/zips timeout 7200 sh plan/tools/h264sw/conformance/run.sh` → `conform-run: PASS (101 in, 103 out)`、results.txt が前と同じ。
- `BASE=<Q1 が示す基点> timeout 300 sh plan/tools/h264sw/check-skeleton.sh`（header を変えない Phase は今の BASE b753848bf2 で PASS）。
- zedBSD の libmedia（自分の worktree の build/、初回は `make CONFIGMK=config/config.<機種>.mk llvm-toolchain-cached` と `sysroot`）: `timeout 2400 make CONFIGMK=config/config.<機種>.mk -j4 build/<機種>/packages/libmedia/.stamp`（<機種> は x86_64・rpi5・vf2）→ 各 exit 0、warning 0（-O2 -Wall -Wextra -Werror が main に入った後）。
- 速さ: p002 の host の計測（zedBSD の clang 23 -O2、前後を同じ時に続けて、同じ stream の file）。
- `git diff --check`。

## 変えてはいけない物

- この Phase の「変えてよい物」の外の source。HAL・toolchain・Makefile・`mk/`・`sources.mk`・共有の build/（AGENTS.md §7）。新しい `.c` を足さない。
- 他の WS の記録、`plan/tools/h264sw/`（不備は Q1 へ。この WS の試験は `plan/ws338/tests/`）。
- rm などの削除をしない（消す物は path と理由を Q1 へ）。
- header の不備・足りない口を見つけたら（header を変えない Phase）、直さずに止めて Q1 に返す: どの header のどの関数か、何が足りないか、案の差分。

## 終わりの条件

- 上の確かめが全部 PASS（出力を自分で読む）。自分の diff を coding-style §14 で見直した。source の comment・log に WS・Phase の番号や plan/ への参照が無い。表の block に `Values referenced: ITU-T H.264 (08/2021) ...`。
- `git commit -m WIP -- <変えた path>`（push しない）。SHA・command と結果・未実施・残りを Q1 へ。
- この phase.md に「結果」の節（日付は host の `date`、command、数、未実施）を足し、ws.md の表の行を「実装済み・試験 PASS」に（cleared は Q1）。
