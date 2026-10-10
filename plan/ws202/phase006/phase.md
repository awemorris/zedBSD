<!-- awesome-plan project=zedbsd record=ws202-p006 -->

# ws202-p006: AAC の信号処理と back end

Status: planned
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 9 LW
依存: p004（`end_us`）、p005

## 現在の適用方針

[最新ユーザー決定](../policy-20261010.md)が以下の旧第2版手順に優先する。具体的手順の改訂/reviewは未了。

## 目的

p005 の構文の結果から PCM を作り、`media_aac_ops` として decoder の表に入れる。host の ffmpeg の decoder と精度の基準で一致させ、他の container の AAC を回帰させない。

## 成果（`userland/desktop/libmedia/`）

1. `aac-tools.c`（design §6.3）: pulse、逆量子化、PNS、M/S（**右の channel が INTENSITY_HCB・HCB2・NOISE_HCB の band には掛けない**、M-04）、intensity、TNS。
2. `aac-filterbank.c`（design §6.4）: FFT による IMDCT、sine・KBD、窓の列、overlap-add。表は `pthread_once`。
3. `aac.c` の back end（`media_aac_ops`、backend "libmedia"、codec "aac"）:
   - open: codec が AAC でなければ FORMAT。入力の形（design §3.1）: ASC（private data）、ADTS（codec 名 `adts`、最初の packet で config）、どちらも無い時は
     degraded 1 だけで track の rate・channels から LC。HE-AAC は degraded 1 だけ（H3 (a)）。`media_sound_open`。
   - send: parse → 道具 → filterbank → `media_sound_frame`（packet の pts で切り詰め・trim）。NULL は drain。読めない frame は EINVAL。
   - receive: 1 frame があれば 1（時刻は切り詰めの後の最初の sample）。前の frame が変換されていなければ `media_sound_discard`（D23）。
   - sound: `media_sound_convert`（呼び手の rate）。trim: `media_sound_trim`。flush: overlap・TNS・PNS・sound の状態を空に。close。
4. `decoder.c` の表: `{ &media_aac_ops, &media_avcodec_ops }`。J1 で container を絞る時は open が track の container を見る。
5. host 試験 `plan/ws202/tests/run-host-aac.sh`（design §10.2）:
   - 参照は実行の時に `ffmpeg -c:a aac -i X -c:a pcm_f32le -f f32le -`（decoder の指定は `-i` の前）で `build/ws202-host-aac/` に作る。
   - 比べるのは自前の切り詰めの後の float（core の rate、channel ごと、downmix の前。内部の関数を直に呼ぶ）。ffmpeg は先頭の priming だけ切り末尾は切らない
     （design §15 E4）ので、自前の末尾の切り詰めは比べる時に外す（または ffmpeg の長さで比べる）。5.1 は ffmpeg の FL FR FC LFE BL BR と自前の C・L・R・Ls・Rs・LFE を対応させる。
   - PNS・intensity の無い stream: max |差| ≦ 2^-14、RMS(差) ≦ 2^-17、sample の数が一致。
   - `aac-is-pns`: design §10.2 の band の比べ（PNS の band は energy ±1 dB、他の band は差の energy −80 dB 以下、EIGHT_SHORT の frame は frame の energy ±1 dB）。
   - 乱れの 1000 通り（seed 固定）で crash しない。2 thread の同時の open（TSan）。1 frame の decode の時間の平均（U4）。
   - D23: receive の後に sound を呼ばず次の receive → 残りが捨てられる。trim の後の最初の sample の時刻が before_us。
6. `plan/tools/media/run-host-codec.sh`（WS の外: 差分を Q1 へ）: `aac-adts.ts` を足し、TS・mkv・mp4 の AAC が自前の back end で鳴る（今の試料の TS の AAC が回帰しない）。

## 確認

| コマンド | 期待 |
| --- | --- |
| `sh plan/ws202/tests/run-host-aac.sh` | 全部の基準の中 |
| `sh plan/ws202/tests/run-host-aac-parse.sh`、`run-host-sound.sh` | PASS |
| `sh plan/tools/media/run-host-codec.sh` | PASS（AAC は libmedia の back end、TS の AAC を含む） |
| libmedia・music・videoplayer・libbrowser の build | warning 0 |

## 注意

- 基準を満たさない時は、どの道具で外れるかを stream ごとに切り分けて直す。基準を緩めない。
- ffmpeg の decoder の内部の値を読まない（出力の PCM だけ）。


## 構造改訂と部分結果（2026-10-10）

AAC-LCだけを実装。HE-AAC/非対応toolはPROFILEを返す。avcodec opsをlibraryの表へ入れず、音のfallbackはapp側。Huffman数値部分以外のparser/filterbankは未実装。 [変更理由・依存・結果](../policy-20261010.md)。旧記録は保持し、対象外の未実施条件をclearedとしない。共有投影/他担当/GitHubはQ1へpending。
