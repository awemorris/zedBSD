# Amaterrace GPU（`~/amaterrace-gpu`）との仕事の仕方

GHCI 1.0 の仕様（`docs/ghci.md`）と Amaterrace GPU の RTL は `/home/awe/amaterrace-gpu` にある。zedBSD の GHCI の driver（`src/drivers/gpu/ghci/`）はこの仕様に従う。
2026-11-11 ユーザーの決定（要点）。今の WS は [WS347](ws347/ws.md)。

## 役割

- **Claude は監査**: 仕様と RTL を読み、食い違いを証拠（仕様の節、RTL の file:line、期待値と実際の値）つきで書き出し、driver を実装する。
  RTL は書かない（担当にも書かせない）。Claude は仕様・RTL を読む力とソフトウェアを実装する力が高く、RTL を実装する力はまだ未知数。
- **Gemini は実装**: Claude の指揮の下で RTL を直し、パイプライン化してタイミングを閉じる（その力が高い）。今は Gemini 3.8 Flash で、仕様を覚えきれず整合しない誤りがよくある。
- **ユーザー**: 仕様の問い（下の B）を決め、Gemini に依頼の文を渡す。

## 規則

- `docs/ghci.md` が正。RTL は誤りの見込みが高い物として扱う。
- Gemini の仕様・実装・回答の誤りは単純な誤りとして淡々と指摘し、直るまで「依頼 → 監査 → 指摘」を繰り返す。Claude の監査が正しい前提で進め、仕様と実装の乖離で迷わない。
- 食い違いの 2 分類:
  - **A**（仕様の文が明確で RTL が違う）→ Gemini への依頼。
  - **B**（仕様が本当に黙っている・表せない・設計の意図が要る）→ ユーザーの判断。推測で埋めない。迷ったら B。
- 台帳は WS ごとに 1 つ（WS347 は [ws347/ghci-rtl-questions.md](ws347/ghci-rtl-questions.md)）。決定が変わったら行を書き換える（訂正を書き足さない）。担当は会話でなく台帳を読む。
- Gemini とのやり取りは file で: 依頼 `gemini-request-NN.md`、質問 `gemini-questions-NN.md`、Gemini の報告は `gemini-report-NN.md` として保存、Claude の照合・監査は `gemini-answers-NN.md`・`audit-<SHA>.md`。依頼は具体的に（変える所・期待値・受け入れの試験）。
- 仕様（ghci.md）を Claude が書き換えるのは、ユーザーが中身を決めた時だけ。依頼で Gemini に書き換えさせる時は、置き換えの文を指定する。
- driver は仕様だけに従い、RTL の誤りを吸収しない（疎通の試験のための回避はユーザーの承認の後、device の版で限定し台帳の行と対にする）。
- 読む事、`git log/show/diff`、ユーザーが指示した bitstream の JTAG での書き込み（Vivado、`~/.local/bin/vivado` の wrapper、作業の directory は repo の外）はよい。
  Vivado は host で同時に 1 つ（ユーザーや Gemini の合成が動いていれば待つ）。`/home/awe/amaterrace-worktrees` は片付けない。zedBSD の計画で参照する時は `am-ws001` のように書く。

## 実機

- KC705（Kintex-7）を VF2 の PCIe につなぐ。JTAG は host の USB（Digilent `210203368704A`、device `xc7k325t_0`）。書き込みは揮発。
- 電源: KC705 は SwitchBot のプラグミニ 2 `14C19FA80F86`、VF2 はプラグミニ 3 `AC276E432AE6`。KC705 を書いた後は VF2 だけを入れ直して PCIe を走査させる。
- minimal の preset（`make preset-minimal` が `config/minimal.mk` を `config.mk` に写し、`include/atg_config.vh` を作る）。KC705 の GPU の clock は 200 MHz、SFU 無し（試験は compiler が SFU を使わない範囲）、VA 30 bit。
