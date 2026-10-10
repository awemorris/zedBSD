<!-- awesome-plan project=zedbsd record=ws202-p006 -->

# ws202-p006: AAC の信号処理と back end

Status: planned
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 10 LW
依存: p004（`end_us`）、p005

## 目的

p005 の構文の結果から PCM を作り、`media_aac_ops` として decoder の表に入れる。host の ffmpeg の decoder と精度の基準で一致させ、他の container の AAC を回帰させない。

## 成果（`userland/desktop/libmedia/`）

1. `aac-tools.c`（design §6.3）: pulse、逆量子化、PNS、M/S（**右の channel が INTENSITY_HCB・HCB2・NOISE_HCB の band には掛けない**、M-04）、intensity、TNS。
2. `aac-filterbank.c`（design §6.4）: FFT による IMDCT、sine・KBD、窓の列、overlap-add。表は `pthread_once`。
3. `aac.c` の back end（`media_aac_ops`、backend "libmedia"、codec "aac"）:
   - open: codec が AAC でなければ FORMAT。入力の形（design §3.1）: ASC（private data）、ADTS（codec 名 `adts`、最初の packet で config）、どちらも無い時は
     degraded 1 だけで track の rate・channels から LC。HE-AAC は degraded 1 だけ（H3 (a)）。**D26（仮、J7 (a)）: ADTS と core の rate 24 kHz 以下の LC は degraded 0 で FORMAT**。
     AVI の LATM の tag（`0x706d`）は FORMAT（`avi.c` 710 付近で codec 名を確かめる、U19）。`media_sound_open`。
   - send: parse → 道具 → filterbank → `media_sound_frame`（packet の pts で切り詰め・trim）。ADTS の複数の block は decode した frame を最大 4 つの列に入れる（満ちていれば
     EAGAIN、L2-08）。NULL は drain。読めない frame は EINVAL。
   - **pre-roll（D30、M2-10）**: flush の後の最初の frame は decode して overlap を作るだけで出さない（receive は返さない、pts は使わない）。
   - receive: 1 frame があれば 1（時刻は切り詰めの後の最初の sample）。前の frame が変換されていなければ `media_sound_discard`（D23）。
   - sound: `media_sound_convert`（呼び手の rate）。trim: `media_sound_trim`。frame_us: 1024 ÷ core の rate（µs）。flush: overlap・TNS・PNS・sound の状態を空にし、pre-roll の印を立てる。close。
4. `decoder.c` の表: `{ &media_aac_ops, &media_avcodec_ops }`。J1 で container を絞る時は open が track の container を見る。
5. host 試験 `plan/ws202/tests/run-host-aac.sh`（design §10.2）:
   - 参照は実行の時に `ffmpeg -c:a aac -i X -c:a pcm_f32le -f f32le -`（decoder の指定は `-i` の前）で `build/ws202-host-aac/` に作る。
   - 比べるのは自前の切り詰めの後の float（core の rate、channel ごと、downmix の前。内部の関数を直に呼ぶ）。ffmpeg は先頭の priming だけ切り末尾は切らない
     （design §15 E4）ので、自前の末尾の切り詰めは比べる時に外す（または ffmpeg の長さで比べる）。5.1 は ffmpeg の FL FR FC LFE BL BR と自前の C・L・R・Ls・Rs・LFE を対応させる。
   - PNS・intensity の無い stream: max |差| ≦ 2^-14、RMS(差) ≦ 2^-17、sample の数が一致。
   - `aac-is-pns`: design §10.2 の band の比べ（PNS の band は energy ±1 dB、他の band は差の energy −80 dB 以下、EIGHT_SHORT の frame は frame の energy ±1 dB）。
   - 乱れの 1000 通り（seed 固定）で crash しない。2 thread の同時の open（TSan）。1 frame の decode の時間の平均（U4）。
   - D23: receive の後に sound を呼ばず次の receive → 残りが捨てられる。trim の後の最初の sample の時刻が before_us。
   - pre-roll（D30）: 各 stream の 5 つの時刻で flush と「目標 − 1 frame から読み、trim は目標」をし、出た最初の 1024 sample が通しの decode の同じ sample と
     max |差| ≦ 2^-14。
6. `plan/tools/media/run-host-codec.sh`（WS の外: 差分を Q1 へ）: `aac-adts.ts` を足す。今の試料の TS は `h264-mp3.ts` だけで AAC の TS は無いので、TS の AAC を確かめるのは
   この新しい試料だけ（L2-06）。host は libavcodec があるので、D26 で `aac-adts.ts` と 22.05 kHz の LC は libavcodec、mp4・mkv の 44.1・48 kHz の LC は自前が decode する
   ことを期待にする。libavcodec の無い形（2 段目で自前）は `run-host-aac-parse.sh`・`run-host-aac.sh` が aac の ops を degraded 1 で直に呼んで確かめる（環境変数・試験だけの build を使わない）。

## 確認

| コマンド | 期待 |
| --- | --- |
| `sh plan/ws202/tests/run-host-aac.sh` | 全部の基準の中 |
| `sh plan/ws202/tests/run-host-aac-parse.sh`、`run-host-sound.sh` | PASS |
| `sh plan/tools/media/run-host-codec.sh` | PASS（mp4・mkv の 44.1・48 kHz の AAC は libmedia、ADTS と 22.05 kHz は libavcodec） |
| design §10.6 の build（`ZEDBSD_CONFIG=config/ci/config-amd64.mk`、libmedia・libbrowser・videoplayer・music） | exit 0、`grep -c 'warning:'` が 0 |

## 注意

- 基準を満たさない時は、どの道具で外れるかを stream ごとに切り分けて直す。基準を緩めない。
- ffmpeg の decoder の内部の値を読まない（出力の PCM だけ）。
