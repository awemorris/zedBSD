<!-- awesome-plan project=zedbsd record=ws202-p011 -->

# ws202-p011: Video Player、media-probe の音、利用者の文書

Status: planned
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 3 LW
依存: p007、p010

## 現在の適用方針

[最新ユーザー決定](../policy-20261010.md)が以下の旧第2版手順に優先する。具体的手順の改訂/reviewは未了。

## 目的

Video Player を自前の decoder に合わせ、p012 に要る音の道具と scenario を揃える（design §9.3・§10.3）。

## 成果

1. `userland/desktop/videoplayer/`:
   - `main.c` の `vp_notice`: DEVICE・PROFILE・BUSY の文、MISSING の文の変更（design §9.3）。
   - 縦横比: `media_frame_aspect` を描画の fitted の計算に。
   - log（L-04）: `VIDEOPLAYER OPEN … video=<codec>/<backend> audio=<codec>/<backend>`。`VIDEOPLAYER FRAMES shown=N time_ms=T` に `late=M` を**足す**（`time_ms` を残す）。
     1 枚目・100 枚ごとに加えて、一時停止・終わり・close の時にも 1 行。`vp_media_take` が捨てた数を数える。
   - seek（D14 改）: `media.c` の flush の後に `media_decoder_trim(sound, skip_before)` を呼び、0 なら音の frame の単位の捨てをしない（ENOTSUP なら今のまま）。
   - comment の「libavcodec が要る」の記述を直す。
2. `userland/tests/media-probe/` に音の部分（M-10 の決定、design §10.3）: `--audio-rms`（普通の `media_decoder_sound` で 16 bit・48 kHz・stereo、1024 frame ごとの左右の RMS）。
   `plan/ws202/tests/host-media-rms.c`（libmedia の source を host で compile し同じ計算）で AAC の全 stream の `X.rms` を作り `streams/` に置く。比べ: RMS < 1e-4 の frame は
   絶対差 ≦ 1e-6、他は相対差 ≦ 1e-3。試験のための口を `media_decoder_*` の名で足さない。
3. scenario（`tests/`、Q1 へ差分か許可）:
   - `tests/scenarios/apps/videoplayer/play.md`: `areas`・`paths`・目的を今の形（AAC は libmedia、MPEG-4 Part 2 は libavcodec）に。
   - 新 `h264-native.md`（5330、libavcodec 無しの image、`h264-high-b-aac.mp4` で `video=h264/vulkan-video audio=aac/libmedia`、再生・seek・終わり、`late=`）。
   - 新 `no-video-decode.md`（QEMU、libavcodec 無し、`problem=4` と notice の撮影）。
   - stream の置き場所は p010 の T1 で確かめた path（U12）。
4. 利用者の文書: `docs/reference/media-playback.md`（新、英語）: 自前で再生できる形、Vulkan Video の要る機械、AAC-LC と HE-AAC の扱い、libavcodec が入っている時の扱い、
   notice の意味、制限（`iTunSMPB` を読まない、8 個の video の context、他の Vulkan の実装の de-tile）。docs/ から plan/ へ link しない。
   `docs/reference/vulkan-video.md` の「Example program」に libmedia が利用者であることを 1 文。

## 確認

| コマンド | 期待 |
| --- | --- |
| videoplayer・media-probe の build（`config-media.mk`） | warning 0 |
| `sh plan/ws202/tests/run-host-media-rms.sh`（`host-media-rms.c` で `.rms` を作る） | 2 回作って同じ |
| image の package の一覧（`config-media.mk`） | libmedia・videoplayer・music・openssh・audiod・media-probe があり、libavcodec が無い（H-07） |

QEMU・実機は p012。


## 構造改訂と部分結果（2026-10-10）

Video Playerはapp共通adapterでtrackごとにnativeを試し、非対応ならapp所有のdlopen fallback。libmedia backend表にavcodecを入れない。native/fallback双方のframe/scaler寿命とno-FFmpeg noticeを確認する。 [変更理由・依存・結果](../policy-20261010.md)。旧記録は保持し、対象外の未実施条件をclearedとしない。共有投影/他担当/GitHubはQ1へpending。
