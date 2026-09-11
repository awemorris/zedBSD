from pathlib import Path
import importlib.util,json,re
spec=importlib.util.spec_from_file_location('s',Path(__file__).with_name('sync.py'));s=importlib.util.module_from_spec(spec);spec.loader.exec_module(s)
R=s.ROOT;D=s.SYNC;state=json.loads((D/'state.json').read_text())
updates={'ws025':('incomplete',None,'修正後確認の残件を保持。新規目標・active Queueなし。'),'ws009':('incomplete',None,'DOC-54はWS014の手動保留に依存。'),'ws025-p028':('uncleared','canceled','ユーザー指示で採用撤回。削除済みの専用実装を復活させない。'),'ws025-p029':('uncleared','normal','修正後QEMU確認待ち。実機を必須条件にしない。'),'ws025-p030':('uncleared','normal','現行IMOD実装は確認済み。動作・比較の証拠は残確認。'),'ws025-p032':('planned','normal','現行対応は未着手。PC-9821V13/64MB/CF IDEの起動直後ビープ停止。'),'ws025-p038':('uncleared','normal','q303停止済み。現行APIとSPARC初回フレームの確認が残る。')}
for k,(status,disp,note) in updates.items():
 r=state['records'][k];remote=s.issue(r);local=R/r['path'];old=local.read_text()
 block='<!-- awesome-plan-current:start -->\n\n## Current state — 2026-09-11 adoption\n\nStatus: '+status+'\n'+('Phase disposition: '+disp+'\n' if disp else '')+'\n'+note+'\n以下の旧試行記録は履歴として保持。本項は既存判断の正規化であり実装の再開許可ではない。\n\n[Guardrail](https://github.com/awemorris/zedBSD/issues/363) · [Queue](https://github.com/awemorris/zedBSD/issues/362)\n\n<!-- awesome-plan-current:end -->\n'
 def insert(text):
  if '<!-- awesome-plan-current:start -->' in text:return re.sub(r'<!-- awesome-plan-current:start -->.*?<!-- awesome-plan-current:end -->\n?',block,text,flags=re.S)
  m=re.search(r'^# .+\n',text,re.M);pos=m.end() if m else 0
  return text[:pos]+'\n'+block+'\n'+text[pos:]
 payload=insert(remote['body']);newlocal=insert(old)
 op={'id':'adopt-current-'+k,'record':k,'kind':'issue-body','status':'pending','base_hash':s.digest(remote['body']),'local_before':old,'local_after':newlocal,'payload':payload};p=D/'outbox'/(op['id']+'.json');s.write(p,op)
 local.write_text(newlocal)
 if remote['body']!=payload:s.api('repos/awemorris/zedBSD/issues/'+str(r['number']),'PATCH',{'body':payload})
 result=s.issue(r);assert result['body']==payload;s.accept_base(k,r,result);s.write(D/'state.json',state);op['status']='confirmed';s.write(p,op)
# Future Work now owns the table; retain the Master anchor as navigation.
r=state['records']['master'];remote=s.issue(r);body=remote['body']
a=body.find('## Future List');b=body.find('## Known bugs',a)
if a>=0 and b>a:
 payload=body[:a]+'## Future List(やりたいことリスト)\n\n[F-001〜F-003の由来・再開点はFuture Work](https://github.com/awemorris/zedBSD/issues/364)。現在の実行対象ではない。\n\n'+body[b:]
 op={'id':'adopt-master-future-owner','record':'master','kind':'issue-body','status':'pending','base_hash':s.digest(body),'payload':payload};p=D/'outbox'/(op['id']+'.json');s.write(p,op)
 s.api('repos/awemorris/zedBSD/issues/1','PATCH',{'body':payload});result=s.issue(r);assert result['body']==payload;s.accept_base('master',r,result);s.write(D/'state.json',state);op['status']='confirmed';s.write(p,op)
print('Current remaining work normalized; Future owner linked; no new execution authorization')
