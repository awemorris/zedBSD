<!-- awesome-plan project=zedbsd record=ws177-p017 -->

# ws177-p017: libbrowser に欄を autocomplete で探して focus する口

Parent: [WS177](../ws.md)
Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc §2））（旧: test-wait → Q1 の判定待ち（2026-10-08 q902 P1 の照合: T1-424 PASS（ZBROWSER MAIL one-time-code-field error=0、fill length=4 error=0、PNG は Q1 の目視））（旧: test-wait（2026-10-08 P1 q890 の 1: 実装・host PASS（plain・ASan）・zedBSD の build warning 0。使う側は [ws177-p018](../phase018/phase.md)）））
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q890 の 1（P1、2026-10-08）
Origin: [ws177-p014](../phase014/phase.md) の判断（backlog-p2 86 の後半）。2026-10-08 朝 ユーザー「libbrowser に要素を探して focus する口を足してよい（描画の改善は止めたまま）」

## 設計と変更（2026-10-08 P1）

- public の口 `int browser_view_focus_field(struct browser_view *view, const char *autocomplete)`（`userland/desktop/include/browser/browser.h`）: 文書の順で最初の、text を取る control（text・password の欄、textarea）で、focus でき、描かれていて、autocomplete 属性の token（ASCII の空白で区切る、大小を問わない）に指定の語を持つ物に、Tab と同じく ring 付きで focus を移し、caret を末尾に、見える所へ scroll する。page の focus の event が起きる。0 / ENOENT（無い、focus は動かない）/ errno（script・layout の失敗）。
- [browser component の規則](../../standards/browser-component.md) に沿う: 引数は C の文字列だけ（Wayland の型・event を出さない）。callback の中から呼ばない（同じ view の mutation、2026-10-02 の契約）を header に書いた。`BROWSER_API_VERSION` は 2 のまま（struct は変えず、export を 1 つ足すだけで、古い client はそのまま動く。利用側は browser の shell だけ）。
- 実装: `libbrowser/page/input.c` の `page_focus_field`（`input_focusable`・`input_rendered`・`dom_control_kind` を使う、token の照合は `input_has_token`、wide の文字列は読まない）、`page/page.h`、`view/view.c` の `browser_view_focus_field`（`view_update` の後、`view_scroll_into_view`）。描画の code は変えていない。
- 試験: `plan/ws177/tests/host-focus-field.{c,sh}`・`pages/focus-field.html`・`pages/no-field.html`（新、public の `<browser.h>` だけの client を host の libbrowser.so（`plan/ws074/tests/host-build.sh`）に link）。

## 確認（host・build、2026-10-08）

- `sh plan/ws177/tests/host-focus-field.sh plain` と `asan` → PASS 4: email の欄・非表示の欄・disabled の欄を飛ばし「section-x ONE-TIME-CODE」の欄に focus（`focus=b`）、数字の打鍵が入る（`value=42`）、欄の無い頁は ENOENT で focus の event が起きない。
- build（warning 0）: `make -j16 BUILD=build/p1-mailer ZEDBSD_CONFIG=plan/ws169/tests/config-amd64-mailer.mk build/p1-mailer/dynamic/libbrowser.so build/p1-mailer/bin/browser`。style-check: input.c・view.c の指摘は変更前と同じ 4 件（既存、自分の追加は 0）。

## 未実施

- QEMU（p018 の AAT と一緒に T1）。`plan/tools/browser-component/run.sh` の回帰は流していない（export の追加だけ）。
- iframe の中の欄、shadow DOM は探さない（engine の今の focus の順と同じ範囲）。

## Event

2026-10-08 / q890-i01（P1）: 実装と host・build の確認。
