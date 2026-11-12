<!-- awesome-plan project=zedbsd record=ws333-p013 -->

# ws333-p013: CPU の ioctl・`/sbin/thermal` の CPU と電力の表示・strategy の保存と restore

Status: in-progress（2026-11-12 P1・q2016-i01: 新しい build の正式な image で起動の時の restore と QEMU amd64 を確かめ、全 platform の vmunix を build した。残りは riscv64 virt の QEMU（新しい build に platform が無い）。cleared の判定は Q1）
Parent: [WS333](../ws.md)
Queue: q2013-i01（P1）

## 目的

design.md §9 の p013: p004 で決めた UAPI の CPU の範囲（32〜63）の ioctl を `src/kern/cpuperf.c` に実装し（`kern_thermal_ioctl_range_register`）、`/sbin/thermal` に CPU（topology・domain・相対の性能・HFI・idle の滞在）と電力（meter）の表示と直の設定（root と desktop の group の利用者）を足し、strategy の保存（`/etc/thermal.conf`）と rc の `/sbin/thermal restore`（Q333-1 B、保存が無ければ balanced）を入れる。

## 依存

- p004（UAPI のユーザーの答え）、p005 後半（`/dev/thermal` の device と core の ioctl・`/sbin/thermal` の熱の表示）、p006（cpuperf の枠組み）。
- 持ち越し（p011）: hypervisor の下と MSR の道の無い build は topology だけ（controls 無し）。MC6 の出し方は p004 の答えに従う。

## 受け入れ

- CPU の ioctl の host 試験（layout、EINVAL・EPERM・EOPNOTSUPP の道、`KERN_CPUPERF_READ_SAMPLE` の有無）。
- `/sbin/thermal` の strategy の保存と restore の host 試験。
- amd64 の vmunix（MSR の道の有無の 2 通り）と全 port の vmunix の warning 0。
- QEMU amd64 で `/sbin/thermal` が sensor・fan 0・受け手の結果（cpu は対応外）・topology を出し、rc の restore が balanced を渡す（ssh・QMP で判定）。riscv64 の virt でも `/sbin/thermal` が動く。

## 実装（2026-11-12 P1、q2013-i01）

- `src/kern/cpuperf.c`: `kern_cpuperf_init()` の終わりで範囲 32〜63 を `kern_thermal_ioctl_range_register` に登録（枠組みが立たなかった機械では登録しないので ENOTTY）。handler は request の値の全体で照合: GET_CPU（READ_SAMPLE は書き込みで開いた fd だけ、未知の request の bit は EINVAL、CPU が答えない sample は 0 と `SAMPLE_TIMEOUT`、範囲外は ENOENT）、GET_CPU_DOMAIN（RANGE は steps[0..1] に両端、STEPS は並び）、SET_CPU_DOMAIN（書き込みで開いた fd だけ、未知の field・reserved は EINVAL、範囲外の domain は ENOENT、ALL）、GET・SET_CPU_RELATIVE（SET は書き込みで開いた fd だけ）、GET_CPU_IDLE（書き込みで開いた fd だけ、scope は CORE）、GET_PACKAGE_IDLE（誰でも、scope は PACKAGE、CPU の無い package は ENOENT）。読みだけの fd は EPERM。誰が書き込みで開けるかは `/dev/thermal` の mode と group（root:desktop 0664）が決める。UAPI と枠組みの同じ意味の値が同じ事を `_Static_assert` で確かめる。`kern_cpuperf_package_count()` を足した（`include/kern/cpuperf.h`、GET_STATUS の `cpu_package_count`）。
- `userland/base/thermal/main.c`（新、`/sbin/thermal`）: 要約・sensors・fans・meters・receivers・strategy（表示と選び、manual の 3 軸）・restore・boost・cpu（一覧・1 つ・sample・domains・set・relative）・idle（cpu・package、`-i` 秒で割合）・power（`-i` 秒、W）・watch（事象の種類を選べる）。1 行 1 項目、key=value。終了の値 0・1（kernel の拒み、理由 1 行）・2（使い方）。`/dev/thermal` は書き込みで開き、EACCES（desktop の group の外）なら読みだけで開く（変更は kernel が EPERM で拒む）。strategy は kernel が値を受け取った時（EOPNOTSUPP と受け手の失敗を含む）に `/etc/thermal.conf`（root:desktop 0664）に書く: root は一時 file を root:desktop 0664 にして fsync・rename、desktop の group の利用者（`/etc` に file を作れない）は今ある file を上書きして ftruncate・fsync。EOPNOTSUPP は `strategy NAME result=unsupported kept=yes` と 0。`restore`（root、起動の時）は conf が無ければ空で作り、group と mode が違えば root:desktop 0664 に直し、1 行を読む（無い・空・壊れていれば balanced、壊れていれば警告）。strategy は file に書き直さない。
- `userland/base/thermal/service`（oneshot、`/sbin/thermal restore`、late）と `userland/base/etc/rc.conf` の `thermal`（enabled、optional）。service の file を image に入れるのは mk の案（p005 の 3・5）の後。

## 確かめた事

- host 試験 `plan/ws333/tests/cpuperf-host/run.sh <out>`: 5 scenario PASS（新しい ioctl の scenario は 52 checks: 範囲の登録、照合、CPU の topology と種類と anchor、sample の書き込みの fd と答えない CPU の SAMPLE_TIMEOUT、domain の RANGE と範囲外、直の設定の書き込みの fd・field・reserved・ENOENT・ALL と CUSTOM、relative の選び、idle の書き込みの fd と scope と ENOENT。読みだけの fd は EPERM）。ASan・UBSan でも PASS。
- `/sbin/thermal` の compile: x86_64・arm64（rpi5 の sysroot）・riscv64（vf2 の sysroot）で `-Wall -Wextra -Werror` の warning 0。
- QEMU amd64: p005 の後半の get-set の試験で strategy の保存（`cat /etc/thermal.conf`）・`thermal restore`・manual・`thermal cpu`・`thermal power`・boost の拒みを画面で確かめた（[p005](../phase005/phase.md) の「確かめた事（後半）」）。
- 2026-11-12 P1（q2015-i01、desktop の group）: image は `make CONFIGMK=<x86_64 の config> CONFIG_KERN_THERMAL=y CONFIG_DRIVER_X86_POWER_CONTROL=y CONFIG_THERMAL_TEST_INJECT=y CONFIG_GRAPHICAL_LOGIN=n CONFIG_USERLAND_PROGRAMS="login passwd su useradd account-admin id ls cat thermal sync service passkey" programs` の後に同じ変数で `build/x86_64/hdd-image.img`（x86_64 の `defaults.mk` が `CONFIG_KERN_THERMAL := n` を置くので y は command line で渡す）。`thermal-inject` は手で compile して `build/x86_64/sbin/` に置いた。QEMU amd64（`plan/ws333/tests/thermal-qemu/run.sh IMAGE OUT group`、画面の読み取り）: PASS。起動の時の service の `thermal restore` が `/etc/thermal.conf` を root:desktop 0664 で作り（`ls -l` で `/dev/thermal` も `crw-rw-r-- root desktop`）、`id user` に `45(desktop)`、`su user -c 'thermal strategy powersave'` が generation 2 で通り終了 0、file に `strategy powersave`、`useradd bob`（group の外）の `su bob -c 'thermal strategy balanced'` は `thermal: strategy: Operation not permitted` で終了 1、bob は strategy を読めて file は powersave のまま。get-set の試験も PASS（起動の時の restore が balanced の generation 1 を渡すので、後の世代を 1 つずつ進めた）。画面 `build/review/ws333-q2015/qemu-group.png`・`qemu-get-set.png`（P1 の worktree）。
- 未実施: `/sbin/thermal` の strategy の保存と restore の host 試験（QEMU で代わりに確かめた）、MSR の道の無い amd64 の build（既定は扱わない、ws.md の前提）、全 port の vmunix、riscv64 の virt の QEMU、idle と power の実の値（5330 の p014）、Settings が作った利用者が desktop の group に入る事の QEMU（account-admin の host 試験は PASS、group の無い試験の file では入らない道）。

## 締め（2026-11-12 P1、q2016-i01）

- 起動の時の restore: 新しい build の正式な image（`userland/base/thermal/Makefile` が `/sbin/thermal` と `/etc/service.d/thermal` を入れた物、作り方は [p005](../phase005/phase.md) の「締め」）で、QEMU amd64 の get-set の試験の最初の `thermal` が `strategy balanced generation=1 result=unsupported`（起動の時の service が保存の無い conf から balanced を渡した）。group の試験で `/etc/thermal.conf` が root:desktop 0664 で作られている。4 本（get-set・emergency・direct・group）PASS。
- amd64 の vmunix の MSR の道の有無の 2 通り: x86_64 の default（`CONFIG_KERN_THERMAL=n`、MSR の道の symbol 0）と thermal（`CONFIG_DRIVER_X86_POWER_CONTROL=y`、`cpux86_hwp*` 6）が rc=0 warning 0。`CONFIG_KERN_THERMAL=y` で MSR の道だけ n の組は build していない（前提の外）。
- 全 platform の vmunix: [p005](../phase005/phase.md) の「締め」の表。intelmid64 は mk の重複で落ちる（mk の案 1）。
- 未実施: riscv64 の virt の QEMU（新しい build に platform が無い）、`/sbin/thermal` の保存と restore の host 試験（QEMU で代わりに確かめた）。
- cleared の材料: CPU の ioctl の host 試験 PASS（52 checks、q2013-i01）、amd64 の 2 通りの build、QEMU amd64 の sensor・fan 0・受け手（cpu は unsupported）・topology・起動の restore。受け入れの「riscv64 の virt でも `/sbin/thermal` が動く」だけが未達で、原因は build system（WS333 の source ではない）。Q1 がこの項を外すか、riscv64 の platform を待つかを決める。
