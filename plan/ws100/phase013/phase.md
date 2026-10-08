<!-- awesome-plan project=zedbsd record=ws100-p013 -->
# ws100-p013: 音量の slider のドラッグでのフリーズ（BUG-170）と、確認の音を離した時に 1 回にする

Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: T1-087 PASS 3/3）（旧: in-progress（実装済み・T1 の試験待ち。2026-10-04、P2 generation11））
Disposition: normal
Parent: [WS100](../ws.md)（Settings の側は WS089 の source。受け入れはこの Phase にまとめる）
Queue: q683 / q683-i01（P2）。承認: 2026-10-04 user（AML と並走する UAT の Bug、17 時以降に実行）と 17 時の体制の指示（[queue](../../queue.md) の「2026-10-04 17 時以降の予定」）
Bug: [BUG-170](../../bugs/BUG-170.md)

## 範囲

- system bar の音量の popup と Settings の Sound の頁で、slider をドラッグすると desktop がしばらく止まる件（UAT 2026-10-04 の D1・D2、実機 5330）。
- 2026-10-04 user「確認の音は離した時に 1 回」: ドラッグの途中では確認の音を鳴らさず、離した時に 1 回だけ鳴らす。wheel の notch ごと、mute を外した時の音は今まで通り。
- 範囲の外: audiod・pci-hda・compositor の主 loop の変更（下の「読みで調べたこと」で原因と言える所が見つからなかった）。

## 受け入れ

| # | 基準 | 確かめ方 |
| --- | --- | --- |
| K1 | system bar の slider を速く長くドラッグ（10 ms ごとの motion、計 1.6 s）しても zdesktop が遅れない: 最初の step から離した時の `set ... final=1` までが 3.6 s 以内。離した直後の icon の click で popup が 3 s 以内に閉じる | `volume-bug170.sh`（QEMU、T1） |
| K2 | そのドラッグで確認の音の要求は離した時の 1 回だけ（`ZWL VOLUME feedback ... via=slider` が 1 つ増える、`via=held` が無い） | 同上 |
| K3 | Settings の slider の同じドラッグ（1.2 s）で `SOUND feedback error=0` が 1 つだけ増え、audiod が最後の値になる | 同上 |
| K4 | 既存の試験が通る（`volume-p004.sh` の A1〜A6、`volume-p005.sh`） | QEMU、T1 |
| K5 | 実機 5330 で D1・D2 のドラッグでフリーズしない | ユーザーの次の UAT（未実施） |

## 変えたこと

- `userland/desktop/wayland/volume.c`（system bar）: ドラッグの途中の確認の音（250 ms に 1 回、`VOLUME_FEEDBACK_MS`）と、tick で鳴らす保留の音（`feedback_waiting`・`feedback_ms`・log の `via=held`）を外した。`volume_set` は final（wheel の notch、ドラッグの終わり、mute）の時だけ 1 回鳴らし、mute を入れる時は鳴らさない。ドラッグの途中の音量の送り（50 ms に 1 回）は今まで通り。
- `userland/desktop/settings/sound.c`・`settings.h`（Settings の Sound の頁）: 同じく途中の音と保留（`feedback_waiting`・`feedback_at`・`SOUND_FEEDBACK_MS`）を外し、final の時だけ鳴らす。`se_sound_wait` は送りの保留だけを見る。
- `plan/ws100/design.md` の A4 の規則を「slider は離した時に 1 回だけ」に直した。
- 試験 `plan/ws100/tests/volume-bug170.sh`（新規）: K1〜K3。既存の `volume-p004.sh`（A4 の音の数 6 以上: release 1・mute off 1・wheel 5 で足りる）と `volume-p005.sh`（release で 1 つ増える）は変更なしで新しい規則に合う。

## 読みで調べたこと（原因）

ticket の候補は「値の変化ごとの確認の音の再生要求の滞留」。読んだ結果:

1. **zdesktop と Settings は、確認の音を既に 250 ms に 1 回へ間引いていた**（`VOLUME_FEEDBACK_MS`・`SOUND_FEEDBACK_MS`）。値の変化ごとに要求していたのではない。音量の送りも 50 ms に 1 回。
2. zdesktop → audiod の経路（`libkeiland-backend-zedbsd/audio-zedbsd.c`）は O_NONBLOCK の socket で、kernel の unix socket も O_NONBLOCK を MSG_DONTWAIT として扱う（`src/kern/net/socket-file.c`）。送れない時は接続を落とすだけで待たない。audiod の返事（DONE・VOLUME_CHANGED）は tick ごとに読み切る。
3. audiod（`userland/base/audiod/`）: FEEDBACK は音の頭に戻すだけ（重ならない）。DEVICE_VOLUME は mixer の ioctl（pci-hda の codec の verb 2 つ）と購読者への報告。どちらも軽い。
4. kernel の pci-hda の音量の書き込み（`hda_volume_write` → `hda_command`）は codec の応答を最長約 11 ms（1 ms の tick）polling するが、framework の spinlock の外で、kernel は preemptible、CPU は 12。desktop 全体を止める lock は見当たらない。
5. compositor の設定（WS135）の `settings_flush` は Settings の `look_changed` を起こすが、cache を読むだけ。session.log は tmpfs（`/run`）で、行ごとの write は軽い。
6. QEMU の既存の試験（`volume-p004.sh`）のドラッグは 150 ms 間隔の 3 step だけで、実機の touchpad の 10 ms ごとの motion を再現していなかった。

よって、**読みだけではフリーズの根を確定できていない**。確実に言えるのは「ドラッグの途中に 250 ms ごとの確認の音（4 回/秒）が鳴り続けていた」ことで、これはユーザーの観察（連続で確認の音を鳴らそうとしている）と合う。この Phase はユーザーの指定どおり途中の音を無くし、K1 の速いドラッグで遅れが無いことを QEMU で確かめる。実機でまだ止まるなら（K5 が NG）、次は session.log の `ZWL PERF`（5 s ごとの pass の数と work の率）と Settings の `SLOW-FRAME` をドラッグの前後で取り、止まっている process を特定する（実機の SSH から `ps` の状態と CPU 時間も取る）。

## 確認（host、2026-10-04）

| 確認 | 結果 |
| --- | --- |
| zedBSD の build（`ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=build/p2-ci`、`bin/wayland`・`bin/settings`） | exit 0、warning 0（`-Werror`） |
| Settings の host の build（`plan/ws089/tests/host-build.sh`） | exit 0 |
| `sh -n plan/ws100/tests/volume-bug170.sh` | 通る |
| `git diff --check` | 問題なし |

## QEMU の試験（T1 に依頼、Q1 経由。結果待ち）

- image: `plan/ws100/tests/build-volume-image.sh BUILD`（config `plan/ws100/tests/config-amd64-volume.mk` と `--file` の複写だけ）。
- 流す試験と合否: `volume-bug170.sh IMAGE`（K1〜K3）、`volume-p004.sh IMAGE`・`volume-p005.sh IMAGE`（K4）。各 script の最後の行が PASS。

## 残り

- K5（実機）はユーザーの次の UAT。
- 根が確定していない（上の「読みで調べたこと」）。実機で再現する場合の次の手順は上に書いた。
