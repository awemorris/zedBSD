<!-- awesome-plan project=zedbsd record=ws330-p003 -->

# ws330-p003 H.265 の decoder の header と骨格を source に、試験の道具、BASE

- 親: [ws.md](../ws.md)、設計 [design.md](../design.md)。型: O（Opus、phase-runner）。
- 状態: planned（2026-10-31 p001 が作成）。依存: p002。

## 1. 読む物

- 共通: [design.md](../design.md) §13.1 の「読む物（共通）」（AGENTS.md §4・§7・§9・§10、plan/agents/protocol.md、plan/coding-style.md の全文、design.md §1・§2・§3・§7・§8・§10.1、`vcodec.h`・`h265sw.h`）。
- 仕様: ITU-T H.265 (V11) (01/2026)、`/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h265-202601.pdf`（SHA-256 `78b093480e178a2b99d8cb7499934240f359c50c6d09568f8be651bc1151d4d2`、git の外）。**PDF の頁 = 印刷の頁 + 18**。`pdftotext -layout -f N -l M` で読み、表と式（上付きが崩れる: 2^15 が「215」になる）は Read の tool で PDF の頁の画を見て確かめる。
- design.md の全文、関係する header（proposed/ か source）。

## 2. すること

proposed/ の h265sw*.h（と h265enc.h は p033 まで置かない）を `userland/desktop/libmedia/` に置く（p002・p026 の名前に合わせて機械的に置き換え、内容は変えない）。全 .c の骨格（ENOSYS・何もしない）、Makefile（libmedia の source の列に足す。backend の表には登録しない）、`plan/ws330/tests/run-unit.sh`・`check-skeleton.sh`（WS326 の形）、試験の補助（`plan/ws326/tests/unit/check.h` の利用、H.265 の syntax の書きの補助: vcodec-bitwriter と vcodec-cabac-enc の上の小さな関数、ただし 2 値化は各試験で書く方針を補助の comment に）、`test-bitwriter` に当たる補助の自己試験。design-reviewer の指摘で header を直す時はこの Phase で（以後は凍結）。

## 3. 受け入れ

- `check-skeleton.sh`: 各 header の単独 compile（cc と clang、c89 pedantic）と全 .c の compile、`BASE=` の照合。
- amd64 build warning 0、WS326・WS329 の試験が変わらず PASS。
- BASE の SHA を ws.md と各 phase.md に書く。

## 4. 規則

- 範囲の外の作業をしない。計画に無い依存は Q1 に返す。`git commit -m WIP -- <path>...`、push しない、rm をしない。QEMU・実機は Q1 の割り当てで 1 本の script。console の log で判定しない。
- 他の実装の code・comment・構造・名前を写さない。source の comment に開発の管理情報を書かない。

## 結果

（担当が書く）
