<!-- awesome-plan project=zedbsd record=ws035p020 -->

# ws035-p020: 設計 — `/dev/graphics` の共通層とGPU scanoutへの引き継ぎ

Phase ID: `ws035-p020`
Parent: [WS035](../ws.md)
Status: cleared（q323-i01、2026-09-23。設計 [graphics-design.md](../graphics-design.md)、レビュー [review.md](review.md)）
Phase disposition: normal
Queue: q323（q323-i01）
実行: メインセッション（設計・敵対的レビューとも。`plan/master.md`「実行体制とQueue運用方針」）

## 目的

`/dev/graphics` の共通層（kernel内部API、通知、戻し）と、GPU scanoutへの引き継ぎを設計する。
実装は p005（共通層、pcat・pc98のbackend化）と p024（GPU scanoutへの切替えと戻し）で行う。
**この Phase ではソースを変更しない。**

## 調べた事実（2026-09-23）

- `/dev/graphics` に共通層は無い。`pcat-graphics.c`（816行）と `pc98-graphics.c`（814行）が
  同じcdevを2回書いており、`diff` は88行しかない。backendの関数表は `struct` になっておらず、
  名前に機種が入っている（`drv_pcat_graphics_backend_enter` / `drv_pc98_...`）。
- 画面の所有者が2系統ある。`/dev/graphics` の `graphics_owner`＋`kern_text_suspend()` と、
  GPUの `GPU_DISPLAY_CLAIM` のlease＋`kern_text_observe()`/`snapshot()`。**互いを知らない。**
- textの復帰も2種類ある（backendの `leave`＋`resume` と、GPUがsnapshotを描き続ける形）。
- `/dev/graphics` の利用者は `userland/X11/xzed`（`zwm`・`zterm` はxzed経由）と
  noctのbeui backend。どちらも `KERN_GRAPHICS_GET_CAPS` を見る。
- rpi4・sun4u・x68k には `/dev/graphics` が無い。

## 決めたこと

1. `include/uapi/graphics.h` は変えない。既存の利用者を壊さないため。
2. 共通層は `src/drivers/generic/graphics.c` と `include/kern/graphics.h`。
   backendは `struct drv_graphics_ops` で、`drv_gpu_ops` と同じ形（`private_data`、不透明handle）。
3. `CAP_GLYPH` は共通層が常に提供する。フォントは機種と無関係なため。
4. 画面の調停器を共通層に置き、状態は `TEXT`・`GRAPHICS`・`GPU` の3つ。
   textへ戻す責任を1か所にする。調停器を触るのはUAPIの入口だけ。
   lockの順序は「共通層のmutex → GPUのlock」の一方向。
5. p024のGPU backendはCPUで描き、`flush` でscanoutへ出す。GPUの描画エンジンは使わない。
6. p005は 1 platform = 1 登録。`backend.c` の分割はしない。

## 成果物

- [`plan/ws035/graphics-design.md`](../graphics-design.md): 設計（現状の事実、共通層、調停、
  p005・p024の分け方と受け入れ、制限7件）。
- [`review.md`](review.md): 敵対的レビュー。**8件を指摘し、7件を設計へ反映、1件を制限として記録**。

レビューで見つかった主なもの:

- **R1**: GPU backendが `CAP_GLYPH` を落とすと `xzed` が起動しなくなる（`main.c:472` が必須検査）。
  → glyphを共通層の仕事にした。
- **R2**: `/dev/graphics` のGPU backendが調停器を通ると、自分で自分を `EBUSY` で弾く。
  → 調停器はUAPIの入口だけが触る。
- **R3**: GPUのfault経路から `release` を呼ぶと、GPUのlockを持ったままtext layerへ入る。
  → faultからは呼ばず、sessionの `close` で呼ぶ。
- **R4**: 共通層のmutexとGPUのlockでABBAになる。→ lock順序を一方向に定めた。
- **R5**: shutdownの `restore_text()` がGPU所有の画面を戻せない。→ 強制解放のcallbackを置く。
- **R6**: 「boot-testが共通層の回帰確認になる」は誤り。consoleは `/dev/graphics` を通らない。
  → host fixtureと、`/dev/graphics` を実際に使う試験プログラム＋screendumpに直した。

## 受け入れ

- 設計に、共通層のAPI・調停の状態遷移・lock順序・p005とp024の分け方・受け入れ条件・制限がある。
- 敵対的レビューを行い、指摘と対応を残した。
- ソースを変更していない。
