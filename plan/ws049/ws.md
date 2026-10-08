<!-- awesome-plan project=zedbsd record=ws049 -->

# WS049: kernel 内の ACPI AML interpreter

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-08 q910 P2 の照合: p007・p008・p016 は実機（電源ボタン・蓋・AC・EC）待ち、p017 は T1-093 PASS で Q1 の判定と ⑤ のユーザーの判断待ち。T1-086 は T1-090 で置き換わった（未実施のまま））
Primary Milestone: MG003
Related Milestones: MG006, MG008
Objectives: O2, O4
Parent: [Master](../master.md)
Queue: q677（p008、実機待ち）・q678（p007、QEMU PASS・実機待ち）・q694（p016、実機待ち）・q693（p009、cleared）
Resume point: 2026-10-04 p008（BUG-165、実機の UAT 待ち、P3）、p007（SCI の unmask と event thread の start を直し T1-090 で QEMU PASS、実機の電源ボタン・蓋・AC・EC 待ち）、p016（実機待ち）、p009（cleared、T1-091）。次は design 050 §12 の WS049 の口の Phase（Q1 が Queue にする）
<!-- awesome-plan-current:end -->

## 目標

kernel の中に ACPI の AML interpreter を持ち、DSDT・SSDT を読み込んで namespace を作り、driver が AML の method（`_STA`・`_CRS`・`_PS0`・`_PS3`・
`_DSM`・`_Lxx`/`_Exx` ほか）を評価できるようにする。UCSI（WS050）、USB-C の DisplayPort Alternate Mode（WS051）、S0i3 の電源管理（WS052）の土台。

## きっかけ

2026-09-24 ユーザー指示: 「下記をそれぞれWSとして追加してほしいです。カーネル内ACPI AMLインタプリタの実装。…」

## 今あるもの

- ACPI の table の解析は HAL の中にだけある（`src/hal/amd64/bsp-pcat/acpi.c`、`src/hal/i386/acpi.c`: RSDP（loader が渡すものか firmware の走査）・XSDT・MADT・MCFG）。
- HAL に ACPI の table の専用の口は無いが、**機種依存の情報を名前で問い合わせる `hal_get_arch_handoff(name)`**（`include/hal/hal.h`）がある
  （2026-09-24 ユーザーの指摘）。今の名前は `boot.command-line`・`boot.selector`・`pcat.boot-font`・`pcat.framebuffer`（`src/hal/amd64/bsp-pcat/boot.c`）。
  kernel は、ここに足す名前（例: RSDP の物理番地か、検証済みの table の一覧）で ACPI の table を受け取り、`hal_space_map_device()` で読める。
  `hal.h` は変えずに済むが、`src/hal` の既存の宣言の実装に名前を足すので、**差分ごとの承認が要る**（p001 が案を作る）。
- SCI の割り込みは `hal_irq_register()`、PM1・GPE の register は `hal_io_inp*`・`hal_io_outp*`（port I/O）と `hal_mmio_*` で扱える見込み（p001 で確かめる）。
- 対象機は Dell Latitude 5330（Alder Lake-P、WS029・WS031 と同じ）を想定（仮定。ユーザーの確認が要る）。QEMU の q35 の DSDT は小さく、単体の試験に使える。

## 範囲

- AML の byte code の解析と namespace（Scope・Device・Method・Name・OperationRegion・Field・Mutex・Event・Alias、外部参照）。
- 評価器: 整数・文字列・buffer・package、制御（If・While・Return）、演算、`Notify`、`Sleep`・`Stall`、`Acquire`・`Release`。
- OperationRegion の access: SystemMemory、SystemIO、PCI_Config、EmbeddedControl（EC の driver が要る）、GenericSerialBus は要否を調べる。
- SCI と GPE の割り込み、`_Lxx`・`_Exx` の実行、`Notify` を driver へ。
- `_OSI` の答え方（Windows の版を名乗るかどうか。Alder Lake の機種は多くの機能を Windows の版で切り替える）。

範囲外（別 WS）: UCSI（WS050）、DP Alt Mode（WS051）、電源管理（WS052）。

## 受け入れ

- QEMU（q35）と対象機の DSDT・SSDT を読み込んで namespace を作り、全 method を評価せずに列挙でき、`_STA`・`_CRS`・`_HID` を評価できる。
- 対象機で EC を介する `_Qxx`（例: 電源 button・lid）か GPE の event が driver に届く。
- AML の試験の集合（自作の ASL を iasl で作った AML と、実機の table の抜き出し。license に注意）で評価の結果を確かめる。
- 規約（`plan/coding-style.md`）の全文、build（warning 0）、boot test。

## Phase 一覧

設計: [design.md](design.md)（2026-09-27、p001）。p002〜p005、p010〜p015 は host だけで進められる（完了）。p006〜p008 は HAL の差分の承認（と対象機の table）が前提、p009 はその後。

| Phase | 内容 | Status | 依存 | 対象 |
| --- | --- | --- | --- | --- |
| [ws049-p001](phase001/phase.md) | 調査と設計: table の道、利用者の要求、構成、評価の方式、HAL の差分の案、試験の方法 | cleared（2026-09-27） | — | 設計文書 |
| [ws049-p002](phase002/phase.md) | object・namespace・byte 列・DefinitionBlock の読み込み、host の harness | cleared（2026-09-27） | p001 | `src/drivers/acpi/` |
| [ws049-p003](phase003/phase.md) | 評価器: method、制御、全ての式の opcode、参照、変換、Store の規則 | cleared（2026-09-27） | p002 | 同上 |
| [ws049-p004](phase004/phase.md) | OperationRegion・Field・IndexField・BankField・BufferField、region の handler と `_REG` | cleared（2026-09-27） | p003 | 同上 |
| [ws049-p005](phase005/phase.md) | 同期と OS の口: Mutex・Event・Sleep・Notify・`_OSI`・Load/LoadTable/Unload・`_INI`、stack の予算 | cleared（2026-09-27） | p004 | 同上 |
| [ws049-p006](phase006/phase.md) | kernel への組み込み（amd64）: kernel image への link（`CONFIG_DRIVER_ACPI`、vmunix.mk、`pcat.c` の `drv_acpi_attach()`）、起動時の読み込み、診断の口、QEMU（q35・OVMF）での確認 | cleared（2026-09-27。承認済みの `acpi.rsdp` の差分と統合、boot test PASS、guest の `/dev/acpi` の namespace 278 行が host と同じ、device の評価の違い 11 行は firmware の設定する hardware の状態だけ） | p010、HAL の差分の承認（済） | `src/drivers/acpi/`、platform |
| [ws049-p007](phase007/phase.md) | SCI・GPE・固定 event・EC の kernel での確認: SCI の割り込み、event thread、QEMU の `system_powerdown`（固定の電源 button）と GPE | in-progress（2026-10-04。SCI の line の unmask と event thread の `thread_start` の欠けを直し T1-090 で QEMU の受け入れ PASS。実機の UAT 待ち、[phase007](phase007/phase.md)） | p006、p011 | 同上 |
| [ws049-p008](phase008/phase.md) | 対象機（Latitude 5330）の table と実機の確認（BUG-165: DSDT が 0:10.6 の PCI_Config で止まる） | in-progress（2026-10-04。実装と host の試験は済み、実機の UAT 待ち） | 対象機の table（2026-10-04 取得）。p007 より先に実行（2026-10-04 Q1 の予定） | 同上 |
| [ws049-p009](phase009/phase.md) | 規約の全文の確認と最終の確認 | cleared（2026-10-04。[findings](phase009/findings.md) の全件を適用、host の確認、T1-091 の QEMU の回帰 PASS） | p002〜p008、p010、p011 | WS の全 source |
| [ws049-p016](phase016/phase.md) | 橋の下の機能の PCI_Config の region の bus（親の PCI-PCI bridge の secondary bus を辿る）と `drv_pci_find_device()` の bridge の先の bus | in-progress（2026-10-04。実装と host の試験は済み、QEMU の回帰と実機待ち） | p008 の発見、Q1 の判断（q694、p007 の後・p009 の前） | `src/drivers/acpi/`、`src/drivers/pci/pci.c` の最小 |
| [ws049-p015](phase015/phase.md) | ECDT: `_REG`・`_INI` の前の EC（ECDT の検査、早い address space、後の device の GPE と query、`_CRS` との食い違い） | cleared（2026-09-27。kernel の上の実行は p007 で） | p011 | `src/drivers/acpi/` |
| [ws049-p014](phase014/phase.md) | FACS の Global Lock の hardware の手順（firmware と取り合い、pending・GBL_RLS・GBL_STS）。host の疑似の firmware で試験 | cleared（2026-09-27。kernel の上の実行は p007 で） | p011 | `src/drivers/acpi/` |
| [ws049-p013](phase013/phase.md) | p006 のうち承認なしでできる部分: 診断の口 `/dev/acpi`（read で namespace、path を write して評価）と、kernel と harness で共有する出力の処理（`acpi-text.c`） | cleared（2026-09-27。kernel の上の実行は p006 で） | p010 | `src/drivers/acpi/` |
| [ws049-p012](phase012/phase.md) | 壊れた table への堅牢性: AML の byte を変えた table を sanitizer の下で読み込み、全 method を走らせる（fuzz） | cleared（2026-09-27） | p011 | 試験 |
| [ws049-p011](phase011/phase.md) | p007 のうち承認なしでできる部分: event の核（FADT、ACPI mode、PM1・GPE、`_Lxx`/`_Exx`、wake GPE、割り込みと thread の分担）と EC（`_CRS`・`_GPE`・`_GLK`、protocol、EmbeddedControl の region、`_Qxx`）。host の疑似の hardware で試験、kernel の側は compile | cleared（2026-09-27） | p010 | `src/drivers/acpi/` |
| [ws049-p010](phase010/phase.md) | p006 のうち承認なしでできる部分: firmware の table の発見（`acpi-tables.c`、host の疑似の物理 memory で試験）、kernel の glue（`acpi-kern.c`、kernel の flag で compile） | cleared（2026-09-27） | p005 | `src/drivers/acpi/` |

## 人間の判断が要る点

- `hal_get_arch_handoff("acpi.rsdp")` を足す差分の承認（[design.md](design.md) §9、差分 [proposed/hal-acpi-rsdp.diff](proposed/hal-acpi-rsdp.diff)。
  `hal.h` は変えない。未適用）。p006 以降の前提。
- 診断の口の形と device 番号（[phase013](phase013/phase.md): UAPI を足さない text の `/dev/acpi`、`0x000B0000` を実装済み。ioctl や
  `/dev/system` への統合にするなら直す）。
- 対象機（Latitude 5330 でよいか）と、その table の取り出し（Linux で `sudo acpidump -b`）。
- `_OSI` でどの Windows を名乗るか（design §8、案は `Windows 2022` まで真）。

## 2026-10-04 UAT の結果（Q1）

素の 5330 で DSDT が読み込めない（`ACPI: DSDT Dell Inc stopped at offset 0x1c89b (error 13)`、[BUG-165](../bugs/BUG-165.md)）。電源が切れない（BUG-119）・タッチパッド（BUG-156・167）・電池の情報（BUG-159、ws134-p009）の共通の根の候補。**次の Phase（planned、番号は着手の時に Q1 が振る）**: offset 0x1c89b の AML を特定し、error 13 の原因を直す。5330 の DSDT を取り出して host の試験に入れ、実機で `\_S5` による電源の切断を確かめる。最優先（17 時以降）。

## 2026-10-04 予定（Q1）

USB-C DP Alt Mode（WS050・WS051）・電源管理（WS052）・WS132 の共通の前提。q677（p008: BUG-165 の DSDT と 5330 の table）→ q678（p007: SCI・GPE・電源ボタン・EC）を最初に行う（[queue.md](../queue.md)）。
