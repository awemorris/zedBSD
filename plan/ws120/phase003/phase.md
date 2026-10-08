<!-- awesome-plan project=zedbsd record=ws120-p003 -->

# ws120-p003: decoder（WAV・FLAC）

Parent: [WS120](../ws.md)
Status: canceled（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: 2026-10-07 の決定（m4a＋libavcodec の add-in、p008・p009）で置き換え）（旧: planning）
Disposition: canceled（2026-10-07 q831: ユーザーの決定（形式は m4a だけ、AAC は libavcodec の add-in、独自の decoder は後）で取り下げ。置き換えは [p008](../phase008/phase.md)・[p009](../phase009/phase.md)。[p001](../phase001/phase.md) の「2026-10-07 の決定と設計」）
Queue / attempts: none
Goal: decoder の library の枠と、WAV・FLAC の decoder を作り、host で参照の PCM と bit で一致させる。
Prerequisites: p001 cleared、D1・D2 の決定（既定案は独自実装）。planning の理由はこれら。
Investigation bound: 4 時間。

## 範囲（D2 = A の場合）

- library の枠（p001 の設計の置き場所と API、3 OS の Makefile、exports.map）。
- WAV: RIFF/WAVE、PCM 8/16/24/32 bit、IEEE float、WAVE_FORMAT_EXTENSIBLE、LIST/INFO の metadata。
- FLAC（RFC 9639）: STREAMINFO・VORBIS_COMMENT・PICTURE・SEEKTABLE、frame の header と CRC、constant・verbatim・fixed・LPC の subframe、Rice の residual、stereo の decorrelation、seek。
- 外部の実装の code を参照・転記しない（仕様書に基づく）。
- `plan/ws120/tests/`: 素材（p001 の方法）と参照の PCM、壊れた file・切れた file で落ちない試験。

D2 が B・C の場合は、この Phase を「選んだ package の取得・検証・監査と組み込み」に書き直す（範囲の変更として記録）。

## 受け入れ

- WAV・FLAC の全素材で参照の PCM と bit で一致、metadata が合う、seek の位置が規定以内、壊れた入力で落ちない（fuzz の短い試験を含む）。

## 検証

host 試験、新しい C は [coding-style](../../coding-style.md) の全文、build の warning 0。

## 所有 path

decoder の library（p001 で決める。例 `userland/base/libaudio-decode/`）、`plan/ws120/`。

## 依存・未決の判断

p001、D1、D2。
