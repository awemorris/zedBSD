---
id: desktop.bar.network-details
title: Alt を押しながら network の icon を click すると接続の詳細
status: active
areas: [compositor, bar, network]
paths: [userland/desktop/wayland/network.c, userland/desktop/wayland/network-info.c, userland/base/networkd/]
machine: either
human: none
since: ws099-p032
---

## 目的
network の詳細（Interface・IPv4 address・MAC address など）が出て、値が system と合うことを確かめる（UAT 5.2〜5.5）。

## 準備
desktop で network に繋がっている（5330 は有線か Wi-Fi、QEMU は USB の network）。

## 操作と確認
1. 操作: Alt を押しながら bar の network の icon（`ZWL NETWORK icon` の位置）を click。
   確認事項: 詳細。正解: menu でなく詳細が開く。行に Interface・IPv4 address・MAC address がある。確認方法: log `ZWL NETWORK info open` と `ZWL NETWORK info row label=… value=…`、撮影。
2. 操作: `ifconfig -a` を root で流す。
   確認事項: 値。正解: 詳細の IPv4 address と MAC address が `ifconfig` と同じ。確認方法: 出力と log の比較。
3. 操作: Esc。
   確認事項: 詳細。正解: 閉じる。確認方法: log `ZWL NETWORK info close`。
4. 操作: icon を普通に click（状態の島のパネルが開く、WS192）、パネルの Wi-Fi の行の左の方を click、Esc。
   確認事項: menu。正解: `KWL STATUS panel open`、行で `KWL STATUS panel close via=item` といつもの menu（`ZWL NETWORK open`）、Esc で閉じる（`ZWL NETWORK close`）。確認方法: log。

## 合格
1〜4 の正解。

## 注記
Received・Sent が 1 秒ごとに増えるか（UAT 5.3）は通信を起こしながらの撮影 2 枚で人が見る（任意）。
