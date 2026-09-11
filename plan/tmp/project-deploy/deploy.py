import json, subprocess, pathlib, time, re
D=pathlib.Path('plan/tmp/project-deploy'); P=json.loads((D/'project.json').read_text()); pid=P['id']
def save(n,x): (D/n).write_text(json.dumps(x,ensure_ascii=False,indent=2)+'\n')
def gh(args,body=None):
 r=subprocess.run(['gh']+args,input=json.dumps(body) if body is not None else None,text=True,capture_output=True)
 if r.returncode: raise RuntimeError(r.stderr+'\n'+r.stdout[:2000])
 x=json.loads(r.stdout) if r.stdout.strip() else None
 if isinstance(x,dict) and x.get('errors'): raise RuntimeError(str(x))
 return x
def gql(q): return gh(['api','graphql','--input','-'],{'query':q})['data']
def j(x): return json.dumps(x,ensure_ascii=False)
def fields(): return gql('{node(id:'+j(pid)+'){... on ProjectV2{fields(first:60){nodes{... on ProjectV2Field{id name dataType}... on ProjectV2SingleSelectField{id name options{id name}}}}}}}')['node']['fields']['nodes']
if __name__=='__main__':
 specs={'Logical ID':None,'Record Kind':['Board','WS','Phase'],'Primary Milestone':['MG%03d'%n for n in range(1,10)],'Related Milestones':None,'Objectives':None,'Awesome Plan Status':['planning','planned','in-progress','incomplete','completed','cleared','uncleared'],'Phase Disposition':['normal','canceled'],'WS Priority':'NUMBER','Focused Goal IDs':None,'Current Focus':['Yes','No'],'Queue ID':None,'Queue Item Status':['pending','in-progress','cleared','uncleared'],'Planning Category':['Other','Outlook','Current Queue'],'Parent WS':['ws%03d'%n for n in range(1,27)],'Status Note':None}
 fs=fields()
 for name,opt in specs.items():
  if any(f['name']==name for f in fs): continue
  args=['project','field-create',str(P['number']),'--owner','awemorris','--name',name,'--data-type','SINGLE_SELECT' if isinstance(opt,list) else ('NUMBER' if opt=='NUMBER' else 'TEXT'),'--format','json']
  if isinstance(opt,list): args+=['--single-select-options',','.join(opt)]
  gh(args); print('field',name,flush=True)
 fs=fields(); save('created-fields.json',fs); fby={f['name']:f for f in fs}
 issues=[]; cursor=None
 while True:
  after=',after:'+j(cursor) if cursor else ''
  x=gql('{repository(owner:"awemorris",name:"zedBSD"){issues(first:100'+after+'){nodes{id number body}pageInfo{hasNextPage endCursor}}}}')['repository']['issues']
  issues+=x['nodes']
  if not x['pageInfo']['hasNextPage']: break
  cursor=x['pageInfo']['endCursor']
 save('issues.json',issues); iby={i['number']:i for i in issues}
 rows=json.loads((D/'items.json').read_text()); state=json.loads((D/'state.json').read_text()) if (D/'state.json').exists() else {}
 # Reconcile membership before adding anything, including a resumed interrupted batch.
 cursor=None
 while True:
  after=',after:'+j(cursor) if cursor else ''
  x=gql('{node(id:'+j(pid)+'){... on ProjectV2{items(first:100'+after+'){nodes{id content{... on Issue{number repository{nameWithOwner}}}}pageInfo{hasNextPage endCursor}}}}}')['node']['items']
  for it in x['nodes']:
   c=it.get('content') or {}
   if c.get('repository',{}).get('nameWithOwner')=='awemorris/zedBSD': state[str(c['number'])]={'item_id':it['id'],**state.get(str(c['number']),{})}
  if not x['pageInfo']['hasNextPage']: break
  cursor=x['pageInfo']['endCursor']
 missing=[r for r in rows if str(r['issue_url'].split('/')[-1]) not in state]
 for off in range(0,len(missing),15):
  batch=missing[off:off+15]; q=[]
  for n,r in enumerate(batch):
   num=int(r['issue_url'].split('/')[-1]); issue=iby[num]
   assert 'record='+r['logical_id']+' ' in issue['body'] or 'record='+r['logical_id']+' -->' in issue['body']
   q.append('a%d:addProjectV2ItemById(input:{projectId:%s,contentId:%s}){item{id}}'%(n,j(pid),j(issue['id'])))
  got=gql('mutation{'+''.join(q)+'}')
  for n,r in enumerate(batch): state[r['issue_url'].split('/')[-1]]={'item_id':got['a'+str(n)]['item']['id']}
  save('state.json',state); print('added',len(state),flush=True); time.sleep(1)
 completed={2,5,6,8,10,11,12,16,18,19,20,21,22,23,24}
 focus={'ws025':'fg001, fg002, fg003','ws025-p029':'fg001','ws025-p030':'fg003','ws025-p032':'fg002','ws025-p038':'fg001'}
 priority={'ws025':1,'ws006':2,'ws022':3,'ws019':4,'ws002':5,'ws009':6}
 desired=[]
 for r in rows:
  lid=r['logical_id']; num=r['issue_url'].split('/')[-1]; body=iby[int(num)]['body']
  v={'Logical ID':lid,'Record Kind':r['kind'],'Planning Category':'Other','Current Focus':'Yes' if lid in focus else 'No'}
  if r.get('primary_milestone'): v['Primary Milestone']=r['primary_milestone']
  for key,name in [('related_milestones','Related Milestones'),('objectives','Objectives')]:
   if r.get(key): v[name]=', '.join(r[key])
  if lid in focus: v['Focused Goal IDs']=focus[lid]
  if lid in priority: v['WS Priority']=priority[lid]
  a=re.findall(r'(?im)^\s*(?:[-*] )?(?:\*\*)?(?:status|状態)(?:\*\*)?\s*[:：]\s*(.+)',body); raw=a[0].strip() if a else ''
  if r['kind']=='WS':
   w=int(lid[2:]); v['Awesome Plan Status']='completed' if w in completed else ('planned' if w==26 else 'incomplete')
   if w in (13,15): v['Awesome Plan Status']='planning'; raw='Future List; execution not scheduled'
   elif w==14: raw='Manual hold'
   elif w==9: raw='DOC54 manual-held dependency on WS014'
  elif r['kind']=='Phase':
   v['Parent WS']=r['parent_ws']; v['Phase Disposition']=r.get('phase_disposition','normal')
   s=raw.lower()
   if lid=='ws025-p028': v['Awesome Plan Status']='uncleared'; raw='canceled; adoption withdrawn, not achievement evidence'
   elif lid=='ws025-p038': v['Awesome Plan Status']='uncleared'; raw='q303 stopped; current callback/API and SPARC initial-frame verification remains'
   elif s.startswith(('uncleared','partial','blocked','carried forward')): v['Awesome Plan Status']='uncleared'
   elif s.startswith(('complete','cleared')): v['Awesome Plan Status']='cleared'
   elif s.startswith(('planned','pending','queue-ready')): v['Awesome Plan Status']='planned'
   elif s.startswith('future'): v['Awesome Plan Status']='planning'
   elif s.startswith('in-progress'): v['Awesome Plan Status']='in-progress'
   else: raw=raw or 'No explicit status; refer to Issue'
  if raw: v['Status Note']=raw[:1000]
  desired.append({'logical_id':lid,'number':int(num),'values':v})
 save('desired.json',desired)
 ops=[]
 for r in desired:
  st=state[str(r['number'])]
  for name,val in r['values'].items():
   if st.get('values',{}).get(name)==val: continue
   f=fby[name]
   if 'options' in f: value='{singleSelectOptionId:'+j(next(o['id'] for o in f['options'] if o['name']==val))+'}'
   elif name=='WS Priority': value='{number:'+str(val)+'}'
   else: value='{text:'+j(val)+'}'
   op='updateProjectV2ItemFieldValue(input:{projectId:%s,itemId:%s,fieldId:%s,value:%s}){projectV2Item{id}}'%(j(pid),j(st['item_id']),j(f['id']),value)
   ops.append((r['number'],name,val,op))
 for off in range(0,len(ops),35):
  batch=ops[off:off+35]; gql('mutation{'+''.join('a%d:%s'%(n,x[3]) for n,x in enumerate(batch))+'}')
  for num,name,val,_ in batch: state[str(num)].setdefault('values',{})[name]=val
  save('state.json',state); print('fields',min(off+35,len(ops)),'/',len(ops),flush=True); time.sleep(1)
 print('membership and field upload complete',flush=True)
