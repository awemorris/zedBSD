<!-- awesome-plan project=zedbsd record=ws192-p001 -->

# ws192-p001: 状態の島の操作パネル（glass）の設計と実装

Status: planned（2026-10-09 Q1、q917 P1）
Parent: [WS192](../ws.md)

## 手順

1. 今の右上の状態の島（compositor の bar の右端の icon 群）、各 icon の click の動作（WiFi・音量・IME・Bluetooth・電池・通知）、既存の glass の描画（例 notify-popup・App Home 等で使っている物）を読む。
2. パネルの設計を phase.md に書く: 項目と並び、寸法（指で押せる大きさ、最小 44 px 相当を目安）、開閉（島の tap・外の tap・Esc）、mouse と touch、多画面の時に開く画面、既存の個別 popup との関係。境界（app → libkeiland → compositor → backend）を守る。
3. 実装と host 試験、build warning 0。AAT の scenario の案を T1 の依頼として足す（p002）。

## 受け入れ

- 設計の記録、build warning 0、host 試験 PASS、T1 の依頼の行。

## 2026-10-09 P1 q917: 調べ

- 状態の島（status pill）は compositor の `shell.c` の `bar_layout_on`: 右から時計の pill、その左に状態の pill。slot（34 px）は左から IME の言語（出ている時）、removable media（出ている時）、Bluetooth（出ている時）、network、volume、電池（ある機械）。島の高さは bar の group（`BAR_GROUP_HEIGHT`）。
- 押した時（`shell.c` の press の分配、上から順）: media の icon（`kwl_media_button`、今は何もしない）、IME の chip（`kwl_ime_indicator_button`、次の言語）、volume（`volume.c`、260 px の glass の popup に slider と Mute の switch、wheel で ±5）、network（`network.c`、Wi-Fi の switch と AP の一覧・key の欄・Alt+click で詳細）、Bluetooth（`bluetooth-bar.c`、switch と機器の一覧と Settings）。各 icon は 20〜26 px で、指では狙いにくい（ユーザーの観測）。
- glass の popup の描き方: `glass_shape_draw`（MODE_SHADOW の影と MODE_GLASS の白 0.86・edge 0.85、半径 12）、`glass_draw_text`、`glass_draw_solid`。volume・network・Bluetooth の popup は同じ形。
- touch: bar の上の指は `touch.c` の ROUTE_SHELL で `shell_press` と follow（pointer と同じ press・motion・release の経路）になるので、panel は mouse と touch を同じ code で受けられる。

## 設計（実装の前に Q1 へ要点を送る）

### 開閉

- 島（状態の pill の全体、icon の間の隙間も）の click・tap で、その output の bar の下にパネルが開く。もう一度島を押すと閉じる。パネルの外の press は閉じて、そこで止まる（下の window へは行かない。今の popup と同じ）。Esc で閉じる。
- 今の個別の popup は bar からは直接開かない（島の press は全部パネルに統一）。パネルの中の項目から開く: Wi-Fi の行の「›」→ 今の network の menu（AP の一覧・key の欄）、Bluetooth の行の「›」→ 今の Bluetooth の menu。開く時はパネルを閉じて、同じ右上の位置に出す。
- 残す近道: network の icon の Alt+click（詳細）、volume の icon の上の wheel（±5）。どちらも mouse の物で、touch には関係しない。
- 多画面: 押した bar の output に開く（今の popup と同じ）。App Home の上の島（白い状態の表示）も同じに開く。

### 形と寸法

- 位置: 右端を時計の pill の右端（output の右 − `BAR_EDGE`）に揃え、上は bar の下 + 6 px。幅 360 px（狭い output では output の幅 − 16 に縮める）。半径 20、内側の余白 16、行の間 8。
- glass: 今の popup と同じ（影と白 0.86 の glass）。
- 行（上から。出ない物は詰める。高さはどれも 56 px、指の当たりは行の全体）:
  1. **Wi-Fi**: 左に network の icon（大きく）、名前「Wi-Fi」と状態（接続中の SSID・Off・Searching…・有線なら「Wired」）、右に大きい switch（52×32）と「›」。switch で on/off、行の残り（「›」を含む）で network の menu。
  2. **Bluetooth**（島に Bluetooth が出ている時だけ）: 同じ形。switch で power、「›」で Bluetooth の menu。
  3. **Sound**: 1 行目に「Sound」と %（Muted）、2 行目に 48×48 の speaker の button（押すと mute の切り替え）と slider（track 8 px、knob 28 px、行の高さ 56）。press・drag で volume、離した時に確認の音（今の popup と同じ）。音の無い時は薄くし理由を書く。
  4. **Input**（IME が出ている時だけ）: 名前「Input」と今の言語（「あ」「A」と言語名）、押すと次の言語（今の chip と同じ `STATUS_NEXT`）。
  5. **Battery**（ある機械だけ）: 「Battery 85%」（充電中は「Charging」）、操作は無い。
  - media の icon は今も押して何もしない（WS156 H7）ので、パネルにも項目を作らない。
- 文字は kl_tr で日本語にも。

### 境界

- compositor の中だけ: 新しい `userland/desktop/wayland/status-panel.c`（開閉・配置・描画・press・motion・key）と、`shell.c` の分配（島の press をパネルへ）、`volume.c`・`network.c`・`bluetooth-bar.c`・`input-method.c` に、パネルが使う小さな公開の関数（状態を読む・switch を切り替える・menu を指定の位置に開く・volume を設定する）。libkeiland・backend・protocol は変えない。
- test のための log: `KWL STATUS panel open output= x= y= width= height=`、`KWL STATUS item name=wifi|wifi-more|bluetooth|bluetooth-more|mute|volume|input|battery x= y= width= height=`、`KWL STATUS act name=…`、`KWL STATUS panel close via=island|outside|key|item`。

### 試験（p002 で T1）

- AAT の scenario（mouse と touch の両方）: 島の真ん中を tap → panel open、Wi-Fi の switch（network-probe）→ off/on、「›」→ network の menu、volume の slider の drag → 値、mute、Input → 次の言語、外の tap と Esc で閉じる、PNG。
- host 試験: 配置の計算（output の幅ごと、出る行の組み合わせ）を compositor の他の host 試験と同じ形で。
