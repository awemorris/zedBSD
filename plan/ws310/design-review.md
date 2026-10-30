<!-- awesome-plan project=zedbsd record=ws310-design-review -->

# WS310 設計の敵対的レビュー（第 1 版への review と反映）

- 対象: [design.md](design.md) 第 1 版（commit 41a915514）と [user-questions.md](user-questions.md)・[ws.md](ws.md)。
- review: design-reviewer の subagent（2026-10-31、P6 が起動）。file は変えていない。blocker 0・major 12・minor 9。
- review が読んだ物: acpi-kern.c・acpi-event.c・aml-sync.c・aml-eval.c・acpi-pci-root.c・acpi-pci-power.c・acpi-sleep.c・acpi-dev.c、pcat.c、pci-power.c、rtl8125.c、boot.c・main.c・boot.h、cpu-x86.c、5330 の dsdt・ssdt2/3/6/7/14・dyn-ssdt16〜21、5330-run.sh、`plan/ws031/display-ref/dmesg.txt`。aml-host で 5330 の表に PCIe の query（DWORD3 0x1D → 戻り 11/1f/18、`\OSCC` 0 → 0x18）・PCIe の commit（0x30 → `\OSCC` 0x30）・platform の query（0x64 → 戻り 11/60、`OSCP` は 0x64 のまま、`OSCO` 4、revision 2 は 0x0A）・processor の commit（DWORD2 0 → `PF00` 0x80000000 → 0）を評価した（harness の memory は 0 なので `U4SE`・`CFGD` は 0 で、`DOCM`・`CHCM`・Load の道は通っていない）。
- P6 は反映の前に主な指摘を code で確かめた: `enable_acpi_mode()` が `start_events()` の中（`acpi-event.c:1237〜`）、`DRV_ACPI_D3_COLD` の呼び手が `acpi-sleep.c` の外に無い、`rtl8125.c` の `disable_link_power_management`、`CPC3` の revision 3・23 要素（`dyn-ssdt19.dsl:209〜211`）、語の EINVAL で起動が止まる（`main.c:187〜195`）。Linux v6.12.5 の tree（`/home/awe/visionfive-linux`）で ACPI mode → `_INI` → platform → processor の順（`init/main.c:1100`、`drivers/acpi/bus.c:1313`・`:1367〜1388`）と `_OSC` の定数を確かめた。

## 指摘と反映

| ID | 節 | 要点 | 反映 |
| --- | --- | --- | --- |
| M1 | §4.3 | `_INI` の後・`start_events()` の前は ACPI mode（SCI_EN）の前で、Global Lock も共有前。5330 の GNVS・CPU NVS の `Lock` の field に書く | 反映。位置を `start_events()` と Global Lock の後、EC の attach の前に移した（§4.3・§5.1・§5.9）。harness に `--acpi-mode` を足し、前後を比べる（§6.1） |
| M2 | §4.3・§10 | PCI の driver の bind の後に firmware が TBT の Force Power・CM の切り替え・GPIO を行う | 反映。firmware が触る device と driver の扱いの表を §4.3 に、「USB-C に device を挿したまま起動」を §6.3 の必須に。前に呼ぶ案は §10 に理由つきで |
| M3 | §3.1・§5.6・§6.3 | 5330-run.sh は毎回 Debian を挟むので、CM の mode と CMOS の前提が毎回変わる | 反映。毎回の起動で `U4SE`・`U4CM`・`CMSK`・`SCME` と CMOS 0xF2 を記録、比べは同じ pre-boot の状態どうし（§6.3）。Q310-2 の背景に書いた |
| M4 | §3.1 | `STCM` の `SCME` の書き、`CHCM` の `DFPE`、`SCME` を読む TBT の電源の資源と D-state を読み落とした。`CHCM` の失敗は `_OSC` の戻りに出ない | 反映（§3.1 の 3、§5.11 の行、§6.1 の 5、§6.3 の S0 idle、§5.12 の WS052） |
| M5 | §4.2 | `_PR3`（bit 2）を申告する根拠（D3cold の道）が誤り。呼び手が無い | 反映。bit 2 は申告しない（§4.2・§10） |
| M6 | §2・§5.5 | 「ASPM を設定する code は無い」は誤り（rtl8125 が落とす） | 反映。§2 に既存の書きを列挙、§5.5 に落とす・復元・有効にするの 3 つの規則 |
| M7 | §4.2・§6.3 | PCIe の control 0x30 を使う code の無い今求めても確かめが空振り。support bit 1 との食い違い | 反映。control は WS300 の有効にする code が入るまで 0、その後に bit 1・0x30 と p007 で確かめる（§4.2・§5.12・§8）。Q310-4 の推しを直した |
| M8 | §6.3・§7 | `_CPC` は revision 3 で、FFH の記述子と CPU NVS の整数が混ざる。「一致」の判定が定まらない。AP の `_CPC` は PR00 の表に依る | 反映。§3.2 に要素と依存、§6.3 に要素ごとの判定 |
| M9 | §5.11 | platform の commit が失敗しても query で `OSCP` は書かれ、processor を呼べば `_CPC` が出る | 反映。platform が GRANTED・MASKED でなければ processor の段を飛ばす（§4.3・§5.11） |
| M10 | §6.3・§7・§8 | AC・lid・button・抜き差しは物理の操作で担当が行えない。遠隔の電源も無い | 反映。§6.3 を「script で自動（受け入れの必須）」と「ユーザーが拾える一覧（記録だけ）」に分け、止まった時の戻しを書いた |
| M11 | §9・§8 | guardrail は GPL の定数を出典つきで許すのに、merge を仕様の入手に縛り、p004 の行と食い違う | 反映。Linux v6.12.5 の header の定数を `Values referenced:` で使う道を採り、merge を仕様に縛らない（§9・§8 の p004） |
| M12 | ws.md | ws.md に 2026-10-29 の発言の原文がある（AGENTS §8） | 反映しない。Q1 の起動の指示が「ws.md の既にある原文の引用はそのまま残す」だったため。design.md §0 は「原文」の語を外し、扱いを Q1 への確認（§11 の 5）に挙げた |
| m1 | §4.3 | `_INI` の後の理由が違う。`GCAP` は `_INI` が決める `OSYS` を見る | 反映（§2・§3.2・§4.3） |
| m2 | §5.6 | 不正な語は起動を止める（安全側ではない）。列の語は `boot.c` の検査に向かない | 反映。on/off の 4 つの語に変え、誤記の振る舞いを正した（§5.6・§10） |
| m3 | §5.2・§5.5 | VMD の下は合成の segment。negotiate は公開の header でなく内部へ | 反映（§2・§5.2・§5.5） |
| m4 | §4.2 | `SPNT` の Notify の条件は `PFxx & 0x08` と `TCNT`。`_PTC` が変わる点 | 反映（§3.2・§4.2） |
| m5 | §2 | attach の行番号のずれ | 反映（`:190〜203`・`:205`） |
| m6 | §6.1・§6.3 | `_STA` は Method で `/dev/acpi` は評価する。Load は CFGD = 0 で試されもしない。「p002 の器具」の参照の誤り | 反映（§6.1 の 1 は Name・field unit だけ、§6.1 の冒頭、§6.3 の電力は WS333 の RAPL の meter） |
| m7 | §6.2 | q35 の `PCI0._OSC` は SSDT で置き換えられない。QEMU は LTR・hotplug を落とす | 反映。PCIe の失敗の型は host の ASL の試験、QEMU は q35 の物だけ（§6.2） |
| m8 | §7・§8 | 「有れば」の項で受け入れが決まらない。p006 が PASS を求めない。p005 の依存に Q310-3・4 が無い。起動の時間の閾値と CHCM の矛盾 | 反映（§6.3 の必須と任意、§8 の p003・p005・p006、起動の時間は CHCM の構成で記録だけ） |
| m9 | §4.2 | `cpuperf=off`・`cpuperf.hwp=off` の時の CPPC の申告。bit 0・9・11〜14・17・19 が表に無い | 反映。bit 5・6 は HWP を扱う時だけ、§6.3 に組を足し、全ての bit を表に載せた |

## review の総評（要点）

1. 5330 の AML の読みはほぼ正確で、query で状態を変える点も harness で再現できた。TBT の `SCME`・`DFPE` と、`OSYS` が `_INI` で決まる点を読み落としていた。
2. 主な弱点は、呼ぶ位置が ACPI mode の前で driver の bind の後である事（M1・M2）と、試験の道具が毎回 Debian を挟み物理の操作は担当が行えない事（M3・M10）。
3. bit の選びの根拠の誤り（`_PR3`・既存の ASPM の書き）と、`_CPC` の判定・出典の扱いの食い違いを直せば、「厳重」の中身が検証できる形になる。

## 推測のまま残る事（review と P6 の共通）

SMM が `OSCC`・`HWPV`・CMOS 0xF2 を読むか、ACPI_ENABLE の SMI が `_OSC` の結果を書き換えるか。仕様も firmware の source も無い（design §12）。
