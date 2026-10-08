<!-- awesome-plan project=zedbsd record=ws120-p004 -->

# ws120-p004: decoder（MP3）

Parent: [WS120](../ws.md)
Status: canceled（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: 2026-10-07 の決定（m4a＋libavcodec の add-in、p008・p009）で置き換え）（旧: planning）
Disposition: canceled（2026-10-07 q831: ユーザーの決定（形式は m4a だけ、AAC は libavcodec の add-in、独自の decoder は後）で取り下げ。置き換えは [p008](../phase008/phase.md)・[p009](../phase009/phase.md)。[p001](../phase001/phase.md) の「2026-10-07 の決定と設計」）
Queue / attempts: none
Goal: MPEG-1/2 Audio Layer III の decoder と ID3v2 の metadata を作り、host で参照の decoder との差を規定以内にする。
Prerequisites: p001 cleared、D1 で MP3 を選んだこと、D2 の決定、p003 の library の枠。planning の理由はこれら。
Investigation bound: 4 時間。超えたら uncleared にして残りを分ける。

## 範囲（D2 = A の場合）

- frame の同期と header、Xing/Info/VBRI（長さと seek）、side information、bit reservoir、Huffman、再量子化、stereo（MS・intensity）、reorder、alias の削減、IMDCT、frequency inversion、polyphase の合成。MPEG-2/2.5 の低い sample rate。
- ID3v2.3/2.4（題・artist・album・APIC の cover）、ID3v1。
- 外部の実装の code を参照・転記しない。
- 試験: 参照の PCM（host の参照 decoder で作る）との差（p001 で決めた数値、例 RMS の差が -80 dBFS 以下）、CBR・VBR・mono・joint stereo・低い rate、壊れた入力。

## 受け入れ

- 全素材で規定の差以内、長さ・seek・metadata が合う、壊れた入力で落ちない。

## 検証

host 試験、[coding-style](../../coding-style.md) の全文、build の warning 0。

## 所有 path

decoder の library、`plan/ws120/`。

## 依存・未決の判断

p001、p003、D1、D2。
