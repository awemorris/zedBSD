<!-- awesome-plan project=zedbsd record=ws009-p019 -->

# ws009-p019: DOC-54 gpu-drivers.md: driver と node の種類の表

Status: planned（2026-10-30 P1 の設計、q1701。p009 を節ごとに分けた物）
Parent: [WS009](../ws.md)
Approval: 2026-10-27 ユーザーの回答（クリック）「今書く」（DOC-54）。2026-10-30 ユーザー「WS009、WS296・WS297は標準廉価実装手順を適用してphaseを作り替え、今から実行してOKです。ただ、1 WSずつ実施します。」
実装の担当: Sonnet 5.5 high（1 回の起動）
依存: なし（骨格 5fedf634d と照合の script が main にあること）。p010〜p021 は互いに独立で、並べて起動してよい
骨格の基点: 5fedf634d

## 1. 読む物

- AGENTS.md の §4（担当の共通の規則）・§7（禁止）・§10（管理情報を書かない）。
- [docs/style.md](../../../docs/style.md) と [gpu-doc-rules.md](../gpu-doc-rules.md)（書き方・形・確かめ・終わりの条件の共通の規則。この phase.md と同じ重さの手順）。
- 自分の file の骨格の全文（各節の `<!-- spec: -->` が書く事の仕様）と [gpu.md](../../../docs/reference/gpu.md) の骨格（共通の規約の spec）。
- `grep -rln drv_gpu_register src/drivers/gpu`（gpu.c を除く 11 file）と各 file の ops の capabilities の値（macro は数値まで辿る）
- bit を後から足す所: amdrdna/runtime.c・amdrdna/node.c、nvrtx/node.c、ghci/ghci.c・ghci/vk/vk-node.c、ほか grep `capabilities |=`・`capabilities =`
- i915 の display の 3 つの道: i915/display.c・i915/bootfb.c・i915/capture.c
- display の flag（`GPU_DISPLAY_FIFO`・`VIRTUAL_CLOCK`・`ATOMIC_MODE_PRESENT`・`HAS_CURSOR`・`HAS_OVERLAYS`）を立てる所（grep）
- config/drivers/ の表と各 driver の build の規則（architecture）
- venus/venus.c `venus_get_info`・venus/transport.c

## 2. 作る物

- `docs/reference/gpu-drivers.md`
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 確かめの項目

- 骨格の全ての `PENDING` と `<!-- spec: -->` を埋めた（script の `--final`）。
- 要求の節ごとに `Requires:`・`Errors:`・`Sources:` の形（gpu-doc-rules.md 2 節の 6）。
- 構造体の表の Direction と Meaning を全行。定数の表の Meaning を全行。
- 自己の照合の表と、`Errors:` から 3 つの読み直し（gpu-doc-rules.md 4 節）。
- 出典にする試験の見当: この file は要求の節を持たない（Sources・Errors の規則は掛からない）。表の Source の列に driver の file への link。

## 4. 注意

- 行は driver でなく node の種類。bit は名前で書く。
- runtime の bind が `drv_gpu_register` の前か後かを、呼ぶ順を読んで決める。後なら、先に開いた program が後の GPU_GET_INFO で新しい bit を見ることを書く。読み切れなければ未確認として結果の節に。
- bit と振る舞いの差（venus の JOB）は事実として書き、不具合と呼ばない。

## 5. 変えてはいけない物

- 2 節の file 以外の全部（gpu-doc-rules.md 5 節）。見出し（H1〜H4）と機械の列。照合の script。header・source・HAL・toolchain・共有の build。
- 不備を見つけたら直さずに止め、file・場所・足りない物を報告する。

## 6. 確かめの command

```sh
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 5fedf634d --final docs/reference/gpu-drivers.md
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 5fedf634d
timeout 120 /home/awe/zedBSD-claude1/build/NoctLang/build-static/noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture
git diff --check
```

## 7. 終わりの条件

gpu-doc-rules.md 6 節。commit は `git commit -m WIP -- docs/reference/gpu-drivers.md plan/ws009/phase019/phase.md plan/ws009/ws.md`。

## 結果

（未実施）
