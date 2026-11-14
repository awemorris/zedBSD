<!-- awesome-plan project=zedbsd record=ws349 -->

# WS349: Raspberry Pi 5 の HDMI の音

Status: incomplete
Primary Milestone: MG006
Parent: [Master](../master.md)
Owner: Q1

## 目標

Pi 5（と Pi 500）は `/dev` に音の device が無く HDMI の音が出ない。

## ユーザーの決定（要点）

- 2026-11-13 に外部の利用者から Raspberry Pi 500（BCM2712 D0、8 GB）で zedBSD（c4cede8、2026-11-03 の source）が起動したという報告を受けた（Pi 500 の dtb を FAT に足して起動、desktop・日本語入力・Wi-Fi・有線・ブラウザ・SSH・shutdown は動作、HDMI の音は無し）。
- 2026-11-13 ユーザーから、Pi 500 の報告の #1・#6・#7 をそれぞれ WS に、#2・#3・#4 を Bug に（修正済みの疑いが強い）、#5（起動の直後の時計）は仕様、#8（build の手順）は見直して誤りがあれば直す、という趣旨の指示を受けた。

## Resume point

- 2026-11-15 P8（q2124-i01）: p002 を終えた（[values.md](values.md)、p001 の N・CTS の訂正と補い）。次: p003（計算の部品と host 試験）。
- 2026-11-15 P8（q2118-i01）: p001 を第二版に改訂（乖離の確かめ: Pi 5 の display は HDMI に触れず firmware が mode を持つ、C1 と D0 の dtb で DREQ が違う、dma の cell の上位の bit の意味、audio の clock の gate）。design-reviewer の指摘 15 件を反映（[design-review.md](phase001/design-review.md)）。p002〜p007 を立てた。ユーザーへの問いは [user-questions.md](user-questions.md)（U1 mk の枠の名前と時期、U2 二つの画面、U3 起動の後の画面）。次: p002（値の照合）と p003（計算の部品と host 試験）は mk の答えを待たずに進められる。p004 は U1 の kernel.mk・config.mk の枠が当たってから。
- 2026-11-14 Q1: 目標の確かめ。Pi 5 には音の driver が 1 つも無く、rpi5 の既定で audio の core も n（`mk/platform-rpi5/defaults.mk`）。内蔵の音の出口は HDMI（3.5 mm の jack は無い）。ユーザーから、目標は HDMI の音のままでよい、という趣旨の答えを受けた（2026-11-14）。mk の `CONFIG_DRIVER_RPI5_AUDIO` の枠は RP1 を指し source が空なので、HDMI の driver の名前と枠は設計の改訂で決め直す（mk の変更は案としてユーザーへ）。p001 の設計は design-reviewer を通していないので、次は design-reviewer と改訂。

- 2026-11-13 Q1 が立てた。P1 が処理する。
- 2026-11-13 P1: p001 の設計の第一版（[phase.md](phase001/phase.md)）。次: design-reviewer、未確定の出典の確かめ（p002 の案）、実装の Phase を Q1 が立てる。

## Phase

| Phase | 中身 | 状態 | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 設計: BCM2712 の HDMI audio の経路（HDMI の controller・DMA・audiod の device の口）と、Pi 4 の vc4 の HDMI audio との違い、出典（guardrail の値の規則）。設計の後に実装の Phase を立てる | cleared（2026-11-13 P1 の第一版。2026-11-15 P8 が第二版に改訂、design-reviewer を反映） | — |
| [p002](phase002/phase.md) | 値の照合と出典の表（[values.md](values.md)） | cleared（Q1 の受け入れ待ち。2026-11-15 P8、p001 の N・CTS の 1 行を訂正） | p001 |
| [p003](phase003/phase.md) | 計算の部品 `rpi5-hdmi-audio-wire.c` と host 試験 | planned | p002 |
| [p004](phase004/phase.md) | attach と読みだけの probe、Pi 5 の実機で未確定を記録し設計を直す | planned | p003、U1（mk の枠） |
| [p005](phase005/phase.md) | 再生の driver と audio の core への登録、docs | planned | p004 |
| [p006](phase006/phase.md) | Pi 5 の実機の再生の試験（capture の録音の解析） | planned | p005 |
| [p007](phase007/phase.md) | 規約の全文の見直し | planned | p006 |
