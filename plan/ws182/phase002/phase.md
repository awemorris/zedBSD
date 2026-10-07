<!-- awesome-plan project=zedbsd record=ws182-p002 -->
# ws182-p002: 実装 — 電源ボタンでメニュー、押下と解放

Parent: [WS182](../ws.md)
Status: uncleared（2026-10-08 Q1: T1-377 QEMU PASS（1〜5 の行、電源は切れない、PNG に card、T1 の worktree build/t1-377-out/p/）。残り: 実機（5320・5330）の短押し・1.5 s 長押しの gap_ms で押下/解放の推定と 2 s の窓を確定、D1 のユーザーの確認）
Disposition: normal
Queue: q861 / q861-i01
依存: [p001](../phase001/phase.md)（設計。D1 はユーザーの確認待ち、推奨のまま実装）

## 変更

| file | 内容 |
| --- | --- |
| `userland/desktop/libkeiland-backend-zedbsd/events-zedbsd.c` | `events_power_release`: 送った power-button の押下の後 `EVENTS_RELEASE_MS`（2000 ms、record の `time_ns`）以内の次の power-button の record を解放として落とし `KL EVENTS power-button release gap_ms=N`。sleep button は対象外 |
| `userland/desktop/libkeiland-backend/backend-private.h` | `power_release_due`・`power_press_ms` |
| `userland/desktop/wayland/backend-host.c` | `kwl_backend_power_button`: `KWL EVENT power button ms=N` → sleep の前後は `KWL POWER button skip reason=sleep` → 入力として数える（`lock_input_ms`）→ greeter・lock・dialog 表示中は `skip reason=greeter|locked|showing` → `kwl_power_dialog_open(server, "button")` |
| `power-dialog.c`・`power-layout.h`・`sleep.c` | 注記（power button が開く、zedBSD の session の Power Off・Restart は root・wheel） |
| `tests/scenarios/os/power/power-button.md` | メニュー・解放の gap・lock の手順に更新（ZWL → KWL） |
| `plan/ws182/tests/host-power-release.c` | host 試験（下） |

## 確認

| 確認 | コマンド | 結果 |
| --- | --- | --- |
| zedBSD の build | `make BUILD=build/p1-ws182/amd64 build/p1-ws182/amd64/dynamic/obj/userland/desktop/{wayland/backend-host,wayland/power-dialog,wayland/sleep,libkeiland-backend-zedbsd/events-zedbsd,libkeiland-backend/backend}.o`（`-Wall -Wextra -Werror`） | warning 0 |
| Linux の build | `make -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/p1-ws182/linux .../obj/userland/desktop/{wayland/backend-host,wayland/power-dialog,wayland/sleep,libkeiland-backend/backend}.o` | warning 0 |
| host 試験 | `cc -std=gnu17 -Wall -Wextra -Werror -fsanitize=address,undefined -I. -Iinclude -o build/p1-ws182/host/host-power-release plan/ws182/tests/host-power-release.c && build/p1-ws182/host/host-power-release` | PASS（短押し、3 s 後の押下、1.5 s・2.5 s の長押し、解放の後の素早い押下、sleep button） |
| シナリオの形 | `python3 plan/tools/aat/check-scenarios.py` | PASS |
| `git diff --check` | | 問題なし |
| QEMU（T1） | 下の依頼 | 未実施（T1 待ち） |
| 実機 5320・5330 | `tests/scenarios/os/power/power-button.md`（人の手） | 未実施 |

## T1 への依頼（QEMU、Q1 経由）

image: この commit の AAT の image（`plan/tools/aat/build-image.sh`）。QMP の `system_powerdown` を電源 button の押下として使う（QEMU の q35 は固定の power button の event で 1 回の押下に 1 record、解放は来ない）。
1. login 後、mark → `system_powerdown` → `KWL EVENT power button`・`KWL POWER dialog open source=button poweroff=1 restart=1`（wheel の kei）、撮影（暗い desktop と 4 つの button）。電源は切れない。
2. Esc → `KWL POWER choice=cancel via=escape`。
3. 3 s 以上空けて `system_powerdown` → 再び `dialog open source=button`（2 s の窓より後は新しい押下）。外の click で `choice=cancel via=outside`。
4. 窓の確認: `system_powerdown` を 2 回、0.5 s 空けて → 2 回目は `KL EVENTS power-button release gap_ms=` で落ちる（QEMU は解放を送らないので、これは窓が働く証拠。dialog は 1 回）。Esc。
5. Super+L で lock → `system_powerdown` → `KWL POWER button skip reason=locked`、dialog は出ない。
合格: 1〜5 の行が出て、電源が切れず、PNG が暗い desktop の中央に card。

実機（5320・5330、人の手）: 短押しで `KL EVENTS power-button release gap_ms=` が数百、1.5 s の長押しで約 1500（解放が 2 つ目の Notify という推定の確定）。

## 残り・制限

- D1（greeter・lock でメニューを出さない）はユーザーの確認待ち。結論が変われば直す。
- 1 回の押下に 1 record の機械（QEMU）では、2 s 以内の 2 回目の押下が落ちる（dialog が開いていれば元々何もしない）。2 s より長い押下の解放は押下として扱われ、dialog が開いていれば何もしない。
- 電源 button で wake した時、その解放が sleep の答えの 1000 ms より後に来ると、復帰の直後に dialog が開く（長く押したまま起こした時）。実機で見えたら窓を伸ばす。
- ws132-p008 の D1（短押しで S0i3）はこの WS で置き換わった。ws132 の記録の更新は Q1。Sleep をメニューに足すのは WS052。
