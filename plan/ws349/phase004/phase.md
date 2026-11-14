<!-- awesome-plan project=zedbsd record=ws349-p004 -->

# ws349-p004 attach と読みだけの probe（Pi 5 の実機）

- WS: [ws.md](../ws.md)。設計: [p001](../phase001/phase.md)。依存: p003、mk の枠（[user-questions.md](../user-questions.md) U1 の答えと、ユーザーが当てた kernel.mk・config.mk の枠）。実機: Q1 が Pi 5 を割り当てる。
- 状態: planned
- 担当の定義の推し: phase-runner-high。

## 目的

再生の部分を書く前に、実機で p001 の未確定（Q1・Q2・Q5 の静的な部分・Q6・Q8）を読みだけで確かめ、結果で p001 の D3・D8・D10 を直す。

## 作る物

- `src/drivers/platform/rpi5/rpi5-hdmi-audio.c`・`.h` の attach の部分: tree の読み（HDMI の 2 node・DMA・DVP・clock・hd・HVS）、map、stepping、D2a の安全の確かめ、port の選び、DREQ の照合、PHY の読み戻し。**HDMI・DVP・DMA の register に何も書かず、`drv_audio_register()` も呼ばない**。読んだ値を 1 回だけ klog に出す（`rpi5-hdmi-audio: probe ...` の決まった形の行、register ごとの値）。
- `src/kern/platform/rpi5.c` の呼び出し（`CONFIG_DRIVER_RPI5_HDMI_AUDIO` の下、`rpi4.c` の音と同じく失敗しても起動を続ける）。
- 試験の config `plan/ws349/tests/config-rpi5-hdmi-audio.mk`（`plan/tools/aat/config-rpi5-aat.mk` と同じ形、`CONFIG_DRIVER_AUDIO`・`CONFIG_DRIVER_RPI5_HDMI_AUDIO` を y）。
- 実機の script `plan/ws349/tests/pi5-hdmi-audio.sh`（`plan/tools/aat/pi5-aat-board.sh` の形: check・deploy・probe・restore。deploy は vmunix を `.ws349old` に残して入れ、probe は起動の後に SSH で `bootlog` の `rpi5-hdmi-audio:` の行と、FAT の dtb・config.txt・overlay の一覧を回収して要約を出す。restore は元に戻し cksum を照らす）。

## 実機で記録する事

1. HVS の VERSION（stepping）、firmware が渡した dtb（tree の `compatible` と `model`）、FAT の dtb と config.txt の `device_tree`・`dtoverlay`（Q1）。
2. 両 port の DVP の reset、SCHEDULER_CONTROL、HOTPLUG、RAM_PACKET_CONFIG（全体の enable と slot）、CRP_CFG、MAI_CTL、MISC_CONFIG の gate（Q5・Q6）。
3. PHY の RM_OFFSET・REFFRQ・VCOCLK_DIV と求めた f_TMDS、HVS が測った refresh と firmware の mode（`display-device.c` の出す幅・高さ・refresh）と照らした f_TMDS の妥当さ（Q2）。
4. 使っていない port の読みの後の `drv_rpi5_serror_check()` の結果（Q8）。capture を閉じた状態（未接続）で 1 回、開いた状態で 1 回。
5. DMA の channel の CS・CB・DEBUG（他の誰かが使っていないか）。

## 受け入れ条件

- kernel の build（`make -j4 CONFIGMK=plan/ws349/tests/config-rpi5-hdmi-audio.mk BUILD=build/ws349-p004 vmunix`、timeout 3600 s）が warning 0。
- 実機の probe の要約が 1〜5 を全て持つ（無い項目は理由つきで未実施）。起動が login prompt まで進む（`plan/tools/boot-test.sh` か SSH の login で確かめる）。
- 結果で p001 の Q1・Q2・Q5（静的）・Q6・Q8 を閉じるか、閉じない理由を書き、D3・D8・D10 を直した（同じ commit）。
- 実機は restore で元に戻した（cksum の照合）。`sync` を流した。

## 注意

- kernel の log は register の値の記録にだけ使う（合否は要約の値で判定）。
- 再試行は同じ条件で 3 回まで。電源の入れ直しは Q1。
