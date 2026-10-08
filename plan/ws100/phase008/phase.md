<!-- awesome-plan project=zedbsd record=ws100-p008 -->

# ws100-p008: L3 確かめの音の遅れの計測と短縮

Status: cleared（2026-10-08 q910 P2 の照合: ws.md の表の 2026-09-30 Q1 の判断（QEMU で 50 ms 以内）に合わせた。実機は未実施）（旧: uncleared（2026-09-30、サブエージェント P4、worktree `ws100-volume`（branch `wt/ws100`）。QEMU だけ、実機は未実施））
- 計測の道具と内訳はそろった。
- guest の中の経路（zdesktop の変更 → device がその音の byte を取るまで）は中央値 31〜37 ms で、50 ms 以内。
- host で QEMU の WAV に音が出るまで（QMP の入力 → WAV の音の始まり）は中央値 106〜111 ms で、50 ms を超える。超えた分は QEMU の HD Audio の codec の buffer と USB の入力の経路で、guest からは縮められない。
- 直しは入れていない（下の「直す候補」）。判定の扱いに main の判断が要る。

Disposition: normal
Parent: [WS100](../ws.md)
Queue: なし（2026-09-30 Q1 の割り当て）

## 範囲と受け入れ

段 L3: 操作（release・wheel の notch）から確かめの音の始まりまで 50 ms 以内（QEMU の値。実機は後で）。
経路の各点（zdesktop の volume.c → keiland_audio → audiod → HDA の DMA）の時刻を log に出して内訳を取り、大きい所を直す。
回帰: volume-p004・volume-p005・C9・boot test。

## 道具

- **zdesktop**（`volume.c`）:
  - `ZWL VOLUME set … at_ms=`（末尾に足した。既存の試験の形はそのまま）。
  - 確かめの音を頼むたびに `ZWL VOLUME feedback at_ms= via=`。
- **audiod**（`device.c`・`mix.c`・`audiod.h`）: 環境変数 `AUDIOD_TIMING_LOG` が file を指すとき、確かめの音ごとに 3 行を出す。
  - `feedback asked`: 受け取った時刻、device の frontier と書いた位置。
  - `feedback mixed`: 最初の frame を混ぜた device の byte と時刻、frontier からの先行の ms。
  - `feedback played`: device の DMA の位置（KERN_AUDIO_GET_OPTR）がその byte を越えた時刻。今の位置から byte の分を引いて推定する。
  - 変数が無いときは何もしない。時計はどちらも guest の CLOCK_MONOTONIC（ms）。
- **`plan/ws100/tests/feedback-latency.sh IMAGE [OUTDIR]`**:
  - volume の image を HD Audio 付きで起動し、audiod を timing の log 付きで起動し直す。
  - wheel の notch を NOTCHES 回（既定 10）、slider の押して離すを RELEASES 回（既定 5）行う。
  - 変更ごとに内訳（request・ipc・to_mix・mixed_ahead・to_play・total）と中央値を出す。
  - 続けて host の測り方を出す。
- **`plan/ws100/tests/wav-latency.py`**（host の測り方）:
  - QMP の入力を送った host の時刻を記録し、QEMU の WAV の大きさを 2 ms ごとに見る。WAV の backend は再生に合わせて書くので、frame 番号 → host の時刻の対応が取れる。
  - WAV の音の始まり（150 ms の無音の後の閾値超え）を host の時刻にして、入力との差を出す。
  - 対応の誤差は、書き込みの単位（約 27 ms）の中で最小の値を取るため小さい側に寄る。

## 結果（QEMU、intel-hda + hda-duplex、1024 frame の period）

guest の中（`feedback-latency.sh`、3 回の run: 15・6・8 回の変更の中央値）:

| run | total（変更 → device がその byte を取る） | 内訳の中央値 |
| --- | --- | --- |
| 1 | 35 ms（最大 42） | request 0、ipc 1、to_mix 14、mixed_ahead 21、to_play 21 |
| 2 | 37 ms（最大 41） | to_mix 14 |
| 3 | 31 ms（最大 41） | to_mix 9 |

- **request 0 ms・ipc 0〜1 ms**: zdesktop は変更と同じ ms に頼み、audiod は 1 ms 以内に受け取る。
- **to_mix 0〜29 ms（中央値 9〜14）**: audiod が次に目を覚ますまでの待ち。
  - 目を覚ますのは device の frontier が 1 fragment（4096 byte = 21.3 ms）進むごとなので、音はその時の period から混ざる。
- **mixed_ahead / to_play 21 ms**: 混ぜた byte は frontier の 1 fragment 先で、そこに DMA が届くまで 21 ms かかる。
- USB の入力の経路（一時的な log で測って外した）: kernel の USB の転送の完了（evdev の時刻）から zdesktop の変更まで 0〜1 ms。

host の測り方（QMP の入力 → WAV の音の始まり）:

| run | wheel の notch | 
| --- | --- |
| 1 | 中央値 110 ms（99〜117） |
| 2 | 111 ms |
| 3 | 106 ms（最大 115） |

- slider の release は、押したとき（final=0）にも確かめの音が鳴る。そのため WAV の音の始まりは release の前にあり、release を起点にした値（16〜30 ms）は正しい計測にならない。wheel の値を使う。
- guest の中との差（約 75 ms）は、guest から見えない 2 つの部分:
  - (1) QEMU の hda-codec の buffer。kernel の `audio.c` の注にある 8 KiB = 42.7 ms で、DMA の位置は音より先を行く。
  - (2) host の QMP → usb-tablet → guest の xHCI の割り込み転送の完了（約 30 ms の見込み）。
- 実機では、(1) は HDA の FIFO（数百 byte）、(2) は mouse の polling（1〜8 ms）になる。guest の中の約 35 ms と合わせて 50 ms 以内の見込みだが、実機では未計測。

WAV と guest の log の一致の確かめ: 10 回の wheel の音の間隔は、WAV（853〜896 ms）と audiod の `played`（833〜894 ms）でほぼ同じ（差は 1 fragment 以内）。

## 直す候補（入れていない）

guest の中で縮められるのは、audiod の to_mix と先行の 21 ms で、どちらも 1 fragment の大きさで決まる。

- **kernel の audio の fragment を小さくする**（`src/drivers/audio/audio.c`、`AUDIO_FRAGMENT_BYTES` 4096 → 2048 か 1024）:
  - 効果: 待ちと先行が半分〜4 分の 1 になり、guest の中は約 35 → 18〜10 ms。
  - 副作用:
    - 割り込みが 2〜4 倍になる。
    - `AUDIO_MMAP_LEAD_FRAGMENTS` と `AUDIO_DRAIN_TAIL_FRAGMENTS` は QEMU の codec の 8 KiB の先読みに合わせて fragment の数で決めてあるので、byte で持つよう直す必要がある。
    - QEMU の DMA は最大 8 KiB を一度に取るので、先行を小さくすると音が途切れる恐れがある。
  - 範囲: kernel の driver の変更で、WS100 の外。
- **audiod が要求を受けた時点で、すでに書いた period に足し込む**: 書いた period は DMA の先読みの範囲（kernel の lead）の中にあり、聞こえない恐れがあるので採らない。
- QEMU の codec の buffer と USB の入力の経路は、guest から変えられない。QEMU の設定の話で、製品の遅れではない。

## 確認

| 確認 | 命令 | 結果 |
| --- | --- | --- |
| build | `plan/ws100/tests/build-volume-image.sh build/amd64`（`build/ws100-p004-build.sh`） | exit 0、warning 0 |
| style | `plan/tools/style-check.py`（device.c・mix.c・audiod.h・volume.c） | 件数は変更の前と同じ（audiod の既存の件だけ、volume.c 0） |
| 計測 | `feedback-latency.sh build/ws100-p008-before.img build/ws100-p008/before`（と tmp の 2 run） | 上の表 |
| 回帰 | `volume-p004.sh`、`volume-p005.sh`（同じ image） | PASS、PASS |
| 回帰: C9 | `plan/ws099/tests/criteria.sh build/ws100-p008-criteria.img build/ws100-p008/criteria C9`（`build-criteria-image.sh` で作った image） | 10 本すべて PASS |
| boot test | `OUTPUT=build/ws100-p008-boot plan/tools/boot-test.sh build/ws100-p008-before.img` | PASS |

未実施: 実機（5330）の計測。

## 残り（main の判断が要る）

1. L3 の QEMU での判定: host の WAV（QEMU の codec の buffer 42.7 ms を含む）で測るか、guest の中の経路（35 ms）で測るか。
2. kernel の fragment を小さくする直し（上）を別の Phase にするか。
3. 実機の計測（`feedback-latency.sh` の guest の log の部分は実機でもそのまま使える）。
