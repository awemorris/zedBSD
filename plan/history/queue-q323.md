<!-- awesome-plan project=zedbsd record=queue -->

# Queue q323: `/dev/graphics` とaudioの設計、zdesktop基本、rootfsのtree化ほか

<!-- awesome-plan-current:start -->
Status: stopped（2026-09-23、ユーザー指示）
Active Queue: none（q323 stopped）
Executor: メインセッション（サブエージェントは使わない）
Last Queue: q322 finished（履歴 `plan/history/queue-q322.md`）
<!-- awesome-plan-current:end -->

Approval: current user「では、8個をキューに入れて、実行してください。」（2026-09-23、8項目案の提示後）
Start UTC: 2026-09-23T03:00:00+00:00
Stop: 2026-09-23、ユーザー指示「未完了のphaseをunclearedにしてください。停止してください。」

## 選定

8項目はいずれも前提（WS035のrefactor、ws035-p001、ws034-p001）がclearedで、**項目同士に依存が無い**。
実行はメインセッションが上から1つずつ行う（`plan/master.md`「実行体制とQueue運用方針」）。

| Order | Attempt | Phase | Status | Scope |
| --- | --- | --- | --- | --- |
| 1 | q323-i01 | [ws035-p020](../ws035/phase020/phase.md) | cleared | 設計: `/dev/graphics` の共通層とGPU scanoutへの引き継ぎ（敵対的レビュー込み） |
| 2 | q323-i02 | [ws034-p002](../ws034/phase002/phase.md) | cleared | `which`（base独自実装） |
| 3 | q323-i03 | [ws034-p036](../ws034/phase036/phase.md) | cleared | rootfsのtree化（UFS imageをツリーから作る、symlink、開発用ファイル） |
| 4 | q323-i04 | [ws035-p011](../ws035/phase011/phase.md) | **uncleared** | zdesktop: `zwl` → `/bin/zdesktop` 改名、基本のウィンドウ管理 |
| 5 | q323-i05 | [ws035-p021](../ws035/phase021/phase.md) | cleared | 設計: audioフレームワークと `/dev/dsp`（敵対的レビュー込み） |
| 6 | q323-i06 | [ws035-p010](../ws035/phase010/phase.md) | cleared | libtruetype（`cmap`・`glyf`・`hmtx`、anti-alias、`/lib/libtruetype.so`） |
| 7 | q323-i07 | [ws035-p018](../ws035/phase018/phase.md) | **uncleared** | networkdの状態push通知（購読） |
| 8 | q323-i08 | [ws034-p003](../ws034/phase003/phase.md) | **uncleared** | PCI列挙UAPI（`/dev/system`）とlspci |

## 順序の理由

- 1と5は設計Phase。1は p005・p024 を、5は p006→p007→p009→p019 を開ける。
- 3（rootfsのtree化）は bash 以降の全パッケージの前提。`/usr/include`・`.so`・SONAME symlinkの置き方がここで決まる。
- 4は `zwl` を `/bin/zdesktop` へ改名する。WS031の p027・p028 は `zwl` を参照するので、4より後に回す。
- 4は1の設計結果を見てから着手する（`/dev/graphics` の前提が変わりうる）。

## 範囲外

`/dev/graphics` の実装（p005・p024）、audioの実装（p006以降）、パッケージ本体（bash以降）、WS036、WS031。
HALの変更は差分ごとに事前承認。aggregate `make check` は使わない。commitは `git commit -m WIP` のみでpushしない。

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q323-i01 | ws035-p020 | **cleared**。設計 `plan/ws035/graphics-design.md`、敵対的レビュー `phase020/review.md`（8件指摘、7件反映・1件を制限へ）。ソース変更なし |
| q323-i02 | ws034-p002 | **cleared**。`/bin/which`。host 15ケース、実QEMU 4ケース。amd64・pcat・pc98 build warning 0。途中で `include/kern/boot.h` のenum前方参照（UEFI bootloaderの `-Werror`）を修正 |
| q323-i03 | ws034-p036 | **cleared**。ツリーがrootの正本、UFS imageは `--tree` でそこから。`rootfs.tar.gz` 廃止、`/usr/include`＋link用objectを収録。`--mode` が `--file` より先に走る順序バグと、UFSのinodeがバイト数だけで決まる問題を修正。amd64・pcat・pc98 で login 確認 |
| q323-i04 | ws035-p011 | **uncleared**。着手時の調査で、移動・リサイズに要る合成（コンポジタ自身の描画先と blit）が存在しないと判明。今の `zwl_present()` はクライアント画像をそのまま scanout する全画面1枚の作りで、libvulkan も使っていない。設計Phaseを先に置く必要がある。ソース変更なし（改名も未実施）|
| q323-i05 | ws035-p021 | **cleared**。設計 `plan/ws035/audio-design.md`、敵対的レビュー `phase021/review.md`（7件指摘、6件反映）。ソース変更なし |
| q323-i06 | ws035-p010 | **cleared**。`/lib/libtruetype.so`（cmap 4/12、glyf、hmtx、走査線 anti-alias）。host 27項目×実フォント4種、破損フォント6000件のsanitizer PASS。`line_height` の丸めと、壊れたフォントが呼び出し側のmemoryを壊すAPIの穴を修正 |
| q323-i07 | ws035-p018 | **uncleared**。購読（`NETWORKD_OP_SUBSCRIBE`、購読者8、変化での通知、`net watch`）を実装し amd64 build は warning 0。**動作は未確認**。コンソールへのキー入力で文字が落ちて検証できず、SSHハーネス（`plan/tools/guest/`）へ切り替えた途中で停止 |
| q323-i08 | ws034-p003 | **uncleared**。未着手（Queue が先に停止した）|

## 停止時の状態（2026-09-23）

cleared 5件（p020・p002・p036・p021・p010）、uncleared 3件（p011・p018・p003）。

**uncleared の理由はそれぞれ違う。**

- **ws035-p011**: 前提の欠落。移動・リサイズに要る合成の仕組みが無く、設計 Phase が先に要る。ソース変更なし。
- **ws035-p018**: 実装はtreeにあるが**動作の証拠が無い**。build は通る。
- **ws034-p003**: 未着手。依存は解けているので次の Queue へそのまま入る。

## この Queue で作った道具

- `plan/tools/guest/`（`guest.sh`・`guest.py`・`net.conf`）: ゲストへ SSH で入り、コマンド実行・
  ファイル転送・ゲストの `lldb`・QEMU gdbstub でのカーネルデバッグを行うハーネス。**未完成**
  （ゲストは起動し sshd も起きるが、SSH がまだ応答しない）。
- `plan/ws035/tests/guest-console.py`: 画面を読み QMP の `send-key` で入力する道具。
  シフトが押しっぱなしになる件と文字が落ちる件を直したが、コンソール入力は不安定なままである。
- `plan/ws035/tests/truetype-test.c`: libtruetype の host fixture。
- `plan/tools/boot-test.py`: VGA text buffer（9 pixel間隔）も読めるようにし、
  空白セルが NUL になる件を直した。`boot-test.sh` に `BOOT_MODE=bios-ide` を追加。

## 未確認のまま残した観測

QEMU 上で、USB network adapter を boot disk と同じ xHCI コントローラに置いた直後に
`usb-storage: BOT CBW error=13` が続き root filesystem が応答しなくなった。別コントローラなら起動した。
**原因は切り分けていない。実機では未確認。** 不具合として扱っていない。

**訂正（2026-09-23、WS040）**: この観測の原因は USB storage でも usb-net との同居でもなかった。
`KERN_CLOCK_HZ` が 1000 になったのに USB の timeout が 1 tick = 10 ms で換算されていて、
**5 s のつもりの timeout が約 500 ms** になっていた。TCG の遅いゲストで負荷がかかると要求が
500 ms を超え、`ETIMEDOUT` → overlay の read-only 隔離に至った。
KVM で disk を 1 I/O/秒に絞る実験で再現を確かめた。詳細は [WS040](../ws040/ws.md)。
