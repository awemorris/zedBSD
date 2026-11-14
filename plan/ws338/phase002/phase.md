<!-- awesome-plan project=zedbsd record=ws338-p02 -->

# ws338-p02 host の計測の道具と基準

- 親: [WS338](../ws.md)、設計 [design.md](../design.md)。
- 状態: planned（2026-11-15 p001 で作成）
- 依存: p001
- 担当の想定: Opus 5.5（phase-runner）

## 目的と範囲（design §9 の計測の道、§3）

- `plan/ws338/tests/` に作る（`plan/tools/h264sw/` は変えない。WS の完了で Q1 が移す）:
  1. `speed.sh`: `plan/tools/h264sw/speed/run-speed.sh` と同じ流れ（build → ffmpeg との一致 → 時間 → profile）に、compiler の指定（既定は zedBSD の `build/llvm/bin/clang` 23 を host の target で、`-O2`）、`SPEED_THREADS`（p009 の前は 1 だけ受ける受け皿）、`taskset` の CPU の指定、module ごとの**時間**（perf の self の samples の数と、割合の両方）、stream の file を引数で固定する口（前後の比較で同じ file）。
  2. `outputs.sh`: run-stream の stream と壊した stream（固定の seed）・途中の開始、適合の in の stream の、全出力の frame の MD5 の一覧を 1 file に書く。前後の tree の一覧の diff が 0 で「出力が同じ」とする（run-stream は壊した stream の回復だけを見て、出力の一致を見ないため）。
  3. `zedbsd-bench.sh`: Makefile を使わず、`build/llvm/bin/clang` と自分の worktree の sysroot（`build/<機種>/sysroot`）で、decoder の .c（backend を除く `h264sw-*.c`・`vcodec-*.c`・`bits.c`）と speed-bench を libmedia と同じ flag（`-O2 -fPIC` と arch の flag。§1 の決定の後の package.mk の値を読んで合わせる）で直に cross compile し、zedBSD の program（arm64・x86_64）を作る。
  4. `measure-hw.sh`: 1 本の script（配置 → 実行 → 回収 → 要約）。guest へ `plan/tools/guest/guest.sh` の SSH で program と stream を置き、1 thread の best・median、60 秒以上の連続の decode の平均、4 本の 1 thread の process を同時に流した時の各々の fps、出力の MD5（host の一覧と比べる）、取れれば温度と clock を要約に。mutex・cond の往復の費用を測る小さな program も（libmedia の `media-os-posix.c` の口で）。
- host の基準: `speed.sh` で grain・clean・cavlc・実 A・実 B の 1 thread（zedBSD の clang 23 -O2 と gcc -O2）、profile（module の時間）。`outputs.sh` の基準の一覧。数と一覧を `plan/ws338/measure/` に写す。

## 受け入れ

- 4 本の script が動き、host の基準の数と一覧が記録された。`zedbsd-bench.sh` が arm64・x86_64 の program を warning 0 で作る（実機での実行は p003）。
- 道具の変更で decoder の source を変えていない。

## 変えてよい物

- `plan/ws338/tests/`、`plan/ws338/measure/`、この phase.md と ws.md の行。

## 読む物

1. AGENTS.md の §4・§7・§9・§10、plan/agents/protocol.md
2. plan/coding-style.md の全文（ANSI C、宣言は関数の先頭、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、§14 の checklist で自分の diff を見直す）。拡張は `__inline`・`__attribute__` の綴りで。
3. [design.md](../design.md) の §1（決まった事・出典）と、この Phase の節。[../../ws326/design.md](../../ws326/design.md) の §1（出典の規則: FFmpeg・openh264・JM・x264 などの code・comment・構造・名前を写さない。仕様と自分の profile から導く）・§4（所有と寿命）
4. 仕様: ITU-T H.264 (08/2021)。PDF は ITU の公開の URL（`T-REC-H.264-202108-S!!PDF-E`）から自分の worktree の `build/sources/specs/` へ（git に入れない）、SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`。印刷の頁 N = PDF の頁 N + 22。

## 変えてはいけない物

- この Phase の「変えてよい物」の外の source。HAL・toolchain・Makefile・`mk/`・`sources.mk`・共有の build/（AGENTS.md §7）。新しい `.c` を足さない。
- 他の WS の記録、`plan/tools/h264sw/`（不備は Q1 へ。この WS の試験は `plan/ws338/tests/`）。
- rm などの削除をしない（消す物は path と理由を Q1 へ）。
- header の不備・足りない口を見つけたら（header を変えない Phase）、直さずに止めて Q1 に返す: どの header のどの関数か、何が足りないか、案の差分。

## 終わりの条件

- 上の確かめが全部 PASS（出力を自分で読む）。自分の diff を coding-style §14 で見直した。source の comment・log に WS・Phase の番号や plan/ への参照が無い。表の block に `Values referenced: ITU-T H.264 (08/2021) ...`。
- `git commit -m WIP -- <変えた path>`（push しない）。SHA・command と結果・未実施・残りを Q1 へ。
- この phase.md に「結果」の節（日付は host の `date`、command、数、未実施）を足し、ws.md の表の行を「実装済み・試験 PASS」に（cleared は Q1）。
