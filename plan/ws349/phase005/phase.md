<!-- awesome-plan project=zedbsd record=ws349-p005 -->

# ws349-p005 再生の driver と audio の core への登録

- WS: [ws.md](../ws.md)。設計: [p001](../phase001/phase.md)（p004 の結果で直した版）。依存: p004。
- 状態: planned
- 担当の定義の推し: phase-runner-high。

## 目的

p001 の「起動から再生までの順」と D5〜D10・D14 を実装し、`/dev/dsp0` を出す。

## 作る物

- `rpi5-hdmi-audio.c` の残り: DMA の device と ring・control block の確保（D6）、`drv_audio_ops`（prepare・start・stop・position、S16_LE・2・48000 の 1 形式）、割り込みの手続き（D7 の順、D14 の lock）、監視の kernel thread と software の時計（D10）、HDMI の側と MAI の書き込み（D2 の境界の中だけ）、InfoFrame の手順（thread の中だけ）、DMA の止め方（D5）。
- p004 の probe の klog は、attach の 1 行の要約（port、stepping、DREQ、f_TMDS、N・CTS）にまとめる。
- `docs/architecture/audio.md`: kernel の audio の層の例に「SoC の HDMI（Raspberry Pi 5）」を足し、D11 の制約（Pi 5 で mode を変える driver は音の driver を止めてから）を 1 段落。docs から plan/ へ link しない。

## 受け入れ条件

- kernel の build（試験の config、`BUILD=build/ws349-p005`、timeout 3600 s）が warning 0。p003 の host 試験が PASS のまま。
- 自分のレビュー: stop の後に ring に触れない、割り込みの手続きが HDMI の register を書かない、書く register が D2 の一覧の中だけ、待ちに全て上限、`drv_audio_interrupt()` を自分の lock の外で呼ぶ。結果を phase.md に箇条で。
- 出典は block ごと（Values referenced・Procedure referenced）、名前は独自、source に開発の管理情報を書かない。
- 実機の試験は p006（この Phase では build と host 試験まで）。

## 試験（短い）

- `sh plan/ws349/tests/run-hdmi-audio-wire-test.sh`。
