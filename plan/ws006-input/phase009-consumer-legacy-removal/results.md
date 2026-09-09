# ws006-p009 q126 results

Status: uncleared
Date: 2026-09-09

## Implementation

Retired public event/mode/key-state/drain UAPI and unused internal poll/read helpers.
Removed event owner, records, legacy queue and early event-reader keymap. The HAL input producer,
per-source subscriber state, dispatch worker and TTY discipline remain; read/poll use TTY only.
No current kernel shell calls the removed helpers. Xzed and the selected Noct BeUI source use evdev.

## Evidence so far

- IN-T50 audit: 1185 current C/header files, no retired symbols; surviving ISATTY=13 preserved.
- Input ownership/device/subscriber/console/PCAT/PC98/X68k host gates ordinary+ASan+UBSan PASS.
  Old p031 extraction markers no longer exist in input.c; fixtures now link the current complete
  source. Weak fail-fast sentinels satisfy ASan-retained unused cdev tables; any unexpected use aborts.
- amd64, PCAT, PC98 make -j16 PASS (explicit config/ci selections). The first default build used
  the user's current PC98 config; its log was renamed to avoid misreporting it as amd64.
- Initial IN-T12 guest failed the new retired-ioctl assertion because the test assumed ENOTTY.
  Actual TTY default returns EOPNOTSUPP; the fixture now checks that ordinary result and unchanged
  caller buffers. Production error behavior was not changed to fit the fixture.
- Final IN-T12/IN-T50 guest PASS: six retired numeric ioctls rejected, geometry/termios/isatty intact,
  login and shell commands work, evdev keyboard/relative-pointer capability boundaries pass.
  Evidence: ../temp/q126-console-evdev-final/ (relative to WS root temp directory).

USB HID and desktop consumer runtime gates remain in progress. No commit or aggregate make check.

## q126 最終検証と未クリア境界

- USB HID host: `/tmp/zedbsd-q126-hid-host.log`。production統合ソースを直接使用するようfixtureを更新。通常/ASan+UBSan各92checks、xHCI hot-unplug lifecycle、GCC analyzerともPASS。
- Xzed: `/tmp/zedbsd-q126-xzed-host.log`。実consumerの能力探索・入力・切断・再同期host fixtureとASan/UBSan、旧APIなしaudit PASS。
- Noct/BeUI: `../temp/q126-beui/`、NOCT-T011/T012/T013 QEMU PASS。現行選択Noctの画像・入力consumerを確認。
- USB HID QEMU: `../temp/q126-usb-hid/results.tsv`。xHCIはkeyboard console、keyboard/relative/absolute evdev records、hotplugとstale fd、64MiB USB root read同時進行PASS。
- 同じcampaignのEHCI/UHCI paired cellはFAIL。`paired/guest.log`末尾で `usb0: port 1 enumeration failed (13)`、USB Storage root registrationが180秒timeout。login/input probeに到達せず、原因は未確定。旧console実装とのA/B比較は未実施なので、この変更と無関係とは断定しない。
- 今回Xzedの新たなGUI runtime campaignは未実施。host consumer検証と過去の移行証拠をGUI実行の代用にはしない。

旧UAPI撤去の実装と上記通過範囲を保持するが、全受け入れ達成とは扱わない。
p009は**uncleared**、WS006は未完了。再開時はpaired topologyのUSB root列挙を旧consoleとの比較で切り分け、必要な修正を独立Phaseへ設計し、pairedとXzed GUIを再検証する。既存の実機観測は履歴のまま保持し、今回新たな実機PASSは加算しない。
ユーザーの指示に従い、このQueueを閉じて次のPriority WS022へ進む。

通常amd64イメージ復元: `make -j16 ZEDBSD_CONFIG=config/ci/config-amd64.mk` PASS（`/tmp/zedbsd-q126-amd64-restored.log`）。BeUI専用payloadのビルドから通常成果物へ戻した。
