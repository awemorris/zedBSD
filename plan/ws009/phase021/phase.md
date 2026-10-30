<!-- awesome-plan project=zedbsd record=ws009-p021 -->

# ws009-p021: DOC-54 gpu-examples.md: compile できる例

Status: planned（2026-10-30 P1 の設計、q1701。p009 を節ごとに分けた物）
Parent: [WS009](../ws.md)
Approval: 2026-10-27 ユーザーの回答（クリック）「今書く」（DOC-54）。2026-10-30 ユーザー「WS009、WS296・WS297は標準廉価実装手順を適用してphaseを作り替え、今から実行してOKです。ただ、1 WSずつ実施します。」
実装の担当: Sonnet 5.5 high（1 回の起動）
依存: なし（骨格 4dfcce008 と照合の script が main にあること）。p010〜p021 は互いに独立で、並べて起動してよい
骨格の基点: 4dfcce008

## 1. 読む物

- AGENTS.md の §4（担当の共通の規則）・§7（禁止）・§10（管理情報を書かない）。
- [docs/style.md](../../../docs/style.md) と [gpu-doc-rules.md](../gpu-doc-rules.md)（書き方・形・確かめ・終わりの条件の共通の規則。この phase.md と同じ重さの手順）。
- 自分の file の骨格の全文（各節の `<!-- spec: -->` が書く事の仕様）と [gpu.md](../../../docs/reference/gpu.md) の骨格（共通の規約の spec）。
- 着手の時に gpu-doc-rules.md 1 節の `git log 4dfcce008..HEAD -- ...` で基点の後の UAPI の変化を見る。
- include/uapi/gpu*.h（例で使う要求の構造体と comment）
- gpu.md・gpu-submission.md・gpu-display.md の骨格の spec（例が示す規約）
- userland/tests/gpu-admission・gpu-recovery・display-control・ghci-display の main.c（呼び方の参考。写さず、自分で書く）
- 例の中で独自の名前に `GPU_`・`gpu_` を付けない（gpu-doc-rules.md 2 節の 10）

## 2. 作る物

- `docs/reference/gpu-examples.md`
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 確かめの項目

- 骨格の全ての `PENDING` と `<!-- spec: -->` を埋めた（script の `--final`）。
- 表の Meaning の列（あれば）を全行。
- 節ごとの主張の表と、3 つの読み直し（gpu-doc-rules.md 4 節の要求の節の無い file の形）。
- 出典にする試験の見当: この file は要求の節を持たない。Where the tests are の節に userland/tests への link。

## 4. 注意

- 各例は ```c の block で、1 行目を `/* example: NAME */`（NAME は info・storage・submit-wait・display）。include は `<uapi/gpu.h>` などと POSIX の header。
- 使う構造体ごとに `_Static_assert(sizeof(struct ...) == N, ...)`。N は構造体の表の大きさ。
- submit-wait の stream は gpu-op.h の命令から最小の物を作るか、bytes を呼び手が用意する関数に分ける。動かす試験はしない（compile だけ）と文書に書かない（文書は例の性質だけを書く）。
- 確かめに `--examples` を足す（3 節）。sysroot の無い target は script が飛ばして表示する。どの target で compile したかを結果の節に書く。

## 5. 変えてはいけない物

- 2 節の file 以外の全部（gpu-doc-rules.md 5 節）。見出し（H1〜H4）と機械の列。照合の script。header・source・HAL・toolchain・共有の build。
- 不備を見つけたら直さずに止め、file・場所・足りない物を報告する。

## 6. 確かめの command

```sh
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --examples --final docs/reference/gpu-examples.md
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008
timeout 120 /home/awe/zedBSD-claude1/build/NoctLang/build-static/noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture
git diff --check
```

## 7. 終わりの条件

gpu-doc-rules.md 6 節。commit は `git commit -m WIP -- docs/reference/gpu-examples.md plan/ws009/phase021/phase.md plan/ws009/ws.md`。

## 結果

（未実施）
