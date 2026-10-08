<!-- awesome-plan project=zedbsd record=ws079-p012 -->

# ws079-p012: kernel の multitouch（USB HID の touch screen の指 → evdev の protocol B）と注入の device の touch

<!-- awesome-plan-current:start -->
Status: cleared（2026-10-08 q910 P2 の照合: ws.md の表の 2026-09-28 main の判断（QEMU と host）に合わせた。実機の touch の LCD は未実施）（旧: in-progress（2026-09-28 の区切り: 実装・host 試験・amd64 の build・QEMU の guest の確認まで。実機（touch LCD）は未着で未実施。clearance は main の判断））
Disposition: normal
Parent: [WS079](../ws.md)
Queue: main の指示（Kei desktop subagent、2026-09-28）。Awesome Plan の Queue の item ではない
Resume point: 下の「残り」
<!-- awesome-plan-current:end -->

## 範囲

- ユーザー（2026-09-28）:「ウィンドウのフローティングタイトルバーを二本指で…」→ 二本指の短い上へのこすり（p013）の前提として、
  kernel が USB HID の touch screen の指を読む。main の依頼: Touch Screen（0x0D:0x04）の Finger（0x0D:0x22）の collection の Contact Identifier・
  Tip Switch・Confidence・X/Y と Contact Count（1 つの report が frame の一部の指だけを運ぶ hybrid も）を、evdev の multitouch protocol B
  （`ABS_MT_SLOT`・`ABS_MT_TRACKING_ID`・`ABS_MT_POSITION_X/Y`・`BTN_TOUCH`、最初の指の `ABS_X/Y`）へ。論理は host で試験できる
  `hid-digitizer.c` と同じ形の file に、合成の 10 点の touch screen の descriptor の host 試験、注入の device（`/dev/input-inject`）の TOUCH の種類、
  複数の指の台本、amd64 の kernel の warning 0、pen の試験の image での 2 本指の台本の evdev の読み取り。
- HAL に触れない（input の層と USB HID の driver だけ）。compositor（wl_touch）は p013。

## 実装（2026-09-28）

| 場所 | 内容 |
| --- | --- |
| `include/drivers/usb/hid-touch.h`・`src/drivers/usb/hid-touch.c`（新） | touch の状態機械（host で試験できる純粋な関数）。`drv_hid_touch_describe`（capability 8: SYN・`BTN_TOUCH`・`ABS_X/Y`・`ABS_MT_SLOT`・`ABS_MT_TRACKING_ID`・`ABS_MT_POSITION_X/Y`。軸 6: 位置は指の X/Y の範囲と resolution、slot は report の指の数、Contact Count があれば最低 10、最大 16、tracking 0..65535）、`drv_hid_touch_reset`、`drv_hid_touch_report_is_touch`、`drv_hid_touch_translate`。frame: Contact Count が 0 でない report が frame を始め、0 の report が続き（hybrid）、最後の指が来たら書く。count の後ろの entry は使わない。count の無い screen は 1 report = 1 frame。frame の終わりに触れていると報告されなかった指は離す（Tip Switch 0、Confidence 0（掌）、報告されない）。離した slot はその frame の間は新しい指に渡さない（読み手が離すのを先に聞く）。新しい指は空いている最小の slot と次の tracking id（65535 の後は 0）。書く順: slot 順に（離す: `ABS_MT_TRACKING_ID -1`、新しい: id・X・Y、動いた: 変わった座標）、必要なときだけ `ABS_MT_SLOT`、`BTN_TOUCH` の変化、最も古い指の `ABS_X/Y`（変わったときだけ）、何か書いたら `SYN_REPORT`。report が失われて count の途中で次の count が来たら、開いていた frame を離す処理なしで書いてから始める |
| `include/drivers/usb/hid-report.h` | 値の擬似の型 `HID_REPORT_TYPE_TOUCH`（0x8001、code は指と項目 `HID_TOUCH_CODE(finger, item)`、Contact Count は `HID_TOUCH_CONTACT_COUNT_CODE`）、`hid_report_layout_info.touch_contacts`、`struct hid_report_touch_info`、`drv_hid_report_layout_get_touch` |
| `src/drivers/usb/usb-hid.c` | parser: Touch Screen の application collection の中の field は touch の状態機械だけへ（`add_touch_field`）。Finger の collection は開いた後の最初の field でその report の中の番号（0 から、最大 16、それ以上は無視）を得る。Tip Switch・Confidence・Contact Identifier・X・Y と Contact Count（指の外）を `HID_FIELD_TOUCH` の field にし、layout の capability と軸には入れない（最初の指の X/Y の範囲と Physical/Unit の resolution を別に持つ）。Scan Time などは無視。以前は指の X/Y が `ABS_X/Y` に写って 2 本目で重複の EINVAL になり、touch screen の descriptor は丸ごと拒否されていた。device: touch screen があれば **別の input device**（名前「<製品名> Touchscreen」か「USB HID touchscreen」、物理 path に `/touch`）として登録し、touch の report はそちらへ。interface が touch screen だけ（自分の capability が EV_SYN だけ）なら本来の device は登録しない。pen と touch が同じ interface にある機種（D1 の AES の LCD はこの形の見込み）は pen の device と touch の device の 2 つになる |
| `platform/{amd64,arm64,pcat}/vmunix.mk` | `hid-touch.c` を `usb-hid.c` と同じ条件で。amd64 は注入の device（`CONFIG_INPUT_TEST_INJECT=y`）が USB HID 無しでも `hid-touch.c` を入れる |
| `include/uapi/input-inject.h`・`src/drivers/generic/input-inject.c`・`config/kernel-options.list` | `INPUT_INJECT_KIND_TOUCH`。setup に `report_contacts`（1 report の指の数、1..10。pen は 0）と `reserved`（0）を足した。touch は「Test touchscreen (input-inject)」（10 slot、範囲 0..x_max・0..y_max）。以後の write は `struct input_inject_touch_frame`（count ≤ 10、指ごとに contact_id 0..255・tip 0/1・x・y）。kernel が frame を `report_contacts` 本ずつの report（最初の report に Contact Count、以後 0 ＝ hybrid）に分けて `drv_hid_touch_translate` に通して emit する（USB の touch screen と同じ状態機械を guest で通る。descriptor の解析は host 試験が覆う）。frame 全体を先に検査して拒否は EINVAL |
| `userland/base/tests/touchinject/`（新）・`plan/ws079/tests/config-amd64-pen.mk` | 試験の道具 `touchinject`（既定 n、pen の試験の image に追加）。台本 1 行 = 1 frame（`;` で指の命令を並べる）: `size W H [N]`・`down ID X Y`・`move ID X Y`・`up ID`・`swipe DX DY STEPS MS`（触れている指を全部）・`wait/hold MS`。`-d MS`（node を探して名前・6 軸・event を名前で印字）、`-c`（拒否の確認 14 件）。台本 `/usr/share/touchinject/two-fingers.touch` |
| `plan/ws079/tests/host-hid-touch.c`・`run-hid-touch.sh`・`p012-guest.sh`・`p012-two-fingers.expected`（新） | 試験（下） |

## 確認（実行したもの）

| 確認 | 結果 |
| --- | --- |
| `plan/ws079/tests/run-hid-touch.sh`（host。`usb-hid.c`・`hid-digitizer.c`・`hid-touch.c` を kernel と同じく freestanding・`-Werror` で compile。合成の descriptor を試験の中で組む） | `host-hid-touch: ok (193 checks)`。10 指（Tip・Confidence・Contact ID 0..127・X/Y 0..4095、216×135 mm、Contact Count、Scan Time、Contact Count Maximum の feature）: layout の capability は EV_SYN だけ、touch 10 指・count あり、X 19 / Y 30 単位/mm、describe の 8 capability・6 軸（slot 0..9、tracking 0..65535）。2 本指の台本: 最初の指 slot 0・tracking 0、2 本目 slot 1、報告された up で離す（`ABS_X/Y` は残った指へ）、変化の無い frame は 0 event、戻った指は空いた slot 0 と次の tracking、Confidence 0 で離す、最後の指で `BTN_TOUCH 0`、count 0 の frame で全部離す、count の後ろの古い entry を無視、10 本（離す指の slot はその frame で渡さない→9 本、次の frame で 10 本目）。hybrid（1 report 2 指 + count）: 3 本は 2 つ目の report で初めて書かれ、count の後ろの entry を無視、report の欠落（count の途中で次の count）は離さずに書く、報告されない指を離す、12 本は 8+2 本で slot を埋め 2 本は追わない。count も Contact ID も無い screen: 1 report = 1 frame、場所が指の名前、slot は 2。pen（report 2）と touch（report 1）が同じ interface: layout の capability は pen のもの（MT の軸無し、`ABS_X` は pen の 21600）、touch は自分の X 範囲、pen の report は pen、touch の report は touch に振り分け。pen だけの layout は ENOENT、範囲の無い describe は EINVAL、tracking 65535 の次は 0 |
| 同（`EXTRA_CFLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all"`、状態機械と試験に適用） | `ok (193 checks)` |
| 試験の感度（mutation、worktree の使い捨ての `build/mutate-touch.sh`、source の写しを変える） | 見えない指を離さない版 13 件 FAIL、`ABS_MT_SLOT` を毎回出す版 4 件 FAIL、count の後ろの entry を読む版 6 件 FAIL |
| `plan/ws079/tests/run-hid-pen.sh`（p002 の pen の host 試験、回帰） | `host-hid-pen: ok` |
| `make -j16 … build/amd64/vmunix`（`ZEDBSD_CONFIG=plan/ws079/tests/config-amd64-pen.mk`、`CONFIG_INPUT_TEST_INJECT=y`、`-Werror`） | rc=0、warning 0（`hid-touch.o`・`input-inject.o` を含む） |
| `make -j16 … build/ws079-p012-default/vmunix`（main の `config.mk` の写し、注入なし） | rc=0、warning 0。`hid-touch.c` は compile され、`input-inject.c` は入らない（map に `drv_input_inject_register` 無し） |
| `plan/ws079/tests/build-pen-image.sh build/amd64`（worktree の build、touchinject 入り） | exit 0。自分の code の warning 0（log の warning は openssl・noct の既存のもの） |
| QEMU（Venus の guest、`pen-guest.sh start build/amd64/hdd-image.img`）: [p012-guest.sh](../tests/p012-guest.sh) | **PASS**。`touchinject -c` 14/14 ok（setup: 指 0・11、reserved、指のある pen、未知の kind、pen の event、frame: 11 本、tip 2、id 256、画面の外、reserved が EINVAL。良い frame と指 0 の frame は通る）。2 本指の台本を `touchinject -d` で読んだ結果が [p012-two-fingers.expected](../tests/p012-two-fingers.expected) と完全一致（node `/dev/input/event4`「Test touchscreen (input-inject)」、6 軸、48 event: 2 本の down、2 frame の上への swipe、1 本の up で `ABS_X` が残った指へ、新しい指が空いた slot 0 で `ABS_MT_SLOT` 無しに tracking 2、3 本の frame（2 つの report に分かれた hybrid）で slot 2・tracking 3、3 本の同時の up で `BTN_TOUCH 0`）。pen の回帰: `peninject -c` 15/15、pen の台本 182 event（p003 と同じ）。compositor の下で touch screen が seat に入る（`ZWL INPUT device=/dev/input/event4 kind=pointer abs=1`、今は `ABS_X/Y` の絶対 pointer として。wl_touch は p013）、injector を閉じると `INPUT_CLOSED`、compositor は落ちない。出力 `build/ws079-p012/`（worktree） |
| 同じ guest（新しい kernel）で `plan/ws079/tests/zdesktop-p013.sh`（USB の `usb-tablet` の pointer の経路の回帰） | PASS |
| `plan/tools/boot-test.sh`（共有の `build/amd64/hdd-image.img`（09-26）の写しの ESP の `vmunix` を注入なしの kernel に替えた image） | PASS、login prompt（`build/ws079-p012-boot/out/login.png`、worktree）。userland は 09-26 のまま |
| `plan/tools/style-check.py`（hid-touch.c・hid-touch.h・変えた行の usb-hid.c と input-inject.c）、`git diff --check` | 指摘 0（input-inject.c の pen の部分と usb-hid.c の既存の行の指摘は以前からのもの）。clang-format は host に無く未実施 |

style の修正（空行と comment だけ）の後に kernel 2 つと host 試験 2 つをやり直した（warning 0、ok）。guest の試験はその前の同じ意味の build で行った。

## 設計からの差・判断

- 注入の touch は evdev の event ではなく「指の frame」を受け、kernel の中で USB と同じ状態機械に通す（pen の注入は evdev の event のまま）。
  guest で protocol B の組み立て（hybrid を含む）を通すためで、userland に同じ論理を二重に書かない。descriptor の解析は host 試験が覆う。
- touch screen は pen と別の input device にした（Linux の hid-multitouch と同じ分け方）。同じ device にすると `ABS_X` の範囲と `BTN_TOUCH` が pen と衝突する。
- Touch Pad（0x0D:0x05）は範囲外（touch screen だけ）。`INPUT_PROP_DIRECT` は input の層に property が無いので出していない（p002 の残り 3 と同じ）。
- `ABS_MT_TOUCH_MAJOR`（Width/Height）・`ABS_MT_TOOL_TYPE`・Contact Count Maximum（feature report）は読まない。slot の数は report の指の数から（count があれば最低 10）。
- 実機の USB の touch screen で 2 つ目の input device を登録する経路（`usb_hid_activate`）は QEMU に USB の multitouch の device が無いので動かしていない（build と既存の USB HID の device の回帰だけ）。

## 残り（resume の条件）

1. 実機: 10 インチの touch LCD（USB-C か HDMI + USB）と AES の pen が届いたら、report descriptor を読み（Input の flag に Null State などがあると今の parser は EOPNOTSUPP で丸ごと拒否する。vendor の array の field も同じ）、pen と touch の 2 つの device が出ること、protocol B の event を確かめる。
2. p013: compositor の `wl_touch`、touch の接触を `zwl_corner_contact_*` と Home・Wiseview の端へ、二本指の短い上へのこすりで窓を後ろへ（`window_lower()`）。
3. arm64・pcat の kernel の build は未実施（rule だけ足した）。
