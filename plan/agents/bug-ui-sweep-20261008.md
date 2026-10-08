# UI の Bug の AAT による再現の一斉確認（q911、2026-10-08 夜、P2）

ユーザー（2026-10-08 夜）:「UI関連のバグは、なんだかほとんど片付いているのに残っている気がします。まとめてAATを流して、再現できなかったらcloseしたいです。」

- 範囲: [Bug Board](../known-bugs.md) の open（resolved・duplicate でない）の UI の Bug（compositor・Settings・Files・Phone・Mail・Notes・Text Editor・widget・title bar・App Home・Wiseview・画面 keyboard・IME・font など）。各 ticket の再現の手順・期待・最新の節（2026-10-08 の q869・q867・q868 の確認を含む）を読んで分けた。
- この文書は分類と T1 への依頼。**Bug の Status は変えていない**（判定は T1 の結果の後に Q1 とユーザー）。
- 作った物: シナリオ `tests/scenarios/bugs/*.md`（22、status active）、helper `plan/tools/aat/scenarios/helpers_bugs.py`、suite `tests/suites/bugs-ui.suite`（既存 10 ＋ bugs 22 = 32）、image の config `plan/tools/aat/config-amd64-aat-bugs.mk`（AAT の image ＋ `network-probe`・`glxtest`）、`plan/tools/aat/build-image.sh` の `AAT_CONFIG` の上書き。
- 確認（host）: `python3 plan/tools/aat/check-scenarios.py` PASS（148、suite bugs-ui 32）、`helpers_bugs.py --list` 22、`sh plan/tools/aat/tests/run-host.sh` aat-host: PASS。QEMU は未実施（T1）。

## 分類

(A) QEMU の AAT で再現を試せる、(B) 実機の device が要る、(C) 要望・後の版と決まった物で再現の対象でない。

### (A) QEMU の AAT（suite bugs-ui）

| Bug | シナリオ | 理由（1 行） |
| --- | --- | --- |
| [BUG-170](../bugs/BUG-170.md) | `desktop.bar.volume-slider`・`apps.settings.sound-page`（既存） | slider の drag を pointer で出し、確認の音の数（≤ 1）と drag の後の撮影の速さを log で見られる（QEMU に音の device を付ける） |
| [BUG-172](../bugs/BUG-172.md)・[BUG-191](../bugs/BUG-191.md) | `bugs.key-repeat-rate` | Settings の rate を 5・40 にして a を 3 秒押し続けた文字の数で、設定が app の repeat に効くかを数える（PS/2 の typematic と安定の体感は B） |
| [BUG-173](../bugs/BUG-173.md) | `apps.terminal.japanese-history`（既存） | Terminal の sh の履歴の長い日本語の行を撮る（sh の行の編集。UI ではないが既存の helper で安い） |
| [BUG-179](../bugs/BUG-179.md) | `desktop.windows.maximize-double-click`（既存） | double click から dock と描き直しの after_ms を log で測る（QEMU は Venus の swapchain が遅い、T1-146） |
| [BUG-180](../bugs/BUG-180.md) | `desktop.windows.unmaximize-drag`（既存） | docked の窓を bar から下へ引く途中と後を撮る（一度最大に戻らないか） |
| [BUG-182](../bugs/BUG-182.md) | `bugs.browser-tap-button` | touch screen の tap で HTML の button の onclick が走るか（console の行） |
| [BUG-184](../bugs/BUG-184.md)・[BUG-188](../bugs/BUG-188.md) | `bugs.settings-wifi-click-tap` | network-probe（模擬の Wi-Fi）で on→off の click と、AP の行の 1 回の touch の tap の join を log で見る |
| [BUG-203](../bugs/BUG-203.md)・[BUG-204](../bugs/BUG-204.md) | `bugs.phone-ime-flick` | Phone の欄に IME の「日本」と flick の「が」を入れて送り、PHONE SEND と file で見る |
| [BUG-205](../bugs/BUG-205.md) | `bugs.bold-text` | Settings・Phone・Files（英語と日本語）の太字を撮る（目視） |
| [BUG-206](../bugs/BUG-206.md) | `bugs.browser-url-scheme` | URL の欄の文字の長さ（`KWL TITLEBAR text … length`）で file:// が付くかを見る（https は network、無ければ data:） |
| [BUG-207](../bugs/BUG-207.md) | `bugs.browser-busy-scroll` | https の頁の読み込み中の wheel で scroll の frame が出るか（guest から Internet が要る） |
| [BUG-208](../bugs/BUG-208.md) | `bugs.docked-f11` | dock した Terminal の F11 の全画面の configure と、戻りの docked=1 |
| [BUG-209](../bugs/BUG-209.md) | `bugs.alt-tab-order` | Alt+Tab の開く app と Tab の行き先を bar の並びと比べる、素早い Alt+Tab は開いたまま |
| [BUG-214](../bugs/BUG-214.md) | `bugs.opacity-frosted` | slider の初期値 100 と Frosted glass の switch の off・on の panels=opaque・glass |
| [BUG-217](../bugs/BUG-217.md) | `bugs.layout-session-switch` | open-docked、windowed での切り替えで float、docked での切り替えで dock |
| [BUG-218](../bugs/BUG-218.md) | `bugs.phone-padding` | 前半（Phone の padding）を Settings と並べて撮る（目視）。後半（touchpad の慣性の開始の遅れ）は B |
| [BUG-219](../bugs/BUG-219.md) | `apps.files.open-from-home`・`apps.phone.open-from-home`（既存） | open.png の title bar の menu の項の薄い下線を見る（目視） |
| [BUG-229](../bugs/BUG-229.md) | `bugs.osk-home-restore` | App Home の後の `KWL OSK restore kind=flick` |
| [BUG-230](../bugs/BUG-230.md) | `bugs.osk-pull-hint` | 右下の角を pointer で押して引いた hint を 3 段で撮る（目視） |
| [BUG-231](../bugs/BUG-231.md) | `bugs.osk-qwerty-ime` | QWERTY の panel の a・space・Enter が `send via=ime` で、file に日本語 |
| [BUG-232](../bugs/BUG-232.md) | `desktop.home.switch-running`（既存） | 起動中の app を App Home で選ぶと switch で launch が無い |
| [BUG-233](../bugs/BUG-233.md)・[BUG-234](../bugs/BUG-234.md) | `bugs.files-open-programs` | desktop の icon の double click で x の bit の動画は Video Player、/bin/ls の写しは残る Terminal |
| [BUG-235](../bugs/BUG-235.md) | `desktop.home.power-off-dialog`・`desktop.session.logout-login`（既存） | App Home の Power Off の確認の dialog と、そこからの Log Out・login（Restart は session を終えるので流さない） |
| [BUG-237](../bugs/BUG-237.md) | `bugs.icon-holes` | App Home・Alt+Tab・dark の App Home の icon の記号を撮る（目視） |
| [BUG-242](../bugs/BUG-242.md) | `bugs.emacs-shell-ls` | Terminal の `emacs -nw` の M-x shell で `ls /` を撮る（目視） |
| [BUG-259](../bugs/BUG-259.md) | `bugs.pdf-resize-drag` | 角の drag の間の page 1 の raster の回数（≤ 4）と RESIZE settled |
| [BUG-265](../bugs/BUG-265.md) | `bugs.title-tap-drag` | 浮いた title bar の tap and drag（mouse と touch screen）で dock しないで動く |
| [BUG-270](../bugs/BUG-270.md) | `bugs.touch-top-edge-wiseview` | touch screen の上端からの 1 本指の swipe で Wiseview、bar の tap は今どおり |
| [BUG-273](../bugs/BUG-273.md) | `bugs.x11-docked-close` | dock した glxtest を bar の close（aatlib.close と同じ y=22）で閉じ、close の行と glxtest の終わりを分けて見る |

### (B) 実機の device が要る

| Bug | 理由（1 行） |
| --- | --- |
| [BUG-156](../bugs/BUG-156.md) | touchpad の 2 本指の scroll（aat は touchpad の指を出せない） |
| [BUG-166](../bugs/BUG-166.md) | touchpad の押し込み・tap-drag での title bar の移動 |
| [BUG-167](../bugs/BUG-167.md) | touchpad の物理の押し込み |
| [BUG-178](../bugs/BUG-178.md) | touchpad の tap-drag の 2 回目の置き直しの位置（touch screen の経路は bugs.title-tap-drag で一部） |
| [BUG-190](../bugs/BUG-190.md) | touchpad の tap の押下の規則（ユーザーの決定どおりかを UAT） |
| [BUG-211](../bugs/BUG-211.md) | touchpad の 2 本指の慣性（axis_source=finger、aat の wheel では出ない） |
| [BUG-215](../bugs/BUG-215.md)・[BUG-216](../bugs/BUG-216.md)・[BUG-228](../bugs/BUG-228.md) | touchpad の 3 本指・2 本指の gesture（216 の「Wiseview」の題が無いことは既存の desktop.wiseview.super-tab の撮影で見られる） |
| [BUG-247](../bugs/BUG-247.md)・[BUG-254](../bugs/BUG-254.md) | 5320・5330 の touchpad の tap の間隔・2 本指の着く時間差 |
| [BUG-218](../bugs/BUG-218.md)（後半） | touchpad の 2 本指の scroll の開始の遅れ |
| [BUG-169](../bugs/BUG-169.md)・[BUG-213](../bugs/BUG-213.md)・[BUG-222](../bugs/BUG-222.md) | USB の有線 LAN（ue0）の抜き差し・link の速度 |
| [BUG-157](../bugs/BUG-157.md)・[BUG-174](../bugs/BUG-174.md)・[BUG-183](../bugs/BUG-183.md)・[BUG-185](../bugs/BUG-185.md)・[BUG-187](../bugs/BUG-187.md)・[BUG-189](../bugs/BUG-189.md)・[BUG-212](../bugs/BUG-212.md) | 実の Wi-Fi の radio（AX211）と networkd の振る舞い。表示の模擬は T1-151 で済み |
| [BUG-176](../bugs/BUG-176.md) | 起動の直後の Wi-Fi の自動の接続の間の bar の表示（QEMU に Wi-Fi が無く networkd がすぐ答える） |
| [BUG-193](../bugs/BUG-193.md) | session の前の起動の logo の animation の速さ（実機の時計と表示） |
| [BUG-221](../bugs/BUG-221.md)・[BUG-226](../bugs/BUG-226.md) | 描画の遅れ。QEMU の Venus では present が律速（T1-400）、判断は 5330 の native の経路の測定 |
| [BUG-239](../bugs/BUG-239.md) | 死んだ client の解放の遅さは QEMU の Venus に固有（T1-399）、判断は 5330 の測定 |
| [BUG-252](../bugs/BUG-252.md) | i915 の frame の頼み方。Venus では client の commit で frame が回り隠れる |
| [BUG-223](../bugs/BUG-223.md) | 直の scanout（direct=1）は i915。QEMU は reason=backend（T1-244） |
| [BUG-255](../bugs/BUG-255.md)・[BUG-159](../bugs/BUG-159.md) | 蓋と HDMI、電池 |
| [BUG-058](../bugs/BUG-058.md)・[BUG-085](../bugs/BUG-085.md)・[BUG-120](../bugs/BUG-120.md) | i915 の実機・passthrough の間欠・GPU の枠 |

### (C) 要望・後の版と決まった物（再現の対象でない）

| Bug | 理由（1 行） |
| --- | --- |
| [BUG-220](../bugs/BUG-220.md) | 列の幅の drag の要望（実装済み q798・q803）。受け入れは使い心地（UAT）。必要なら AAT で列の端の drag と `ZFILES COLUMN saved` を見る scenario を足せる |
| [BUG-236](../bugs/BUG-236.md) | App Home の見た目の要望（案 A 実装済み、ユーザーの目の判断） |
| [BUG-227](../bugs/BUG-227.md) | amazon の WAF の challenge。ユーザーの決定でベータ2 の後（WS074 の新しい Phase）。再現は確実 |
| [BUG-241](../bugs/BUG-241.md) | Hebrew・Thai などの font と shaping。ユーザーの決定でベータ3。再現は確実 |

### 対象外（UI でない）と Board の古い行

- UI でない open: BUG-013・023・024・025（PC-98・LX6、ベータ4）、036・041（QEMU の USB・NVMe）、052（Q2）、095・105・119・249（電源・USB の mouse）、145（AX211 の DHCP）、165・195・196（ACPI）、199（host 試験）、238（emacs -nw、既存の `apps.emacs.edit-save` が見る）、251・272（起動の Ctrl+Shift）、258・269（USB・ESP）、261（GPIO）、263（AF_UNIX）、271（印刷）。
- Board の行が ticket より古い（Q1 が直す）: BUG-246（ticket は resolved、5320 でユーザーが確認）、BUG-248（resolved、T1-360）、BUG-099（resolved、2026-10-04 close）、「優先度と依存」の表の BUG-135・143・144（各 ticket は resolved）。BUG-203・204 の行の「Mailer・Calendar は未実施」も古い（T1-260・T1-317 で確認、ticket の q869）。
  → 2026-10-08 Q1 の依頼で P2 が Board の行を ticket に合わせて直した（BUG-246・248・099・036・041 の行、優先度の表の 158・135・143・052・095・041・144/124・036/033/027/103、BUG-203・204 の行）。

## T1 への依頼（1 回の QEMU）

- **image**: 依頼の tree（この commit を Q1 が main に統合した後の main の SHA）で、`AAT_CONFIG=plan/tools/aat/config-amd64-aat-bugs.mk plan/tools/aat/build-image.sh build/t1-bugsweep-img`（AAT の image ＋ `network-probe`・`glxtest`。2026-10-08 の規則どおり使い回さず作る。結果に image の tree の SHA）。
- **起動**: 標準の AAT の QEMU の起動（T1-450 と同じ）に、音の device（`-audiodev none,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0`、BUG-170）と、guest から Internet に届く user-net（BUG-206・207 の https）。
- **流し方**: `plan/tools/aat/run-aat.sh qemu build/t1-bugsweep/out bugs-ui --record build/t1-bugsweep/summary.md`（32 シナリオ、見積もり 40〜50 分。長い時は同じ起動のまま `bugs-ui` の前半の既存 10 個と `'bugs.*'` の 2 回に分けてよい。`desktop.session.logout-login` は session を作り直すので最後）。
- **返す物**: `summary.md`・`verdicts.tsv`・`records/`・`logs/`・`png/` の場所、image の SHA、各シナリオの verdict と note。FAIL の解析はしない（Q1・P2）。

### 各 Bug の判定の読み方

verdict は runner の物（pass・fail・needs-person）。「再現した」は fail の note が `BUG-NNN reproduced` で始まる物。それ以外の fail（`aat …` の error、窓が出ない、image に program が無い）は**試験の失敗**で、再現とは数えない（Q1 に返す）。

| シナリオ | 再現しなかった | 再現した |
| --- | --- | --- |
| `bugs.browser-tap-button`（182） | pass | fail「BUG-182 reproduced: the tap did not press …」（mouse の click も効かない時は頁の問題、判定しない） |
| `bugs.settings-wifi-click-tap`（184・188） | pass | fail「BUG-184 reproduced」（switch が動かない）・「BUG-188 reproduced」（1 回の tap で join の要求が無い）。network-probe が無ければ needs-person |
| `bugs.phone-ime-flick`（203・204） | pass | fail「BUG-203 reproduced」・「BUG-204 reproduced」 |
| `bugs.bold-text`（205） | needs-person: 撮影 settings・phone・files・settings-ja の太字をユーザーが見る | 同左（見た目の判断） |
| `bugs.browser-url-scheme`（206） | pass（note に確かめた URL） | fail「BUG-206 reproduced: the field held N characters …」。「no page with a scheme could be opened」は試験の失敗 |
| `bugs.browser-busy-scroll`（207） | pass（読み込み中に scroll の frame） | fail「BUG-207 reproduced」。needs-person「the page did not come」は network 無しで判定なし |
| `bugs.docked-f11`（208） | pass | fail「BUG-208 reproduced」 |
| `bugs.alt-tab-order`（209） | pass | fail「BUG-209 reproduced」 |
| `bugs.opacity-frosted`（214） | needs-person で、log の check が全部通り、page.png の slider が 100%、opaque.png で透けない | fail（switch が効かない）、または撮影で 100% なのに透ける |
| `bugs.layout-session-switch`（217） | pass | fail「BUG-217 reproduced」 |
| `bugs.phone-padding`（218 前半） | needs-person: phone.png と settings.png の本体の端と title bar との隙間が同じ | 同左 |
| `bugs.osk-home-restore`（229） | pass | fail「BUG-229 reproduced」 |
| `bugs.osk-pull-hint`（230） | needs-person: hint の 3 枚が白い四角でなく扇形と「Keyboard」 | 同左（白い四角なら再現） |
| `bugs.osk-qwerty-ime`（231） | pass | fail「BUG-231 reproduced」 |
| `bugs.files-open-programs`（233・234） | needs-person で、check が全部通る（video.png で再生、ls.png で出力と終了の案内） | fail「BUG-233 reproduced」・「BUG-234 reproduced」 |
| `bugs.icon-holes`（237） | needs-person: 記号が白でなく壁紙・景色を見せる | 同左 |
| `bugs.emacs-shell-ls`（242） | needs-person: shell-ls.png の ls が揃った列 | 同左（崩れていれば再現） |
| `bugs.pdf-resize-drag`（259） | pass（page 1 の raster ≤ 4） | fail「BUG-259 reproduced: page 1 drawn N times」 |
| `bugs.title-tap-drag`（265） | pass | fail「BUG-265 reproduced (the mouse / a finger)」 |
| `bugs.touch-top-edge-wiseview`（270） | pass | fail「BUG-270 reproduced」 |
| `bugs.x11-docked-close`（273） | pass | fail「BUG-273 reproduced: the press did not reach the close button」（試験の座標・bar の当たり）か「… glxtest did not end」（X の窓の close の経路）。glxtest が無ければ needs-person |
| `bugs.key-repeat-rate`（172・191） | pass（5/s で 6〜22、40/s で 70〜130）か needs-person（40/s が頭打ち、値を記録） | fail「BUG-191 reproduced」 |
| `desktop.bar.volume-slider`・`apps.settings.sound-page`（170） | pass（feedback ≤ 1、drag の後の撮影 < 5 s） | fail「… feedback sounds (BUG-170)」か「the screen took N s」。sound=0 の needs-person は音の device 無しで判定なし |
| `apps.terminal.japanese-history`（173） | needs-person: history.png で prompt が残り行が崩れない | 同左 |
| `desktop.windows.maximize-double-click`（179） | pass（after_ms ≤ 200）。QEMU で needs-person（after_ms > 200）の時は compositor の dock の行が 2 回目の press と同時なら再現でない（T1-146 の結論、client の描き直しは Venus） | dock の行が無い・遅い |
| `desktop.windows.unmaximize-drag`（180） | needs-person: during.png・after.png で窓が一度最大に戻らない | 同左 |
| `apps.files.open-from-home`・`apps.phone.open-from-home`（219） | pass で、open.png の title bar の menu の項に薄い下線 | 下線が無い |
| `desktop.home.switch-running`（232） | needs-person で check が通る（switch、launch 0・map 0） | fail「a second Files was started」など |
| `desktop.home.power-off-dialog`・`desktop.session.logout-login`（235） | dialog: needs-person で check が通る（Esc と外の click で cancel、確認なしに終わらない）。logout-login: pass | fail「the session ended without asking」 |

## 注記

- 2 つの helper（settings-wifi、x11）は bugs の config の image が要る。標準の AAT の image では needs-person（理由つき）になり、他は動く。
- `bugs.*` は full の suite に入れていない（bugs の config が要る物があるため）。Bug を close した後も回帰に残す物は Q1 が `tests/scenarios/` の分野へ移して full に入れる（2026-10-06 の試験の整理の基準）。
- 見積もりは 1 シナリオ 1〜2 分。helper の座標は log の行から取る（aatlib の方針）。log の行の名前は今の main の source で確かめた（読み）。QEMU では未実行なので、helper の誤りでの fail はありうる（試験の失敗として P2 に返す）。

## 2026-10-08 夜の決定（T1-481 の前、Q1 の依頼で P2 が Board と ticket に記録）

- ユーザー「BUG-220, BUG-236,BUG-227,  BUG-241,はcomplete.」→ (C) の 4 つは resolved。
- ユーザー「Touchpadは動作確認できたので、下記をすべてclose します。…」→ (B) の touchpad の BUG-156・166・167・178・190・211・215・216・228・247・254 は resolved。BUG-218 は touchpad の部分だけ閉じ、Phone の padding（bugs.phone-padding）は T1-481 の結果待ち。
- BUG-253 は Board の resolved を scheduled（実機の確認待ち）に戻した（BUG-255 の診断の UAT では蓋の事象が届かない起動があった）。
