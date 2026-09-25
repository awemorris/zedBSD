<!-- awesome-plan project=zedbsd record=ws003 -->

# WS003: 旧実機 bring-up（終了・再利用禁止）

<!-- awesome-plan-current:start -->
Status: completed
Completed: 2026-09-12（ユーザー判断で終了。受け入れ全体は満たしていない）
Primary Milestone: MG003
Related Milestones: MG008
Objectives: O2, O4
Parent: [Master](../master.md)
Queue: なし
Resume point: なし（新しい要求は新しい WS として立てる。この WS は再開しない）
<!-- awesome-plan-current:end -->

## 目標

Latitude 5320・Panasonic CF-SV7／LX6・PC-9821V13 などの実機で zedBSD を起動し、移植上の問題を解消する。

## 結果

xHCI の列挙・command・endpoint 回復・SuperSpeed、USB storage の flush、共通 boot parameter とその x86 handoff、boot slot と root 選択、複数 swap、UEFI の大きな媒体、CF-SV7 の初期 ACPI、PC-9821V13 の IPL と Stage-1 を成立させた。

## 制限・移管

ユーザー指示（2026-09-12）で終了し、再利用しない。インストーラの実機受け入れ（p018・p019・p026〜p032）は WS028、PowerPC 移植（p033〜p039）は WS027 へ移し、元の Phase は canceled とした。p025（HAL の clock source 分割）などの残りは Future Work F-004 に保留した。

## Phase 一覧

| Phase | 内容 | Status |
| --- | --- | --- |
| ws003-p001 | Latitude 5320 hardware inventory | uncleared（終了時点。後継なし） |
| ws003-p002 | Latitude UEFI memory-map normalization | cleared |
| ws003-p003 | Latitude xHCI capability/MMIO bring-up | uncleared（終了時点。後継なし） |
| ws003-p004 | Latitude xHCI device enumeration | cleared |
| ws003-p005 | xHCI command and cancellation lifecycle | cleared |
| ws003-p006 | xHCI halted-endpoint recovery | cleared |
| ws003-p007 | shared DMA allocation synchronization | cleared |
| ws003-p008 | xHCI device association lifetime | cleared |
| ws003-p009 | xHCI SuperSpeed endpoint context | cleared |
| ws003-p010 | USB-storage flush capability | cleared |
| ws003-p011 | common boot-parameter core and init selection | cleared |
| ws003-p012 | x86 boot-parameter handoff | cleared |
| ws003-p013 | boot slots and root-source selection | cleared |
| ws003-p014 | multi-source swap activation | cleared |
| ws003-p015 | four-platform boot-parameter acceptance | cleared |
| ws003-p016 | static image boot parameters and Python-regression removal | cleared |
| ws003-p017 | UEFI LoadOptions firmware compatibility | uncleared（終了時点。後継なし） |
| ws003-p018 | Latitude existing-FAT NVMe overlay installation and boot | canceled（WS028 へ移管） |
| ws003-p019 | Latitude NVMe native installation and boot | canceled（WS028 へ移管） |
| ws003-p020 | Panasonic CF-SV7 early ACPI/interrupt bring-up | cleared |
| ws003-p021 | portable GPT image extent on larger USB media | cleared |
| ws003-p022 | PC-9821V13 IPL stack and disk-read contract | cleared |
| ws003-p023 | PC-9821V13 IPL entry localization | cleared |
| ws003-p024 | PC-9821V13 Stage-1 fixed-read compatibility | cleared |
| ws003-p025 | HAL clock-source split and amd64 SMP monotonic counter | uncleared（Future Work F-004 へ保留） |
| ws003-p026 | PC98 QEMUの /sbin 配置修復 | canceled（WS028 へ移管） |
| ws003-p027 | PC98 menuconfigのPCI・USB選択 | canceled（WS028 へ移管） |
| ws003-p028 | Let's Note LX6 USB起動のbootパーティション識別 | canceled（WS028 へ移管） |
| ws003-p029 | PC-9821V13 インストール実機受け入れ | canceled（WS028 へ移管） |
| ws003-p030 | Let's Note SV7 インストール実機受け入れ | canceled（WS028 へ移管） |
| ws003-p031 | Let's Note LX6 インストール実機受け入れ | canceled（WS028 へ移管） |
| ws003-p032 | 実機インストーラbring-up変更の最終規約確認 | canceled（WS028 へ移管） |
| ws003-p033 | OF起動契約・APM/FAT imageとXCOFFローダ入口 | canceled（WS027 へ移管） |
| ws003-p034 | zedboot.cfg・FAT読み取り・PPC ELF handoff | canceled（WS027 へ移管） |
| ws003-p035 | PPC HAL・mac99基板対応とカーネル初期起動 | canceled（WS027 へ移管） |
| ws003-p036 | amd64でUSB OHCI・USBストレージを検証 | canceled（WS027 へ移管） |
| ws003-p037 | PPCユーザーABI・libcとinit到達 | canceled（WS027 へ移管） |
| ws003-p038 | PPC USB boot・rootfs.img/data.img統合 | canceled（WS027 へ移管） |
| ws003-p039 | PPC/OHCI変更の最終規約・統合確認 | canceled（WS027 へ移管） |

## 記録の所在

各 Phase の計画・結果・試験は、2026-09-24 の plan 整理で削除した。git の commit `04bc9eab` 以前の `plan/ws003/` にある。Queue ごとの履歴は [plan/history](../history/index.md) に残る。
