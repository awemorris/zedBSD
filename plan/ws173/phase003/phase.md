<!-- awesome-plan project=zedbsd record=ws173-p003 -->

# ws173-p003: AAT の host の道具と AAT の image の config

Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: T1-200c PASS、その後の AAT の実行で使われている）（旧: test-done（2026-10-07 q834 P2: target（QEMU）での確かめは T1 の AAT の実行で済み（T1-200c で注入・撮影・転送・log の待ちが PASS、その後 T1-202c・T1-232・T1-305・T1-315 などで runner と一緒に使われている）。判定は Q1）（旧: in-progress（2026-10-05 夜、host の自己試験まで）））
Disposition: normal
Parent: [WS173](../ws.md)
Queue: q777（Q1、2026-10-05 夜、最優先）
依存: [p001](../ws.md)（`/dev/input-inject` のマウスとキーボード、P1）、[p002](../ws.md)（`keiland-shot`、P1）。口の形が決まるまで CLI の骨組みと SSH の部分を先に（Q1 の指示）

## 範囲（Q1 の指示、2026-10-05 夜）

host の道具 `plan/tools/aat/`: SSH で素の 5330（UAT の image、10.0.30.3）に入り、1 つの CLI で、マウスの移動・click・drag・wheel、key・文字列の入力、画面の撮影の取得、log の行の待ち、compositor の log からの窓の座標。AAT の image の config `plan/tools/aat/config-amd64-aat.mk`（UAT の config ＋試験の注入＋撮影の口、製品には入れない）。QEMU での自己試験を T1 に頼む手順。

## 作った物

- `plan/tools/aat/aat`（python3、1 file）: 命令 `check`・`run`・`get`・`put`・`start`・`stop`・`move`・`rel`・`click`（`--button`・`--count`）・`down`・`up`・`drag`・`wheel`・`key`（`ctrl+alt+t` の chord）・`type`（ASCII、US の配列）・`shot`・`mark`・`wait-log`・`lines`・`windows`・`where`。target は `--target user@host[:port]`・`--target 5330`（root@10.0.30.3）・`--qemu`（`GUEST_RUNTIME/session.json`）・`--local`（自己試験）。
  - SSH は ControlMaster で 1 本を使い回す。鍵は checkout の guest の harness の鍵（`plan/tmp/guest/id_ed25519`）。root 以外の user では `sudo -S`（`AAT_PASSWORD`、既定 kei。zedBSD の sudo に `-n` は無い）。
  - log は host で UTF-8 を解く（`tail -c +N` で mark の後の bytes を取る）。`wait-log` は 0.3 秒ごとに読む。
  - 窓: `ZWL MAP`・`UNMAP`・`WINDOW centred`・`RESIZE settled`・`RESIZE end`・`GLASS press move` の行から surface ごとの最後の位置と大きさ。
  - 入力: P1 の `aat-input`（`AAT_INPUT`、既定 `/bin/aat-input`）の命令を SSH で 1 つずつ root で呼び、`ok` を確かめる（`error WHY` なら失敗）。`move`→`move-to`、`rel`→`move`、`click`（2 回は `double-click`）、`down`・`up`、`drag`（左 button、STEPS）、`wheel X Y N [H]`→`move-to`・`wheel`・`hwheel`（N > 0 で上）、`key`、`type`（改行は `key enter`）。`start` は撮影の大きさで `aat-input start --width --height`（前の server は先に stop）。2026-10-05 夜に P1 の口の形（socket の CLI）に合わせた。最初の FIFO の daemon の案は取り下げた。
  - 撮影: `AAT_SHOT`（既定 `/bin/keiland-shot {path}`）で target に書かせて `cat` で取る。PNG の header で確かめ、`start` は撮影の大きさを画面の大きさにする。
- `plan/tools/aat/config-amd64-aat.mk`: UAT の config ＋ `CONFIG_INPUT_TEST_INJECT := y`。P1 の daemon と撮影の package の名前は入った時に足す（今は注記）。
- `plan/tools/aat/build-image.sh BUILD`: `test-image.sh --no-harness` に harness の root の鍵と host 鍵だけを足す（harness の `/etc/net.conf` は QEMU の USB の network の設定で、実機の network を置き換えるので入れない）。
- `plan/tools/aat/tests/run-host.sh`（`fake-aat-input.py`・`fake-shot.py`）: host の自己試験。
- `plan/tools/aat/README.md`: 使い方、protocol の案、P1 への依頼、自己試験の手順。

## 判断（P2）

- **絶対の pointer**: compositor は相対の mouse に加速をかける（`pointer-accel.c`）ので、相対の量では狙った pixel に置けない。P1 の `aat-input` の絶対の pointer（`move-to`）を `move`・`click`・`drag`・`wheel` に使う。`rel` は相対の確かめ用。
- 素の 5330 の起動は毎回ユーザーが USB で行い、エージェントは起動の後に SSH で入る（2026-10-05 ユーザーの決定、Q1 の伝達）。
- 窓の app の名前・題名・titlebar の部品の座標は今の log に無い。P1 に log の行か一覧の口を頼む（README）。

## 確かめ（host）

- `sh plan/tools/aat/tests/run-host.sh` → `aat-host: PASS`（26 項目: start と画面の大きさ、click・double・3 回・drag・wheel・move・rel・button・chord・type が `aat-input` に送る命令、画面の外・知らない button・ASCII の外の拒否、`aat-input` の拒否の報告、mark と wait-log・lines・timeout、`where`・`windows`・unmap、shot、run の状態、put・get、stop の後の no-server）。偽の `aat-input` は P1 の main.c（88700c5c、P1 の worktree）の命令の形を写した物。
- `build-image.sh` の引数（net.conf を除き root の鍵と host 鍵が残る）を dry-run で確かめた。image の build は未。

## 未実施

- 5330 の実機: 未実施（AAT の image を USB に書くのはユーザー。WS173 p005）。
- （済み、2026-10-07 の見直し）target の QEMU での確かめは T1-200c 以降の AAT の実行で済み。`keiland-shot`・`aat-input` は `config-amd64-aat.mk` に入っている。
