<!-- awesome-plan project=zedbsd record=ws173-p002 -->

# ws173-p001・p002: AAT の入力の注入と画面の撮影（P1 の実装の記録）

Phase ID: `ws173-p001`・`ws173-p002`（2 つの Phase をこの 1 つの記録にまとめた。ws.md の行は Q1 が更新）
Parent: [WS173](../ws.md)
Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: p001・p002 とも T1-200 で aat-p002 PASS）（旧: in-progress（2026-10-05 P1 generation19 q776。実装・build・host 試験まで、T1 の QEMU の試験待ち））

## 実装

- p001（88700c5c）: UAPI `include/uapi/input-inject.h` に `INPUT_INJECT_KIND_MOUSE`（x_max・y_max 0 で相対、正なら絶対の pointer = QEMU の
  usb-tablet と同じ形。Q1 は承認の範囲と判断）・`INPUT_INJECT_KIND_KEYBOARD`（KEY_ESC〜KEY_COMPOSE、F13〜F24、明るさの 2 つ、repeat は拒む）、
  同時の open を 4 まで（1 open = 1 device）。`src/drivers/generic/input-inject.c`。道具 `userland/tests/aat-input`（server と socket
  `/run/aat-input.sock`、move-to・move・click・double-click・down・up・drag・wheel・hwheel・key・key-down・key-up・type・sleep）。
- p002: compositor の `userland/desktop/wayland/shot.c`（`ZEDBSD_TEST_SCREEN_CAPTURE=y` の時だけ link、他は `shot-none.c`。Linux・FreeBSD は
  常に none）。socket `$XDG_RUNTIME_DIR/keiland-shot.sock` か `/tmp/keiland-shot.<uid>.sock`（0600、compositor の利用者と root）。`PING`・`SHOT`。
  SHOT は次の frame の swapchain の image を render pass の後で buffer に copy（Vulkan readback）、fence の後に送る。swapchain は
  TRANSFER_SRC を付けて作り、駄目なら付けずに作り直して `ERROR unreadable`。道具 `userland/tests/keiland-shot`（PNG、RGB 8 bit、
  stored deflate）。docs `docs/reference/agent-acceptance-test.md`。

## 確認

- build（warning 0）: kernel（injector あり）、compositor（capture あり・なし）、`aat-input`、`keiland-shot`、`make keiland-linux`。
- host: `plan/ws173/tests/keiland-shot-host-test.sh` PASS（偽の compositor の socket、PNG を python で復号して画素を照らす）。
- QEMU（T1）: **未実施**。`plan/ws173/tests/aat-p002.sh`（image は `plan/ws173/tests/config-amd64-aat.mk`）。
- 実機: 未実施（i915 の swapchain の readback は AAT の最初の実行で確かめる）。
