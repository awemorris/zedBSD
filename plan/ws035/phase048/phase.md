<!-- awesome-plan project=zedbsd record=ws035p048 -->

# ws035-p048: HD Audio の既定を ON

Phase ID: `ws035-p048`
Parent: [WS035](../ws.md)
Status: **cleared**（q350-i01、2026-09-24）
Phase disposition: normal
Queue: q350（q350-i01）
実行: メインセッション

## 経緯

2026-09-24 ユーザー決定「HDA はデフォルト ON で OK」。

## 変更

- `config/drivers/pci.drivers`: `CONFIG_DRIVER_PCI_HDA` の既定を `y`、label から「experimental」を外した（対象は従来どおり amd64 だけ）。
- `Makefile`: config がこの key を持たないとき（CI の config など）の既定を、**amd64 では `y`、他では `n`** にした。
  driver が build されるのは amd64 だけなので、i386 で `y` にすると `pcat.c` の登録が未定義の関数を呼ぶ。
  menu が書いた config は key を明示するので、menu 側の既定と同じになる。

## 検証

| 検証 | 結果 |
| --- | --- |
| menu の既定を save（amd64・i386・pc98） | amd64 は `y`、i386・pc98 は `n` |
| CI の kernel（`config/ci/config-{amd64,pcat,pc98}.mk`） | 3 つとも warning 0。amd64 の vmunix に hda・audio の symbol がある |
| menu の既定の amd64 image（`plan/ws035/tests/config-amd64-p048-default.mk`。serial の操作のため command line で `CONFIG_PCAT_SERIAL_MIRROR=y`）を ICH9 HDA 付きで起動 | `/dev/dsp0`・`/dev/mixer0` があり、`hda: codec 0, 1 output pin(s), input no, volume yes, 2 format(s), MSI` |
| 同じ image を HDA 無しで起動 | login でき、`/dev/dsp0` は無い（driver が付かないだけ） |
