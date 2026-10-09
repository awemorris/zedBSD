<!-- awesome-plan project=zedbsd record=beta2-triage -->

# ベータ2 の残り作業とトリアージ（2026-10-09 Q1、同日更新）

公開は 10/17（OSC 当日、朝から会場）なので、**10/16 中にリリースの準備を終える**。作業日は 10/09〜10/16。
体制: P1（実装・debug）＋T1（QEMU の試験）＋ユーザー（5330 の UAT・判断）。この表は計画で、Queue の承認ではない。
見積もりは LW（Logical Week、このプロジェクトの単位で 1 LW ≈ エージェントの実時間 20 分。master の 2026-10-06 の見直しと同じ）。完了した項目は消す。

## 日程

| 日 | やること |
| --- | --- |
| 10/09〜10/12 | 試験待ちの物を T1 で流して直す。5330 の UAT（下の「実機」をまとめて） |
| **10/13** | **機能の凍結の目標**（2026-10-09 ユーザー）。release notes・既知の問題をまとめる。ベータなので UAT の Bug を直し切れなくてよく、code freeze はぎりぎりまで行わないこともある |
| 10/14〜10/15 | 最終回帰（QEMU）と 5330 の確認、出た Bug を「直す／既知の問題に書く」で仕分け |
| **10/16** | 最終の image・配布物・license の一覧・release notes を確定。WS143・WS083 が間に合わなければここで OFF。公開の手順の確認（公開はユーザーの指示で） |

## 必須

| 項目 | 状態 | LW | 担当 |
| --- | --- | --- | --- |
| [WS129](ws129/ws.md) p005 release notes と既知の問題（Bluetooth・Vulkan Video は条件付き） | P1 下書き中 | 3 | P1 |
| [WS129](ws129/ws.md) p013 利用の手引きの更新（menuconfig・keiland の prerequisites） | P1 次 | 1.5 | P1 |
| [WS129](ws129/ws.md) p002 license の一覧を release の image の rootfs で確かめる | 一覧は再生成済み（27 components、open 0） | 0.5 | P1 |
| [WS129](ws129/ws.md) p006 最終回帰（release の image） | 10/14〜 | 3 | T1 |
| [WS129](ws129/ws.md) p008 公開の準備（tag・CI・配布物の確認） | 10/16 | 1.5 | P1・Q1 |
| T1 の未実行の試験 15 本: T1-493・494（再試験）、T1-496 [WS192](ws192/ws.md) パネル、T1-497 [WS193](ws193/ws.md) menuconfig、T1-498 [WS194](ws194/ws.md) prerequisites、T1-495 Python、T1-477〜484・486 | 実装済み・試験待ち | 8 | T1 |
| 上の試験で出る FAIL の直し | — | 4 | P1 |
| WS192・WS193・WS194 の UAT の指摘の直し | — | 3 | P1 |
| 5330 の UAT（WS192 パネル・BUG-253 蓋・BUG-269 ESP の書き込み・WS183 タップ・WS187 ロック画面・WS161 YubiKey・WS090 の測定・WS177・BUG-222 の速度） | ユーザー待ち | —（ユーザーの時間） | ユーザー |
| UAT で出る Bug の debug の枠 | — | 10 | P1 |
| T1-481 の needs-person の PNG 11 枚（build/review/bugsweep/）の判定 | ユーザー待ち | —（15 分） | ユーザー |

## 入れる（間に合わなければ直前で OFF、2026-10-09 ユーザー）

| 項目 | 状態 | LW | 担当 |
| --- | --- | --- | --- |
| [WS143](ws143/ws.md) Bluetooth の HID（BR/EDR・LE のキーボード・マウス）。release の config に入れ済み | 実装・host 試験済み。T1 の再試験・5330 の確認・直しが残る | 4.5 | P1・T1・ユーザー |
| [WS083](ws083/ws.md) Vulkan Video（H.264） | T1-435 と FAIL の直し（不確実）、p007 の hang の道具と実機 | 3.5 | P1・T1 |

## 直す（UAT で出た物の中で小さく効果の大きい物、直し切れなくてよい）

| Bug | 症状 | LW |
| --- | --- | --- |
| [BUG-184](bugs/BUG-184.md) | Settings で WiFi をオフにできない | 1 |
| [BUG-235](bugs/BUG-235.md) | Log Out の icon で確認なしに終わる | 1 |
| [BUG-232](bugs/BUG-232.md) | App Home で起動中の app を選ぶと切り替わらない | 2 |
| [BUG-219](bugs/BUG-219.md) | title bar の題と menu の字が同じ | 1 |
| [BUG-237](bugs/BUG-237.md) | app の icon の白抜きが透過 | 1 |
| [BUG-179](bugs/BUG-179.md)・[BUG-180](bugs/BUG-180.md) | 最大化の遅れ・最大化の解除のドラッグ | 3 |
| [BUG-271](bugs/BUG-271.md) | IPP で Brother に PDF を送ると断られる（LPD は動く） | 2 |

## 合計

| 区分 | LW |
| --- | --- |
| 必須 | 34.5 |
| 入れる（WS143・WS083） | 8 |
| 直す（Bug） | 11 |
| **計** | **53.5 LW**（約 18 時間。P1 と T1 が並行するので 7 日の中に余裕がある。UAT の待ちは含まない） |

## 既知の問題に書いて出す（ベータ3 以降）

| 項目 | 理由 |
| --- | --- |
| BUG-189・BUG-212（有線と WiFi の同時接続）、BUG-217（最大化の session の状態）、BUG-223（動画の全画面）、BUG-205（太字の font） | 設計の変更が要る |
| BUG-255（蓋を閉じた間の HDMI）、BUG-159（電池で 5 fps）、BUG-145（AX211 の DHCP）、BUG-165（5330 の DSDT） | 調査が長い・実機の時間が要る |
| [WS001 p045](ws001/phase045/phase.md) POSIX の header の残り・p046〜p051、[WS126](ws126/ws.md) Python | ベータ3 の列（合間に P1） |
| 規約の全文の見直しの Phase（各 WS） | ユーザーの決定でベータ3 |

## 運用

- 凍結の目標の 10/13 の後も、ベータなので UAT の Bug の直しは 10/16 の準備に間に合う範囲で続ける。新しい仕様の変更は「ベータ3 に回すか」を Q1 がユーザーに聞く。
- 毎日の終わりに Q1 がこの表を更新し、完了した項目を消す。
