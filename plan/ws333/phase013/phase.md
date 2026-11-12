<!-- awesome-plan project=zedbsd record=ws333-p013 -->

# ws333-p013: CPU の ioctl・`/sbin/thermal` の CPU と電力の表示・strategy の保存と restore

Status: in-progress（2026-11-12 P1、q2013-i01: 実装と host 試験・QEMU の一部まで。起動の時の restore の service と riscv64 の QEMU と全 port の build は mk の案の答えの後）
Parent: [WS333](../ws.md)
Queue: q2013-i01（P1）

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

## 実装（2026-11-12 P1、q2013-i01）

- `src/kern/cpuperf.c`: `kern_cpuperf_init()` の終わりで範囲 32〜63 を `kern_thermal_ioctl_range_register` に登録（枠組みが立たなかった機械では登録しないので ENOTTY）。handler は request の値の全体で照合: GET_CPU（READ_SAMPLE は root、未知の request の bit は EINVAL、CPU が答えない sample は 0 と `SAMPLE_TIMEOUT`、範囲外は ENOENT）、GET_CPU_DOMAIN（RANGE は steps[0..1] に両端、STEPS は並び）、SET_CPU_DOMAIN（root、未知の field・reserved は EINVAL、範囲外の domain は ENOENT、ALL）、GET・SET_CPU_RELATIVE（SET は root）、GET_CPU_IDLE（root、scope は CORE）、GET_PACKAGE_IDLE（誰でも、scope は PACKAGE、CPU の無い package は ENOENT）。UAPI と枠組みの同じ意味の値が同じ事を `_Static_assert` で確かめる。`kern_cpuperf_package_count()` を足した（`include/kern/cpuperf.h`、GET_STATUS の `cpu_package_count`）。
- `userland/base/thermal/main.c`（新、`/sbin/thermal`）: 要約・sensors・fans・meters・receivers・strategy（表示と選び、manual の 3 軸）・restore・boost・cpu（一覧・1 つ・sample・domains・set・relative）・idle（cpu・package、`-i` 秒で割合）・power（`-i` 秒、W）・watch（事象の種類を選べる）。1 行 1 項目、key=value。終了の値 0・1（kernel の拒み、理由 1 行）・2（使い方）。strategy は kernel が値を受け取った時（EOPNOTSUPP と受け手の失敗を含む）に `/etc/thermal.conf` を一時 file・fsync・rename で書き直す。EOPNOTSUPP は `strategy NAME result=unsupported kept=yes` と 0。`restore` は conf の 1 行を読み（無い・壊れていれば balanced、壊れていれば警告）書き直さない。
- `userland/base/thermal/service`（oneshot、`/sbin/thermal restore`、late）と `userland/base/etc/rc.conf` の `thermal`（enabled、optional）。service の file を image に入れるのは mk の案（p005 の 3・5）の後。

## 確かめた事

- host 試験 `plan/ws333/tests/cpuperf-host/run.sh <out>`: 5 scenario PASS（新しい ioctl の scenario は 52 checks: 範囲の登録、照合、CPU の topology と種類と anchor、sample の root と答えない CPU の SAMPLE_TIMEOUT、domain の RANGE と範囲外、直の設定の root・field・reserved・ENOENT・ALL と CUSTOM、relative の選び、idle の root と scope と ENOENT）。ASan・UBSan でも PASS。
- `/sbin/thermal` の compile: x86_64・arm64（rpi5 の sysroot）・riscv64（vf2 の sysroot）で `-Wall -Wextra -Werror` の warning 0。
- QEMU amd64: p005 の後半の get-set の試験で strategy の保存（`cat /etc/thermal.conf`）・`thermal restore`・manual・`thermal cpu`・`thermal power`・boost の拒みを画面で確かめた（[p005](../phase005/phase.md) の「確かめた事（後半）」）。
- 未実施: `/sbin/thermal` の strategy の保存と restore の host 試験（QEMU で代わりに確かめた）、MSR の道の無い amd64 の build（既定は扱わない、ws.md の前提）、全 port の vmunix、起動の時の rc の restore、riscv64 の virt の QEMU、idle と power の実の値（5330 の p014）。
