<!-- awesome-plan project=zedbsd record=ws307-p007 -->

# ws307-p007 UEFI の runtime services と Boot####

- 親: [WS307](../ws.md)。仕様: [spec.md](../spec.md) §5.8（U5・U15）。Queue: q1665 / q1665-i01（担当 P4、2026-10-29）。
- Status: in-progress（2026-10-30 ユーザーの回答で HAL の差分の案は取り下げ。kernel の driver の形で作り直す（改訂 3、下）。code はまだ無い）
- 出典: 2026-10-28 U5「…Boot####の作成もWSに入れる。」、U15「末尾に足します。また、KeiではなくzedBSDです。…」。

## 最初の段（2026-10-29、P4）

- HAL の差分の案: [proposed/hal-uefi-variables.md](../proposed/hal-uefi-variables.md)（loader の handoff v8、`hal_get_arch_handoff("uefi.runtime")`、`hal.h` に UEFI の変数の 4 関数、amd64 の実装の方針、判断 H1〜H3 と推し）。AGENTS.md §7 により承認まで実装しない。
- 承認までの間: installer（p009・p010）は loader を ESP に置き、S11 で「firmware の boot menu で選ぶ」を出すだけ（spec §5.8）。

## 改訂 2（2026-10-29、P4）

- ユーザーの回答（原文、Q1 の中継）:「UEFIに限らないファームウェア環境変数という概念で作り直してみてください。もう一度レビューして承認します。」
- 案を [proposed/hal-fw-variables.md](../proposed/hal-fw-variables.md)（firmware の環境変数: 名前空間（UEFI・Open Firmware・U-Boot・Pi の EEPROM）＋vendor＋UTF-8 の名前の key、`hal_fw_variable_space_info`・`get`・`set`・`next`）に作り直した。前の案 [proposed/hal-uefi-variables.md](../proposed/hal-uefi-variables.md) は置き換え（file の削除は Q1）。

## 残り

- ユーザーの承認（改訂 2 の F1〜F3、Q1 経由）。
- 承認の後: loader の v8、HAL の実装、`/dev/system` の UAPI（形・版・上限を code の前に決める）、`efiboot`、QEMU の OVMF で Boot#### を書いて読み戻す（案 §4）。

## 改訂 3（2026-10-30、Q1 の中継、q1698-i01）

- ユーザーの回答（原文、ws.md）:「WS307ですが、ファームウェアの環境変数は文字列でいいのではないかと思ったのと、HALでアクセスする必要性が感じられませんでした。UEFIの起動エントリーは、UEFIランタイムサービスを経由すると思うのですが、それはカーネルのドライバで実装するものではないでしょうか。カーネルにファームウェア変数書き込みのドライバを作るのがいいと思います。」「ご確認の、バイト列にするのはOKです。」
- **取り下げ**: [proposed/hal-uefi-variables.md](../proposed/hal-uefi-variables.md)（最初の案）と [proposed/hal-fw-variables.md](../proposed/hal-fw-variables.md)（改訂 2）は取り下げた。`hal.h` は変えない。file の削除は Q1（履歴は git に残る）。
- Q1 の指示の形（2026-10-30）: loader が UEFI の system table（runtime services の表）と memory map を渡し、kernel は既存の `hal_get_arch_handoff` で新しい名前（`uefi.system-table`・`uefi.memory-map` など）として受け取る。名前の引き当ては `src/hal/amd64` の実装（承認不要）。kernel の firmware 変数の driver が runtime services の領域を kernel に map し、呼び出しを直列にし、amd64 では MS x64 ABI で GetVariable・SetVariable・GetNextVariableName を呼ぶ。`/dev` の口は名前が文字列（GUID と名前）、値が byte 列。userland の `efiboot` が Boot####（EFI_LOAD_OPTION）と BootOrder を作り・読む。QEMU の OVMF で Boot#### を書いて読み戻す。
- 順番: q1698-i01 の他の項目（p015・p002・p004・p008・p014）の後。

### 設計の下書き（実装の前に詰める、P4）

1. **loader**（`bootloader/uefi`）: ExitBootServices の直前の最後の GetMemoryMap の生の descriptor の列（型・物理・仮想・page 数・attribute）を、boot allocation（lifetime KEEP）の領域に写して渡す。handoff に版を足し（v8）、system table の物理 address、map の物理 address・byte 数・descriptor の大きさ・版を置く。
2. **HAL の実装**（`src/hal/amd64/bsp-pcat/boot.c`、API は変えない）: 既存の framebuffer の値と同じく、名前ごとに 1 つの `uint64_t` を返す: `uefi.system-table`・`uefi.memory-map`・`uefi.memory-map-size`・`uefi.memory-descriptor-size`・`uefi.memory-descriptor-version`。無ければ NULL（BIOS・古い loader）。
3. **driver**（amd64、`src/drivers/firmware/` の新しい file）: system table の signature・revision・CRC32 を確かめ、RuntimeServices の表を得る。
   - **map の方式（詰める点）**: (a) runtime の領域を 1:1（VA＝PA）で map した専用の page table に呼び出しの間だけ切り替える（SetVirtualAddressMap を呼ばない）、または (b) 領域を kernel の VA に map し SetVirtualAddressMap を 1 回呼ぶ。どちらも「ring 0 から実行できる低い VA の 1:1 の map」が少なくとも 1 度要る（(b) でも SetVirtualAddressMap 自体は物理の address で呼ぶ）。既存の口: `hal_space_map(HAL_SPACE_SYS, …, HAL_SPACE_EXEC…)` は kernel の半分にしか置けず、`hal_space_create` の空間は user の page（SMEP・SMAP で ring 0 から実行・参照できない見込み）。loader の `bootstrap_cr3`（identity の map）は paging の後に回収される。まず HAL を変えずに済む形（kernel の半分に runtime の領域を map して (b) の SetVirtualAddressMap を HAL の起動の早い段（identity の map がまだある時）に HAL の実装の中で済ませる、など）を調べ、既存の口で足りない時は理由を付けて Q1 に返す（AGENTS.md §7）。
   - **割り込み**: 呼び出しの間は割り込みを止め、mutex で直列にする（firmware は再入できない。flash への書き込みで数 ms 止まり得る）。
   - **FP の状態**: amd64 の HAL は user の FP の状態を register に置いたまま kernel を走らせ（`task.c` の context switch で fxsave/fxrstor）、firmware は SSE を使ってよい。呼び出しの前に driver が自分の 16 byte 境界の領域に fxsave64 し、UEFI の求める MXCSR 0x1F80・x87 の制御語 0x037F にして呼び、後で fxrstor64 する（割り込みを止めた間なので context switch は挟まらない）。
   - **ABI**: 関数の型に `__attribute__((ms_abi))`（clang が shadow space と callee-saved の XMM6〜15 を扱う）。stack は今の kernel の stack（16 byte 境界）。
4. **/dev の口**: `/dev/fwvar`（root だけ、0600）の ioctl: GET・SET・NEXT。名前は GUID の文字列（36 文字）と UTF-8 の名前（kernel が UCS-2 に変える）、値は byte 列と attribute。上限（名前の長さ・値の大きさ）を code の前に決める。
5. **efiboot**（userland）: `list`（BootOrder と各 Boot####）、`create`（説明「zedBSD」、HD(GPT の partition 番号・署名・開始・大きさ)/File(`\EFI\ZEDBSD\BOOTX64.EFI`)、BootOrder の末尾に足す、U15）、`delete`。installer の S11 から使う所は spec §5.8。
6. **試験**: QEMU の OVMF（書ける VARS の複写）で zedBSD を起動し、`efiboot create` → `efiboot list` で読み戻し、再起動の後も残ること、OVMF がその entry で起動すること（BootNext か BootOrder）。

### 引き継ぎ（2026-10-30 09:13、q1698-i01 のラップアップ）

- 記録だけ（3c14ef8f4、branch `agent/p4-ws307o`）。code は無い。`proposed/` の 2 file は取り下げ、削除は Q1。
- 次の 1 手: 設計の下書きの 3 の「map の方式」を詰める（`hal_space_map(HAL_SPACE_SYS, …)` で EXEC の kernel の map が作れるか、SMEP・SMAP の下で 1:1 の map を HAL の実装の中だけで作れるか、SetVirtualAddressMap を HAL の起動の早い段で呼ぶ形か）。既存の口で足りなければ理由を付けて Q1 に返す。決めた後 loader v8 → HAL の名前 → driver → /dev → efiboot → OVMF の試験の順。
