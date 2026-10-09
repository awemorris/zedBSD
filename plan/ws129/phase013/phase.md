<!-- awesome-plan project=zedbsd record=ws129-p013 -->
# ws129-p013: 利用の手引きと既知の問題の下書き

Status: in-progress（q715、P2、2026-10-05。下書きは済み、受け入れの判定は Q1・ユーザーの review）
Disposition: normal
Parent: [WS129](../ws.md)
Focused goal: fg019（ベータ1）
Queue: q715 / q715-i01（P2）
目安: 1.5h（文書）

## 範囲

[release.md](../release.md) §9 の決定（U3・U8・U13 ほか）に従い、`docs/release/` に英語で次を書く:

1. 利用の手引き: USB への image の書き方（Linux・Windows・macOS）、BIOS/UEFI の設定、最初の login、Wi-Fi の接続、U3 の password と sshd の注意、対象の platform。
2. 既知の問題の最初の一覧: [Bug Board](../../known-bugs.md) の未解決の Bug から利用者に見える物を選ぶ。10/13 の RC で p005 が見直す。

機能の一覧（release notes の本体）は p005 に残す。

## 受け入れ

上の 2 つの文書が docs/release/ に在り、release.md §9 の決定と矛盾しない。docs/ から plan/ へ link しない。

## 所有 path

`docs/release/`、`plan/ws129/`。

## 実施（2026-10-05、q715、P2）

- `docs/release/README.md`（release の文書の索引）、`docs/release/zedbsd-1.0.0-beta1-guide.md`（利用の手引き）、`docs/release/zedbsd-1.0.0-beta1-known-issues.md`（既知の問題）を英語で（U8・U13）。`docs/README.md` の Sections に 1 行（Q1 の許可、2026-10-05）。docs/ から plan/ への link は無い（grep で確かめた）。
- 手引き: 対象は Latitude 5330（5320 は WS118 がベータ4 以降なので「未対応」、既知の問題に）、USB 4 GB 以上、SHA256SUMS の確かめ（Linux・macOS・Windows）、USB への書き方（Linux の dd、macOS の rdisk、Windows の balenaEtcher・Rufus）、UEFI・Secure Boot を切る（loader は署名なし）、F2・F12、最初の login（今の実装: 起動時に kei で自動 login、`kei`/`kei`、root は login できない＝U10 の lock）、Wi-Fi（AX211、WPA2-Personal、5GHz は BUG-145）・USB の LAN（CDC-NCM/ECM、起動前に挿す＝BUG-168）・RTL8822BU、security の注意（password は公開で Beta 1 では変えられない（passwd が無い）・sshd が有効で kei は止められない・信頼できる network だけで使う、U3・U4）、Windows の zip（release に在る時だけの条件つき、U6）、license（LICENSES.md・`/usr/share/licenses/INDEX`）。
- 既知の問題: Bug Board の未解決（tracking・scheduled）から利用者に見えるものを選んだ（BUG-165・159・119/095・156・167・166・190・182・172/191・105・145・157・168・176・174・170・171・120/175・177・173）。開発・QEMU・PC-98・FreeBSD・Windows の内部の問題は入れていない。BUG-183〜189（Wi-Fi の Settings、q703 で直した、T1 の結果待ち）は入れていない。
- ユーザーの未決（master の pending-decisions、Q1 が朝に聞く）: 自動 login・root の lock（U10）・passwd が無い件・CI の config。手引きは今の実装で書いた。決まったら p005（10/13 の RC の見直し）で手引きの「4. Start」と「Security notes」を直す。

### 確かめ

- 文書の主張を source で確かめた: login の lock（`userland/base/login/verify.c`）、自動 login（`/etc/keiland/autologin` = kei、release の image の rootfs で確認）、Settings の頁の名前（Wi-Fi・Sound、`userland/desktop/settings/pages.c`）、passwd・su・doas が無い、release の asset の名前（`.github/workflows/release.yml`）。
- 未実施: 手順（USB への書き込み・F2/F12・Secure Boot）は実機で確かめていない（p007 の実機の確認で）。

## ベータ2 の更新（2026-10-09、q920、P1）

Q1 の割り当て（q920 (3)「menuconfig の新しい階層・keiland の prerequisites を反映」）。
- `docs/howto/build-from-source.md` §3 を WS193 の menu（CPU / Board・Boot Option・Development・Base・Desktop・Packages・Firmware・Build boot image・Exit）に書き直した。menu に無い設定（Variant・kernel の option・X11・試験）は config.mk に手で書き menu が保つこと、Build boot image の進捗と log の場所、Variant の表を値で。i386 は menu から外れた。
- `README.md` の Keiland の Linux・FreeBSD の節に、WS194 の package の確認・導入の問い・build の後の install の問い・`KEILAND_ASK=n` を足した。
- 利用の手引き `docs/release/zedbsd-1.0.0-beta2-guide.md`: Security notes を今の実装に（passwd と Settings → Users の Change Password、`sudo service stop/disable sshd`、kei は wheel で sudo できる）。有線の後挿し（BUG-168 は QEMU で直った、実機は UAT、review の comment）、Bluetooth の節（条件付き）、「Building it yourself」の節。
- 確かめ: Settings の頁の名前（pages.c）、Users の Change Password（page-users.c）、`/etc/group` の wheel に kei、`service` の stop・disable（service-command.c）、menu の項目（tools/menuconfig.py）。docs/ から plan/ への link は無い（comment の中の名前だけ）。
- 未実施: 手引きの手順の実機の確認（p007）。
