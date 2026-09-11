from pathlib import Path
import importlib.util,json
spec=importlib.util.spec_from_file_location('s',Path(__file__).with_name('sync.py'));s=importlib.util.module_from_spec(spec);spec.loader.exec_module(s)
D=s.SYNC;R=s.ROOT;state=json.loads((D/'state.json').read_text());pid='PVT_kwHOC2X95c4BjFGl'
def j(x):return json.dumps(x)
def gql(q):
 x=s.api('graphql','POST',{'query':q})
 if x.get('errors'):raise RuntimeError(str(x['errors']))
 return x['data']
fs=gql('{node(id:'+j(pid)+'){... on ProjectV2{fields(first:50){nodes{... on ProjectV2Field{id name}... on ProjectV2SingleSelectField{id name options{id name}}}}}}}')['node']['fields']['nodes'];f={x['name']:x for x in fs}
for k in ['queue','guardrail','future-work','bug-board','past-log']:
 node=json.loads((D/'base'/(k+'.json')).read_text())['node_id']
 op={'id':'adopt-project-'+k,'record':k,'kind':'project-item','status':'pending','content_id':node};p=D/'outbox'/(op['id']+'.json');s.write(p,op)
 item=gql('mutation{addProjectV2ItemById(input:{projectId:'+j(pid)+',contentId:'+j(node)+'}){item{id}}}')['addProjectV2ItemById']['item']['id']
 for name,value in [('Logical ID',k),('Record Kind','Board'),('Planning Category','Other')]:
  field=f[name];v='{singleSelectOptionId:'+j(next(o['id'] for o in field['options'] if o['name']==value))+'}' if 'options' in field else '{text:'+j(value)+'}'
  gql('mutation{updateProjectV2ItemFieldValue(input:{projectId:'+j(pid)+',itemId:'+j(item)+',fieldId:'+j(field['id'])+',value:'+v+'}){projectV2Item{id}}}')
 op.update(status='confirmed',item_id=item);s.write(p,op)
# Add a usable bug index view without pretending the historical bugs are separate tickets.
views=gql('{node(id:'+j(pid)+'){... on ProjectV2{views(first:30){nodes{id name}}}}}')['node']['views']['nodes']
if not any(v['name']=='Bugs' for v in views):
 fields=s.api('users/awemorris/projectsV2/2/fields?per_page=100');ff={f['name']:f['id'] for f in fields}
 payload={'name':'Bugs','layout':'table','filter':'logical-id:bug-board','visible_fields':[ff['Title'],ff['Logical ID']]}
 op={'id':'adopt-bugs-view','kind':'project-view','status':'pending','payload':payload};p=D/'outbox'/(op['id']+'.json');s.write(p,op)
 v=s.api('users/awemorris/projectsV2/2/views','POST',payload);op.update(status='confirmed',result=v);s.write(p,op)
# Link native milestones in the Master without rewriting the existing MG definitions.
r=state['records']['master'];i=s.issue(r);body=i['body'];ms=json.loads((R/'plan/milestones.json').read_text())
nav='\nGitHub Milestones: '+' · '.join('['+k+']('+v['url']+')' for k,v in ms.items())+'\n'
if '\nGitHub Milestones:' not in body:
 payload=body+nav;op={'id':'adopt-master-milestones','record':'master','kind':'issue-body','status':'pending','base_hash':s.digest(body),'payload':payload};p=D/'outbox'/(op['id']+'.json');s.write(p,op)
 s.api('repos/awemorris/zedBSD/issues/1','PATCH',{'body':payload});check=s.issue(r);assert check['body']==payload
 local=R/r['path'];text=local.read_text();local.write_text(text+nav if '\nGitHub Milestones:' not in text else text)
 s.accept_base('master',r,check);s.write(D/'state.json',state);op['status']='confirmed';s.write(p,op)
x=gql('{node(id:'+j(pid)+'){... on ProjectV2{items(first:1){totalCount}views(first:30){nodes{name number filter}}}}}')
s.write(D/'project-adoption-verified.json',x);assert x['node']['items']['totalCount']==366
print('Verified 366 Project items and standing board navigation')
