<!-- awesome-plan project=zedbsd record=ws050-p005 -->

# ws050-p005: i915 との連携（HPD・pin・向きの二つの出所）

Phase ID: `ws050-p005`
Parent: [WS050](../ws.md)
Status: in-progress（2026-10-07 P2: 範囲 1〜5 の正常系を実装、host ucsi-host 66・ucsi-acpi-host 28・host-tc 71・tables 34、kernel と I915_TESTS の build warning 0。実機の確認は ws050-p006（Type-C は QEMU に無い））
Phase disposition: normal
Queue: q834 の続き（P2、Q1 の ACK 2026-10-07「範囲 1〜5 で ACK、weak の口は coding-style に合う形で」）

## 範囲（ws.md の表、design.md §13）

`typec_display_report`、HPD・pin・向きの UCSI と i915 の二つの出所の統合、TC の port と connector の対応付け。

## 注（2026-10-07、ws050-p002 の仕様との照合から、設計の見直しが要る）

- **UCSI の GET_CONNECTOR_STATUS に DP の HPD・pin の field は無い**（3.1 Table 6-43。1.2 にも無い）。design.md §13 の「UCSI 2.0 以上で PPM が返すなら」
  の経路は、3.x の **GET_ATTENTION_VDO（0x16、DP の Attention VDO = HPD の状態）** と **GET_CAM_CS（0x18、今の mode の Configuration/Status = pin の割り当て）**
  で設計し直す。この 2 つが 2.0・2.1 にあるかは unconfirmed（2.x の文書が手に入らなかった）。
- 向きは 3.1 の GET_CONNECTOR_STATUS の bit 86（p002 で実装済み、2.0 で同じかは unconfirmed）。
- GET_ALTERNATE_MODES は 3.1 でも 1 回 2 つまで（2.x の大きな MESSAGE IN でも同じ。p002 は 2 つずつ offset を進める）。

## 範囲（2026-10-07 Q1 の ACK）

1. typec の層に i915 の TC の port ごとの display の記録（hpd、pin、lane 数、向き、generation）、`drv_typec_display_report`・`_get`、TC の port と connector の
   対応表（`drv_typec_display_bind`、既定は未対応、決まるまで i915 の値を connector に入れない）。
2. 統合: 対応がある connector は i915 の値を採り UCSI の値を並べる（/dev/typec に `hpd=1(i915) ucsi=1`）、200 ms 以上違えば `disagree` と log 1 回。
3. UCSI: version ≥ 3.0 で DP の mode の connector に GET_CAM_CS（と GET_ATTENTION_VDO）。
4. i915 → 層: tc-kern.c で readout の後と hotplug の work の connected の step（IRQ の外、tc の lock の外）、`CONFIG_DRIVER_TYPEC` 無しでも link する weak の口。
5. host と build。

## 調べ（2026-10-07、UCSI 3.1 の文書、P2 の scratchpad の ucsi.Upv9）

- GET_ATTENTION_VDO（0x16、§6.5.21 Table 6-55〜57）: command は connector 16〜22。data: alt mode の index 0〜15（0xFF は mode に無い）、VDO の数 16〜18、
  sequence 21〜23、VDM header 24〜55、VDO 56〜87（DP の Attention の VDO = DP Status）。bmOptionalFeatures の「GET_ATTENTION_VDO supported」が 0 なら not supported。
- GET_CAM_CS（0x18、§6.5.22 Table 6-58〜60）: command は connector 16〜22、current alt mode（GET_CURRENT_CAM の配列の index）24〜31。data: index 0〜7、
  Status 8〜39（DP は DP Status の VDO）、VDO の数 40〜47、VDO[N] 48 + 32N（DP は Configuration の VDO）。
- DP の VDO（VESA DP Alt Mode Table 5-3・5-4、Linux の include/linux/usb/typec_dp.h と同じ値）: Status の bit 7 = HPD の状態、bit 8 = IRQ_HPD。
  Configuration の bit 15:8 = pin の割り当て（bit 8 A、9 B、10 C、11 D、12 E、13 F）。i915 の FIA の DFLEXPA1 の値は 3 = C、4 = D、5 = E。
- 方針: HPD と pin は GET_CAM_CS だけで取れる（Status と Configuration）。GET_ATTENTION_VDO は IRQ_HPD の事象の時だけ要る（p005 では使わず backlog の候補）。

## 実装（2026-10-07 P2）

- 層（`include/drivers/typec/typec.h`・`src/drivers/typec/typec.c`）: `struct drv_typec_dp_state`（known・hpd・pin・lanes・向き）、`enum drv_typec_dp_pin`
  （A=1〜F=6、i915 の FIA の DFLEXPA1 と同じ番号）、display port（i915 の TC1〜TC4、`DRV_TYPEC_DISPLAY_PORT_MAX` 4）ごとの記録と対応表
  （`drv_typec_display_report`・`_get`・`_bind`、既定は未対応で i915 の値は connector に入らない）。connector の record に `dp_ucsi`（UCSI が埋める）と、
  層が写しの時に埋める `display_port`・`dp_display`・`dp`（採った値: i915 があれば i915、無ければ UCSI）・`dp_source`・`dp_disagree`。同じ report の繰り返しは
  generation を進めず listener にも知らせない（hotplug の work が繰り返すので）。
- 統合と disagree: hpd、両方に pin がある時の pin、両方が知る時の向きが違えば待ちを始め、`DRV_TYPEC_DISAGREE_MS`（200 ms）続いたら `dp_disagree` と
  log 1 回（`typec: connector N: DisplayPort disagree: display hpd=… pin=…, ucsi hpd=… pin=… (generation G)`）。一致で解ける。判定は report・publish・get・
  `drv_typec_display_check` で進め、待ちのある report は UCSI の thread を起こし、thread（`ucsi-acpi.c`）は `drv_typec_display_check` の残りの ms だけ待つ。
  時刻は `typec-os.h` の新しい `drv_typec_os_now_ms`（kernel は `kern_ticks_to_ms(sched_ticks())`、host の 2 つの試験にも実装）。
- `/dev/typec`: connector の行に ` display-port=1 hpd=1(display) ucsi=1 pin=C(display) ucsi=D lanes=4 disagree`（source の名は層の中立な名前 `display`・`ucsi`。
  ACK の例の `(i915)` を層では `display` と書く）、報告のあった display port ごとに `display-port 1: hpd=1 pin=C lanes=4 connector=1 generation=G` の行。
- UCSI（`ucsi.c`）: version ≥ 3.0 で、connector が DisplayPort の mode にいる時 GET_CAM_CS（0x18）。Status の bit 7 が HPD、Configuration の VDO の
  bit 15:8 の最も低い bit が pin。GET_CAM_CS は bmOptionalFeatures の Alternate Mode Details に含まれる（3.1 §6.7.3、その確かめの中で呼ぶ）。
  「current alt mode」には GET_CURRENT_CAM の返した値（connector の mode の index）を渡す（3.1 の文は配列の位置とも読める、unconfirmed、backlog）。
- GET_ATTENTION_VDO は実装しない（Q1 の ACK 2026-10-07）: HPD と pin は GET_CAM_CS の Status と Configuration で取れ、Attention の VDO が要るのは
  IRQ_HPD（sink の短い pulse）の事象だけで、p005 の範囲（HPD・pin・向きの統合）には要らない。backlog-p2.md の WS050 ws050-p005 の行。
- i915（`display/tc.c`・`tc.h`・`tc-kern.c`・`tc-kern.h`・`hotplug.c`）: `drv_i915_tc_dp_sample`（port の lock の中で live status、DP-alt で持つ時だけ FIA の pin と
  lane 数）、`drv_i915_tc_kern_report`（weak の `drv_typec_display_report`、`CONFIG_DRIVER_TYPEC` 無しの kernel では NULL で何もしない）。呼ぶ所は
  `drv_i915_tc_kern_start` の readout の後（宣言した port ごと）と `i915_hpd_tc_connected_step` の `drv_i915_tc_connected` の後（hotplug の work、IRQ の外、
  tc の lock の外）。向きは常に unknown（FIA の lane mask からは判定しない、design §13）。

## 確かめ（2026-10-07）

- host: `make -C plan/ws050/tests OUT=build/p2-ws050/host all` → ucsi-host 66/66（新しく first-dp-1.x と dp-* 18: 3.1 の PPM で GET_CAM_CS の HPD・pin D、
  未対応の port の report は connector を変えない、bind で i915 の値を採る、pin C と D の違いで 200 ms の待ち（100 ms で残り 100、同じ report は新しい generation
  にならず thread を起こす）、250 ms で disagree と log 1 回・text、pin D で一致、2 つ目の bind は EBUSY、範囲外は EINVAL、unbind で UCSI に戻る、unplug で
  UCSI の値が消える）、ucsi-acpi-host 28/28（5330 の table）。`sh plan/ws051/tests/host-tc.sh build/p2-ws051-host` → host-tc 71/71（dp-sample 4 を追加）・tables 34/34。
- kernel: `make -C plan/ws050/tests kernel-check` warning 0。`make -j16 ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=build/p2-ci vmunix` warning 0、
  vmunix の check PASS（LTO で `drv_typec_display_report` は `drv_i915_tc_kern_report` に inline されて link されたことを逆 assemble で確認）。
  `I915_TESTS=y I915_TEST_SET=execution`・`display_ktest`（build/p2-i915t）warning 0。
- style-check: 新しい code の指摘は tc.c の critical section の本体の段落（既存の形）だけ。
- 未実施: QEMU（UCSI・Type-C が無く意味のある試験が無い）、実機（ws050-p006、5330 が届かない）。

## 再開の情報

- p005 の範囲は実装済み。残りは backlog（対応の出所、IRQ_HPD、2.x の版の下限、index の意味）と ws050-p006 の実機の確認。

## q877（P1、2026-10-08）: TC の port と connector の対応の出所（`_PLD`）と起動時の bind

ws051 の監査（q874）で残りと分かった「対応の出所が無く、kernel から `drv_typec_display_bind` を呼ぶ所が無い」を実装した（ws177/backlog-p2.md:155 の行）。

### 5330 の table で分かったこと（plan/ws049/tests/latitude5330、iasl -d、読み取りだけ）

- UCSI の connector は `\_SB.UBTC.CR01`〜`CR0A`（ssdt8 `UsbCTabl`）。GNVS の `TTUP`・`TPnU`・`TPnD` の条件で作られ、`_PLD` は `TPLD(1, FPMN(n))`（visible、group position は GNVS の `TPnP` か `TPnT`、runtime の値）。
- i915（GFX0）の出力の device には `_PLD` が無い（ssdt6）。なので Linux の port-mapper のように display の connector の `_PLD` と直接は合わせられない。
- Type-C subsystem の xHCI の USB 3 の port `\_SB.PC00.TXHC.RHUB.SS01`〜`SS04` に Dell の table（ssdt12）が `_PLD` を付けている: SS01 = PLCA（visible、position 1）、SS02 = PLCB（position 2）、SS03・SS04 = PLDU（見えない）。TCSS の port n は display の TCn と同じ lane（FIA、ADL-P）。
- 注: SS0n の `_PLD` は Buffer を裸で返す（ACPI の決まりは Buffer の Package）。両方を受ける。

### 実装

- typec の層（`include/drivers/typec/typec.h`・`src/drivers/typec/typec.c`）: `struct drv_typec_location`、`drv_typec_location_decode`（`_PLD` の revision・visible bit 64・group token bit 86:79・group position bit 94:87）、`drv_typec_location_match`（visible で token と position が同じ connector がちょうど 1 つの時だけ。0 か 2 以上は NONE）。
- `ucsi-acpi.c` の `drv_ucsi_acpi_attach`: probe の後に `ucsi_acpi_map_displays`。
  - UCSI の device の直下の device を namespace の順に connector 0、1、… とする（Linux の `ucsi_find_fwnode` と同じ取り方。UCSI の connector の番号と CR0n の対応、design の A9 は推定のまま）。
  - TCn は `TXHC.RHUB.SS0n` の `_PLD` と同じ場所の connector に `drv_typec_display_bind(n−1, connector)`。
  - 合わない port は bind しない（i915 の値は connector に入らない、今までどおり）。
  - log は 1 port に 1 行: `typec: display port TCn: connector C (group G position P), bound` か `... not bound`。

### 確かめ

- host: `make -C plan/ws050/tests OUT=build/ws050-p005/host run` → ucsi-host 79 PASS（`pld-*` 7、`match-*` 6 を追加）。`... acpi` → ucsi-acpi-host 33 PASS。5330 の table に GNVS を `TTUP 2`・`TP1U`・`TP2U 1`・`TP1P 1`・`TP2P 2` と置き、attach で TC1 → connector 0（CR01）、TC2 → connector 1（CR02）、TC3・TC4 は bind なし。CR01 の `_PLD` は GNVS を読むたびに従う（`TP1P 3` で position 3）。`kernel-check` は警告なし、kernel（config/ci/config-amd64.mk）の build は rc 0、warning 0。
- QEMU: 意味が無い（UCSI・Type-C が無い）。T1 には頼まない。

### 要る採取（Q1 へ）

- 5330 の実際の GNVS の `TPnP`・`TPnT`・`TPnD`（CR0n の group position）は table に無い。新しい kernel を 5330 で起動した時の dmesg の `typec: display port TC1: ...`・`TC2: ...` の 2 行で分かる（Linux も GNVS も要らない）。
- 物理の確かめ: TC1・TC2 に順に USB-C の機器を挿し、`/dev/typec` で `display-port=N` の付いた connector が挿した connector になることを見る（ws050-p006 の実機）。
- 合わなかった時の案: 5330 の値で bind が外れたら、VBT の child の `dp_usb_type_c`・`usb_type_c` の番号を第 2 の出所にする（今は実装しない）。
