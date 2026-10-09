<!-- awesome-plan project=zedbsd record=ws001-p050 -->

# ws001-p050: complex.h と tgmath.h

Status: planning（2026-10-09 Q1、ベータ3 以降、未割当。ユーザーの WS001 の「上限 4 LW で止める」があるので Q1 の割当まで着手しない）
Parent: [WS001](../ws.md)
Queue: なし

## 経緯

[ws001-p045](../phase045/phase.md)（POSIX.1-2024 の header の全数の照合）で、宣言も実装も無いと分かった関数のうち、大きい物を分けた（2026-10-09 Q1 承認）。照合の表は `plan/ws001/tests/posix-headers/`（`check.py`）。

## 範囲

<complex.h>（複素数の数学関数 約 70、I・CMPLX ほか）と、それを使う compiler の <tgmath.h> が通ること。

## 進め方の案

libm（src/libc の math）への複素数の関数の追加。精度の基準と試験。足した後に `check.py` で該当の名前が無い物の列から消えることを確かめる。

## 受け入れ

- 該当の名前が x86_64・i386・aarch64 で宣言され、実装がある（または規格の許す option の宣言で持たないと明示）。
- build warning 0、host 試験、必要なら guest の試験は T1。
