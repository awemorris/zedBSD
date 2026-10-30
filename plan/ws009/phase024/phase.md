<!-- awesome-plan project=zedbsd record=ws009-p024 -->

# ws009-p024: DOC-54 正確さの敵対的レビュー（Opus を推す）

Status: in-progress（2026-10-30 P1、q1704-i01。worktree p1-ws009r、起点 d73c023ef）
Parent: [WS009](../ws.md)
Approval: 2026-10-27 ユーザーの回答（クリック）「今書く」（DOC-54）。2026-10-30 ユーザー「WS009、WS296・WS297は標準廉価実装手順を適用してphaseを作り替え、今から実行してOKです。ただ、1 WSずつ実施します。」
実装の担当: Opus（設計の担当の推し。推論が多い）
依存: p023 が main に merge 済み
骨格の基点: 4dfcce008

## 1. 読む物

- AGENTS.md の §4（担当の共通の規則）・§7（禁止）・§10（管理情報を書かない）。
- [docs/style.md](../../../docs/style.md) と [gpu-doc-rules.md](../gpu-doc-rules.md)（書き方・形・確かめ・終わりの条件の共通の規則。この phase.md と同じ重さの手順）。
- 自分の file の骨格の全文（各節の `<!-- spec: -->` が書く事の仕様）と [gpu.md](../../../docs/reference/gpu.md) の骨格（共通の規約の spec）。
- 着手の時に gpu-doc-rules.md 1 節の `git log 4dfcce008..HEAD -- ...` で基点の後の UAPI の変化を見る。
- docs/reference/gpu*.md の全文
- include/uapi/gpu*.h
- src/drivers/gpu/gpu.c・gpu-fence.c・各 driver（主張を確かめる所）

## 2. 作る物

- `docs/reference/gpu*.md（誤りの直しだけ）`
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。
- 見つけた header・code の食い違い（Bug の候補）と、tree の他の comment と code の食い違い（例: userland/desktop/sessiond/seat.c の node の open の comment）の一覧を Q1 に送る。

## 3. 確かめの項目

- 骨格の全ての `PENDING` と `<!-- spec: -->` を埋めた（script の `--final`）。
- 全 file で `--final` と `--examples` が PASS。4 節の照合の結果を表で結果の節に。

## 4. 注意

- 各 file から主張を抜き出し（errno の条件・寿命・並行・待ちの上限・capability の要否）、source と照らす。少なくとも要求ごとに 1 つ、全体で 60 以上。
- 誤りは直し、表（主張・file・根拠・結果）を結果の節に置く。header・code の側の食い違いは Q1 に送る。
- 書き方の規則（gpu-doc-rules.md 2 節、docs/style.md）の見直しも行う。

## 5. 変えてはいけない物

- 2 節の file 以外の全部（gpu-doc-rules.md 5 節）。見出し（H1〜H4）と機械の列。照合の script。header・source・HAL・toolchain・共有の build。
- 不備を見つけたら直さずに止め、file・場所・足りない物を報告する。

## 6. 確かめの command

```sh
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --examples --final
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008
timeout 120 /home/awe/zedBSD-claude1/build/NoctLang/build-static/noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture
git diff --check
```

## 7. 終わりの条件

gpu-doc-rules.md 6 節。commit は `git commit -m WIP -- docs/reference/gpu*.md plan/ws009/phase024/phase.md plan/ws009/ws.md`。

## 結果

途中（2026-10-30、P1）。gpu.md・gpu-requests.md の review と直しを commit 0b5cf893f。他の 12 file は file の群ごとに読み取りの review を並べて進行中。最後の照合・link の検査は未実施。
