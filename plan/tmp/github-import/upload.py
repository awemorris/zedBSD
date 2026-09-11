import json,os,re,subprocess,time,hashlib
from pathlib import Path
root=Path('/home/awe/zedBSD'); os.chdir(root)
out=root/'plan/tmp/github-import';out.mkdir(exist_ok=True)
repo='awemorris/zedBSD';sha=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()
statefile=out/'state.json'
state=json.loads(statefile.read_text()) if statefile.exists() else {'repository':repo,'base_commit':sha,'records':{},'status':'prepared'}
tracked=set(subprocess.check_output(['git','ls-tree','-r','--name-only',sha],text=True).splitlines())
renames=json.loads((root/'plan/tmp/ws-phase-directory-renames.json').read_text())
reverse=sorted(((x['to'],x['from']) for x in renames),key=lambda x:-len(x[0]))
paths=[root/'plan/master.md']+sorted(root.glob('plan/ws[0-9][0-9][0-9]/ws.md'))+sorted(root.glob('plan/ws[0-9][0-9][0-9]/phase[0-9][0-9][0-9]/phase.md'))
def identity(p):
 if p.name=='master.md':return 'master'
 if p.name=='ws.md':return p.parent.name
 return p.parent.parent.name+'-p'+p.parent.name[5:]
approved=json.loads((out/'manifest.json').read_text())
assert {e['source'] for e in approved}=={str(p.relative_to(root)) for p in paths}
for e in approved:
 assert hashlib.sha256((root/e['source']).read_bytes()).hexdigest()==e['sha256'], 'Source changed since approval: '+e['source']
records={identity(p):p for p in paths}; bypath={p.resolve():i for i,p in records.items()}
def save():
 tmp=statefile.with_suffix('.new');tmp.write_text(json.dumps(state,ensure_ascii=False,indent=2)+'\n');tmp.replace(statefile)
def gh(*args):return subprocess.check_output(['gh',*args],text=True).strip()
def pages_decode(text):
 decoder=json.JSONDecoder();pages=[]
 while text.strip():
  text=text.lstrip();value,end=decoder.raw_decode(text);pages.append(value);text=text[end:]
 return pages
def discover():
 pages=gh('api',f'repos/{repo}/issues?state=all&per_page=100','--paginate')
 result={}
 for page in pages_decode(pages):
  for item in page:
   m=re.search(r'<!-- awesome-plan project=zedbsd record=([^ ]+) -->',item.get('body') or '')
   if m:
    if m[1] in result:raise RuntimeError('Duplicate managed ID '+m[1])
    result[m[1]]=item
 return result
existing=discover()
for key,item in existing.items():
 if key in records:state['records'].setdefault(key,{}).update(number=item['number'],url=item['html_url'])
if not state['records'] and gh('api',f'repos/{repo}/issues?state=all&per_page=1')!='[]':raise RuntimeError('Repository no longer empty; recheck #1')
def remote_file(target):
 try:rel=str(target.relative_to(root))
 except ValueError:return None
 if rel not in tracked:
  for new,old in reverse:
   if rel==new or rel.startswith(new+'/'):
    candidate=old+rel[len(new):]
    if candidate in tracked:rel=candidate;break
 if rel not in tracked and rel.startswith('plan/old/'):
  candidate='plan/'+rel.removeprefix('plan/old/')
  if candidate in tracked:rel=candidate
 if rel in tracked:
  from urllib.parse import quote
  return f'https://github.com/{repo}/blob/{sha}/'+quote(rel)
 return None
link=re.compile(r'\[([^\]\n]*)\]\(([^\s)]+)\)')
def render(key):
 p=records[key];source=p.read_text(); title=source.splitlines()[0].lstrip('# ').strip()
 prefix=f'[{key}] '
 if key=='master':title='[Awesome Plan] Master Board'
 elif not title.lower().startswith(key):title=prefix+title
 def replace(m):
  label,url=m.groups();path,sep,fragment=url.partition('#')
  if not path or re.match(r'^[a-zA-Z][\w+.-]*:',path):return m[0]
  target=(p.parent/path).resolve();targetid=bypath.get(target)
  if targetid in state['records'] and state['records'][targetid].get('url'):
   return '['+label+']('+state['records'][targetid]['url']+')'
  dest=remote_file(target)
  if dest:return '['+label+']('+dest+(sep+fragment if sep else '')+')'
  return label+'（ローカル資料: `'+url+'`）'
 source=link.sub(replace,source)
 note='既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。'
 parent=''
 if key!='master':
  parentid='master' if p.name=='ws.md' else p.parent.parent.name
  parent=f"\n親: [{parentid}]({state['records'][parentid]['url']})\n"
 body=f'<!-- awesome-plan project=zedbsd record={key} -->\n\n{note}\n\n元資料: `{p.relative_to(root)}`\n'+parent+'\n'+source
 extras=[]
 while len(body)>58000:
  cut=body.rfind('\n',0,55000);extras.insert(0,body[cut:]);body=body[:cut]
 if extras:body+='\n\n続きはこのIssueの取り込み追記コメントに保持します。\n'
 return title[:250],body,extras
state['status']='publishing';save()
for key,p in records.items():
 title,body,extras=render(key);payload=out/(key+'.md')
 entry=state['records'].setdefault(key,{})
 if not entry.get('url') or not payload.exists():payload.write_text(body)
 entry.update(path=str(p.relative_to(root)),source_sha256=hashlib.sha256(p.read_bytes()).hexdigest(),payload=str(payload.relative_to(root)));save()
 if not entry.get('url'):
  entry['operation']='create-pending';save()
  try:url=gh('issue','create','--repo',repo,'--title',title,'--body-file',str(payload))
  except subprocess.CalledProcessError:
   found=discover().get(key)
   if not found:raise
   url=found['html_url']
  entry.update(url=url,number=int(url.rsplit('/',1)[1]),operation='created');save()
  if key=='master' and entry['number']!=1:raise RuntimeError('Master not #1; stop expansion')
  print(key,url,flush=True);time.sleep(2)
 for n,chunk in enumerate(extras):
  if n<entry.get('continuations',0):continue
  marker=f'<!-- awesome-plan-import record={key} continuation={n} -->'
  comments=pages_decode(gh('api',f"repos/{repo}/issues/{entry['number']}/comments?per_page=100",'--paginate'))
  if not any(marker in (c.get('body') or '') for page in comments for c in page):
   f=out/f'{key}-continuation-{n}.md';f.write_text(marker+'\n\n'+chunk)
   gh('issue','comment',str(entry['number']),'--repo',repo,'--body-file',str(f));time.sleep(2)
  entry['continuations']=n+1;save()
# User decision: do not prepend redundant child-Issue lists to bodies.
# Read back all Issues; retain actual published bodies separately for resume.
verified=discover()
for key in records:
 item=verified[key];assert item['number']==state['records'][key]['number']
 assert item['body'].rstrip()==(out/(key+'.md')).read_text().rstrip(), 'Readback mismatch: '+key
 (out/(key+'-published.md')).write_text(item['body'])
 state['records'][key]['verified']=True
state['status']='uploaded';save();print('UPLOAD VERIFIED',len(records),flush=True)
