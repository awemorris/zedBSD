#!/usr/bin/env python3
"""Finite mathematical and session-protocol fixtures; no GPU/VM is required.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""
import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import sys
from types import SimpleNamespace
import unittest
from unittest import mock

import vkdemo_oracle as oracle

_spec = importlib.util.spec_from_file_location('vkdemo_qemu', Path(__file__).with_name('vkdemo-qemu.py'))
harness = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(harness)

_spec = importlib.util.spec_from_file_location('venus_remote', Path(__file__).with_name('run-venus-remote.py'))
wrapper = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(wrapper)


def sample(index, pixels, at):
    return {'run': 'fixture', 'mode': 'fixed' if index < 3 else 'live',
            'sample': index % 3 + 1, 'frame': index + 1, 'time_ms': at,
            'rgb_sha256': hashlib.sha256(pixels).hexdigest(), 'width': 320, 'height': 240}


def marker(value):
    return ('VKDEMO PRESENT run={run} mode={mode} sample={sample} frame={frame} '
            'time_ms={time_ms} rgb_sha256={rgb_sha256} width={width} height={height}').format(**value)


def ppm(path, pixels):
    path.write_bytes(b'P6\n320 240\n255\n' + pixels)


class OracleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.images = {at: oracle.reference(at) for at in (0, 500, 1000, 1570, 2500)}
        cls.samples = [sample(i, cls.images[at], at)
                       for i, at in enumerate((0, 1000, 2500, 0, 500, 1000))]

    def test_fixed_times_match_with_small_bounded_exclusions(self):
        for at in oracle.FIXED_TIMES:
            with self.subTest(time_ms=at):
                result = oracle.verify_pixels(self.images[at], at)
                self.assertTrue(result['passed'])
                self.assertEqual(result['mismatch_pixels'], 0)
                self.assertLess(result['excluded_boundary_pixels'], 1200)
                self.assertGreater(result['checked_foreground_colors'], 3000)

    def test_live_rotation_may_have_one_visible_face(self):
        # At 1570 ms the analytic camera sees just the +X face of the cuboid.
        result = oracle.verify_pixels(self.images[1570], 1570)
        self.assertEqual(set(result['visible_faces']), {'(0, 1)'})
        self.assertEqual(result['mismatch_pixels'], 0)
        self.assertTrue(result['passed'])

        # The correct silhouette alone must not admit a flat or stale texture.
        flat = bytearray(self.images[1570])
        for index in range(0, len(flat), 3):
            if tuple(flat[index:index + 3]) != oracle.CLEAR:
                flat[index:index + 3] = bytes((224, 56, 32))
        rejected = oracle.verify_pixels(flat, 1570)
        self.assertFalse(rejected['passed'])
        self.assertGreater(rejected['mismatch_pixels'], 2500)
        stale = oracle.verify_pixels(self.images[1000], 1570)
        self.assertFalse(stale['passed'])
        self.assertGreater(stale['mismatch_pixels'], 2500)

    def test_camera_ray_and_uv_have_independent_known_values(self):
        # The center ray hits the -Z face after undoing the two box rotations.
        face, u, v = oracle.Scene(0).hit(160, 120)
        self.assertEqual(face, (2, -1))
        self.assertAlmostEqual(u, 0.389360130292, places=10)
        self.assertAlmostEqual(v, 0.616001093604, places=10)
        self.assertEqual(oracle.texture(0, 0), (32, 32, 32))
        self.assertEqual(oracle.texture(1, 1), (32, 221, 221))
        self.assertEqual(oracle.texture(8 / 64, 0), (224, 56, 32))

    def test_clear_and_flat_faces_are_rejected(self):
        clear = bytes(oracle.CLEAR) * (320 * 240)
        self.assertFalse(oracle.verify_pixels(clear, 1000)['passed'])
        flat = bytearray(self.images[1000])
        for index in range(0, len(flat), 3):
            if tuple(flat[index:index + 3]) != oracle.CLEAR:
                flat[index:index + 3] = bytes((224, 56, 32))
        self.assertFalse(oracle.verify_pixels(flat, 1000)['passed'])

    def test_wrong_uv_and_unrotated_old_frame_are_rejected(self):
        scene = oracle.Scene(1000)
        wrong = bytearray()
        for y in range(240):
            for x in range(320):
                hit = scene.hit(x + 0.5, y + 0.5)
                wrong.extend(oracle.CLEAR if hit is None else oracle.texture(1 - hit[1], hit[2]))
        self.assertFalse(oracle.verify_pixels(wrong, 1000)['passed'])
        result = oracle.verify_pixels(self.images[0], 1000)
        self.assertFalse(result['passed'])
        self.assertGreater(result['mismatch_pixels'], 5000)

    def test_one_interior_rgb_error_is_not_tolerated(self):
        pixels = bytearray(self.images[0])
        pixels[0] ^= 1
        result = oracle.verify_pixels(pixels, 0)
        self.assertFalse(result['passed'])
        self.assertEqual(result['mismatch_pixels'], 1)

    def test_geometry_payload_and_time_bounds(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'frame.ppm'
            ppm(path, self.images[0])
            self.assertEqual(oracle.read_ppm(path), self.images[0])
            path.write_bytes(b'P6\n319 240\n255\n' + self.images[0])
            with self.assertRaises(ValueError):
                oracle.read_ppm(path)
            path.write_bytes(b'P6\n320 240\n255\n' + self.images[0][:-1])
            with self.assertRaises(ValueError):
                oracle.read_ppm(path)
        for at in (-1, 3600001, 1.5):
            with self.assertRaises(ValueError):
                oracle.Scene(at)

    def test_sequence_rejects_missing_stale_reordered_or_stopped_frames(self):
        self.assertEqual(oracle.verify_sequence(self.samples)['frames'], 6)
        invalid = []
        invalid.append(self.samples[:-1])
        changed = copy.deepcopy(self.samples)
        changed[4]['time_ms'] = changed[3]['time_ms']
        invalid.append(changed)
        changed = copy.deepcopy(self.samples)
        changed[5]['rgb_sha256'] = changed[4]['rgb_sha256']
        invalid.append(changed)
        changed = copy.deepcopy(self.samples)
        changed[1], changed[2] = changed[2], changed[1]
        invalid.append(changed)
        changed = copy.deepcopy(self.samples)
        changed[2]['run'] = 'old-run'
        invalid.append(changed)
        for values in invalid:
            with self.assertRaises(ValueError):
                oracle.verify_sequence(values)

    def test_marker_only_accepts_exact_identity_and_schedule(self):
        text = marker(self.samples[0])
        self.assertEqual(harness.parse_marker(text, 'fixture', 1), self.samples[0])
        self.assertIsNone(harness.parse_marker(text, 'old-run', 1))
        self.assertIsNone(harness.parse_marker(text, 'fixture', 2))
        bad = dict(self.samples[0], time_ms=123)
        with self.assertRaises(ValueError):
            harness.parse_marker(marker(bad), 'fixture', 1)
        conflict = dict(self.samples[0], rgb_sha256='0' * 64)
        with self.assertRaises(ValueError):
            harness.parse_marker(text + '\n' + marker(conflict), 'fixture', 1)

    def test_same_process_finite_ack_session_and_done(self):
        # This peer proves sequencing/hash/oracle/ack/DONE plumbing only, not GPU rendering.
        stage = [0]
        calls = []
        def send(text):
            calls.append(text)
            stage[0] += 1
        def console(*unused):
            if stage[0] <= 6:
                return marker(self.samples[stage[0] - 1])
            if stage[0] == 7:
                return 'VKDEMO DONE run=fixture frames=6\nroot@zedbsd$ '
            first = dict(self.samples[3], run='fixture-ordinary', frame=1, sample=1)
            last = dict(self.samples[4], run='fixture-ordinary', frame=2, sample=2)
            return ('VKDEMO START run=fixture-ordinary\n' + marker(first) + '\n' + marker(last) +
                    '\nVKDEMO DONE run=fixture-ordinary frames=2\nroot@zedbsd$ ')
        def capture(path, output, timeout):
            ppm(output, self.images[self.samples[stage[0] - 1]['time_ms']])
            return {'fixture_peer': True}
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'case' / 'evidence'
            output.mkdir(parents=True)
            debug = output / 'guest.log'
            debug.write_bytes(b'')
            args = SimpleNamespace(token='fixture', timeout=60)
            report = {}
            with mock.patch.object(harness.common, 'console_text', side_effect=console), \
                 mock.patch.object(harness, 'capture_rfb', side_effect=capture), \
                 mock.patch.object(harness.time, 'sleep'):
                harness.exercise(args, SimpleNamespace(text=send), output, debug,
                                 output / 'vnc.sock', SimpleNamespace(poll=lambda: None), report)
            self.assertEqual(report['status'], 'pass')
            self.assertEqual(calls[1:7], ['\n'] * 6)
            self.assertEqual(calls[7], '/bin/vkdemo --duration=2 --token=fixture-ordinary\n')
            self.assertTrue(report['ordinary']['completed'])
            self.assertEqual(len(report['samples']), 6)
            self.assertTrue(report['guest_completed'])
            self.assertEqual(json.loads((output / 'result.json').read_text())['status'], 'pass')
            report['transport_harness_sha256'] = 'a' * 64
            local = {'artifacts': {'transport_harness': {'sha256': 'a' * 64},
                                   'oracle': {'sha256': report['oracle_sha256']}},
                     'fetched_evidence': {path.name: wrapper.digest(path)
                                          for path in output.iterdir() if path.is_file()}}
            local_args = SimpleNamespace(token='fixture', output_root=Path(directory), attempt='case')
            wrapper.verify_vkdemo_result(local_args, local, report)
            changed = bytearray((output / 'frame-1.ppm').read_bytes())
            changed[-1] ^= 1
            (output / 'frame-1.ppm').write_bytes(changed)
            with self.assertRaises(RuntimeError):
                wrapper.verify_vkdemo_result(local_args, local, report)


class WrapperTests(unittest.TestCase):
    def test_venus_cli_keeps_defaults_and_vkdemo_uses_isolated_profile(self):
        for profile in ('venus', 'vkdemo'):
            argv = ['wrapper', '--attempt', 'fixture']
            if profile == 'venus':
                argv += ['--left', '255,0,0', '--right', '0,255,0']
            with mock.patch.object(sys, 'argv', argv), mock.patch.object(wrapper, 'run', return_value=23) as run:
                self.assertEqual(wrapper.main(profile), 23)
            args = run.call_args.args[0]
            self.assertEqual(args.phase, profile)
            self.assertEqual(args.build_directory, wrapper.REPO / f'build/{profile}-amd64')
            self.assertEqual(args.config, wrapper.REPO / f'plan/ws014/tests/config-{profile}-amd64.mk')
            self.assertEqual(args.frame, 1 if profile == 'venus' else 0)
            self.assertEqual(args.timeout, 120 if profile == 'venus' else 180)
            self.assertFalse(args.skip_build)
        self.assertNotIn('frame-1.ppm', wrapper.PROFILES['venus']['evidence'])
        self.assertNotIn('frame.ppm', wrapper.PROFILES['vkdemo']['evidence'])
        self.assertFalse(any(name.endswith(('.img', '.sock', '.fd'))
                             for name in wrapper.PROFILES['vkdemo']['evidence']))

    def test_invalid_cli_is_rejected_before_build_or_transfer(self):
        cases = [('venus', ['--host', 'host;unexpected']),
                 ('venus', ['--remote-root', '/home/awe/../unexpected']),
                 ('venus', ['--timeout', '-1']),
                 ('venus', ['--left', '256,0,0']),
                 ('vkdemo', ['--frame', '1']), ('vkdemo', ['--boot-only']),
                 ('vkdemo', ['--left', '1,2,3'])]
        for profile, extra in cases:
            with self.subTest(profile=profile, extra=extra):
                argv = ['wrapper', '--attempt', 'fixture', *extra]
                with mock.patch.object(sys, 'argv', argv), mock.patch.object(wrapper, 'run') as run, \
                     mock.patch.object(sys, 'stderr', io.StringIO()):
                    with self.assertRaises(SystemExit) as raised:
                        wrapper.main(profile)
                self.assertEqual(raised.exception.code, 2)
                run.assert_not_called()

    def test_venus_result_identity_and_evidence_gate_is_preserved(self):
        sha = 'a' * 64
        args = SimpleNamespace(profile='venus', phase='venus', frame=1, boot_only=False,
                               render_server='/server', left=[255, 0, 0], right=[0, 255, 0])
        local = {'remote_exit_code': 0, 'console': {'physical_address': 1, 'bytes': 32768},
                 'disposable_image': {'sha256': sha},
                 'artifacts': {'harness': {'sha256': sha}, 'rfb_client': {'sha256': sha}},
                 'fetched_evidence': {name: sha for name in wrapper.EVIDENCE_FILES}}
        remote = {'status': 'pass', 'qemu_exit_code': 0, 'phase': 'venus', 'frame': 1,
                  'console_address': 1, 'console_size': 32768, 'image_sha256': sha,
                  'source_image_sha256': sha, 'environment': {'RENDER_SERVER_EXEC_PATH': '/server'},
                  'render_server_sha256': sha, 'harness_sha256': sha, 'rfb_client_sha256': sha,
                  'boot_surface_available': False, 'guest_log_sha256': sha, 'renderer_log_sha256': sha,
                  'expected': [args.left, args.right], 'frame_sha256': sha}
        self.assertEqual(wrapper.verify_remote_result(args, local, remote), 'pass')
        for field, value in (('frame', 2), ('qemu_exit_code', 1), ('harness_sha256', 'b' * 64)):
            with self.assertRaises(RuntimeError):
                wrapper.verify_remote_result(args, local, dict(remote, **{field: value}))


if __name__ == '__main__':
    unittest.main(verbosity=2)
