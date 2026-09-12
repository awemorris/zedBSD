#!/usr/bin/env python3
"""Finite generation/rollback fixtures with substitute compiler and validator.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

These tests verify asset handling; they do not compile GLSL or validate SPIR-V.
The checked-in provenance records the separate real compiler/validator run.
"""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
SHADERS = ROOT / 'userland/base/vkdemo/shaders'
_spec = importlib.util.spec_from_file_location('vkdemo_regenerate', SHADERS / 'regenerate.py')
regenerate = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(regenerate)

FAKE_TOOL = '''#!/usr/bin/env python3
import json,pathlib,sys
root=pathlib.Path(__file__).parent
metadata=json.loads((root/'versions.json').read_text())
compiler=pathlib.Path(__file__).name=='glslc'
if sys.argv[1:] == ['--version']:
 print(metadata['compiler' if compiler else 'validator'])
 sys.exit(0)
if compiler:
 source=pathlib.Path(sys.argv[-3]); extension=source.suffix[1:]
 (root/('seen-'+extension)).write_bytes(source.read_bytes())
 (root/('path-'+extension)).write_text(str(source))
 if extension=='frag' and (root/'fail-compile').exists(): sys.exit(17)
 if extension=='vert' and (root/'edit-source').exists():
  (root/'shaders/cuboid.vert').write_bytes(b'edited during compile')
 pathlib.Path(sys.argv[-1]).write_bytes((root/('original-'+extension+'.spv')).read_bytes())
else:
 if sys.argv[-1].endswith('frag.spv') and (root/'fail-validate').exists(): sys.exit(18)
'''


class ShaderGenerationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='vkdemo-shader-fixture-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.directory = self.root / 'shaders'
        self.directory.mkdir()
        for name in ('cuboid.vert', 'cuboid.frag', 'cuboid.vert.spv', 'cuboid.frag.spv', 'provenance.json'):
            shutil.copyfile(SHADERS / name, self.directory / name)
        shutil.copyfile(SHADERS.parent / 'shaders.h', self.root / 'shaders.h')
        (self.root / 'versions.json').write_bytes((SHADERS / 'provenance.json').read_bytes())
        for extension in ('vert', 'frag'):
            shutil.copyfile(SHADERS / f'cuboid.{extension}.spv', self.root / f'original-{extension}.spv')
        for name in ('glslc', 'spirv-val'):
            tool = self.root / name
            tool.write_text(FAKE_TOOL)
            tool.chmod(0o700)
        self.targets = [self.directory / 'cuboid.vert.spv', self.directory / 'cuboid.frag.spv',
                        self.root / 'shaders.h', self.directory / 'provenance.json']
        self.before = {path: path.read_bytes() for path in self.targets}

    def generate(self):
        regenerate.generate(self.directory, str(self.root / 'glslc'), str(self.root / 'spirv-val'))

    def assert_unchanged(self):
        self.assertEqual({path: path.read_bytes() for path in self.targets}, self.before)

    def test_complete_pair_reproduces_checked_in_header_and_metadata(self):
        actual_run = subprocess.run
        with mock.patch.object(regenerate.subprocess, 'run', side_effect=actual_run) as calls:
            self.generate()
        self.assertEqual(len(calls.call_args_list), 6)
        for call in calls.call_args_list:
            self.assertEqual(call.kwargs['timeout'], 30)
            self.assertEqual(call.kwargs['stdin'], subprocess.DEVNULL)
        self.assert_unchanged()
        for extension in ('vert', 'frag'):
            self.assertEqual((self.root / f'seen-{extension}').read_bytes(),
                             (SHADERS / f'cuboid.{extension}').read_bytes())
            self.assertNotEqual((self.root / f'path-{extension}').read_text(),
                                str(self.directory / f'cuboid.{extension}'))

    def test_fragment_compile_failure_publishes_nothing(self):
        (self.root / 'fail-compile').touch()
        with mock.patch.object(regenerate, 'publish') as publish:
            with self.assertRaises(subprocess.CalledProcessError):
                self.generate()
        publish.assert_not_called()
        self.assert_unchanged()

    def test_fragment_validation_failure_publishes_nothing(self):
        (self.root / 'fail-validate').touch()
        with mock.patch.object(regenerate, 'publish') as publish:
            with self.assertRaises(subprocess.CalledProcessError):
                self.generate()
        publish.assert_not_called()
        self.assert_unchanged()

    def test_source_edit_during_compile_refuses_publication(self):
        (self.root / 'edit-source').touch()
        with self.assertRaisesRegex(RuntimeError, 'GLSL changed'):
            self.generate()
        self.assert_unchanged()
        self.assertEqual((self.root / 'seen-vert').read_bytes(), (SHADERS / 'cuboid.vert').read_bytes())

    def test_timeout_keeps_previous_set_and_does_not_run_more_tools(self):
        actual_run = subprocess.run
        def run(arguments, **options):
            self.assertEqual(options['timeout'], 30)
            if '-fshader-stage=fragment' in arguments:
                raise subprocess.TimeoutExpired(arguments, options['timeout'])
            return actual_run(arguments, **options)
        with mock.patch.object(regenerate.subprocess, 'run', side_effect=run) as calls:
            with self.assertRaises(subprocess.TimeoutExpired):
                self.generate()
        self.assertEqual(len(calls.call_args_list), 5)
        self.assert_unchanged()

    def test_failed_publication_restores_the_complete_previous_pair(self):
        actual_replace = regenerate.os.replace
        attempts = [0]
        def replace(source, destination):
            attempts[0] += 1
            if attempts[0] == 2:
                raise OSError('fixture publication failure')
            return actual_replace(source, destination)
        changed = {path: b'new fixture bytes' for path in self.targets}
        with mock.patch.object(regenerate.os, 'replace', side_effect=replace):
            with self.assertRaisesRegex(OSError, 'fixture publication failure'):
                regenerate.publish(changed)
        self.assert_unchanged()
        self.assertEqual(list(self.directory.glob('.*')), [])

    def test_failed_publication_removes_new_outputs_that_had_no_prior_file(self):
        path = self.root / 'new.spv'
        second = self.root / 'second.spv'
        actual_replace = regenerate.os.replace
        def replace(source, destination):
            if destination == second:
                raise OSError('fixture new-file failure')
            return actual_replace(source, destination)
        with mock.patch.object(regenerate.os, 'replace', side_effect=replace):
            with self.assertRaises(OSError):
                regenerate.publish({path: b'new one', second: b'new two'})
        self.assertFalse(path.exists())
        self.assertFalse(second.exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)
