<!-- awesome-plan project=zedbsd record=ws035p051 -->

# ws035-p051: 設計: デスクトップの合成（compositing）

Phase ID: `ws035-p051`
Parent: [WS035](../ws.md)
Status: **cleared**（q354-i01、2026-09-24。設計を提出。**ユーザーの微調整と承認を待つ**）
Phase disposition: normal
Queue: q354（q354-i01）
実行: メインセッション

## 経緯

p011（zdesktop の窓管理）は、合成の仕組みが無いことが分かって uncleared（q323-i04）。2026-09-24 ユーザー指示
「デスクトップのコンポジットは、設計をください。現時点であまり細かい指示を思いつかないので、設計を出してくれれば、微調整して OK しようと思います」。

## 成果物

[compositing-design.md](../compositing-design.md)。source は変えていない。

要点:

- D1 合成は Vulkan（textured quad、不透明と alpha の pipeline 2 本）。出力画像 3 枚は zdesktop が確保し、自分で
  `GPU_DISPLAY_PRESENT`（`BLOB`）する。libvulkan の swapchain は使わない（表示の持ち主を 1 つにし、直接 scanout と切り替えるため）。
- D2 GPU の画像は OPAQUE_FD で Vulkan に import。`wl_shm` を足し、共有メモリの画像は damage の範囲を upload。
- D3 acquire fence を commit と一緒に渡す request を足す。fence は fd にして poll に入れ、CPU で wait しない。sampling の完了は
  VkFence の fd で知り、`wl_buffer.release` を送る。
- D4 変化が無ければ描かない。1 回目は全画面の描き直し、2 回目で damage（buffer age）。表示 1 回につき最大 1 回描く。
- D5 全画面 1 枚・不透明・linear・cursor 無しのときは、今の直接 scanout（copy 0）を使う。
- D6 座標は出力の pixel。configure の既定（通常・最大化・全画面・resize 中・activated）、cascade の初期位置。
- D7 装飾は server-side が既定、当たり判定は zdesktop。D8 cursor は合成する（plane が無い）。D9 `/dev/graphics` との調停は既存の設計のまま。
- 実装の Phase の提案: p052（合成の核）→ p053（`wl_shm`・cursor）→ p054（acquire fence）→ p011（改名と窓管理）→ p055（damage）。
- 自己レビュー（敵対的な見直し）8 項目を §8 に載せた。設計の Agent を使わない運用のため。

## ユーザーのレビューの反映（2026-09-24）

ユーザーのレビュー（2 つのモード: 全画面でそのまま scanout するゲーム向けのモードと、背景の上に窓を下から順に Vulkan で描く
合成 window manager のモード。SteamOS のような構成。すりガラスの効果も可能に。`wl_shm` の CPU copy は要らないのでは）を設計へ反映した:

- D0 を新設: 全画面モード（最前面の全画面の窓、copy 0、cursor は描かない）とウィンドウモード（背景→窓を下から順）を窓の状態で明示的に切り替える。旧 D5 の自動判定は D0 に統合。
- D1: 描く順（背景→窓→タスクバー→cursor）。窓ごとの描き方を持ち、背後を読む効果は render pass を区切って載せる。
- D2: `wl_shm` を外した。CPU copy の中身（普通のメモリの画素を CPU で staging へ memcpy し、GPU で窓の画像へ copy する commit ごとの 2 段の copy）を説明し、GPU の画像だけを受ける形にした。toolkit の要否は p028。
- D8: cursor の画像は zdesktop が持つ。D10: すりガラス・影を載せる構造。
- Phase の提案: p052 を 2 つのモードの核に、p053 を cursor に改め、p057（効果）を足した。

## 2 回目のレビューの反映（2026-09-24）

ユーザーの再レビュー（`wl_shm` は CPU の copy の補助経路として持つ、ただし主要な Vulkan の経路の速さを最適化する。
D1 は swapchain を使い、全画面モードでは swapchain を破棄・停止して直接 scanout。切替に時間がかかってよい）を反映した:

- D1: ウィンドウモードは `VK_KHR_display` の swapchain で出す。旧 D1（自前の出力画像 3 枚を `GPU_DISPLAY_PRESENT`）の理由は frame ごとの切替だったので、D0 の明示的な切替で不要になった。
- D0: 全画面モードへの切替の手順（`vkDeviceWaitIdle` → swapchain と surface の破棄で lease を返す → zdesktop が claim → client の画像を import して present）と戻る手順。切替の隙間の console は p052 で確かめる。
- D2: GPU の画像の最適化の表（import は buffer ごとに 1 回、全画面の import も 1 回、1 frame 1 submit、CPU で待たない、変化が無ければ描かない、release を早く返す）。`wl_shm` を補助の経路として持ち、damage の範囲の行だけを CPU で copy する。
- D8: `set_cursor` は shm・GPU の surface を受ける。p053 を `wl_shm` と cursor に戻した。

## 承認をもらいたい点

設計の §10 の 8 項目（レビューの反映後）。承認までは p052〜p055 を ws.md に「提案」として置き、Queue に入れない。


## 承認（2026-09-25）

ユーザー「承認します」（§10 の 1〜8）。p052〜p057 を secondary queue sq001 に入れた。
