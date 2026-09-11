from pathlib import Path
import importlib.util,json,subprocess,time
spec=importlib.util.spec_from_file_location('s',Path(__file__).with_name('sync.py'));s=importlib.util.module_from_spec(spec);spec.loader.exec_module(s)
R=s.ROOT;D=s.SYNC;state=json.loads((D/'state.json').read_text()); model=json.loads((R/'plan/tmp/traceability/model.json').read_text())
def gql(q):
 x=s.api('graphql','POST',{'query':q})
 if x.get('errors'): raise RuntimeError(str(x['errors']))
 return x['data']
def j(x):return json.dumps(x)
ms=s.api('repos/awemorris/zedBSD/milestones?state=all&per_page=100');mapping={}
for k,v in model['milestones'].items():
 matches=[m for m in ms if m['title'].startswith(k+':')]
 if matches:m=matches[0]
 else:
  payload={'title':k+': '+v['title'],'description':'Objectives: '+', '.join(v['objectives'])+'\n\n'+v['acceptance']+'\n\nStatus: planning. Master: https://github.com/awemorris/zedBSD/issues/1'}
  op={'id':'adopt-milestone-'+k,'kind':'milestone-create','status':'pending','payload':payload};p=D/'outbox'/(op['id']+'.json');s.write(p,op)
  m=s.api('repos/awemorris/zedBSD/milestones','POST',payload);op.update(status='confirmed',number=m['number']);s.write(p,op)
 mapping[k]={'number':m['number'],'node_id':m['node_id'],'url':m['html_url']}
s.write(R/'plan/milestones.json',mapping)
# Read actual parent/milestone first; never replace an existing different parent.
ids={k:json.loads((D/'base'/(k+'.json')).read_text())['node_id'] for k in state['records']}
items=[(k,v) for k,v in state['records'].items() if k.startswith('ws')]
operations=[]
for off in range(0,len(items),40):
 batch=items[off:off+40]; got=gql('{'+''.join('a%d:node(id:%s){... on Issue{id parent{id} milestone{id}}}'%(n,j(ids[k])) for n,(k,v) in enumerate(batch))+'}')
 for n,(k,v) in enumerate(batch):
  actual=got['a'+str(n)];ws=k.split('-p')[0];parent=ws if '-p' in k else 'master';mg=model['workstreams'][ws]['primary'];mid=mapping[mg]['node_id']
  if actual['parent'] and actual['parent']['id']!=ids[parent]:raise RuntimeError('Existing different parent '+k)
  if not actual['parent']:operations.append((k,'parent','addSubIssue(input:{issueId:%s,subIssueId:%s}){issue{id}}'%(j(ids[parent]),j(ids[k]))))
  if not actual['milestone'] or actual['milestone']['id']!=mid:operations.append((k,'milestone','updateIssue(input:{id:%s,milestoneId:%s}){issue{id}}'%(j(ids[k]),j(mid))))
for off in range(0,len(operations),8):
 batch=operations[off:off+8];q='mutation{'+''.join('a%d:%s'%(n,o[2]) for n,o in enumerate(batch))+'}'
 op={'id':'adopt-relations-'+s.digest(q)[:16],'kind':'graphql','status':'pending','payload':q};p=D/'outbox'/(op['id']+'.json');s.write(p,op)
 gql(q);op['status']='confirmed';s.write(p,op);print('relations',min(off+8,len(operations)),'/',len(operations),flush=True);time.sleep(1)
checks=[]
for off in range(0,len(items),40):
 batch=items[off:off+40]; got=gql('{'+''.join('a%d:node(id:%s){... on Issue{id parent{id} milestone{id}}}'%(n,j(ids[k])) for n,(k,v) in enumerate(batch))+'}')
 for n,(k,v) in enumerate(batch):
  a=got['a'+str(n)];ws=k.split('-p')[0];parent=ws if '-p' in k else 'master'
  assert a['parent']['id']==ids[parent];assert a['milestone']['id']==mapping[model['workstreams'][ws]['primary']]['node_id'];checks.append(k)
s.write(D/'relations-verified.json',{'records':checks,'count':len(checks)})
print('Verified',len(checks),'native parent and milestone assignments',flush=True)

for p in (D/'outbox').glob('adopt-relations-*.json'):
 op=json.loads(p.read_text())
 if op['status']=='pending':
  op['status']='confirmed';op['verification']='complete parent/milestone read-back after timeout';s.write(p,op)
