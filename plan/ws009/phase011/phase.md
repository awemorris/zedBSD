<!-- awesome-plan project=zedbsd record=ws009-p011 -->

# ws009-p011: DOC-54 gpu-capabilities.md: capability の bit・GET_INFO・GET_CAPSET・capset 4 の形

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
- src/drivers/gpu/gpu.c: `gpu_info_ioctl`（capabilities は `device->ops->capabilities`）・`gpu_capset_ioctl`・`gpu_ioctl_body`（bit の無い時の errno）
- 各 driver の capset の関数（`grep -rn 'get_capset' src/drivers/gpu`）: venus/venus.c `venus_get_capset`・`venus_get_info`、amdrdna/runtime.c `runtime_capset`、nvrtx/runtime.c、powervr/render-runtime.c、bcm2711/render-runtime.c、i915/command.c `i915_get_capset`、mali、ghci/vk/vk-node.c
- userland/desktop/libvulkan/context.c の capset を読む所（`VULKAN_VENDOR_CAPSET_*`・`VULKAN_NATIVE_CAPSET_*`、`vulkan_load_word(capset.data + N)`）と各 flag の使い道
- src/drivers/gpu/venus/transport.c（strict queue が無い時の `GPU_JOB_RESERVE`・`GPU_JOB_CAPACITY` の ENOTSUP）
- bit を足す・消す所: amdrdna/node.c・amdrdna/present.c、nvrtx/node.c、powervr/render-runtime.c、bcm2711/render-runtime.c、mali/sched.c・mali/render.c、ghci/ghci.c・ghci/vk/vk-node.c（register の前か後か）

## 2. 作る物

- `docs/reference/gpu-capabilities.md`
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 確かめの項目

- 骨格の全ての `PENDING` と `<!-- spec: -->` を埋めた（script の `--final`）。
- 要求の節ごとに `Requires:`・本文・`Errors:`（最後に `- Other values:`）・`Sources:` の形（gpu-doc-rules.md 2 節の 6）。
- 構造体の表の Direction と Meaning を全行。定数の表の Meaning を全行。
- 自己の照合の表と、`Errors:` から 3 つの読み直し（gpu-doc-rules.md 4 節）。
- 出典にする試験の見当: gpu-admission・gpu-recovery・display-scanout（GPU_GET_INFO）。capset を直に読む試験は無い（libvulkan が consumer）。

## 4. 注意

- ユーザーの決定: 2026-10-29 ユーザーの回答（クリック、Q1 の中継）Q-D54-1「capset の形も書く」。解釈: capset id 4・version 0 の payload の形（大きさ・magic・flag の欄）を書く。
- capset の payload は public な header に無い ABI。文書が reference になる。表の名前（`VENDOR_MAGIC` など）は文書の名前で header の macro ではないと書く。
- capset の表の値は script が libvulkan と照らす。driver が書く値（例: amdrdna の flags）と libvulkan の定義が食い違えば結果の節に書く。
- bit が node の寿命の中で増えるかは p019（drivers）でも扱う。ここでは結論と、program がいつ問い直すかを書く。

## 5. 変えてはいけない物

- 2 節の file 以外の全部（gpu-doc-rules.md 5 節）。見出し（H1〜H4）と機械の列。照合の script。header・source・HAL・toolchain・共有の build。
- 不備を見つけたら直さずに止め、file・場所・足りない物を報告する。

## 6. 確かめの command

```sh
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --final docs/reference/gpu-capabilities.md
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008
timeout 120 /home/awe/zedBSD-claude1/build/NoctLang/build-static/noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture
git diff --check
```

## 7. 終わりの条件

gpu-doc-rules.md 6 節。commit は `git commit -m WIP -- docs/reference/gpu-capabilities.md plan/ws009/phase011/phase.md plan/ws009/ws.md`。

## 結果

（未実施）
