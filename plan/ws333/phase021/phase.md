<!-- awesome-plan project=zedbsd record=ws333-p021 -->

# ws333-p021: passive（見張り → poll の thread → 受け手の limit）

Status: cleared（2026-10-31 Q1、main a57216e81）
Parent: [WS333](../ws.md)
Queue: q1774-i01（P6）

## 目的

design.md §9 の p021 と §4.9: CPU の sensor が passive の水準を超えたら、見張りが poll の thread に伝え、poll の thread が `limit` を持つ受け手（今は cpu）に 5 秒ごとに 100‰ ずつ下げた上限を渡す（下限 guaranteed は受け手の写し）。水準 − 3 °C で戻す。受け入れは host 試験まで（Q303-14、5330・Pi 5 では動く場面が無い）。

## 作った物

- `src/kern/thermal.c`:
  - 見張りの 1 周で、`KERN_THERMAL_SENSOR_CPU` の sensor のどれかが passive の水準を跨いだ状態（p005 の `passive_crossed`、3 °C の hysteresis）なら `thermal_passive_wanted` を立てる（写しの lock の下）。ZONE の passive は使わない。
  - poll の thread（毎秒）の `thermal_poll_passive()`: 欲しい間は 1000 → 900 → … → 0、欲しくなくなったら 100 ずつ 1000 まで。最初の下げはすぐ、以後は前の段から 5 秒（kernel の tick）ごと。底（0）と 1000 に着いたら受け手を呼ばない。段ごとに `limit` を持つ全部の受け手へ `KERN_THERMAL_DOMAIN_ALL` で、lock を持たずに渡す。EOPNOTSUPP は数えず、他の失敗は受け手ごとに 1 回だけ log。段ごとに log 1 行。
  - `struct kern_thermal_status` の `passive_permille`（GET_STATUS の後半で出せる）。
- 試験: `plan/ws333/tests/thermal-host/` の scenario `passive`。

## 設計からの差（理由）

- 下限は core では 0‰ にし、guaranteed の床は受け手の写し（cpuperf の `cpuperf_ceiling()` が guaranteed より下げない）に任せた。core は受け手の範囲を知らない。

## 確かめた事

- host 試験（`plan/ws333/tests/thermal-host/run.sh`、scenario `passive`、21 check）PASS: ZONE の passive では呼ばない、CPU の sensor が水準を超えるとすぐ 900、hysteresis の中でも下げ続け 0 まで 10 段で止まる（各段の間が 4.5 kernel 秒以上、domain は ALL）、失敗する受け手・EOPNOTSUPP の受け手も同じ数だけ呼ばれる、3 °C 下で 100 から 1000 まで 10 段で戻って止まる、status の `passive_permille`。わざと壊す確かめ: 5 秒の間を外すと FAIL。
- 全 20 port の vmunix warning 0（p016 と同じ build）。

## 未実施・残り

- 本物の cpu の受け手（cpuperf）との結合と provider のある機械（p011 の後）。動く場面のある実機は無い（5330 の TCPU の `_PSV` は 110 °C、Pi 5 は passive の trip が無い）。
- passive の上限の UAPI の表示（p004・p013）。
- 規約の全文の見直し（p024）。
