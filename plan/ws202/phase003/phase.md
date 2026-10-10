<!-- awesome-plan project=zedbsd record=ws202-p003 -->

# ws202-p003: libmedia の共通の部品と back end の表

Status: planned
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 7 LW
依存: p001、H2（back end の順）、J1（container を絞るか）

## 現在の適用方針

[最新ユーザー決定](../policy-20261010.md)が以下の旧第2版手順に優先する。具体的手順の改訂/reviewは未了。

## 目的

AAC と H.264 の back end が共有する部品を作り、decoder.c と `media-decoder.h` を design §9.1 の形にする。この Phase を先に merge する（ws.md の merge の順）。

## 成果（`userland/desktop/libmedia/`）

1. `bits.c`（design §7.3）: `struct media_bits`、`media_bits_init`・`_read`（1〜32 bit）・`_read1`・`_skip`・`_ue`・`_se`・`_left`・`_align`、overrun の印、
   `media_rbsp_unescape`。
2. `picture.c`（design §7.1・§7.2）:
   - `struct media_picture`（表示の幅・高さ、SAR、色 `MEDIA_COLOUR_601/709/2020`、full range、luma・chroma の pointer と pitch、参照の数、pool）。
   - pool（D11）: `media_picture_pool_create(width, height, free_max)`、`_get`（空きが無ければ malloc、尽きない）、`media_picture_ref`・`_unref`（空きが
     `free_max` 未満なら list へ、以上なら free。pool が閉じた後の最後の unref で pool を消す）、`media_picture_pool_close`。mutex は pool に 1 つ。
   - scaler: `media_picture_scale`、`media_picture_scaler_free`。BT.601・709・2020、limited・full、1:1 の速い道、bilinear（行の表の cache）。
   - ops に入れる関数（`media_picture_ops_*`、picture_aspect を含む）。
3. `sound.c`（design §6.5・§8.5）: `struct media_sound`、`media_sound_open`（入力の rate・channel の配置・`end_us`）、`_frame`（1 frame の float の planar と時刻を受け、
   先頭・末尾の切り詰め・trim・downmix をして保持）、`_convert`（保持した frame を呼び手の rate へ resample して 16 bit stereo を capacity まで。残りは捨てる）、
   `_discard`（変換されなかった frame を捨て resampler の履歴を空にする、D23）、`_trim`（before_us）、`_flush`、`_close`。
   - resampler（D13）: 入力と出力が同じ rate なら通さない。違えば 64 tap・Kaiser・位相 256・位相の間の線形補間。Kaiser の β は通過域 ±0.1 dB・阻止域 −80 dB を
     満たす値を host 試験で決めて記録。filter の表は `pthread_once` で 1 回（design §6.9）。rate ごとの表が要るなら rate の組で cache（mutex）。
   - downmix: mono、stereo、3〜8 channel（PCE の matrix_mixdown か BS.775、正規化）、LFE を捨てる。
4. `media-private.h`: 上の型と関数、`struct media_decoder_ops` に open の `int degraded`、`backend`（名）、`picture_aspect`、`trim`。avcodec.c は degraded を無視し、
   backend "libavcodec"、aspect 1:1、trim は ENOTSUP。
5. `media-decoder.h`: `MEDIA_PROBLEM_DEVICE 4`・`_PROFILE 5`・`_BUSY 6`、`media_decoder_backend`、`media_decoder_trim`、`media_frame_aspect`、音の約束（D23）の comment。
   試験だけの口を `media_decoder_*` の名で足さない（`exports.map` の wildcard で export されるため）。
6. `decoder.c`: 表は `{ &media_avcodec_ops }` のまま。2 段の試しと問題の選び方（design §9.1）。picture 系の ops が NULL の back end（音だけ）を扱う。
7. `Makefile`: 新しい source。libm の要否（`sin`・`exp`・`sqrt`・`pow`）を zedBSD の libc で確かめる。

## 手順

1. `plan/coding-style.md` の全文を読んで当てる。
2. 上を作る。avcodec.c の動作は変えない。
3. host 試験（`plan/ws202/tests/`）: `run-host-bits.sh`、`run-host-picture.sh`（pool は ring 8＋window 2＋深さ 16 の模擬で尽きない、上限を越える返却は free）、
   `run-host-sound.sh`（1・10・18 kHz の sine と sweep の 44.1 → 48 kHz、48 → 48 の素通り、通過域・阻止域、切り詰め、D23 の discard、trim）。
   `plan/tools/media/run-host-codec.sh` が新しい source を link するようにする（WS の外: 差分を Q1 へ）。

## 確認

| コマンド | 期待 |
| --- | --- |
| `sh plan/ws202/tests/run-host-bits.sh` | PASS |
| `sh plan/ws202/tests/run-host-picture.sh` | 式との差 ≦ 1、pool が尽きない、時間の行 |
| `sh plan/ws202/tests/run-host-sound.sh` | 通過域 ±0.1 dB、1・10・18 kHz の SNR ≧ 80 dB、画像 ≦ −80 dB、素通りが bit 一致、切り詰め・trim の数、discard の後に履歴が空 |
| `sh plan/tools/media/run-host-codec.sh` | 既存の PASS |
| `make -j16 BUILD=build/<担当> ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_USER_PROGRAMS="libmedia videoplayer music libbrowser" …` の成果 | warning 0 |

## 注意

- 試験だけの環境変数で動作を切り替えない（coding-style §12）。
- この Phase では app の振る舞いは変わらない。


## 構造改訂と部分結果（2026-10-10）

libraryのops表はnativeだけ。avcodec opsの導入/外部ops注入/degraded引数/2段選択を廃止し、FFmpeg固有load/reason/bitstream責務をapp側へ移す。具体的移管手順はpolicyのlibraryとapp境界を正とする。 [変更理由・依存・結果](../policy-20261010.md)。旧記録は保持し、対象外の未実施条件をclearedとしない。共有投影/他担当/GitHubはQ1へpending。
