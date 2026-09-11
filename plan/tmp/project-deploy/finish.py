from deploy import *
readme='''[Master Board](https://github.com/awemorris/zedBSD/issues/1) の Objectives → Milestone Goals → WS → Phase を表示するProjectです。

Milestones / Workstreamsから各WS、Phasesから親WS別のPhaseを辿れます。Current Focusはfg001〜fg003、PriorityはMaster指定順です。完了済みPriorityも履歴として保持しています。

Awesome Plan Statusが計画上の状態です。Issueのopen/closedや既定Statusとは別です。Status Noteに元の条件・限定を残します。clearedは各Phaseが定めた範囲の記録であり、製品全体の完成を意味しません。Primary MilestoneはMG分類のカスタムフィールドで、GitHub native milestoneではありません。

Current Queue / Outlookは現在空です。q303は停止・終了済みで、新しい実行Queueは作っていません。Current Focusは実行許可ではありません。p028はcanceledで達成証拠から除外します。MG009はPrimary WS未定義のため、詳細はMasterを参照してください。

Issue本文が計画内容の正本です。このProjectは2026-09-10時点の表示用投影で、以後の変更はIssueとフィールドを併せて更新してください。自動同期は未導入です。
'''
save('project-readme.json',{'text':readme})
gh(['project','edit','2','--owner','awemorris','--description','zedBSD: Objectives → Milestone Goals → Workstreams → Phases','--readme',readme,'--format','json'])
# Preserve the latest issue body and only insert the navigation line.
issue=gh(['api','repos/awemorris/zedBSD/issues/1']); save('master-before.json',issue)
line='[GitHub Project — zedBSD / Awesome Plan](https://github.com/users/awemorris/projects/2)'
body=issue['body']
if line not in body:
 pos=body.find('\n',body.find('# ')) if '# ' in body else 0
 body=body[:pos+1]+'\n'+line+'\n'+body[pos+1:]
 save('master-patch.json',{'body':body})
 gh(['api','--method','PATCH','repos/awemorris/zedBSD/issues/1','--input','-'],{'body':body})
check=gh(['api','repos/awemorris/zedBSD/issues/1'])
assert check['body']==body
(D/'master-published.md').write_text(body+'\n')
for p in ['plan/tmp/github-import/master.md','plan/tmp/github-import/master-published.md','plan/tmp/traceability/master-published.md']:
 pathlib.Path(p).write_text(body+'\n')
p=pathlib.Path('plan/master.md'); s=p.read_text()
if line not in s: s=s.replace('\n','\n\n'+line+'\n',1); p.write_text(s)
print('Project README and Master navigation verified')
q='{user(login:"awemorris"){projectV2(number:2){id title public repositories(first:10){nodes{nameWithOwner}} views(first:20){nodes{id name number layout filter fields(first:40){nodes{... on ProjectV2Field{id name}... on ProjectV2SingleSelectField{id name}}}}}}}}'
x=gql(q); save('views-readback.json',x); print('views read back')
