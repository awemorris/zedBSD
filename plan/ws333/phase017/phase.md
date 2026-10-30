<!-- awesome-plan project=zedbsd record=ws333-p017 -->

# ws333-p017: ACPI WMI の層と AML の harness の移し

Status: cleared（2026-10-31 Q1、main 809dfe329）
Parent: [WS333](../ws.md)
Queue: q1771-i01（P6）

## 目的

design.md §9 の p017（WS303 design §4.1・§6.1 の WMI の部分と、WS333 design §4.7 の変更）: ACPI の WMI の対応の device（PNP0C14）の `_WDG` を読み、GUID から block を引き、method（WMxx）と query（WQxx）を driver に呼ばせる層。AML の host の harness を `plan/ws049/tests/` から `plan/tools/aml-host/` に移し、WMI と Dell の provider（p018）の試験に要る口を足す。受け入れ: WMI の host 試験、SMI を起こさない、amd64 の build（warning 0）。

## 判断

- 2026-10-31 ユーザーから、昼頃まで不在の間は実装と試験を進める、という趣旨の指示を受けた（Q1 経由）。
- 2026-10-31 Q1 の OK: harness の 94 file を `git mv` で `plan/tools/aml-host/` へ。QEMU の guest の道具（`qmp-send.py`・`guest-events.sh`・`guest-compare.sh`・`qemu-acpi-dump.py`・`config-acpi.mk`）は ws049 に残す。ws049・ws052 の参照の path の書き換えと `acpi-kern.c` の plan/ 参照の comment の削除も可。過去の phase.md・Bug の記述は直さない。
- `_WDG` の entry は 20 byte（GUID 16、識別子 2、instance の数 1、flags 1）。Microsoft の文書の struct は最後の 2 つを USHORT と書くが、同じ文書の `_WDG` の例と firmware（5330）は 1 byte ずつ。例と firmware に従い、comment に書いた。
- `_WDG` の entry は全部をそのまま載せる（WS303 の review M9 の「31 entry をそのまま」）。query の method（WQxx）の無い data block も載せ、`drv_acpi_wmi_query` が ENOENT を返す（zedBSD の規則。5330 の AMW0 の "BC" がこの形）。WQxx が Name（data）の時は引数なしで読む（5330 の WQMO・WQCC）。
- EXPENSIVE の data block の query は、WCxx があれば前に WCxx(1)、後に WCxx(0)（Microsoft の文書の WCxx の説明。5330 には EXPENSIVE の block は無い）。
- attach の時は `_WDG` が Name（buffer）の device だけを評価する。Method の `_WDG` は飛ばして log 1 行。名指しの `drv_acpi_wmi_attach_path(path)` は Method の `_WDG` も評価する（呼び手が「data を返すだけ」と知っている device 用、p018 の `\_SB.DIAG`）。
- attach は繰り返して呼べる（載った device は飛ばす）。WS333 design §4.7 の「attach の試し直し（1 秒おきに 3 回）を poll の thread で」は、この Phase では **繰り返せる attach の口まで**にした。理由: 試し直しが要るのは WMI を使う provider（p018 の Dell）が居る時だけで、今は使い手が無い。kernel の熱の core（`src/kern/thermal.c`）の poll の thread から driver の `drv_acpi_wmi_attach` を呼ぶと kern → drv の向きが逆になり、core に「poll の thread で 1 回だけ走らせる」口も無い。p018 の Dell の provider が自分の受け手の `poll`（1 秒ごと）で見つからない block の attach を 3 回まで呼び直す形を推す（p005 の phase.md の「残り」の WMI の試し直しはこの形で p018 へ）。
- 表は static（block 48、device 16）。attach は interpreter の lock（`drv_acpi_os_lock`）の下で追記し（その間 AML は走らせない）、block の数を release の store で公開する。読み手は acquire の load で数を読み、それより下の block を lock なしで読む（公開した block は変えない）。
- 事象（`_WED`）の口と user への生の WMI の口は作らない（WS303 §4.1）。WMI の層は `kern_*` と HAL を使わず、`drv_acpi_*` と kcrt と `aml-os.h` の log と lock だけ（host の harness でも同じ file が動く）。
- 出典（guardrail の「出典」）: block ごとの複数行の comment に「Value(s) referenced:」「Procedure referenced:」で、Microsoft「Windows Instrumentation: WMI and ACPI」（learn.microsoft.com、dn614028、archived、ms.date 2017-06-01）と「GUID structure (guiddef.h)」（learn.microsoft.com、ms.date 2022-08-12）。文書は 2026-10-31 に公開の site から WebFetch で取った（写しは scratchpad、git に入れない）。GPL の source は読んでいない。

## 作った物

- `include/drivers/acpi/wmi.h`（新）: `struct drv_acpi_wmi_entry`・`struct drv_acpi_wmi_block`、flag（EXPENSIVE 1・METHOD 2・STRING 4・EVENT 8）、`drv_acpi_wmi_guid_parse`・`drv_acpi_wmi_entries_parse`・`drv_acpi_wmi_control_name`（純な decode）、`drv_acpi_wmi_attach`・`drv_acpi_wmi_attach_path`・`drv_acpi_wmi_block_count`・`drv_acpi_wmi_block_at`・`drv_acpi_wmi_find`（GUID の文字列と n 番目）・`drv_acpi_wmi_method`（WMxx(instance, method id, buffer)、入力は写し、64 KiB まで）・`drv_acpi_wmi_query`（WQxx(instance)）。
- `src/drivers/acpi/acpi-wmi-table.c`（新、Microsoft の文書に基づく 1 module）: `_WDG` の entry の decode（20 の倍数でなければ EINVAL、入りきらなければ E2BIG と前の分）、GUID の文字列 → `_WDG` の byte の順（Data1〜3 は little-endian）、制御の method の名前（event と ACPI の名前に使えない文字は EINVAL）。
- `src/drivers/acpi/acpi-wmi.c`（新）: namespace を歩いて `_HID` が PNP0C14（文字列か EISA の 0x140CD041）で present な device を集め、`_WDG` を上の規則で読んで表に載せる。method・query は上の規則。
- `src/drivers/acpi/acpi-kern.c`: `drv_acpi_attach()` の熱の sensor の後・LPS0 の前に `drv_acpi_wmi_attach()`（EC の後）。手を入れた file なので、comment の開発の管理情報（WS・Phase・Bug の番号と plan/ の参照）11 か所を消した（guardrail「source の中の開発の管理情報」）。log の文字列は変えていない。
- harness の移し（`git mv`、94 file）: `plan/ws049/tests/` → `plan/tools/aml-host/`（`aml-host.c`・`aml-host-hardware.[ch]`・`Makefile`・`asl/`・`latitude5330/`・`run-asl.py`・`check-latitude5330.sh`・`fuzz.py`・`ecdt-check.py`・`make-firmware.py`・`compare-*.sh`・`compare-devices.py`・`acpiexec-namespace.py`）。build の出力は `build/aml-host/`。参照の path を書き換えた: 移した script と Makefile、`plan/ws049/tests/guest-compare.sh`、`plan/ws052/tests/Makefile`・`run-host-sleep.sh`。
- harness の拡張（WS303 第 2 版 R9）: `aml-host-wmi.[ch]`（新）と `aml-host.c` の option: `--wmi`（attach と block の一覧）、`--wmi-path PATH`、`--wmi-query GUID:I`、`--wmi-method GUID:I:M:HEX`、`--eval-args 'PATH i:… s:… b:…'`（引数つきの評価）、`--ec-seq A=V,V…`（EC の byte の読みが値の列を順に取る、`--ec` の時）、`--smi-memory A=HEX`（SMI の command port の書きのたびに system memory に byte を置く、firmware の答えの模擬）。`aml-host-hardware.c` が SMI の command port の書きを数え、各呼びが自分の起こした書きの数を `SMI writes N` で出す。`plan/ws052/tests/Makefile` に link に要る 3 file を足した。
- `plan/ws333/tests/wmi/`（新）: `test-wmi-table.c`（decode の単体試験、Microsoft の例の `_WDG` と 5330 の AMW0 の `_WDG`、79 check）と `run.sh`（単体試験と 5330 の表の harness の試験）。

## 確かめた事

- `sh plan/ws333/tests/wmi/run.sh`: 9 項目 PASS。
  - unit: 79 check（GUID の文字列 9 種の拒み、例と 5330 の GUID の byte、entry の decode・59/19/21 byte の拒み・E2BIG・空、名前）。
  - 5330 の DSDT・SSDT1〜15（`--events --ec --reg --init`）: Name の `_WDG` の 4 device（WFDE 3・WFTE 2・AMW0 7・AMWV 2、計 14 block）を載せ、Method の `_WDG` の 5 device（AMW2・AMW5・AMW4・DIAG・ECMO）は評価しない。`--wmi-path \_SB_.DIAG` で 2 block（DM・MO）を足して 16。`WQAA` の query が "DELL"・" WMI"・1・0x8000・1、DDV の method 0x12 が 3（WS303 §2.1 と合う）。DDV の method 0x20 が `--ec-seq` の EC の列で `{4, Buffer{00 39 0f ff}}`（WS303 §2.3 の Debian の観測と同じ 3897 rpm）。**attach・名指しの attach・query・DDV の 2 つの呼びの SMI の port の書きは全部 0**。
  - 拒み: method の block の query・instance の超過・data の block の method は EINVAL、WQBC の無い "BC" と知らない GUID は ENOENT、PNP0C14 でない path は ENODEV。
  - 対照（harness が SMI を数え答えられる事）: AMW2 の Method の `_WDG` を名指しで評価すると SMI 1、SMBIOS の呼び（class 0x11・select 0x13・input 1・2）を WMBA で呼ぶと SMI 1 で、`--smi-memory` の byte が buffer の 0x14 に返る。
  - mapping device の無い表（SSDT1 だけ）: ENODEV、block 0、SMI 0。
- わざと壊す確かめ: attach で Method の `_WDG` も評価させると、31 block（WS303 の review M9 の 31 と同じ）・attach の SMI 2 で attach・attach-no-smi・fan-no-smi・contrast-smi が FAIL（元に戻して PASS）。
- 移しの回帰（移した直後と拡張の後の 2 回）: `make -C plan/tools/aml-host`（ASan・UBSan、`-Werror`）warning 0、`python3 plan/tools/aml-host/run-asl.py` 21 passed、`sh plan/tools/aml-host/check-latitude5330.sh` 5 項目 PASS（LTO の stack 7184 B で 10 KiB の予算の中）、`make -C plan/ws052/tests` と `sh plan/ws052/tests/run-host-sleep.sh` PASS、`make -C plan/tools/aml-host kernel-check`（kernel の target と flag、`-Werror`）warning 0（`wmi_attach_device` の frame 1096 B）。
- `python3 plan/tools/style-check.py`（新しい file・`acpi-kern.c`・harness の file）0 件（harness の `aml-host.c` の既存の 1 件も直した）。`git diff --check` 0。
- amd64 の vmunix（`sh plan/tools/ports/build-vmunix-all.sh /tmp/p6-ws333/p017-amd64 amd64`）rc=0、warning 0。vmunix に `drv_acpi_wmi_attach` がある（llvm-nm）。
- 全 20 port の vmunix（`sh plan/tools/ports/build-vmunix-all.sh /tmp/p6-ws333/p017-all`、21b2148c5 の tree）: 20 port 全部 rc=0、warning 0（amd64・intelmac・pcat・pc98・rpi4・rpi5・odroid-c5・visionfive2・zybo・armv7-virt・riscv64-virt・riscv32-virt・m68k-virt・ppc32-mac99・ppc64-pseries・mips32-malta・mips64-malta・mips32-rg350・sun4u・x68k）。
- QEMU amd64（`make ZEDBSD_CONFIG=plan/ws333/tests/config-amd64-boot.mk BUILD=/tmp/p6-ws333/p017-img disk-image`、warning 0、image の vmunix に `drv_acpi_wmi_attach` がある。`BOOT_MODE=uefi-usb OUTPUT=/tmp/p6-ws333/p017-boot plan/tools/boot-test.sh /tmp/p6-ws333/p017-img/hdd-image.img`）: PASS。画面の PNG（`/tmp/p6-ws333/p017-boot/login.png`）で login prompt を見た。console・serial の log は判定に使っていない。root の login の後の dmesg は password を入れないので見ていない（q35 に PNP0C14 は無く、無い道は log を出さない）。

## 未実施・残り

- QEMU の q35 には PNP0C14 が無いので、QEMU の起動は WMI の attach の「無い」道（ENODEV、log なし）の回帰だけ。WMI の device のある表での kernel の attach は host の harness（5330 の表）だけで確かめた。QEMU に `-acpitable` で WMI の SSDT を足す試験は、`boot-test.sh` の UEFI の mode が QEMU の追加の option を取らないのでしていない。
- 5330 の実機での attach の確かめ（dmesg の `acpi: WMI ...` の行）は p018 の実機の試験で。
- WMI の attach の試し直しを呼ぶのは p018（上の判断）。
- 規約の全文の見直しは p024。
