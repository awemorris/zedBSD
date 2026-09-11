from deploy import *
f=gh(['api','-H','X-GitHub-Api-Version: 2026-03-10','/users/awemorris/projectsV2/2/fields?per_page=100']); save('rest-fields.json',f); ids={x['name']:x['id'] for x in f}
common=['Title','Logical ID','Awesome Plan Status','Primary Milestone']
specs=[('Milestones / Workstreams','record-kind:WS',common+['Related Milestones','Objectives'],'Primary Milestone',None),('Current Focus','current-focus:Yes',common+['Focused Goal IDs','Status Note'],'Record Kind',None),('Priority','record-kind:WS has:ws-priority',common+['WS Priority','Status Note'],None,'WS Priority'),('Phases','record-kind:Phase',common+['Phase Disposition','Status Note'],'Parent WS',None),('Current Queue','planning-category:"Current Queue"',common+['Queue ID','Queue Item Status'],None,None),('Outlook','planning-category:Outlook',common+['Focused Goal IDs'],None,None)]
state=json.loads((D/'views.json').read_text()) if (D/'views.json').exists() else {}
for name,fil,visible,group,sort in specs:
 if name in state: continue
 b={'name':name,'layout':'table','filter':fil,'visible_fields':[ids[n] for n in visible]}
 if group: b['group_by']=[ids[group]]
 if sort: b['sort_by']=[[ids[sort],'asc']]
 save('view-request.json',b)
 state[name]=gh(['api','--method','POST','-H','X-GitHub-Api-Version: 2026-03-10','/users/awemorris/projectsV2/2/views','--input','-'],b)
 save('views.json',state); print('created view',name,flush=True)
