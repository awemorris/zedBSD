<!-- awesome-plan project=zedbsd record=ws202-p003 -->

# ws202-p003: libmedia の共通の部品と back end の表

Status: uncleared（software実装あり、全条件の確認は未完）
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 8 LW
依存: p001、H2（back end の順）、J1（container を絞るか）、J7（1 段目の譲り、仮 (a)）

## 目的

AAC と H.264 の back end が共有する部品を作り、decoder.c と `media-decoder.h` を design §9.1 の形にする。この Phase を先に merge する（ws.md の merge の順）。

## 成果（`userland/desktop/libmedia/`）

1. `bits.c`（design §7.3）: `struct media_bits`、`media_bits_init`・`_read`（1〜32 bit）・`_read1`・`_skip`・`_ue`・`_se`・`_left`・`_align`、overrun の印、
   `media_rbsp_unescape`（slice の data を除く全ての NAL の RBSP に使う、L2-04）。
2. `picture.c`（design §7.1・§7.2）:
   - `struct media_picture`（表示の幅・高さ、SAR、色 `MEDIA_COLOUR_601/709/2020`、full range、luma・chroma の pointer と pitch、参照の数、pool）。
   - pool（D11）: `media_picture_pool_create(width, height, free_max)`、`_get`（空きが無ければ malloc、尽きない）、`media_picture_ref`・`_unref`（空きが
     `free_max` 未満なら list へ、以上なら free。pool が閉じた後の最後の unref で pool を消す）、`media_picture_pool_close`。mutex は pool に 1 つ。
   - scaler: `media_picture_scale`、`media_picture_scaler_free`。BT.601・709・2020、limited・full、1:1 の速い道、bilinear（行の表の cache）。
   - ops に入れる関数（`media_picture_ops_*`、picture_aspect を含む）。
3. `sound.c`（design §6.5・§8.5）: `struct media_sound`、`media_sound_open`（入力の rate・channel の配置・`end_us`）、`_frame`（1 frame の float の planar と時刻を受け、
   先頭・末尾の切り詰め・trim・downmix をして保持）、`_convert`（保持した frame を呼び手の rate へ resample して 16 bit stereo を capacity まで。残りは捨てる）、
   `_discard`（変換されなかった frame を捨て resampler の履歴を空にする、D23）、`_trim`（before_us）、`_flush`、`_close`。
   - resampler（D13、design §6.5）: 入力と出力が同じ rate なら通さない。違えば Kaiser の windowed sinc・位相 256・位相の間の線形補間。cutoff と帯域は
     `R = min(入力, 出力)` 基準（通過域 0〜0.41R で ±0.1 dB、0.5R より上で −80 dB）。up sampling は 64 tap、down sampling（64・88.2・96 kHz → 48 kHz）は kernel を
     比で広げ tap を比で増やす（96 → 48 は 128 tap）。β と tap の数を host 試験で決めて記録。表は rate の組ごとに 1 回（cache は mutex、design §6.9）。
   - downmix: mono、stereo、3〜8 channel（PCE の matrix_mixdown か BS.775、正規化）、LFE を捨てる。
4. `media-private.h`: 上の型と関数、`struct media_decoder_ops` に open の `int degraded`、`backend`（名）、`picture_aspect`、`trim`、`frame_us`。avcodec.c は degraded を
   無視し、backend "libavcodec"、aspect 1:1、trim は ENOTSUP、frame_us は 0。
5. `media-decoder.h`: `MEDIA_PROBLEM_DEVICE 4`・`_PROFILE 5`・`_BUSY 6`、`media_decoder_backend`、`media_decoder_trim`、`media_decoder_frame_us`（D30 の pre-roll の
   seek に使う）、`media_frame_aspect`、音の約束（D23）の comment。
   試験だけの口を `media_decoder_*` の名で足さない（`exports.map` の wildcard で export されるため）。
6. `decoder.c`: 表は `{ &media_avcodec_ops }` のまま。2 段の試しと問題の選び方（design §9.1、2 段目の問題を優先する D27）。picture 系の ops が NULL の back end（音だけ）を扱う。
7. `Makefile`: 新しい source。libm の要否（`sin`・`exp`・`sqrt`・`pow`）を zedBSD の libc で確かめる。

## 手順

1. `plan/coding-style.md` の全文を読んで当てる。
2. 上を作る。avcodec.c の動作は変えない。
3. host 試験（`plan/ws202/tests/`）: `run-host-bits.sh`、`run-host-picture.sh`（pool は ring 8＋window 2＋深さ 16 の模擬で尽きない、上限を越える返却は free）、
   `run-host-sound.sh`（44.1 → 48 kHz の 1・10・18 kHz の sine と sweep、96 → 48 kHz の 1・10・18 kHz と 30・36 kHz の sine、88.2・64 → 48、48 → 48 の素通り、通過域・
   阻止域・折り返し、切り詰め、D23 の discard、trim）。decoder.c の D27 は偽の back end 2 つ（1 段目 FORMAT・2 段目 DEVICE）の host の試験で。
   `plan/tools/media/run-host-codec.sh` が新しい source を link するようにする（WS の外: 差分を Q1 へ）。

## 確認

| コマンド | 期待 |
| --- | --- |
| `sh plan/ws202/tests/run-host-bits.sh` | PASS |
| `sh plan/ws202/tests/run-host-picture.sh` | 式との差 ≦ 1、pool が尽きない、時間の行 |
| `sh plan/ws202/tests/run-host-sound.sh` | 通過域 ±0.1 dB、通過域の sine の SNR ≧ 80 dB、up の像 ≦ −80 dB、down の 30・36 kHz の折り返し ≦ −80 dB、素通りが bit 一致、切り詰め・trim の数、discard の後に履歴が空、D27 |
| `sh plan/tools/media/run-host-codec.sh` | 既存の PASS |
| design §10.6 の build（`ZEDBSD_CONFIG=config/ci/config-amd64.mk`、libmedia・libbrowser・videoplayer・music） | exit 0、`grep -c 'warning:'` が 0 |

## 注意

- 試験だけの環境変数で動作を切り替えない（coding-style §12）。
- この Phase では app の振る舞いは変わらない。


## 構造改訂と部分結果（2026-10-10）

libraryのops表はnativeだけ。avcodec opsの導入/外部ops注入/degraded引数/2段選択を廃止し、FFmpeg固有load/reason/bitstream責務をapp側へ移す。具体的移管手順はpolicyのlibraryとapp境界を正とする。 [変更理由・依存・結果](../policy-20261010.md)。旧記録は保持し、対象外の未実施条件をclearedとしない。共有投影/他担当/GitHubはQ1へpending。

## 2026-10-10 i04開始

[有限実行AAC入力](../policy-20261010.md#有限実行-codex-ws202-20261010-aac-input)のbits部分をユーザー継続指示で開始。p001のwhole clearanceを代用せず、metadata/Huffmanに必要な独立private部品だけを先に作る。picture/sound/ops/全共通部品は未完。

## 2026-10-10 i04部分結果

i04の具体partial scopeをclearedとして終了。[source/設計の具体化・host/build・C全文review・制限](../aac-input-result-20261010.md)。picture/sound/opsのwhole criteriaは未実装。 p003全体をclearedとしてcloseしない。Q1への統合・共有projectionはpending。

## 2026-10-10 自走実装の進捗

ユーザーの動画プレイヤで再生可能になるまで自走する指示により、[codex-ws202-playback](../policy-20261010.md#自走の実行承認-codex-ws202-playback)を継続中。旧degraded/LC-core-only/FFmpeg-library-backendの手順は適用しない。[AACの実PCM・共通音声の途中証拠](../aac-native-progress-20261010.md)を保存。whole Phaseのclearanceではなく、app/H.264/end_us/seek等の未完criteriaを保持する。独立sourceのみ変更、Master/共有Queue/他担当投影はQ1へpending。

## Native再生software結果（2026-10-10）

Event: `ws202-native-playback-software-20261010-p003`。Queue: [codex-ws202-playback](../policy-20261010.md#自走の実行承認-codex-ws202-playback)。

CPU NV12 pool/scaler/SAR/6色係数、continuous stereo PCM/trim、native-only backend表とapp所有境界を実装。32held pictureのclose後寿命、odd extent、独立式、resampling/seekをhost確認。全tone/band/PCE配置のmatrixは未完。

[最終source/command/結果・限界](../playback-result-20261010.md)、[Q1統合](../handoff-20261010.md)、[T1の準備済み依頼](../t1-playback-request-20261010.md)。旧第2版の手順・昔のpartial outcomeを保存し、最新記録が未実装記述の現在状態を置換する。whole criteriaを満たしたとは扱わず、Q1の意味の統合と未実施matrix/実機結果が再開条件。main/共有投影/GitHubの更新はQ1 pending。


## main統合の追記（2026-10-10）

Event: `ws202-main-integration-20261010-p003`。ユーザー「mainへの統合はあなたがやってOKです。」によりsourceと記録をmainへ統合。最新の承認済み方針・手順・確認・残件は[統合記録](../main-integration-20261010.md)と[policy](../policy-20261010.md)。上の設計時点の推奨、旧未実装/統合pendingは履歴として保存する。software出力の有無とwhole clearanceを区別する。標準readbackの依存はp017、ref-listは既存p016。p012/T1→p013/User UAT→whole p014の確認は未実施、Master/共有Board/GitHubへの投影はQ1に保持。
