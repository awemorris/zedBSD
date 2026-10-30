<!-- awesome-plan project=zedbsd record=ws306-user-questions -->

# WS306 ユーザーに聞く点（p001 第 3 版、2026-10-31 P5）

Parent: [WS306](ws.md)、設計 [design.md](design.md)。

**どの WS か**: WS306「GPU の driver の枠組みの『ゲーム最適化』の UAPI と driver の識別」。ゲームが環境変数で、GPU の driver の最適化の ON/OFF と
細かい option を自分の process（の GPU の session）だけに指定でき、GPU の名前・driver の版・option の一覧を取れるようにする WS。
**今の状態**: p001（設計）の第 3 版を書いた。Q306-1〜6 は 2026-10-29 に回答済み（環境変数の名前・書式・知らない名前の扱い・一覧の関数・
desktop には描画の最適化を入れない・WS291 の投入の経路の改善は全部に有効）。
**決まっていない事**: 下の 2 つ（Q306-7・Q306-9）。どちらも推しで進められる（推しのままなら Phase の表もそのまま）。Q306-8 は設計のレビューで前提が誤りと分かり取り下げた（下に記録）。

用語:
- **NGG**: AMD の GPU（RDNA）の頂点の処理の新しい形（primitive shader）。見えない三角形を早く落とす「culling」と組み合わせると速い。WS291 で入れた。
- **boot の行**: loader の設定 file（zedbsd.cfg）に書く kernel の引数。`amdrdna.vk.ngg=0/1/2` は amdrdna の driver が起動の時に読み、GPU 全体で NGG を使うかを決める（今の既定は 0 = 使わない）。/bin/wayland の設定ではない。
- **session**: 1 つの program が `/dev/gpuN` を開いた 1 回分。ゲーム最適化の設定は session ごと。

## Q306-7 boot の行 `amdrdna.vk.ngg` を残すか

- 背景: WS306 の後、ゲーム（ゲーム最適化が ON の session）では NGG と culling を option（`ngg`・`ngg_cull`、既定 ON）で決め、boot の行は見ない。
  desktop など OFF の session は、2026-10-29 の回答（desktop には描画の最適化を入れない）どおり NGG を使わない。boot の行を残すと、開発者が明示した時だけ OFF の session にも NGG を強制できる（WS291 の測り・不具合の切り分けに使っている）。
- 推し: **(a) 診断用に残す**。既定 0（desktop には入れない）のまま、書いた時だけ OFF の session に効く。文書に「診断用」と書く。
- 他の案: (b) 消す（OFF の session は常に NGG なし。測りは全部 `VULKAN_ZEDBSD_GAME=1` で行う）。

## Q306-8（取り下げ）option を関数でも設定できるようにするか

- 取り下げた理由（2026-10-31 P5、design-reviewer の第 2 回の M5）: 問いの前提「setenv は thread の動いている program では安全でない」は POSIX の一般論で、
  zedBSD の libc は getenv・setenv を lock で守っているので zedBSD では安全だった。環境変数だけで足りるので、2026-10-27 の決定（環境変数）のまま進める。
  他の OS に移す code の注意（thread を作る前に設定する）は文書に書く。ユーザーに聞く必要は無い。

## Q306-9 WS291 の完了を待たずに枠組みの Phase を進めるか

- 背景: 2026-10-27・28 の決定は「WS306 は WS291 で RDNA の最適化をやりきった後に、要素を option に移して実装し直す作業」。WS291 の RDNA の段は RX 5600 XT が抜かれて止まっている（2026-10-30）。
  WS306 のうち、UAPI・核・各 driver の名前と版・libvulkan・vulkaninfo・枠組みの文書（p002〜p006・p008・p009）は WS291 の成果に依存しない。依存するのは amdrdna の option の作り直し（p007）とその文書（p010）だけ。
- 費用: (a) では、核の Phase（p003）が kernel の GPU の共通の file（gpu.c）を、WS291 の job の ABI の案（gpu.c の分岐と job の口）や WS250（GPU の仕事の取り消しと復帰、実装中）と同じ時期に変えるので、merge の時の衝突の手直しが出うる（数 LW の見込み）。
- 推し: **(a) 枠組みの Phase（p002〜p006・p008・p009）は先に進め、p007（NGG の option 化）と p010 だけ WS291 の RDNA の完了を待つ**。衝突は Q1 が merge の順で抑える。
- 他の案: (b) WS306 の全部を WS291 の RDNA の完了の後に始める（決定の文のまま）。
