# WS050 設計: USB-C の UCSI driver

Status: 案の第 3 版（2026-10-04、ws050-p001、P1 generation16）。第 1 版（generation15）に [敵対的レビュー](design-review-2026-10-04.md)
（A1〜A9・B1〜B8・C1〜C3・D・E）を反映し（第 2 版）、§10 の 5 項目のユーザーの決定（末尾の記録）を反映した（第 3 版）。レビューの各項目の扱いは
§11、HPD・pin・向きの二つの出所の扱いは §13。

## 1. 目的と範囲

USB Type-C Connector System Software Interface（UCSI）で、USB-C の connector の状態（接続・向き・電源とデータの役割・partner の種類・
Alternate Mode・USB PD の contract）を読み、変化の通知を受け、必要な操作（data と power の role の切り替え、connector の reset、Alternate Mode の
選択）を行い、Alternate Mode の状態を WS051（DP Alt Mode）へ、電源の状態を WS052・WS132 へ渡す（決定 2）。UCSI は 1.x と 2.x の両方の配置と
field を扱う（決定 3）。

plug の**向き**（CC1/CC2）は UCSI 2.0 以上では GET_CONNECTOR_STATUS の field から、1.x では i915（WS051）の TCSS・FIA から取る（決定 4、§13）。
DP の **HPD と pin の割り当て**は二つの経路を持つ: UCSI 2.0 以上の PPM が返す時は UCSI から、i915 が TCSS・FIA から取れる時は i915 からも
（1.x では i915 だけ、決定 5 と補足、§13）。

範囲外: USB PD の message を OS が直接話すこと（TCPM。UCSI では PPM（PD controller と EC の firmware）が話す）、Thunderbolt・USB4 の
tunnel、role の切り替えの UI（口は `/dev/system` の WS132 へ）。

## 2. UCSI の要点

UCSI は OS（OPM: OS Policy Manager）と platform（PPM: Platform Policy Manager）の間の mailbox の規約である（Intel の
「USB Type-C Connector System Software Interface Specification」。1.0〜1.2、2.0〜2.1、3.0）。**この節の bit・offset・command の細部は
記憶による要約であり、p002 の実装の前に仕様書の該当版（1.2 と 2.x）で確かめる**（【仕様要確認】）。Linux の driver（GPL）の code は写さない。

### 2.1 data structure（1.x の配置、合計 0x30 byte）

| offset | 名前 | 大きさ | 書く側 |
| --- | --- | --- | --- |
| 0x00 | VERSION（BCD、例 0x0120 は 1.2） | 2 | PPM |
| 0x02 | 予約 | 2 | — |
| 0x04 | CCI（Command Status and Connector Change Indication） | 4 | PPM |
| 0x08 | CONTROL（command の code と引数） | 8 | OPM |
| 0x10 | MESSAGE_IN（PPM → OPM の data） | 16 | PPM |
| 0x20 | MESSAGE_OUT（OPM → PPM の data） | 16 | OPM |

**配置の決め方**（B2、決定 3）: VERSION は EC が実行時に書く値で、5330 での値は未知（UAT で記録する）。driver は配置を VERSION で選ばず、
platform の AML が定義する mailbox の region の大きさと field の並びで決める（5330 は 0x38 byte、MGI・MGO が各 16 byte で 1.x の配置、§3）。
2.x の配置（MESSAGE_IN・MESSAGE_OUT が大きい版。offset と大きさは仕様書の 2.0・2.1 で確かめる）も最初から扱う: transport は配置の表
（`struct ucsi_layout`: 各 field の offset と大きさ）を AML の region から選んで核に渡し、核は MESSAGE_IN の大きさに従って一覧の command
（GET_ALTERNATE_MODES・GET_PDOS）の 1 回の件数を決める。VERSION は field の意味（GET_CONNECTOR_STATUS の版ごとの bit、向き・DP の状態の有無）の
判定に使う。どちらの表にも合わない region なら attach しない（log を出す）。2.x の実機は無いので、2.x は host の試験（疑似の PPM の 2.x の版）
で確かめる。

### 2.2 CCI

command の完了（Command Completed）、busy、error、not supported、reset completed、cancel completed、acknowledge command（ACK_CC_CI の完了）、
MESSAGE_IN の data の長さ、変化した connector の番号（Connector Change Indicator、0 は無し）。

### 2.3 command（この WS で使うもの）

| code | command | 用途 |
| --- | --- | --- |
| 0x01 | PPM_RESET | 初期化と回復（reset completed を poll で待つ。§2.4・§6） |
| 0x02 | CANCEL | 長い command の取り消し（使うのは timeout の回復だけ。§6） |
| 0x04 | ACK_CC_CI | command の完了・connector の変化の受領の通知 |
| 0x05 | SET_NOTIFICATION_ENABLE | 通知する変化の種類（段階を分けて有効にする。§5、B6） |
| 0x06 | GET_CAPABILITY | connector の数、bmOptionalFeatures、Alternate Mode の数、PD の版 |
| 0x07 | GET_CONNECTOR_CAPABILITY | connector ごとの能力（DFP/UFP/DRP、Alternate Mode の有無、provider/consumer） |
| 0x0C | GET_ALTERNATE_MODES | connector・partner（SOP）・cable（SOP'）の SVID と mode の VDO の一覧（offset を進めて全件、§5、B4） |
| 0x0D | GET_CAM_SUPPORTED | connector が入れる Alternate Mode（connector の GET_ALTERNATE_MODES の一覧への index の bitmap、B4） |
| 0x0E | GET_CURRENT_CAM | 今入っている Alternate Mode（同じ一覧への index、B4） |
| 0x10 | GET_PDOS | source・sink の PDO（MESSAGE_IN に 4 件まで、offset を進めて全件、B4） |
| 0x12 | GET_CONNECTOR_STATUS | 接続、電源の方向、partner の種類と flag（USB・Alternate Mode）、電源の operation mode（USB 既定・BC・1.5A・3A・PD、B8）、RDO、変化の bit。2.0 以上は向きと、版によっては DP の状態（HPD・pin）（【仕様要確認】、§13） |
| 0x13 | GET_ERROR_STATUS | error の詳細（log 用） |
| 0x03 | CONNECTOR_RESET | connector の reset（hard reset か data reset。決定 2） |
| 0x08 | SET_UOR | data の role（DFP/UFP）の切り替えと受け入れの設定（決定 2） |
| 0x09 | SET_PDM | power direction mode（版による。要否は仕様書で） |
| 0x0B | SET_PDR | power の role（source/sink）の切り替えと受け入れの設定（決定 2） |
| 0x0F | SET_NEW_CAM | Alternate Mode に入る・出る（決定 2。firmware が自分で入らない platform と、利用者の明示の選択に） |
| 0x11 | GET_CABLE_PROPERTY | cable の能力（速度、電流、active・passive。Alternate Mode の選択の判断に） |

command の code は記憶による（【仕様要確認】、p002 の前に仕様書で確かめる）。操作の command（CONNECTOR_RESET・SET_UOR・SET_PDR・SET_NEW_CAM）は
GET_CONNECTOR_CAPABILITY と GET_CAPABILITY の bmOptionalFeatures で PPM が対応すると言うときだけ出し、結果（新しい role・mode）は
GET_CONNECTOR_STATUS・GET_CURRENT_CAM の読み直しで確かめる。

### 2.4 手順と ACK の規則（B3）

1. OPM が CONTROL（と要れば MESSAGE_OUT）を書き、PPM に知らせる（ACPI では `_DSM` の function 1）。
2. PPM が CCI と MESSAGE_IN を更新し、通知する（ACPI では `Notify (UCSI の device, 0x80)`）。
3. command completed（error の完了も含む）を受けたら MESSAGE_IN を読み、ACK_CC_CI（Command Completed Acknowledge）を送る。**次の command は
   その ACK の完了（CCI の Acknowledge Command bit）を待ってから出す**。ACK_CC_CI 自体の完了は ACK しない。PPM_RESET の完了（reset completed）も
   ACK しない。
4. CCI の connector の番号は、command の完了・busy・ACK 待ちの間に届いた CCI でも latch して覚える（取りこぼさない）。connector change の受領は
   その connector の GET_CONNECTOR_STATUS を読んだ後に ACK_CC_CI（Connector Change Acknowledge）で知らせる。command の完了と connector change が
   同時なら 1 回の ACK_CC_CI に両方の bit を立ててよい。
5. 同時に出せる command は 1 つ。busy なら待つ。timeout（通常の command 5 秒、PPM_RESET は 1 秒ごとに数回、値は仕様書と実機で決める）で
   CANCEL、それも完了しなければ PPM_RESET からやり直す（§6）。

## 3. 対象機（Latitude 5330）の UCSI

[5330 の table](../tools/aml-host/latitude5330/)（`ssdt8.dat`、OEM Table ID `UsbCTabl`）を `iasl -d` で読んだ結果（レビューの §F で確認済み）:

- device `\_SB.UBTC`: `_HID` `USBC000`、`_CID` `PNP0CA0`、`_UID` 1、`_DDN` "USB Type-C"。
- `_STA`: GNVS の `USTC == 1` で、EC の 0x80・0x81（`VER1`・`VER2` に写る UCSI の VERSION）が 0 でなく、`OSYS >= 0x07DF` なら 0x0F。
  `OSYS` は `\_SB.PC00._INI` が `_OSI ("Windows 2015")` で設定する（zedBSD の `_OSI` は `Windows 2022` まで真）。**VERSION を mailbox に書くのは
  `_STA` だけ**（A2）なので、driver は `_STA` を評価してから VERSION を読む。
- EC の経路: `\ECRB`・`\ECWB` は `\_SB.PC00.LPCB.ECDV.ECR1`・`ECW1`（Serialized）で、EC の region が繋がる前（`ECRD == 0`）は SMI の経路 `EISC`。
  `_Q79` は `ECRD != 1` なら何もせず返る。**attach は EC の `_REG`（`ECRD = 1`）の後**（kernel の順: `drv_acpi_attach()` の `_REG`・`_INI` の後、
  EC の attach の後。§5）。
- `_CRS`: Memory32Fixed、基底は GNVS の 32 bit の field `UBCB`（**mailbox への pointer**。GNVS の中ではない、A3）、長さ 0x1000。mailbox は
  `OperationRegion (USBC, SystemMemory, UBCB, 0x38)` の `VER1`・`VER2`・`RSV1`・`RSV2`・`CCI0..3`・`CTL0..7`・`MGI0..F`・`MGO0..F`
  （§2.1 の 1.x の配置）。field は `Field (USBC, ByteAcc, Lock, Preserve)`（byte ごとに Global Lock）。
- mailbox の memory の種類は不明（A3）。firmware が boot services data に置いていれば、kernel の page の allocator が BOOT_RECLAIM として再利用して
  壊す。UAT で `UBCB` の値と、その address の memory map の型を記録する。型が USABLE か BOOT_RECLAIM なら driver は attach せず log を出す
  （型を問う kernel の口の有無は p003 で調べる。§12 の 5）。
- `_DSM`（UUID `6f8398c2-7ca4-11e4-ad36-631042b5008f`）: function 0 は `Buffer (1) {0x1F}`（integer でない、A7）、**1 = write**（`Acquire (\ECMU)`
  の下で MGO0..F を EC の 0xA0..0xAF、CTL0..7 を 0x88..0x8F へ書き、EC の 0xB0 に 0xE0）、**2 = read**（`Acquire (\ECMU)` の下で EC の 0x90..0x9F
  を MGI0..F、0x84..0x87 を CCI0..3 へ写す）、3 = `XDCE`、4 = `UDRS`。Arg1（revision）は 1 を渡す（B8、仕様書で確かめる）。Arg3 は空の Package。
- 通知: `_Q79`（EC の query）が `Acquire (\ECMU)` の下で 0x90..0x9F・0x84..0x87 を mailbox に写し、`Notify (\_SB.UBTC, 0x80)` してから Release。
- `\ECMU` は root の `Mutex (ECMU, 0)`。`_DSM` 1・2 と `_Q79` は全部この mutex の下で mailbox を書く。
- connector: `\_SB.UBTC.CR01`〜`CR0A` が GNVS（`TTUP`・`TPxU`・`TPxD`）の条件で作られ（番号が飛ぶことがある）、`_PLD`・`_UPC` を持つ（`_ADR` は
  全部 0）。**UCSI の connector の番号 n と `CR0n` の対応は未確認**（A9）。実機で確かめ、合わない時は `_PLD` の位置だけを補助の情報として出し、
  対応付けはしない。

依存: EC の query の配送（ws049-p007、QEMU は済み、実機待ち）、DSDT・SSDT の読み込み（ws049-p008、BUG-165）、EC の region（`_REG`）。

## 4. 構成

```
src/drivers/typec/
  ucsi.c         UCSI の核: command の実行、CCI の処理、ACK の規則、回復（transport に依らない）
  ucsi.h         核と transport の間の口（kernel 内）
  ucsi-acpi.c    ACPI の transport: USBC000/PNP0CA0 の device、_CRS の写像、_DSM、Notify、\ECMU の下での mailbox の読み書き
  typec.c        Type-C の connector の層: 状態の保持、generation、利用者への通知、/dev/typec の診断の出力
include/drivers/typec/typec.h  kernel 内の公開の口（connector の状態の struct、listener の登録）
```

- transport の口（ucsi.h）: `exchange_write(control, message_out)`（mailbox への書き込みと `_DSM` 1 を一つの操作に）、`exchange_read(&cci,
  message_in)`（`_DSM` 2 と mailbox の読みを一つの操作に）、`mailbox_read(&cci, message_in)`（Notify の後の直接の読み）、通知の callback。
  host の試験は疑似の transport を差し込む。
- **mailbox の一貫性**（A1）: ACPI の transport は、`\ECMU` を取った 1 つの interpreter entry の中で「`_DSM` 2 → CCI・MESSAGE_IN の読み」
  （書くときは「MESSAGE_OUT・CONTROL の書き → `_DSM` 1」）を行う。`_Q79` は `\ECMU` を待つので割り込めない（`_DSM` の中の `Acquire (\ECMU)` は
  同じ thread の再取得で通る。sync level は全部 0 で、Serialized の `ECR1` の mutex も 0）。これには WS049 に「AML の mutex を名前で取った entry の中で
  callback を走らせる」公開の口が要る（§12 の 1）。口の試験と保険として、CCI → MESSAGE_IN → CCI の二度読みで CCI が変わっていたら読み直す。
- **mailbox の写像**（A4）: AML の SystemMemory の写像は handler の private な LRU の cache（追い出しで unmap）なので共有できない。driver は
  `hal_space_map_device(HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_NOCACHE)` で別に写像する（属性は AML と同じ uncached）。基底が page に
  揃わない・0x38 byte が page を跨ぐ場合も扱う（page に揃えて 2 page まで写す）。
- 核は 1 つの kernel thread（`ucsi`）で全ての command を順に実行する。
- 状態: connector ごとに、接続、電源の方向（source/sink）、電源の operation mode、partner の種類（DFP・UFP・powered cable・debug・audio
  accessory）、partner の flag（USB・Alternate Mode）、Alternate Mode の一覧（SVID・VDO）と今入っている mode、PDO と RDO（PD の contract）、
  向き（1.x では「不明」）、`_PLD` の位置。版の番号（generation）を変化ごと・PPM_RESET の回復ごとに増やす（B7）。
- 利用者への口（kernel 内）: `typec_listener_register(callback, argument)`。callback は `ucsi` thread から、変化した connector と generation を
  受ける。**callback は block せず、`ucsi` thread を待つ要求を出さない**（C2。要求は自分の queue に積んで自分の thread で処理する）。状態は
  `typec_connector_get(index, &state)` で写して読む（lock の中で callback を呼ばない）。
- 操作の口（kernel 内、決定 2）: `typec_connector_set_data_role(index, role)`・`typec_connector_set_power_role(index, role)`・
  `typec_connector_reset(index, kind)`・`typec_connector_enter_mode(index, svid, mode)`・`typec_connector_exit_mode(index)`。要求は `ucsi` thread の
  queue に積んで順に実行し、結果は listener の変化の通知で返す（呼び手を block しない）。利用者への正式な口は WS132 の `/dev/system`（決定 1）。
- 診断の口: `/dev/typec`（text、read で全 connector の状態、`/dev/acpi` と同じ形、UAPI を足さない、決定 1）。device 番号は `/dev/acpi`
  （`0x000B0000`）と同じ帯の空きを kernel の cdev の表で確かめて取る（C3）。正式な通知は WS132 の `/dev/system` の事象（Keiland が受け取る）。
- i915 からの報告の口（決定 4・5、§13）: `typec_display_report(tc_port, &report)`（i915 が TC の port ごとに HPD・pin・lane 数・向き（取れれば）を
  渡す）。typec の層が UCSI の connector に対応付けて状態に入れる。

### 4.1 lock の順（A5・A6）

- Notify の handler は Notify の opcode から同期で呼ばれる（`aml-operator.c` の `op_notify` → `drv_acpi_notify`）。そのとき呼び出しの thread は
  interpreter の lock と `\ECMU` を持っている。走る thread は AML を走らせている thread（通常は ACPI の event thread、限らない）。
- handler は state の mutex を取らない。spinlock を最内の lock として「通知あり」を立て、`ucsi` thread の waitq を起こすだけ。spinlock と
  waitq は `drv_acpi_notify_install` の前に初期化する。
- `ucsi` thread は state の mutex を持ったまま `drv_acpi_evaluate`（interpreter の lock を取る）を呼ばない。順は「interpreter の lock（AML）→
  `\ECMU` → 終わってから state の mutex」。kern の lock の rank の検査は無い（`lock.c`）ので、この順は文書と review で守る。
- driver を止めた後も Notify の handler は残る（WS049 に notify の除去が無い、A6）。softc は解放せず `stopped` の flag で handler を no-op にする。
  install は interpreter の entry の中で行う（`drv_acpi_notify_install` は lock 無しで `node->notify` を書き換え、event thread の配送と競合する）。
  WS049 の新しい Phase で install・remove を entry の中で直列化する口を足す（§12 の 3）。

## 5. 初期化

`drv_acpi_attach()` の後（`_REG`・`_INI`・EC の attach の後、`pcat.c` の ACPI の後）に `ucsi_acpi_attach()` を呼ぶ（A2）。

1. ACPI の walk で `_HID USBC000` か `_CID PNP0CA0` の device を探す。無ければ何もしない。
2. `_STA` を評価する（これが VERSION を mailbox に書く）。present でなければ何もしない。
3. `_CRS` から Memory32Fixed の基底と長さを読み（§12 の 4）、mailbox の region（`USBC`）が 1.x か 2.x の配置の表に合うことを確かめ、合う表を
   選ぶ（B2、決定 3）。memory map の型を確かめる（A3）。写像する（A4）。
4. VERSION を読み、log に出す（1.x / 2.x の field の意味の判定に使う。B1）。
5. `_DSM` function 0（Buffer の bit 1・2）で function 1・2 があることを確かめる（**必須の検査**、A7。ToUUID の混合 endian を誤ると function 1・2 は
   黙って `Buffer {0}` を返すので、function 0 の bit で検出する）。
6. spinlock・waitq・state を初期化し、`drv_acpi_notify_install(device, …)` で 0x80 を受ける（entry の中、A6）。
7. thread を起こし: PPM_RESET → reset completed を poll（reset 後は通知が無効、B7）→ SET_NOTIFICATION_ENABLE（command の完了と error だけ、B6）
   → GET_CAPABILITY → connector ごとに GET_CONNECTOR_CAPABILITY・GET_CONNECTOR_STATUS（接続していれば GET_ALTERNATE_MODES（connector・SOP・
   SOP'、offset を進めて全件）・GET_CAM_SUPPORTED・GET_CURRENT_CAM・GET_PDOS（全件））→ bmOptionalFeatures で使える通知に絞って
   SET_NOTIFICATION_ENABLE（connector の変化）→ 全 connector の GET_CONNECTOR_STATUS を読み直す（有効にする前の変化を取りこぼさない、B6）。

## 6. 誤りと回復

- command の timeout → CANCEL → PPM_RESET。PPM の error は GET_ERROR_STATUS を読んで log。not supported はその機能を使わない。busy は待つ。
- PPM_RESET の回復は全状態を無効にする: 状態を「不明」にし、generation を増やして利用者に知らせ、§5 の 7 を初めから列挙し直す。reset の直後の
  CCI に前の reset completed が残って偽の完了に見える恐れ（推測、B7）があるので、reset の前に CCI を記録し、reset completed は CONTROL を書いた
  後に変化した CCI でだけ受ける（実機で確かめる）。続けて失敗したら driver を止める（kernel は止めない）。
- **通知の取りこぼしと poll**（A8）: `_DSM` 2 は 1 回で EC の byte を 20 回読み（Serialized の ECR1、各回に EC の transaction、待ちは interpreter
  の lock を持った busy の stall）、mailbox への書き込みも Global Lock 付きで 20 回ある。Notify の後は `_Q79` が写し終えているので `_DSM` 2 を
  呼ばず mailbox を直接読む（`\ECMU` の下で）。Notify が来ない時だけ `_DSM` 2 と CCI を poll する: 20ms から始めて 100ms まで間隔を延ばし、
  timeout まで。poll で動いたことは log に残す。`_DSM` 1・2 の所要は実機で測る（UAT の項目）。
- suspend・resume（C1、WS052 の S0ix）: resume の後は通知の再有効化か PPM_RESET からの列挙のやり直しが要る。suspend 中に届いた `_Q79` は
  resume の後の読み直しで拾う。WS052 の suspend・resume の hook（未設計）への依存として、WS052 の設計に渡す。

## 7. 試験

- host（核）: `ucsi.c` を host で compile し、疑似の PPM（C の model）と、**実機の mailbox の記録（CONTROL・CCI・MESSAGE_IN の列）を再生する
  fixture** の 2 つにつなぐ（D: 設計と同じ読み方の疑似だけでは B1・B4・B5 の誤りを検出できない）。筋書き: 初期化、抜き差し、Alternate Mode の
  変化、busy・error・timeout・CANCEL・PPM_RESET の回復、通知の取りこぼし（poll）、ACK の規則（B3）、offset を進める一覧（B4）。ASan・UBSan。
  正解値の出所: 同じ 5330 で Linux の `/sys/class/typec` の値を black box の正解として記録する（code は写さない）。
- host（AML）: ws049 の harness に 5330 の `ssdt8.dat` を読み込み、EC の模擬を `ECRD = 1` に（でないと SMI の経路で `_Q79` が Notify しない）、
  `\_SB.PC00._INI` で `OSYS` を設定させて `_STA` が 0x0F になることを確かめる。`_DSM` の function 0 が `Buffer {0x1F}`、`_CRS` が Memory32Fixed、
  `_DSM` 2 が 0x90..0x9F を写すことを確かめる。**`_Q79` と `ucsi` の読みが交互に走る試験**（`\ECMU` の下の読みがちぎれないこと）を足す（A1）。
- QEMU: UCSI の device は無い。T1 に boot test を依頼し、driver が「device 無し」で何もしないことだけを確かめる（自分で QEMU を起動しない）。
- 実機（UAT）: 5330 の VERSION（B2）、`UBCB` と memory map の型（A3）、`CR0n` と connector の対応（A9）、`_DSM` 1・2 の所要（A8）、
  `/dev/typec` で各 port の抜き差し・電源の方向・partner の Alternate Mode、dmesg の通知の経路（Notify か poll か）。機材: USB-C（DP Alt Mode）の
  monitor か USB-C→DP の adapter、USB PD の充電器、USB-C の hub（D）。

## 8. 他の WS との境界

- WS049: 足す公開の口を §12 にまとめた（新しい Phase として Q1 に提案）。
- WS051: DP Alt Mode。Alder Lake では Alternate Mode に入るのは PPM（EC と PD controller の firmware）と PMC の mux で、OS は状態を読む。
  UCSI 1.x から得られるのは「DP の SVID 0xFF01 の mode に入った事実」と mode の能力の VDO（MID）だけで、pin の割り当て（C/D/E）と HPD は
  得られない（B5）。i915 は TCSS の live status と FIA の register から pin と lane を、Type-C の hotplug の割り込みから HPD を読む（Linux の i915 の
  `intel_tc.c` と同じ分担。code は写さない）。UCSI 2.0 以上で HPD・pin が取れる時は UCSI からも取る（決定 5 の補足、§13）。WS051 の画面の出力は
  WS050 を待たずに進められる（決定 5）。firmware が自分で DP mode に入らない platform では WS051 が WS050 の SET_NEW_CAM（`typec_connector_enter_mode`）
  を使う。
- WS052・WS132: 電源の方向と contract、充電器の有無を `/dev/system` の電源の状態へ。WS052 の suspend・resume の hook に §6 の再列挙を載せる（C1）。

## 9. Phase の案（E を反映して分け直した）

| Phase | 内容 | 依存 | 受け入れ |
| --- | --- | --- | --- |
| p001 | 調査と設計（この文書） | WS049 の namespace | この文書、§10 の判断 |
| p002 | UCSI の核・Type-C の層の状態と kernel 内の口（`ucsi.c`・`ucsi.h`・`typec.c` の状態と generation・`typec.h`）、疑似の PPM と記録の再生の host の試験 | p001、§10、仕様書での §2 の確認、**実機の VERSION と mailbox の記録（UAT）** | host の筋書きが全て通る、ASan/UBSan、規約、kernel の flag で warning 0 |
| p003 | ACPI の transport と kernel への組み込み（`ucsi-acpi.c`、attach の位置、Notify、thread、`/dev/typec`） | p002、WS049 の新しい Phase（§12）、ws049-p007・p008 | AML の host の試験（`_Q79` との交互を含む）、kernel の build、QEMU の boot test（T1）、UAT の手順 |
| p004 | 操作の command（CONNECTOR_RESET・SET_UOR・SET_PDR・SET_NEW_CAM・GET_CABLE_PROPERTY）と操作の口（決定 2） | p003 | host の試験（疑似の PPM の role の切り替え・mode の出入り・reset）、実機で data・power の role の切り替えと DP mode の出入り（PPM が対応する範囲） |
| p005 | i915 との連携（`typec_display_report`、HPD・pin・向きの二つの出所の統合、§13）と TC の port と connector の対応付け | p003、WS051 の p002（TC の port の核） | host の試験（二つの出所の優先と食い違い）、実機で 1.x の向き・HPD・pin が i915 から入る |
| p006 | 実機の確認と規約の全文の確認 | p003〜p005、実機 | 実機で抜き差し・向き・電源・Alternate Mode が読め通知が届く、操作ができる、規約、build、boot test |

第 1 版の p004（Type-C の層）は p002（状態と口、host の試験に要る）と p003（`/dev/typec`）に分けて入れた。Alternate Mode と PDO の読み直しは核
（p002）。2.x の配置と field（決定 3）は p002 の核と疑似の PPM の 2.x の版で扱う。

## 10. 人間の判断の点（2026-10-04 に決定済み。決定は末尾の「ユーザーの決定」、この節は第 2 版の案の記録）

1. **公開の形**: 診断の text の `/dev/typec`（UAPI を足さない、`/dev/acpi` と同じ）で始め、利用者向けの正式な口は WS132 の `/dev/system` に
   任せる案。別の形（専用の ioctl の device）にするか。
2. **role の切り替え**（SET_UOR・SET_PDR）と CONNECTOR_RESET・SET_NEW_CAM（Alternate Mode の選択）を範囲に入れるか。案: 入れない（状態の読みと
   通知、Alternate Mode の状態に絞る）。採用なら ws.md の目標の「必要な操作（role の切り替え、Alternate Mode の選択）ができる」を直す。
3. **UCSI の版**: 1.x の配置だけに対応する案（5330 の region は 1.x）。2.x 以降の配置の機種が対象に入ったら足す。配置は VERSION でなく AML の
   region で決める（B2）。
4. **向き**（B1）: ws.md の受け入れの「向きが読める」は UCSI 1.x では満たせない見込み。案: 受け入れを「向きは UCSI 2.0 以上の PPM でだけ（1.x は
   不明と公開）」に直す。別の出所（TCSS の register など、未確認）で補うかどうか。
5. **WS050・WS051 の ws.md の HPD・pin の記述**（B5）: 「HPD・pin は i915（TCSS・FIA）、UCSI は補助」に直す案。

## 11. レビューの項目の扱い

| 項目 | 扱い |
| --- | --- |
| A1 mailbox の競合 | §4: `\ECMU` を取った entry の中で読み書き、WS049 の口（§12）、二度読みの保険、交互の host 試験（§7） |
| A2 VERSION と attach の順 | §3・§5: `_STA` の後に VERSION、attach は `_REG`・EC の attach の後 |
| A3 mailbox の memory | §3・§5: `UBCB` は pointer、memory map の型を UAT で記録、USABLE・BOOT_RECLAIM なら attach しない |
| A4 写像 | §4: driver は別に uncached で写像、page の跨ぎを扱う |
| A5 lock の順 | §4.1 |
| A6 handler の残り | §4.1: stopped の flag、install を entry の中で、WS049 の口（§12） |
| A7 package・function 0 | §3・§5: function 0 は Buffer の bit を必須で検査、package の公開は §12 |
| A8 poll の負荷 | §6: Notify の後は `_DSM` 2 を呼ばない、poll は 20→100ms、所要を実機で |
| A9 connector の対応 | §3: 実機で確かめ、合わなければ対応付けしない |
| B1 向き | §1・§2.3・§13（決定 4: 2.0 以上は UCSI、1.x は i915） |
| B2 配置 | §2.1・§5（決定 3: 1.x と 2.x の両方）、p002 の前に実機の VERSION |
| B3 ACK | §2.4 |
| B4 index・offset | §2.3・§5 |
| B5 DP の pin・HPD | §1・§8・§13（決定 5 と補足: 二つの経路） |
| B6 通知の有効化の順 | §5 の 7 |
| B7 PPM_RESET | §5・§6 |
| B8 BC・USB の世代・revision | §2.3・§3 |
| C1 suspend・resume | §6・§8（WS052 への依存） |
| C2 callback | §4 |
| C3 cdev 番号 | §4・§10-1 |
| D 試験の穴 | §7 |
| E Phase と依存 | §9・§12 |

## 12. WS049 に足す公開の口（新しい WS049 の Phase の案、Q1 が Queue にする）

UCSI（p003）が使い、他の driver（WS051・WS052・WS132 の ACPI の利用者）も使う見込みの口。ws049-p009（規約の確認）の後に、WS049 の新しい Phase
として入れる案。HAL の API は変えない（`include/drivers/acpi/acpi.h` と WS049 の source だけ）。

1. **AML の mutex を名前で取った entry の中で callback を走らせる口**: `drv_acpi_run_locked(const char *mutex_path, callback, argument)`
   （entry に入り、名前の AML mutex を entry の thread として取り（sync level の規則に従う）、callback を呼び、放して出る）。callback の中では
   `drv_acpi_evaluate` を同じ entry の入れ子として呼べる（既存の `drv_acpi_enter` の入れ子の規則）。
2. **package の生成の公開**: `drv_acpi_object_package_new()` と要素の設定（`_DSM` の Arg3 の空の package、他の `_DSM` の引数）。
3. **notify の除去と、install・remove の直列化**: `drv_acpi_notify_remove()`、install・remove を interpreter の entry の中で行い、event thread の
   配送と競合しない形に。
4. **`_CRS` の共通の解析**: Memory32Fixed・QWord/DWord の memory・IO・FixedIO・IRQ・Interrupt を解く関数（今は EC の driver が自分で解いている）。
5. （要否を p003 で調べる）memory map の型を問う口（A3）。kernel に既にあれば使う。無く、HAL の API が要るなら止めて Q1 に相談する。

（Q1 が q696 ws049-p017 として Queue に入れ、1〜4 を実装した。5 は ws049-p017 の調べで kernel に口が無く HAL の API（`hal.h` か
`hal_get_arch_handoff` の新しい名前）が要ると分かり、ユーザーの判断待ち。それまで A3 は「UAT で `UBCB` と memory map の型を記録して確かめる」で
設計を進める（2026-10-04 Q1）。）

## 13. HPD・pin・向きの二つの出所（決定 4・5 と補足）

| 項目 | UCSI | i915（WS051） |
| --- | --- | --- |
| 向き（CC1/CC2） | 2.0 以上の GET_CONNECTOR_STATUS の field | TCSS・FIA の register から取れるかを実機で確かめる（下） |
| DP の HPD | 2.0 以上で PPM が返すなら（【仕様要確認】） | DE の HPD の live status（`GEN11_DE_HPD_ISR` の TC の bit、`TCSS_DDI_STATUS` の HPD_LIVE_STATUS_ALT）と割り込み |
| DP の pin の割り当て | 2.0 以上で PPM が返すなら（【仕様要確認】）、1.x は mode の VDO（能力）だけ | FIA の `PORT_TX_DFLEXPA1` の pin、`TCSS_DDI_STATUS` の pin の field、lane の割り当て `DFLEXDPSP` |

- **向きを i915 から取れるか**: Linux の i915 の register の定義（`i915_reg.h`）に plug の向きの bit は無い（`DDI_BUF_PORT_REVERSAL` は VBT の board の
  lane の反転で、plug の向きではない）。FIA の `DP_LANE_ASSIGNMENT`（1・2 lane の時に 0x1/0x2/0x4/0x8・0x3/0xC のどれか）が向きで変わる可能性が
  ある。WS051 p002 の診断で、DP の pin D（2 lane）の adapter を両向きに差して lane の割り当てと TCSS の register を記録し、向きと対応するかを
  確かめる。対応しなければ 1.x の向きは「不明」と公開し、取れないことを Q1 に報告する（ユーザーの決定 4 の前提が成り立たない）。
- **優先**: 画面を出す判断（link training、lane 数、HPD での modeset）は常に i915 の値で行う（実際に lane を割り当てているのは FIA で、DDI に届く
  HPD は TCSS のもの）。`/dev/typec` と利用者への状態では、両方ある時は i915 の値を採り、UCSI の値も並べて出す（`hpd=1(i915) ucsi=1` の形）。
  i915 が無い（i915 の driver が動いていない、QEMU）時は UCSI の値を採る。
- **食い違い**: 両方ある時に値が違えば、log に 1 回（変化ごと）両方の値と generation を出し、状態に `disagree` の印を付ける。HPD は一時的に
  ずれる（PPM の通知と HPD の割り込みの順が保証されない）ので、200 ms 待って両方を読み直してから食い違いと判定する。
- **対応付け**: i915 の TC の port（TC1・TC2）と UCSI の connector の番号の対応は未確認（A9 と同じ問題）。`_PLD` の位置、VBT の child、実機で
  一つずつ差して確かめ、対応表を typec の層に持つ（決まるまでは i915 の値を UCSI の connector に入れない）。

## ユーザーの決定（2026-10-04、§10 の 5 項目、クリックの回答を 1 つずつ）

1. 公開の口: 「/dev/typec の診断 text→正式は /dev/system」（UAPI を足さない診断の text の /dev/typec で始め、利用者への正式な通知は WS132 の /dev/system の事象（Keiland が受け取る）に任せる）。
2. role の切替（SET_UOR/SET_PDR）・CONNECTOR_RESET・Alternate Mode の選択（SET_NEW_CAM）: **「入れる」**（WS050 の範囲に残す。案の「範囲外」は不採用）。ws.md の目標は今のまま、Phase を足す。
3. UCSI の版: **「1.x と 2.x の両方」**（2.x の大きな MESSAGE_IN と追加の field も最初から扱う。配置は AML の region で決め、VERSION で field の意味を選ぶ。2.x の実機は無いので host の試験で）。
4. 向き（CC1/CC2）: **「1.x でも i915 から取る」**（1.x の時は i915 の TCSS の register から向きを取る。WS051 との連携が要る。取れるかは未確認で、p001 の改訂で確かめる。2.x では UCSI の field）。
5. HPD・pin: 「i915 の TCSS・FIA から」（UCSI は mode に入った事実と能力の補助。WS050・WS051 の ws.md を直し、WS051 は UCSI を待たずに進められる）。

補足（2026-10-04 ユーザー）:「DisplayPort の HPD と pin の割り当ては UCSI 1.x では得られませんが、2.0で取れる場合は取りましょう。i915から取れるなら取りましょう。両方実装があるといいと思います。」→ 5 の改訂: HPD・pin は両方の経路を実装する。UCSI 2.0 以上の PPM で取れる時は UCSI から取り、i915 の TCSS・FIA から取れる時は i915 からも取る（1.x では i915 だけ）。両方ある時の優先と食い違いの扱いは設計で決める。

## A3 の解決（2026-10-05、ユーザーの指摘で Q1 が source を確かめた）

ユーザー「device mapはMMIOのアドレスをマップするAPIですよね。RAM範囲をマップしないようにHAL実装ができているのでは？」→ そのとおり。amd64 の `hal_space_map_device` の実装（`src/hal/amd64/space.c` の `device_window_map`）は、要求の範囲の各 page を `amd64_ram_lookup` で調べ、RAM（allocator が配る範囲）が 1 page でも含まれれば `HAL_ERR_INVALID` で拒む（「Excludes all RAM, including bytes outside partial device boundary pages.」）。よって **HAL の API の追加は不要**: UCSI の driver は mailbox を `hal_space_map_device` で map し、拒まれたら attach しない（RAM に置かれた mailbox は使わない）。Q1 の前の説明（「確かめない」）は誤りだった。残る確かめ: (1) `ram_builder` に BOOT_RECLAIM などの再利用される範囲が入っているか、(2) `device_fixed_range`（boot の時の legacy の固定の写像）を通る範囲は RAM の確かめを通らないので、mailbox がそこに当たらないか。p002 の前に読みで確かめる。ws049-p017 の ⑤（memory map の型を問う口）は不要として閉じる。

2026-10-05: 決定 4 の改訂 → 向きは受け入れから外す（取れれば表示するだけ）。plan/ws051/design.md の §13 (2) の決定。
