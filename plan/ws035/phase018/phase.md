<!-- awesome-plan project=zedbsd record=ws035p018 -->

# ws035-p018: networkd の状態 push 通知（購読）

Phase ID: `ws035-p018`
Parent: [WS035](../ws.md)
Status: **cleared**（q325-i01、2026-09-23）
Phase disposition: normal
Queue: q323（q323-i07、uncleared）→ q325（q325-i01、cleared）
実行: メインセッション

## 範囲

networkd に購読の仕組みを入れ、状態が変わったときに購読者へ知らせる。
zdesktop のタスクバー（p013）が WiFi や接続の状態を、定期的に聞かずに表示できるようにするため。
WS005-p016 から移管。

## 調べた事実

- networkd に通知の仕組みは**無かった**。`notify_init()` は init への起動通知（`KERN_NOTIFY_FD`）だけで、
  クライアントへ知らせるものではない。
- 接続は1要求1接続。`accept4()` → `handle_request()` → `close()` で、長く保つ接続が無い。
- 状態を得る手段は `NETWORKD_OP_SHOW` の問い合わせだけ。

## 入れたもの（ソースはtreeにある）

### プロトコル（`userland/base/net/protocol.h`）

- `NETWORKD_OP_SUBSCRIBE = 64`。応答したあとも接続を閉じない。
- 通知は `request_id = NETWORKD_NOTIFICATION_ID`（0）で、要求への応答と区別できる。
- 購読者は最大 `NETWORKD_SUBSCRIBER_MAX`（8）。固定なので、購読者が daemon に確保をさせられない。

### daemon（`userland/base/networkd/main.c`）

- 購読者の表（固定 8）、`subscriber_add`／`subscriber_drop`／`subscribers_close`。
- `handle_request()` が `SUBSCRIBE` を見たら `accept_subscriber()` へ回し、
  **接続を保持したかどうかを返す**。保持したものは呼び出し側が閉じない。
- 最初のフレームは現状（`show_interfaces()` の出力）。
  購読者が「一度 SHOW して、それから購読」をしなくて済むようにするため。
- `poll()` の集合に購読者を入れる（`events = 0`）。切れたことをその場で気づくため。
- 状態が動いたら `notify_state_changed()` で印をつけ、ループの先頭で1回だけ送る
  （1つの操作が複数の変化として現れるため）。印をつける場所:
  - kernel の route event（interface の up/down、address、route）
  - `networkd_lan_configured()`・`networkd_lan_down()`
  - **WiFi は値の比較**。遷移箇所が10か所以上あり、どれか1つが書き忘れると
    購読者が黙って知らされなくなるため、`deliver_state_changes()` が
    `managed_wlan.state` を前回値と比べる。比較は書き忘れようがない。
- 書けない購読者は**捨てる**。daemon は network を設定できる唯一のもので、
  読まないクライアントを待つと全部が止まる。
- 権限は `SHOW` と同じ（`operation_allowed(role, "SHOW")`）。
- 停止時に全購読者を閉じる。

### クライアント（`userland/base/net/main.c`）

`net watch`。接続を半分閉じない（`shutdown(SHUT_WR)` をしない）で保ち、
届いたフレームの `OUTPUT` を標準出力へ書き続ける。`net show` と同じ文面。

## 検証（q325-i01、2026-09-23）

シリアルコンソール（ws035-p037）でゲストを操作した。キー入力は使っていない。

| 確かめたこと | 結果 |
| --- | --- |
| 最初のフレーム | `net watch` が接続直後に現状（`net show` と同じ文面）を受け取る |
| 変化の通知 | `net down` で offline、`net up` で online のフレームが追加で届く（NE2000 の `ne0`、pcat） |
| 上限がちょうど8 | 8つの `net watch` がそれぞれ最初のフレームを受け取り、9つ目は `net: watch: too many watchers: Device or resource busy`（exit 1）。amd64、KVM |
| 切れた購読者の枠が空く | 1つを `kill` した後、新しい `net watch` が受け入れられて最初のフレームを受け取り、その次はまた `EBUSY` |
| daemon の継続 | 購読者が消えた後も `net show` と新しい購読が動く |

検証の途中で daemon と `net watch` のバグを3つ直した（q325）。
1. 通知の `request_id` 0 を、header の encoder が拒否していた。
2. `SUBSCRIBE` の要求にも「要求の後で相手が書く側を閉じる」ことを求めていて、購読を保つ接続と両立しなかった。
3. `net watch` が daemon の拒否（`EBUSY` など）を表示せずに捨てていた。

権限のないクライアントの拒否は試していない（ゲストに root 以外のログイン利用者がいない）。
権限の判定は `SHOW` と同じ関数（`operation_allowed(role, "SHOW")`）を通る。

## この Phase で作った道具

`plan/tools/guest/`（`guest.sh`・`guest.py`・`net.conf`）: ゲストへ SSH で入り、
コマンドを実行し、ファイルを送受し、ゲストの `lldb` と QEMU の gdbstub でデバッグするハーネス。
鍵は `plan/tmp/guest/` に作り、`guest.sh extra-files` が出す make 引数でイメージへ入れる。

**未完成**: ゲストは正常に起動し sshd も起きるが、**SSH がまだ応答しない**。
`ue0`（USB CDC-ECM）が上がっているか、DHCP が取れているか、sshd が listen しているかは未確認。

`plan/ws035/tests/guest-console.py`（キー入力側）のバグを2つ直した。
`-` の扱いでシフトが押しっぱなしになる件と、押下時間が長すぎて文字が落ちる件。
それでもコンソール入力は不安定なままで、それが SSH へ切り替えた理由である。

## 観測したこと（結論ではない）

QEMU 上で、USB の network adapter を boot disk と同じ xHCI コントローラに置いた直後に
`usb-storage: BOT CBW error=13` が続き、root filesystem が応答しなくなった。
同じイメージで adapter を別のコントローラに置くと起動した。
**原因は切り分けていない**（emulator か、この指定か、ゲストのドライバか）。**実機では未確認**。
ハーネスは動いた並びを採っているだけで、これを不具合として扱ってはいない。

**訂正（2026-09-23、WS040）**: この観測の原因は USB storage でも usb-net との同居でもなかった。
`KERN_CLOCK_HZ` が 1000 になったのに USB の timeout が 1 tick = 10 ms で換算されていて、
**5 s のつもりの timeout が約 500 ms** になっていた。TCG の遅いゲストで負荷がかかると要求が
500 ms を超え、`ETIMEDOUT` → overlay の read-only 隔離に至った。
KVM で disk を 1 I/O/秒に絞る実験で再現を確かめた。詳細は [WS040](../../ws040/ws.md)。

## 受け入れ

達成。購読の開始・通知・上限・解放・daemon の継続を実機相当（QEMU）で確かめた。
SSH のハーネス（上の道具）は p038 に、USB と同居したときの観測は p039 に引き継いだ
（観測の原因は WS040 の timeout だった）。
