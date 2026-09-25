<!-- awesome-plan project=zedbsd record=ws065p003 -->

# ws065-p003: builtin の拡張

Phase ID: `ws065-p003`
Parent: [WS065](../ws.md)
Status: in-progress
Queue: q453-i01
Disposition: normal

## 範囲

`source`（`.` と同じ、PATH の次に今の directory も探す bash の動き）、`let`、`test`・`[` の `==`、`declare`・`typeset`（`-r`・`-x`・`-i` は値の型なしで、`-g`、`-p`、scalar のみ）、`pushd`・`popd`・`dirs`、`printf -v 名前`、`builtin`。

## 受け入れ

bash を参照にした case が bash と同じ。dash との差分試験が変わらない。
