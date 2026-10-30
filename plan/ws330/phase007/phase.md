<!-- awesome-plan project=zedbsd record=ws330-p007 -->

# ws330-p007 h265sw-rps（short-term の参照画の組と予測）

- 親: [ws.md](../ws.md)、設計 [design.md](../design.md)。型: S（Sonnet 5.5 high、phase-runner-low、標準廉価実装手順、1 回の起動）。
- 状態: planned（2026-10-31 p001 が作成）。依存: p003。BASE: p003 の commit（p003 が書く）。

## 1. 読む物

- 共通: [design.md](../design.md) §13.1 の「読む物（共通）」（AGENTS.md §4・§7・§9・§10、plan/agents/protocol.md、plan/coding-style.md の全文、design.md §1・§2・§3・§7・§8・§10.1、`vcodec.h`・`h265sw.h`）。
- 仕様: ITU-T H.265 (V11) (01/2026)、`/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h265-202601.pdf`（SHA-256 `78b093480e178a2b99d8cb7499934240f359c50c6d09568f8be651bc1151d4d2`、git の外）。**PDF の頁 = 印刷の頁 + 18**。`pdftotext -layout -f N -l M` で読み、表と式（上付きが崩れる: 2^15 が「215」になる）は Read の tool で PDF の頁の画を見て確かめる。
- header（仕様の正本）: `userland/desktop/libmedia/h265sw-rps.h`、`userland/desktop/libmedia/vcodec-bitreader.h`（p003 の前は [proposed/](../proposed/)）。
- 仕様の節: 7.3.7（印刷 50 = PDF 68）、7.4.8（印刷 102〜104 = PDF 120〜122、7-59〜7-71）。

## 2. 作る物

- `userland/desktop/libmedia/h265sw-rps.c`（header の関数を全部実装する）。
- 単体試験: `plan/ws330/tests/unit/test-h265_rps.c`。
- この phase.md の「結果」、ws.md の表のこの行。

## 3. 試験の項目

- 明示の組の往復（num_negative・num_positive の境、delta の累積 7-67〜7-70）。
- 予測の組: deltaRps の正負、use_delta_flag の 0 と 1、源の組の端の entry（j = NumDeltaPocs）、slice header の組（index == num_sets、delta_idx_minus1）。7-61・7-62 を手で追った 3 つの例（期待の値は手で計算）。
- 上限: 予測で 1 つ増えて max_pictures を超える時 EINVAL で、配列の外を書かない（ASan）。delta_idx_minus1 が index 以上、abs_delta_rps_minus1 の 2^15。
- 乱数の bit 列（ASan・UBSan）。
- 共通: 誤りの入力・途中で切れた入力・乱数の入力で落ちず（ASan・UBSan 0）決められた誤りを返す。

## 4. 注意

- 7-61 の loop は書く前に数を確かめる（header の comment）。DeltaPoc の和は int32_t に収まる（header）が、計算の途中も int64_t で検査してよい。
- 算術は vcodec.h の規則（負の値の shift は vcodec_asr32・vcodec_shl32、積は広い型、狭い型へは clip）。

## 5. 変えてはいけない物

- design.md §13.1 の「変えてはいけない物」: 全部の `h265sw*.h`・`vcodec-*.h`・`h264sw*.h`・`h264enc*.h`、担当の外の `.c`、Makefile、`plan/ws330/tests/run-unit.sh`・`check-skeleton.sh`・共通の試験の補助（自分の test を除く）、HAL・toolchain・共有の build/。
- header の不備・仕様との食い違いを見つけたら header を直さず止めて Q1 に返す（どの header のどの関数で何が足りないか）。header が黙っている所は自分で決め、結果の節に列挙する。phase.md が仕様と食い違えば仕様に従い報告する。

## 6. 確かめの command

```
timeout 900 sh plan/ws330/tests/run-unit.sh h265_rps
timeout 1800 sh plan/ws330/tests/run-unit.sh all
BASE=<p003 の SHA> timeout 300 sh plan/ws330/tests/check-skeleton.sh
（H.264 の回帰: p003 が plan/tools.md に登録した WS326・WS329 の単体試験の道具の all。WS326・WS329 の完了で plan/ws326・ws329 の tests は plan/tools へ移るので、p003 の結果の節の path を使う）
（amd64 の libmedia.so の build: plan/ws326/design.md §6 の command、warning 0）
git diff --check
```

## 7. 終わりの条件

- 上の command が全部 PASS（出力の最後の行を自分で読む）。
- わざと壊す確かめ（git の外で実装を数通り壊し、試験が FAIL することを確かめて戻す、`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。
- 表の値は仕様から取り、独立した 2 つの方法で照合（`pdftotext -layout` と頁の画など）。試験の期待の値は仕様の表・式と手の計算だけから作る（他の実装の出力を使わない）。
- 他の実装（HM・FFmpeg・x265・libde265 など）の code・comment・構造・名前を写さない。source の comment に開発の管理情報（WS・Phase の番号、plan/ への参照、日付）を書かない。表の block に「Values referenced: ITU-T H.265 (01/2026) Table …」。
- 規約の checklist（plan/coding-style.md §14）で自分の diff を見直す。
- 新しい file は `git add` の後 `git commit -m WIP -- <path>...`。push しない。rm をしない。
- この phase.md の「結果」の節（日付は host の `date`、command と件数、自分で決めた点、未実施）と ws.md の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 結果

（実装の担当が書く）
