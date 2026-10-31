<!-- awesome-plan project=zedbsd record=ws049p008 -->

# ws049-p008: 対象機（Latitude 5330）の table と実機の確認（BUG-165）

Phase ID: `ws049-p008`
Parent: [WS049](../ws.md)
Status: cleared（2026-10-13 ユーザー「UATの結果待ちのものは、すべてテスト済みなのでclearedにして進めたりcloseしたりしてください。」。旧: in-progress、実装と host 試験は済み、5330 の実機の確認（BUG-165）の UAT 待ち。ws.md の表は 2026-10-13 ユーザーの実機の確かめ「OKでした」で cleared）
Phase disposition: normal
Queue: q677 / q677-i01（P1 generation15）。承認: 2026-10-04 user「次の新規実装項目は、USB-C の DisplayPort Alternate Modeの実現を目標にします。…共通のpredecessorがAMLですね。…実行は17時以降に行います。」と 17 時の体制の指示
Bug: [BUG-165](../../bugs/BUG-165.md)（共通の根の候補: [BUG-119](../../bugs/BUG-119.md)・[BUG-156](../../bugs/BUG-156.md)・[BUG-167](../../bugs/BUG-167.md)・[BUG-159](../../bugs/BUG-159.md)）

## 目的と範囲

2026-10-04 の UAT で、素の 5330 の起動で DSDT の読み込みが `ACPI: DSDT Dell Inc stopped at offset 0x1c89b (error 13)` で止まり、DSDT の定義
（`\_S5`・I2C HID・電池・AC・lid）が一つも登録されなかった。これを直し、5330 の DSDT・SSDT が最後まで読み込まれるようにする。

- 無い PCI 機能の PCI_Config の region は、読みは全ビット 1、書きは無視（`src/drivers/acpi/acpi-kern.c`）。
- 5330 の DSDT・SSDT を取り出して host の試験に入れ、読み込みが最後まで進むまで直す（他の error で止まればそれも直す）。
- 実機の確認（DSDT の読み込み、Shut Down の電源 OFF、タッチパッド、電池）はユーザーの UAT。そのための image の作り方と観点をここに書く。

範囲外: SCI・GPE・電源ボタン・EC の kernel での確認（p007）、規約の全文の確認（p009）。

## 受け入れ

1. host: 5330 の DSDT と SSDT 15 個が、0:10.6・0:10.7 を無い機能として、`stopped at` 無しに全部読み込まれる。古い挙動（ENODEV）では
   UAT と同じ offset 0x1c89b で止まる（再現の確認）。`_REG`・`_INI`、`\_S5_`、電池の `_BIF`・`_BST`、AC の `_PSR`、lid の `_LID` が評価でき、
   引数の無い全 method が走る。
2. kernel: amd64 の kernel が warning 0 で build できる。
3. 実機（ユーザーの UAT）: dmesg に `stopped at`・`did not load` が無い。Shut Down で電源が切れる（BUG-119）。タッチパッド（BUG-156・167）と
   電池・AC（BUG-159）を確かめ直す。

## 原因

- error 13 は zedBSD の errno で ENODEV。5330 の DSDT は最上位（module level）で `\_SB.PC00.THC0`（`_ADR 0x00100006`、Touch Host
  Controller、BIOS で無効）の vendor ID を `If ((VDID != 0xFFFFFFFF))` と読んで、その device の method を定義するか決める
  （`iasl -d` の 26527 行）。無い機能の config space が全ビット 1 を返すことを前提にしている。
- kernel の PCI_Config の handler（`pci_handler`）は列挙に無い機能に ENODEV を返し、interpreter は table の実行の error で table 全体を
  打ち切る（`aml-table.c` の `drv_acpi_table_load_bytes`）。
- 実在の PCI と ACPICA（Linux）は、無い機能の読みに全ビット 1 を返し、書きを捨てる。

## 設計と変更

| file | 内容 |
| --- | --- |
| `src/drivers/acpi/acpi-kern.c` | `pci_handler`: 列挙に無い機能は `pci_absent()` で答えて成功する。読みは幅の全ビット 1（64 bit は `UINT64_MAX`）、書きは捨てる。log は機能ごとに 1 回（同じ機能が続く間は出さない。`pci_absent_logged`、interpreter の lock の下）: `acpi: PCI_Config region of absent function B:D.F reads as all ones` |
| `plan/ws049/tests/aml-host.c` | `--absent-pci B:D.F`（繰り返し可、kernel と同じ答え）、`--absent-pci-fails`（古い kernel と同じ ENODEV、再現用）、`--ec-ports D,C`（模擬の EC の port を table の `_CRS` の位置へ。5330 は 0x930/0x934） |
| `plan/ws049/tests/aml-host-hardware.[ch]` | `hardware_ec_ports()`（模擬の EC の data・command の port。既定は 0x62/0x66 のまま） |
| `plan/ws049/tests/asl/absentpci.*` | 新しい ASL の試験（harness だけ。acpiexec は全機能を 0 の memory として模擬するので走らせない）: 無い機能の vendor ID が全ビット 1、書きが捨てられる、最上位の `If` が device の定義を飛ばす、64 bit の読み、ある機能は書いた値を保つ、table が最後まで走る |
| `plan/tools/aml-host/latitude5330/`（2026-10-31 に移した） | 5330 の DSDT・SSDT1〜15（BIOS 1.31.1、2026-10-04 に取り出し）と [README](../../tools/aml-host/latitude5330/README.md)（出所・sha256・license の注意） |
| `plan/ws049/tests/check-latitude5330.sh` | 5330 の table の host の試験（受け入れ 1 の 4 項目） |

## 検証（2026-10-04、host と kernel の build。QEMU・実機は未実施）

| command | 結果 |
| --- | --- |
| `make -C plan/ws049/tests`（ASan・UBSan の harness） | warning 0 |
| `plan/ws049/tests/check-latitude5330.sh` | **PASS**: old（`--absent-pci-fails`）は `DSDT Dell Inc stopped at offset 0x1c89b` で UAT と同じ所で止まる / load: 全 table が読み込まれる / power: `_REG`・`_INI`、`\_S5_ = Package { 0x7, 0, 0, 0 }`（SLP_TYP 7）、`BAT0._BIF`・`_BST`、`AC._PSR`、`LID0._LID` が評価できる / methods: 1349 個が走り、失敗は既知の `\_SB_.PTID.TSDD`（ENOENT）だけ |
| 0:10.6・0:10.7 だけでなく、bus 0 の `_ADR` を持つ 42 機能を全て無い機能として読み込み・`--reg --init --devices` | 失敗 0（全部が無い極端の場合も止まらない） |
| `plan/ws049/tests/compare-firmware.sh latitude5330 …`（RSDP・XSDT を経る kernel と同じ table の発見の道） | same（7984 nodes） |
| `python3 plan/ws049/tests/run-asl.py` | 17 passed, 0 failed（新しい absentpci を含む）。absentpci は `--absent-pci-fails` では offset 0x96 で止まり、`--absent-pci` 無しでは MAIN が 1 を返す（試験が新しい挙動を区別することの確認） |
| `make -C plan/ws049/tests kernel-check`（kernel と同じ target・flag） | warning 0 |
| `make ZEDBSD_CONFIG=config/ci/config-amd64.mk vmunix -j8`（worktree の `build/amd64`） | warning 0、`kernel include check: PASS (307 objects, 8146 dependencies)`、`amd64 vmunix check: PASS`、vmunix に新しい log の文字列がある |
| `python3 plan/tools/style-check.py`（変えた C の 4 file）・`git diff --check` | 0 findings |

host の模擬の memory は 0 を返すので、評価の値は実機の値ではない。確かめたのは interpreter が止まらずに通ることだけ。

## 実機の確認（ユーザーの UAT）

### image の作り方

前回の UAT（[uat.md](../../history/uat-20261004.md)）と同じ作り方で、この Phase の commit を含む main から作る:

```
plan/tools/guest/test-image.sh --no-harness plan/uat/config-uat.mk build/uat-3 \
    （build-demo-image.sh と同じ --file: apps.conf・壁紙・authorized_keys）
I915_TEST_VBT=n
```

kernel の差分はこの Phase の `acpi-kern.c` だけ（config は変えない）。

### 確かめる観点

| # | 観点 | 合格 |
| --- | --- | --- |
| 1 | dmesg の ACPI の行 | `stopped at`・`the DSDT did not load` が無い。`acpi: PCI_Config region of absent function 0:10.6 reads as all ones`（と 0:10.7）が出る。`/dev/acpi` の namespace に `\_S5_`・`\_SB_.BAT0`・`\_SB_.AC__`・`\_SB_.LID0`・`\_SB_.PC00.I2C1.TPD0` がある |
| 2 | Shut Down（greeter と desktop の両方） | 電源が切れる（BUG-119） |
| 3 | タッチパッド | 2 本指のスクロール・スワイプ・押し込み（BUG-156・167）。I2C HID の device が ACPI で見つかるか |
| 4 | 電池・AC | `sysctl` に battery・AC の項目が出るか、AC の抜き差し（BUG-159、ws134-p009） |
| 5 | i915 の opregion | `ACPI_RUNTIME_UNAVAILABLE` が消えるか |

2〜5 は DSDT の読み込みの先の機能（p007 の SCI・EC、各 driver）にも依る。DSDT が読めても動かないものは、その driver の Bug・Phase として切り分ける。

## 発見（範囲外、記録のみ）

- **橋の下の機能の PCI_Config の region の bus**: `aml-field.c` の `region_resolve_pci()` は bus を `_BBN`（root の bus）だけで決め、
  親の PCI-PCI bridge の secondary bus を辿らない（ACPICA の `AcpiHwDerivePciId` は辿る）。5330 では `\_SB_.PC00.RPxx.PXSX.PCCX`・
  `PEGx.PEGP.PCCX`（31 個、読みだけ。`PRES`・`PNVM`・`PAHC`・`ISGX` が class code を見る）が bus 0・device 0・function 0（host bridge）を
  読んでしまう。加えて `drv_pci_find_device()`（`src/drivers/pci/pci.c`、他の WS の source）は root の bus の機能しか探さないので、bus を
  正しくしても橋の下の機能は「無い」扱いになる（`drv_pci_foreach_device()` は木を辿る）。書きの field は無く、table の読み込みは止めない。
  RTD3（WS052）の判断に効くので、WS049 の別の Phase（または p009 の前）で直す案を Q1 に送った → [ws049-p016](../phase016/phase.md)（q694）で直した。
- `\_SB_.PTID.TSDD` は `\_TZ.TZ00._TMP` を読むが、table に `\_TZ.TZ00` が無い（firmware の不備。ACPICA でも AE_NOT_FOUND）。
- SSDT の `xh_Dell_` が `\_SB_.PC00.XHCI.RHUB.HS01`〜`HS08` の scope を開くが、DSDT の `HS01` などは GNVS（`PU2C`）の値で作られるので、
  模擬の memory（0）では無い。実機では作られる見込み（host の模擬の限界）。
- EC が timeout で失敗した method の後に `ACPI: a mutex was still held when the evaluation ended` が出る（EC の port を合わせる前の試行で観測）。
  実機の EC が応えれば出ない見込み。p007 で EC を確かめる時に見る。

## 残り

- 実機の確認（上の表）。結果で cleared か、別の Bug・Phase への切り分け。
- QEMU の回帰（boot test と guest の `/dev/acpi` が p006 と同じ）は p007 の QEMU の試験と一緒に Q1 経由で T1 へ依頼する。

## 引き継ぎ（2026-10-04、P1 → P3。Q1 の指示「Bug の修正は P3 に移します」）

- 済んだこと: 原因の確認（5330 の table で）、`acpi-kern.c` の修正、harness の `--absent-pci`・`--absent-pci-fails`・`--ec-ports`、ASL の試験
  `absentpci`、`check-latitude5330.sh`、kernel の build（warning 0）。上の「検証」の表。
- 取り出した table: `plan/ws049/tests/latitude5330/`（`dsdt.dat`、`ssdt1.dat`〜`ssdt15.dat`、README に sha256）。disassemble は `iasl -d`。
- 残り: (1) 実機の UAT（上の「実機の確認」の image と観点）。結果で cleared か切り分け。(2) QEMU の回帰（boot test と guest の `/dev/acpi` の
  namespace が p006 と同じ）を Q1 経由で T1 に依頼（p007 の QEMU 試験とまとめてよい）。(3) 「発見」の橋の下の PCI の bus（Q1 の判断待ち）。
- 再開点: 実機の UAT の結果を受け取ったところ。host の試験の再実行は `make -C plan/ws049/tests && plan/ws049/tests/check-latitude5330.sh`。
