<!-- awesome-plan project=zedbsd record=ws065p002 -->

# ws065-p002: 展開の拡張

Phase ID: `ws065-p002`
Parent: [WS065](../ws.md)
Status: in-progress
Queue: q452-i01
Disposition: normal

## 範囲

`${v:offset}`・`${v:offset:length}`（負の値を含む）、`${v/p/r}`・`${v//p/r}`・`${v/#p/r}`・`${v/%p/r}`、`${v^}`・`${v^^}`・`${v,}`・`${v,,}`、`${!v}`（間接）。`$@`・`$*` への適用は bash と同じ。

## 受け入れ

bash を参照にした case が bash と同じ。dash との差分試験が変わらない。
