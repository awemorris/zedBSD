<!-- awesome-plan project=zedbsd record=ws001-p048 -->

# ws001-p048: netdb の network・protocol の database

Status: planning（2026-10-09 Q1、ベータ3 以降、未割当。ユーザーの WS001 の「上限 4 LW で止める」があるので Q1 の割当まで着手しない）
Parent: [WS001](../ws.md)
Queue: なし

## 経緯

[ws001-p045](../phase045/phase.md)（POSIX.1-2024 の header の全数の照合）で、宣言も実装も無いと分かった関数のうち、大きい物を分けた（2026-10-09 Q1 承認）。照合の表は `plan/ws001/tests/posix-headers/`（`check.py`）。

## 範囲

getprotobyname・getprotobynumber・getprotoent・setprotoent・endprotoent と struct protoent、getnetbyaddr・getnetbyname・getnetent・setnetent・endnetent と struct netent、gethostent（netdb.h）。

## 進め方の案

/etc/protocols・/etc/networks の file（base の etc に足す）と読み手、hosts の列挙。resolver（userland/base/libc/resolver*.c）との関係。足した後に `check.py` で該当の名前が無い物の列から消えることを確かめる。

## 受け入れ

- 該当の名前が x86_64・i386・aarch64 で宣言され、実装がある（または規格の許す option の宣言で持たないと明示）。
- build warning 0、host 試験、必要なら guest の試験は T1。
