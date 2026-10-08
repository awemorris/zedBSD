<!-- awesome-plan project=zedbsd record=ws120-p006 -->

# ws120-p006: 努力目標 Ogg Vorbis

Parent: [WS120](../ws.md)
Status: canceled（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: 2026-10-07 の決定（m4a＋libavcodec の add-in、p008・p009）で置き換え）（旧: planning）
Disposition: canceled（2026-10-07 q831: ユーザーの決定（形式は m4a だけ、AAC は libavcodec の add-in、独自の decoder は後）で取り下げ。置き換えは [p008](../phase008/phase.md)・[p009](../phase009/phase.md)。[p001](../phase001/phase.md) の「2026-10-07 の決定と設計」）
Queue / attempts: none
Goal: D1 で Ogg Vorbis を選んだ場合、Ogg の container と Vorbis I の decoder を足し、app で再生できるようにする。
Prerequisites: p003・p005 cleared、D1・D2 の決定。ベータ1 では drop してよい（努力目標）。
Investigation bound: 4 時間。

## 範囲（D2 = A の場合）

- Ogg の page と packet、CRC、granule の位置と seek。Vorbis I の identification・comment・setup の header、codebook、floor 1（floor 0 は範囲を p001 で決める）、residue、channel coupling、逆 MDCT と窓、overlap-add。
- 外部の実装の code を参照・転記しない（Vorbis I の仕様に基づく）。
- 試験: 参照の PCM との差（p001 の数値）、壊れた入力。

## 受け入れ

- host 試験の差が規定以内、app で `.ogg` を再生し QEMU の WAV で判定。

## 検証

host 試験、QEMU、[coding-style](../../coding-style.md) の全文。

## 所有 path

decoder の library、`userland/desktop/music/`（形式の登録）、`plan/ws120/`。

## 依存・未決の判断

p003、p005、D1、D2。
