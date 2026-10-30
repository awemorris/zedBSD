<!-- awesome-plan project=zedbsd record=ws009-p022 -->

# ws009-p022: DOC-54 gpu-requests.md: 要求の索引の 2 列と共通の errno の表

Status: planned（2026-10-30 P1 の設計、q1701。p009 を節ごとに分けた物）
Parent: [WS009](../ws.md)
Approval: 2026-10-27 ユーザーの回答（クリック）「今書く」（DOC-54）。2026-10-30 ユーザー「WS009、WS296・WS297は標準廉価実装手順を適用してphaseを作り替え、今から実行してOKです。ただ、1 WSずつ実施します。」
実装の担当: Sonnet 5.5 high（1 回の起動）
依存: p010〜p021 の全部が main に merge 済み
骨格の基点: 4dfcce008

## 1. 読む物

- AGENTS.md の §4（担当の共通の規則）・§7（禁止）・§10（管理情報を書かない）。
- [docs/style.md](../../../docs/style.md) と [gpu-doc-rules.md](../gpu-doc-rules.md)（書き方・形・確かめ・終わりの条件の共通の規則。この phase.md と同じ重さの手順）。
- 自分の file の骨格の全文（各節の `<!-- spec: -->` が書く事の仕様）と [gpu.md](../../../docs/reference/gpu.md) の骨格（共通の規約の spec）。
- 着手の時に gpu-doc-rules.md 1 節の `git log 4dfcce008..HEAD -- ...` で基点の後の UAPI の変化を見る。
- p010〜p021 の各 file の要求の節の `Requires:`・`Errors:` の行
- src/drivers/gpu/gpu.c: `gpu_ioctl`・`gpu_ioctl_body`・各 handler の入口（readable・writable・capability の照合。`grep -n 'readable\|writable\|GPU_CAP_' src/drivers/gpu/gpu.c`）

## 2. 作る物

- `docs/reference/gpu-requests.md`
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 確かめの項目

- 骨格の全ての `PENDING` と `<!-- spec: -->` を埋めた（script の `--final`）。
- 表の Meaning の列（あれば）を全行。
- 節ごとの主張の表と、3 つの読み直し（gpu-doc-rules.md 4 節の要求の節の無い file の形）。
- 出典にする試験の見当: この file は要求の節を持たない。

## 4. 注意

- 索引の Open mode と Capability は、各節の `Requires:` から集め、handler の入口で一つずつ確かめる。code と違えば code に揃え、その節の `Requires:` も直す（この Phase は他の gpu*.md の `Requires:` の行だけを直してよい）。
- 条件つきの capability（flag で変わる・op の有無）は Y に入れない規則（gpu-doc-rules.md 2 節の 6）。review が挙げた例: `GPU_RESOURCE_DESTROY`・`GPU_RESOURCE_IMPORT`・`GPU_DISPLAY_CONSTRAINTS`・`GPU_BLOB_CREATE`・`GPU_BLOB_CREATE_PLACED`・`GPU_DISPLAY_COMMIT`・`GPU_COMMAND_SUBMIT_SYNC`・`GPU_DISPLAY_POWER`。
- errno の表は各節の `Errors:` から集め、意味を gpu.c で確かめる。

## 5. 変えてはいけない物

- 2 節の file 以外の全部（gpu-doc-rules.md 5 節）。見出し（H1〜H4）と機械の列。照合の script。header・source・HAL・toolchain・共有の build。
- 不備を見つけたら直さずに止め、file・場所・足りない物を報告する。

## 6. 確かめの command

```sh
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --final docs/reference/gpu-requests.md
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008
timeout 120 /home/awe/zedBSD-claude1/build/NoctLang/build-static/noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture
git diff --check
```

## 7. 終わりの条件

gpu-doc-rules.md 6 節。commit は `git commit -m WIP -- docs/reference/gpu-requests.md plan/ws009/phase022/phase.md plan/ws009/ws.md`。

## 結果

（未実施）
