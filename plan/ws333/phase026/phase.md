<!-- awesome-plan project=zedbsd record=ws333-p026 -->

# ws333-p026: CPU の MHz を読み手の権限によらず新しい値で返す（UAPI の直し）

Status: cleared（2026-11-13 Q1 判定: 受け入れの host 試験と build、QEMU の起動と群の外の読みを確かめた。5330 の数は p027 か次の 5330 の測定にまとめる）
Parent: [WS333](../ws.md)

## 目的

`KERN_THERMAL_GET_CPU` の `delivered_mhz` を「今の周波数」にする。今は読み直し（`KERN_THERMAL_CPU_READ_SAMPLE`）を書き込みで開いた fd だけに許し、無ければいつ取ったか分からない最後の sample を印も無く返す。p014 の MHz の食い違いはこの古い値から生まれた。

## 決まった形（2026-11-13 ユーザーの承認の要点）

- 読み手の権限によらず、新しい値を返す。sample が約 1 秒より古ければ kernel が取り直す（約 100 ms）。
- 費用は権限でなく、新しい sample を全ての読み手で共有して抑える（同時の読み手は進行中の 1 回を待つ）。
- `READ_SAMPLE` の印と、書き込みだけに許す制限を外す（`include/uapi/thermal.h` の struct の `request` の扱い、`/sbin/thermal` の一覧、`docs/architecture/power-management.md`、Keiland の monitor の読み方を合わせる）。HAL の API は変えない。

## 受け入れ

- 読みだけの fd で 2 回続けて読むと、2 回目は共有の sample を返し、1 秒の後は取り直す（host の試験）。
- `thermal cpu` が読みだけの fd でも今の MHz を出す。x86_64・aarch64 の build warning 0、host の試験 PASS。5330 は次の測定にまとめる。

## 結果

2026-11-13 P1（q2027-i01、branch agent/p1-ws333j、基点 main 3371056d4f）。

### 形（範囲の中の技術の判断）

- **1 つの測りを機械の全 CPU で共有**: GET_CPU の読み手が、その CPU の rate が約 1 秒（`CPUPERF_FRESH_MS` 1000）より古いと見たら、kernel が全 CPU を 2 回（`CPUPERF_SAMPLE_GAP_MS` 100 の間を空けて）sample し、2 回目の sample の rate を CPU ごとに時刻と一緒に残す（`src/kern/cpuperf.c` の `cpuperf_fresh_ensure`・`cpuperf_fresh_measure`）。1 CPU ずつ測ると `thermal cpu` の一覧が CPU の数 × 100 ms かかり各行の時刻もずれるので、全 CPU を一度に測る（一覧は 100 ms を 1 回だけ待つ）。
- **同時の読み手**: 測りは mutex（`cpuperf_fresh_mutex`）を持って行う。測りの間に来た読み手は mutex で待ち、取った後に rate が新しいのを見てそれを使う（2 回目の測りをしない）。読み手の数によらず、各 CPU を起こすのは 1 秒に 2 回まで（深い idle の CPU の測りの費用の上限）。
- **古い値を返す道を消した**: 起こさない読み（kernel の中の `kern_cpuperf_cpu(cpu, 0, …)`）も、測りが 1 秒より古ければ `delivered_mhz` 0（不明）。起動の直後（測りが無い時）も 0。strategy の apply が取る sample（間隔の分からない rate）はもう読み手に見せない。PLATFORM_LIMIT の flag も測りの 2 回目の sample の物だけ。
- **答えない CPU**: 1 回目か 2 回目の sample に 1 秒で答えない CPU は rate 0・`SAMPLE_TIMEOUT`（他の欄は埋める）。その結果も 1 秒の間は共有する（読み手ごとに 1 秒待たない）。1 回目に答えない CPU には 2 回目を送らない（2 つの sample が 1 回の notify で 1 回に潰れ、間隔の分からない rate になるのを避ける）。
- **apply と重なった sample**: `cpuperf_run` は読み手の SAMPLE を APPLY と重なった時も先に取り、その結果を box の `read` に写す（前は APPLY と重なると読み手の sample を取らず、control の無い CPU では sample が取られなかった）。
- **UAPI**: `KERN_THERMAL_CPU_READ_SAMPLE` を外し、`struct thermal_cpu_info` の `request` を同じ位置の `reserved0`（in: 0、0 以外は EINVAL）にした。struct の大きさ（128）と配置は同じ。`KERN_THERMAL_CPU_SAMPLE_TIMEOUT` は残し、意味を「sample に答えない、delivered_mhz は 0」に。頭の規則の注・struct の注を直した。GET_CPU_IDLE（読むたびに CPU を起こす）の書き込みの制限は残した（範囲の外）。
- **kernel の口**: `KERN_CPUPERF_READ_SAMPLE` を `KERN_CPUPERF_READ_FRESH`（約 1 秒以内の rate を待って得る）に改め、`kern_cpuperf_cpu` の注を新しい意味に。HAL の API・Makefile・mk/ は変えていない。
- **`/sbin/thermal`**: 一覧は request を付けず、p025 の EPERM の後戻しを外した。`thermal cpu <n> sample` の `sample` を外した（`thermal cpu <n>` が新しい値を出す。`thermal cpu 1 sample` は usage で終了 2）。
- **他の使い手（grep `READ_SAMPLE`・`delivered_mhz`・`GET_CPU`）**: Keiland の monitor（`monitor-zedbsd.c`）は温度だけで GET_CPU を読まない（変更なし）。`docs/architecture/power-management.md`（権限の箇条・`thermal cpu` の使い方・mhz の意味）、`plan/ws333/design.md` の権限の文、5330 の script（`5330-p025.sh` の `cpu 0 sample` → `cpu 0`）と `limit-diag.patch`（診断の値を読み手の sample の `box->read` から、`git apply --check` 通る）を合わせた。`plan/ws333/phase004/layout-test.c`（p004 の案の header の確かめ、どこからも流されない）は `KERN_CPUPERF_READ_SAMPLE` を見るので今の header では compile しない。直さず残した（消すかは Q1）。

### 確かめ

- host 試験（worktree、`build/p026/` は git の外）:
  - `sh plan/ws333/tests/cpuperf-host/run.sh build/p026/cpuperf-host`: PASS（range 143・ioctl 94 check ほか全 9 場面）。足した場面: 測りの前の起こさない読みは 0、READ_FRESH の後に起こさない読みが共有、1 秒の後は 0、答えない CPU は ETIMEDOUT と 0 で他の CPU は測れる。ioctl: 読みだけの fd の最初の読みが全 CPU を 2 回ずつ sample して新しい値（1003）、1 秒の中は読みだけ・書き込みの fd とも sample 無しで同じ測り（rate を変えても 1007・1003）、1 秒の後に全 CPU を取り直し（2002、sample が 4 回）、`reserved0` が 1 で EINVAL、答えない CPU は `SAMPLE_TIMEOUT` と 0・他の CPU は 2006。harness の小さな不具合も直した（fork の子が親の failure の数を引き継ぎ、前の場面の FAIL が後の場面にも数えられていた）。
  - `sh plan/ws333/tests/thermal-host/run.sh build/p026/thermal-host`: 30 場面 PASS（5 回流して 5 回とも）。cpuperf.c を一緒に link し、新しい場面 device-cpu-fresh: `/dev/thermal` を読みだけで開き、host の thread 2 本が同時に GET_CPU（2 本目は 1 本目の測りの最中に始まる）→ 測りは 1 回（各 CPU 2 sample）で両方が同じ測りの値、1 秒（試験の時計で）の中は sample 無し、1 秒の後は取り直し（各 CPU 4 sample）、`reserved0` 1 は EINVAL、provider の op は全部その CPU の上で呼ばれた。
  - `sh plan/ws333/tests/thermal-tool-host/run.sh build/p026/tool`: PASS。model を新しい kernel の答え（どの fd でも新しい値、1 秒の間は 1 回の測りを共有、`reserved0` 0 以外は EINVAL）に。読みだけの open で一覧が新しい値（1 回の測りで 3 CPU）、`thermal cpu 1` が同じ測り、取り直しの後は新しい値、書き込みの open も同じ、`thermal cpu 1 sample` は終了 2。
  - 直す前の code で FAIL: 基点の `include/kern/cpuperf.h`・`include/uapi/thermal.h`・`src/kern/cpuperf.c`・`userland/base/thermal/main.c`（`git show HEAD:`）と新しい試験を `-Dreserved0=request -DKERN_CPUPERF_READ_FRESH=KERN_CPUPERF_READ_SAMPLE` で build して流した。cpuperf-host は range・ioctl が FAIL（読みだけの fd で sample されない、1 秒の後に取り直さない、測りの前に strategy の sample を返す、timeout が出ない）、thermal-host は device-cpu-fresh が FAIL（10 check、読みだけの fd で delivered 0・sample 0）、thermal-tool-host は読みだけ・書き込みとも FAIL（古い tool が READ_SAMPLE を送り EINVAL で終了 1）。
- build（worktree の `build/`、main の `build/` は使わず symlink もしない、-j4・`BUILD_JOBS=4`）:
  - `make -j4 BUILD_JOBS=4 CONFIGMK=config/config.x86_64.mk llvm-toolchain-cached` rc 0 → `make -j4 BUILD_JOBS=4 PLATFORM=x86_64 CONFIGMK=config/config.x86_64.mk CONFIG_KERN_THERMAL=y CONFIG_DRIVER_X86_POWER_CONTROL=y sysroot` rc 0 → 同じ変数で `build/x86_64/vmunix` rc 0 warning 0（cpuperf.c・cpu-x86 を compile）、`userland/base/thermal` rc 0 warning 0（sysroot の `uapi/thermal.h` が `reserved0` の物）。
  - rpi5: `make -j4 BUILD_JOBS=4 PLATFORM=rpi5 CONFIGMK=config/config.rpi5.mk sysroot` rc 0 → `build/rpi5/vmunix` rc 0 warning 0（cpuperf.c・thermal-device.c・rpi5-fan を compile）→ `userland/base/thermal` rc 0 warning 0。
- QEMU amd64（image は `sh plan/ws333/tests/thermal-qemu/build-image.sh build/p026/qimg`、warning 0。build-image.sh の `-j16` を `-j4 BUILD_JOBS=4` に直した）: `plan/tools/boot-test.sh build/p026/qimg/hdd-image.img`（uefi-nvme、`OUTPUT=build/p026/boot`）PASS（画面で login prompt）。`plan/ws333/tests/thermal-qemu/run.sh … group` PASS: 足した段で、desktop の group の外の bob（読みだけの open）の `thermal cpu 0` が終了 0 で `delivered_mhz 0`・`flags -`（QEMU は provider が無く rate は 0、EPERM・EINVAL にならない）。`run.sh …`（get-set）PASS（`thermal cpu` の一覧 `cpu 0 core=P … mhz=0`）。画面の文字は `build/p026/tq/*-after.txt`。
- 最後の commit の code（注だけ直した後）で x86_64（`CONFIG_KERN_THERMAL=y CONFIG_DRIVER_X86_POWER_CONTROL=y`）と rpi5 の `vmunix` を build し直して rc 0・warning 0、3 つの host 試験も再び PASS。QEMU の image は注を直す前の cpuperf.c（code は同じ）
- 未実施: 実機（5330 は次の測定にまとめる、Q1）。QEMU には cpuperf の provider が無い（hypervisor の関門）ので、QEMU で新しい値そのものは見られない（host 試験で確かめた）。
