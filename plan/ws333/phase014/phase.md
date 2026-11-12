<!-- awesome-plan project=zedbsd record=ws333-p014 -->

# ws333-p014: 5330 の CPU の受け入れ

Status: in-progress（2026-11-12 P1・q2018-i01: 1（idle の電力）と 4 の hwp=off・相対の性能は取れた、3 は一部、2（i915）と S0 idle は未実施。cleared の判定は Q1）
Parent: [WS333](../ws.md)

## 目的

design.md §8.4 の 5330 の 1〜4 と §1 の受け入れの柱を、1 本の script（配置 → 起動 → 実行 → 回収 → 要約）で確かめる。§4.3.3 の段 4（idle の CPU の tick を止める、HAL の契約の外）の要否の数を取る。WS299（5330 の NVMe の速さ、CPU の idle と P-state が律速と見られる）の待ちを解く数にもなる。

## 依存

- p011（x86 の driver）、p012（tick の位相）、p013（`/sbin/thermal` の CPU と strategy。p004 の答えが遅れる時は p005 後半の注入の口か、試験だけの diag の patch で strategy を選ぶ）。
- Q1 の実機の割当（5330）。

## 受け入れ（design.md §8.4）

1. idle の電力: 同じ image で kernel を替え HLT（`IDLE_WAIT_DEEPEST_CSTATE=1`）・MWAIT C6（既定）・C10・tick の位相の有無（`LAPIC_TICK_TSC_DEADLINE`）・strategy BALANCED の各 60 秒、package の電力（RAPL の meter）と package・core の C-state の滞在。同じ日に Linux の値。
2. i915: WS291 p022 の qs40 × 600 の fps と GT の周波数。
3. HWP の 4 strategy と直の設定で 1 CPU の spin の MHz が要求に従う。E-core の 4 つ組の共有。
4. `cpuperf.hwp=off`・相対の性能の基準・S0 idle の回帰と S0 idle の後の要求。
- PASS: 1〜4 の数が全部取れ、MWAIT の idle の package の電力が HLT より低く、3 の MHz が要求に従う。数の目標（Linux との差）は design.md §11.2 の 9 で Q1 が決めた値。package の深い状態の滞在が 0 のままなら、妨げの候補（ASPM・PSR）を他の WS に渡す材料として記録する。

## 他の Phase から持ち越した 5330 の確かめ

- p002: MWAIT の路を通った事の直の観察（C-state の滞在で）。
- p011: 全 CPU の負荷で performance の P-core が 1.4 GHz に留まる理由（PLATFORM_LIMIT の印 0x8）、E-core の 4 つ組の clock の共有、0xE2 の lock の bit、MC6。
- p012: package の C-state（PC2〜PC7）が両方の tick の形で 0% の理由、全 CPU が同じ時刻に tick を処理する事の遅れ・lock の取り合い。
- p018: boost の (a)(b)（今は既定 n）。SMI の数の実機での数え。
- p021: passive と cpu の受け手の実機の結合（5330 では動く場面が無い見込み）。

## 既に取れた数（2026-11-01 q1788、試験だけの diag の patch、main 82c6ef325）

- p011 と p012 の phase.md の表（idle の package: periodic 10.08 W → TSC-deadline 6.44 W、Debian 3.75 W。HWP の strategy の request、HFI、meter 4 本、`cpuperf=off`）。受け入れの 1〜4 の全部ではない。

## 実行（2026-11-12 22:21〜23:02 P1、q2018-i01、branch agent/p1-ws333g）

### 道具（新しい build に書き直した）

- `plan/ws333/tests/hw5330/kbuild.sh OUT "VAR=VALUE ..."`: `PLATFORM=x86_64 CONFIGMK=config/config.x86_64.mk CONFIG_KERN_THERMAL=y CONFIG_DRIVER_X86_POWER_CONTROL=y` に `CONFIG_CPPFLAGS=…` で macro を足して `build/x86_64/vmunix`（make -j4）。4 つの kernel（`build/tmp/p014k/`、git の外）: hlt（`-DIDLE_WAIT_DEEPEST_CSTATE=1U`、sha256 896fc3cf2706a3e1）・c6（既定、a9236b5db6bf6e7a）・c10（`=7U`、906b9788965556b6）・periodic（`-DLAPIC_TICK_TSC_DEADLINE=0`、c9b94d4a47b3e946）、全部 rc=0 warning 0。
- `plan/ws333/tests/hw5330/thermal-static.sh OUT`: NVMe の zedBSD の userland（2026-11-02 の image）は新しい共有の libc より古く、新しい動的な `/sbin/thermal` は `ld.so: cannot open dependency` で走らない。sysroot の smoke の link と同じ形で静的に link し、static の start が stdio を flush しないので flush する main で包む。
- `plan/ws333/tests/hw5330/5330-p014.sh KDIR NAME [cpu]`: 1 本の script（配置 → 起動 → 実行 → 回収 → 要約、`/tmp/i915-hw.lock`）。p4 の元の kernel（cksum 1363937226）は Debian の `~/ws245/vmunix-p014-orig` に一度だけ残す（`5330-nvme-write.sh kernel` で戻せる）。今の p4 は c6 の kernel。
- 古い `thermal-diag.patch`・`td-summary.py`・`5330-thermal.sh` の p011・p012 は使わなかった（`/dev/thermal` で読めるため）。
- 注: `uname -v` は NVMe の userland の古い文字（`1.0.0-beta2+gd00e071`）を出すが、p4 の vmunix の cksum が入れた kernel と一致する事を確かめた（hlt: 1088212578）。

### 1. idle の電力（`build/hw5330/ws333/run2/`、git の外。desktop の session、各 kernel で起動の 120 秒後に 60 秒の窓）

| kernel | package（RAPL の meter） | package の PC2〜PC7 | cpu0 | cpu11（E-core） |
| --- | --- | --- | --- | --- |
| hlt | 7.701 W | 全部 0.0% | C-state 0% | 0% |
| periodic（tick の位相なし、C6） | 10.514 W | 0.0% | 0% | C6 91.4% |
| c10 | 6.741 W | 0.0% | C7 の欄 81.3% | C6 86.4% |
| c6（既定、strategy firmware） | 6.386 W | 0.0% | C6 90.1% | C6 87.1% |
| c6 ＋ balanced | 5.508 W | 0.0% | C6 86.6% | C6 85.9% |
| Debian（同じ日、intel-rapl:0、60 秒） | 3.344 W | （turbostat が無く未取得） | cpuidle: C10 が 60 秒の約 99.9%（59.9 s） | — |

- PASS の柱「MWAIT の idle の package の電力が HLT より低い」: c6 6.386 W < hlt 7.701 W（満たす）。
- package の深い状態の滞在は全部の kernel で 0%。Debian は cpu0 が C10 に約 100% いて 3.3 W。zedBSD の差（約 2.2〜3 W）は package の C-state に入れない事と見られる。妨げの候補（ASPM・PSR・i915 の DC state・tick）は他の WS に渡す材料（未調査）。
- periodic は C6 に入っても 10.5 W（cpu0 の C-state が 0%、tick の位相を揃えない形では cpu0 が深く眠れない）。p012 の 10.08 W → 6.44 W と同じ傾向。

### 3. HWP の strategy と直の設定（`build/hw5330/ws333/cpu3/`、1 process の spin、`thermal cpu` を 5 秒ごとに 4 回）

| strategy | 要求（P の domain・E の domain の min-max、EPP） | spin の間の最高の MHz |
| --- | --- | --- |
| performance | P 21-56・E 12-33、EPP 0 | 4343（highest_mhz 4341） |
| balanced | 1-56・1-33、EPP 128 | 3600 |
| powersave | 1-21・1-12、EPP 192 | 3280（cpu5、E-core。P-core は最高 1718） |
| manual 100 50 50 | 1-38・1-22、EPP 0 | 1192 |
| manual 0 50 50 | 1-11・1-8、EPP 255 | 2191（cpu11、E-core） |

- 要求の写し（min・max・EPP）は strategy ごとに設計どおりに変わる。performance は highest まで出る。
- 食い違い（未解析）: (a) 4 回の読みの最高が毎回同じ値で、読みが古い写しの見込み（`delivered_mhz` は GET の時にその CPU で読む設計）。(b) powersave で E-core に max 12 を超える 3280 MHz、manual 0 で E-core に 2191 MHz（E-core の highest_mhz が 0 で、MHz の換算の基準が P-core と違う見込み）。(c) manual 100 の spin が 1192 MHz（要求の max 38 なら 3800 まで）。spin の process がどの CPU にいたかを固定できない（zedBSD に CPU の固定の道具が無い）。
- 直の設定: run2 で `cpu set all max=28`（levels 1..56 の中ほど）は `cpu set: in force until the next strategy`、spin の最高 796 MHz（2800 を下回るが、(a)(c) と同じく読みの信頼が低い）。cpu3 では起動の直後の domain の levels が `4294967295`（strategy を一度も渡す前は未知）で中ほどを計算できず、直の設定の数は取れなかった（script は levels を strategy の後に読む直しが要る）。
- 全 CPU の負荷（performance）: P-core 916〜1652 MHz、E-core 560〜802 MHz（電力の上限の見込み、p011 の PLATFORM_LIMIT の件）。
- E-core の 4 つ組: `thermal cpu domains` で E-core は 1 CPU ずつ別の domain（domain 2〜9、各 levels 1..33）。設計の「4 つ組の共有」の形で domain に出ていない。負荷の時の E-core の MHz も 560〜802 とそろっていない。

### 4. `cpuperf.hwp=off`・相対の性能

- `cpuperf.hwp=off`（zedbsd.cfg に一度だけ足し、後で戻した）: `cpu-x86: HWP turned off by cpuperf.hwp=off; the CPUs are described, not controlled`、`HWP off, feedback off`。`thermal cpu` は highest_mhz=0・mhz=0、topology（P/E、domain）は出る。
- 相対の性能: 既定の source は `feedback`（HFI）、`frequency` と `feedback` が使える。performance の時に P 100・E 59、負荷の時に P 83・E 100（HFI の値が負荷で変わる）。

### 未実施

- 2. i915 の qs40 × 600（WS291 p022 の mview と model を NVMe の zedBSD に置く image が要る。今の NVMe の image に無い）。
- S0 idle の回帰と S0 idle の後の要求（`sleepctl s0idle` は起こす wake の事象が要り、5330 に遠隔の電源が無いため、戻らない時に Q1 も入れ直せない）。
- Debian の package の C-state（turbostat が無い）。§4.3.3 段 4 の見積もりの build。
- p002・p011・p012 の持ち越しのうち、MWAIT の路の直の観察は c6・c10 の core の C-state の滞在で取れた（C6 約 87〜90%、c10 で cpu0 の C7 の欄 81%）。p012 の package の C-state 0% の理由・p011 の PLATFORM_LIMIT は未解析。

### cleared の材料（Q1 へ）

- 取れた: 1 の 5 つの kernel・strategy と Debian の数、MWAIT < HLT の柱、3 の要求の写し・performance の最高、4 の hwp=off と相対の性能。
- 取れていない・食い違い: 2、S0 idle、3 の MHz が要求に従う事の確かな数（読みの古さ・E-core の換算の見込み）、E-core の 4 つ組。uncleared か、残りを別の Phase にするかは Q1 の判断。
