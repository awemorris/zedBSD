<!-- awesome-plan project=zedbsd record=ws330-p002 -->

# ws330-p002 decoder 側の共通化（vcodec.h・vcodec-frame・vcodec-output と WS326 p026 の残り、H.264 の decoder を移す）

- 親: [ws.md](../ws.md)、設計 [design.md](../design.md)。型: O（Opus、phase-runner）。
- 状態: planned（2026-10-31 p001 が作成）。依存: WS329 completed、WS326 p026。

## 1. 読む物

- 共通: [design.md](../design.md) §13.1 の「読む物（共通）」（AGENTS.md §4・§7・§9・§10、plan/agents/protocol.md、plan/coding-style.md の全文、design.md §1・§2・§3・§7・§8・§10.1、`vcodec.h`・`h265sw.h`）。
- 仕様: ITU-T H.265 (V11) (01/2026)、`/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h265-202601.pdf`（SHA-256 `78b093480e178a2b99d8cb7499934240f359c50c6d09568f8be651bc1151d4d2`、git の外）。**PDF の頁 = 印刷の頁 + 18**。`pdftotext -layout -f N -l M` で読み、表と式（上付きが崩れる: 2^15 が「215」になる）は Read の tool で PDF の頁の画を見て確かめる。
- design.md の全文、関係する header（proposed/ か source）。

## 2. すること

design.md §4.1・§5.1・§5.2 の C1〜C4 を行う。proposed/common/ の header を source に置き（p026 の名前に合わせる）、H.264 の decoder の該当の code を移す。振る舞いを変えない。

## 3. 受け入れ

- WS326: `sh plan/ws326/tests/run-unit.sh all`、`run-stream.sh`、適合（JVT の `in` の全部）が前後で同じ（件数と MD5）。
- WS329: run-unit all と roundtrip の MD5 が前後で同じ。
- 新しい test-vcodec_frame（pool の limit・dead・side_init・参照の数）・test-vcodec_output（queue と retime）。
- amd64 の libmedia.so の build warning 0。WS326 の check-skeleton の BASE をこの commit に移すことを Q1 に報告。

## 4. 規則

- 範囲の外の作業をしない。計画に無い依存は Q1 に返す。`git commit -m WIP -- <path>...`、push しない、rm をしない。QEMU・実機は Q1 の割り当てで 1 本の script。console の log で判定しない。
- 他の実装の code・comment・構造・名前を写さない。source の comment に開発の管理情報を書かない。

## 結果

（担当が書く）
