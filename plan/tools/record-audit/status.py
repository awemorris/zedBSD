#!/usr/bin/env python3
# Counts mismatches between the plan records: the WS tables (workstreams-open.md, workstreams-closed.md) vs ws.md, ws.md Phase tables vs phase.md,
# Phases without a Status line, completed WSs that still hold Phase directories or tests, and WSs still
# planning/planned although a Phase ran.  Run from the repository root:
#   python3 -I plan/tools/record-audit/status.py | awk "{print \$1}" | sort | uniq -c
# Written by the 2026-10-15 records audit (plan/record-integrity.md); the status words are matched by keyword, so treat counts as a guide.
import re,os,glob
KW=['planning','planned','incomplete','completed','closed','in-progress','cleared','uncleared','canceled','cancelled']
def first_kw(s):
    s=s.lower()
    best=None
    for k in KW:
        m=re.search(r'(?<![a-z-])'+k+r'(?![a-z])',s)
        if m and (best is None or m.start()<best[0]): best=(m.start(),k)
    return best[1] if best else None
def status_line(path):
    for i,l in enumerate(open(path,encoding='utf-8',errors='replace'),1):
        if re.match(r'^\s*(-\s*)?\**\s*(Status|状態)\s*\**\s*[:：]',l):
            return i,l.strip()
        if i>60: break
    return None,None
reg={}
# The WS rows live in two tables: open WSs in plan/workstreams-open.md, completed/canceled/closed WSs in plan/workstreams-closed.md.
S=open('plan/workstreams-open.md',encoding='utf-8').read().split('\n')+open('plan/workstreams-closed.md',encoding='utf-8').read().split('\n')
for l in S:
    m=re.match(r'^\| \[WS(\d+)\]\(ws(\d+)/ws\.md\) \| ([^|]*)\| ([^|]*)\| ([^|]*)\|',l)
    if m: reg.setdefault(m.group(1),[]).append(m.group(5).strip())
out=[]
for d in sorted(glob.glob('plan/ws[0-9]*')):
    n=d[7:]
    ws=d+'/ws.md'
    if not os.path.exists(ws): out.append(f"NOWSMD {d}"); continue
    i,l=status_line(ws)
    wsst=first_kw(l) if l else None
    if not l: out.append(f"NOSTATUS {ws}")
    if n not in reg: out.append(f"NOREG WS{n} ({wsst})")
    else:
        if len(reg[n])>1: out.append(f"DUPREG WS{n} {reg[n]}")
        rs=first_kw(reg[n][0])
        if rs!=wsst: out.append(f"REGDIFF WS{n} ws.md={wsst} master={rs} | ws:{(l or '')[:100]} | reg:{reg[n][0][:100]}")
    phases=sorted(glob.glob(d+'/phase*/phase.md'))
    pst={}
    for p in phases:
        pi,pl=status_line(p)
        ps=first_kw(pl) if pl else None
        pst[p]=ps
        if not pl: out.append(f"PNOSTATUS {p}")
    retained='<!-- retained-after-completion -->' in open(ws,encoding='utf-8',errors='replace').read()
    if wsst=='completed' and phases and not retained: out.append(f"COMPLETED_WITH_PHASES {d} n={len(phases)} {[os.path.basename(os.path.dirname(p))+':'+str(pst[p]) for p in phases][:6]}")
    if wsst=='completed' and os.path.isdir(d+'/tests') and not retained: out.append(f"COMPLETED_WITH_TESTS {d} {len(os.listdir(d+'/tests'))}")
    exec_=[p for p in phases if pst[p] in ('in-progress','cleared','uncleared')]
    if wsst in ('planning','planned') and exec_: out.append(f"WS_NOT_STARTED_BUT_EXEC {d} {wsst} {[os.path.basename(os.path.dirname(p))+':'+pst[p] for p in exec_]}")
    # table in ws.md
    txt=open(ws,encoding='utf-8',errors='replace').read().split('\n')
    for j,row in enumerate(txt,1):
        m=re.match(r'^\|\s*\[?(?:ws\d+-)?p(\d{3})\]?(?:\(([^)]*)\))?\s*\|(.*)$',row)
        if not m: continue
        pn=m.group(1); pp=f"{d}/phase{pn}/phase.md"
        if pp in pst and pst[pp]:
            cells=[c.strip() for c in m.group(3).split('|')]
            ts=None
            for c in cells:
                k=first_kw(c) if re.match(r'^\**(planning|planned|in-progress|cleared|uncleared|canceled|cancelled|completed|incomplete)',c.lower()) else None
                if k: ts=k;break
            if ts and ts!=pst[pp] and not (ts=='canceled' or pst[pp]=='canceled'):
                out.append(f"TABLEDIFF {ws}:{j} p{pn} table={ts} phase.md={pst[pp]}")
for d in reg:
    if not os.path.isdir('plan/ws'+d): out.append(f"REG_NO_DIR WS{d}")
print('\n'.join(out))
