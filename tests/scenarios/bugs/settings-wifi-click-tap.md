---
id: bugs.settings-wifi-click-tap
title: Settings の Wi-Fi: on の後に switch で off にでき、AP の行の 1 回の tap で接続する
status: active
areas: [settings, network, touch]
paths: [userland/desktop/settings/page-network.c, userland/desktop/settings/network.c, userland/tests/network-probe/]
machine: qemu
human: none
since: BUG-184
---

## 目的
BUG-184（on の後に off を押すと Scan が押されて off にならない）と BUG-188（AP の一覧で 1 回の tap では接続されず 2 回で接続）を、networkd の代わりの network-probe（模擬の Wi-Fi、3 つの AP）で確かめる。

## 準備
image は `plan/tools/aat/config-amd64-aat-bugs.mk`（`/bin/network-probe` 入り）。helper が `/sbin/service stop networkd` で networkd を止め（compositor の networkd の watch の接続が切れる。有線の interface はそのまま、networkd が退けるのは Wi-Fi の radio だけ）、`network-probe 300` を立てる。compositor は 1 秒以内に watch をやり直して probe とつながる（`KWL NETWORK state … wifi=off|searching|…`、Settings は compositor を通して probe と話す）。終わりに probe を止め、`/sbin/service start networkd` で networkd を戻す。2026-10-09 P1（T1-481）: 前の準備（socket を退けるだけ）では compositor の古い watch が `wifi=absent` のままで、頁に Wi-Fi の switch が無かった。

## 操作と確認
1. 操作: Settings を Wi-Fi の頁で開く。
   確認事項: AP の一覧。正解: `ZSETTINGS NETWORK scan count=3`。確認方法: log。
2. 操作: Wi-Fi の switch（control 1）を click して off、もう一度 click して on、もう一度 click して off。
   確認事項: 各 click で switch が動く（BUG-184）。正解: off: `ZSETTINGS NETWORK switch shows on=0` と `switch settled wifi=1`、on: `shows on=1` と `settled wifi=2〜5` と `scan count=3`。確認方法: log、撮影 off・on。
3. 操作: on に戻し、2 つ目の AP の行（control 101）を touch screen で 1 回 tap。
   確認事項: 1 回で接続を求める（BUG-188）。正解: probe の log（`/tmp/aat-probe.log`）に `NETPROBE request op=N ssid=…`、Settings に `ZSETTINGS NETWORK state reachable=1 connected=1 … wifi=4 ssid=…`。確認方法: log、撮影 tapped。

## 合格
2 の off が効き、3 の 1 回の tap で接続する。image に network-probe が無い時は needs-person（理由を書く）。

## 注記
実の radio（AX211）と networkd の振る舞い（BUG-183・185・187・189 の残り）は実機の UAT。5330 の touchpad の tap は別の経路。 helper は `plan/tools/aat/scenarios/helpers_bugs.py`、一覧と読み方は `plan/agents/bug-ui-sweep-20261008.md`（q911）。
