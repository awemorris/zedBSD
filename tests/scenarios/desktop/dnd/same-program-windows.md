---
id: desktop.dnd.same-program-windows
title: data device を 2 つ持つ client への drag は両方に届き、落とした 1 つだけが中身を受ける
status: active
areas: [compositor, dnd]
paths: [userland/desktop/wayland/data.c, userland/desktop/wayland/dnd-state.c, userland/tests/data-probe/main.c]
machine: either
human: none
since: ws189-p002
---

## 目的
ws189 の設計 §0 の欠け 1・§3.6: 1 つの client が複数の wl_data_device を持つ（libkeiland の program は窓ごとに 1 つ）と、compositor は drag をその client の
全部の device に送り（enter・motion・leave）、型を受けた最初の device にだけ drop を送ることを確かめる。

## 準備
desktop。試験の image は `plan/ws189/tests/config-amd64-aat-dnd.mk`（AAT の image に `data-probe`）。kei で `data-probe --token=two --two-devices` を開き
（1 つの窓に data device が 2 つ）、Terminal（`echo dndsame` を打って Enter）と重ならないように置く。

## 操作と確認
1. 操作: Terminal の出力の `dndsame` を double-click で選び、その上で押して data-probe の窓の上へ動かして止める。`aat mark start` は押す前。
   確認事項: 両方の device の enter。正解: `KWL DATA drag enter client=… devices=2`、`DATAPROBE drag enter text=1` と `DATAPROBE drag enter device=2 text=1`、
   `KWL DATA drag state=copy`。確認方法: `aat lines 'KWL DATA drag' --since start`、data-probe の log。
2. 操作: 離す。
   確認事項: 1 つだけの drop。正解: `KWL DATA drag drop`、`DATAPROBE drag drop`（device 1）と `DATAPROBE drag received when=drop bytes=7 text=dndsame`、
   device 2 は `DATAPROBE drag leave device=2`（`DATAPROBE drag drop device=2` が無い）、`KWL DATA drag finish`。確認方法: log。

## 合格
devices=2 の enter、両方の device の enter、device 1 だけの drop と中身、device 2 の leave。

## 注記
T1-433 は Terminal の New Window（Ctrl+Shift+N）で試して `devices=1` だった。Terminal の New Window は別の process を起動する（terminal/main.c
`main_new_window` の fork と exec）ので、1 つの client に device は 1 つで `devices=1` が正しい（設計 §0 の「Terminal・Notes の 2 つ目の窓が該当」は誤り、
今の Kei の program に 1 つの process で窓を 2 つ持つ物は無い）。§3.6 の経路は data-probe の `--two-devices` で確かめる（ws189-p002 F2）。
