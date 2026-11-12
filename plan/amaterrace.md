# Amaterrace GPU（`~/amaterrace-gpu`）との仕事の仕方

GHCI 1.0 の仕様（`docs/ghci.md`）と Amaterrace GPU の RTL は `/home/awe/amaterrace-gpu` にある。zedBSD の GHCI の driver（`src/drivers/gpu/ghci/`）はこの仕様に従う。
2026-11-11・12 ユーザーの決定と、これまでの開発の経験（要点）。今の WS は [WS347](ws347/ws.md)。

## 役割と、これまでに分かった得手・不得手

- **Claude は監査と driver**: 仕様と RTL を読み、食い違いを証拠（仕様の節、RTL の file:line、期待値と実際の値）つきで書き出し、driver を実装する。
  RTL は書かない（担当にも書かせない）。Claude は仕様・RTL を読む力とソフトウェアを実装する力が高く、RTL を実装する力はまだ未知数。
  **Claude のレビューの指摘の表には、毎回 15% ほど誤りが含まれる**（経験。例: 2026-11-12 の依頼 05 の S1 で、仕様の STATUS の CP_STATE の行を見落とした）。
- **Gemini は RTL の実装**（今は Gemini 3.8 Flash）: パイプライン化とタイミングの収束が非常に優れる。一方、仕様のごく細かい所の実装の誤りは人間の実装と同程度にあり、
  **仕様の変更への追従が得意でなく、旧い仕様が部分的に残る**（水平展開の弱さ）。Claude が仕様と RTL を監査すればこの問題は避けられる。
- **ユーザー**: 仕様の問い（下の B）を決め、Claude と Gemini の間で文書を渡す。
- **相互のレビュー**: RTL の開発は Claude と Gemini が互いにレビューし合って進める。Claude が自分の指摘を検証し直すより、
  Claude の指摘の表を Gemini に渡して妥当性を確かめてもらう方が、context と費用の効率が良い（経験）。

## 進め方（1 回の周回）

1. Claude が仕様と RTL を監査し、指摘を具体的な依頼の文（`gemini-request-NN.md`、まとめ版で前の依頼を置き換える）にする。
2. Gemini がまず**指摘の妥当性を確かめて 1 次回答**する（誤った指摘は仕様の行を示して返す）。
3. Claude が 1 次回答を仕様で照らし、Gemini が正しければ依頼の文を改訂する（自分の誤りを記録に残す）。Gemini が誤っていれば仕様の行を示して返す。
4. Gemini が実装し、報告（`gemini-report-NN.md`）を返す。
5. Claude が監査し（`audit-<SHA>.md`）、残りを次の依頼にする。旧い仕様の残り（水平展開の漏れ）を特に探す。

## agy（Gemini）への直接の指示（2026-11-12 から）

- Claude は agy に直接指示を送り、返事を受け取れる（ユーザーは Claude を UI にする）:
  `cd /home/awe/amaterrace-gpu && agy --conversation <会話の ID> -p "<指示>"`
- **今の RTL の実装のセッション**: `00de1bda-f4cb-45eb-94a3-091c9e9bd964`（2026-11-12、依頼 06 を実装中）。指示はこの ID に送る: `agy --conversation 00de1bda-f4cb-45eb-94a3-091c9e9bd964 -p "..."`（作業の directory は `/home/awe/amaterrace-gpu`）。
- 他の会話の ID は `~/.gemini/antigravity-cli/cache/last_conversations.json`（作業の directory ごとの最後の会話。実装中のセッションと違う事がある）と `conversation_metadata.json`。
- `-p` で送った往復は agy の対話の画面に出ない（2026-11-12 ユーザーが確かめた）。ユーザーに見せる事は Claude が報告する。
- 長い作業は背景で待ち、時間の上限を付ける。指示は file の依頼（`gemini-request-NN.md`）を指して短く書き、file の変更の可否を明記する。

## 規則

- `docs/ghci.md` が正。RTL は誤りの見込みが高い物として扱う。ただし Claude の指摘も 15% ほど誤りうるので、Gemini の反論は仕様の文で照らしてから受ける・返す。
- Gemini の仕様・実装・回答の誤りは単純な誤りとして淡々と指摘し、直るまで周回する。仕様と実装の乖離で迷わない。
- 食い違いの 2 分類:
  - **A**（仕様の文が明確で RTL が違う）→ Gemini への依頼。
  - **B**（仕様が本当に黙っている・表せない・設計の意図が要る）→ ユーザーの判断。推測で埋めない。迷ったら B。
- 台帳は WS ごとに 1 つ（WS347 は [ws347/ghci-rtl-questions.md](ws347/ghci-rtl-questions.md)）。決定が変わったら行を書き換える（訂正を書き足さない）。担当は会話でなく台帳を読む。
- Gemini とのやり取りは file で: 依頼 `gemini-request-NN.md`（まとめ版、番号を振り直し「済んだ物」を付ける。Gemini は前の依頼を覚えきれない）、質問 `gemini-questions-NN.md`、Gemini の報告は `gemini-report-NN.md` として保存、Claude の照合・監査は `gemini-answers-NN.md`・`audit-<SHA>.md`。依頼は具体的に（変える所・期待値・受け入れの試験）。tb の期待値は仕様の文から作らせる（RTL の今の値に合わせた期待値で PASS させない）。
- 仕様（ghci.md）を Claude が書き換えるのは、ユーザーが中身を決めた時だけ。依頼で Gemini に書き換えさせる時は、置き換えの文を指定する。
- driver は仕様だけに従い、RTL の誤りを吸収しない（疎通の試験のための回避はユーザーの承認の後、device の版で限定し台帳の行と対にする）。
- 読む事、`git log/show/diff`、ユーザーが指示した bitstream の JTAG での書き込み（Vivado、`~/.local/bin/vivado` の wrapper、作業の directory は repo の外）はよい。
  Vivado は host で同時に 1 つ（ユーザーや Gemini の合成が動いていれば待つ）。KC705 の build は `tools/vivado-build.sh` の既定の timeout（1800 秒）では足りない。`/home/awe/amaterrace-worktrees` は片付けない。zedBSD の計画で参照する時は `am-ws001` のように書く。

## 実機

- KC705（Kintex-7）を VF2 の PCIe につなぐ。JTAG は host の USB（Digilent `210203368704A`、device `xc7k325t_0`）。書き込みは揮発。
- 電源: KC705 は SwitchBot のプラグミニ 2 `14C19FA80F86`、VF2 はプラグミニ 3 `AC276E432AE6`。KC705 を書いた後は VF2 だけを入れ直して PCIe を走査させる。
- minimal の preset（`make preset-minimal` が `config/minimal.mk` を `config.mk` に写し、`include/atg_config.vh` を作る）。KC705 の GPU の clock は 200 MHz、SFU 無し（試験は compiler が SFU を使わない範囲）、VA 30 bit。
