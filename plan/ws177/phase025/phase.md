<!-- awesome-plan project=zedbsd record=ws177-p025 -->

# ws177-p025: 印刷の堅牢化の 4 — Settings で printer の名前・path・queue を変える（案 Q）

Parent: [WS177](../ws.md)
Status: in-progress（2026-10-08 夜 P2 q905）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q905（P2、2026-10-08 夜、承認済み）
Origin: [backlog-p2](../backlog-p2.md) の 127（ws145-p004）、[案](../phasing-20261008.md) の Q。番号は Q1 の割り当て（KL_VERSION 75、kl_system_manager_v1 version 24）。

## 実装（2026-10-08 P2）

- **protocol**（`libkeiland/system/kl-system-protocol.h`・`system-protocol.c`）: kl_system_printers_v1 の request 6 `edit(uint request, uint printer, string name, string path)`（since 24、name・path の "" はそのまま）。`KL_SYSTEM_MANAGER_VERSION` 24、`KL_SYSTEM_SINCE_PRINTER_EDIT` 24、printers の interface の版 24・7 requests。
- **compositor**（`wayland/printers-shell.c`）: version 24 以上の object の edit を受け、name は job の title と同じ規則（UTF-8・制御文字なし・127 byte まで）、path は空白と制御文字なし。外れれば result INVALID、backend へ。24 より前の object の edit は EPROTO。
- **backend**（`libkeiland-backend/print/print.c`・`keiland-backend.h`）: `kl_backend_print_edit`、writer の thread の変更 `PRINT_CHANGE_EDIT`（無い printer は EINVAL）。
- **libkeiland**（`keiland.h`・`system/system.c`・`exports.map` は `exports.py` で作り直し）: `kl_system_printers_edit`（KL_VERSION 75）。name は title と同じく 1 行に（制御文字を空白、127 byte で切る）、path に空白があれば EINVAL、24 より前の desktop は ENOTSUP。
- **Settings**（`settings/page-printers.c`・`settings.h`）: 各 printer の行に Edit。押すと「Edit a Printer」の card（副題に printer の名前、Name と Path or queue の欄に今の値、Save・Cancel）。Tab は card の 2 つの欄の間、Enter は Save、Esc は keyboard を返す。答えで「The printer is changed.」と card を閉じる。編集中に printer が消えたら card を閉じる。
- **printtest**: `printtest edit PRINTER NAME [PATH]`。
- **AAT**: `tests/scenarios/apps/settings/printers.md` に手順 4（printtest edit で名前と queue を変える）を足し、頁の撮影を 5 に。`helpers_printers.py` に同じ確認。

## 確認

- host（新規）: `sh plan/ws177/tests/host-page-printers.sh` → PASS（ASan・UBSan、widget と desktop の代わりで描いた物と依頼を記録: 各行の Edit、Edit で card と今の名前・queue と名前の欄の focus、打鍵・Tab・Enter で `kl_system_printers_edit(2, "Basement (LPD)x", "lpr")`、答えの間の Save の無効、答えで card が閉じ文が出る、Cancel、編集中の printer の消失、Add の card が残る）。
- host（追加）: `sh plan/ws177/tests/host-printers-shell.sh` に edit の 6 項目（OK と printer の event の新しい名前・queue、名前の C1 と path の空白は INVALID、無い printer は INVALID、version 23 の object は EPROTO）→ PASS。`plan/ws131/tests/host-system.sh` PASS（manager の版 24）。
- build: `make -j16 ZEDBSD_CONFIG=config/current-uat.mk BUILD=build/amd64 build/amd64/bin/wayland build/amd64/bin/settings build/amd64/bin/printtest build/amd64/bin/pdfviewer` exit 0・warning 0（libkeiland.so の exports の検査を含む）、`make -j16 keiland-linux` exit 0。style-check 指摘なし。
- QEMU: 未実施（T1 に apps.settings.printers の手順 4・5）。
