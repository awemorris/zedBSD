#!/usr/bin/env python3
"""Conservative Awesome Plan body journal. Semantic reconciliation stays explicit."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[2]
SYNC = ROOT / 'plan/.sync'


def digest(text):
    return hashlib.sha256(text.encode()).hexdigest()


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.new')
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')
    os.replace(temporary, path)


def api(endpoint, method='GET', payload=None):
    args = ['gh', 'api', '--hostname', 'github.com', '--method', method, endpoint]
    if payload is not None:
        args += ['--input', '-']
    result = subprocess.run(args, input=json.dumps(payload) if payload is not None else None,
                            text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(result.stderr.strip())
    return json.loads(result.stdout)


def issue(record):
    return api('repos/awemorris/zedBSD/issues/' + str(record['number']))


def accept_base(record_id, record, remote):
    write(SYNC / 'base' / (record_id + '.json'), remote)
    record['remote_hash'] = digest(remote['body'] or '')
    record['remote_updated_at'] = remote['updated_at']
    record['local_hash'] = digest((ROOT / record['path']).read_text())
    record['freshness'] = 'reconciled'


def remote_changed(record_id, record, remote):
    base = json.loads((SYNC / 'base' / (record_id + '.json')).read_text())
    return (digest(remote['body'] or '') != record['remote_hash'] or
            any(remote.get(k) != base.get(k) for k in ['state', 'state_reason']))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['status', 'fetch', 'prepare', 'publish', 'reconcile'])
    parser.add_argument('id', nargs='?')
    parser.add_argument('--file', help='Reviewed remote Markdown body for prepare')
    parser.add_argument('--reason', help='Semantic reconciliation evidence')
    args = parser.parse_args()
    state_path = SYNC / 'state.json'
    if not state_path.exists():
        raise SystemExit('Missing cache: discover identity markers; see plan/tools/README.md. Do not create Issues.')
    state = json.loads(state_path.read_text())
    if args.command == 'status':
        operations = [json.loads(p.read_text()) for p in (SYNC / 'outbox').glob('*.json')]
        print(json.dumps({'records': len(state['records']),
                          'operations': [{'id': o['id'], 'record': o.get('record'), 'status': o['status']} for o in operations if o['status'] != 'confirmed'],
                          'conflicts': [p.name for p in (SYNC / 'conflicts').glob('*.json')],
                          'local_changes': [k for k,r in state['records'].items() if digest((ROOT/r['path']).read_text()) != r['local_hash']]}, ensure_ascii=False, indent=2))
        return
    if args.command == 'publish':
        op_path = SYNC / 'outbox' / (args.id + '.json')
        op = json.loads(op_path.read_text())
        if op['status'] == 'confirmed':
            print('Already confirmed'); return
        record = state['records'][op['record']]
        remote = issue(record)
        if digest((ROOT/record['path']).read_text()) != op['local_hash']:
            raise SystemExit('Local file changed after preparation; preserve/reconcile, do not replay.')
        base = json.loads((SYNC/'base'/(op['record']+'.json')).read_text())
        if any(remote.get(k) != base.get(k) for k in ['state', 'state_reason']):
            raise SystemExit('Issue lifecycle changed; fetch decisions and reconcile before publishing.')
        if remote['body'] != op['payload']:
            if digest(remote['body'] or '') != op['base_hash'] or remote_changed(op['record'], record, remote):
                op['status'] = 'conflicted'; write(op_path, op)
                write(SYNC/'conflicts'/(op['id']+'.json'), {'operation':op,'remote':remote})
                raise SystemExit('Remote changed: conflict preserved, no write.')
            # Non-atomic version check. One cooperating writer is required.
            api('repos/awemorris/zedBSD/issues/'+str(record['number']), 'PATCH', {'body':op['payload']})
        result = issue(record)
        if result['body'] != op['payload']:
            write(SYNC/'conflicts'/(op['id']+'.json'), {'operation':op,'remote':result})
            raise SystemExit('Read-back differs; do not claim synchronized.')
        accept_base(op['record'], record, result)
        write(state_path, state)
        op['status'] = 'confirmed'; op['verified_at'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        write(op_path, op); print('Published and verified', op['record']); return
    if args.id not in state['records']:
        raise SystemExit('Unknown logical ID')
    record = state['records'][args.id]
    remote = issue(record)
    if args.command == 'fetch':
        write(SYNC/'remote'/(args.id+'.json'), remote)
        comments = []
        page = 1
        while True:
            batch = api('repos/awemorris/zedBSD/issues/'+str(record['number'])+'/comments?per_page=100&page='+str(page))
            comments += batch
            if len(batch) < 100: break
            page += 1
        write(SYNC/'remote'/(args.id+'-comments.json'), comments)
        local_changed = digest((ROOT/record['path']).read_text()) != record['local_hash']
        changed_remote = remote_changed(args.id, record, remote)
        record['freshness'] = 'needs-reconciliation' if local_changed or changed_remote else 'reconciled'
        record['fetched_at'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        if local_changed or changed_remote:
            write(SYNC/'conflicts'/(args.id+'-versions.json'), {'base': json.loads((SYNC/'base'/(args.id+'.json')).read_text()), 'local': (ROOT/record['path']).read_text(), 'remote':remote})
        write(state_path,state); print(record['freshness'], 'comments',len(comments)); return
    if args.command == 'reconcile':
        if not args.reason: raise SystemExit('--reason is required after semantic review')
        snapshot = json.loads((SYNC/'remote'/(args.id+'.json')).read_text())
        if remote['body'] != snapshot['body'] or remote['updated_at'] != snapshot['updated_at']:
            raise SystemExit('Remote changed since fetch; fetch/review again')
        if any(o.get('record')==args.id and o['status']!='confirmed' for o in (json.loads(p.read_text()) for p in (SYNC/'outbox').glob('*.json'))):
            raise SystemExit('Pending operations must be resolved before adopting a new base')
        write(SYNC/'reconciliations'/(uuid.uuid4().hex+'.json'), {'record':args.id,'reason':args.reason,'remote':remote,'local':(ROOT/record['path']).read_text()})
        accept_base(args.id,record,remote);write(state_path,state)
        (SYNC/'conflicts'/(args.id+'-versions.json')).unlink(missing_ok=True)
        print('Reviewed base saved; no remote mutation');return
    if not args.file: raise SystemExit('--file is required')
    if remote_changed(args.id, record, remote):
        raise SystemExit('Remote changed; fetch and reconcile first')
    if digest((ROOT/record['path']).read_text()) != record['local_hash']:
        raise SystemExit('Unjournaled local edit; fetch/preserve/reconcile before preparing')
    payload = Path(args.file).read_text()
    marker = '<!-- awesome-plan project=zedbsd record='+args.id+' -->'
    if marker not in payload: raise SystemExit('Identity marker missing')
    op_id = uuid.uuid4().hex
    op = {'id':op_id,'record':args.id,'kind':'issue-body','base_hash':record['remote_hash'],
          'payload':payload,'payload_hash':digest(payload),'local_hash':record['local_hash'],
          'prerequisites':[],'status':'prepared'}
    write(SYNC/'outbox'/(op_id+'.json'),op)
    # Save payload separately from the locally rendered plan; never overwrite evidence.
    write(SYNC/'drafts'/(op_id+'.json'),{'remote_body':payload})
    op['status']='pending';write(SYNC/'outbox'/(op_id+'.json'),op)
    print(op_id)


if __name__ == '__main__':
    main()
