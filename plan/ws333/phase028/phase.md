<!-- awesome-plan project=zedbsd record=ws333-p028 -->

# ws333-p028: 5330 を人手なしで戻せる起動（Intel TCO watchdog の driver と一回だけの起動）

Status: in-progress（2026-11-13 Q1、q2028-i01 P3）
Parent: [WS333](../ws.md)

## 目的

[p027](../phase027/phase.md) の危ない試験（`i915.start=manual`・S0 idle・新しい image）を、遠隔の電源の無い 5330 で人手なしに流せるようにする。2026-11-13 ユーザーが進め方を承認した。

- 一回だけの起動: 既にある。`~/boot-zedbsd.sh`（`plan/tools/hw5330/debian/boot-zedbsd.sh`）が UEFI の BootNext で zedBSD を一度だけ起動し、次の起動は Debian。
- 足りない物: zedBSD が止まった時に再起動させる hardware の watchdog。zedBSD には watchdog の driver が無い。

## 事実（2026-11-13 Q1 が 5330 の Debian で見た）

- `iTCO_wdt: Found a Intel PCH TCO device (Version=6, TCOBASE=0x0400)`、heartbeat 30 秒、`intel_pmc_bxt` を使う（NO_REBOOT の bit は PMC の側）。PCH は Alder Lake-P（SMBus 00:1f.4 8086:51a3、eSPI 00:1f.0 8086:5182）。

## 範囲

1. Debian で TCO の watchdog が本当に機械を再起動させるかを確かめる（`/dev/watchdog` を開いて叩くのを止める → 再起動して Debian に戻る）。戻らなければ止めて Q1 に返す。
2. zedBSD の Intel TCO watchdog の driver（v6 の PCH。TCOBASE と NO_REBOOT の見つけ方は Intel の PCH の datasheet と ACPI に基づき、本来どうあるべきかで設計する）。起動の option で有効にし（既定は off）、kernel の timer か thread で叩く。kernel が止まれば叩かれず再起動する。panic の時も叩かない。user の `/dev/watchdog` は範囲の外（要れば案）。
3. 確かめ: host 試験（register の列）、QEMU（q35 の ICH9 の TCO で動く範囲）、5330 で watchdog を有効にした zedBSD を一回だけの起動で起こし、わざと止めて Debian に戻る事。

## 受け入れ

- 5330 で、watchdog を有効にした zedBSD が止まると、人手なしに Debian に戻る。普通に動いている間は再起動しない（10 分以上）。build warning 0。
