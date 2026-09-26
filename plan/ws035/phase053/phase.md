<!-- awesome-plan project=zedbsd record=ws035p053 -->

# ws035-p053: `wl_shm`（補助の経路）と cursor

Phase ID: `ws035-p053`
Parent: [WS035](../ws.md)
Status: in-progress（q458-i01）
Phase disposition: normal
Queue: q458-i01
設計: [compositing-design.md](../compositing-design.md)（2026-09-25 承認）の D2（`wl_shm`）・D8（cursor）

## 目的

`wl_shm` 1（ARGB8888・XRGB8888）と `wl_shm_pool` を持つ（toolkit の software 描画、cursor の theme、小さな client のための補助の経路）。
cursor を zdesktop が描く（ウィンドウモードだけ）。

## 受け入れ

1. `wl_shm` の client の画像が窓として正しく出る（画面の読み取り）。pool は作成時と `resize` で 1 回だけ mmap し、commit ごとの CPU の copy は damage の範囲の行だけ。copy が終わった時点で `wl_buffer.release`。
2. cursor: zdesktop の既定の cursor（矢印）がウィンドウモードで pointer の位置に alpha で合成される。`wl_pointer.set_cursor` の `wl_shm` の surface と GPU の surface を受け、buffer の無い surface は cursor を隠す。全画面モードでは描かない（copy 0 のまま）。
3. GPU の画像の窓の frame の時間が、`wl_shm` の窓の有無で変わらない（測定）。
4. 新しい C は `plan/coding-style.md` の全文に従い、style-check の指摘 0。amd64 の build は warning 0。boot test。

## 設計（実装の決定）

- `wl_shm_pool`: client の fd（`shm_open` の後に `shm_unlink` した匿名のもの）を pool の作成時と `resize` で mmap（読み取りだけ）。`wl_buffer` は pool の中の offset・幅・高さ・stride・format。
- 窓ごとの staging buffer（HOST_VISIBLE・HOST_COHERENT、persistent map）と sampling 用の VkImage（optimal）を、shm の buffer の大きさで作る（大きさが変わるまで使い回す）。commit で damage の行を CPU で staging へ copy し、次の frame の command buffer の先頭で `vkCmdCopyBufferToImage`（damage の範囲だけ）。p053 では damage を行の範囲（最小の y から最大の y）にまとめる。
- XRGB8888 は alpha を 1 として描く（不透明の pipeline）、ARGB8888 は alpha の pipeline。
- cursor: zdesktop の既定の矢印（16×16 程度、白と黒の縁、ARGB）を起動時に作る。`set_cursor(serial, surface, hotspot)` の surface は role「cursor」になり、commit された画像を cursor に使う（shm と GPU のどちらも）。hotspot を引いた位置に最後に alpha の quad で描く。pointer が動くとウィンドウモードで描き直す（全画面の描き直し、damage は p055）。
- 試験の client: `wl_shm` で描く小さな client（`userland/base/tests/` か wltest の別の mode）と、cursor を動かす QMP の `input-send-event`（usb-tablet の絶対座標）。
