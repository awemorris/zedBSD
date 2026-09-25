<!-- awesome-plan project=zedbsd record=ws030 -->

# WS030: 標準 Vulkan 1.0 と直接表示 library

<!-- awesome-plan-current:start -->
Status: completed
Completed: 2026-09-13（q308）
Primary Milestone: MG006
Related Milestones: MG002
Objectives: O2
Parent: [Master](../master.md)
Queue: なし
Resume point: なし（新しい要求は新しい WS として立てる。この WS は再開しない）
<!-- awesome-plan-current:end -->

## 目標

標準 Vulkan 1.0 の全 137 core 関数と direct-display WSI を、標準 header と `/lib/libvulkan.so` でアプリへ提供し、純粋な標準 Vulkan のアプリから実画面へ present できるようにする。

## 結果

公開 header・共有 library・dispatch、memory・同期・transport、全 core 関数と描画、direct-display WSI（KHR_surface・display・swapchain・display_swapchain）を実装した。QEMU＋virglrenderer＋Intel ANV で回転する直方体を描画し、終了・再起動・console への復帰・表示の所有権を確認した。設計は [vulkan-direct-display.md](vulkan-direct-display.md)。

## 制限・移管

正式な CTS 認証は主張しない。Wayland の backend は WS014 p006 が追加した。

## Phase 一覧

| Phase | 内容 | Status |
| --- | --- | --- |
| ws030-p001 | 公開header・共有library・dispatch | cleared |
| ws030-p002 | memory・同期・汎用transportとK支援 | cleared |
| ws030-p003 | 全Vulkan1.0 core・描画とdirect-display WSI | cleared |
| ws030-p004 | 全API意味論・適用規約と統合受け入れ | cleared |

## 記録の所在

各 Phase の計画・結果・試験は、2026-09-24 の plan 整理で削除した。git の commit `04bc9eab` 以前の `plan/ws030/` にある。Queue ごとの履歴は [plan/history](../history/index.md) に残る。
