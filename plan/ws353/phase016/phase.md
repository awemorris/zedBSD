<!-- awesome-plan project=zedbsd record=ws353-p016 -->

# ws353-p016: 試験（QEMU の回帰と 5330 の idle）

Status: planned
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §11。

## 目的

WS353 の全部の変更の後の tip で、idle の起床と電力、入力・描画・animation・hotplug の応答を確かめる。

## 確かめ

- QEMU（Venus、P1 の名前・port、1 本の script）: 起動（`plan/tools/boot-test.sh`）、WS309 の guest 試験（Q1 の割当）、WS099 の C1・C2、hotplug（venus-head.sh の抜き差しと出力の移動）、key repeat・pointer・touch・slide・fade、sleep と lock、game mode の出入り、Log Out。idle 60 s で compositor の全 thread の timeout の起床が 0（時計が見える時の分の 1 回を除く。WS352 の poll の診断）、`KWL PERF` の `timeouts` 0。
- 5330（Q1 の割当、`5330-idle.sh` の形、Debian から 1 回の UEFI の起動）: idle の package の電力と PC2 を WS333 p029（100 Hz 固定で 4.37 W・PC2 87 %）と比べる。入力（touch pad の tap・key repeat）と描画の目視の代わりに `--log-frames` の時刻。display の抜き差しは実機の HDMI があれば（無ければ未実施）。
- arm64・riscv64 の build（compositor・libvulkan・kernel）warning 0。実機（Pi・VF2）は Q1 の割当があれば。

## 受け入れ

- QEMU の全項目 PASS。5330 の idle が WS333 p029 と同等以上（下回る時は thread ごとの起床の数を添えて Q1 に）。実施しなかった物は未実施と書く。

## 依存

- p002〜p015（p013 は未決 3 の決定しだいで除いてよい）。
