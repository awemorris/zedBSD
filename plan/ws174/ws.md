<!-- awesome-plan project=zedbsd record=ws174 -->
# WS174: 起動時の Ctrl / Shift で boot の選択を変える

Master: [master](../master.md)
Status: incomplete（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: p003 を cleared（C3 の間欠は BUG-272）。2026-10-08 q910 P2 の照合: p003 は T1-214 の再試行で全 cell PASS（1 回目は C3 だけ FAIL）→ Q1 の判定（間欠の C3 を Bug にするか）。実機（UEFI の 5330）は UAT）（2026-10-06 P1: p002・p005 の実装と記録。p003 は T1-213 が FAIL（key の検出・試験の sysctl）、同日直して T1 の再試験待ち。2026-10-05 夜 追加。設計の第 3 版まで。ユーザーが仕様を変更: Ctrl = kmsg を console（logo なし、640x480 の希望）、Shift = login を console、config の形式は変えず UEFI の bootloader だけ。ユーザーの実装の指示あり（BIOS は後日））
Primary Milestone: MG003
Related: MG006（graphical boot）

## 由来

ユーザー（2026-10-05 夜）「おっと、イメージを実機で起動したら、カーネルの起動の途中でpanicしたとみられますが、グラフィカルブートなのでわかりません。ブート時にctrlキーが推されていたらsafe boot optionsの設定に切り替えるように、ブートローダとブートコンフィグファイルを変更したいです。設計だけできますか？」

## 目標

boot の時に **Ctrl** が押されていたら UEFI の bootloader が kernel に渡す `kmsg=` を `console` にし（logo を出さず、GOP に 640x480 を希望）、**Shift** が押されていたら `login=` を `console` にして（sessiond が終わり getty が始まる）kernel を起動し、kernel の最後の message が画面で読める。boot の config の file の形式は変えない。kernel・init・sessiond は変えない。

ユーザーの仕様の変更（2026-10-05 夜）と決定（logo を落とす: Yes、640x480: Yes、1 秒止める: No）、実装の指示「OKです。ブートローダの仕様変更を実装してください。BIOSは後日でよいです。」は [phase001/phase.md](phase001/phase.md) の末尾の 3 節。

## Phase

| Phase | 内容 | 状態 | 依存 |
| --- | --- | --- | --- |
| [ws174-p001](phase001/phase.md) | 設計（UEFI loader の Ctrl・Shift の検出、parameter record の書き換え、kernel への渡し方、docs、試験）。全文 [design.md](phase001/design.md) **第 3 版** | cleared（2026-10-05 夜 Q1、第 3 版。Q1 の決定で S2 は外した） | — |
| [ws174-p002](phase002/phase.md) | module と host 試験: `bootloader/common/boot-override.c/.h`（record の書き換え、純粋）、`vmunix.mk` の object の rule と link、`tests/config-amd64-keys.mk`。host 試験 O1〜O11 | cleared（2026-10-06 P1、Q1 の確認待ち） | p001 |
| [ws174-p003](phase003/phase.md) | UEFI loader: `bootloader/uefi/boot-keys.c/.h`（Ex protocol の検出）・`uefi.h` の宣言・`bootx64.c` の S0・S1（Q1 の決定で S2 は外した）・書き換えの適用・640x480 の希望・告知・`A64 PARAMS OVERRIDE`。host 試験 K1・K2。docs 4 件。`run-boot-keys-qemu.sh`。T1 の QEMU 5 cell（Q1 経由） | cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: T1-214 の再試行で全 cell PASS。1 回目の C3 の不検出は BUG-272（tracking）。旧: uncleared（2026-10-06 T1-213 FAIL。P1 が直した: loader は `EFI_NOT_READY` と共に…） | p002 |
| ws174-p004（案、後日） | BIOS PC/AT: int 16h AH=02h の Shift/Ctrl の flag、`zbl_boot_override_apply()` の i386 の object、告知、`stage2_end` の記録 | planning（ユーザー「BIOSは後日でよいです」） | p002 |
| [ws174-p005](phase005/phase.md) | 規約の全文の見直し（新しい関数と変えた関数） | cleared（2026-10-06 P1、指摘 0、`efi_main()` の既存の本体は例外として記録。Q1 の確認待ち。T1-213 の後に `zbl_uefi_boot_keys_sample()` を変えたので、その関数は見直しの再確認が要る） | p003 の実装（p004 をやるならその後） |

## 受け入れ（WS）

- 実機 5330 で、Ctrl と Shift（を押したまま Space を繰り返し叩く）で起動すると loader の告知 2 行が出て kernel の message が console に流れ、**止まった時に kernel の最後の message が画面に残る**（写真）。Ctrl だけ・Shift だけ・修飾 key 単独で効くかは firmware の事実として記録する。担当はユーザーと Q1、どの実装 Phase の cleared にも含めない（design.md §9.3）。
- QEMU の証拠（T1、design.md §9.2）と実機の証拠は分けて記録する。
