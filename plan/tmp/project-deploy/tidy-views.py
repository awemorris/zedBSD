from deploy import *
x=gql('{node(id:'+j(pid)+'){... on ProjectV2{views(first:20){nodes{id name number filter}}}}}')['node']['views']['nodes']
for v in x:
 if v['name']=='View 1': gql('mutation{deleteProjectV2View(input:{viewId:'+j(v['id'])+'}){clientMutationId}}')
 if v['name']=='Priority': gql('mutation{updateProjectV2View(input:{viewId:'+j(v['id'])+',filter:"record-kind:WS ws-priority:>0"}){projectV2View{id name filter}}}')
print('Default empty view removed; Priority numeric filter set')
