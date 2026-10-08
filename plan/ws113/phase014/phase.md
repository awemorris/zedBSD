<!-- awesome-plan project=zedbsd record=ws113-p014 -->
# ws113-p014: 拡張の時に個々の display を off にする（Settings の Display の頁）

Status: test-wait（5330）。2026-10-08 q902 P1 の照合: QEMU の機能は T1-370 で ok（試験の期待は Q1 が直した）。5330 の UAT で eDP の off が BUG-266（DP-alt の TC への付け替えが WARN_ON で失敗、session が落ちる、P2 最優先）。直した後に UAT（旧: test-wait（2026-10-08 T1-370: 機能は全 ok、試験の期待の 2 行を Q1 が直した。5330 の実機の UAT 待ち））
Disposition: normal
Parent: [WS113](../ws.md)

## 由来（2026-10-08 ユーザーの UAT、5330 の実機）

「HDMIに出力されました。extendもmirrorも動いています。マウスはまだextendに移動できません。extendのとき、個別のディスプレイをオフにできる設定を作ってください。そうすれば、タッチパネルのテストができるのではかどります！」

→ D-MODES（2026-10-02 の「出力ごとの off は Settings に出さない」）をこの指示で置き換える: 拡張の時だけ、display ごとに on/off を選べる。

## 範囲（案、P1 が詰める）

1. Settings の Display の頁: 拡張の時、card ごとに on/off（少なくとも 1 つは on のまま、全部 off は出来ない）。mirror では出さない。
2. compositor（kwl_displays_apply・displays.conf）: off の display は head を閉じる（RELEASE、消灯）。off にしたのが anchor（内蔵の panel など）なら、on の display へ anchor を移す（p004a の switch・p011a の付け替え）。on に戻すと拡張の位置に戻る。保存（D-STORE）。
3. kl_system_displays の protocol: apply に off の印（places の行に off、または新しい field）、snapshot の flags に OFF。libkeiland の API の版を上げる。
4. 試験: host（validate: 全部 off の拒否、anchor の移し替え）、QEMU（Venus 2 出力: head 1 を off・on、head 0 を off で anchor が head 1 へ）、5330 の実機（内蔵を off にして HDMI だけ、HDMI を off にして内蔵だけ、touch の試験に使えること、ユーザー）。

## 受け入れ

- build warning 0、host 試験、T1 の QEMU、5330 でユーザーの UAT。

## 2026-10-08 実装（q855、P1）

Q1 の ACK:「p014 の範囲 1〜5 で進めてよい（protocol v19・KL_VERSION 59 も可）」。

| 区分 | 内容 |
| --- | --- |
| protocol（v19） | `kl_system_displays_v1` に request 3 `set_shown(request, key, shown)`（since 19、`KL_SYSTEM_SINCE_SHOWN`）、snapshot の flag `KL_SYSTEM_DISPLAY_OFF` 0x20（mirror でも付く）。`KL_SYSTEM_MANAGER_VERSION` 19。結果: 0、mirror と最後の 1 つの on は INVALID（EINVAL）、接続していない key は UNAVAILABLE（ENODEV）、session が active でなければ DENIED（EPERM）。 |
| libkeiland（KL_VERSION 59） | `kl_system_displays_set_shown`（v19 より前の compositor には ENOTSUP）、`KL_DISPLAY_OFF`、exports.map、interface の版 19 と request 4 つ。 |
| compositor | `displays.c`: displays.conf の `off=KEY` 行（`kwl_displays_is_off`・`kwl_displays_set_off`、16 まで）。`heads.c`: `heads_off`（拡張で off、かつ他に on の display が接続している時だけ off = 全部が消えることは無い。lid の kept_off は別）、`kwl_heads_sync` は off の display の head を開かず、開いていれば閉じる（窓・pointer は anchor へ退避、p007）。`kwl_displays_set_shown`（拡張だけ、最後の on は拒否、保存、head を閉じる・開く、anchor が off なら移す）、`kwl_displays_anchor_follow`（anchor の display が off なら on の display へ `kwl_output_switch`、`KWL DISPLAYS anchor off name=.. to=..`）を set_shown・apply（mode の変更）・`kwl_output_tick` の最初の look と hotplug の後に。mirror では off の display も複製に映る（設定は保つ）。`displays-shell.c`: request の処理と OFF の flag。 |
| Settings | 拡張の時、display の行ごとに switch（on/off、押すとすぐ `set_shown`）、最後の on と要求の待ちの間は押せない。off の display は配置の図と Apply の place から外す。結果の文（EINVAL「At least one display stays on.」など）と ja の翻訳 4 つ（tr.py check 0 problems）。編集中の draft は位置だけを保ち、flag などは snapshot の値に更新。 |
| probe | `keiland-system` に `display-shown KEY on\|off`。 |

確認（host、2026-10-08）:
- `sh plan/ws113/tests/host-displays.sh` PASS（off 行の読み書き、重複は 1 行、on に戻す、空の key、上限 16）。
- `sh plan/ws131/tests/host-system.sh` PASS（manager の版 19 の表、HDMI の off・on と flag、最後の on の拒否、接続していない key の ENODEV、空の key）。
- `sh plan/ws113/tests/host-output-switch.sh` PASS（`kwl_displays_anchor_follow` の stub を足した）、`sh plan/ws113/tests/host-plane.sh` PASS。
- build（warning 0）: zedBSD（wayland・settings・keiland-system、main の merge の後も）、Linux（`keiland-linux.mk all`）。Settings の page-display.c は gnu89 の host compile も通る（settings-render の link の失敗は以前からの WS089 の backlog）。style-check: 変えた file で増えない。

未実施:
- QEMU（T1 `plan/ws113/tests/displays-p014.sh`、image は `config-amd64-p006.mk`）: head 1 の off・on、anchor の off で head 1 へ移る、最後の on の拒否、displays.conf の保存と再起動、mirror での表示と拒否。
- 5330 の実機（ユーザー）: 内蔵を off にして HDMI だけ、HDMI を off にして内蔵だけ、touch の試験。FreeBSD の build。

## T1-370 の判定（2026-10-08 Q1）

試験の期待の誤りだけで FAIL（retry も同じ）: display 0 は Venus でも最初の display として KL_DISPLAY_INTERNAL（0x1）を持つ（最初の snapshot が flags=0x7）ので、off の後 0x21・on に戻して 0x5 が正しい。他の項目（head 1 の off・on、anchor が display 1（0x6）へ、最後の on の off が EINVAL、displays.conf の off= が残る、mirror の拒否、拡張に戻って off のまま、KWL FAILED 無し）は全部 ok → Q1 が試験の 90・97 行の期待を 0x21・0x5 に直した。機能は QEMU で PASS とみなし、test-wait（5330 の実機の UAT）。
