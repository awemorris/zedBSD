<!-- awesome-plan project=zedbsd record=ws330-p031 -->

# ws330-p031 decoder の速さ

- 親: [ws.md](../ws.md)、設計 [design.md](../design.md)。型: O（Opus、phase-runner）。
- 状態: planned（2026-10-31 p001 が作成）。依存: p029、p030。

## 1. 読む物

- 共通: [design.md](../design.md) §13.1 の「読む物（共通）」（AGENTS.md §4・§7・§9・§10、plan/agents/protocol.md、plan/coding-style.md の全文、design.md §1・§2・§3・§7・§8・§10.1、`vcodec.h`・`h265sw.h`）。
- 仕様: ITU-T H.265 (V11) (01/2026)、`/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h265-202601.pdf`（SHA-256 `78b093480e178a2b99d8cb7499934240f359c50c6d09568f8be651bc1151d4d2`、git の外）。**PDF の頁 = 印刷の頁 + 18**。`pdftotext -layout -f N -l M` で読み、表と式（上付きが崩れる: 2^15 が「215」になる）は Read の tool で PDF の頁の画を見て確かめる。
- design.md の全文、関係する header（proposed/ か source）。

## 2. すること

測り（5330・Pi 5、1 thread）、近道（MC の端の fast path、変換の 0 の行の省略、CABAC の表引き）、WPP・tile の行の並列（WS318 の thread の台帳）。設計を phase.md に足してから実装。

## 3. 受け入れ

- Q330-3 の目標。
- 近道の前後で全試験の MD5 が同じ。

## 4. 規則

- 範囲の外の作業をしない。計画に無い依存は Q1 に返す。`git commit -m WIP -- <path>...`、push しない、rm をしない。QEMU・実機は Q1 の割り当てで 1 本の script。console の log で判定しない。
- 他の実装の code・comment・構造・名前を写さない。source の comment に開発の管理情報を書かない。

## 結果

（担当が書く）
