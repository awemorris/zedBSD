<!-- awesome-plan project=zedbsd record=ws177-p003 -->

# ws177-p003: UCSI の堅牢化の host の分（案 G）

Parent: [WS177](../ws.md)
Status: in-progress（2026-10-08 P1 q882 の 3: host の分を実装・host PASS・kernel build。実機の確認は 5330 の後）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q882 の 3（P1、2026-10-08、承認は Q1 の dispatch「G の host の分（backlog-p2 148〜150・155）: UCSI の堅牢化の、5330 なしで作れる所。記録の再生の host 試験と停止の道。実機の確認は 5330 の後（BUG-256 にも注意、触らない）」）
Origin: [backlog-p2](../backlog-p2.md) の 148（ws050-p002 核）・149（p003 ACPI の transport）・150（p004 操作）・155（p005 i915 との連携）、[案](../phasing-20261008.md) の G
仕様: UCSI 3.1（USB Promoter Group、2026-06）の text（§6 の運用の model、§6.5.2 CANCEL、§6.5.8 SET_CCOM、§6.5.18 GET_ERROR_STATUS、Table A-1）。1.2 は既存の code の引用だけ（文書は手元に無い）。
触らない: i915 の AUX・dp-ext（BUG-256）。

## 範囲（host で作れて確かめられる物）

### G-a 核の誤りの道（148・150）

1. **ERROR**（CCI bit 30）: 失敗した command の完了を ACK → `GET_ERROR_STATUS`（0x13）→ ACK。Error Information（Table 6-48 の 16 bit）を 1 行の log に名前で出し、`ucsi->error_information` に残す。順は Linux の `ucsi_read_error()` と同じ（先に ACK）。仕様は「PPM は ACK の後に Error Status を消してよい」と書くが、我々の one-command の読み方（次の command の前に完了を ACK）と Linux の実機での実績を採る（判断の記録）。connector の番号（bit 16）は 3.0 以上で、失敗した command が connector を名指す時だけ入れる（1.x・2.x は 0 = 予約、2.x の文書が無いので 3.0 から）。GET_ERROR_STATUS 自体の失敗は読み直さない（再帰しない）。
   errno の対応（Linux に沿う）: 存在しない connector → ENODEV、不正な引数・未知の command → EINVAL、相性の無い partner → EOPNOTSUPP、CC の通信の誤り・契約の交渉の失敗 → EPROTO、dead battery・partner の swap の拒否・swap の拒否・policy の衝突 → EPERM、他 → EIO。
   操作（request）の結果: record に `request_error`（errno）に加えて `request_error_information`（Error Information の bit）を置く（`drv_typec_request_finish` に引数を足す）。`/dev/typec` の text にも出す。
2. **BUSY のまま timeout**（5 s）: 最後の CCI が BUSY なら `CANCEL`（0x02）を送り、Cancel Completed（bit 26）か完了を待って ACK → 呼び手に ETIMEDOUT（PPM は健全）。CANCEL も答えない、または BUSY の無い無応答の timeout → `stuck`（PPM が止まった）。
3. **回復**（`stuck` の時）: 公開の入口（`drv_ucsi_service`・`drv_ucsi_request`・`drv_ucsi_poll`）の最後で `PPM_RESET` から start と同じ列挙をやり直す（通知の有効化・capability・各 connector・変化の通知）。続けて 3 回失敗したら `failed`（以後の入口は ENODEV を返す、transport が停止の道へ）。成功で回数を 0 に。実行中だった request の結果は ETIMEDOUT を record に残してから回復する。
4. **通知の取りこぼし**: 核は完了を通知で見たか（`notified_completions`）、refresh の読みで見つけたかを数える。新しい `drv_ucsi_poll`（refresh で CCI を読み、示された変化を処理）。新しい `drv_ucsi_notifying`（start 以降に通知で完了が 1 回でも来たか）。ACPI の thread は idle の待ちが何も無く終わる度に poll する: 通知が来る PPM は今の 60 s、来ない PPM は 1 s。通知が 1 回落ちても、示されて ACK されない変化は 60 s 以内に拾う（PPM は ACK まで次の変化を通知しないので、落ちると止まる）。
5. **記録**（再生の材料）: 核が始めから 128 回の mailbox の交換を log に機械で読める 1 行で出す（`ucsi: rec W control=%016llx` と `ucsi: rec R cci=%08x in=<hex>`、MESSAGE IN は CCI の長さだけ）。診断の口（coding-style §12 の診断の例外、試験の switch ではない）。5330 の起動の dmesg がそのまま 5330 の記録になる。

### G-b ACPI の transport（149）

6. **start の失敗**: thread が start を 1 s・2 s・4 s の間をおいて 3 回まで試す。それでも失敗、または核が `failed` → **停止の道**。
7. **停止の道**（`ucsi_acpi_stop`）: 操作の口を外す（`drv_typec_operator_set(NULL, NULL)`）、待っている request を全て ENODEV で終える（新しい layer の `drv_typec_requests_fail`、record を publish）、Notify の handler を外す（`drv_acpi_notify_remove`）、mailbox の写像を外す（新しい `drv_typec_os_unmap`、`hal_space_unmap_device`、HAL の API は不変）、connector の数を 0 に（`/dev/typec` は「無し」）、log `ucsi: stopped (why)`、thread は終わる。
8. **複数の UCSI の device**: 2 台目以降を見つけたら log に「無視」（layer が 1 driver だけを持つため、対応は後）。
9. **layer の lock の遅延の初期化の競合**: `typec_lock_ready` の bool を atomic な 1 回の初期化（0 未作成・1 作成中・2 済み、他は 2 になるまで待つ）に。

### G-c 操作（150）

10. **取り消しの口**: `drv_typec_request_cancel(serial)`: まだ取られていない request を列から外し、その connector の record に ECANCELED で publish。取られた（実行中・済み）なら EBUSY、知らない serial は ENOENT。列が満ちた時（DRV_TYPEC_REQUEST_MAX）の振る舞いを試験で固定。
11. **SET_CCOM**（0x08、3.1 §6.5.8）: 新しい request の種類（CC の動作: Rp だけ・Rd だけ・DRP・無効）。3.0 以上だけ（1.x の 0x08 は SET_UOM で中身が違う、2.x は文書が無い）、他は ENOTSUP。

### G-d i915 との連携（155）

12. 食い違いの log: 始まりの 1 回に加え、食い違いの中身が変わった時にも出す。
13. display の driver の停止で report を消す: 新しい `drv_typec_display_forget(port)`（known = false、比較をやめる）を ws177-p002 の `drv_i915_tc_stop` の後に i915 の tc-kern から port ごとに呼ぶ。

## 範囲の外（残す）

- 5330 の実の記録での再生（記録は 5 の log で 5330 の起動から取る。取れたら fixture に）、実機の測定（_Q79 と _DSM の交錯）、suspend・resume（WS052）、1.x の CONNECTOR_RESET の bit 23 と 2.0・2.1 の配置の文書での確認（文書が無い）、SET_CAM_PRIORITY（chunk、使う人がまだ無い）、GET_ATTENTION_VDO の IRQ_HPD、data role の現在の値、`_PLD`・VBT の対応の出所の実機での確かめ、i915 の向き。

## 試験（host）

- `plan/ws050/tests/ucsi-host.c`（master の Tools に無い WS050 の試験。WS050 は未完了なので WS の試験として直して足す）に: ERROR → GET_ERROR_STATUS の順と errno・record の bit、BUSY の timeout → CANCEL → ETIMEDOUT、無応答 → PPM_RESET と列挙のやり直し・3 回で failed、通知の無い PPM の poll、記録の書式を fake の run から作って再生する transport で同じ record になる、取り消し・列の満杯、SET_CCOM の版の分岐、食い違いの log、display の forget。
- `plan/ws050/tests/ucsi-acpi-host.c`（5330 の table）: start の失敗の再試行と停止の道（handler の除去・写像の解除・request の終わり）、2 台目の device の log。
- kernel の build（warning 0）。

## 設計レビュー（2026-10-08 design-reviewer）と反映

敵対的レビュー（H1〜H4・M1〜M7・L1〜L9）を受けて、上の設計を次のとおり改めた（上の本文は最初の案、ここが採った形）。

| 指摘 | 採った形 |
| --- | --- |
| H1 変化の読みが失敗すると ACK されず、PPM は次の変化を通知しない | `ucsi_pending_handle`: PPM が答えている（stuck でない）失敗なら log して change を ACK する（record は前の物のまま） |
| H2 回復の再試行が次の入口（60 s）まで起きず、enumerate の失敗を「回復済み」と数える | `ucsi_recover` の中で最大 3 回、500 ms・1 s の pause を置いて試す。enumerate がどの理由で失敗してもその試行は失敗。3 回とも失敗で `failed`（ENODEV） |
| H3 停止が原子的でない、thread が停止を見ない | layer に `drv_typec_driver_stop(error)`: 1 回の lock で stopped・kick を外す・待つ request を全て error で record に・全 connector の generation を上げ、lock の外で listener に知らせる。以後 put は ENODEV、publish は ENODEV。record は最後の状態のまま残す（count は 0 にしない、`/dev/typec` に「stopped」の行）。新しい start（connectors_reset に connector がある）で解除。thread は step の -ENODEV で止める |
| H4 cancel の publish が driver の publish に消される | request の結果の欄（serial・error・information）は layer の持ち物: `drv_typec_connector_publish` は driver の写しの値を使わず、layer の値を保つ。cancel は lock の下で欄と generation を書き、listener に知らせる |
| M1 errno の衝突 | 存在しない connector は EINVAL。EOPNOTSUPP は ENOTSUP と同じ値のまま、詳細は `request_error_information` で見る（決め） |
| M2 予期された失敗にも GET_ERROR_STATUS | `ucsi->tolerant` の間（SOP・SOP' の alt modes、CAM_CS、PDOs、cable）は理由を聞かない |
| M3 poll の起点・kick を通知と数える・S0ix | poll は PPM と最後に交換した時刻から: 通知が来る PPM 60 s、来ない PPM 5 s（1 s をやめた）。`ucsi_acpi_wait` は handler の flag だけを通知と返す |
| M4 SET_CCOM | **今回は入れない**（範囲の外へ）。入れる時の条件: bmOptionalFeatures bit 0、1 bit だけで capability の部分集合、PPM_RESET で DRP に戻る、command が起こした変化は通知されないので遅らせて読み直す |
| M5 失敗した request の後の request が待たされる | step は失敗を log して残りの request を続ける |
| M6 fake の mailbox が常に最新、記録の再生が自分の出力を読む循環 | 残す（限界として記録）。再生の試験は書式と決定性の確認まで、5330 の実の記録で意味を持つ |
| M7 forget の weak・generation・watch | i915 の tc-kern から weak extern で呼ぶ。forget は generation を上げ、dp_watch を消し、listener に知らせる |
| L1 Error Information 0 | 「理由なし（ACK で消された可能性）」の行を出す |
| L2 CANCEL が捨てられた時 | 元の command の完了をそのまま評価する（ETIMEDOUT にしない） |
| L3 記録の番号と終わり | 行に通し番号（W・R、I は R と同じ番号）、満杯で `rec N end` の行。再生は番号の飛びを数える |
| L4 lock の初期化 | atomic な 0/1/2 のまま（platform の init に置く案は採らない）。失敗の log は残す（無害） |
| L5・L6 unmap・attach の失敗 | `drv_typec_os_unmap`（`hal_space_unmap_device`、HAL の API は不変）、解除で mailbox を NULL。probe・notify の install の失敗は写像を返し、thread の start の失敗は停止の道 |
| L7 範囲の差 | 下の「範囲の外」に足した |
| L8 i915 の tc-kern を触る | `drv_i915_tc_kern_stop` に forget の呼び出しだけ（ws177-p002 の自分の関数）。AUX・dp-ext は触らない（BUG-256） |
| L9 BUSY の時間 | 5 s は BUSY を含む合計のまま（仕様は BUSY で timer をやり直してよい）。5330 での CANCEL の挙動は未確認 |

範囲の外（追加、L7）: driver の再起動（停止の後に attach し直す口は無い）、WS052 の resume の入口（`ucsi_recover` を公開して呼ぶ形が候補）、GET_CAM_CS の 2.x と index の意味、connector の変化の通知に付いた ERROR の GET_ERROR_STATUS、同じ connector への複数の request の結果の上書き（record は最後の 1 つ）、SET_CCOM・SET_CAM_PRIORITY。backlog 155 の「bind を呼ぶ物が無く」は古い（ucsi-acpi.c の map_displays が bind を呼ぶ）。

## 変更（2026-10-08 P1）

- 核 `src/drivers/typec/ucsi.c`・`ucsi.h`: `ucsi_execute`（完了を返し CCI を `ucsi->cci` に、記録の行、通知か poll かの数え）、`ucsi_command`（stuck の短絡、CANCEL、ERROR の ACK と GET_ERROR_STATUS、tolerant）、`ucsi_cancel`・`ucsi_error_status`・`ucsi_error_errno`・`ucsi_error_log`・`ucsi_control_connector`・`ucsi_settle`・`ucsi_recover`・`ucsi_enumerate`（start の列挙を分けた）・`ucsi_record_write`・`ucsi_record_read`、公開の `drv_ucsi_poll`・`drv_ucsi_notifying`、`ucsi_pending_handle`（H1）。旧の不具合も直った: PPM の ERROR で完了した command を ACK せずに次を送っていた。
- layer `typec.c`・`include/drivers/typec/typec.h`: record の `request_error_information`、`drv_typec_request_finish` の引数、`drv_typec_request_cancel`、`drv_typec_driver_stop`・`drv_typec_driver_stopped`、`drv_typec_display_forget`、publish が結果の欄を保つ、食い違いの中身が変わったら log し直す、text の stopped の行・information。
- OS `typec-kern.c`・`typec-os.h`: lock の atomic な 1 回の初期化、`drv_typec_os_unmap`。
- ACPI `ucsi-acpi.c`・`include/drivers/typec/ucsi-acpi.h`: start の 3 回の試し、停止の道 `drv_ucsi_acpi_stop`（公開）、quiet の poll、step の続行と -ENODEV、wait の通知の判定、2 台目の device の log、probe・install の失敗の解放。
- i915 `src/drivers/gpu/i915/display/tc-kern.c`: 停止で各 port の report を forget（weak）。
- 試験: `plan/ws050/tests/ucsi-host.c`（失敗の理由・3.0 と 1.2 の GET_ERROR_STATUS・errno、予期された失敗、CANCEL と遅い完了、回復と 3 回で failed、通知の無い PPM の poll、記録と再生（番号）、取り消し・結果の保持・失敗した変化の ACK・driver の停止と再開、食い違いの log し直し、forget）、`ucsi-acpi-host.c`（5330 の table: 停止で request が ENODEV・unmap 1 回・Notify が届かない・step と request の拒否、PPM が始まらない時の 3 回の start と停止）。

## 確認（host・build、2026-10-08）

- `make -C plan/ws050/tests OUT=build/p1-ucsi all` → ucsi-host PASS 124・FAIL 0、ucsi-acpi-host PASS 43・FAIL 0（ASan/UBSan）。`kernel-check` warning 0。
- kernel（warning 0、-Werror）: `make -j16 BUILD=build/p1-k ZEDBSD_CONFIG=config/current-uat.mk build/p1-k/vmunix`。`sh plan/ws051/tests/host-tc.sh` 112/0・34/0（i915 側の回帰）。
- style-check: `ucsi.c`・`typec.c`・`typec-kern.c`・`ucsi.h`・`typec.h`・`tc-kern.c` 0、`ucsi-acpi.c` 3 → 3（既存）。`git diff --check` 0。

## 未実施・残り

- 実機（5330）: 起動の dmesg の `ucsi: rec` の行（記録）を取り、fixture（`plan/ws177/tests/ucsi-record-5330.txt` の予定）にして再生する。CANCEL・GET_ERROR_STATUS の実機の挙動、idle の `_DSM` 2 の負荷、Notify の有無。QEMU には UCSI が無いので T1 の依頼は無し。
- 上の「範囲の外」。

## Event

2026-10-08 / q882-i03（P1）: 設計 → design-reviewer → 反映して実装、host・build の確認。merge 依頼を Q1 へ。
