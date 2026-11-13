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

## agy（Gemini）を Claude の配下として動かす（2026-11-12 ユーザーの決定）

- **Claude がハーネス（指揮する側）になり、agy を配下のエージェントとして `-p` で起動する。** `agy -p` は Claude の子の process（subprocess）として動き、返事を Claude に返して終わるので、Claude の配下のエージェントと同じ形になる。 agy の会話は対話の画面で開かない（開いた process は `-p` の turn を知らず、文脈が 2 つに分かれる。同じ会話への `-p` はセッションを再起動し、動いている配下のエージェントを止める）。
- 送り方: `cd /home/awe/amaterrace-gpu && agy --model gemini-3.8-flash-high --conversation <ID> -p "<指示>"`。**model は必ず指定する**（Flash、2026-11-12 ユーザー。指定しないと既定の別の model で動く）。**相手の会話の ID はユーザーが知らせる**（2026-11-12 ユーザーの決定。cache の file からは今の作業中の会話を正しく選べない）。知らされた ID が無い時は、新しい会話として `-p` だけで起動する（前の確かめをしてから）。
- 1 回の `-p` で 1 つのまとまった作業を頼む。指示は依頼の file（`gemini-request-NN.md`）を指して短く書き、file の変更の可否と、返事に書く物（commit・試験・未実施）を明記する。agy は背景で起動し、時間の上限を付けない（2026-11-12 ユーザー、2026-11-13 に重ねて: 丸一日かかる事もある。AGENTS.md §4 の「時間の上限」の例外）。Claude Code の Bash の背景の実行（`run_in_background`）は既定 30 分・最大 2 時間で止めるので使わない。`setsid nohup sh -c 'agy … > <log> 2>&1; echo "rc=$?" >> <log>; sync' … < /dev/null &` で切り離して起動し、log と `build/agy/<task>.md` を見て進みを知る。 agy は timing closure のために修正と合成を繰り返し、数時間から丸一日動く事がある（2026-11-13 ユーザー）。log が長く動かない・同じ合成を繰り返すように見えても、止まったと決めず、止めない・急かさない。返事を Claude が監査し、次の `-p` を送る。
- `-p` で起動した会話は agy の app（Web）の画面で見られる。指示と返事は毎回 Claude がユーザーに報告する。
- agy は process が終わっても、Gemini の cache で同じ会話を続けられる。remote control の起動などで process が入れ替わると、同じ作業が別の process（別の会話の ID）で続く事がある（2026-11-12）。**起動の前に、amaterrace-gpu で動いている agy の process・作業ツリーの未 commit の変更・最近の commit を確かめ、他の agy が作業中なら起動しない**（同じ作業ツリーで 2 つが動くと壊れる）。続きは、前の会話を `--conversation` で再開して頼む方を優先する。

- **agy への頼み方の流れ**（2026-11-13 ユーザーの決定。依頼 08 の X1・X2 の聞き方をよいとして）。1 回の `-p` は 1 段（1 項目）だけで、終わったら agy は止まって返す。次は Claude が返事を確かめてから。
  1. 設計した RTL の記述を頼む。
  2. 返ってきた RTL に Claude のレビューの指摘を送り、agy に対応の方針を返してもらう（この回は file を変えない）。
  3. その方針に沿った RTL の修正を頼む。
  4. GHCI の仕様（`docs/ghci.md`）の確かめや、仕様と実装の乖離の指摘を送り、対応の方針を返してもらう。
  5. testbench: agy が RTL を実装・修正した時は、変えた module の小さな単体の testbench（`sim/run.sh NAME` を 1 本ずつ、変えた module の物）を agy が流してよい。長い物・上位の物（`atg_top*`・`atg_ghci_top`・`atg_kc705_*`・`atg_su` の全体など）と全体の回帰（`sim/run.sh` の全部・`make sim`）は agy は流さず、返事に「流してほしい testbench の名前（と plusarg）と理由」を書き、Claude が流して（並列 4 まで、時間の上限、background）結果を返す。
- Claude は agy の process の出力（stream-json）を見て、上位の testbench や全体の実行を始めたら止める。
- 前の形（155 本を途中の報告なしに数時間流した）は、この流れで置き換えた。

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
