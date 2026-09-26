<!-- awesome-plan project=zedbsd record=ws068p006 -->

# ws068-p006: i915 実機での GL（GLX の zgears、App Home の X11、仮想デスクトップ）

Phase ID: `ws068-p006`
Parent: [WS068](../ws.md)
Status: in-progress（q484-i01）
Phase disposition: normal
Queue: q484-i01
承認: 2026-09-26 ユーザーの自律実行の指示（EGL/GLES・X11 を含むデスクトップ関連を優先）
設計: design.md §7（i915 では F-023 の不足が GLES の app に効く）

## 範囲

1. 事前の対策: gl_Position の書き換えの OpCompositeInsert（F-023 で i915 の compiler に無い）を OpCompositeConstruct に（Venus で確認済み）。
2. i915 実機の capture の scenario `zdesktop-x11`（plan/ws031/tests/i915-capture.py）: App Home から Gears（Xzed --rootless と GLX、
   固定機能の GL が i915 の Vulkan 実行器で動く）と X terminal、Ctrl+Alt+→・← でデスクトップ 2 と 1。`zdesktop-home` の icon の位置を
   6 つの app に。Xzed の log を disk へ。
3. 実機で起きた不足は、この Phase の範囲で直せるもの（変換層・shader の形）は直し、実行器・compiler に要るものは F-023 と Bug に記録する。

## 受け入れ

1. 実機（5330、i915、capture）で `zdesktop-x11` の各 check の結果と画像を記録する（通らなければ原因を記録）。
2. 直したものは Venus の回帰（egl-p008・x11-p005・zdesktop-p070）が通る。
3. 実機の LCD での目視は未実施でよい（capture の画像）。
