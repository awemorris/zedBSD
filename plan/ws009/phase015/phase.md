<!-- awesome-plan project=zedbsd record=ws009-p015 -->

# ws009-p015: DOC-54 gpu-fences.md: fence の要求と *_SYNC

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
- src/drivers/gpu/gpu-fence.c（全体、626 行）
- src/drivers/gpu/gpu.c: `gpu_fence_ioctl`・`gpu_fence_create_ioctl`・`gpu_fence_get`・`gpu_fence_state_ioctl`・`gpu_fence_bind_ioctl`・`gpu_fence_binding_add`・`gpu_fence_binding_end`・`gpu_fence_retire`・`gpu_dependency_wait`・`gpu_dependency_ioctl`
- include/uapi/gpu-fence.h の comment

## 2. 作る物

- `docs/reference/gpu-fences.md`
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 確かめの項目

- 骨格の全ての `PENDING` と `<!-- spec: -->` を埋めた（script の `--final`）。
- 要求の節ごとに `Requires:`・本文・`Errors:`（最後に `- Other values:`）・`Sources:` の形（gpu-doc-rules.md 2 節の 6）。
- 構造体の表の Direction と Meaning を全行。定数の表の Meaning を全行。
- 自己の照合の表と、`Errors:` から 3 つの読み直し（gpu-doc-rules.md 4 節）。
- 出典にする試験の見当: acquire-fence（FENCE_CREATE・BIND・SIGNAL）、gpu-fence（中を grep で確かめる）。QUERY・WAIT・RESET・*_SYNC は source tree の試験が無いかもしれない。

## 4. 注意

- `GPU_COMMAND_SUBMIT_SYNC`・`GPU_DISPLAY_PRESENT_SYNC` の外側は header を持たない。入れ子の command・present が version と入れ子の size を持つ。
- `GPU_DISPLAY_PRESENT_SYNC` の present の意味は gpu-display.md（別の Phase）が書く。ここは待ちと signal と errno を書き、present の欄は link で済ませる。
- fence の要求が `GPU_CAP_FENCE` を要るかは `gpu_fence_ioctl` を読んで要求ごとに決める。

## 5. 変えてはいけない物

- 2 節の file 以外の全部（gpu-doc-rules.md 5 節）。見出し（H1〜H4）と機械の列。照合の script。header・source・HAL・toolchain・共有の build。
- 不備を見つけたら直さずに止め、file・場所・足りない物を報告する。

## 6. 確かめの command

```sh
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --final docs/reference/gpu-fences.md
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008
timeout 120 /home/awe/zedBSD-claude1/build/NoctLang/build-static/noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture
git diff --check
```

## 7. 終わりの条件

gpu-doc-rules.md 6 節。commit は `git commit -m WIP -- docs/reference/gpu-fences.md plan/ws009/phase015/phase.md plan/ws009/ws.md`。

## 結果

（未実施）
