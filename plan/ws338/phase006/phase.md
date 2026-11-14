<!-- awesome-plan project=zedbsd record=ws338-p06 -->

# ws338-p06 transform・mb の書き直し

- 親: [WS338](../ws.md)、設計 [design.md](../design.md)。
- 状態: planned（2026-11-15 p001 で作成）
- 依存: p002
- 担当の想定: Sonnet 5.5 high（phase-runner-low）

## 目的と範囲（design §5 の p006）

- `h264sw-transform.c`・`h264sw-mb.c` だけ。口（header）と `h264sw-recon.c` を変えない。
- (1) 逆変換（8.5.12・8.5.13）の列の pass を「列を lane にした行の演算」に書き、行の pass と合わせて vector 化。(2) DC だけの block の速い道。範囲が保証される所の clamp を外す（壊れた stream で未定義の動作をしない範囲は保つ。保証の根拠を comment に）。(3) `h264sw_add_residual` を行の loop に。(4) `h264sw_mb_locate`（6.4.12）: macroblock の内側の 4x4・8x8 の neighbour は file の中の表。
- 確かめに足す物: remark（幅 4・8 の loop）。transform＋mb の module の時間が前の 0.6 倍以下。UBSan（run-unit・run-stream・適合は ASan・UBSan で build される）の報告 0。

## 変えてよい物

- `userland/desktop/libmedia/h264sw-transform.c`、`h264sw-mb.c`、この phase.md と ws.md の行。

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
