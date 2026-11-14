<!-- awesome-plan project=zedbsd record=ws349-p006 -->

# ws349-p006 Pi 5 の実機の再生の試験

- WS: [ws.md](../ws.md)。設計: [p001](../phase001/phase.md)。依存: p005。実機: Q1 が Pi 5 を割り当てる（capture の板のある host で）。
- 状態: planned
- 担当の定義の推し: phase-runner-high。

## 目的

HDMI から音が正しく出る事を、host の capture の録音の解析で確かめる。

## 道具

- `plan/ws349/tests/pi5-hdmi-audio.sh` に `play` の段を足す: deploy の後、board で音を鳴らし、host で capture の板の音（USB audio）を録り、解析して要約を出す（1 本の script、読むのは要約だけ）。
- board で鳴らす道具: `userland/tests/audio-loop` に再生だけの形（左右で別の周波数、長さ、短い音の繰り返し、audiod 経由と `/dev/dsp0` への直の write）を足すか、試験の config だけに入る小さな program を `plan/ws349/tests/` に置く（どちらにするかは担当が決め、範囲は試験の道具に限る）。
- host の解析: Python（`-I`）で録音の左右の周波数・振幅・左右の入れ替わり・開始の遅れ・途切れ（0 の連続や位相の跳び）を数える。

## 試験の項目

1. Q7: capture の板が音の device を出すか。出さなければ 2〜7 は未実施とし、人の耳の確かめを「ユーザーが拾える試験の一覧」に載せるよう Q1 に送る（依頼しない）。
2. audiod 経由: 左 1 kHz・右 1.5 kHz、−12 dBFS、10 秒。周波数・振幅（±1 dB）・左右・開始の遅れ。
3. `/dev/dsp0` への直の write（audiod を止めて）: 2 と同じ。
4. 20 ms の音を 1 秒ごとに 10 回: 全部が録音にある。
5. 5 分の連続の再生: 途切れ 0（Q3）。EXTERNAL_CTS_EN を立てた時と落とした時の 2 通り（試験の build の切り替えか sysctl、担当が決める）。
6. hotplug: 再生の途中に capture を閉じ、2 秒後に開く。閉じた間に書き手が止まらない（software の時計）、開いた後に音が戻る（Q4・Q5）。
7. 2 つの dtb: config.txt の `device_tree=` で C1 と D0 の dtb で起動し、2 を通す（Q1 の照合の経路）。
8. 再生の途中の reboot が戻る（D15）。
9. 別の mode の monitor の付け替え（CTS の計算し直し）: 台に別の monitor が無ければ未実施。

## 受け入れ条件

- 要約で 2〜8 が PASS（未実施は理由つき）。QEMU の試験は無い（Pi 5 の機種が無い）。
- 結果で p001 の Q3・Q4・Q5・Q7 を閉じる。
- 実機は restore で元に戻した。`sync`。
- PASS の後に、Q1 へ「defaults.mk の y の案を当てる時期」（U1）の報告。
