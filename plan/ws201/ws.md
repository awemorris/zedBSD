<!-- awesome-plan project=zedbsd record=ws201 -->

# WS201: /home の暗号化（UFS の key slot、FIDO2 の hmac-secret／PRF と回復のパスワード）

<!-- awesome-plan-current:start -->
Status: planning
Primary Milestone: MG002
Related Milestones: MG006
Objectives: O2
Parent: [Master](../master.md)
Queue: 未割当
Target: **ベータ3**（2026-10-10 ユーザー。「WSだけ追加してください。検討は今は不要です。」）
Resume point: p001（設計）から。今は検討しない。
<!-- awesome-plan-current:end -->

## 由来（2026-10-10 ユーザー、原文）

```
ベータ3でホームディレクトリの暗号化を行います。WSだけ追加してください。検討は今は不要です。

UFSの先頭にキースロットを置きます。

[ Disk Header / Metadata ]
├── Salt (HMAC-secretに渡すソルト値など)
├── Master Key Verification (復号成功判定用のハッシュ/マジックバイト)
└── Key Slots (複数用意)
    ├── Slot 0: FIDO2 Token (HMAC-secret 導出鍵でラップされた Master Key)
    ├── Slot 1: Recovery Password (Argon2等で導出された鍵でラップされた Master Key。アルゴリズム未定)
    ├── Slot 2: Spare FIDO2 Token (予備キー用)
    └── Slot 3: Empty / Disabled

セキュリティーキーが登録され、ディスク暗号化が有効にされた場合、ログイン時にFIDO2でPRFの鍵を生成し、鍵でスロットを開けます。/home/以下だけが暗号化対象になります。inodeに暗号化フラグがつきます。ディスク全体で真の共通鍵は1つでいいです。ディスク暗号化を有効にする初回操作では、/home以下を暗号化する処理を走らせます。共通鍵は、USBイメージに固定されるとよくないので、初回スロット設定時に鍵も/dev/ramdomで生成するのがいいと思います。
```

## 目標（ユーザーの文の要約、設計は p001 で）

- UFS の先頭に disk の header（salt、master key の確かめ、key slot 4 つ: FIDO2・回復のパスワード・予備の FIDO2・空き）。
- master key は disk に 1 つ。初回の slot の設定の時に /dev/random で作る（USB の image に固定しない）。
- 暗号化の対象は /home 以下だけ。inode に暗号化の flag。
- ログインの時に FIDO2 の hmac-secret（PRF）で鍵を作り、slot を開ける。
- 暗号化を有効にする初回の操作で /home 以下を暗号化する処理を走らせる。
- 関連: [WS199](../ws199/ws.md)（セキュリティキーの管理）・WS172（passkey の枠組み、TPM）。

## Phase

| Phase | 目的 | Status |
| --- | --- | --- |
| p001 | 設計（header と slot の形式、鍵の導出と wrap、inode の flag、UFS と buffer cache への組み込み、ログインの経路、初回の変換、回復）。design-reviewer。ベータ3 | planning |
