<!-- awesome-plan project=zedbsd record=queue-q410 -->

# Queue q410: WS053 の規約と最後の回帰（ws053-p005）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー指示「LTOは検討後、実現可能なら、適用をお願いします。」「引き続き自走してください。」範囲は [ws053-p005](ws053/phase005/phase.md)。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q410-i01 | [ws053-p005](ws053/phase005/phase.md) | cleared（規約の違反 0、既定の build が 5 config で full LTO・warning 0、amd64・rpi4 で login。WS053 completed） |

依存: ws053-p002〜p004（cleared）。人間の判断は要らない。

Upcoming Work Outlook: WS046 p007（BUG-033、中断中）・p008・p005。

## 結果の要約（ws053-p005）

- 規約: `kcrt.c` の WS053 の変更は規約どおり（style-check の 7 件は既存の範囲外のコード）。`kbench.c` を書き直して `plan/tools/kbench/` へ（違反 0、`build.sh` を追加）。`pc98-boot.py` を `plan/tools/` へ。
- 既定の build: amd64・intelmac・rpi4・pcat・pc98 が full LTO、warning 0（vmunix 2,117,336・1,545,400・2,763,776・1,303,888・759,852 bytes）。
- boot（QEMU）: amd64・rpi4 で login（`build/boot-test-amd64-q410`・`build/boot-test-rpi4-q410`）、pcat・pc98 は q409。実機は未実施。
- 観察: amd64 で初回の `sshd-start: making a rsa host key` が `login:` の後に出た（出力の順の競合、記録だけ）。
- WS053 を completed にし、Phase の directory を削除した（この commit の前の commit に全文がある）。
