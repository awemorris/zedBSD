<!-- awesome-plan project=zedbsd record=ws182-p001 -->
# ws182-p001: 設計 — 電源ボタンの押下からメニューへ

Parent: [WS182](../ws.md)
Status: cleared 待ち（2026-10-08 P1 / q861-i01。設計を書いた。下の D1 はユーザーの確認待ち、推奨のまま p002 を実装）
Disposition: normal
Queue: q861 / q861-i01

## 由来

ユーザー（2026-10-07）「電源ボタンのハンドリングは、あとで実装でいいです。ログオフ、電源オフ、などのメニューを表示できるようにしたいです。独立WSにして、ベータ2の最後に実装しましょう。」Q1 の依頼（2026-10-08）: Restart も出し、Sleep は WS052（ベータ3）なので出さない。

## 事実

- 経路: kernel の `acpi-power.c` が `\_SB.PBTN` の Notify 0x80 を `/dev/system` の POWER/PRESS（subject `power-button`、`time_ns` は kernel の ms の時計）にし、
  `libkeiland-backend-zedbsd/events-zedbsd.c` が host の `power_button` を呼び、compositor の `kwl_backend_power_button`（`backend-host.c`）が
  `KWL EVENT power button` を出すだけだった。Linux・FreeBSD の backend は呼ばない。
- **1 回の押下で 2 行**（Q1 の 5320・5330 の観察）の原因（5330 の DSDT・FADT、`plan/ws049/tests/latitude5330/`、iasl で逆アセンブル）:
  - FADT は Control Method Power Button = 1。`acpi-event.c` は固定の power button の event をこの flag の時に入れないので、PM1 の固定の event は来ない。
  - S0 の押下は EC の query `_Q66` → `NEVT` → `ECG1` の bit 0 → `EV3 (1, 2)` → `BTNV` → `Notify (PBTN, 0x80)`。`Arg1 == 2` の枝は押されているかを見ずに無条件で Notify する。
  - 同じ `BTNV` の HIDD の枝（Windows の modern standby 用）は `ECBT (One, 0x04)`（button が今押されているか）で 0xCE（押下）と 0xCF（解放）を分ける。つまり EC は**押下と解放の両方で** query を出し、PBTN の枝では両方が同じ 0x80 になる（推定、実機の gap で確かめる）。
  - wake の押下は `_L18` → `Notify (PBTN, 0x02)` で、kernel は押下と数えない（ws052-p003）。その解放は復帰の後に 0x80 で来うる。
- App Home の Power Off の dialog（ws099-p037、`power-dialog.c`）は Power Off・Restart・Log Out・Cancel を持ち、`kwl_power_dialog_open(server, "button")` を予定していた。Power Off・Restart は backend が offer する時だけ有効（zedBSD は login 画面と root・wheel の session）。dialog は greeter・lock の間は閉じる（`kwl_power_dialog_tick`）。
- 関係: ws132-p008 の D1（短押しで dialog 無しの S0i3）は、2026-10-07 のユーザーの決定（この WS）で置き換わった（`sleep.c` の注記は既にそう書く）。ws132 の記録の更新は Q1。Sleep をメニューに足すのは WS052。

## 設計

1. **押下と解放**（zedBSD の backend、`events-zedbsd.c`）: 送った power-button の押下の後、`EVENTS_RELEASE_MS`（2000 ms、record の `time_ns`）以内に来た次の
   power-button の record を解放として落とし、`KL EVENTS power-button release gap_ms=N` を log する。それより後、または解放の後の record は新しい押下。
   OS に固有の firmware の癖なので compositor ではなく backend に置く（guardrail の backend の配置）。sleep button は対象外（観察が無い）。
   - 代償: 1 回の押下で 1 record しか出さない機械（QEMU の固定の event など）で 2 s 以内に押し直すと 2 回目が落ちる。メニューが開いていれば元々何もしないので実害は小さい。2 s より長い押下の解放は新しい押下になるが、同じ理由で無害。
2. **compositor**（`backend-host.c` の `kwl_backend_power_button`）、押下ごとに順に:
   1. `KWL EVENT power button ms=N`。
   2. sleep の前後（`kwl_sleep_button_ignored`: sleep が進行中、または答えの後 1000 ms）は無視（`KWL POWER button skip reason=sleep`）。wake の押下の解放を拾わないため。
   3. 押下は利用者の入力として数える（`lock_input_ms`）: 時間で消えた画面が点き、無入力の時間が数え直しになる。
   4. greeter（`reason=greeter`）・lock（`reason=locked`）はメニューを出さない。
   5. dialog が開いていれば何もしない（`reason=showing`）。閉じかけなら開き直す。
   6. `kwl_power_dialog_open(server, "button")`（`KWL POWER dialog open source=button ...`）。選択・取り消しは App Home からと同じ。
3. **D1（2026-10-08 ユーザー「現状ではオーケーです」で確定）**: greeter と lock ではメニューを出さない。理由: lock 画面は ws035-p102 で電源の button を持たない（未認証の電源 off・log out を避ける）、greeter は Restart・Shut Down が画面にある。代案: greeter でも dialog（Log Out を除く）を出す（greeter の描画・入力の経路に dialog を足す変更が要る）。
4. Sleep は出さない（WS052、ベータ3）。長押し（4 s 程度）の強制の電源断は firmware のまま。

## 確認（p002）

- build: compositor（zedBSD・Linux）と backend、warning 0。host: 既存の `plan/ws099/tests/host-power-layout.sh`（dialog の layout、変えない）。
- QEMU（T1）: QMP `system_powerdown` → `KWL EVENT power button`・`KWL POWER dialog open source=button`、Esc で `choice=cancel`、電源は切れない。3 s 空けてもう一度で再び開く。lock 中は `skip reason=locked`。
- 実機（5320・5330、人の手）: 短押しで dialog が 1 回、`KL EVENTS power-button release gap_ms=` が 1 行（短押しで数百 ms、1.5 s の長押しで約 1500 → 押下と解放の推定を確定）。`tests/scenarios/os/power/power-button.md` を更新する。
