<!-- awesome-plan project=zedbsd record=ws333-p011 -->

# ws333-p011: x86 の CPU の driver の統合

Status: in-progress（受け入れの確かめ（host 試験・amd64 の 2 通りの build・全 port の build・QEMU）は全部済み。2026-11-01 q1788 で 5330 の実機（HWP・HFI・RAPL・`cpuperf=off`）も動いた。cleared は Q1 の判定）
Parent: [WS333](../ws.md)
Queue: q1770-i01（P7）

## 目的

design.md §9 の p011: [p006](../phase006/phase.md) の cpuperf の枠組みの provider の口に、[p008](../phase008/phase.md)〜[p010](../phase010/phase.md) の decode の module を使う x86 の CPU の driver を繋ぐ。関門（hypervisor・CPUID の bit・allow list・既定 n・`cpuperf=off`）、MSR の小さな file、probe、HWP の enable の順と既に 1 の道、Highest の MHz の測り、HFI の page、RAPL の meter（[p005](../phase005/phase.md) の `kern_thermal_meter_register`）・滞在の統計、provider の登録、起動の parameter の key。

## 判断

- 2026-10-31 ユーザーから、昼頃まで不在の間は実装と試験を進める、という趣旨の指示を受けた（Q1 経由）。
- Q1 の判断: legacy の 0xCE の比を PERF_CTL に書く道は SDM に根拠が無いので作らない（HWP の道だけ、design.md §15.2・§15.7・§15.8 の Q1-d）。AMD・`_PSS` は仕様が取れておらず範囲の外（Q1-b）。HWP の無い Intel の CPU は provider の `apply` 無し（describe だけ）。
- 設計の読み（design.md の中で決めた、普通の技術の判断）:
  - **`cpuperf=off` は driver 全体を止める**（provider を登録しない。枠組みの既定の表のまま）。診断の switch なので、topology の読みも含めて driver の code を一切走らせない方を採った。
  - **hypervisor の下は topology だけの provider**（design.md §4.5・§4.14「ABSENT（MSR の道）… topology は出す」）。MSR には一度も触らず、`start`・`apply` などは NULL。`CONFIG_CPU_PERF=n` も同じ形（RDMSR・WRMSR は compile されない）。
  - **`cpuperf.hwp=off`**: HWP が off のままなら HWP を enable せず describe だけ（meter と滞在は残る）。firmware か前の OS が HWP を既に enable していたら（reset まで消えない）log を 1 行出して HWP の道のまま（WS301 design §4.2）。
  - Intel 以外の vendor は provider を登録しない（decode は Intel の SDM の leaf の読み）。
  - module（`kern_cpuperf_topology.module`）は L2 cache を共有する CPU の群（leaf 04H の共有の bit）。core・die は x2APIC ID のその domain より上の bit で番号を付ける（machine の中で一意）。
  - domain の共有は `KERN_CPUPERF_SHARE_CORE`（SMT の兄弟は 1 つの clock。E-core の 4 つ組の共有は未確認なので core ごと、design.md §4.4・WS301 §4.3）。
  - RAPL の meter は 32 bit の生の計数を返し、一周の補いと µJ への換算は熱の core（`kern_thermal_meter_ops` の単位 10^9 / 2^shift nJ）。`cpux86_energy_counter_update()`・`cpux86_energy_microjoules()` は driver では使わない（core と同じ事を二重にしない）。package が 1 つの機械だけで登録（`thermal.h` の約束）。platform の計数（64DH）は最初の読みが 0 なら支えが無いと見て登録しない。
  - 滞在: package は PC2・PC3・PC6・PC7、core は CC3・CC6・CC7（allow list の物）を TSC の rate（0xCE の最大の非 turbo の比 × 100 MHz、§15.7 の近似）で ns に。core の `available` は CPUID.05H の sub-state の数が 0 でない状態。MC6（664H、module の scope）は `kern_cpuperf_idle` に scope の欄が無く CC6 と状態の番号がぶつかるので出さない（下の持ち越し）。
  - HFI: provider の登録の後に package ごとに表（CPUID の頁の数、4 GiB 未満、§15.7 の TME の注意）と driver の写しを確保し、package の最初の CPU の `start` で PTR → CONFIG（bit 0 だけ、Thread Director は使わない）。既に CONFIG bit 0 が立っていれば（前の software の表）触らない。写しは feedback の notify（bit 26 を見て写し、`cpux86_feedback_ack()` の値を書く）、読みは seq（奇数の間は書き換え中）で守る。
  - 測り（`measure`）と `sample` の delivered MHz は APERF/MPERF の差 × 0xCE の最大の非 turbo の MHz（SDM Vol. 3B §22.7.2: TSC と MPERF は最大の非 turbo の周波数の近くで数える）。0xCE が allow list の外の model では `measure` を出さない（相対の性能は UNKNOWN のまま）。
- design.md §4.3.3 の段 2（0xE2 の package の C-state の上限を provider の口に）: `kern_cpuperf_provider_ops` に口が無く（p003 の header）、使うかは Q333-2 の答えの後なので作らない。

## 成果（branch agent/p7-ws333b）

- `src/drivers/platform/pcat/cpu-x86/`（新。名前は decode の module の `cpux86*` と分け、p007 の道具の glob に入らない）:
  - `cpu-x86-insn.c`・`.h`: CPUID・RDMSR・WRMSR の inline asm（RDMSR・WRMSR は `CONFIG_CPU_PERF` の時だけ）。driver が命令を実行する唯一の file。
  - `cpu-x86.c`・`.h`: `drv_cpu_x86_init()`（`cpuperf=off`・vendor・CPU の記録・関門・provider の登録）、`topology`（CPU の上で 1FH か 0BH・04H・05H・06H・1AH）・`describe`。全 amd64 の kernel。
  - `cpu-x86-control.c`（`CONFIG_CPU_PERF=y` だけ）: probe（allow list・0xCE・leaf 06H・PM_ENABLE の読み・HFI の layout）、`start`（PM_ENABLE が 0 なら 1 を書いて読み直し → HWP_CTL の bit 0 を 0 に → HWP_INTERRUPT に 0 → CAPABILITIES → HFI の enable）、`apply`（REQUEST を encode して書く。EPP の無い CPU は 0、`KERN_CPUPERF_UNKNOWN` は今の EPP、window・bit 42・valid は 0）、`sample`（CAPABILITIES・HWP_STATUS の excursion を PLATFORM_LIMIT に写し ack・delivered MHz）、`measure`、`feedback`・`feedback_read`・`relative`（FEEDBACK）、`idle_package`・`idle_core`、meter（package・cores・graphics・platform）。
  - `cpu-x86-private.h`: 2 つの file が共有する machine と CPU の記録。
- `include/kern/boot.h`・`src/kern/boot.c`: 起動の parameter の key `cpuperf=`・`cpuperf.hwp=`（on か off）。driver は parse の前（platform の attach）に loader の行を `kern_boot_parameters_token_present()` で読む（i915.gen9= と同じ）。触った comment の開発の管理情報（ws の番号 4 か所）を消した。
- `src/kern/platform/pcat.c`: UEFI の変数の後に weak の `drv_cpu_x86_init()`（i386 の pcat の kernel は link しない）。触った comment の WS159 を 2 か所消した。
- `Makefile`: `CONFIG_CPU_PERF ?= n` と `-DCONFIG_CPU_PERF=0/1`（platform の config の stamp に入る）。`config/kernel-options.list` に menuconfig の項目（amd64、既定 n）。
- `platform/amd64/vmunix.mk`: `cpu-x86.c`・`cpu-x86-insn.c` は全 amd64、`cpu-x86-control.c` は y だけ。
- 試験: [tests/cpu-x86-host/](../tests/cpu-x86-host/)（`run.sh <out>`）、[tests/cpu-x86-qemu.sh](../tests/cpu-x86-qemu.sh)、[tests/config-amd64-cpuperf.mk](../tests/config-amd64-cpuperf.mk)。

## 確かめた事

- host 試験 `plan/ws333/tests/cpu-x86-host/run.sh /tmp/p7-ws333-p011/host2` → `cpu-x86-host: PASS`。driver の source をそのまま host の cc（`-Wall -Wextra -Werror`）で compile し、CPUID は場面ごとの表（CPU ごと）、MSR は「その processor に有る register か・今触ってよいか（HWP の他の register は PM_ENABLE の後だけ、INTERRUPT は CPUID の bit だけ）・予約の bit を書いていないか・hypervisor の下では全部」を見て、外れたら #GP の代わりに fault を数える模型。y の build で 13 場面（5330-topology 166・5330-control 139・5330-feedback 80・hwp-on-at-boot 57・hwp-off 50・cpuperf-off 4・hypervisor 45・not-intel 2・unlisted 47・no-hwp 49・no-epp 48・hfi-on-at-boot 49・two-packages 45 の check、失敗 0、fault 0）、n の build（RDMSR・WRMSR の模型を link しないので参照があれば link で落ちる）で 3 場面（without-control 44・cpuperf-off 4・not-intel 2）。ASan・UBSan の build（`SANITIZE="-fsanitize=address,undefined -fno-sanitize-recover=all"`）でも PASS。
  - 5330 の形（i5-1245U: P-core 2 の SMT と E-core 8、x2APIC 0・1・8・9・16〜30、P の能力 56・21・20・1、E 33・12・15・1、06_9AH）: 起動で MSR を 1 つも書かない、topology（core・thread・L2 の module・P/E）、RANGE と CORE、start の順（最初の書きが PM_ENABLE）、HWP_CTL の bit 0 を落とす、HFI の PTR と CONFIG=1、apply の encode（EPP・UNKNOWN の EPP・範囲外は EINVAL で書かない）、sample の excursion と ack、delivered と measure（4400・3300 MHz、大きな差での溢れ無し）、HFI の写し・ack・行の読み・相対、滞在の ns（1600 MHz の TSC）、meter 3 本と単位 1/2^14 J。
  - わざと壊す確かめ（scratchpad の写しで 1 つずつ）: hypervisor の関門・HWP_CTL の書き・PM_ENABLE の書き・EPP の関門・HFI の前の software の検査・rate の式・meter の 1 package の条件・`cpuperf.hwp=off`・share・L2 の module・HFI の ack・`cpuperf=off`・P/E の写し・platform の meter の 0 の検査・enable の前の CAPABILITIES の読み、の 15 個と、滞在の 2 つの関門を両方外した物、全部を該当の場面が FAIL で捕えた（滞在の関門は allow list と TSC の rate の 2 重なので、片方だけ外したものは捕えない＝冗長の守り）。
- decode の module の道具（p007）: `BASE=39f4a5597 check-skeleton.sh` PASS（header は不変）、`run-unit.sh all` PASS。
- amd64 の vmunix: `make ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=/tmp/p7-ws333-p011/amd64-def …/vmunix`（既定 n）と `CONFIG_CPU_PERF=y`（`BUILD=/tmp/p7-ws333-p011/amd64-n`、名前は最初の試しの誤りで中身は y）の 2 通りで rc 0・warning 0（a904b540d の tree）。その後の log の 1 行の追加（9eec145cc）の後は、下の QEMU の 2 つの image の build（y と既定）で kernel を含め warning 0。
- 全 port の vmunix: `plan/tools/ports/build-vmunix-all.sh /tmp/p7-ws333-p011/all`（a904b540d の tree）で 20 port 全部 rc 0・warning 0。pcat だけ 1 回目が rc 2（`stdbool.h` が無い）: 同時に走った pc98 と、この worktree の `build/i386/sysroot`（main の sysroot への symlink を置いていた所）を make が作り直す途中でぶつかった物で、pcat だけを再実行して rc 0・warning 0（`/tmp/p7-ws333-p011/all-pcat/pcat.log`）。main の `build/i386/sysroot` は日時・中身とも変わっていない事を見た。
- QEMU+KVM（`-cpu host`、hypervisor の bit が立つ。host は Xeon Gold 6130、06_55H）: [tests/cpu-x86-qemu.sh](../tests/cpu-x86-qemu.sh)（image の build → `plan/tools/boot-test.sh` で login prompt → 模擬の keyboard で root の login と `dmesg | grep cpu-x86` → 画面の PNG。もう 1 度起動して HMP の `x` で枠組みの `cpuperf_provider` と driver の `cpu_x86_provider` の 12 語を読む。console・serial の log は判定に使っていない）。
  - `CONFIG_CPU_PERF=y`（[tests/config-amd64-cpuperf.mk](../tests/config-amd64-cpuperf.mk)、9eec145cc の tree、`IMAGE_BUILD=/tmp/p7-ws333-p011/qemu-y/img … /tmp/p7-ws333-p011/qemu-y2`）: **PASS**。画面に `cpu-x86: running under a hypervisor; the model-specific registers are left alone` と `cpu-x86: family 06H model 55H, 4 CPUs described`。HMP: `cpuperf_provider` = `cpu_x86_provider`（0xffffffff80f79850）、topology・describe だけが非 0 で start・apply・sample・measure・feedback・idle などは全部 0（関門で止まり、MSR の道の provider が無い）。PNG `build/review/ws333-p011/qemu-cpuperf-y.png`（worktree の中、git に入れない）。1 回目（qemu-y）は BOOT_THEN が root の password を待たずに落ちた（道具の手順の誤り、HMP の判定は同じく PASS）ので直してやり直した。
  - 既定（n、[tests/config-amd64-cpu-x86.mk](../tests/config-amd64-cpu-x86.mk)、`/tmp/p7-ws333-p011/qemu-n`）: **PASS**。画面に `cpu-x86: family 06H model 55H, 4 CPUs described`（hypervisor の行は無い: n の kernel は関門の code を持たない）、HMP は y と同じ形（0xffffffff80f78880）。driver は全 amd64 の kernel で topology を読むので、既定の kernel の起動の回帰として流した。PNG `build/review/ws333-p011/qemu-default.png`。

## 5330 の実機（2026-11-01、q1788-i01、P1）

- 道具: [tests/hw5330/](../tests/hw5330/)。`5330-thermal.sh p011 VMUNIX NAME`（kernel を NVMe の p4 へ → BootNext で起動 → 60 秒 → 測る → Debian へ戻す → `cpuperf=off` を zedbsd.cfg に足してもう 1 回起動 → cfg を戻す → 要約）。`/dev/thermal` の UAPI（p004）がまだ無いので、試験だけの kernel の patch `thermal-diag.patch`（main に入れない）で root の `hostname td-dump`・`td-s-NAME` が `kern_thermal_*`・`kern_cpuperf_*` を呼び、kernel の log に `thermal-diag:` の行を出す。判定は SSH の dmesg（実機）。要約は `td-summary.py`。
- kernel: main 82c6ef325 ＋ `CONFIG_CPU_PERF=y` ＋ thermal-diag.patch ＋ `plan/ws291/tests/i915-stats-diag.patch`（`sh plan/ws333/tests/hw5330/kbuild.sh build/q1788/kA "CONFIG_CPU_PERF=y" …`、rc 0、warning 0、sha256 先頭 e3a19c937043b189）。userland は main 82c6ef325 の image（config-5330.mk）。
- 3 回流した: q1788-p011（負荷が動かず）、q1788-p011b（同じ。`sysctl -n` が zedBSD に無く CPU の数が読めなかった。道具の誤り）、q1788-p011c（直した後、下の数）。起動の行と idle・strategy の結果は 3 回とも同じ形。log は worktree の `build/hw5330/ws333/q1788-p011c/`（git の外）。

| 確かめ | 結果（q1788-p011c） |
| --- | --- |
| 起動の行 | `cpu-x86: family 06H model 9aH, 12 CPUs described, hybrid`、`HWP on first request, feedback on first request, base 2500 MHz, 1 package`、`4 energy meters, 1/2^14 J per count`、`cpuperf: provider registered; 12 of 12 CPUs answered, 10 domains`（P-core の SMT の 2 組と E-core 8 つ） |
| HWP の start（strategy performance の最初の request） | error 0、receiver "cpu" の apply 100 ms。全 CPU が HAS_CONTROL。HWP の能力 P 1/11/21/56、E 1/8/12/33（lowest/efficient/guaranteed/highest。host の模型の efficient 20・15 と違う、模型の値は推定だった） |
| Highest の MHz の測り | `cpuperf: cpu 0 (kind 1) runs at most about 4310 MHz`（他の回 4344）、`cpu 4 (kind 2) … 3300 MHz` |
| HFI | start の後に全 CPU で feedback が読める: P 56/92、E 33/100（available/efficiency）、相対 P 100・E 77。start の前は UNKNOWN |
| RAPL の meter | 4 本とも進む。idle 11 秒: package 6.44 W・cores 1.12・graphics 0.67・platform 18.04。全 CPU の負荷（performance）13 秒: package 25.19・cores 19.43・platform 37.89 |
| strategy の request（domain の min/max/desired/EPP） | performance P 21/56/0/0・E 12/33/0/0、powersave P 1/21/0/192・E 1/12/0/192、balanced P 1/56/0/128・E 1/33/0/128。どれも error 0 |
| 全 CPU の負荷での delivered MHz | performance: P 1399〜1400・E 1199〜1200（package 25.2 W、TCPU 71 °C）、powersave: P 1600・E 1200（11.6 W、67 °C）。performance の P が powersave より低い理由は未確認（一部の CPU に PLATFORM_LIMIT の印 0x8）。Linux の同じ負荷の数は取っていない |
| package の C-state の滞在 | idle で PC2・PC3・PC6・PC7 とも増えない（PC2 は起動の頃の 16.8 ms のまま）。package が深い C-state に入っていない（p012 の件） |
| `cpuperf=off` | `cpu-x86: turned off by cpuperf=off; the CPUs keep the default description`、meter 0 本、CPU の class 0（既定の記述）、package の滞在は EOPNOTSUPP。起動と SSH は普通 |

- 参考（Debian 6.19、同じ機械、iGPU は vfio-pci で画面の driver なし、powercap の energy_uj を 10 秒）: package 3.75 W・core 0.04 W・psys 13.91 W。条件が違うので比は出さない。
- 判定: 起動の関門・HWP の enable の順・HFI・RAPL・`cpuperf=off` は実機で動いた。cleared は Q1 の判定。

## 未実施

- 他の amd64 の QEMU の形（TCG の `-cpu max`、hypervisor の bit の無い CPU の模型）、Intel Mac の config の起動。
- 5330 の実機: 2026-11-01 に上の表（q1788）。E-core の 4 つ組が clock を共有するか、0xE2 の lock、MC6 は未確認。全 CPU の負荷で performance の P-core が 1.4 GHz に留まる理由は未確認。
- PC8〜PC10（630H〜632H）の滞在は読まない（allow list に無い、§15.8 の Q1-c）。5330 の 0xE2 の lock の bit も未読（Q1-e）。

## 持ち越し

- p013: CPU の ioctl で `kern_cpuperf_*` を出す時、hypervisor の下と `CONFIG_CPU_PERF=n` は topology だけ（controls 無し）。
- p014: 5330 で HWP の start・strategy の写し・HFI の表・meter・滞在を確かめる。E-core の 4 つ組が clock を共有するか（共有なら share を MODULE に）。
- MC6（module の C6）の出し方: `kern_cpuperf_idle` に scope の欄を足すか、別の口（p004・p013 の UAPI の時）。
- 0xE2 の package の C-state の上限: Q333-2 の答えと provider の口の追加（kernel の header）の後。
