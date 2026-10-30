<!-- awesome-plan project=zedbsd record=ws330-p024 -->

# ws330-p024（取り下げ: p024a と p024b に分けた）

- design-reviewer の指摘（Sonnet の 1 回に入らない恐れ）で、CU の再構成を [p024a](../phase024a/phase.md)（h265sw-inter: PU の motion と inter 予測）と [p024b](../phase024b/phase.md)（h265sw-recon: QP・intra・残差・PCM・bypass）に分けた。この file は Q1 が消す（`git rm`）。
