<!-- awesome-plan project=zedbsd record=ws036 -->

# WS036: amd64の成果を他のplatformへ反映し、buildを通す

<!-- awesome-plan-current:start -->
Status: incomplete
Primary Milestone: MG008
Related Milestones: MG003, MG001
Objectives: O2, O4
Parent: [Master](../master.md)
Queue: なし
Resume point: rpi4 は QEMU で login まで到達。残りは p021（最終回帰）、p026（LLVM の AArch64 zedbsd target）、p027（起動 parameter、HAL 差分の承認待ち）
<!-- awesome-plan-current:end -->

## 単一目標

GPUドライバの導入などでamd64を中心に大きく変わったkernelの成果を、i386 PC/AT（pcat）、PC-98、arm64（Raspberry Pi 4）、
sun4u（sparcv9）、X68000（x68k）へ反映し、各platformの壊れたbuildを通す。

到達点: 各platformの対象build（kernelと、そのplatformのuserland）がwarning 0で通る。QEMUで起動できるplatformは、
起動してloginまで確認する。QEMUが無いplatformはbuildまでとし、実機での確認は人間が行う。

2026-09-23 ユーザー指示「ビルドに失敗しているプラットフォームについては、GPUドライバの導入で大きく変更されたカーネルについて、
amd64で開発した成果をi386, pc/at, pc98, arm64/rpi4, sun4u, x68kにも反映し、壊れたビルドを通す、というwsを作ってphaseを作成して
ください。なるべく細かく分解したphaseにして、優先度は下げ、スロットが空いたときに実行していきましょう。」

## 現状（2026-09-23 更新、refactor完了後の実測）

WS035のrefactor（p002・p003・p023・p036・p004）がすべてclearedになった後の状態を測り直した。
pcat・pc98はこの日に通したので（下の「Queue外で達成した分」）、残る未達はrpi4・sun4u・x68kである。

| Platform | kernel | userland | 起動 |
| --- | --- | --- | --- |
| amd64 | 成功（warning 0） | 成功 | UEFI USB起動でlogin promptを確認 |
| pcat（i386） | **成功**（warning 0） | **成功**（`disk-image` まで） | **QEMU（BIOS+IDE）でlogin promptを確認** |
| pc98 | **成功**（warning 0） | **成功**（`disk-image` まで） | **QEMU（PC-98 fork）でlogin promptを確認** |
| rpi4（arm64） | **成功**（warning 0、2026-09-24 p012） | **成功**（`disk-image` まで） | **QEMU（raspi4b）で login を確認**（2026-09-24 p012） |
| sun4u | 失敗: 失敗rule 123、エラー790種。`src/hal/pmem-constraints.c` が無い（No rule） | 未測定（kernelが先） | 未到達 |
| x68k | 失敗: 失敗rule 128、エラー803種。同じ No rule | 未測定（kernelが先） | 未到達 |

**rpi4・sun4u・x68kの失敗の主因は、refactor前の想定（`kern_ptrace` の未link）ではなく、
ws035-p035でamd64に対して行ったuapi/libcのinclude分離が、これらのplatformで行われていないこと**である。
3つとも `-nostdinc -Iinclude -Iinclude/libc` でtreeのlibcヘッダを直接読んでおり、`include/uapi/*` の
「host buildのinclude pathにlibcヘッダがある」guardに当たっている。amd64は `-nostdlibinc` ＋ uapi＋kcrtへ移った。
rpi4はuserlandもsysrootではなくtreeのヘッダを読んでいる。

sun4uとx68kは今もGCC（`~/opt/sparc64/bin/sparc64-unknown-elf-gcc` 等）でbuildしており、
`__UINT32_C_SUFFIX__` / `__UINT64_C_SUFFIX__` の不正suffixが各120件残る（p003・p022の対象）。

このWSはWS035のrefactor後の状態から始める（2026-09-23ユーザー指示。refactorの間はamd64以外が壊れてよかった）。

## i386 の位置付け（2026-09-23 ユーザー決定）

**i386（pcat・pc98）は移植デモ用である。** 基本的なコマンドと Xzed サーバが動くデモができればよく、
性能はあまり考えない。この WS で i386 に求めるのは、build が通り、起動して login でき、
デモが動くところまでである。

## sun4u・x68k のサポート除外（2026-09-23 ユーザー決定）

**sun4u（sparcv9）と x68k（m68k）は、コードを残したままサポートから外す。**
この WS の対象は amd64・pcat・pc98・rpi4 になる。

その platform だけの Phase（p013〜p020）と、それらのための toolchain（p022 LLVM への
Sparc・M68k の追加、p023 GCC への fallback）、GCC で build するための p003・p004 は、
完了扱いにせず **canceled** とした。source と platform の mk は消さない。
tick 周期の見直し（WS040）でも、両者は今の値のまま据え置く。

## rpi4 の到達点（2026-09-24 ユーザー指示）

「rpi4 カーネルはビルドできるように修正して、qemu でログインプロンプトが表示されるところまでもっていきましょう」。
rpi4 の Phase（p001 の rpi4 分、p025、p010、p011 と、login prompt までに要る分）の優先度を上げる。
受入は QEMU（`raspi4b` か `virt`、p001 で選ぶ）で login prompt が出ること。HAL の差分は従来どおり承認を待つ。

## Queue外で達成した分（2026-09-23）

ユーザーの指示「pc98版でログインプロンプトまで進めるかチェックしてください」「pc/atも起動できるようにしてください」
に応じた作業で、下のPhaseの内容が実際に達成された。Queueを組まずに行ったが、実測の結果なのでclearedとして記録する。
行った変更は次のとおり（すべてcommit済み、pushなし）。

- **kcrtへの追従**（p024のpcat・pc98分、p005・p008）: `platform/{pcat,pc98}/vmunix.mk` から
  削除済みの `src/kern/locale-record.o` とlibcのオブジェクト一式を外し、`src/kern/kcrt.o`・`src/kern/heap.o` を足した。
  未linkだった `src/kern/vm-device.o` を追加。
- **ptrace**（p005・p008）: `struct reg` は `include/uapi/reg.h` が `__x86_64__` にしか定義していないため、
  `src/kern/ptrace.c` はamd64専用のままとし、他のplatformでは `sys_ptrace_call()` がENOSYSを返すようにした。
- **pcatのdriver**（p006）: `src/drivers/platform/pcat/serial-mirror.o` を追加。
- **i386 HAL**（p006）: `src/hal/i386/lib.c` に `hal_mmio_read8()`・`hal_mmio_write8()` を実装した。
  `include/hal/hal.h` の既存宣言に対するi386側の補完で、**ユーザーの事前承認を得てから適用した**（hal.hは不変）。
- **共通**（p002）: `src/rtld/rtld.c` の `static_tls_displacement()` は呼び出しが `R_X86_64_TPOFF64` の分岐に
  しか無く、i386では未使用で `-Werror,-Wunused-function` を起こしていた。宣言・定義をamd64専用に囲んだ。
- **image生成**: `tools/build/check-bios-hdd-image.noct` が生成側の `--size-mib`・`--fat-size-mib` を
  「unsupported BIOS checker option」で拒否していたため、他の生成専用オプションと同じく読み飛ばすようにした。
- **起動確認の道具**: `plan/tools/boot-test.py` が8 pixel間隔のcellしか読めず、VGA text bufferのconsole
  （i386 kernel。描画はadapterが行い、同じ8x16 glyphを9 pixel間隔に置く）を読めなかった。8と9の両方を試して
  認識できた文字が多い方を採るようにし、`boot-test.sh` に `BOOT_MODE=bios-ide` を足した。amd64のUEFI起動も回帰なし。
  pc98はPC-98 forkのQEMUと `pmemsave 0xa0000` のtext VRAMで読む（`plan/master.md` の boot test の節）。

p009の「pc98はQEMUが無いのでbuildまで」は**前提が誤っていた**。`~/qemu-pc98/build/qemu-system-i386`
（`-M pc9821,pegc=off,coregraph=on`）が使えるので、login promptまで確認した。実機確認は引き続き人間が行う。

## 制約

- HAL（`include/hal/hal.h`、`src/hal/`）の宣言・実装・責務の変更は、具体的な差分ごとにユーザーの事前承認が要る。
  各platformのHALへamd64と同じ契約の実装を足す作業は、差分を用意して承認を得てから適用する。
  `#include` のパス変更だけは2026-09-23に承認済み。
- aggregate `make check` は使わない。platformごとの対象buildとQEMUで確かめる。
- commitはrootが `git commit -m WIP` で行い、pushしない。

## Phase一覧

優先度は低い。他のWSのQueueでスロットが空いたときに、依存の順に入れる。

| Combined ID | Phase | Status | 依存 | 主なファイル範囲 |
| --- | --- | --- | --- | --- |
| ws036-p001 | 調査と設計（rpi4 分は q366 の p012 で実施。sun4u・x68k はサポート外）: rpi4・sun4u・x68kの失敗の一覧、uapi/libcのinclude分離とkcrtをこの3つへ広げる方針、amd64で増えたkernel・HAL契約（vm_device、GPU、console等）と各platformの未対応の対応表、HAL差分の要否の判断、Phaseの見直し。pcat・pc98は対象外（達成済み） | cleared（q366-i01。rpi4 の失敗の原因と対応は [p012](phase012/phase.md)。HAL の差分は不要だった） | WS035のrefactor（p004まで） | 文書 |
| ws036-p025 | 共通: uapi/libcのinclude分離とkcrtを**rpi4へ**広げる（ws035-p034・p035をamd64以外へ。sun4u・x68kは2026-09-23にサポート外）。`-nostdlibinc`、uapiのguard、`kern_*()` への置換え。3つの最大の失敗原因 | cleared（q366-i01。`-nostdlibinc`＋`KERN_UAPI_NATIVE`・`KERN_KCRT_NATIVE`、kcrt。[p012](phase012/phase.md)） | p001 | platformのmk、src/hal、src/kern |
| ws036-p022 | toolchain: LLVMにSparc（sun4u）とM68k（x68k、LLVMでは試験的target）を加えて再buildし、`llvm-host-archive` でアーカイブを作り、GitHub Release `rev-0` を更新する（patch levelを上げる） | canceled（2026-09-23 ユーザー決定: sun4u・x68k はコードを残してサポート外） | p001 | `toolchain/llvm` |
| ws036-p023 | toolchain: LLVMで対応できないplatformだけ、GCCのcross toolchainにfallbackする（p022の結果で要否を決める） | canceled（2026-09-23 ユーザー決定: sun4u・x68k はコードを残してサポート外） | p022 | `toolchain/`、platformのmk |
| ws036-p002 | 共通: `src/rtld/rtld.c` の未使用関数（i386系userlandの `-Werror` 停止） | cleared（Queue外、2026-09-23） | p001 | `src/rtld` |
| ws036-p003 | 共通: GCCでbuildするplatform（sun4u、x68k）の `stdint.h` の `__UINT32_C_SUFFIX__` 等。p022でLLVMへ移れば不要になる見込み | canceled（2026-09-23 ユーザー決定: sun4u・x68k はコードを残してサポート外） | p022, p023 | libcのヘッダ |
| ws036-p004 | 共通: 存在しない `src/hal/pmem-constraints.c` の参照（sun4u、x68k） | canceled（2026-09-23 ユーザー決定: sun4u・x68k はコードを残してサポート外） | p001 | platformのmk |
| ws036-p005 | pcat: kernelのsource一覧をamd64に揃える（`kern_ptrace`、`vm_device_*` 等、kernel側の未link） | cleared（Queue外、2026-09-23） | p001 | `platform/pcat` のmk |
| ws036-p006 | pcat: `drv_pcat_serial_mirror` と、HALの不足（i386の `hal_mmio_read8`・`hal_mmio_write8` を承認のうえ追加） | cleared（Queue外、2026-09-23） | p005 | pcatのdriver、HAL |
| ws036-p007 | pcat: build通過とQEMU（i386）での起動・login | cleared（Queue外、2026-09-23。`BOOT_MODE=bios-ide` のboot-testがPASS） | p002, p006 | 試験 |
| ws036-p008 | pc98: kernelのsource一覧とHALの不足（`kern_ptrace`、`vm_device_*`） | cleared（Queue外、2026-09-23） | p005 | `platform/pc98`、HAL |
| ws036-p009 | pc98: build通過と、PC-98 forkのQEMUでの起動・login（前提の訂正: QEMUは使える。実機確認は人間） | cleared（Queue外、2026-09-23） | p002, p008 | 試験 |
| ws036-p010 | rpi4: kernelのsource一覧とHALの不足（p025の後に残る分） | cleared（q366-i01。audio・kcrt を足し、SD の lock と serial の console driver を足した。HAL の差分は不要。[p012](phase012/phase.md)） | p001, p025 | `platform/arm64`、HAL |
| ws036-p011 | rpi4: userlandのaarch64向けcompile flag（`-mtls-dialect=` 66件）とuserland build。userlandがsysrootではなくtreeのヘッダを読んでいる件を含む | cleared（q366-i01。aarch64 の sysroot は無いので tree の header のまま `KERN_UAPI_NATIVE`。sysroot は p026。[p012](phase012/phase.md)） | p001, p025 | `platform/arm64` |
| [ws036-p012](phase012/phase.md) | rpi4: build通過とQEMU（raspi4b）での起動 | cleared（q366-i01。login prompt、serial で login と command、`dyntest` PASS、8/8 起動。sched の起動時の競合も直した） | p010, p011 | 試験 |
| ws036-p013 | sun4u: HAL契約への追従（page・space・task・irq等。arm64で行ったのと同じ種類。HAL差分は承認後に適用） | canceled（2026-09-23 ユーザー決定: sun4u・x68k はコードを残してサポート外） | p003, p004, p022, p025 | `src/hal/sparcv9`、HAL |
| ws036-p014 | sun4u: kernel・driverの残りのbuildエラー | canceled（2026-09-23 ユーザー決定: sun4u・x68k はコードを残してサポート外） | p013 | kern、drivers |
| ws036-p015 | sun4u: userlandのbuildエラー | canceled（2026-09-23 ユーザー決定: sun4u・x68k はコードを残してサポート外） | p003 | userland、libc |
| ws036-p016 | sun4u: build通過とQEMU（sparc64）での起動 | canceled（2026-09-23 ユーザー決定: sun4u・x68k はコードを残してサポート外） | p014, p015 | 試験 |
| ws036-p017 | x68k: HAL契約への追従（HAL差分は承認後に適用） | canceled（2026-09-23 ユーザー決定: sun4u・x68k はコードを残してサポート外） | p003, p004, p022, p023, p025 | `src/hal/m68k`、HAL |
| ws036-p018 | x68k: kernel・driverの残りのbuildエラー | canceled（2026-09-23 ユーザー決定: sun4u・x68k はコードを残してサポート外） | p017 | kern、drivers |
| ws036-p019 | x68k: userlandのbuildエラー | canceled（2026-09-23 ユーザー決定: sun4u・x68k はコードを残してサポート外） | p003 | userland、libc |
| ws036-p020 | x68k: build通過（QEMUが無いのでbuildまで。実機確認は人間） | canceled（2026-09-23 ユーザー決定: sun4u・x68k はコードを残してサポート外） | p018, p019 | 試験 |
| ws036-p024 | 共通: 他platformのkcrt対応（各platformの `vmunix.mk` から `locale-record.c` を外し、kcrt・heapを足す）とmapファイル生成の追加（ws035-p034で判明）。**pcat・pc98分は2026-09-23に達成済み**。残りはrpi4・sun4u・x68kで、p025に含めて行う | cleared（rpi4 分は q366-i01。sun4u・x68k はサポート外） | p001, p025 | platformのmk、libc |
| ws036-p021 | 全platform（amd64・pcat・pc98・rpi4）の回帰と全文規約確認（必須の最終確認） | planning | p007, p009, p012 | 全体 |
| ws036-p026 | toolchain: LLVM の zedbsd target に AArch64 を足す（`__ZEDBSD__`、driver の link の emulation）と aarch64 の sysroot。rpi4 の `KERN_UAPI_NATIVE` 等と `ld.lld` の直接呼び出しを外し、Noct・zedinst・開発 file を rpi4 にも入れる（p012 で判明） | planning | p012 | `toolchain/llvm`、`platform/arm64` |
| ws036-p027 | rpi4: 起動 parameter（DTB の `/chosen/bootargs`）を `hal_get_arch_handoff("boot.command-line")` で渡す。今は legacy autoroot。**HAL の差分が要る（承認待ち）** | planning | p012 | `src/hal/arm64`、HAL |

toolchain（p022・p023）は2026-09-23のユーザー指示で加えた。LLVMのtarget追加を優先し、無ければGCCにfallbackする。
今のLLVMは `AArch64;X86` だけをbuildしている。

p001の調査で、失敗の原因が上の分け方と合わない場合は、p001の結果でPhaseを分け直す。HAL差分が要るPhaseは、
差分の承認が得られるまで人間の判断待ちになるので、Queueには承認後に入れる。

2026-09-23の実測で、失敗の原因が当初の分け方と合わなくなったため、p025（uapi/libcのinclude分離とkcrtを
rpi4・sun4u・x68kへ広げる）を足し、p010・p011・p013・p017・p024の依存に入れた。p024のpcat・pc98分は達成済みで、
残りはp025が担う。pcat・pc98のPhase（p005〜p009）とp002はcleared。

並行の目安: platformごとのPhaseは、ファイル範囲（`platform/<名前>`、`src/hal/<arch>`）が重ならないので、
別のplatform同士なら同じQueueで並行できる。共通Phase（p003・p004・p025）は各platformのPhaseより前に行う。

残る到達点: rpi4（build＋QEMU起動）、sun4u（build＋QEMU起動）、x68k（buildまで）、toolchain（p022・p023）、
最終回帰（p021）。pcat・pc98は到達済みだが、p021の全platform回帰では再確認する。
