<!-- awesome-plan project=zedbsd record=ws202-p006 -->

# ws202-p006: AAC の信号処理と back end

Status: planned
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 8 LW
依存: p004（`end_us`）、p005

## 目的

p005 の構文の結果から PCM を作り、`media_aac_ops` として decoder の表に入れる。host の ffmpeg の decoder と精度の基準で一致させる。

## 成果（`userland/desktop/libmedia/`）

1. `aac-tools.c`（design §6.3 の順）: pulse の加算、逆量子化（`|q|^(4/3)` の表 8192 個と `2^(0.25·(sf−100))`、init で計算）、PNS（自前の LCG、
   band の energy に合わせる、CPE の ms_used の band は同じ雑音）、M/S、intensity（position の `2^(-0.25·pos)` と符号）、TNS（係数の逆量子化、
   parcor から LPC へ、上向き・下向きの all-pole の filter、long は order ≦ 12・short は ≦ 7）。
2. `aac-filterbank.c`（design §6.4）: N/4 点の複素 FFT（radix-2、bit 反転、twiddle は init）による IMDCT（2048・256）、sine・KBD（α 4・6、I0 の級数）の窓、
   ONLY_LONG・LONG_START・EIGHT_SHORT・LONG_STOP、前の window_shape、overlap-add の保持。
3. `aac.c` の back end（`media_aac_ops`、backend 名 "libmedia"、codec 名 "aac"）:
   - open: codec が AAC でなければ FORMAT。private data（ASC）を読む（無ければ sample entry の rate・channels で LC）。p005 の判断で PROFILE・
     degraded。`media_sound_open`（入力 rate = core の rate、channel の配置、`end_us`）。
   - send: frame を parse（p005）→ 道具 → filterbank → `media_sound_push`（packet の pts で先頭・末尾の切り詰め）。packet が NULL なら drain の印。
     読めない frame は EINVAL（play.c は連続 16 回で曲を止める）。
   - receive: push した frame が 1 つあれば 1（時刻は packet の pts を切り詰めた後の最初の sample の時刻）、無ければ 0。
   - sound: `media_sound_take`。flush: overlap・TNS・PNS の状態と `media_sound_flush`。close。
   - picture 系の ops は NULL（音だけ）。decoder.c が NULL を見て呼ばないことを確かめる（必要なら decoder.c の側で NULL を断る）。
4. `decoder.c` の表: `{ &media_aac_ops, &media_avcodec_ops }`（p010 で vkvideo を先頭に足す）。
5. host 試験 `plan/ws202/tests/host-aac.c`・`run-host-aac.sh`（design §10.2）: AAC の全 stream を `media_decoder_*` で decode し、core の rate・channel ごとの
   float（downmix・resample の前。試験のために back end の内部の関数 `aac_decode_frame` を直に呼ぶ形でよい）を、host の ffmpeg の
   `-c:a aac -f f32le` の出力（実行の時に `build/ws202-host-aac/` に作る）と比べる:
   - PNS の無い stream: 各 channel で max |差| ≦ 2^-14、RMS(差) ≦ 2^-17、sample の総数が一致。
   - `aac-tools-low`（PNS あり）: 1024 sample ごとの RMS の比が ±1 dB 以内、PNS の無い band だけの比べは不要。
   - 1 frame の decode の時間の平均を表示（U4）。
   - 誤った入力（切った frame、乱れた bit）で crash しない（ASan/UBSan、乱れの 1000 通り、seed を固定）。
6. `sh plan/tools/media/run-host-codec.sh` が AAC を自前の back end で通すことを確かめ、期待の行を直す（WS の外の file: Q1 へ差分）。

## 確認

| コマンド | 期待 |
| --- | --- |
| `sh plan/ws202/tests/run-host-aac.sh` | 全 stream で基準の中、PNS の stream は ±1 dB、乱れの 1000 通りで無事 |
| `sh plan/ws202/tests/run-host-aac-parse.sh`、`run-host-sound.sh` | PASS |
| `sh plan/tools/media/run-host-codec.sh` | PASS（AAC は libmedia の back end） |
| libmedia・music・videoplayer の build | warning 0 |

## 注意

- 基準を満たさない時は、どの道具（窓の切り替え、TNS、intensity 等）で外れるかを stream ごとに切り分けて直す。基準を緩めない（U6 の記録は design）。
- ffmpeg の decoder の内部の値を読まない（出力の PCM だけを比べる）。
