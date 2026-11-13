<!-- awesome-plan project=zedbsd record=ws353-p017 -->

# ws353-p017: 規約の全文の見直し

Status: planned
Parent: [WS353](../ws.md)。

## 目的

WS353 が変えた C の code（kernel の gpu・driver、libvulkan、compositor、backend）を [coding-style.md](../../coding-style.md) の全文と照らし、書き方だけを直す（動作は変えない）。source の comment・log に開発の管理情報（WS・Phase の番号など）が無い事も確かめる。

## 確かめ

- build warning 0、変えた所の host 試験の再実行。

## 依存

- p016。
