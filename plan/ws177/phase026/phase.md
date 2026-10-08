<!-- awesome-plan project=zedbsd record=ws177-p026 -->

# ws177-p026: Settings の Notifications の頁（案 A2）

Parent: [WS177](../ws.md)
Status: in-progress（2026-10-08 夜 P2）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: Q1 の投入（2026-10-08 夜、ユーザーの決定「Settings の他の頁と同じ sidebar の 1 項目で良い」、承認済み）
Origin: [backlog-p2](../backlog-p2.md) の 67（ws169-p002: mail.codes.* の switch を Settings の Notifications の頁にも）、[案](../phasing-20261008.md) の A2。

## 決めたこと（2026-10-08 P2）

- 頁は Settings の sidebar の「Notifications」（今の「later」の stub を置き換える）。2 つの card:
  - **Applications**: Mail・Calendar・Phone・Browser の switch（`notify.allow.<application ID>`、既定 on）。off の application の通知を compositor が断る（`result(request, DENIED)`）。名前は post の app、無ければその client の窓の app_id。設定の無い application（keiland-notify など）は断らない。
  - **Sign-in Codes**: Browser が新しいメールの sign-in の code を読んでよいか（`mail.codes.browser`、Mail の中の switch と同じ設定）。
- 設定の key は `settings-keys.c` の表に 4 行（compositor の resolver、BOOL、既定 1、KEPT）。

## 実装（2026-10-08 P2）

- `settings/page-notifications.c`（新規）、`pages.c`（Notifications の行を実の頁に、検索の語に sign-in codes mail）、`settings.h`、3 つの Makefile。switch は look の設定から描く時に読み、click で `se_look_set_number` で書く（設定が書けない時は無効）。
- `settings-keys/settings-keys.c`: notify.allow.mailer・calendar・phone・browser。
- `wayland/notify-shell.c`: `notify_allowed`（`kwl_settings_number` で notify.allow.<name>）、0 なら `KWL NOTIFY denied` の log と DENIED。窓の app_id を探す関数を notify-popup.c から `kwl_notify_app_id`（kwl.h）として notify-shell.c に移した（popup と共用）。
- 訳: `locale/settings.keys` に application の 4 つの名、`locale/ja/settings.tr` を `tools/i18n/tr.py update` で更新し新しい 10 の文を訳した（他の WS の未訳の 5 つは空のまま）。

## 確認

- host（新規）: `sh plan/ws177/tests/host-notifications.sh` → PASS（ASan・UBSan）: compositor の門（mailer on は posted、calendar off は DENIED、設定の無い app は posted、名前の無い post は窓の app_id の browser（off）で DENIED、on に戻すと posted）、頁（2 つの card、5 つの switch の状態、click で calendar on・codes on・mail off を書く、書けない設定で switch が無効）。
- host（既存）: `plan/ws131/tests/host-system.sh` PASS、`plan/tools/settings/host-settings.sh` 38 passed、`host-store.sh` 43 passed、`tools/i18n/tr.py check` 0 problems。
- build: `make -j16 ZEDBSD_CONFIG=config/current-uat.mk BUILD=build/amd64 build/amd64/bin/wayland build/amd64/bin/settings` exit 0・warning 0、`make -j16 keiland-linux` exit 0。style-check 指摘なし。
- QEMU: 未実施（T1 に）。
