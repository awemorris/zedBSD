# ベータ2 の残りの照合の後半（2026-10-08 夜、P2 q910）

前半の [sweep-beta2-rc-20261008.md](sweep-beta2-rc-20261008.md)（P1 q902、優先の 17 WS）が範囲の外にしたベータ2 の WS を照合した報告。base は main 8d314a170（agent/p2 に merge）。
照らした物: master の工数の表（「ベータ1」「ベータ2」の行、まだ completed でない物と、表に無いベータ2 の WS178〜WS182）、各 ws.md・phase.md、T1・T2 の台帳（T1-475 まで）、master の decisions-log。source は変えていない。

範囲から除いた物:
- 前半の 17 WS（WS113・051・050・083・090・156・183・161・157・155・143・177・187・188・189・190・191）。
- ベータ3 以降に回った物（decisions-log）: WS001・004・046・095 の続き・186（2026-10-07）、WS009・026・106・139（2026-10-08）、WS052・068・074・101・115・116・117・126・152・158・171・172・176・180・184・185。WS162 は WS172 に吸収。10/13 以降: WS129（release）と Linux・FreeBSD の作業（WS112、WS131 の 3 OS の回帰、各 WS の Linux・FreeBSD の build）。ベータ4: WS124・125 ほか。止めている物: WS153。全文規約の Phase は全部ベータ3（2026-10-08）。
- 要検討・ブロック・アイディアの WS（WS037・038・039・044・048・080・082・098・118・119・141・144・146・147・150）。
- WS031（Vulkan 実行器）と WS075（i915 の高度化）は「空き時間だけ」（master の Focus）なので、状態だけ見て残りの作業の数には入れない。

照合した WS（48）: WS014・029・031・033・049・066・073・075・078・079・081・084・089・094・099・100・102・120・121・122・127・128・130・131・132・134・142・145・148・149・151・154・159・160・164・165・166・167・168・169・170・173・174・175・178・179・181・182。

## 1. 直した記録

この Queue の範囲（ws.md の Status の行と Phase の表、phase.md の Status の行）だけを直した。旧い値は「旧: …」として残した。

- **ws.md の表と phase.md の食い違い（phase.md の方が新しい、Q1・main の判定済み）**を、表を phase.md に合わせて直した:
  ws014 p011、ws073 p041、ws078 p004（p007・p008 の cleared も書き足し）、ws099 p019・p034（p034b cleared）、ws102 p022、ws127 p002、ws131 p003、ws154 p001〜p004、ws164 p002、ws165 p005、ws166 p002・p003、ws168 p004、ws173 p004、ws179 p001〜p003、ws181 p002〜p005・p008・p010。ws033 p001（表 planned → phase.md の uncleared）。ws173 p001・p002 の表は「Q1 の判定待ち（T1-200 PASS）」にした。
- **phase.md の方が古かった物**（表に main・Q1 の判定があった）は phase.md の Status を表に合わせた: ws089-p015 → canceled（2026-10-05 夜、WS158 に吸収、WS158 はベータ3）、ws100-p008 → cleared（2026-09-30 Q1）、ws079-p012 → cleared（2026-09-28 main、実機は未実施）。
- **ws.md の Status の行**: 上の 48 WS 全部に「2026-10-08 q910 P2 の照合: …」として実態の 1 行を足した。ws127（planned）・ws131（planning）・ws159（planning）・ws164（planning）は Phase が実行済みなので incomplete に直した。
- Phase を新たに cleared にしたものは無い（全部 Q1 の既存の判定の写し）。

## 2. Q1 が判定すべき物

### 2.1 Phase

| Phase | 証拠 | 推し |
| --- | --- | --- |
| ws099-p023（BUG-136/137） | q609-i01 で P2 が cleared を提案（phase.md の「判定の提案」）。P2 の lane では q609 は finished / cleared | cleared |
| ws099-p034 | 第 1 版は p034b に置き換わり、p034b は cleared（T1-228） | cleared（p034b で） |
| ws099-p035（p035b〜d） | ユーザーが案 A を選択。T1-229 pass（stage の PNG）、T1-231 は needs-person 2・回帰 pass（switch-running の log、launch・MAP が増えない） | cleared（遅れの値の体感は 5330 の UAT） |
| ws099-p038（最大化の余白 8 px） | T1-264 の files-maximized.png（Q1 に送付済み）、T1-277 で p134・p076・p132・p137 PASS | cleared |
| ws089-p012 | T1-238 PASS（settings-p012） | cleared |
| ws089-p013（About の memory） | T1-278 (1) PASS（Memory の行 `8.0 GB (7.8 GB free)`） | cleared |
| ws089-p019（titlebar の欄から Down） | p012 の q805 で compositor を直し、T1-238 で PASS | cleared（p012 で済み）か canceled |
| ws066-p001 | T1-165 の測定（受け入れ: true ≤ 700 µs、sh -c ≤ 950 µs）。p002 は cleared | cleared → WS066 の完了 |
| ws100-p013（BUG-170） | T1-087 PASS 3/3 | cleared |
| ws049-p017 | T1-093 PASS。⑤ はユーザーの判断 | cleared（⑤ は Future Work か別 Phase） |
| ws127-p008・ws128-p008 | T2-025 PASS 8/8（QEMU の回帰）。FreeBSD の build は 10/13 以降 | cleared（FreeBSD の分は 10/13 以降の作業へ） |
| ws128-p012（montage-4 の icon） | ユーザーが 2026-10-06 に決定、実装済み | UAT の image で確認して cleared |
| ws121-p001・ws130-p001・ws132-p001・ws159-p001・ws164-p001・ws165-p001・ws167-p001・ws168-p001（設計） | 判断はユーザーが決定済みで、後続の Phase が実装・cleared | cleared（ws132 の D1〜D3、ws130 の §9 は決定の記録を確かめてから） |
| ws131-p002（設計 第 2 版） | ユーザーのレビュー済み（D7 は確認中）、p003〜p027 が実装 | D7 を確かめて cleared |
| ws131-p013 | T1-085 は「流さない」指示のまま。kl_ の改名は後の p021〜p023 で済み、それ以降の回帰が多数 PASS | cleared（後続の証拠で）か canceled |
| ws134-p007・p013 | T1-066 PASS、T1-070 で p013 PASS | cleared（i915 の実の値は p010 の実機で） |
| ws168-p003 | T1-395（p004 の QEMU で sandbox_spawn も通る） | cleared |
| ws169-p000・ws170-p000（mock） | T1-181・T1-177b の撮影の後、p002 以降で実装・cleared | cleared |
| ws173-p001・p002・p003・p006 | T1-200 で aat-p002 PASS、T1-200c で p003 PASS、その後の AAT の実行で runner も使われている | cleared |
| ws174-p003 | T1-214: 1 回目 C3 だけ FAIL、変更なしの再試行で全 cell PASS | cleared か、C3 の間欠を Bug にしてから cleared |
| ws175-p002・p010 | T1-265b で fail 0・needs-person 2（PNG の目視） | PNG を見て cleared |
| ws079-p017 | ws175-p010 と同じ作業（T1-265b） | ws175-p010 と一緒に |
| ws178-p001 | T1-340: p005 PASS、x11-p004 の段 3 FAIL は T1-341 で WS178 の前の main でも同じ（既存） | cleared ＋ x11-p004 の段 3 を Bug に |
| ws181-p011 | T1-462 の log PASS、PNG はユーザーへ | ユーザーが見た後に cleared |
| ws182-p001（設計） | D1 はユーザーの確認待ち、推奨で p002 を実装 | D1 の確認の後 cleared |
| ws120-p002〜p007 | 2026-10-02 の形式の計画（WAV・FLAC・MP3・Ogg・独自 AAC）。2026-10-07 の決定（m4a＋libavcodec の add-in、p008・p009 cleared）で置き換わった | canceled（置き換え） |
| ws014-p001・ws078-p001 | 古い設計・棚卸しの Phase が planning のまま。後続は実装・cleared | cleared か canceled |
| ws084-p004 | 乖離 1〜3 の表と案の patch 2 つ（未適用）。直すかは Q1 | 判断（今は実害なし） |
| ws094-p009（L3 の数値） | uncleared（(a) 1954 ms、(c) 90 ms、目標に届かない。残りは import と Venus の呼び出し） | ベータ3 の性能の Phase へ回すかの判断 |

### 2.2 WS の完了の判定（実装の残りが無く、残りは規約＝ベータ3 だけ、または全部 cleared）

WS029（p001〜p007 cleared）、WS066（p001 の判定の後）、WS078（p001 の判定の後、p005 は規約）、WS122（p001〜p005 cleared）、WS148・WS149・WS151（ws148-p002 が 2026-10-08 に cleared。Resume point の「T1 の後に完了」の条件を満たした）、WS160（p001・p002 cleared）、WS166（p004 は規約）、WS167（p001 の判定の後、p003 は規約）、WS179（p001〜p003 cleared）。
実機の UAT が残る物（UAT の後に完了）: WS142（触った感じと閾値）、WS154（SKK の操作感、範囲の判断）、WS164・WS165（UAT）、WS168・WS169・WS170（実装は済み、規約はベータ3）。

**訂正（2026-10-08 P2、Q1 の判定の反映の時）**: WS102 は §2.2 から外す。phase の directory はどれも cleared だが、ws.md の表に directory の無い p010・p011（L3 の計測と直し）・p012（IME と組んだ変換、WS095 の続き＝ベータ3）・p013（Windows の QEMU の touch）・p014（規約）が planned のまま。照合の道具が directory だけを見ていた。他の 11 WS の表は確かめ直して、規約の Phase の外に planned が無いことを確かめた。

### 2.3 その他の判断

- WS121（browser の動画）: browser がベータ3（2026-10-08 ユーザー）なので、p003（Range）と WS の残りをベータ3 にするか。
- WS073（Bug の WS）: uncleared の p025・p033・p040・p045 は BUG の tracking。Bug Board に任せて WS を閉じるか。
- WS089-p014（Network の頁を 5330 の実の Wi-Fi で）: 5330 の Wi-Fi（AX211、BUG-134 ほか）の driver の成果が要る。ベータ2 に入れるか。

## 3. T1 の依頼の束（QEMU 1 回、案）

この範囲の WS は QEMU で確かめられる物がほぼ済んでいる。未実行で意味のある物は少ない:

1. **WS099 の B5 の回帰**（p022 の回帰の部分。規約はベータ3）: 先に `plan/ws099/tests/criteria.sh` の C9 の一覧から、消えた `zdesktop-p072.sh`（b9439f9ae で削除、T1-401 で `No such file`）を外す直しが要る（WS099 の試験の直し、P の担当。2026-10-06 の試験の整理の基準に沿って一覧から外す）。その後に criteria の image で `criteria.sh … C1 C2 C3 C4 C5 C7 C8 C9 C10`（C10 の 60 分は夜間の別枠なので外す）。合否: C1〜C5・C7〜C9 が PASS。
2. **WS033 p001**（USB の LAN の後挿し・抜去・carrier の変化、QEMU の L1〜L3）: uncleared で中断のまま。実装・直しが要るので、P の担当の作業の後に T1。
3. **WS100 p009**（L3 の音量の曲線、25・50・75・100% → -30・-15・-7・0 dB ± 3 dB）: planned。QEMU の HDA の WAV で測れるなら T1（`plan/ws035/tests/hda-wav-check.py` の形）。実装の確かめが先。
4. 既存の未実行の行の整理（Q1 の台帳）: T1-085（ws131-p013・p014、「流さない」指示）と T1-086（ws049、T1-090 で置き換わった）は取り下げでよい。

どれも前半の §3 の束（AAT の image）とは別の image（criteria の image・LAN の image）。1 と 3 は短い。

## 4. 実機が要る物

### 5330（素の起動、ユーザーの立会いか SSH の log）

| 対象 | 見る物 |
| --- | --- |
| ws099-p012 | C1 の黒・文字の画面 0 枚（目視）、Shut Down から 10 秒以内の電源断（BUG-119）、`SESSIOND GREETER failed` 0（BUG-122） |
| ws094-p012 | L1 の手順と L3 の (a)(c) の時間 |
| ws089-p011・p014 | p011 は passthrough の S7（壁紙・透明度・検索、透明度 85% の frame の率）。p014 は実の Wi-Fi（driver が要る） |
| ws100-p006 | HDA で鳴らす（診断の log と耳） |
| ws049 p007・p008・p016 | 電源ボタン・蓋・AC・EC の ACPI の事象（BUG-165） |
| ws079 | S8・S9（mouse）、`demo-s8-s9-manual.md`。ws079-p012 の実の touch の LCD |
| ws084-p003 | 再起動の 10 回（reboot-loop.sh） |
| ws130-p008 | DHCPv6 の実の LAN |
| ws132-p008 | 蓋の事象（/dev/system）。蓋の自動の切り替えはベータ3 |
| ws134 p007・p010 | i915 の telemetry の実の値 |
| ws142 | touchpad の gesture の触った感じと閾値 |
| ws154・ws164・ws165 | SKK の操作感、Welcome、手書き |
| ws159 p002・p005・p006 | I2C の touchpad と GPIO の割り込み（WS183 の実機と同じ回） |
| ws173-p005 | 素の 5330 で AAT |
| ws174 | 起動時の Ctrl・Shift（UEFI） |
| ws182-p002 | 電源ボタンの短押し・1.5 s の長押し（5320 も） |
| ws127-p007・ws128-p007 | Files・標準 app の実機の UAT |
| ws073-p032 | 素の 5330 での確認 |
| ws181 | 整列・Home・端の gesture の UAT（WS177 の案 U の UAT と一緒） |

### Windows の機械（ユーザー）

- ws081: touch の計測（`plan/ws081/tests/windows-touch.md`）、その後 L3 の p017。

## 5. 残りの作業（LW、前半と同じく 1 LW ≈ 実時間 1〜2 時間、実機の待ちは含まない）

| WS | 残り | 実装 | 試験・判定 | 計 |
| --- | --- | --- | --- | --- |
| WS099 | criteria.sh の C9 の一覧の直し、B5 の回帰、p012 の実機 | 0.1 | 0.4 | 0.5 |
| WS089 | 判定、p011・p014 の実機 | 0 | 0.2 | 0.2 |
| WS033 | p001 の後挿し・抜去・carrier の QEMU と直し | 0.5 | 0.2 | 0.7 |
| WS049 | 実機の ACPI の事象 | 0 | 0.3 | 0.3 |
| WS079・WS081 | S8・S9 の実機、Windows の計測と p017（L3） | 0.3 | 0.3 | 0.6 |
| WS084 | p004 の判断で直すなら乖離 1・3、p003 の実機 | 0.3 | 0.2 | 0.5 |
| WS094 | p012 の実機（p009 はベータ3 の性能へ回すなら 0、残すなら +1.0） | 0 | 0.2 | 0.2（+1.0） |
| WS100 | p009 の音量の曲線、p006 の実機 | 0.5 | 0.4 | 0.9 |
| WS121 | p003 の Range（ベータ2 に残すなら） | （0.5） | （0.2） | 0（+0.7） |
| WS127・WS128 | 実機の UAT、p012 の確認 | 0 | 0.4 | 0.4 |
| WS130・WS132 | 実機（DHCPv6、蓋の事象） | 0 | 0.4 | 0.4 |
| WS134 | p003 の直し（fps・GPU の名前）、実機の値 | 0.5 | 0.4 | 0.9 |
| WS142・WS154・WS159・WS164・WS165 | 実機の UAT | 0 | 1.1 | 1.1 |
| WS173 | p005 の素の 5330 の AAT | 0.3 | 0.7 | 1.0 |
| WS174 | C3 の間欠（Bug にするなら）、実機 | 0.3 | 0.2 | 0.5 |
| WS178 | x11-p004 の段 3（既存の FAIL）の調べと直し | 0.5 | 0.2 | 0.7 |
| WS182 | 実機の電源ボタン | 0 | 0.2 | 0.2 |
| 判定・完了だけ | WS014・029・066・078・102・120・122・131・145・148・149・151・160・166・167・168・169・170・175・179・181 | 0 | 0 | 0 |
| 計 | | 3.3 | 5.8 | **9.1 LW**（条件付き +1.7） |

空き時間の WS031（p015〜p048、約 8 LW）・WS075（p007a・b ほか、約 4 LW）は入れていない。前半の 16.6 LW と合わせると、ベータ2 の残りは約 25.7 LW（条件付き +4.2）。

## 6. その他の見つけた事

- **WS099 の C9 の一覧が消えた試験を指している**（criteria.sh の p072、T1-401）。回帰の束を流す前に直す（§3 の 1）。
- **x11-p004 の段 3 の FAIL は既存**（T1-341: WS178 の前の main でも同じ）。Bug Board に無ければ Bug にする。
- **ws174-p003 の C3** は 1 回目だけ FAIL（Ctrl+Shift を検出せず）、再試行で PASS。間欠の可能性。
- WS148・WS149・WS151 は ws.md の Resume point（「T1 の目視の後に p002 を cleared、WS を完了」）が 2026-10-08 に満たされたが、WS の Status は incomplete のまま。
- ws014・ws029・ws031 は古い形式（Phase の表に link が無い、phase.md に Status の行が無い物がある: ws031 p001〜p018、ws102 p024）。照合は ws.md の表の値で行った。
- WS120 の p002〜p007 は 2026-10-02 の計画の残りで、2026-10-07 の決定と食い違う（§2.1）。
- ws099・ws142・ws173・ws049・ws078・ws102・ws122・ws132・ws169・ws170 などで、phase.md はあるが ws.md の表に link の行が無い Phase がある（表の外の箇条書きか、別の節に書かれている）。表の構造は変えていない。

## 7. 追記（2026-10-08 P2、Q1 の判定の反映の時の確かめ直し）

**訂正: criteria.sh の C9 の一覧の p072 は既に外れていた**。`plan/ws099/tests/criteria.sh` の `C9_TESTS` から p072 を外す直しは 621197e04（2026-10-08 06:42）で main に入っていた。T1-401 の image の tree b2bb1cd02 はその前。§3 の 1・§6 の「直しが要る」は誤りで、直しは要らない。WS099 の B5 の回帰は今の tree でそのまま流せる。


phase の directory の無い、ws.md の表だけの Phase を 48 WS で確かめ直した（規約の Phase を除く）。§2.2 の WS102（上の訂正）の他に、報告に入っていなかった物:
- ws134-p009（K4 kernel: ACPI の thermal・電池、BUG-165 の後、実機）: planned。実装 約 0.5 LW ＋ 5330。
- ws132-p007（実機の UAT）・ws081-p007（touch の実物での調整）: 実機（§4 の対象と同じ回）。
- ws094-p015（条件つき、p012 の実機の値次第）、ws174-p004（BIOS、ユーザー「後日」）、ws169-p006（OAuth2、今回は作らない）: ベータ2 の外として数えない。
- ws031 の p021〜p051 の多数: 空き時間の WS（数えない）。
§5 の計は WS102（p010・p011 の L3 の計測と直し 約 1.0 LW、p013 の Windows の QEMU は実機）と ws134-p009（約 0.5 LW）を足して **約 10.6 LW**（条件付き +1.7）。
