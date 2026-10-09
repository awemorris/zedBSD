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
| **10/16** | 最終の image・配布物・license の一覧・release notes を確定。WS143・WS083 は必須（2026-10-09 ユーザー）。間に合わなければここで OFF。公開の手順の確認（公開はユーザーの指示で） |

## 必須

| 項目 | 状態 | LW | 担当 |
| --- | --- | --- | --- |
| [WS129](ws129/ws.md) p005 release notes と既知の問題 | 下書き済み、ユーザーの review 待ち・comment の整理 | 1 | P1・ユーザー |
| [WS129](ws129/ws.md) p013 利用の手引きの更新 | 下書き済み、ユーザーの review 待ち | 0.5 | P1・ユーザー |
| [WS129](ws129/ws.md) p006 最終回帰（release の image） | 10/14〜 | 3 | T1 |
| [WS129](ws129/ws.md) p008 公開の準備（tag・CI・配布物の確認） | 手順を phase.md に用意済み（host の確かめ PASS）、実行は 10/16 | 0.5 | P1・Q1 |
| T1 の未実行の試験 9 本: T1-499〜503（BUG-234・BUG-188・IPv6・音量の回帰・Bluetooth の回帰・FreeBSD の prerequisites の再試験）、T1-495 Python、T1-477・483・484 | 2026-10-09 夜: 480・482・486・493・496・497 は済み、498 は Debian PASS | 5 | T1 |
| 試験の FAIL の直し（T1-494・479・478・498 の分は直して再試験中、次に出る物の枠） | — | 3 | P1 |
| WS192・WS193・WS194 の UAT の指摘の直し | — | 3 | P1 |
| 5330 の UAT（下の「UAT の確認項目」） | ユーザー待ち | —（ユーザーの時間） | ユーザー |
| UAT で出る Bug の debug の枠 | — | 10 | P1 |
| [WS143](ws143/ws.md) Bluetooth の HID（BR/EDR・LE のキーボード・マウス）。release の config に入れ済み | 実装・host 試験済み。T1-502 の回帰・残りの Phase・5330 の確認と直し | 4.5 | P1・T1・ユーザー |
| [WS083](ws083/ws.md) Vulkan Video（H.264） | host の残り（p007 hang の道具・p008 性能と門）→ T1-435（5330 の実機）と FAIL の直し（不確実）。直前に OFF にする門の手順を用意 | 3.5 | P1・T1 |
| T1-481 の needs-person の PNG 11 枚（build/review/bugsweep/）の判定 | ユーザー待ち | —（15 分） | ユーザー |

## UAT の確認項目（ユーザー、5330 の release の image）

迷ったら PNG か一言を Q1 へ。✔ は動けば OK、✘ はその場の様子（何をしたら何が起きたか）を教えてください。

| # | 項目 | 手順（自明でない物だけ） | 期待 |
| --- | --- | --- | --- |
| 1 | [WS192](ws192/ws.md) 状態の島の操作パネル | 右上の icon の島を指で tap（mouse の click でも） | 右上に glass のパネル。Wi-Fi の switch、音量の slider と mute、Input の行の tap で言語の切り替え、Wi-Fi の「›」で AP の一覧。外の tap・Esc・島の再 tap で閉じる |
| 2 | [BUG-188](bugs/BUG-188.md) Wi-Fi の 1 回の tap | Settings → Wi-Fi で保存済みの AP の行を 1 回だけ tap | 接続が始まる（2 回の tap は要らない） |
| 4 | [BUG-234](bugs/BUG-234.md) Files から program | Files で /bin を開き `ls` を開く | Terminal が開き、ls の出力と終了の案内が残る |
| 5 | [WS183](ws183/ws.md) touchpad の tap | 1 本指の tap、素早い 2 回の tap、tap の直後に指を置いて動かす | tap はすぐ click（遅れを感じない）、2 回は double click、最後は drag |
| 6 | [WS187](ws187/ws.md) ロック画面 | (a) 手動で Lock、(b) 蓋を閉じて開ける・放置で自動 lock（5 分以内） | 時計が中央より上に大きい。画面の下の方から上へ swipe（touchpad・touchscreen）か mouse の wheel を上で解除の画面。(a) は必ず認証、(b) は 5 分以内なら swipe だけで解除 |
| 7 | [WS161](ws161/ws.md)・WS172 YubiKey | YubiKey 5（USB）を挿し、ロック画面か login で Hardware Key を選び鍵に触れる。NFC は ACR1252U に YubiKey 5 NFC を置く | 解除・login できる。PIN・Password の選択も出る |
| 8 | [WS143](ws143/ws.md) Bluetooth のキーボード・マウス | Settings → Bluetooth で BR/EDR（従来型）と LE の機器をそれぞれ pairing、文字を打つ・pointer を動かす。その間 Wi-Fi も使う | 入力が効く。Wi-Fi が切れない。✘ なら 10/16 に OFF |
| 9 | [WS083](ws083/ws.md) Vulkan Video（H.264） | T1-435 の後に Q1 が案内。Video Player で H.264 の mp4 を再生 | 映像が出て止まらない。✘ なら 10/16 に OFF |
| 10 | [BUG-253](bugs/BUG-253.md) 蓋 | HDMI を挿したまま蓋を閉じ、開ける。起動ごとに違うことがあるので 2〜3 回の起動で | 閉じると HDMI へ、開けると戻る |
| 11 | [BUG-222](bugs/BUG-222.md) USB LAN の速さ | 別の PC から USB LAN（ue0）経由で大きい file を scp。Settings の Network で link の速度を見る | 速さを教えてください（前回 950 KB/s）。link の速度が出る |
| 12 | [BUG-269](bugs/BUG-269.md) ESP の書き込み | Q1 が SSH で kernel を ESP に書く。ユーザーは止まった時の電源の再投入だけ | SSH が止まらない |
| 13 | [WS090](ws090/ws.md) 描画の速さ | desktop で範囲選択の枠を drag、Text Editor・Files で scroll | もたつかない（体感で、遅い所を教えてください） |
| 14 | WS177 準正常系（USB-C・PIN・手書き・Notes） | USB-C の monitor・充電器を数回抜き差し／Terminal で `fidoctl` の PIN の設定／Notes の手書きで tap と書き込み／Notes の Save Clean Copy を PDF Viewer で開く | 固まらない・PIN が画面に出ない・手書きが崩れない・PDF が開ける |
| 15 | [WS193](ws193/ws.md) menuconfig（host） | 自分の PC で `make menuconfig` → Build boot image | 新しい階層、進捗の bar と今の対象の名前。できた image が起動 |
| 16 | [WS194](ws194/ws.md) keiland-linux（Debian など、任意） | `make keiland-linux` | 足りない package を y/N で聞く、build の後に install を y/N で聞く |
| 22 | [BUG-189](bugs/BUG-189.md) 有線と Wi-Fi の両方の接続 | USB LAN と Wi-Fi の両方をつなぎ、Settings → Network の Active Network を見る | USB LAN（有線）が出る。再現しなければ close（2026-10-08 ユーザー） |
| 23 | [BUG-212](bugs/BUG-212.md) 有線を抜いた後の Wi-Fi | 有線の接続中に Wi-Fi もつなぎ、有線の cable を抜く | Wi-Fi が切れずに使える。再現したら Wi-Fi の off・on の前に Q1 へ（SSH で networkd と intel-ax211 の log を取る）。再現しなければ close |
| 17 | 写真の判定 | build/review/bugsweep/ の PNG 11 枚（T1-481 の needs-person） | 見た目が正しいかを OK／NG で |

## 合計

| 区分 | LW |
| --- | --- |
| 必須（WS143・WS083 を含む） | 38 |
| **計** | **38 LW**（約 13 時間。P1 と T1 が並行するので 7 日の中に余裕がある。UAT の待ちは含まない） |

## 既知の問題に書いて出す（ベータ3 以降）

| 項目 | 理由 |
| --- | --- |
| BUG-217（最大化の session の状態）、BUG-223（動画の全画面）、BUG-205（太字の font） | 設計の変更が要る |
| BUG-255（蓋を閉じた間の HDMI）、BUG-159（電池で 5 fps）、BUG-145（AX211 の DHCP）、BUG-165（5330 の DSDT） | 調査が長い・実機の時間が要る |
| [WS001 p045](ws001/phase045/phase.md) POSIX の header の残り・p046〜p051、[WS126](ws126/ws.md) Python | ベータ3 の列（合間に P1） |
| 規約の全文の見直しの Phase（各 WS） | ユーザーの決定でベータ3 |

## 運用

- 凍結の目標の 10/13 の後も、ベータなので UAT の Bug の直しは 10/16 の準備に間に合う範囲で続ける。新しい仕様の変更は「ベータ3 に回すか」を Q1 がユーザーに聞く。
- 毎日の終わりに Q1 がこの表を更新し、完了した項目を消す。
