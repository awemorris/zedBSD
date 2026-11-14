<!-- awesome-plan project=zedbsd record=ws338-p09 -->

# ws338-p09 thread

- 親: [WS338](../ws.md)、設計 [design.md](../design.md)。
- 状態: planned（2026-11-15 p001 で作成）
- 依存: p008 が main に
- 担当の想定: Opus 5.5（phase-runner-high を推す）

## 目的と範囲（design §7）

- 呼び手の thread が P（ring が満ちた時と待ちの間は R・D も取る）、N − 1 本の worker が R・D。N の既定は min(online の CPU, 4)。
- R は P が記録した intra の隣（同じ slice で available な A・B・C・D）の R だけを待つ。D(r) は行 r と r＋1 が済み（R か隠蔽）で、D(r−1) の後（D は 1 本の鎖）。隠蔽の要る行の D は picture の終わりの隠蔽の後。
- 同期: 進みの値は全部 mutex の中、通知は待つ者がいる時だけかまとめて、進みの値は cache line を分ける。`media-os.h`・`media-os-posix.c`・`media-os-win32.c` に `media_os_cond_wait`・`media_os_cond_signal`・`media_os_cpu_count` を足す。
- 口: `h264sw_decoder_create` に thread の数と `const struct media_threads *`（NULL 可）。backend は engine から非公開の口で受ける（公開の `media.h`・`media-decoder.h`・exports.map は変えない）。thread の名前 "h264-decode-1"〜"-3"。speed-bench・conform・decode-stream に thread の数の引数（`plan/tools/h264sw/` の変更は Q1 の許可、または `plan/ws338/tests/` に写しを置く）。
- 寿命: reset・drop は取り消して静止を待つ（thread は残す）、destroy は静止 → drop → join。起動の失敗は起動できた本数で続ける（0 なら N = 1）。

## 受け入れ

- 1〜4 thread で出力が bit 単位で同じ（適合・run-stream・outputs.sh）。ThreadSanitizer の build で run-stream と適合の一部（Q1 と決める）の報告 0。aarch64（QEMU の user mode）で N = 4 の run-stream、host を taskset で 1〜2 CPU に絞った N = 4 の run-stream が PASS。
- host（taskset で別々の物理 core、1 thread と交互に 3 回）: 4 thread で 1 thread の 2.4 倍以上、2 thread で 1.6 倍以上。p008 の P の割合からの上限の 80% に満たなければ理由を記録。
- Windows の build（`apps/browser-win32`、x86_64 と arm64、-Werror）の warning 0（道具が無ければ未実施と書き Q1 へ）。

## 変えてよい物

- `userland/desktop/libmedia/h264sw-*.c/.h`（backend を含む）、`media-os.h`・`media-os-posix.c`・`media-os-win32.c`、`decoder.c`・`engine.c` の中の非公開の道（公開の header を変えない）、`plan/ws338/tests/`、この phase.md と ws.md の行。

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
