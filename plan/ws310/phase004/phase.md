<!-- awesome-plan project=zedbsd record=ws310-p004 -->

# ws310-p004: `acpi-osc-codec`（純粋な関数）と host 試験

Status: in-progress（2026-11-15 P2 q2106-i01: 実装済み・試験 PASS。cleared は Q1）

- Queue: q2106-i01（P2、worktree `p2-ws310`、branch `agent/p2-ws310`、基点 main 52b2e7e351）。
- 範囲: `src/drivers/acpi/acpi-osc-codec.c`・`.h`（新）、`plan/ws310/tests/osc-codec/`（新）、この phase.md。kernel の build への組み込み（`mk/kernel.mk`）は案だけ（下）。
- 設計: [design.md](../design.md) §5.1・§5.2・§5.3・§8 の p004 の行。

## 受け入れ条件（design §8）

正常・masked・failure・UUID・revision・短い Buffer・Integer・余計な bit・count 2 と 3 の試験が PASS（ASan・UBSan）、わざと壊す 3 通り以上を捕える、UUID と bit の値の block に `Values referenced:`。

## 実装

- `src/drivers/acpi/acpi-osc-codec.h`: 3 つの UUID（ToUUID の byte の並び）、DWORD1 の bit（query・failure・UUID・revision・masked）、platform の revision・DWORD 数・CPPC（bit 5）・CPPC 2（bit 6）、processor の revision・DWORD 数、PCIe の revision・DWORD 数・support（拡張 config・ASPM・Clock PM・segment・MSI）・control（hotplug・SHPC・PME・AER・PCIe Capability・LTR・DPC）。`enum drv_acpi_osc_outcome`（ABSENT・GRANTED・MASKED・REFUSED・BROKEN）、`struct drv_acpi_osc_request`・`struct drv_acpi_osc_result`。kernel の header は `uapi/errno.h` だけ（host で単独に build できる）。
- `src/drivers/acpi/acpi-osc-codec.c`:
  - `drv_acpi_osc_encode(request, query, buffer, size)`: DWORD1（query なら bit 0 だけ、commit は 0）・support・（3 DWORD の時）control を little endian で書き、長さ（count × 4）を返す。count が 2・3 以外、buffer が小さい、NULL なら 0。
  - `drv_acpi_osc_decode(request, buffer, length, result)`: buffer が NULL（Buffer 以外が返った）か count × 4 より短い → BROKEN。DWORD1 の bit 1・2・3 → REFUSED（何も得ない）。他は返った support・control を求めた bit で切り、bit 4 か求めた bit より少なければ MASKED、全部なら GRANTED。返りの bit 0（予約）は見ない。長い buffer は先頭の DWORD を読む。request・result が読めなければ EINVAL。
  - `drv_acpi_osc_commit_request(asked, query, commit)`: query が GRANTED・MASKED なら「求めた ∩ query で得た」で commit を作り true（0 になっても commit する、design §5.3 の 4）。他は false と 0 の commit。
  - `drv_acpi_osc_outcome_name()`: log の語。
- 出典（block ごとの `Values referenced:`、file の先頭には書かない）: DWORD1 の bit は ACPI 6.5 §6.2.11、platform の UUID と bit は §6.2.11.2・Table 6.13、processor の UUID と revision は ACPI 6.5 Appendix C の `_PDC` の例、ToUUID の byte の並びは §19.6.142 Table 19.41、DWORD の little endian は §19.3.5 Table 19.5。PCIe の UUID・bit は PCI Firmware Specification が無いので Linux v6.12.5 `include/linux/acpi.h` の値（値だけ。code・名前・comment・構造は写していない）と 5330 の DSDT `\_SB.PC00._OSC`。

## 試験

`sh plan/ws310/tests/osc-codec/run.sh`（出力は `build/tmp/ws310-osc-codec`）:

1. `test-osc-codec.c`（ASan・UBSan、52 の確かめ）。期待の値は手の計算: UUID は試験の中で文字列を Table 19.41 の並びに置き直して比べる。encode は platform の query（`01 00 00 00 60 00 00 00`）、PCIe の commit（`00…00 11 00 00 00 30 00 00 00`）、byte の順（0x01020304 → `04 03 02 01`）、短い buffer・count 1・4・NULL の拒否、count 2 で control を書かない事。decode は granted、masked（bit 4 あり・無し）、failure・UUID・revision の REFUSED（support を残した firmware でも 0）、7 byte・0 byte・Integer（NULL）の BROKEN、query の bit の折り返し、長い buffer、5330 の `\_SB.PC00._OSC` の query の答え（status 0x11・control 0x18、求めた 0x1F・0x1D → MASKED・support 0x1F・control 0x18）、余計な bit（全て 1 → 求めた 0x11・0x10 に切る）、3 DWORD に 8 byte → BROKEN、count 5・result NULL の EINVAL。commit は masked → 0x1F・0x18、空の commit も作る、REFUSED・BROKEN・ABSENT は作らない。
2. 3 つの UUID を iasl の `ToUUID` で compile した表を aml-host で評価し、codec の byte と比べる（独立の照合）。
3. codec を kernel の flag（`--target=x86_64-unknown-none -ffreestanding -nostdlibinc -mno-red-zone -mgeneral-regs-only -Os -Wall -Wextra -Werror` ほか、`-DKERN_UAPI_NATIVE`）で compile、warning 0。
4. わざと壊した 4 通りの写し（DWORD を big endian、求めた bit で切らない、failure を拒否にしない、UUID の byte の順）を sed で `build/` に作り、どれも unit が FAIL する事。

結果（2026-11-15、host の cc と clang）:

```
unit: PASS (test-osc-codec: PASS (52 checks))
uuids (iasl ToUUID = codec for platform, processor, pcie): PASS
kernel-flags (warning 0): PASS
mutant-big-endian (test-osc-codec: FAIL (3 of 52 checks)): PASS
mutant-no-cut (test-osc-codec: FAIL (2 of 52 checks)): PASS
mutant-failure-accepted (test-osc-codec: FAIL (1 of 52 checks)): PASS
mutant-uuid-byte (test-osc-codec: FAIL (2 of 52 checks)): PASS
osc-codec: PASS
```

## mk の差分の案（ユーザーの承認まで当てない）

Claude は `Makefile`・`mk/` を変えない（AGENTS §7）。codec を kernel に入れるのは、それを呼ぶ `acpi-osc.c`（p005）と同じ時でよい。案（`mk/kernel.mk` の `# [ACPI Core]` の一覧、名前の順）:

`src/drivers/acpi/acpi-kern.c \` の行と `src/drivers/acpi/acpi-pci-power.c \` の行の間に 1 行（tab で字下げ）:

```make
	src/drivers/acpi/acpi-osc-codec.c \
```

p005 では同じ一覧に `src/drivers/acpi/acpi-osc.c` も足す案になる。`plan/tools/aml-host/Makefile` は今は codec を要らない（p002 の harness は codec を使わない）。

## 未実施

- kernel の build（`make` の target）での compile: codec は今の `mk/` に入っていない（上の案）ので、代わりに kernel の flag で単独に compile した（試験 3）。toolchain の clang（`build/llvm`）はこの worktree に無いので host の clang。
- arm64・riscv64: codec は arch に依らない C で、kernel の build に入るのは p005 の後。
