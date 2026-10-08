<!-- awesome-plan project=zedbsd record=ws078 -->

# WS078: Kei Operating System への名前の移行

<!-- awesome-plan-current:start -->
Status: completed（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2.2）。旧: incomplete）
Primary Milestone: MG006
Parent: [Master](../master.md)
Queue: なし
<!-- awesome-plan-current:end -->

## 結果（2026-10-08 完了）

Kei Operating System への名前の移行: kernel・driver・UAPI・libc・bootloader の識別子（p002）、実行 file と source の directory（p003）、見える文字列と Kei の起動の画面（p004、残りは q875 で直し T1-397）、data の path・API・protocol（p006）、make の変数と protocol の header（p007・p008、T1-401・403）。

## 制限・移管

全文規約の確認（p005）はベータ3。

## Phase

| Phase | 内容 | 最終の状態 |
| --- | --- | --- |
| ws078-p001 | 棚卸しと対応表（識別子・path・見える文字列・protocol 名を分類し、新しい名前を決める。判断が要る点を列挙） | cleared（2026-10-08） |
| ws078-p002 | kernel・driver・UAPI・libc・bootloader の識別子の改名（`KERN_` 等。機械的。build の warning 0 と boot test） | cleared（2026-09-28） |
| ws078-p003 | 実行ファイルと source の directory の改名（userland/desktop/、`/bin/wayland` 等）と参照 | cleared（2026-09-28） |
| ws078-p006 | データの path（/etc/keiland、/usr/share/keiland、font の keiland*.ttf、/usr/libexec/keiland-x11）、AP | cleared（2026-09-28） |
| ws078-p004 | 見える文字列: boot の logo（Kei）・greeter・lock・banner・os-release 等 | cleared（2026-10-08） |
| ws078-p005 | 全文規約確認と回帰（必須の最終確認） | planning → 全文規約はベータ3 |
| ws078-p007 | ws078-p007（提案）: 注釈の `zdesktop` を一度に置き換える | cleared（2026-10-08） |
| ws078-p008 | ws078-p008: make の変数と protocol の header の file 名の旧名（[guide.md](../guide.md) §2.3 の B2・B3） | cleared（2026-10-08） |

Phase の directory とこの WS だけの試験は、完了の規則（AGENTS.md）で削除する（git の履歴に残る、削除は Q1）。
