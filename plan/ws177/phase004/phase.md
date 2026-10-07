<!-- awesome-plan project=zedbsd record=ws177-p004 -->

# ws177-p004: desktop の UI の小物（案 D）

Parent: [WS177](../ws.md)
Status: cleared（2026-10-08 Q1 判定、T1-406 の accent と T1-413 の名前の欄の 32）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q884 の 1（P1、2026-10-08、承認は Q1 の dispatch「次の Queue（q884、承認済み、plan/ws177/phasing-20261008.md の案）」）
Origin: [backlog-p1](../backlog-p1.md) の 10・33・34・35・44、[案](../phasing-20261008.md) の D

## 範囲と設計（2026-10-08 P1）

| 行 | 内容 | 形 |
| --- | --- | --- |
| 10 | Settings の管理の欄の長さ | libkeiland の `struct kl_field` に `limit`（その欄の text が持てる最大の byte、0 は従来の 511）と `kl_field_set_limit` を足した（**KL_VERSION 64**）。key・input method の commit・`kl_field_set` は limit を越えない（commit は文字の頭で切る）。limit を下げると今の text も文字の頭で切る。Settings の管理の欄は account-admin が取る長さに: 名前 32（`ADMIN_NAME_MAX`）、全名 64（`admin_display_valid`）、password 256（`ACCOUNT_PASSWORD_MAX`）。`admin_apply` の「too long」の検査は守りとして残す |
| 33 | accent の丸を keyboard で | Appearance の page に key の口 `se_look_key`: Tab で選んだ丸に keyboard（もう一度 Tab・Esc で外す）、左右で移る（端で止まる）、Space・Enter で選ぶ（click と同じ）。keyboard のある丸に accent の輪（丸の 7 px 外）。page を移ると外れる。log `ACCENT focus=N`（-1 は無し） |
| 34 | network の行の hover の as_is | 行の band と行の ink で描く部品（label・SSID・padlock・signal・Connecting...・wired の行）だけを、それぞれ `kwl_accent_as_is` で囲んだ。他（switch の off、key の欄）は dark の写像のまま。`network_draw_row_in` の `kept` の引数を無くした |
| 35 | IME の候補の popup の accent | keiland-ime が `kl_appearance_open` で外観と accent を watch し、選んだ候補の地を `kl_accent_values(kl_accent_get(), LIGHT)` の accent、文字を ink に（popup の窓は従来どおり明るい）。外観の無い compositor では青のまま。終わりに watch を閉じる |
| 44 | Files の Help のカードの `…` | 各行の text を `kl_text_break` でカードの幅で折り返し、カードの高さを折り返した行の数から計る（`help_rows`・`help_text_width`）。key の列のある行は列の後の幅で |

backlog-p2 12（Notes の 512 byte）: limit は上限を下げる口で、Notes の長い行（512 byte を越える）には使えない（`kl_field` の配列が 512）。同じ口ではないので一緒にしない（Notes の Phase で text area か可変の欄にする）。

## 変更

- `userland/desktop/include/keiland/keiland.h`（KL_VERSION 64、`kl_field.limit`、`kl_field_set_limit`）、`libkeiland/ui/field.c`、`libkeiland/exports.map`（exports.py で生成）。
- `settings/page-users-admin.c`・`page-look.c`・`pages.c`・`settings.h`・`ui.c`。
- `wayland/network.c`。
- `ime/popup.c`・`main.c`・`program.h`。
- `files/ui-help.c`。
- 試験: `plan/ws177/tests/host-field-limit.{c,sh}`（host）、`desktop-p004.sh`・`config-amd64-p004.mk`（QEMU、T1）。

## 確認（host・build、2026-10-08）

- `sh plan/ws177/tests/host-field-limit.sh` → PASS（plain、ASan/UBSan）: 零の欄は 511、limit 4 で 5 つ目の key を拒む、3 byte の文字は半分に切らない・入るだけ commit、長い set は切る、limit を下げると文字の頭で切る、部屋を越える limit は部屋。
- build（warning 0、-Werror）: `make -j16 BUILD=build/p1-d ZEDBSD_CONFIG=plan/ws113/tests/config-amd64-p006.mk build/p1-d/dynamic/libkeiland.so build/p1-d/bin/settings build/p1-d/bin/files build/p1-d/bin/wayland` と `build/p1-d/bin/keiland-ime`。`python3 userland/desktop/libkeiland/exports.py --check` OK。
- style-check: 変えた file は前と同じか少ない（page-look.c は 1 → 1、既存）。

## 未実施

- QEMU（T1）: `plan/ws177/tests/desktop-p004.sh`（image は `config-amd64-p004.mk` を `plan/ws089/tests/build-settings-image.sh` で）。accent の keyboard（log）と、管理の名前の欄の 32 の絵。
- 絵の確認（UAT、5330 か QEMU）: IME の候補の地が accent、dark での network の行の hover、Files の Help のカードが `…` 無しで折り返す。Linux・FreeBSD の build は未実施。

## Event

2026-10-08 / q884-i01（P1）: 実装と host・build の確認（利用制限での中断の後に再開、worktree の変更は保たれていた）。

## T1-406（2026-10-08 Q1）

desktop-p004.sh PASS（accent の keyboard の focus、Q1 が PNG を目視）。試験の道具の不具合（qmp-pointer に click の step が無く Users > Add を開けない）で kl_field の上限は QEMU で未確認。P1 が試験を直して再依頼する。

2026-10-08 / T1-406（Q1 の判定）: desktop-p004.sh は PASS（accent の 5 本）。ただし `qmp-pointer` に click の step が無く Users > Add が開かず、名前の欄の 32 は未確認（打った文字は Current password の欄へ）。直し（P1）: click を move・down・up に、Add User... は Settings の `ZSETTINGS CONTROL index=21` の行から位置を取る、名前・全名の欄の長さを log（`USERS admin field=N length=L`、password は出さない）、32 で止まり 33 以上が無いことを log で判定。T1 の再試験を依頼。

## T1-407（2026-10-08 Q1）

FAIL（4 回とも同じ）: accent の 5 行は ok、`the Add User... button (control 21) is not on the Users page`（users-layout.txt は controls=4、index 1・2・3・10 だけ）。kl_field の上限は QEMU で未確認のまま。P1 に戻す（試験の利用者が管理者でないか、Add の口の位置の前提の誤り）。

## T1-407（2026-10-08、4 回とも一部 FAIL）と試験の直し（P1）

accent の 5 行は ok。`FAIL: the Add User... button (control 21) is not on the Users page`（users-layout.txt は controls=4: index 1・2・3・10 の password の card だけ）。原因は試験の前提: Settings を root で起動していた（log `USERS account name=root`、`list count=1`）。root は人の account ではないので一覧に自分の行が無く、`admin_available` が自分の行の admin（wheel）を見つけられず管理の card を出さない（code の動きは設計どおり、実の session は kei）。compositor の側は `kl_backend_account_can_administer` で ADMINISTER を出している。
直し: `desktop-p004.sh` の 2 を、Settings を kei（wheel の人）で起動する形に（root の compositor の socket `/tmp/wayland-0` を chmod 666、`su kei -c`、log は `/tmp/s-kei.log`、出力 `settings-kei.log`、FAIL の時の users-layout.txt に USERS の行も）。code は変えない。T1 に再試験を依頼する（T1-407 の再）。

## T1-412（2026-10-08 Q1）

FAIL（2 回とも）: kei で起動した Settings が `ZSETTINGS INSTANCE alone errno=21`・`ZSETTINGS FAILED operation=window error=5` で窓を作れずに終わった（settings-kei.log）。試験の起動の仕方（kei の環境・XDG_RUNTIME_DIR・socket の権限）を P1 が直す。

## T1-412（2026-10-08、desktop-p004 は FAIL）と移し替え（P1）

kei で起動した Settings は `ZSETTINGS INSTANCE alone errno=21`（EOPNOTSUPP。runtime の folder の /tmp が kei の物でないので、ひとつだけの起動の socket を作らない。害は無い）を出した後、`ZSETTINGS FAILED operation=window error=5`（EIO）で窓を作れずに終わった。root で走る `--testing` の compositor に別の uid の client を繋ぐ形は、この試験の道具の外になる（窓の資源の権限）。
直し: 名前の欄の 32 の確認を、kei の本物の session で走る AAT に移した。`tests/scenarios/apps/settings/manage-users.md` の 2（新しい段）と `plan/tools/aat/scenarios/helpers_apps.py` の `apps.settings.manage-users`: Add User を開き、英字を 40 字打って `USERS admin field=0 length=32` が出て 33 以上が無いことを見て撮り、Esc で閉じてから元の段に進む。`check-scenarios.py` は PASS。`desktop-p004.sh` からは 2 を除いた（accent の 5 本と失敗の検査が残る）。code は変えない。

## T1-413 の判定（2026-10-08 Q1）

PASS: AAT apps.settings.manage-users の段 5 で 40 字を打つと `USERS admin field=0 length=32` で止まり 33 以上の行が無い。Q1 が PNG（build/review/t1-413/apps.settings.manage-users-name-32.png）で名前の欄が 32 字（abcdefghijklmnopqrstuvwxyzabcdef）で止まるのを目視。accent は T1-406。IME の候補の accent・dark の network の行・Files の Help の折り返しは実機の UAT の絵。cleared。
