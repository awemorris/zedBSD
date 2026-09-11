import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('plan_sync', Path(__file__).with_name('sync.py'))
s = importlib.util.module_from_spec(spec)
spec.loader.exec_module(s)


class SyncTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        s.ROOT = self.root
        s.SYNC = self.root / 'plan/.sync'
        for d in ['base', 'outbox', 'conflicts']:
            (s.SYNC / d).mkdir(parents=True)
        self.local = self.root / 'plan/master.md'
        self.local.write_text('local rendering')
        self.remote = {'body': '<!-- awesome-plan project=zedbsd record=master -->\nold', 'updated_at': 'before'}
        self.writes = 0
        r = {'path': 'plan/master.md', 'number': 1}
        s.accept_base('master', r, self.remote)
        s.write(s.SYNC / 'state.json', {'records': {'master': r}})
        self.payload = self.root / 'payload.md'
        self.payload.write_text('<!-- awesome-plan project=zedbsd record=master -->\nnew')

    def tearDown(self):
        self.temp.cleanup()

    def api(self, endpoint, method='GET', payload=None):
        if '/comments?' in endpoint:
            return []
        if method == 'PATCH':
            self.writes += 1
            self.remote = {'body': payload['body'], 'updated_at': 'after'}
        return dict(self.remote)

    def run_command(self, *args):
        out = io.StringIO()
        with patch.object(sys, 'argv', ['sync.py', *args]), patch.object(s, 'api', self.api), contextlib.redirect_stdout(out):
            s.main()
        return out.getvalue().strip()

    def prepare(self):
        return self.run_command('prepare', 'master', '--file', str(self.payload))

    def test_publish_and_retry(self):
        op = self.prepare()
        self.run_command('publish', op)
        self.run_command('publish', op)
        self.assertEqual(self.writes, 1)
        self.assertEqual(self.remote['body'], self.payload.read_text())

    def test_remote_conflict_does_not_overwrite(self):
        op = self.prepare()
        self.remote['body'] = 'human edit'
        with self.assertRaises(SystemExit):
            self.run_command('publish', op)
        self.assertEqual(self.writes, 0)
        self.assertTrue((s.SYNC / 'conflicts' / (op + '.json')).exists())

    def test_fetch_preserves_unjournaled_local_edit(self):
        self.local.write_text('local unjournaled change')
        self.run_command('fetch', 'master')
        self.assertEqual(self.local.read_text(), 'local unjournaled change')
        self.assertTrue((s.SYNC / 'conflicts/master-versions.json').exists())

    def test_timeout_after_remote_success_is_idempotent(self):
        op = self.prepare()
        self.remote['body'] = self.payload.read_text()
        self.run_command('publish', op)
        self.assertEqual(self.writes, 0)

    def test_human_lifecycle_change_blocks_publish(self):
        op = self.prepare()
        self.remote['state'] = 'closed'
        self.remote['state_reason'] = 'not_planned'
        with self.assertRaises(SystemExit):
            self.run_command('publish', op)
        self.assertEqual(self.writes, 0)

    def test_local_change_after_prepare_blocks_publish(self):
        op = self.prepare()
        self.local.write_text('new change')
        with self.assertRaises(SystemExit):
            self.run_command('publish', op)
        self.assertEqual(self.writes, 0)


if __name__ == '__main__':
    unittest.main()
