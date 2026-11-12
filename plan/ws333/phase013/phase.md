<!-- awesome-plan project=zedbsd record=ws333-p013 -->

# ws333-p013: CPU の ioctl・`/sbin/thermal` の CPU と電力の表示・strategy の保存と restore

Status: planned（2026-11-12 P1 が design.md §9 から立てた）
Parent: [WS333](../ws.md)

## 目的

design.md §9 の p013: p004 で決めた UAPI の CPU の範囲（32〜63）の ioctl を `src/kern/cpuperf.c` に実装し（`kern_thermal_ioctl_range_register`）、`/sbin/thermal` に CPU（topology・domain・相対の性能・HFI・idle の滞在）と電力（meter）の表示と root の直の設定を足し、strategy の保存（`/etc/thermal.conf`）と rc の `/sbin/thermal restore`（Q333-1 B、保存が無ければ balanced）を入れる。

## 依存

- p004（UAPI のユーザーの答え）、p005 後半（`/dev/thermal` の device と core の ioctl・`/sbin/thermal` の熱の表示）、p006（cpuperf の枠組み）。
- 持ち越し（p011）: hypervisor の下と MSR の道の無い build は topology だけ（controls 無し）。MC6 の出し方は p004 の答えに従う。

## 受け入れ

- CPU の ioctl の host 試験（layout、EINVAL・EPERM・EOPNOTSUPP の道、`KERN_CPUPERF_READ_SAMPLE` の有無）。
- `/sbin/thermal` の strategy の保存と restore の host 試験。
- amd64 の vmunix（MSR の道の有無の 2 通り）と全 port の vmunix の warning 0。
- QEMU amd64 で `/sbin/thermal` が sensor・fan 0・受け手の結果（cpu は対応外）・topology を出し、rc の restore が balanced を渡す（ssh・QMP で判定）。riscv64 の virt でも `/sbin/thermal` が動く。
