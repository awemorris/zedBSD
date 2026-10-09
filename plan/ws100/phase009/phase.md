<!-- awesome-plan project=zedbsd record=ws100-p009 -->

# ws100-p009: L3 音量の曲線（25・50・75・100% → -30・-15・-7・0 dB ± 3 dB）

Phase ID: `ws100-p009`
Parent: [WS100](../ws.md)
Status: test-wait（T1-478、2026-10-08 夜 Q1）
Phase disposition: normal
Queue: Q1 の投入（2026-10-08「WS100 p009（音量の曲線、約 0.5 LW）を実装し T1 の依頼文を」）
依存: p006 の a（codec の amplifier の段の log。QEMU と 5330 の段数・1 段の dB を読むため）。実機の確かめは p006 の b の後。
基準: ws.md の段 L3「0〜100% の段ごとの大きさの差が耳で等しく聞こえる（実機で 25・50・75・100% の dB が -30・-15・-7・0 dB ± 3 dB）」（ユーザー「案のまま」2026-09-30 朝）。

## 今（2026-10-01 に code で確かめた）

| 経路 | 今の写し方 | 25% | 50% | 75% |
| --- | --- | --- | --- | --- |
| audiod の software の音量（codec に amplifier が無い時、`userland/base/audiod/mix.c:31` の注釈・`soft_gain` 613 行・`SOFT_STEP` 61162） | 1% あたり 0.6 dB（60 dB の幅） | -45 dB | -30 dB | -15 dB |
| pci-hda の codec の amplifier（`src/drivers/pci/pci-hda.c:1852` `left = volume->left * controller->volume_steps / 100U`） | 段に線形（段は dB で等間隔なので dB で線形）。幅は codec 次第（例 0.75 dB × 87 段 = 65 dB） | 約 -49 dB（例） | 約 -33 dB | 約 -16 dB |
| 目標 | — | -30 ± 3 | -15 ± 3 | -7 ± 3 |

どちらも目標に合わない。

## 範囲

- 百分率 p（1〜100）から dB への写しを 1 つに決める: **dB(p) = 50 × log10(p / 100)**（振幅 = (p/100)^2.5）。25% = -30.1、50% = -15.1、75% = -6.2、100% = 0 で
  目標の ± 3 dB に入る。0 は無音（mute と同じ）。1% は -100 dB で、codec の最小の段より下は最小の段（か mute）。
- audiod の software の音量をこの写しにする（表で持つ。浮動小数の関数を使わない今の作りに合わせる）。
- pci-hda の codec の段への写しをこの写しにする: step = clamp(round(numsteps + dB(p) / step_db), 0, numsteps)。ここで step_db は amp caps の stepsize（0.25 dB 単位、
  値 + 1）、0 dB は最大の段（offset の扱いは HDA の仕様の amp caps の offset を読み、仕様に合わせる。p006 a の log で QEMU・5330 の値を見てから決める）。
- 範囲の外: app ごとの音量、出力の選択、確かめの音の大きさ（-12 dBFS のまま）。

## 手順（2026-10-01 追記）

1. p006 a の log から、QEMU（`hda-duplex`）と 5330（p006 b の `hw-hda.txt`）の `hda: volume nid … steps … step_db … offset …` を読み、この phase.md に表で書く。
   5330 の値が無ければ QEMU の分だけ先に進め、実機の確かめを残りにする。
2. 写しの表を作る script `plan/ws100/tests/volume-curve.py`（新）: `table` で audiod の 101 個の factor（1/65536）を C の配列の形で出し、`check STEPS STEP_DB` で
   pci-hda の写しの 25・50・75・100% の段と dB を出し、目標との差を判定（± 3 dB）して `volume-curve: PASS|FAIL` を出す。
3. audiod: `mix.c` の `soft_gain` を表引きにする（`SOFT_STEP` の繰り返しをやめる）。表は 2 の script の出力を写し、注釈に生成の command を書く。
   `audiod-qemu.sh` の `soft` の case の判定（100 > 50 > 10 > 0、126〜132 行）はそのまま通るはず。
4. pci-hda: `hda_volume_write`（1843 行付近）の段の写しを 1 の式にする。整数で計算する（kernel に libm は無い前提。表か、`dB × 4`（0.25 dB 単位）の整数の表を百分率で引く）。
   p006 a の `hda: volume %u%% -> step %u/%u` の log で確かめる。
5. host 試験: `python3 plan/ws100/tests/volume-curve.py check <QEMU の steps> <step_db>`、`… check 87 0.75`（典型の Realtek の例）、5330 の値（あれば）。
   audiod の表と script の出力の一致: `python3 plan/ws100/tests/volume-curve.py verify userland/base/audiod/mix.c`（script に足す）。
6. QEMU の wav で dB を測る（software と codec の両方）:
   ```
   RUN=$PWD/build/ws100-p009-audiod-run IMG=build/ws100-p009-audiod/hdd-image.img sh plan/ws100/tests/audiod-qemu.sh build/ws100-p009/audiod soft hardware
   ```
   `soft` と `hardware` の case の command を 100・75・50・25 の順に変える必要があれば、`audiod-qemu.sh` に新しい case `curve`（`volume 100 feedback … volume 75 feedback … volume 50 feedback … volume 25 feedback`、
   `mixer=off` と `on` の 2 回）を足し、`hda-wav-check.py windows` の峰の比を dB にして目標と比べ、`curve: … ok` を出す。
7. 回帰: `host-audio.sh`、`audiod-qemu.sh`（全 case）、`volume-p004.sh`（A4 の峰の順の判定が変わらないこと）、`volume-p005.sh`、boot test（guide.md §5.3・§5.4）。
8. 実機（p006 b の後、ユーザーの時間）: 5330 で `audiod-feedback volume 25 feedback sleep 800 volume 50 feedback sleep 800 volume 75 feedback sleep 800 volume 100 feedback` を流し、
   `dmesg | grep "hda: volume"` で段の読み戻しを記録し、計算の dB が目標に入ることを確かめる。ユーザーが「段ごとの差が等しく聞こえるか」を聞く。

## 完了の条件

- `volume-curve: PASS`（QEMU の段・典型の段・5330 の段（あれば））、audiod の表と script が一致。
- QEMU の `curve`（か `soft`・`hardware`）の wav の峰の dB が 25・50・75% で -30・-15・-7 dB ± 3 dB（software と codec の両方）。
- 回帰が全て PASS、build の warning 0、style の新しい違反 0、boot test PASS（PNG をユーザーに見せる）。
- 実機: 5330 の段の読み戻しで計算の dB が目標の中。未実施なら「未実施」と書き、L3 の曲線の実機の分を残りにする。

## 実装（2026-10-08 P2）

- 写し: dB(p) = 50 × log10(p / 100)（範囲のとおり）。
- `plan/ws100/tests/volume-curve.py`（新）: `table`（audiod の 101 個の factor）、`quarters`（pci-hda の 101 個の 1/4 dB）、`check STEPS STEP_DB`、`verify FILE…`（source の表と script の一致）、`wav WINDOWS.txt`（hda-wav-check.py の windows の峰を dB に、100・75・50・25 % の順）、`dmesg FILE`（pci-hda の log の段を曲線と照合）。
- audiod `userland/base/audiod/mix.c`: `soft_gain` を表 `volume_factors[101]` の引きに（`SOFT_STEP` の繰り返しを削除）。1 % は約 -100 dB（前は -59.4 dB）。
- pci-hda `src/drivers/pci/pci-hda.c`: amp caps の stepsize（1/4 dB 単位、値 + 1）を `volume_step_quarters` に、表 `hda_volume_quarters[101]`、`hda_volume_step`（100 % 以上は最大の段、それより下は曲線の 1/4 dB を段の大きさで割って丸めた数だけ下げ、最小の段で止める、0 % は最小の段。mute は今どおり）。100 % は今どおり最大の段（offset より上の正の gain を含む、範囲の「0 dB は最大の段」）。log `hda: volume nid N steps S step Q/4 dB offset O`（amplifier を見つけた時）と `hda: volume P% -> step N/S`（書くたび）。
- 試験: `plan/ws100/tests/audiod-qemu.sh` に case `curve`（mixer=off で 100・75・50・25 % の feedback の峰の dB、mixer on で同じ音量の段を guest の dmesg の行で照合）。既定の case の列にも入れた。
- 注意（QEMU）: QEMU の hda-duplex の mixer on は段を振幅に線形に写す（段 / 74）ので、codec の経路の dB は WAV では測れない。codec の経路は段の照合、dB の耳の確かめは 5330。

## 確認（2026-10-08）

| 確認 | 結果 |
| --- | --- |
| `volume-curve.py check 74 1.0`（QEMU の hda-duplex の想定）・`check 87 0.75`（Realtek の例）・`check 31 1.5` | PASS（74 段: 25・50・75 % が step 44・59・68 = -30・-15・-6 dB） |
| `volume-curve.py verify userland/base/audiod/mix.c src/drivers/pci/pci-hda.c` | PASS（101 値ずつ一致） |
| `volume-curve.py wav`・`dmesg` の合成の入力 | PASS |
| `sh plan/ws100/tests/host-audio.sh` | 14/14 PASS |
| build（`make ZEDBSD_CONFIG=plan/ws035/tests/config-amd64-zdesktop.mk BUILD=build/ws181 build/ws181/bin/audiod build/ws181/vmunix`） | 成功、warning 0、`amd64 vmunix check: PASS` |
| style-check（mix.c・pci-hda.c） | 新しい指摘 0（既存 88 → 87） |
| QEMU（T1: `audiod-qemu.sh … curve soft hardware mute`、`volume-p004.sh`・`volume-p005.sh`、boot test） | 未実施 |
| 5330（`hda: volume nid` の段と stepsize、耳で段ごとの差） | 未実施（p006 b と一緒に） |

## T1-478 の回帰の FAIL（2026-10-09、P1）

本体（curve・soft・mute・hardware）は PASS。回帰の volume-p004 は 1 回目だけ boot の時期（HANDOFF の行）で FAIL、2 回目 PASS（試験の時間の揺れ、製品の変化ではない）。volume-p005 は 2 回とも同じ 5 行が FAIL。原因は `/tmp/s.log` に Settings の行（SOUND report・ZSETTINGS CONTROL）が無く、MAP も新しく出ないこと。画面（sound.png）は Sound の頁で 60%。音量の曲線の変更とは関係が無い。読み: 最初の login で compositor が Settings の Welcome（ws164-p002、T1-267 の PASS の後に入った）を起動し、試験の `/bin/settings sound` が instance を Welcome に渡して終わる（Welcome の行は session の log へ）。**未確認の推定**。
直し 7ad5d84ef（試験）: Settings の起動の前に `ps` を `processes.txt` に残し、動いている Settings を終える。再試験は T1 の行（Q1 が番号）。
