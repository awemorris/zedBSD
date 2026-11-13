<!-- awesome-plan project=zedbsd record=ws353-p006 -->

# ws353-p006: 締め切り A（session・system・protocol の期限）を timer へ（main loop の 2）

Status: planned
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §5.5、対応表の 2-05・2-06・2-15・2-16・2-19〜2-21・2-26〜2-30・2-32・2-34・2-41〜2-43。

## 目的

main が毎 pass に時刻を比べる（または `kwl_due` で出す）締め切りのうち、session・system・protocol の物を、予定がある間だけ arm する timer にする。毎 pass の tick の呼び出しを消す。

## 対象（file）

- fatal client の flush 2 s・`--timeout`（main.c）、handoff（handoff.c）、sleep・消灯・pause・rest（sleep.c）、idle の lock（shell.c）、ping（toplevel.c）、IME（input-method.c）、clipboard（clipboard.c）、drag の release（data.c）、desktop の再起動（desktop.c）、network の 30 s・切り替えと詳細の 1 s（network.c。1 s は view がある間の timer、周期の読み直しが消えるのは networkd の push の後＝範囲の外）、displays の保留（displays-shell.c、p004 で移した物）、sysmon（購読がある間）、machine の読みの打ち切り（machine-shell.c）、shot の答え（shot.c）、phone（phone-shell.c）。
- `kwl_ping_check`・`kwl_ime_tick`・`kwl_sleep_tick` など、毎 pass に呼んでいた tick を、事象の handler と timer の handler に割る。tick の中の「thread の結果の取り込み」は wake の後の take へ。

## 残す振る舞い

- 各期限の長さと、期限の後の結果（lock・消灯・sleep・ping の無応答・IME の再起動など）。時刻の基準（`kwl_milliseconds` の CLOCK_MONOTONIC）。

## 確かめ

- build warning 0。host: 変えた機能に host 試験があれば流す（sleep-rules など）。
- QEMU: idle の lock と消灯の時刻、ping の無応答の表示、IME の再起動、clipboard の打ち切り、desktop の再起動、Log Out（handoff）。WS352 の poll の診断で、idle（予定なし）の main の timeout の起床が、この Phase の対象の分 0。

## 受け入れ

- 対象の期限が timer で動き、毎 pass の比べが code に無い。結果・SHA・未実施を書く。

## 依存

- p005。
