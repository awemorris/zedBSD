<!-- awesome-plan project=zedbsd record=ws324-p006h -->

# ws324-p006h H.265 の解析の適合の試験（HEVC_v1、host、Opus）

Status: planned（2026-11-01 P1 が p006a で作成。p006e・p006f・p006g の cleared と適合の cache（Q-D）の後）

- WS: [WS324](../ws.md)。設計: [h265-parse-design.md](../h265-parse-design.md) §2.3・§8.4・§12。型 O（Opus、phase-runner）。手順 §4.6 の「統合・適合」の Phase。

## やる事

1. 適合の bitstream を取る: `H265SW_CONFORMANCE_DIR=/home/awe/zedBSD-claude1/build/sources/h265-conformance sh <Q-D の後の path>/fetch.sh`（パラメータの無い GET、SHA-256 の記録と照合、zip でない答えで即止めて Q1 に）。
2. host の道具 `plan/ws324/tests/conformance/conform.c` と `run.sh` を作る（design §8.4: h265-stream・h265-dpb（17 slot、出力ごとに release）・h265-std に通し、parameter set と最初の slice header の値を syntax element の名前で、出力の POC の列を書く。zip は `unzip -p` で読む）。比べの script（`compare.py`、`/usr/bin/python3 -I`）: host の `ffmpeg -bsf:v trace_headers` の値と、`ffmpeg -threads 1 -loglevel debug` の `Output frame with POC` の列。各 command に時間の上限。
3. 比べる field の一覧を決めて記録する（VPS・SPS・PPS の全 syntax element のうち trace_headers が出す物、slice header の poc_lsb・RPS・long-term・pps id・型）。
4. 全 in の stream で design §8.4 の 1〜4 を確かめる。prefix SEI（decoding unit information など）を絵の segment の間に持つ stream（HRD_A_Fujitsu_3 など、名前と txt から選ぶ）を明示して確かめ、記録する。食い違いは仕様で原因を確かめ、module の .c の誤りなら直す（header は変えない。header の comment の誤りは記録して規約の見直しの Phase へ）。ffmpeg と仕様が違う場面（NoOutputOfPriorPics、EOS、RASL）は例外の表に理由を書く（ffmpeg を正としない）。
5. out の stream（10 bit など）が parse で落ちずに読め、範囲の判定に必要な値が正しいことも確かめる（decode はしない）。

## 受け入れ

- run.sh の最後の行 `conformance: PASS (<in の数> streams)`、例外の表、比べた field の一覧と件数。
- `sh plan/ws324/tests/run-unit.sh all` と `check-skeleton.sh`（BASE）が PASS のまま。
- 直した .c は phase.md に file・要点・SHA。

## 結果

（担当が書く）
