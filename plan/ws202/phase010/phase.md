<!-- awesome-plan project=zedbsd record=ws202-p010 -->

# ws202-p010: Vulkan Video の back end (2) 表示順・seek・失敗の扱い

Status: planned
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 5 LW
依存: p009

## 目的

p009 の back end を再生に使える形に仕上げ、decoder の表の先頭に入れる（design §5.6・§5.7・§5.9・§9.1）。

## 成果（`userland/desktop/libmedia/vkvideo.c` ほか）

1. 表示順の待ち行列（design §5.6）: decode した picture を pts とともに入れ、`h264_reorder_depth` を超えたら pts の最小を receive で出す。drain
   （send の packet NULL）で全部出す。同じ pts の 2 枚目は捨てる。
2. flush（design §5.7）: 待ち行列の picture を unref、`h264_dpb_restart`、次の decode を RESET に、最初の I まで捨てる、leading の B と参照の無い
   P・B を捨てる（数を log に）。
3. 失敗（design §5.9）: status ERROR の picture は出さない（log の数え、連続 30 で EIO）、DEVICE_LOST で decoder の失敗と共有の device の「壊れた」の印、
   ETIMEDOUT、ENOMEM。
4. `decoder.c` の表を `{ &media_vkvideo_ops, &media_aac_ops, &media_avcodec_ops }` に。問題の選び方（p003）で、video の無い機械・libavcodec の無い
   時に DEVICE になることを確かめる。
5. host 試験 `run-host-vkvideo.sh` に足す: 表示順（`h264-high-b-aac.mp4` の pts が単調に増えて出る、数が AU の数と同じ）、drain、flush の後の捨て方、
   status ERROR の数え、DEVICE_LOST の後の open が新しい device を作る、decoder の close の後も window が持つ picture が使える（pool の参照）。
6. `plan/tools/media/run-host-codec.sh` の期待（host では H.264 は vkvideo が DEVICE → libavcodec の add-in）を直す（WS の外: Q1 へ差分）。

## 確認

| コマンド | 期待 |
| --- | --- |
| `sh plan/ws202/tests/run-host-vkvideo.sh` | p009 と p010 の項目が全部 PASS |
| `sh plan/tools/media/run-host-codec.sh` | PASS |
| libmedia・videoplayer・music・libbrowser の build | warning 0 |
