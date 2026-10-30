<!-- awesome-plan project=zedbsd record=ws333-p018 -->

# ws333-p018: Dell の provider（DDV・熱の mode・受け手・SMM の boost の確かめ）

Status: in-progress（q1776-i01 の範囲（host の harness まで）を実装し確かめた。5330 の実機の試験が残る）
Parent: [WS333](../ws.md)
Queue: q1776-i01（P7）

## 目的

design.md §9 の p018（WS303 design §4.2・§4.5 と WS333 design §4.7・§4.8・§4.14 の Dell の部分）: Dell の laptop の firmware が WMI で出す口を、熱の core（p005）の fan・strategy の受け手・boost にする。

- DDV（8A42EA14…）の fan の回転数を fan として。
- BIOS の熱の mode（Optimized・Cool・Quiet・UltraPerformance）を SMBIOS の呼び出し（A80593CE…、class 0x11・select 0x13）で、strategy の受け手 "dell-wmi" として。
- SMM の fan の state（DIAG の F1DDEE52…）を boost として（確かめが通るまで既定で無効）。
- WMI の attach の試し直し（p017 の Q1 の判断: provider が自分の poll から 3 回まで）。

この Queue（q1776-i01）の範囲は host の harness の試験まで。5330 の実機で SMI を起こす試験は後（2026-10-31 時点で 5330 は止まっている、Q1）。

## 判断

- 2026-10-31 ユーザーから、昼頃まで不在の間は実装と試験を進める、という趣旨の指示を受けた（Q1 経由）。
- 2026-10-31 Q1 の判断（design.md §11.2 の 1）: Dell の SMM の失敗の印の値は、値（数）だけを、出典の block に書いて GPL の file から取ってよい。code は写さない。
- **置き場所**: WS303 §3 の案 `src/drivers/platform/pcat/dell-wmi.c` でなく `src/drivers/acpi/acpi-dell.c`（と純な部分の `acpi-dell-codec.c`、header `include/drivers/acpi/dell.h`）。理由: provider は自分で port・memory に触らず、全部を firmware の AML（WMI の method・`\ECRD`・`\ODV0`）経由で行う ACPI の WMI の使い手で、ACPI の OS の層（`aml-os.h` の log・timer・sleep）を使う。この置き場所なら同じ file が host の harness（`plan/tools/aml-host/`）で kernel と同じに動き、amd64 の vmunix には `src/drivers/acpi/*.c` の wildcard で入る。attach は `acpi-kern.c` の WMI の attach の直後（p017 で開発の管理情報を消した file。`src/kern/platform/pcat.c` に手を入れると ws004 の試験の symbol・log の文字列の扱いまで広がる）。
- **DDV の温度は sensor に足さない**（WS303 §4.15・§2.3: 5330 では acpi-thermal が読む DPTF の participant の `_TMP` と同じ値）。依頼の「DDV の温度」はこの判断の確認として扱い、method 0x22 は使わない。
- **GPL の file から取った値**（guardrail の「MIT・GPL の出典のデータの引用」、Q1 の判断。値ごとに source の block に「Value(s) referenced:」）。2026-10-31 に読んだ file（写しは /tmp/p7-ws333/gpl、git に入れない）:

  | file（Linux 6.12） | SHA-256 | 取った値 |
  | --- | --- | --- |
  | drivers/platform/x86/dell/dell-pc.c | 3a78f398…ff417 | 熱の mode の bit（balanced 0・cool bottom 1・quiet 2・performance 3） |
  | drivers/platform/x86/dell/dell-smbios-base.c | 5b232ab3…7d097 | SMBIOS の呼び出しの完了の status 0 |
  | drivers/platform/x86/dell/dell-smbios-wmi.c | 0601ee2f…6667b | SMBIOS の WMI の method ID 1（5330 の AML は見ない） |
  | drivers/hwmon/dell-smm-hwmon.c | bf5af2c6…5fb99 | SMM の fan の get 0x00A3・set 0x01A3、set の EBX（fan と state × 256）、失敗の印（EAX の下 16 bit が 0xFFFF）、WMI の method ID 1 |
  | include/uapi/linux/i8k.h（host の linux-libc-dev 6.12.107） | b1ba940d…1dd44 | fan の state の high 2（既定の最高） |
  | include/uapi/linux/wmi.h（同） | 135afb08…45945 | 呼び出しの buffer の output の位置 0x14 |

  手順は GPL の file から取っていない。「EAX が command のまま返る = SMI が走らなかった」は 5330 の AML（`\_SB.DIAG.C000`・`C001`、共有の記憶から register を写し戻す）から、SMBIOS の sentinel は WS303 §4.2 の設計から。
- **熱の mode の bit と名前**: bit の値は dell-pc.c、bit と `\ODV0` の 0〜3 の対は 5330 の `WMBA` の Case（AML、hardware の事実）。名前は BIOS の setup の物（Optimized・Cool・Quiet）だが、UltraPerformance は core の名前の枠（15 文字）に入らないので "Performance"。set の input1 の第 2 byte（音響の設定）は 0: 5330 の `WMBA` の Case が 1・2・4・8 だけを見る（第 2 byte が 0 でないと `\ODV0` が更新されない）ので、Dell の Windows の道具もこの形で呼ぶと読んだ（推定。5330 で確かめる）。
- **strategy の写し**（Q333-5 の推し A。master の「問いは推しで進める」）: PERFORMANCE → Performance、BALANCED → Optimized、POWERSAVE → Cool、MANUAL は noise ≤ 33 → Quiet、noise ≥ 67 かつ speed ≥ 67 → Performance、他は Optimized、FIRMWARE は書かない。
- **成否**（v5 M8）: output の 16 byte を sentinel 0xFF で埋めて呼び、全部が sentinel のまま → firmware が呼びを走らせなかった（ENXIO）として EIO を返し、以後 mode の呼びを出さない（EOPNOTSUPP。WS303 §4.2「延長 0 を firmware が受けなければ受け手を使わない」）。書き換わっていて output0 が 0 でない → EIO と status の log、今の mode は「不明」（次の SET で試す）。output0 が 0 → 成功（設計の sentinel に、dell-smbios-base.c の完了の値 0 を足した）。成功の後に `\ODV0` を読み、違えば log（表示の補いだけ）。
- **同じ mode なら書かない**（design §4.8）: 覚えた mode（起動の時は `\ODV0`、その後は成功した SET の mode）と同じなら SMI を出さない。起動の時の `\ODV0` を BIOS が埋めるかは 5330 で確かめる（埋めなければ `\ODV0` = 0 の Optimized を「今の mode」と誤り、BALANCED の最初の SET を省く。その時は起動の時の値を信じない形に直す）。
- **SMI の間隔**（design §4.8「EBUSY を返さず受け手が次の窓まで眠ってから書く」）: SMI を出す操作（mode の set、boost の 1 回の変更）の間を 1 秒あける。strategy の thread で `drv_acpi_os_sleep`。
- **attach と試し直し**（p017 の Q1 の判断）: Dell の記述子（8D9DDCBC…）の block が WMI の表にある時だけ受け手を登録する（記述子が "DELL"・" WMI" で、WMI の buffer が 0x2C 以上）。記述子・DDV の版・fan の読みが失敗した（AML の error、`\ECRD` = 0）片は、受け手の `poll`（1 秒ごと）が `drv_acpi_wmi_attach()` を呼び直してから probe し直す。3 回で諦め log 1 行。記述子の block がそもそも無い machine は試し直さない: Name の `_WDG` の読みは定数の buffer で一時に失敗しない（一時の失敗は method の評価と EC の側）。
- **`\ECRD`**（WS303 §4.2）: DDV の fan の読みの前に `\ECRD` を読み、0 なら読まない（AML が EC の 1 byte ごとに SMI を起こすため）。`\ECRD` の無い firmware は読む。
- **boost**（v5 M9 の直し方、手順は自分で組む。WS303 §4.2 の確かめ (1)(2) が 5330 で通るまで既定で無効）:
  - `CONFIG_DELL_SMM_BOOST`（Makefile、既定 n）。n の kernel は DIAG の `_WDG` を評価せず、fan の ops に boost が無い。harness は y で build する。
  - boost(L>0): 上げていなければ GET で今の state を読み、L 以上ならそのまま（SMI 1）。下なら「前の state」として覚え、max(L, 前の state) を SET し GET で読み直す（SMI 3）。読み直しが合わない・SMM が拒む（EAX の下 16 bit が 0xFFFF）・EAX が command のまま → その fan は以後 boost しない（boost_count 0、log 1 行）。
  - boost(0): 上げた fan だけ、前の state を SET で戻す（SMI 1）。
  - 戻されたかの推し（design §4.7「周期の SMI でなく DDV の rpm で」）: 上げた後の fan の読み（5 秒ごと）で、rpm が上げる前の rpm 以下が 3 回続いたら「firmware が戻した」として以後 boost しない（log）。SMM の GET は boost の SET の時だけ。
  - SMM の fan の番号は DDV の fan の並びと同じと仮定した（5330 は fan 1 つで差が出ない。5330 で確かめる）。
- **harness の拡張**: `--smi-memory A=HEX[,HEX...]`（呼びの SMI ごとに次の値、最後の値が残る。読み込みと `_INI` の間の SMI は最初の値）と `--dell`・`--dell-apply`・`--dell-poll`・`--dell-fan`・`--dell-boost`（熱の core の登録の stand-in が受け手と fan を覚え、core の thread と同じに呼ぶ）。

## 作った物

- `include/drivers/acpi/dell.h`（新）: mode の番号（`\ODV0` の番号）、`drv_acpi_dell_attach`、純な部分（`drv_acpi_dell_mode_for`・`mode_name`・`fans_decode`・`mode_request`・`mode_answer`・`fan_get_request`・`fan_set_request`・`smm_answer`）。
- `src/drivers/acpi/acpi-dell-codec.c`（新）: strategy → mode、DDV の fan の情報の decode（長さ 3n+1・3 byte の組・0xFF、長さ 0 は fan 無し、4 まで）、SMBIOS の buffer（0x2C、sentinel）と答えの検査、SMM の 4 register の buffer と答えの検査。AML も hardware も触らない。
- `src/drivers/acpi/acpi-dell.c`（新）: attach と probe（記述子・mode・DDV の fan・SMM）、受け手（apply・poll・firmware_name）、fan（read・boost）、試し直し、SMI の間隔。
- `src/drivers/acpi/acpi-kern.c`: WMI の attach の直後に `drv_acpi_dell_attach()`（ENODEV は log なし）。
- `Makefile`: `CONFIG_DELL_SMM_BOOST ?= n` と `-DCONFIG_DELL_SMM_BOOST`。
- `plan/tools/aml-host/`: `aml-host-dell.[ch]`（新）、`aml-host.c`（option と `--smi-memory` の値の列）、`Makefile`（Dell の 2 file と harness の file、`-DCONFIG_DELL_SMM_BOOST=1`）。
- `plan/ws333/tests/dell/`（新）: `test-dell-codec.c`（純な部分の単体試験）と `run.sh`（単体試験と 5330 の表の harness の試験）。

## 確かめた事

- `sh plan/ws333/tests/dell/run.sh`: 30 項目 PASS（`dell: PASS`）。
  - unit: 49 check（strategy の写しと manual の境 33/34・66/67、名前が 15 文字以内、5330 の fan の情報 `{4, 00 39 0f ff}` → 種類 0・3897 rpm、長さ 0・0xFF だけ・2 fan・長さ 5・buffer を越える長さ・0xFF の欠け・5 fan（4 を取り E2BIG）、Cool の SMBIOS の buffer の全 byte、4 mode の input1 の bit、sentinel のまま → ENXIO、status 0・-2・output0 が -1 のまま、短い答え、SMM の get・set の buffer の全 byte、EAX が command のまま・0xFFFF・0x1FFFF・2、短い答え）。
  - 5330 の DSDT・SSDT1〜15 の harness（`--events --ec --reg --init`、EC の fan の数・種類・rpm を `--ec-seq` で Debian の観測の値に）の SMI の数（呼びごと）:

    | 場面 | 結果 | SMI |
    | --- | --- | --- |
    | WMI の attach、Dell の attach（記述子・`\ODV0`・DDV の版・fan の読み、fan 1 と受け手を登録） | `dell: thermal mode Optimized`、`1 fans, fan 0 at 3897 rpm` | 0・0 |
    | DDV の fan の読み | 3897 rpm、boost_count 2 | 0 |
    | balanced（今の mode と同じ）／performance／performance（同じ）／powersave／manual 50:20:50（Quiet）／manual 80:80:0（Performance）／manual 50:50:50（Optimized）／firmware | 全部 error 0、mode の名前が順に変わり、`\ODV0` が毎回合う（log「the firmware shows」が無い）、2 回目からの set の前に「waits 1000 ms」 | 0・1・0・1・1・1・1・0 |
    | firmware が呼びを走らせない（`--smi-memory` 無し、output が sentinel のまま） | performance → EIO、次の powersave → EOPNOTSUPP（SMI 無し） | 1・0 |
    | firmware が status -2 を返す | EIO と status の log、mode は不明、次の SET も試す | 1・1 |
    | EC が AML に読めない（`--reg` 無し、`\ECRD` = 0） | fan を登録しない、poll が 3 回 probe し直して「gave up probing 0x4 after 3 retries」 | 全部 0 |
    | EC が fan 0 と答える（`{0, Buffer{0}}`） | 「the DDV lists no fan」、fan を登録しない | 0 |
    | boost 0:2（GET 1 → SET 2 → GET 2）、boost 0:0（前の 1 に戻す） | error 0・0 | 3・1 |
    | boost 0:2（GET が 2） | 書かない | 1、boost 0:0 は 0 |
    | boost 0:2 で読み直しが 1 | EIO、boost_count 0、次の boost 0:1 は EOPNOTSUPP | 3・0 |
    | SMM が拒む（EAX 0xFFFF） | EIO、boost しない | 1 |
    | 上げた後の rpm が前と同じ 3897 が 3 回 | 「the firmware put it back, no boost」、boost_count 0、boost 0:0 は SMI 無し | 3、読み 0、0 |
    | 上げた後の rpm が 3968 に上がる | boost_count 2 のまま、戻した log 無し | 3 |
    | Dell の記述子の無い表（SSDT1 だけ） | ENODEV、何も登録しない | 0 |

- わざと壊す確かめ（tree の写しを /tmp で壊して run.sh）: `\ECRD` の検査を外す → ec・ec-smi・ec-no-fan が FAIL、同じ mode を省く検査を外す → strategy-smi が FAIL、sentinel のままを成功と読む → untouched・untouched-smi が FAIL、boost の読み直しを外す → boost-take・boost-take-smi が FAIL。4 つとも検出。
- `make -C plan/tools/aml-host kernel-check`（kernel の target と flag、`-Werror`）: `src/drivers/acpi/*.c` 全部 warning 0。`acpi-dell.c` を `-DCONFIG_DELL_SMM_BOOST=1` でも同じ flag で compile して warning 0（最大の frame は `dell_apply` 120 B）。
- `python3 plan/tools/style-check.py`（新しい file・`aml-host.c`・`aml-host-dell.c`）0 件。`git diff --check` 0。
- harness の回帰（`aml-host.c` を変えたため）: `sh plan/ws333/tests/wmi/run.sh` PASS、`make -C plan/tools/aml-host` と `sh plan/tools/aml-host/check-latitude5330.sh` 5 項目 PASS（LTO の stack 7184 B）、`python3 plan/tools/aml-host/run-asl.py` 21 passed、`make -C plan/ws052/tests` と `sh plan/ws052/tests/run-host-sleep.sh` PASS。ws052 の Makefile は変えていない: `aml-host.c` の Dell の呼びを weak にし、Dell を link しない harness では `--dell` が「not linked」と答える。
- amd64 の vmunix（`sh plan/tools/ports/build-vmunix-all.sh /tmp/p7-ws333/b-amd64 amd64`）rc=0、warning 0、`drv_acpi_dell_attach` が vmunix にある（llvm-nm、boost の関数は既定 n で無い）。
- 全 20 port の vmunix（`sh plan/tools/ports/build-vmunix-all.sh /tmp/p7-ws333/b-all`、2923e980e と同じ kernel の source）: 20 port 全部 rc=0、warning 0。
- QEMU amd64（`make ZEDBSD_CONFIG=plan/ws333/tests/config-amd64-boot.mk BUILD=/tmp/p7-ws333/img disk-image` rc=0・warning 0、vmunix に `drv_acpi_dell_attach`。`BOOT_MODE=uefi-usb OUTPUT=/tmp/p7-ws333/boot plan/tools/boot-test.sh /tmp/p7-ws333/img/hdd-image.img`）: PASS、画面の PNG（`/tmp/p7-ws333/boot/login.png`）で login prompt を見た。q35 に PNP0C14 は無いので Dell の WMI が無い道（WMI の attach が ENODEV、Dell の attach が ENODEV で log なし）の回帰。console・serial の log は判定に使っていない。

## 未実施・残り

- **5330 の実機の試験**（この Queue の外、電源の入れ直しの後に Q1 が割り当てる。WS303 §6.3 の 1〜4、review m18 の合否と記録の分け）:
  - 前の確かめ（v5 M8、読みだけ）: Debian で `platform_profile` を set → 再起動 → 読んで、mode が再起動で残る事と、profile を書き直す daemon（power-profiles-daemon など）が無い事。残らなければ試験 3 の方法が使えないので Q1 へ。
  - 合否: (1) dmesg に `dell: thermal mode …`・`dell: 1 fans, fan 0 at N rpm`、受け手 "dell-wmi" と fan "dell-fan0" の rpm（idle の 0 も可、負荷の後に 0 でない）。(2) 4 つの mode を set して EIO が無い（sentinel が書き換わり status 0）。(3) zedBSD で set した mode が Debian の `platform_profile` で同じ名前に読める（bit と名前の対、音響の byte 0 で `\ODV0` が更新される）。起動の時の `\ODV0` が BIOS の設定の mode を映すか。
  - 記録だけ: mode ごとの 60 秒の負荷の rpm・温度・set の時間（SMI の停止の長さ）。
  - boost（`CONFIG_DELL_SMM_BOOST=y` の試験の build）: (a) state を上げた時に rpm が上がる、(b) さらに温度が上がった時に EC が state より上げる。両方が通れば既定を y に（その判断は Q1・ユーザー）、どちらかが通らなければ boost は「無し」のまま。安全の止め: TCPU が 90 °C を超えたら boost を 0 にして止める。SMM の fan の番号が DDV の並びと同じか。
- 熱の core の後半（p004 の UAPI・`/dev/thermal`・`/sbin/thermal`）が来るまで、受け手と fan は kernel の中だけで、user から読む口は無い（p005 の後半）。
- [plan/tools.md](../../tools.md) の aml-host の行に、option `--dell`・`--dell-apply`・`--dell-poll`・`--dell-fan`・`--dell-boost` と `--smi-memory` の値の列を足す事（tools.md は Q1 の file）。
- 規約の全文の見直しは p024。
