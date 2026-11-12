<!-- awesome-plan project=zedbsd record=ws333-p001 -->

# ws333-p001: /dev/thermal で統一する電力・熱の設計

Status: in-progress（2026-11-12 P1: 第 3 版で main の実装に合わせた。cleared は Q1 の判定、材料は下の「判定の材料」）
Parent: [WS333](../ws.md)
Queue: q1734（P5）、q2012-i01（P1、仕上げ）

## 範囲

ws.md の p001 の行のとおり。WS301・WS303 の design.md・user-questions.md・review を入力にし、CPU の idle（C-state）・P-state（HWP と driver の P-state）・P/E core の相対性能・熱の zone・fan・strategy・非常の停止を 1 つの /dev/thermal の UAPI と kernel の枠組みにまとめる。p002 の amd64 の idle（MWAIT）は先に実装するので、その形と矛盾しない設計にする。ユーザーの判断が要る点は、背景と案と用語の説明を付けて一覧にする。design-reviewer を通す。

## 進み（2026-10-31 P5）

- 設計の第 1 版（500061ba1）→ design-reviewer（[../design-review.md](../design-review.md)、blocker 0・major 8・minor 24）→ 第 2 版 [../design.md](../design.md)（反映の表は §14、p002 の main f740839e5 の結果を入れた）。問いは [../user-questions.md](../user-questions.md)（Q333-1〜7）、Q1 の判断は design.md §11.2（10 点）。
- p002 に求める条件 1〜7 は Q1 経由で P1 に送った（design.md §4.3.1）。

## 後の Phase への持ち越し（phase.md を立てる時に写す）

- p017・p018: WS303 design §2.1 の「EC は ECM1 の下で読む」は method 0x20 には当たらない（0x20 以外は）と読む（WS303 v5 m5）。
- p018: WS303 design §2.3 の「ACPI の thermal zone」は Linux の thermal_zone（DPTF の participant）と読む（v5 m24）。5330 の熱の試験は合否の手順と記録だけの手順を分ける（v5 m18）。試験の前に Debian で mode の set → 再起動 → 読みで mode が残る事と profile の daemon が無い事を確かめる（v5 M8）。
- p012: 今の形に戻す build の macro、lock の取り合いの確かめ、`lapic.c` の開発の管理情報の comment（あれば）を消す。
- p005・p006・p016・p022: 手を入れる file の開発の管理情報の comment を消す（design.md §4.16）。
- p003: `kern_cpu_count()` を kernel に足す（HAL は変えない）。

## 仕上げ（2026-11-12 P1、q2012-i01、基点 main 3a073a7e62）

### build の確かめ（新しい build system、`mk/`）

使い捨ての config（`build/tmp/ws333/config-<機種>.mk` に `PLATFORM := <機種>` だけ）で、kernel の target を名指して build した。x86_64・intelmid64・vf2 は `BUILD=build/ws333/<名前>`、rpi4・rpi5 は include が `-I$(BUILD)/sysroot` なので既定の `build/<機種>`。sysroot は header だけ（`make CONFIGMK=… $PWD/build/<機種>/sysroot/.headers-installed`）。log は worktree の `build/tmp/ws333/`（git の外）。

| build | コマンドの要点 | 結果 |
| --- | --- | --- |
| x86_64 既定 | `make -j16 CONFIGMK=build/tmp/ws333/config-x86_64.mk BUILD=build/ws333/x86_64-def build/ws333/x86_64-def/vmunix` | rc 0、`warning:` 0。flag に `-DCONFIG_KERN_CPU_PERF=1 -DCONFIG_CPU_PERF=1 -DCONFIG_DELL_SMM_BOOST=1`（defaults.mk の `CONFIG_DRIVER_CPU_PERF := y`・`CONFIG_DRIVER_DELL_SMM_BOOST := y`） |
| x86_64 `CONFIG_KERN_CPU_PERF=y` | 同じに変数を足す | rc 0、warning 0（既定と同じ flag） |
| x86_64 `CONFIG_DRIVER_DELL_SMM_BOOST=n` | 同じ | rc 0、warning 0 |
| x86_64 tick を periodic に | `CONFIG_CPPFLAGS=-DLAPIC_TICK_TSC_DEADLINE=0`（`KERN_CPPFLAGS=` を command line に置くと全部の flag が消えて失敗する） | rc 0、warning 0 |
| x86_64 `CONFIG_DRIVER_CPU_PERF=n CONFIG_KERN_CPU_PERF=n`（旧 main の既定 n に当たる形） | 同じ | **rc 2**: `cpu-x86-control.c` が `drv_cpu_x86_rdmsr`・`wrmsr` の宣言なしで error（21 件）。cpuperf.c も外れる（design.md §16.2 の 1・2） |
| x86_64 `CONFIG_DRIVER_X86_POWER_CONTROL=y` | 同じ | **rc 2**: MSR の 4 file が二重で `duplicate symbol`（§16.2 の 2） |
| intelmid64・vf2 既定 | 同じ形 | rc 0、warning 0 |
| rpi4・rpi5 既定 | `make -j16 CONFIGMK=build/tmp/ws333/config-rpi5.mk build/rpi5/vmunix` | rc 0、warning 0（rpi5 は `-DCONFIG_DRIVER_RPI5_FAN=1`） |
| i386・pc9821 | — | **止めた**。i386 の vmunix の target が compiler-rt の builtins を通して `build/llvm-source/.patched` に依存し、worktree に無い LLVM の tarball を取得して共有の `build/llvm-source` に展開し直した（中身は `patch -R --dry-run` が 14 file とも clean で前と同じ、mtime が変わった。Q1 に報告済み）。Q1 の指示で i386・pc9821 の build はしない |

source と mk の照合: WS333 の C の file（`src/kern/thermal.c`・`thermal-guard.c`・`cpuperf.c`、`src/drivers/platform/pcat/cpu-x86/*.c`、`src/drivers/acpi/*.c`、`src/drivers/platform/rpi5/rpi5-fan.c`）は全部が `mk/kernel.mk` か機種の vmunix.mk に名前がある（`src/drivers/acpi/*.c` 32 file も。旧 build は wildcard）。source が読む config の macro（`CONFIG_CPU_PERF`・`CONFIG_KERN_CPU_PERF`・`CONFIG_DELL_SMM_BOOST`・`CONFIG_DRIVER_RPI5_FAN`・`CONFIG_DRIVER_ACPI`・`LAPIC_TICK_TSC_DEADLINE`・`IDLE_WAIT_DEEPEST_CSTATE`）は全部 mk が定義するか source に既定がある。食い違いは design.md §16.2 の 1〜6。

### mk の差分の案（当てていない、ユーザーの確認が要る）

- 案の差分: [../proposed/mk-cpuperf-config.diff](../proposed/mk-cpuperf-config.diff)（11 file、`git apply --check` が通る）。要点は design.md §16.2 の末尾。
- 確かめ: 案を当てた mk の写し（scratchpad の木、source は worktree への symlink）で、x86_64 既定・`CONFIG_DRIVER_X86_POWER_CONTROL=y`・`CONFIG_DRIVER_DELL_SMM_BOOST=y`、intelmid64・vf2・rpi5 の既定が rc 0・warning 0。x86_64 既定の vmunix は `kern_cpuperf_init` を持ち `drv_cpu_x86_control_probe`・`drv_cpu_x86_rdmsr` を持たない、POWER_CONTROL=y は両方を持つ（llvm-nm）。rpi5 の `kernel.elf` に `kern_cpuperf_init`。
- この案を採ると、x86_64 の既定の kernel の MSR の道と Dell の boost が無効に戻る（旧 main と Q333-7 A・p018 の決定の形）。今の mk の既定の y がユーザーの意図なら、案の defaults.mk の部分を外し、1・2（常の cpuperf.c と 4 file の二重）だけを当てる。

### host 試験（2026-11-12、log は `build/tmp/ws333/host/`）

| 試験 | 結果 |
| --- | --- |
| `sh plan/ws333/tests/cpux86/run-unit.sh all` | PASS（test-builders 21・cpuid 3590873・cpux86 4140・hwp 12804888・power 275202・pstate 120186 checks） |
| `sh plan/ws333/tests/cpux86/check-skeleton.sh` | PASS |
| `sh plan/ws333/tests/cpuperf-host/run.sh build/tmp/ws333/cpuperf-host` | PASS（196 checks、0 failures） |
| `sh plan/ws333/tests/cpu-x86-host/run.sh build/tmp/ws333/cpu-x86-host2` | 1 回目 FAIL → 試験を直して PASS（y 13 場面・n 3 場面、計 831 checks、0 failures）。原因: build の作り直し（8016438f9b）で `cpu-x86-insn.h` の guard が `CONFIG_KERN_CPU_PERF` に変わり、試験は `CONFIG_CPU_PERF` だけを定義していた。source は変えず、試験が mk と同じく 2 つの macro を定義する形にした（6443adad84） |
| `sh plan/ws333/tests/thermal-host/run.sh build/tmp/ws333/thermal-host` | 22 scenarios、0 failed |
| `sh plan/ws333/tests/sessiond-thermal/run.sh build/tmp/ws333/sessiond` | PASS（113 checks、plain と ASan/UBSan） |
| `sh plan/ws333/tests/wmi/run.sh` | PASS（9 項目と unit 79 checks） |
| `sh plan/ws333/tests/dell/run.sh` | PASS（28 項目と unit 49 checks） |

### 設計の照合と Phase の登録

- design.md を第 3 版にした: §16（実装との照合 16.1、build と設計の食い違い 16.2、試験の道具の古さ 16.3）を足し、本文の 4 か所（図の Dell の file、config の名前、HAL の節の driver の名前、p015 の config の置き場所）を直した。設計の意味は変えていないので design-reviewer は回していない。
- design.md §9 にあって ws.md に無かった Phase を立てた: p004（UAPI、この Queue で案まで）・p013・p014・p015・p020・p022・p024。p023 は §9 のとおり条件つき（p014 の数で要ると分かった時だけ）なので立てていない。

### 判定の材料（cleared にできるか、Q1）

- 受け入れ（§9 の p001 の行）: 「review の反映、問い」。第 1 版の review（blocker 0・major 8・minor 24）は第 2 版で全部反映（§14）、問い Q333-1〜7 は 2026-11-01 にユーザーが答えた。第 3 版で実装との差を §16 に整理し、§9 の Phase は全部 ws.md に載った（p023 を除く）。
- 残っている判断（p001 の外の Phase に移せる物）: §11.2 の Q1 の 6（`thermal.emergency=off`、作っていない）・8（WS302 の依存の付け替え）・9（p014 の数の目標）と、§16.2 の mk の案（ユーザー）。どれも設計の形を変えず、6 は p004 の GET_STATUS の欄、9 は p014、mk は p015 の前提に移した。
- P1 の見立て: p001 は cleared にしてよい。§16.2 の mk の食い違い（特に既定の MSR の道と Dell の boost の y）は設計の問題でなく build の設定の問題なので、p001 の判定と分けて Q1・ユーザーが決める。
