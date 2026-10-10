<!-- awesome-plan project=zedbsd record=ws202-p003 -->

# ws202-p003: libmedia の共通の部品と back end の表

Status: planned
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 6 LW
依存: p001、H2（back end の順）

## 目的

AAC と H.264 の back end が共有する部品を作り、decoder.c の表と問題の選び方を design §9.1 の形にする。この Phase の後、AAC の列と H.264 の列が
並べて進められる。

## 成果（`userland/desktop/libmedia/`）

1. `bits.c`（design §7.3）: `struct media_bits`（data・size・bit の位置・overrun）、`media_bits_init`・`_read`（1〜32 bit）・`_read1`・`_skip`・
   `_ue`・`_se`・`_left`・`_align`、`media_rbsp_unescape`（00 00 03 の除去、出力の buffer は呼び手）。
2. `picture.c`（design §7.1・§7.2）:
   - `struct media_picture`（表示の幅・高さ、SAR、色の行列 `MEDIA_COLOUR_601/709/2020`、full range、luma・chroma の pointer と pitch、参照の数、pool）。
   - pool: `media_picture_pool_create(width, height, count)`、`_get`（空きが無ければ count まで malloc）、`media_picture_ref`・`_unref`
     （0 で pool に戻す。pool が閉じた後の最後の unref で pool を消す）、`media_picture_pool_close`。mutex は pool に 1 つ。
   - scaler: `media_picture_scale(picture, &scaler_state, pixels, stride, width, height)`、`media_picture_scaler_free`。固定小数点の BT.601・709・2020、
     limited・full、1:1 の速い道、bilinear（行の表を scaler に cache、大きさが変わったら作り直す）。
   - 自前の back end の ops の picture_free・picture_size・picture_scale・scaler_free に入れる関数（`media_picture_ops_*`）。
3. `sound.c`（design §6.5）: `struct media_sound`（入力の rate・channel 数と配置、出力 48 kHz stereo、resampler の位相と履歴、持ち越しの buffer）、
   `media_sound_open`・`_push`（float の planar の frame と時刻を受け、先頭・末尾の切り詰め・downmix・resample）・`_take`（16 bit stereo を
   capacity まで、残りは持ち越す）・`_flush`（seek）・`_close`。downmix の係数（ITU-R BS.775、PCE の matrix_mixdown）、polyphase の windowed sinc
   （32 tap、Kaiser β は 8.6 程度で帯域内の SNR 90 dB を満たす値を host 試験で決める、位相 256）。
4. `media-private.h`: 上の型と関数の宣言、`struct media_decoder_ops` の open に `int degraded` を足す。avcodec.c の open は引数を受けて無視する。
5. `media-decoder.h`: `MEDIA_PROBLEM_DEVICE 4`・`MEDIA_PROBLEM_PROFILE 5`、`media_decoder_backend`、`media_frame_aspect`（design §9.1）。
   `struct media_decoder_ops` に `backend`（名）と `picture_aspect` を足す（avcodec は "libavcodec" と 1:1）。
6. `decoder.c`: 表は今は `{ &media_avcodec_ops }` のまま（自前の back end は各列の Phase で足す。足す時に表の順を design §9.1 にする）。
   2 段の試し（degraded 0 → 1）と問題の選び方（design §9.1）を実装する。
7. `Makefile`: 新しい source を `LIBMEDIA_SOURCES` に。comment の「decoder は libavcodec だけ」の記述を直す。libm が要るなら（sinc・KBD の `sin`・
   `exp`・`sqrt`）NEEDED と package の依存を確かめる（zedBSD の libc に libm が含まれるかを先に確かめる）。

## 手順

1. `plan/coding-style.md` の全文を読み、新しい file に当てる（ANSI C の宣言、1 行 1 つの意味、comment、関数の頭の宣言の群、goto は cleanup だけ）。
2. 上の 1〜7 を作る。avcodec.c の動作は変えない（open の引数だけ）。
3. host 試験（`plan/ws202/tests/`）: `host-bits.c`・`run-host-bits.sh`、`host-picture.c`・`run-host-picture.sh`、`host-sound.c`・`run-host-sound.sh`
   （design §10.2 の表の基準）。`run-host-codec.sh`（`plan/tools/media/`）が新しい source を link するように直す（main の checkout の
   `plan/tools/` は WS の外の file なので、Q1 に差分を送るか、Q1 の許可を受けて直す）。

## 確認

| コマンド | 期待 |
| --- | --- |
| `sh plan/ws202/tests/run-host-bits.sh` | PASS（ASan/UBSan） |
| `sh plan/ws202/tests/run-host-picture.sh` | 式との差 ≦ 1、pool の参照の数、時間の行（1080p→1080p、→720p） |
| `sh plan/ws202/tests/run-host-sound.sh` | 1 kHz の 44.1→48 kHz の SNR ≧ 90 dB、downmix の係数、切り詰めの数 |
| `sh plan/tools/media/run-host-codec.sh` | 既存の PASS を保つ |
| `make -j16 BUILD=build/<担当> ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_USER_PROGRAMS="libmedia videoplayer music" build/<担当>/dynamic/libmedia.so build/<担当>/bin/videoplayer build/<担当>/bin/music` | warning 0 |

## 注意

- 試験だけの環境変数で動作を切り替えない（coding-style §12）。back end を選ぶ試験は ops を直に呼ぶ。
- この Phase では app の振る舞いは変わらない（表に自前の back end がまだ無い）。
