<!-- awesome-plan project=zedbsd record=ws009-p023 -->

# ws009-p023: DOC-54 正確さの敵対的レビュー（Opus を推す）

Status: planned（2026-10-30 P1 の設計、q1701。p009 を節ごとに分けた物）
Parent: [WS009](../ws.md)
Approval: 2026-10-27 ユーザーの回答（クリック）「今書く」（DOC-54）。2026-10-30 ユーザー「WS009、WS296・WS297は標準廉価実装手順を適用してphaseを作り替え、今から実行してOKです。ただ、1 WSずつ実施します。」
実装の担当: Opus（設計の担当の推し。推論が多い）
依存: p022 が main に merge 済み
骨格の基点: 5fedf634d

## 1. 読む物

- AGENTS.md の §4（担当の共通の規則）・§7（禁止）・§10（管理情報を書かない）。
- [docs/style.md](../../../docs/style.md) と [gpu-doc-rules.md](../gpu-doc-rules.md)（書き方・形・確かめ・終わりの条件の共通の規則。この phase.md と同じ重さの手順）。
- 自分の file の骨格の全文（各節の `<!-- spec: -->` が書く事の仕様）と [gpu.md](../../../docs/reference/gpu.md) の骨格（共通の規約の spec）。
- docs/reference/gpu*.md の全文
- include/uapi/gpu*.h
- src/drivers/gpu/gpu.c・gpu-fence.c・各 driver（主張を確かめる所）

## 2. 作る物

- `docs/reference/gpu*.md（誤りの直しだけ）`
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 確かめの項目

- 骨格の全ての `PENDING` と `<!-- spec: -->` を埋めた（script の `--final`）。
- 要求の節ごとに `Requires:`・`Errors:`・`Sources:` の形（gpu-doc-rules.md 2 節の 6）。
- 構造体の表の Direction と Meaning を全行。定数の表の Meaning を全行。
- 自己の照合の表と、`Errors:` から 3 つの読み直し（gpu-doc-rules.md 4 節）。

## 4. 注意

- 各 file から主張を抜き出し（errno の条件・寿命・並行・待ちの上限・capability の要否）、source と照らす。少なくとも要求ごとに 1 つ、全体で 60 以上。
- 誤りは直し、表（主張・file・根拠・結果）を結果の節に置く。header・code の側の食い違いは Q1 に送る。
- 書き方の規則（gpu-doc-rules.md 2 節、docs/style.md）の見直しも行う。

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

gpu-doc-rules.md 6 節。commit は `git commit -m WIP -- docs/reference/gpu*.md plan/ws009/phase023/phase.md plan/ws009/ws.md`。

## 結果

（未実施）
