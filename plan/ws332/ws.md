<!-- awesome-plan project=zedbsd record=ws332 -->

# WS332: tick の周波数を compile の時の定数でなく API で扱い、可変の tick にする

Status: completed（2026-10-31 P6、Q1 の merge で確定）
Primary Milestone: MG004
Parent: [Master](../master.md)
目標: obsolete の `KERN_CLOCK_HZ`（中身は HAL の `HAL_TIMER_FREQUENCY`）を kernel・driver から無くし、時間は単位（ms・ns）の API で扱う。今の tick の周波数を走っている時に取得する口を用意する（周波数を変える実装は将来）。

## ユーザーの決定（要点）

- 2026-10-30 ユーザーから、HAL_TIMER_FREQUENCY を POSIX の公開の macro でないなら API で取る形にし、client の computer では clock を可変にする（ゲームモード 1000 Hz、一般のデスクトップ 100 Hz、待機 10 Hz など）のが現代的だ、という趣旨の指示を受けた。
- 2026-10-30 ユーザーから、sysconf(_SC_CLK_TCK) の導入で KERN_CLOCK_HZ は obsolete になっており、主に試験していた platform 以外での動作が怪しかったので削除を遅らせていた経緯があり、今回削除する、という趣旨の指示を受けた。WS332 は Q1 の提案どおり。
- 2026-10-30 ユーザーから、可変の tick は今の周波数を取得する口だけ用意すればよく、実際に変える実装は今回は不要、という趣旨の指示を受けた。→ 範囲は KERN_CLOCK_HZ の削除と取得の口まで。周期を変える実装は将来（Future Work）。
- Q1 の調べ: POSIX に kernel の tick の周波数の compile の時の macro は無い（CLOCKS_PER_SEC は clock() の単位で XSI は 1,000,000 固定、times() の単位は sysconf(_SC_CLK_TCK)、CLK_TCK は廃止）。

## 結果

- `KERN_CLOCK_HZ` と `KERN_MS_TO_TICKS()` を消した。src・include に残る率は src/kern/clock.c の `static const uint32_t tick_rate`（初期値 `HAL_TIMER_FREQUENCY`）の 1 か所で、
  `include/kern/clock.h` は `<hal/arch.h>` を include しない。時間は `kern_ms/us/ns_to_ticks()`（待つ側、切り上げ）・`kern_ticks_to_ms/us/ns()`（報告する側、切り捨て）・
  `kern_ticks_to_timespec()`・`kern_duration_to_ticks_ceil()` で変換する（clock.c の out-of-line、飽和）。
- 今の率を取る口: `kern_clock_tick_rate()`（tick の単位の 1 秒の数）と `kern_clock_resolution_ns()`（tick の間の長さ）。値は今は boot の時の HAL の率で固定。
  呼び手は定数として持たず、使う時に呼ぶ。HAL の API は変えていない（hal.h は RTC の節の comment の語だけ、ユーザーの承認で直した）。
- 率を仮定していた残りも時間の単位にした: tick の数の literal（VM の空きの待ち、rpi4-vchiq、i915 の worker の待ち・evade）と、「次の tick まで」の短い待ち 32 か所
  （各所の 1 ms の名前を `kern_ms_to_ticks()`）。`kern_usleep_range()` は残りの時間を待つ形にした。
- wlan の UAPI の期限を tick の絶対値から CLOCK_MONOTONIC の ns（`deadline_ns`、`WLAN_ABI_VERSION` 2）にした（1000 Hz の arch で期限が 10 倍ずれていた既存の不具合）。
- 挙動が変わった所: 100 Hz の arch の空きの memory の待ち 1 回が 100 ms から 10 ms（fault が諦めるまでが comment どおりの約 10 秒に）、TCP の keepalive の ms→tick が切り上げ
  （1000・100 Hz では同じ値）、元が溢れていた値の飽和、wlan の UAPI。他は今のどの率でも同じ値。
- 確かめ: 19 port の vmunix（warning 0）を p002・p003・p004 の各 tree で、QEMU の起動（p002: amd64・pcat・rpi4・riscv64-virt・m68k-virt、p004 の先端: amd64）、
  host 試験（tcp-model・cadence-gem・kd-host・cubic・dw-mmc・meson-mmc・i915 の xd と contracts）。詳細は git の履歴の phase.md（下の表の commit）。

## 制限・移管

- 周期を走りながら変える実装（ゲーム・デスクトップ・待機の率、counter から作る期限の単位、quantum・CPU 時間・ITIMER の数え方、HAL の口、誰が変えるか）は作っていない。
  要る事は [design.md](design.md) §5。Future Work への登録は Q1 に頼む。
- 期限の後ろの補いの `+ 1U`（最後の tick から数えるので早く起きるのを補う物、ahci・hda・rpi4-mmal・vchiq・i2c-hid・tcp.c など）は残した。どの率でも 1 周期の補いとして正しく、
  §5 の期限の単位を変える時に見直す。venus の frame の境界の計算（tick の整数のまま）も同じ。
- 名前が `*_TICKS` の macro（58 個）は変換の呼び出しに展開し、33 か所で条件の中に現れる（coding-style §6 の字面に当たる）。§5 で見直す時に合わせて扱う。
- 未実施: wifi の接続（wlan の UAPI の変更は build と起動だけ）、USB・wifi の時間に関わる driver の実機、実機（5330・Pi など）での起動、design.md の絞った版の再レビュー。

## Phase

| Phase | 目的 | 状態 | commit |
| --- | --- | --- | --- |
| p001 | 設計: 時間の単位の API と今の率を取る口（[design.md](design.md)） | cleared（2026-10-30 Q1） | 551b16d66 |
| p002 | KERN_CLOCK_HZ の削除、wlan の UAPI の deadline_ns、x68k の FPU の検査 | cleared（2026-10-30 Q1） | c46408c64・28bf15354・fd89791b0・b73d872e9・b3b05b2be |
| p003 | 率を仮定する残り（tick の数の literal と「次の tick まで」の 32 か所、`kern_usleep_range()`） | cleared（2026-10-31 P6） | ca16e864b・48219a2e9 |
| p004 | 規約の全文の見直し | cleared（2026-10-31 P6） | 48219a2e9 |
