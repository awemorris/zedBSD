"""One-time, resumable standing-board setup; not a general synchronization command."""
from pathlib import Path
import importlib.util
import json
import re
import os
import time

spec=importlib.util.spec_from_file_location('sync',Path(__file__).with_name('sync.py')); s=importlib.util.module_from_spec(spec); spec.loader.exec_module(s)
R=s.ROOT; D=s.SYNC
old=json.loads((R/'plan/tmp/github-import/state.json').read_text())['records']
mapfile=R/'plan/records.json'
records=json.loads(mapfile.read_text())['records'] if mapfile.exists() else {k:{'path':v['path'],'number':v['number'],'url':v['url']} for k,v in old.items()}
boards={'queue':('plan/queue.md','Queue'),'guardrail':('plan/guardrail.md','Guardrail'),'future-work':('plan/future-work.md','Future Work'),'bug-board':('plan/known-bugs.md','Bug Board'),'past-log':('plan/history/index.md','Past Log')}
allissues=[]; page=1
while True:
 batch=s.api('repos/awemorris/zedBSD/issues?state=all&per_page=100&page='+str(page)); allissues+=batch
 if len(batch)<100: break
 page+=1
byid={}
for i in allissues:
 m=re.search(r'<!-- awesome-plan project=zedbsd record=([^ ]+) -->',i.get('body') or '')
 if m:
  if m[1] in byid: raise RuntimeError('Duplicate logical identity '+m[1])
  byid[m[1]]=i
for k,(path,title) in boards.items():
 if k in byid:
  i=byid[k]
 else:
  marker='<!-- awesome-plan project=zedbsd record='+k+' -->'
  payload={'title':'[Awesome Plan] '+title,'body':marker+'\n\n# '+title+'\n\nAwesome Plan運用移行中。内容を続けて反映します。'}
  op={'id':'adopt-create-'+k,'record':k,'kind':'issue-create','status':'pending','payload':payload}
  opath=D/'outbox'/(op['id']+'.json');s.write(opath,op)
  i=s.api('repos/awemorris/zedBSD/issues','POST',payload);byid[k]=i
  op.update(status='confirmed',number=i['number'],url=i['html_url']);s.write(opath,op)
 records[k]={'path':path,'number':i['number'],'url':i['html_url']}
 s.write(mapfile,{'project':'zedbsd','repository':'awemorris/zedBSD','records':records})
 print('board',k,records[k]['number'],flush=True)
paths={str((R/v['path']).resolve()):v['url'] for v in records.values()}
def render(k):
 path=R/records[k]['path']; text=path.read_text()
 def link(m):
  label,target=m[1],m[2]
  if '://' in target or target.startswith('#'): return m[0]
  raw,sep,frag=target.partition('#'); absolute=str((path.parent/raw).resolve())
  if absolute in paths: return '['+label+']('+paths[absolute]+(sep+frag if sep else '')+')'
  return label+'（ローカル資料: `'+os.path.relpath(absolute,R)+'`、未公開）'
 text=re.sub(r'\[([^\]]+)\]\(([^)]+)\)',link,text)
 return '<!-- awesome-plan project=zedbsd record='+k+' -->\n\n'+text+'\n\n[Master]('+records['master']['url']+')\n'
for k in boards:
 i=s.api('repos/awemorris/zedBSD/issues/'+str(records[k]['number'])); payload=render(k)
 op={'id':'adopt-body-'+k,'record':k,'kind':'issue-body','base_hash':s.digest(i['body'] or ''),'payload':payload,'status':'pending'}; opath=D/'outbox'/(op['id']+'.json');s.write(opath,op)
 if i['body']!=payload: s.api('repos/awemorris/zedBSD/issues/'+str(i['number']),'PATCH',{'body':payload})
 i=s.api('repos/awemorris/zedBSD/issues/'+str(i['number']));assert i['body']==payload
 byid[k]=i;op['status']='confirmed';s.write(opath,op)
# Publish the navigation using the fresh Master, never the historical upload payload.
i=s.api('repos/awemorris/zedBSD/issues/1');body=i['body']
nav='\n## Awesome Plan運用\n\n'+ ' · '.join('['+title+']('+records[k]['url']+')' for k,(_,title) in boards.items())+'\n\nGitHub modeで運用。q303はfinished/stopped、active Queueはありません。仕様固定版は314a669f57265da3084ffac810871b0e660c9526。次回はリポジトリAGENTS.mdとplan/config.mdから開始します。\n'
if '<!-- awesome-plan-operations:start -->' not in body:
 body+='\n<!-- awesome-plan-operations:start -->\n'+nav+'\n<!-- awesome-plan-operations:end -->\n'
 op={'id':'adopt-master-navigation','record':'master','kind':'issue-body','base_hash':s.digest(i['body']),'payload':body,'status':'pending'};s.write(D/'outbox'/(op['id']+'.json'),op)
 s.api('repos/awemorris/zedBSD/issues/1','PATCH',{'body':body});i=s.api('repos/awemorris/zedBSD/issues/1');assert i['body']==body
 op['status']='confirmed';s.write(D/'outbox'/(op['id']+'.json'),op)
byid['master']=i
local=R/'plan/master.md';text=local.read_text()
if '<!-- awesome-plan-operations:start -->' not in text: local.write_text(text+'\n<!-- awesome-plan-operations:start -->\n'+nav+'\n<!-- awesome-plan-operations:end -->\n')
state={'version':1,'project':'zedbsd','mode':'github','records':{},'project_url':'https://github.com/users/awemorris/projects/2','active_queue':None}
for k,r in records.items():
 i=byid[k]; rr=dict(r);s.accept_base(k,rr,i);state['records'][k]=rr
s.write(D/'state.json',state)
s.write(D/'owner.json',{'owner':'current adoption session','state':'active','purpose':'planning migration only; no Queue execution'})
# Retain all original migration payloads as history. Shared mapping is independent.
print('Seeded',len(state['records']),'records; all Board bodies and Master read back',flush=True)
