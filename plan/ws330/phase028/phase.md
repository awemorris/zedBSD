<!-- awesome-plan project=zedbsd record=ws330-p028 -->

# ws330-p028 h265sw-decoder の全体、host の道具、最初の全体の試験

- 親: [ws.md](../ws.md)、設計 [design.md](../design.md)。型: O（Opus、phase-runner）。
- 状態: planned（2026-10-31 p001 が作成）。依存: p004〜p027。

## 1. 読む物

- 共通: [design.md](../design.md) §13.1 の「読む物（共通）」（AGENTS.md §4・§7・§9・§10、plan/agents/protocol.md、plan/coding-style.md の全文、design.md §1・§2・§3・§7・§8・§10.1、`vcodec.h`・`h265sw.h`）。
- 仕様: ITU-T H.265 (V11) (01/2026)、`/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h265-202601.pdf`（SHA-256 `78b093480e178a2b99d8cb7499934240f359c50c6d09568f8be651bc1151d4d2`、git の外）。**PDF の頁 = 印刷の頁 + 18**。`pdftotext -layout -f N -l M` で読み、表と式（上付きが崩れる: 2^15 が「215」になる）は Read の tool で PDF の頁の画を見て確かめる。
- design.md の全文、関係する header（proposed/ か source）。

## 2. すること

decoder.h の flow を実装し、`plan/ws330/tests/stream/decode-h265.c`（design §10.2）と `run-stream.sh` を作る。module の間の食い違いはここで出る（WS326 p021 と同じ）: header の直しが要れば Q1 の許しを得て直し、BASE を移す。他の .c の小さな直しは可（記録する）。

## 3. 受け入れ

- libx265 の組（§10.2）で自前の decoder と ffmpeg の frame ごとの MD5 が全一致、SEI の hash が全一致。
- 壊した stream 200 通りで落ちず止まらず、次の IRAP から戻る。CRA からの開始、seek（reset）、EOS。
- ASan・UBSan で全組。

## 4. 規則

- 範囲の外の作業をしない。計画に無い依存は Q1 に返す。`git commit -m WIP -- <path>...`、push しない、rm をしない。QEMU・実機は Q1 の割り当てで 1 本の script。console の log で判定しない。
- 他の実装の code・comment・構造・名前を写さない。source の comment に開発の管理情報を書かない。

## 結果

（担当が書く）
