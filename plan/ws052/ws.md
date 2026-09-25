<!-- awesome-plan project=zedbsd record=ws052 -->

# WS052: 電源管理（S0i3、modern standby）

<!-- awesome-plan-current:start -->
Status: planning
Primary Milestone: MG003
Related Milestones: MG004, MG006
Objectives: O2
Parent: [Master](../master.md)
Queue: なし
Resume point: p001 から。WS049（AML interpreter）が前提
<!-- awesome-plan-current:end -->

## 目標

ACPI の S0i3（modern standby、S0 low power idle）に入り、wake の event で戻る。操作は `/dev/system` から行う。

## きっかけ

2026-09-24 ユーザー指示: 「電源管理の実装。ACPIのS0i3に対応します。/dev/systemで制御。モダンスタンバイのみで、S3,S4は対応不要。
S0i1, S0i2は必要に応じてサポートを検討するが、基本的にi3のみでよいと思います。」

## 範囲（ユーザーの指示）

- **S0i3 だけ**。S3（suspend to RAM）と S4（hibernate）は対応しない。S0i1・S0i2 は必要になったら検討する。
- 制御は **`/dev/system`**（既にある system の制御 device、`src/drivers/generic/system-device.c`）の ioctl を足す。

## 要るもの（p001 で確かめる）

- ACPI: FADT の `LOW_POWER_S0_IDLE_CAPABLE`、LPS0 の device（`_DSM`、Intel の UUID と Microsoft の UUID: display off/on、entry/exit の通知）、
  各 device の `_PS0`・`_PS3`・`_PRx`・`_DSW`、wake の GPE → **WS049 が前提**。
- Intel の PMC（Alder Lake）: SLP_S0 の residency の確認、PMC の LTR の無視の設定、IP の電源の状態の確認（S0i3 に入れない原因の調べ方）。
- device ごとの suspend/resume: xHCI（USB）、NVMe（APST・D3）、i915（display の電源、DC6/DC9）、HDA、Wi-Fi（RTL8822B など）、有線 LAN、SATA/AHCI の LPM。
- CPU: 全 core の idle を深い C-state（MWAIT の C10 など）にし、timer と割り込みを止める（HAL の idle と timer の口が要るなら承認が要る）。
- wake の源: 電源 button、lid、キーボード（EC の `_Qxx` / GPE）、USB。
- 利用者の操作: `/dev/system` の ioctl（入る、状態、wake の理由）と、それを呼ぶ command。

## 受け入れ

- 対象機で S0i3 に入り（PMC の SLP_S0 の residency が増えることを確かめる）、電源 button か lid で戻り、login した session と network が続く。
- 規約の全文、build、boot test。実機の証拠が中心（QEMU の S0ix は無い）。

## Phase 一覧

| Phase | 内容 | Status | 依存 | 対象 |
| --- | --- | --- | --- | --- |
| ws052-p001 | 調査と設計: 対象機の FADT・LPS0 の `_DSM`・device の電源の method、PMC の register、driver ごとの suspend/resume の要否と順序、CPU の idle と timer（HAL の口の要否）、`/dev/system` の ioctl の案 | planning | WS049 の p001 | 設計文書 |

## 人間の判断が要る点

- HAL（CPU の idle、timer の停止、割り込みの wake）の差分の承認（p001 が案を作る）。
