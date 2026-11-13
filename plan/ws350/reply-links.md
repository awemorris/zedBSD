<!-- awesome-plan project=zedbsd record=ws350-reply-links -->

# Raspberry Pi 500 の報告への返信用の一覧

2026-11-13 の外部の利用者の Raspberry Pi 500 の報告から立てた記録。path は repository の中の物。2026-09-23 以降の記録は GitHub に未公開なので（AGENTS.md §11）、これらはまだ公開の URL にならない（公開はユーザーの指示の後）。

| 記録 | 一行の説明 |
| --- | --- |
| [plan/ws348/ws.md](../ws348/ws.md) | Pi 500 の device tree（`bcm2712-rpi-500.dtb`）を rpi5 の image の FAT に入れ、dtb を足さずに起動できるようにする（#1） |
| [plan/ws349/ws.md](../ws349/ws.md) | Pi 5・Pi 500 の HDMI の音（今は音の device が無い）。設計の第一版まで（#6） |
| [plan/ws350/ws.md](ws.md) | Pi 500 を動作の確かめた機種として README と機種の表に載せる（#7） |
| [plan/bugs/BUG-425.md](../bugs/BUG-425.md) | 設定アプリに有線 LAN の項目が出ない（#2）。今の main では設定の全 page に出て再現せず。初期設定の network の段は Wi-Fi だけを出す作り |
| [plan/bugs/BUG-426.md](../bugs/BUG-426.md) | 初回の起動でアプリの click が効かない事がある（#3）。今の main では初期設定の直後に 4 つのアプリが開き再現せず |
| [plan/bugs/BUG-427.md](../bugs/BUG-427.md) | Wi-Fi の pin の設定の時に ARM64 SError（#4）。Pi 5 で毎起動に再現し、D0 の pin controller の実装の外の register を読む診断が原因と分かり直した |
| [docs/howto/build-from-source.md](../../docs/howto/build-from-source.md) | build の手順（#8）: 今の build system（`CONFIGMK=config/config.rpi5.mk`、`llvm-toolchain-cached` → `sysroot` → `firmware-licenses` → `disk`）に書き直した |

#5（起動の直後の時計）は仕様として記録を立てていない。
