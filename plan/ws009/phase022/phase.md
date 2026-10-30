<!-- awesome-plan project=zedbsd record=ws009-p022 -->

# ws009-p022: DOC-54 統合: 全文書の最終の照合、索引と Requires の食い違いの解消、既存の文書の直し

Status: planned（2026-10-30 P1 の設計、q1701。p009 を節ごとに分けた物）
Parent: [WS009](../ws.md)
Approval: 2026-10-27 ユーザーの回答（クリック）「今書く」（DOC-54）。2026-10-30 ユーザー「WS009、WS296・WS297は標準廉価実装手順を適用してphaseを作り替え、今から実行してOKです。ただ、1 WSずつ実施します。」
実装の担当: Sonnet 5.5 high（1 回の起動）
依存: p010〜p021 の全部が main に merge 済み
骨格の基点: 5fedf634d

## 1. 読む物

- AGENTS.md の §4（担当の共通の規則）・§7（禁止）・§10（管理情報を書かない）。
- [docs/style.md](../../../docs/style.md) と [gpu-doc-rules.md](../gpu-doc-rules.md)（書き方・形・確かめ・終わりの条件の共通の規則。この phase.md と同じ重さの手順）。
- 自分の file の骨格の全文（各節の `<!-- spec: -->` が書く事の仕様）と [gpu.md](../../../docs/reference/gpu.md) の骨格（共通の規約の spec）。
- p010〜p021 の phase.md の結果の節（食い違い・未確認・自分で決めた点）
- src/drivers/gpu/gpu.c（食い違いの解消に要る handler だけ）

## 2. 作る物

- `docs/reference/gpu*.md（食い違いの直しだけ）`
- `docs/reference/control-devices.md`
- `docs/architecture/kernel-and-hal.md`
- `docs/reference/visionfive2-gpu.md`
- `docs/reference/kernel-boot-parameters.md`
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 確かめの項目

- 骨格の全ての `PENDING` と `<!-- spec: -->` を埋めた（script の `--final`）。
- 要求の節ごとに `Requires:`・`Errors:`・`Sources:` の形（gpu-doc-rules.md 2 節の 6）。
- 構造体の表の Direction と Meaning を全行。定数の表の Meaning を全行。
- 自己の照合の表と、`Errors:` から 3 つの読み直し（gpu-doc-rules.md 4 節）。
- 出典にする試験の見当: check-completed-producer-docs.noct も流し、gpu*.md が status の違反を足さないことを確かめる（手前の照合で止まるなら、その最後の行を記録）。

## 4. 注意

- `--final`（全 file）で残る FAIL を直す。索引と `Requires:` の食い違いは、code を読んで正しい方に揃える（どちらを直したかを結果の節に）。
- control-devices.md の「There is no published current GPU object ABI to enumerate here.」を gpu.md への link と 1〜2 文に。kernel-and-hal.md の GPU の段（text console が firmware の framebuffer に描き続けるという文）を今の振る舞い（最初の flip で console が framebuffer から外れる。gpu-display.md の User-visible side effects）に。visionfive2-gpu.md と kernel-boot-parameters.md から gpu.md へ link。
- 各 Phase の報告の header・code の食い違い（Bug の候補）を一覧にして Q1 に送る。文書は今の振る舞いのまま。
- 見出しの変更は禁止のまま（`--base`）。

## 5. 変えてはいけない物

- 2 節の file 以外の全部（gpu-doc-rules.md 5 節）。見出し（H1〜H4）と機械の列。照合の script。header・source・HAL・toolchain・共有の build。
- 不備を見つけたら直さずに止め、file・場所・足りない物を報告する。

## 6. 確かめの command

```sh
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 5fedf634d --examples --final
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 5fedf634d
timeout 120 /home/awe/zedBSD-claude1/build/NoctLang/build-static/noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture
git diff --check
```

## 7. 終わりの条件

gpu-doc-rules.md 6 節。commit は `git commit -m WIP -- docs/reference/gpu*.md docs/reference/control-devices.md docs/architecture/kernel-and-hal.md docs/reference/visionfive2-gpu.md docs/reference/kernel-boot-parameters.md plan/ws009/phase022/phase.md plan/ws009/ws.md`。
加えて: `--final` を file の指定なしで（全 file）PASS、`--examples` で amd64 の compile が 1 回以上。

## 結果

（未実施）
