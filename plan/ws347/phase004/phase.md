<!-- awesome-plan project=zedbsd record=ws347-p004 -->

# ws347-p004 VF2＋KC705 での動作確認とデバッグ

- 親: [WS347](../ws.md)。状態: planned。依存: p001・p003。WS290 p014 を引き継ぐ。

## 範囲

段ごとに確かめ、各段の要約を残す（WS290 design §16.2 の段を土台に）:
1. attach: PCI の match、DEVICE_CAPS・EXTCAP の読み、CP の起動。
2. ring・doorbell・fence・割り込み、空の submit。
3. memory と VM、compute の dispatch、shader での copy・fill。
4. 描画（三角形）と結果の読み戻し。
5. 表示（DISP2 がある時）。

- KC705 の側の不具合・未実装と思われる物は、driver の側の切り分けを記録してユーザーに示す（amaterrace-gpu は直さない）。
- 計測と確かめは 1 本の script（配置 → 起動 → 実行 → 回収 → 要約）にする。

## 受け入れ

- 段ごとの要約。動かない段は原因の切り分け（driver か FPGA か）と次の手。

## 試験の範囲（2026-11-11 ユーザーの決定）

- 今回は疎通の確認の試験。minimal の実機は SFU 無しなので、compiler が SFU の命令（rcp・rsq・exp2・log2・sin・cos・fsqrt.s）を使わない範囲の shader で試験する。
