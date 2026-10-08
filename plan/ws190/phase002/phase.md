<!-- awesome-plan project=zedbsd record=ws190-p002 -->
# ws190-p002: libkeiland の欄と text area の指の選択（handle・編集の bar）

Status: in-progress（実装・host 試験は済み、T1 の AAT の依頼を Q1 へ。T1 の結果の後に cleared の判定）
Disposition: normal
Parent: [WS190](../ws.md)
Queue: q899（P1、2026-10-08）
Design: [p001](../phase001/phase.md) 第 2 版 §1・§2・§4（Q1 の判断 Q1 = Mailer・Phone・Calendar を結ぶ、Q2 = Settings・Files は Future Work）

## 範囲

p001 の §2（libkeiland）、§1.5 の app の結び（Mailer・Phone・Calendar、Notes の box の opt-out）、§4.1 の host 試験、§4.3 の欄の AAT のシナリオ。Text Editor は p003。

## 実装（commit、agent/p1）

- 先行（p001 の review の前、統合済み）: aat-input と aat の touch の命令（b7338ec54）、kuidemo の Notes の text area・選択の log・`kl_ui_window_text`（0ec34f32c）、試験の image の config `plan/ws190/tests/config-amd64-aat-touch.mk`（c3fc3389b）。
- 782ae4fb9: `kl_text_touch` の `bar`（末尾の追加）と規則、`kl_text_touch_select`・`_hide_bar`・`_toggle_bar`、内部の `keiui_text_touch_hold`、`KL_TEXT_TOUCH_BAR`。新しい `ui/text-bar.c`（`kl_text_bar_buttons`・`_layout`・`_hit`・`_draw`、§1.2〜§1.4）。`KEIUI_KEEP_FOCUS`（bar の press は focus を外さない）・`KEIUI_NO_DRAG`（bar の上の drag は何もしない）。KL_VERSION 74（仮）、exports.map。host 試験 `plan/ws190/tests/host-text-bar.c`。
- 1f2b79b3c: 欄と text area の指の選択（§2.3）。kl_ui が `struct keiui_select`（internal.h）を 1 つ持ち、view は kl_ui の写し（欄・area の struct と area の行）だけを使う。`UI_KIND_HANDLE` の記録（knob の届く円、pointer・wheel・axis は飛ばす、tap・long press は何もしない、1 本指は端の drag、2 本指は下の region の scroll）、`kl_ui_end` の始めに handle と bar の記録と描画（後の modal・覆う widget の frame は無し）、bar の命令は key の loop の後に owner への Ctrl+X/C/V/A（`from_bar`、app に渡さない）。mode を出る: 欄の外の tap、key・commit・preedit、focus の移動、窓の keyboard の focus を失う、欄が描かれない、app が text を変えた。`kl_ui_set_text_bar`。語は新しい `ui/text-select.c` の `keiui_select_word`（§2.4）。ui.c は bar の描画を `keiui_bar_calls`（text-bar.c の表）を通して呼び、text-bar.c と text.c に link しない（ui.c だけを使う host 試験のため）。field.c・text-area.c の click・input・描画に組み込み（§2.3 の「欄の 1 frame」）。host 試験 `plan/ws190/tests/host-touch-select.c`。
- 8eebf9716: Mailer・Phone・Calendar の frame の text input を `kl_ui_window_text` に（clipboard を結ぶ）、Notes の box は `kl_ui_set_text_bar(ui, NULL, 0)`、`kl_window_can_paste` は compositor の clipboard が無い時に自分の copy を見る（p001 事実 6）。
- 既存の host 試験の追従（ui.c・field.c が新しい file を要る）: field.c を compile する試験の script に `text-bar.c`・`text-select.c` を足した（ws177 host-text-edit・host-mail-n2、ws090 host-widgets、ws120・ws155・ws157・ws169・ws170・ws179 の host、tools/keiui/host-chooser・tools/files/host-build、ws089 host-build）。`plan/ws177/tests/host-field-limit.c` に選択の stand-in を足した。
- シナリオ `tests/scenarios/desktop/widgets/touch-select.md`（active）。suite の full に `desktop.widgets.*` を足すのは Q1 に依頼。

## 確認（2026-10-08、P1）

- `sh plan/ws190/tests/run-host.sh`: host-text-bar 47/47、host-touch-select 43/43、plain と ASan/UBSan で PASS（frame の後に欄の struct を free しても handle の drag が触らないことを含む）。
- 既存の host 試験: ws177 host-text-edit・host-field-limit、ws090 host-input 96/96・host-widgets 94/94、tools/textedit host-core 58/58、ws102 host-inset、tools/keiui host-chooser 85/85、ws169 mailer・ws170 phone・ws155 calendar・ws120 music・ws157 photos・ws179 accent-widgets・tools/files host-build は rc 0。ws177 host-mail-n2（`kl_drop_frame` の未定義）と ws089 host-build（settings/page-bluetooth.c の snprintf の警告）は**この変更の前の main でも失敗**（未対応、担当の WS の物）。
- build: amd64（config-amd64-aat、BUILD=build/ws190）の libkeiland.so・kuidemo・textedit・mailer・phone・calendar・notes・aat-input、Linux の keiland all で warning 0。`plan/tools/keiland-os-boundary/check.sh` PASS。style-check の新しい指摘 0。`check-scenarios.py` PASS（124）。
- QEMU・実機は未実施（T1 に依頼）。
