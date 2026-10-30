<!-- awesome-plan project=zedbsd record=ws250 -->

# WS250: nvrtx の終わらない GPU の仕事の取り消しと復帰（system は動き続ける）

<!-- awesome-plan-current:start -->
Status: incomplete（すぐ取り組む物。2026-10-27 ユーザー「すぐ取り組むものの一つです。」。2026-10-29 から試験なしで実装を進めている: q1676-i01 P5）
Primary Milestone: MG006
Parent: [Master](../master.md)
Related: [WS037](../ws037/ws.md)（nvrtx）、[WS291 p011](../ws291/phase011/phase.md)（非同期の submit、この WS はその上に作る）、[WS242](../ws242/ws.md)（Ampere/Ada、chid の管理）、`src/drivers/gpu/gpu.c`（共通の GPU の層の監視と drv_gpu_recovery_ops）、`src/drivers/gpu/i915/reset.c`（isolate の手本）
Resume point: 2026-10-29 P5（q1676-i02、worktree p5-ws250、先頭は記録の commit）。p002〜p005・p007 は cleared（実機 10.0.10.3 の RTX 2070、S4 の被害者の上限を直した 3ad5d28fc、規約の見直し f6c18a9c5）。WS の完了の前の残り（QEMU の boot の回帰、GPU の fault の app、isolate の道、GR の停滞を縮める RESET 先の案）は [p007](phase007/phase.md) の「WS の受け入れの確認」。Q1 が要否を決める。
<!-- awesome-plan-current:end -->

## ユーザーの指示（原文、2026-10-15）

（外部の質問「A verifier can catch malformed SPIR-V, but not a valid shader that loops forever. Without DRM's reset path, is a watchdog in your driver the recovery story for now?」への Q1 の説明の後）
「その場合のキャンセルと復帰を実装した方がよくない？device lostでプログラムがそれ以上動けなくなるのはOK。でもシステムとしては動き続けてほしいな。」

## ユーザーの回答（2026-10-29、クリック、Q1 の中継、design.md の第 2 版の改訂の問い）

- Q250-1「nvrtx の 2 s で STOP (推し)」、Q250-2（Q242-2 とまとめた問い）「WS305 の VFIO (推し)」。Q250-3 の順は Q1 が推しで決めた。
- → 解釈: 主の道は nvrtx の 2 s の期限で STOP、GSP の ctxsw の timeout の RC は同じ session の喪失として受ける back-up。実機の S1〜S5 は WS305 の VFIO で RTX を使える時。順は WS242 p003〜p005 → WS250 p002・p003 → WS291 の NVIDIA の段。

- 2026-10-29 ユーザーの回答（クリック、Q1 の中継）Q250-4「案 A を必須に」（推しでない方）。→ 解釈: session ごとの channel を必須の範囲に（design.md の第 4 版の改訂）。session ごとの worker を必須に含めるかは Q250-5 で聞く。

- 2026-10-29 聞き直し: Q250-2（WS305 の VFIO）の答えは前提の誤り（WS305 は止めたまま、passthrough の画面は screendump に写らない）で、WS242 の Q242-4 と同じ問いで聞き直した。ユーザーの回答（クリック）Q242-4「RDNA と差し替え (推し)」（WS250 の実機も同じ）、Q250-5「含める (推し)」。
  → 解釈: 実機は RDNA と差し替えた素の起動。session ごとの worker（第 4 版の p005）を必須に含める（必須 34 LW）。

## 今の振る舞い（Q1 の調べ、2026-10-15、main 5027043ab）

- executor は投入ごとに driver の fence を 2 秒待ち（`vk-node.c` の `VK_NODE_RUN_US`）、来なければ EIO → app に `VK_ERROR_DEVICE_LOST`（`vk-queue.c` の `queue_result`）。走り続ける shader は止めない。
- nvrtx の work channel は GPU に 1 本（`struct nvrtx_work_state`、全 session が共有）。詰まると後の投入（compositor を含む）も ring の空き待ち 2 秒で失敗する。
- 共通の層（`gpu.c` の監視）: job の期限（予約 10 秒・実行 60 秒・停止 10 秒）→ `stop_begin`/`stop_poll` → 確かめられなければ `isolate`（nvrtx は NULL）→ device 全体の `fault`（`runtime_fault` が node を faulted にし以後 EIO）。`reset` も NULL なので回復は reboot（GSP の unload）だけ。
- GSP の channel error の通知（`rpc.c` の `NVRTX_EVENT_CHANNEL_ERROR`）は log に出すだけ。
- 未確認: GSP の firmware が user の channel を自分で見張って止めるか。

## 目的と完了の条件

- 終わらない shader（または GPU の fault）を出した app は `VK_ERROR_DEVICE_LOST` で止まってよい。**他の app と compositor（desktop）は動き続ける**。
- 期限を過ぎた仕事を GPU の上で取り消す（channel の停止・作り直し、GSP の RM の口を使う）。
- 取り消しの後に GPU を使える状態に戻す（共通の層の `isolate`（その session だけ隔離）と、必要なら `reset`（device の作り直し）を実装する）。
- compositor の仕事が app の詰まった仕事の後ろで待たされない形（session ごとの channel か、取り消しで後ろの仕事を流し直す）を設計で決める。
- 試験: host 試験（chid・犯人の判定の scheduler の model・状態機械）と、RTX 2070 を差した 10.0.10.3（RDNA と差し替えた素の起動、`plan/tools/gpu-perf/machines/rtx.conf`）の実機で「終わらない fragment shader を描く vkhang → その app だけ DEVICE_LOST、desktop と mview は動き続け、別の app が描ける」（design.md 6.3 の S0〜S6）。

## Phase（p001 で確定、2026-10-29 design.md 第 6 版の 7 節。必須 34 LW、LW は ±50%）

| Phase | 内容 | 状態 | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 設計（design.md 第 6 版、design-reviewer 1 回） | cleared（2026-10-29 P5。第 6 版の直しは再 review を受けていない） | — |
| [p002](phase002/phase.md) | session ごとの channel、`rpc_lock` と drain の turn、chid の next-fit と host 試験 | cleared（2026-10-29 P5、q1676-i02。実機 S0） | p001 |
| [p003](phase003/phase.md) | 犯人の手がかり（GR の照合・始まりの印）、reaper の見張りと取り消し、RC の event、ops（stop_poll・isolate）、host 試験（scheduler の model） | cleared（2026-10-29 P5、q1676-i02。実機 S1〜S5、S4 の巻き添えを直した） | p002 |
| [p004](phase004/phase.md) | 実機の scenario の script（rtx-hang-check.sh、S0〜S6） | cleared（2026-10-29 P5、q1676-i02。実機で流した） | p002 |
| [p005](phase005/phase.md) | session ごとの lane（番号ごとの thread の pool）と executor の待ちの一部の lock の外出し | cleared（2026-10-29 P5、q1676-i02。実機 S0・S2） | p003 |
| p006 | 選択肢: `reset`（GSP の再起動）。既定は作らない（ユーザーの判断） | planned（選択肢） | — |
| [p007](phase007/phase.md) | 規約の全文と出典の監査、ws.md を完了の形に | cleared（2026-10-29 P5、q1676-i02。f6c18a9c5、WS の受け入れの残りは phase.md） | 全部 |


## 受け入れの残り（2026-10-29 Q1 の判断）

- QEMU の boot の回帰: nvrtx は QEMU（Venus）では読まれないので要らない（amd64 の vmunix の build は main で warning 0 を確かめた）。
- GPU の fault を出す app の試験と isolate の道: Future Work の候補（その app と stuck の出る条件が無い）。
- RESET を先にする案（取り消しの停滞 4.3 s → 約 2 s の見込み）: ユーザーの決定 Q250-1（STOP）から外れるのでユーザーに聞いている。答えまで WS250 は incomplete。

## 閉じ方（2026-10-30）

Q1 の案「QEMU の起動の回帰だけ流して完了、GPU の fault の app と isolate の道は Future Work」にユーザー「WS250は流してよいです。」。→ 次のセッションの始めに QEMU の起動の回帰（design.md 6.2）を流し、PASS なら completed の形に書き直す。
