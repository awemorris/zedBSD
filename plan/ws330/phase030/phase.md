<!-- awesome-plan project=zedbsd record=ws330-p030 -->

# ws330-p030 libmedia の backend（共通の backend の核）

- 親: [ws.md](../ws.md)、設計 [design.md](../design.md)。型: O（Opus、phase-runner）。
- 状態: planned（2026-10-31 p001 が作成）。依存: p029、WS318 completed。

## 1. 読む物

- 共通: [design.md](../design.md) §13.1 の「読む物（共通）」（AGENTS.md §4・§7・§9・§10、plan/agents/protocol.md、plan/coding-style.md の全文、design.md §1・§2・§3・§7・§8・§10.1、`vcodec.h`・`h265sw.h`）。
- 仕様: ITU-T H.265 (V11) (01/2026)、`/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h265-202601.pdf`（SHA-256 `78b093480e178a2b99d8cb7499934240f359c50c6d09568f8be651bc1151d4d2`、git の外）。**PDF の頁 = 印刷の頁 + 18**。`pdftotext -layout -f N -l M` で読み、表と式（上付きが崩れる: 2^15 が「215」になる）は Read の tool で PDF の頁の画を見て確かめる。
- design.md の全文、関係する header（proposed/ か source）。

## 2. すること

WS318 の後の `h264sw-backend.c` から codec に依らない部分を共通の核に出し（D21）、`h265sw-backend.c` をその上に。decoder.c の表で vulkan-video の後・avcodec の前。問い合わせの行。

## 3. 受け入れ

- WS318 の decoder の層の host 試験、H.264 の backend の試験が前後で同じ。
- run-host-decode で MP4・MKV・TS の HEVC が h265-software で ffmpeg と一致。
- amd64 build warning 0、QEMU の Video Player で HEVC の再生（console の log で判定しない）。

## 4. 規則

- 範囲の外の作業をしない。計画に無い依存は Q1 に返す。`git commit -m WIP -- <path>...`、push しない、rm をしない。QEMU・実機は Q1 の割り当てで 1 本の script。console の log で判定しない。
- 他の実装の code・comment・構造・名前を写さない。source の comment に開発の管理情報を書かない。

## 結果

（担当が書く）
