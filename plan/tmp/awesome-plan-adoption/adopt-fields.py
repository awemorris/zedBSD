from pathlib import Path
import importlib.util,json,subprocess
spec=importlib.util.spec_from_file_location('s',Path(__file__).with_name('sync.py'));s=importlib.util.module_from_spec(spec);spec.loader.exec_module(s)
D=s.SYNC;pid='PVT_kwHOC2X95c4BjFGl'
def j(x):return json.dumps(x)
def gql(q):
 x=s.api('graphql','POST',{'query':q})
 if x.get('errors'):raise RuntimeError(str(x['errors']))
 return x['data']
fs=gql('{node(id:'+j(pid)+'){... on ProjectV2{fields(first:50){nodes{... on ProjectV2Field{id name}... on ProjectV2SingleSelectField{id name options{id name color description}}}}}}}')['node']['fields']['nodes'];f={x['name']:x for x in fs}
for name,new in [('Record Kind',['Bug','Queue History']),('Awesome Plan Status',['proposed','active','finished'])]:
 options=f[name]['options'];existing={o['name'] for o in options}
 for value in new:
  if value not in existing:options.append({'name':value,'color':'GRAY','description':''})
 body='['+','.join('{'+','.join(k+':'+(v if k=='color' else j(v)) for k,v in o.items())+'}' for o in options)+']'
 q='mutation{updateProjectV2Field(input:{fieldId:'+j(f[name]['id'])+',singleSelectOptions:'+body+'}){projectV2Field{... on ProjectV2SingleSelectField{id name options{id name}}}}}'
 op={'id':'adopt-options-'+name.replace(' ','-'),'kind':'project-field','status':'pending','payload':q};p=D/'outbox'/(op['id']+'.json');s.write(p,op);result=gql(q);op.update(status='confirmed',result=result);s.write(p,op)
for name,values in [('Bug Reproduction','unknown,unreproduced,reproduced'),('Bug Disposition','tracking,scheduled,resolved,duplicate')]:
 if name not in f:
  op={'id':'adopt-field-'+name.replace(' ','-'),'kind':'project-field-create','status':'pending','name':name,'options':values};p=D/'outbox'/(op['id']+'.json');s.write(p,op)
  subprocess.run(['gh','project','field-create','2','--owner','awemorris','--name',name,'--data-type','SINGLE_SELECT','--single-select-options',values,'--format','json'],check=True,capture_output=True)
  op['status']='confirmed';s.write(p,op)
s.write(D/'fields-adopted.json',gql('{node(id:'+j(pid)+'){... on ProjectV2{fields(first:50){nodes{... on ProjectV2Field{id name}... on ProjectV2SingleSelectField{id name options{id name}}}}}}}'))
print('Future Bug/Queue History kinds and states available; existing option IDs retained')
