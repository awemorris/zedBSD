<!-- awesome-plan project=zedbsd record=ws134-p013 -->
# ws134-p013: M3d app の system の source

Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: T1-070 で monitor-p013 PASS）（旧: in-progress（q662、P2 generation8、2026-10-04。実装・build・host 試験済み、T の QEMU の試験待ち））
Disposition: normal
Parent: [WS134](../ws.md)
設計: [design.md](../design.md) §1.5（data source の層と stub）
依存: p012（T1-069 PASS）

## 実装

- `userland/desktop/monitor/system-source.c`（新）: `sm_system_open`（window の display に `kl_system_open` と `kl_system_monitor_open`、最初の info を
  roundtrip で待つ、info の CPU の数と GPU の数（system が GPU を名乗らない時は描画の device の 1 つ）で calm の sim を開き、kind を
  `SM_SOURCE_SYSTEM` に、host・CPU 数・GPU 名（1 つ目は描画の device の名前））、`sm_system_take`（`kl_system_dispatch`、info の変化、
  `kl_system_monitor_take` の frame を monitor の frame に: まず sim の frame（全部 simulated）、system がくれた field（CPU・memory・swap・network・
  disk と latency・GPU の busy・memory・温度・電力）を上書きして simulated の bit を消す）、`sm_system_close`。
- `main.c`: `--source=auto|system|sim|replay:FILE`、既定は auto（system が開ければ system、開けなければ sim、`ZMON SYSTEM none errno=`）、
  system を名で求めて開けなければ終わる。`main_open_source`。frame の取得は system の時 `sm_system_take`。sim の時だけ 5 分の履歴を先に埋める。
  `ZMON READY source=system`。loop の待ちは system の時 1 秒まで（sample の event が起こす）。
- `monitor.h` に `SM_SOURCE_SYSTEM`、`app.h` に `struct sm_system` と prototype。Makefile 3 つ。
- 規則の段の判定は今までどおり simulated の field を除く（system の時は sim の値で警告しない）。
- 試験: `monitor-p002.sh`・`monitor-p003.sh` の sim の起動に `--source=sim`（既定が auto になったため）。`monitor-p013.sh`（新、guest: 既定で
  `source=system cpus=4`・`ZMON SYSTEM open`、sample の simulated に CPU・memory・swap・network・disk（0x3f）が無い、busy loop で CPU 20% 以上、
  disk の読み 1 MB/s 以上、system.png、`--source=sim` も動く、compositor の sampling の停止、ERROR なし）。
- design.md §1.5 の stub の表の「今」の列を更新（CPU・memory・network・disk と latency は本物、GPU は i915 の使用率が本物（実機で未確認）、
  Venus と FreeBSD は stub、CPU の周波数・温度・電力・電池は stub か出さないのまま）。

## 確かめ

- build: zedBSD の monitor（-Werror）exit 0 warning 0、Linux の gcc の object と clang の -fsyntax-only（main.c・system-source.c）、
  style-check 違反 0、host 試験（monitor-host・interact・rate）PASS、host の preview も動く。
- QEMU（T に依頼）: `monitor-p013.sh`。結果は未着。

## stub の項目（この Phase の後）

design.md §1.5 の表のとおり。本物: CPU（全体・core）、memory の Used・Cache・Available・Swap、network の RX/TX、disk の読み書きと latency、
hostname・CPU の数・uptime、GPU の名前（描画の device）。stub: CPU の周波数、GPU の使用率・memory・温度・電力（QEMU の Venus・FreeBSD。
i915 の使用率は本物になる見込みで実機は未確認）、電池は出さない。
