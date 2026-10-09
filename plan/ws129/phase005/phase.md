<!-- awesome-plan project=zedbsd record=ws129-p005 -->
# ws129-p005: release notes・既知の問題の一覧・利用の手引き

Status: planning（p001 と各 WS の成果を待つ、10/13 頃に集める）
Disposition: normal
Parent: [WS129](../ws.md)
Focused goal: fg019（ベータ1）
Queue: none
目安: 2〜3h

## 範囲

1. release notes: 新機能（fg019 の各 WS の到達を ws.md から集める。到達していないものは書かない）、対象 platform（Latitude 5330・5320、各々の制限）、動作の確認の範囲（QEMU と実機を分ける）。
2. 既知の問題の一覧: [Bug Board](../../known-bugs.md) の open の行から、利用者に見えるものを選び、回避の方法を添える。WS の実機の Phase（ws005-p023・ws118-p004・ws119-p006）の不合格を加える。
3. 利用の手引き: USB への書き方（img.gz の展開、Windows・Linux・macOS）、BIOS の設定（UEFI の boot、Secure Boot を切る要否）、初回の login、WiFi の接続（Settings）、使える USB の LAN・WiFi の adapter、インストーラ（WS119 が載れば）。
4. 置き場所は p001 で決めた所（例 `docs/release/beta1.md`）。CI の本文はその file から作る。

## 受け入れ

文書の草稿を main とユーザーに review してもらう（ユーザーの確認が要る）。

## 所有 path

p001 で決めた文書の path、`plan/ws129/`。

## 依存

p001、各 WS の成果。

## 未決の判断

なし（内容の review はユーザー）。

## 追加（2026-10-05 Q1、ws122-p001 の P2 の依頼）

release notes に FFmpeg（libavcodec ほか、LGPL 2.1 以降）の告知と source の在り処（FFmpeg 9.0.2 の tarball の URL と sha256、zedBSD の build の Makefile）を書く。image には `/usr/share/licenses/ffmpeg/` に COPYING.LGPLv2.1 と LICENSE.md が入る。

## 下書き（2026-10-09、q920、P1）

Q1 の割り当て（q920 (2)）。言語は英語（release.md §9 U13）、置き場所は docs/release/（U8）。
- release notes の下書き: `docs/release/zedbsd-1.0.0-beta2.md`（release.yml が `docs/release/zedbsd-<VERSION>.md` を本文にする）。Downloads・Signing in・機能（desktop・app・hardware・開発）・license と FFmpeg の source（tarball の URL と sha256、Makefile）。
  確かめていない・条件付きの行は HTML の comment の `review:` に理由と差し替えの文を書いた（GitHub では見えない、RC で消す）。
- 2026-10-09 ユーザーの決定（Q1 の中継）: WS143 Bluetooth と WS083 Vulkan Video は入れ、間に合わなければ 10/16 に OFF。両方「入る」で書き、OFF の時の差し替えの文を comment に用意した。Vulkan Video が `i915.debug=video` の門のままなら、その旨を行に書く（p008 の既定化しだい）。
- 既知の問題の更新: `docs/release/zedbsd-1.0.0-beta2-known-issues.md`。resolved の BUG-095・156・157・166・167・168・171・172・174・175・176・177・190 を外し、BUG-253/255（蓋）・189/212（有線と Wi-Fi）・222・205・206/207・203・271 を足した。RC までに直す予定の物（BUG-184・188・235・232・179/180・234・191・238/242）と Bluetooth の行は comment に置き、RC で残っていれば表へ移す。
- 判断が要る点（Q1・ユーザー）: FFmpeg の LGPL 2.1 §6 の source の提供。ffmpeg.org への link だけより、release の asset に `ffmpeg-9.0.2.tar.xz` を載せる方が確実（release.yml の変更、p004 の範囲）。
- 未実施: 機能の行の一つ一つの実機の確認（p007）。link は最終の tag ができるまで切れる。

2026-10-09 ユーザーの決定（Q1 の中継、クリック）: FFmpeg の source は「ffmpeg.org への link だけ」。release notes の節を書き直した（版 9.0.2、ffmpeg.org と tarball の link と sha256、zedBSD は patch を当てず configure の引数は GitHub の tree の package の Makefile）。release.yml は変えない。review の comment を外した。
既知の問題の下書きの comment から plan の file の名前を外した（docs から plan へ参照しない）。release notes の file（docs/release/zedbsd-1.0.0-beta2.md）は前の commit で git に入っていなかったので、8a050dff0 で加えた。
