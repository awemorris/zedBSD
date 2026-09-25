<!-- awesome-plan project=zedbsd record=queue -->

# Queue q324: シリアルコンソールの受信

<!-- awesome-plan-current:start -->
Status: finished
Active Queue: none（q324 finished）
Executor: メインセッション（サブエージェントは使わない）
Last Queue: q323 stopped（履歴 `plan/history/queue-q323.md`）
<!-- awesome-plan-current:end -->

Approval: current user「取り急ぎ、シリアルコンソールに受信を実装して、キー入力しなくてもよいように
してください。そのためのphaseを作り、先にそれを終わらせてから、元のnetコマンドのテストのphaseを
再度投入しましょう。」（2026-09-23）

| Order | Attempt | Phase | Status | Scope |
| --- | --- | --- | --- | --- |
| 1 | q324-i01 | [ws035-p037](../ws035/phase037/phase.md) | cleared | シリアルコンソールの受信。キー入力なしでゲストを操作できるようにする |

## 結果

COM1 は出力のミラーだけで受信が無く、ゲストを操作する手段が QEMU の `send-key` しか無かった。
それは**ゲストが忙しいとコマンドの途中から文字が落ちる**（q323 で `pkill sshd` が `PILLSSHD`、
`net watch` が `et` になった）。

`tty_console_input_byte()` を足し、シリアルを IRQ 4 の割り込み駆動で受信して
同じ line discipline へ流すようにした。ハーネスは `plan/tools/guest/serial.py`。

実QEMU で、キー入力を使わずにログイン・コマンド実行・終了状態の取得を確認した
（`uname -a`、`ls /bin/which` rc=0、`false` rc=1、`net show`）。

## 次に入れるもの（2026-09-23 ユーザー指示）

1. **ws035-p018**（networkd の購読、q323 で uncleared）を再投入する。
   実装は tree にあり build は通る。検証だけが残っている。シリアルで行える。
2. **ws035-p039**（USB CDC-ECM の実機確認）。ECM は実績が無いまま入っており、
   シリアルで観察しながら `ue0` が上がるかを確かめる。後回しでよい。
3. **ws035-p038**（SSHハーネス）。p037 と p039 の後。
4. **ws034-p003**（lspci、q323 で未着手）。

## 範囲外

上の1〜4、WS036、WS031。HALの変更は差分ごとに事前承認。
aggregate `make check` は使わない。commitは `git commit -m WIP` のみでpushしない。
