<!-- awesome-plan project=zedbsd record=ws347-p007 -->

# ws347-p007 直した RTL で bitstream を作り直す

Status: canceled

- 親: [WS347](../ws.md)。状態: canceled（2026-11-11 12:37、RTL はユーザーが直す）。

## 範囲

- p006 の後の amaterrace-gpu main を minimal で Vivado の build。WNS ≥ 0 と LUT の量を記録。閉じなければ strategy を探る（debug の core 無し、
  Vivado の版と strategy を build の tcl に固定する）。
- KC705 に書いて VF2 から CAPS の 3 つの register を読み、仕様の値と照合。

## 受け入れ

- タイミングの閉じた bitstream（WNS ≥ 0）、書き込みの後の CAPS の値が p006 の tb の値と一致。
