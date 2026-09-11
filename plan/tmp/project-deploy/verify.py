from deploy import *
rows=json.loads((D/'desired.json').read_text()); state=json.loads((D/'state.json').read_text()); fby={f['name']:f for f in fields()}
# Two explicit legacy-status exceptions, reviewed against their source Phase books.
for r in rows:
 changes={}
 if r['logical_id']=='ws002-p020': changes={'Awesome Plan Status':'cleared'}
 if r['logical_id']=='ws011-p004': changes={'Phase Disposition':'canceled','Awesome Plan Status':'uncleared'}
 if r['logical_id'] in ['ws025-p029','ws025-p030','ws025-p032','ws025-p038']:
  model=json.loads(pathlib.Path('plan/tmp/traceability/model.json').read_text())['phases'][r['logical_id']]
  related=[m for m in model['milestones'] if m!=r['values']['Primary Milestone']]
  changes['Related Milestones']=', '.join(related) or None
  changes['Objectives']=', '.join(model['objectives'])
 if r['logical_id'] in ['ws025-p023','ws025-p037']:
  changes['Status Note']='Historical phase completion; former vmap/VM ownership design was removed or replaced by 67b28ce0. Not current implementation evidence.'
 for name,val in changes.items():
  f=fby[name]
  prefix='projectId:'+j(pid)+',itemId:'+j(state[str(r['number'])]['item_id'])+',fieldId:'+j(f['id'])
  if val is None:
   gql('mutation{clearProjectV2ItemFieldValue(input:{'+prefix+'}){projectV2Item{id}}}')
  else:
   value='{singleSelectOptionId:'+j(next(o['id'] for o in f['options'] if o['name']==val))+'}' if 'options' in f else '{text:'+j(val)+'}'
   gql('mutation{updateProjectV2ItemFieldValue(input:{'+prefix+',value:'+value+'}){projectV2Item{id}}}')
  r['values'][name]=val; state[str(r['number'])].setdefault('values',{})[name]=val
save('state.json',state);save('desired.json',rows)
allitems=[]; cursor=None
while True:
 after=',after:'+j(cursor) if cursor else ''
 q='{node(id:'+j(pid)+'){... on ProjectV2{items(first:50'+after+'){totalCount nodes{id content{... on Issue{number url}} fieldValues(first:40){nodes{... on ProjectV2ItemFieldTextValue{text field{... on ProjectV2Field{name}}}... on ProjectV2ItemFieldNumberValue{number field{... on ProjectV2Field{name}}}... on ProjectV2ItemFieldSingleSelectValue{name field{... on ProjectV2SingleSelectField{name}}}}}}pageInfo{hasNextPage endCursor}}}}}'
 x=gql(q)['node']['items']; allitems+=x['nodes']
 if not x['pageInfo']['hasNextPage']: break
 cursor=x['pageInfo']['endCursor']
save('items-readback.json',allitems); assert len(allitems)==len(rows)==361
actual={i['content']['number']:i for i in allitems}; errors=[]
for r in rows:
 a=actual[r['number']]; vals={v['field']['name']:v.get('text',v.get('number',v.get('name'))) for v in a['fieldValues']['nodes'] if 'field' in v}
 for k,v in r['values'].items():
  if vals.get(k)!=v: errors.append([r['logical_id'],k,v,vals.get(k)])
save('verification-errors.json',errors); assert not errors,errors[:5]
queries={'Milestones / Workstreams':('record-kind:WS',26),'Current Focus':('current-focus:Yes',5),'Priority':('record-kind:WS ws-priority:>0',6),'Phases':('record-kind:Phase',334),'Current Queue':('planning-category:"Current Queue"',0),'Outlook':('planning-category:Outlook',0)}
counts={}
for name,(query,n) in queries.items():
 x=gql('{node(id:'+j(pid)+'){... on ProjectV2{items(first:1,query:'+j(query)+'){totalCount}}}}')['node']['items']['totalCount']; counts[name]=x
 assert x==n,(name,x,n)
save('view-counts.json',counts)
x=gql('{node(id:'+j(pid)+'){... on ProjectV2{url public repositories(first:10){nodes{nameWithOwner}}views(first:20){nodes{id name number filter layout sortByFields(first:10){nodes{direction field{... on ProjectV2Field{name}... on ProjectV2SingleSelectField{name}}}}groupByFields(first:10){nodes{... on ProjectV2Field{name}... on ProjectV2SingleSelectField{name}}}fields(first:40){nodes{... on ProjectV2Field{name}... on ProjectV2SingleSelectField{name}}}}}}}}')
save('views-readback.json',x)
assert len(x['node']['views']['nodes'])==6
print('Verified: 361 Issue memberships, all desired field values, 6 view filters/counts; no missing values')

views={v['name']:v for v in x['node']['views']['nodes']}
for name,field in [('Milestones / Workstreams','Primary Milestone'),('Current Focus','Record Kind'),('Phases','Parent WS')]:
 assert [f['name'] for f in views[name]['groupByFields']['nodes']]==[field]
assert views['Priority']['sortByFields']['nodes']==[{'direction':'ASC','field':{'name':'WS Priority'}}]
assert x['node']['public'] is False
assert {'nameWithOwner':'awemorris/zedBSD'} in x['node']['repositories']['nodes']
print('Verified view grouping, Priority ascending order, repository link, private visibility')
