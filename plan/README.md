# zedBSD planning entry

Awesome Plan is active in **GitHub mode**. Start with [configuration](config.md)
and [Guardrail](guardrail.md), then fetch Master/Queue/Past Log and relevant
WS/Phase records using [the sync procedure](tools/README.md).

[Master](https://github.com/awemorris/zedBSD/issues/1) ·
[Project](https://github.com/users/awemorris/projects/2)

- [Master cache](master.md): Objectives, Milestones and Current Focused Goals.
- [Queue](queue.md): **q308 finished** — active Queueなし。WS030 completed、WS014 p005の標準API訂正はcleared。
- [Future Work](future-work.md), [known bugs](known-bugs.md), [Past Log](history/index.md).
- wsXXX/phaseYYY: plans and durable evidence; only load the relevant branches.
- old/: superseded proposals and pre-Awesome policy; tmp/: import/deployment evidence.
  Neither is an instruction source or a script to replay automatically.
- [Migration status](migration-status.md): inherited limitations and current setup evidence.

## 採用時の履歴（2026-09-11）

以下のFocus・公開待ち・実行保留は当時の記録。現在状態はQueueと末尾の完了記録を参照する。

2026-09-11: Priorityリスト削除。WS025残件4件をユーザー判断でcleared、
WS025/WS019/WS006/WS022/WS002をcompletedとして閉鎖。後続指示による現行Focusはfg004（WS003の4機種インストーラ実機bring-up）。

Next session: inspect local changes and sync ownership → fetch current records
and decision comments → reconcile pending/conflicted edits → report current focus
and remaining work → follow the user's concrete instruction. Do not resume q303
or cancel/complete other work just because the management method changed.

[今回のbring-up計画](history/2026-09-11-installer-bringup-plan.md)。新規Issue/既存本文・Projectの公開は承認待ちでoutboxに保持。実行Queueなし。

現行Focusにfg005（WS005のネットワーク改善1〜3）を追加。[仕様と計画](ws005/network-improvements-2026-09-11.md)。fg004と両立し、順位・Queue未指定。両計画のGitHub公開は保留。

現行Focusにfg006（PC-9821V13起動改善）を追加。ユーザー指定の実行PhaseはWS003 p022→p023→p024。[具体化した計画](ws003/v13-boot-focus.md)。fg004/fg005は保持、実行Queueなし。

最新の実行状態はQueueおよびAGENTS/config末尾のq308 handoffを正とする。WS030新設、EGL今回cancel、Wayland/native i915後段。古いq303保留・採用時の未公開注記は当時の履歴。

## q308完了: 標準Vulkan・直接表示libraryと標準APIデモ（2026-09-13）

WS030 p001/p002/p003/p004とWS014 p005の標準API訂正をclearedとし、WS030 completed、q308 finished、active Queueなしとする。WS014はincomplete、p001/p004 planning、p004未queue、native i915は別WS029のまま。q307の旧scopeの実測と履歴は保持する。

`libc/include/vulkan/` にVulkan1.0の公開header、`userland/base/libvulkan/` に独立した全137 core＋選択direct-display WSI18の実装を提供し、`/lib/libvulkan.so` に配置した。vkdemoは標準Vulkan/WSIだけを使い、GPU ioctl/Venus codecをアプリへ持ち込まない。ABI、Noct再生成、155実exportとproc-address、全familyの限定意味論試験、U/Kの所有権・権限・失敗回収、適用C規約の独立レビューを実施した。正式CTS認証は主張しない。

最終 `q308-lifecycle-003` は実QEMU10.0.11/virglrenderer1.1.0/Intel ANVで6枚の回転直方体を描画し、実VNC/GPU readback/独立ray-texture oracleが一致（評価対象不一致0）。通常終了後6frame再起動、SIGINT後6frame再起動、640×480文字画面への復帰とechoによる画面更新、別processの表示競合拒否とowner35frame/DONEを確認した。42.671秒、QEMU exit0。最終書式変更後のkernel/appは実行済みbinaryと一致する。

承認済みHAL patch SHA256 `e6ec9e6c2deda41b840fa6f10846438d091f3a20ce782b9251b7979ac7591c8d` のみを適用し、既存hal_space_map_device/device usermapを補完した。追加HAL APIはない。PCI cache属性、queue総数63、allocator破棄、console/query/通知の修正と、先行失敗・再実行理由を保存した。公開coherent HOST_VISIBLE、256MiB aperture、native watchdog等の制約は能力監査へ記録した。

結果は `plan/ws030/results-q308.md`、155行の台帳は `plan/ws030/phase004/api-verification.md`、最終証拠は `plan/ws030/phase004/final-evidence/verification.json`、p005訂正は `plan/ws014/phase005/results-q308.md`、履歴は `plan/history/queue-q308.md`（いずれもlocal/uncommitted）。GitHubは計画Issue/Project/結果コメントの同期であり、source/doc/imageのgit add/commit/pushはユーザーが行う。EGLは今回cancel、Waylandは将来VK_KHR_wayland_surface backendとして追加する。
