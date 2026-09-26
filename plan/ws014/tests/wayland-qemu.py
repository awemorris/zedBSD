#!/usr/bin/env python3
"""Verify real shared-GPU Wayland images in a finite QEMU/Venus session.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import select
import signal
import sys
import time

from venus_rfb import capture as capture_rfb
import wayland_oracle as oracle

_spec = importlib.util.spec_from_file_location('venus_qemu', Path(__file__).with_name('venus-qemu.py'))
common = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(common)


def save(output, report):
    (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')


class RendererPause:
    """Keep verified pidfds for descendants of this still-identical QEMU."""

    def __init__(self, qemu_pid, executable, report):
        self.qemu_pid = qemu_pid
        self.executable = executable.resolve(strict=True)
        self.report = report
        self.handles = []
        self.root_identity = self.identity(qemu_pid)

    @staticmethod
    def identity(pid):
        root = Path(f'/proc/{pid}')
        fields = (root / 'stat').read_text().rsplit(')', 1)[1].split()
        executable = (root / 'exe').resolve(strict=True)
        info = (root / 'exe').stat()
        return (int(fields[19]), int(fields[1]), str(executable), info.st_dev, info.st_ino)

    def owned(self, pid):
        seen = set()
        while pid != self.qemu_pid:
            if pid <= 1 or pid in seen or len(seen) >= 32:
                return False
            seen.add(pid)
            pid = self.identity(pid)[1]
        return self.identity(self.qemu_pid) == self.root_identity

    @staticmethod
    def alive(descriptor):
        poller = select.poll()
        poller.register(descriptor, select.POLLIN)
        return not poller.poll(0)

    def descendants(self):
        pending = [self.qemu_pid]
        descendants = set()
        while pending:
            parent = pending.pop()
            if not self.owned(parent):
                raise RuntimeError('disposable QEMU ancestry changed during discovery')
            for task in Path(f'/proc/{parent}/task').glob('*'):
                try:
                    children = (task / 'children').read_text().split()
                except FileNotFoundError:
                    continue
                for child in map(int, children):
                    if child not in descendants:
                        descendants.add(child)
                        pending.append(child)
            if len(descendants) > 32:
                raise RuntimeError('unexpectedly large disposable QEMU process tree')
        return sorted(descendants)

    def sample_cpu(self, samples):
        """Observe only this VM's QEMU and renderer processes; never signal them."""
        try:
            candidates = [self.qemu_pid] + self.descendants()
        except (FileNotFoundError, ProcessLookupError):
            return
        for pid in candidates:
            try:
                before = self.identity(pid)
                if pid != self.qemu_pid and before[2] != str(self.executable):
                    continue
                fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
                if (self.identity(pid) != before or not self.owned(pid) or
                        int(fields[19]) != before[0]):
                    continue
            except (FileNotFoundError, ProcessLookupError):
                continue
            key = f'{pid}:{before[0]}'
            current = {'pid': pid, 'starttime': before[0],
                       'kind': 'qemu' if pid == self.qemu_pid else 'renderer',
                       'executable': before[2], 'user_ticks': int(fields[11]),
                       'system_ticks': int(fields[12])}
            if key not in samples:
                samples[key] = dict(current, first_user_ticks=current['user_ticks'],
                                    first_system_ticks=current['system_ticks'], observations=0)
            samples[key].update(current)
            samples[key]['observations'] += 1

    def pause(self):
        selected = []
        identities = {}
        for pid in self.descendants():
            try:
                before = self.identity(pid)
            except FileNotFoundError:
                continue
            if before[2] != str(self.executable):
                continue
            descriptor = os.pidfd_open(pid)
            try:
                # Bind before rechecking /proc: a departed pidfd is readable even
                # when its number already names a new process with the same exe.
                if (not self.alive(descriptor) or self.identity(pid) != before or
                        not self.owned(pid) or not self.alive(descriptor)):
                    raise RuntimeError('renderer identity changed while acquiring its pidfd')
            except BaseException:
                os.close(descriptor)
                raise
            self.handles.append((pid, descriptor, before))
            identities[str(pid)] = {'starttime': before[0], 'parent': before[1],
                                    'executable_device': before[3], 'executable_inode': before[4]}
            selected.append(pid)
        if len(selected) < 3:
            raise RuntimeError('recovery test requires the server and two owned context workers')
        for pid, descriptor, identity in self.handles:
            if (not self.alive(descriptor) or self.identity(pid) != identity or
                    not self.owned(pid)):
                raise RuntimeError('renderer identity changed before suspension')
            signal.pidfd_send_signal(descriptor, signal.SIGSTOP)
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            states = []
            for pid, descriptor, identity in self.handles:
                if not self.alive(descriptor) or self.identity(pid) != identity:
                    raise RuntimeError('renderer exited or changed identity during suspension')
                states.append(Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()[0])
            if all(state in ('T', 't') for state in states):
                self.report['renderer_pause'] = {'pids': selected, 'qemu_pid': self.qemu_pid,
                                                 'executable': str(self.executable),
                                                 'identities': identities,
                                                 'stopped': True, 'resumed': False}
                return
            time.sleep(0.01)
        raise TimeoutError('owned renderer workers did not enter the stopped state')

    def resume(self):
        errors = []
        # A second catchable termination must not interrupt cleanup between two
        # owned workers. Restore the mask only after every pidfd has been tried.
        previous_mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM, signal.SIGINT})
        try:
            handles, self.handles = self.handles, []
            for pid, descriptor, identity in handles:
                try:
                    signal.pidfd_send_signal(descriptor, signal.SIGCONT)
                except ProcessLookupError:
                    pass
                except OSError as error:
                    errors.append(f'pid {pid}: SIGCONT: {error}')
                finally:
                    try:
                        os.close(descriptor)
                    except OSError as error:
                        errors.append(f'pid {pid}: close pidfd: {error}')
            if 'renderer_pause' in self.report:
                pause = self.report['renderer_pause']
                if errors:
                    pause.setdefault('resume_errors', []).extend(errors)
                pause['resumed'] = not pause.get('resume_errors')
        finally:
            signal.pthread_sigmask(signal.SIG_SETMASK, previous_mask)
        if errors:
            raise RuntimeError('renderer cleanup failed: ' + '; '.join(errors))


def exercise_fault(args, qmp, output, debug, vnc_path, process, report):
    pause = RendererPause(process.pid, args.render_server, report)
    try:
        exercise_fault_steps(args, qmp, output, debug, vnc_path, process, report, pause)
    finally:
        pause.resume()


def exercise_fault_steps(args, qmp, output, debug, vnc_path, process, report, pause):
    """Run one isolated regression in a fresh disposable VM, then retire the VM."""
    baseline = debug.stat().st_size
    command, expected = {
        'completion-delay': ('/bin/gpu-fence-test --completion-delay', 'GPUFENCE COMPLETION_DELAY PASS'),
        'context-timeout': ('/bin/gpu-fence-test --context-timeout', 'GPUFENCE CONTEXT_TIMEOUT PASS'),
        'submit-load': ('/bin/gpu-fence-test --submit-load',
                        'GPUFENCE SUBMIT_LOAD PASS processes=2 rounds=3 submits=576 verified_bytes=25165824 verified_submits=576'),
        'recovery': ('/bin/gpu-recovery-test --isolated',
                     'GPURECOVERY PASS timeout=1 peer_failed=1 retirement_gate=1 fresh_roundtrip=4096 decoder=1'),
        'producer-exit': ('/bin/gpu-fence-test --producer-exit',
                          'GPUFENCE PRODUCER_EXIT_REAL_RESULT PASS'),
        'producer-stop': ('/bin/gpu-fence-test --producer-stop',
                          'GPUFENCE PRODUCER_STOP_ERROR PASS'),
        'producer-exit-delayed': ('/bin/gpu-fence-test --producer-exit-delayed',
                                  'GPUFENCE PRODUCER_EXIT_DELAYED PASS'),
        'producer-exit-hang': ('/bin/gpu-fence-test --producer-exit-hang',
                               'GPUFENCE PRODUCER_EXIT_HANG PASS'),
    }[args.fault_test]
    report.update(token=args.token, fault_test=args.fault_test, commands=[command],
                  guest_completed=False)
    cpu_samples = {}
    if args.fault_test == 'submit-load':
        pause.sample_cpu(cpu_samples)
    qmp.text(command + '\n')
    deadline = time.monotonic() + args.timeout
    observed = ''
    stopped = False
    resumed = False
    gate_armed = False
    peer_started = False
    delayed = args.fault_test in ('completion-delay', 'context-timeout', 'producer-stop', 'producer-exit-delayed')
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError('QEMU exited during the isolated fault test')
        if args.fault_test == 'submit-load':
            pause.sample_cpu(cpu_samples)
        observed = common.guest_text(debug, baseline) + common.console_text(qmp, output, args)
        (output / 'wayland-observed.log').write_text(observed)
        if re.search(r'kernel panic|amd64 fault v=|GPURECOVERY FAIL|GPUFENCE FAIL|Segmentation fault', observed):
            raise RuntimeError('isolated GPU fault test failed; inspect captured evidence')
        if args.fault_test == 'recovery' and not stopped and 'GPURECOVERY READY renderer=running' in observed:
            pause.pause()
            stopped = True
            qmp.text('stopped\n')
        if args.fault_test == 'recovery' and stopped and not resumed and 'GPURECOVERY RESUME renderer=stopped' in observed:
            pause.resume()
            resumed = True
            qmp.text('running\n')
        if delayed and not gate_armed and 'GPUFENCE DELAY_READY' in observed:
            gate = output / 'completion.gate'
            if report.get('environment', {}).get('Q312_COMPLETION_GATE') != str(gate):
                raise RuntimeError('completion delay gate differs from the isolated renderer environment')
            with gate.open('x') as stream:
                stream.write('one native SUCCESS callback\n')
            gate_armed = True
            qmp.text('armed\n')
        if delayed and gate_armed and not peer_started and 'GPUFENCE DELAY_SUBMITTED' in observed:
            renderer_text = (output / 'qemu-renderer.log').read_text(errors='replace')
            acquired = re.search(r'Q312_COMPLETION_DELAY acquired pid=(\d+) context=(\d+) fence=(\d+) native=SUCCESS duration_ms=15000', renderer_text)
            if acquired is not None:
                pid = int(acquired[1])
                if not pause.owned(pid) or pause.identity(pid)[2] != str(args.render_server.resolve()):
                    raise RuntimeError('delay marker is not from this disposable QEMU renderer')
                report['delay_renderer'] = dict(pid=pid, context=int(acquired[2]), fence=int(acquired[3]))
                peer_started = True
                qmp.text('running\n')
        match = re.search(re.escape(expected) + r'[\r\n]+[\s\S]*root@[^\r\n]*\$ ', observed)
        if match and delayed:
            renderer_text = (output / 'qemu-renderer.log').read_text(errors='replace')
            if 'Q312_COMPLETION_DELAY released ' not in renderer_text:
                time.sleep(0.05)
                continue
        if match:
            report.update(status='pass', guest_completed=True, fault_marker=expected)
            if delayed:
                if not gate_armed or not peer_started:
                    raise RuntimeError('completion delay lacks the two explicit submission handshakes')
                renderer_text = (output / 'qemu-renderer.log').read_text(errors='replace')
                report['completion_delay'] = verify_completion_delay(observed, renderer_text, args.fault_test)
            if args.fault_test == 'submit-load':
                report['load_samples'] = verify_submit_load(observed)
                report['host_cpu_samples'] = {
                    'ticks_per_second': os.sysconf('SC_CLK_TCK'),
                    'processes': list(cpu_samples.values()),
                    'scope': 'QEMU delta covers the whole test; renderer counters are sampled lifetime lower bounds.',
                    'limitations': '50ms loop plus QMP work; exited workers may lose their last interval; short workers may be missed. Includes initialization/cleanup, not per-round GPU time.'}
                if not any(item['kind'] == 'renderer' for item in cpu_samples.values()):
                    raise RuntimeError('submit load lacks separately observed host renderer CPU counters')
            if args.fault_test == 'producer-stop':
                if 'GPUFENCE PRODUCER_STOPPED pending=1 fd_live=1' not in observed:
                    raise RuntimeError('producer stop acceptance lacks pending work and live stopped process')
                measured = re.search(r'GPUFENCE PRODUCER_STOP_WAIT result=(-?\d+) elapsed_ms=(\d+)', observed)
                if measured is None or int(measured[1]) != -4 or not 7000 <= int(measured[2]) <= 13000:
                    raise RuntimeError('producer stop acceptance lacks finite DEVICE_LOST watchdog result')
                report['watchdog_elapsed_ms'] = int(measured[2])
                report['producer_stopped'] = True
            if args.fault_test == 'producer-exit':
                measured = re.search(r'GPUFENCE PRODUCER_EXIT_WAIT result=(-?\d+) elapsed_ms=(\d+)', observed)
                if measured is None or int(measured[1]) not in (0, -4) or 'GPUFENCE PEER_CONTINUES' not in observed:
                    raise RuntimeError('producer exit lacks the real job result and consumer continuation')
                report['producer_exit'] = dict(result=int(measured[1]), elapsed_ms=int(measured[2]))
            if args.fault_test == 'producer-exit-hang':
                measured = re.search(r'GPUFENCE PRODUCER_EXIT_HANG_WAIT result=(-?\d+) elapsed_ms=(\d+)', observed)
                if measured is None or int(measured[1]) != -4 or not 7000 <= int(measured[2]) <= 13000:
                    raise RuntimeError('producer exit hang lacks the configured execution-deadline DEVICE_LOST')
                if 'GPUFENCE ISOLATION_PEER_OK' not in observed or 'quarantined' not in observed:
                    raise RuntimeError('producer exit hang lacks peer continuation after per-context isolation')
                report['watchdog_elapsed_ms'] = int(measured[2])
                report['isolation_peer'] = True
                report['reclaim'] = exercise_reclaim(args, qmp, output, debug, process)
            if args.fault_test == 'recovery':
                if not stopped or not resumed:
                    raise RuntimeError('recovery acceptance lacks the controlled renderer pause/resume')
                measured = re.search(r'GPURECOVERY TIMEOUT status=(\d+) elapsed_ms=(\d+)', observed)
                if measured is None or not 9000 <= int(measured[2]) <= 20000:
                    raise RuntimeError('recovery acceptance lacks the bounded watchdog measurement')
                report['watchdog_elapsed_ms'] = int(measured[2])
                report['watchdog_status'] = int(measured[1])
            save(output, report)
            return
        time.sleep(0.05)
    raise TimeoutError('isolated fault test completion and shell return')


def exercise_reclaim(args, qmp, output, debug, process):
    """Run the ordinary fence test after isolation; its fresh open must reclaim through checked reset."""
    earlier = (common.guest_text(debug, 0) + common.console_text(qmp, output, args)).count('GPUFENCE PASS')
    qmp.text('/bin/gpu-fence-test\n')
    deadline = time.monotonic() + 90
    observed = ''
    while time.monotonic() < deadline:
        if process.poll() is not None:
            return dict(status='fail', reason='QEMU exited during the reclaim run')
        observed = common.guest_text(debug, 0) + common.console_text(qmp, output, args)
        (output / 'wayland-reclaim.log').write_text(observed)
        if observed.count('GPUFENCE PASS') > earlier and re.search(r'GPUFENCE PASS[\r\n]+[\s\S]*root@[^\r\n]*\$ ', observed):
            return dict(status='pass', checked_reset='recovered after all prior sessions' in observed)
        if re.search(r'GPUFENCE FAIL|kernel panic|amd64 fault v=', observed):
            return dict(status='fail', reason='ordinary fence test failed after isolation',
                        checked_reset='recovered after all prior sessions' in observed)
        time.sleep(0.05)
    return dict(status='fail', reason='ordinary fence test did not finish after isolation')


def verify_completion_delay(observed, renderer_text, mode):
    """Distinguish real native success, delayed notification, and independent GPU progress."""
    acquired = re.findall(r'Q312_COMPLETION_DELAY acquired pid=(\d+) context=(\d+) fence=(\d+) native=SUCCESS duration_ms=15000', renderer_text)
    released = re.findall(r'Q312_COMPLETION_DELAY released pid=(\d+) context=(\d+) fence=(\d+) elapsed_ms=(\d+)', renderer_text)
    if (len(acquired) != 1 or len(released) != 1 or acquired[0] != released[0][:3] or
            not 15000 <= int(released[0][3]) <= 25000 or 'Q312_COMPLETION_DELAY failed' in renderer_text):
        raise RuntimeError('isolated renderer did not delay exactly one actual native-success notification')
    if mode == 'producer-stop':
        observations = set(re.findall(r'GPUFENCE PRODUCER_STOP_WAIT result=(-?\d+) elapsed_ms=(\d+)', observed))
        if len(observations) != 1 or 'GPUFENCE PRODUCER_STOPPED pending=1 fd_live=1' not in observed:
            raise RuntimeError('delayed producer-stop lacks one stopped live producer and terminal observation')
        result, elapsed = map(int, next(iter(observations)))
        if result != -4 or not 7000 <= elapsed <= 13000:
            raise RuntimeError('stopped producer did not reach the configured 8-second job error')
        return dict(pid=int(acquired[0][0]), context=int(acquired[0][1]), fence=int(acquired[0][2]),
                    native_result='SUCCESS', notification_delay_ms=int(released[0][3]),
                    producer_result=result, producer_elapsed_ms=elapsed, producer_stopped=True)
    if mode == 'producer-exit-delayed':
        observations = set(re.findall(r'GPUFENCE PRODUCER_EXIT_DELAYED_WAIT result=(-?\d+) elapsed_ms=(\d+)', observed))
        if len(observations) != 1 or 'GPUFENCE PEER_CONTINUES' not in observed:
            raise RuntimeError('delayed producer-exit lacks one consumer wait result and consumer continuation')
        result, elapsed = map(int, next(iter(observations)))
        if result != 0 or not 14000 <= elapsed <= 30000:
            raise RuntimeError('committed job of an exited producer did not signal success with the delayed completion')
        return dict(pid=int(acquired[0][0]), context=int(acquired[0][1]), fence=int(acquired[0][2]),
                    native_result='SUCCESS', notification_delay_ms=int(released[0][3]),
                    consumer_result=result, consumer_elapsed_ms=elapsed, producer_exited=True)
    results = set(re.findall(r'GPUFENCE DELAY_RESULT result=(-?\d+) expected=(-?\d+) elapsed_ms=(\d+)', observed))
    peers = set(re.findall(r'GPUFENCE DELAY_PEER_PASS completed=(\d+) elapsed_ms=(\d+) after_close=1 verified_bytes=(\d+)', observed))
    if len(results) != 1 or len(peers) != 1:
        raise RuntimeError('completion delay lacks one producer result and independent peer cleanup proof')
    result, expected, elapsed = map(int, next(iter(results)))
    completed, peer_elapsed, verified = map(int, next(iter(peers)))
    wanted = -4 if mode == 'context-timeout' else 0
    lower, upper = (7000, 13000) if wanted == -4 else (14000, 30000)
    if result != wanted or expected != wanted or not lower <= elapsed <= upper:
        raise RuntimeError('delayed job disagrees with the selected common supervision policy')
    if completed < 10 or peer_elapsed < 20000 or verified != 4194304:
        raise RuntimeError('independent GPU work did not continue beyond delayed retirement and producer close')
    return dict(pid=int(acquired[0][0]), context=int(acquired[0][1]), fence=int(acquired[0][2]),
                native_result='SUCCESS', notification_delay_ms=int(released[0][3]),
                producer_result=result, producer_elapsed_ms=elapsed,
                peer_completed=completed, peer_elapsed_ms=peer_elapsed, peer_after_close=True,
                peer_verified_bytes=verified)


def verify_submit_load(observed):
    """Require both independent devices, all measured rounds, and newly verified GPU bytes."""
    expression = (r'GPUFENCE LOAD role=(parent|child) round=([012]) mode=(null|explicit) '
                  r'submits=(\d+) bytes=(\d+) repeats=(\d+) elapsed_ms=(\d+) verified_bytes=(\d+) '
                  r'user_cpu_us=(\d+) system_cpu_us=(\d+) verified_submits=(\d+)')
    samples = {}
    for match in re.finditer(expression, observed):
        role, round_text, mode, submits, size, repeats, elapsed, verified, user_cpu, system_cpu, proofs = match.groups()
        round_index = int(round_text)
        sample = dict(role=role, round=round_index, mode=mode, submits=int(submits),
                      bytes=int(size), repeats=int(repeats), elapsed_ms=int(elapsed),
                      verified_bytes=int(verified), user_cpu_us=int(user_cpu), system_cpu_us=int(system_cpu),
                      verified_submits=int(proofs))
        if (mode != ('explicit' if round_index == 1 else 'null') or
                sample['submits'] != 96 or sample['bytes'] != 4194304 or
                sample['repeats'] != 32 or sample['verified_bytes'] != 4194304 or
                sample['verified_submits'] != 96 or sample['elapsed_ms'] <= 0):
            raise RuntimeError('submit load used a different workload or failed byte verification')
        key = role, round_index
        if key in samples and samples[key] != sample:
            raise RuntimeError('submit load has conflicting evidence for one measured round')
        samples[key] = sample
    if set(samples) != {(role, index) for role in ('parent', 'child') for index in range(3)}:
        raise RuntimeError('submit load lacks all three completed rounds from both processes')
    return [samples[key] for key in sorted(samples)]


def exercise(args, qmp, output, debug, vnc_path, process, report):
    deadline = time.monotonic() + args.timeout
    baseline = debug.stat().st_size
    report.update(token=args.token, oracle_sha256=common.digest(Path(oracle.__file__)),
                  samples=[], commands=[], guest_completed=False)

    expected_failure = None
    seen = set()

    def console():
        if process.poll() is not None:
            raise RuntimeError('QEMU exited during Wayland testing')
        value = common.guest_text(debug, baseline) + common.console_text(qmp, output, args)
        fresh = [line for line in value.splitlines() if line and line not in seen]
        seen.update(fresh)
        with (output / 'wayland-observed.log').open('a') as stream:
            stream.write('\n'.join(fresh) + '\n')
        failures = re.findall(r'WLTEST FAILED run=([^ ]+)', value)
        if (re.search(r'kernel panic|amd64 fault v=|ZWL FAILED|ZWL IMPORT_ERROR|GPU SHARE FAILED|GPUFENCE FAIL|GPUADMISSION FAILED|Segmentation fault', value) or
                any(token != expected_failure for token in failures)):
            raise RuntimeError('guest Wayland/GPU failure; inspect console and renderer logs')
        fence_exit = re.search(r'/bin/gpu-fence-test[\r\n][\s\S]*root@[^\r\n]*\$ ', value)
        if fence_exit and re.search(r'GPUFENCE PASS(?:[\r\n]|$)', value) is None:
            raise RuntimeError('external fence test exited without successful completion')
        return value

    def command(value):
        report['commands'].append(value)
        qmp.text(value + '\n')

    def wait(pattern, description):
        matcher = re.compile(pattern)
        while time.monotonic() < deadline:
            found = matcher.search(console())
            if found:
                return found
            time.sleep(0.05)
        raise TimeoutError(description)

    command('/bin/gpu-admission-test')
    wait(r'GPUADMISSION PASS[^\r\n]+', 'same-open concurrent GPU resource lifecycle')
    report['concurrent_admission'] = True
    display_notice = wait(r'GPUADMISSION DISPLAY PASS sequence=(\d+) outputs=(\d+) attempts=(\d+) '
                          r'initial=1 query_preserved=1 acknowledged=1',
                          'initial display inventory notification and exact acknowledgement')
    report['display_notifications'] = {'sequence': int(display_notice[1]),
                                       'outputs': int(display_notice[2]),
                                       'attempts': int(display_notice[3])}

    command('/bin/gpu-fence-test')
    wait(r'GPUFENCE PASS(?:[\r\n]|$)', 'standard external fence fd lifetime and GPU signaling')
    report['external_fence'] = True

    command('/bin/gpu-share-test')
    wait(r'GPU SHARE PASS producer-exit SCM_RIGHTS independent-context import GPU-copy pixels=1024',
         'independent renderer import after producer exit')
    report['independent_renderer_import'] = True
    report['allocation_imports'] = []
    for option, kind in (('buffer', 1), ('optimal', 2)):
        command('/bin/gpu-share-test --' + option)
        marker = wait(r'GPU ALLOCATION PASS kind=' + str(kind) +
                      r' producer-exit independent-context pixels=1024',
                      option + ' allocation after independent producer exit')
        report['allocation_imports'].append({'kind': option, 'marker': marker.group(0)})

    def start_compositor():
        old = {int(value) for value in re.findall(r'ZWL READY[^\r\n]*pid=(\d+)', console())}
        command('/bin/zdesktop --socket=/tmp/wayland-0 --timeout=150 --log-frames &')
        while time.monotonic() < deadline:
            for marker in re.finditer(r'ZWL READY[^\r\n]*pid=(\d+)', console()):
                pid = int(marker.group(1))
                if pid > 1 and pid not in old:
                    report.setdefault('compositors', []).append({'pid': pid, 'ready': marker.group(0)})
                    return pid
            time.sleep(0.05)
        raise TimeoutError('fresh compositor startup')

    def stop_compositor(pid):
        command(f'kill {pid}')
        wait(r'ZWL EXIT frames=\d+ error=0 cleanup_failed=0 pid=' + str(pid) + r'\b',
             'compositor lease-safe shutdown')

    def ordinary(suffix):
        token = args.token + suffix
        command(f'/bin/wltest --display=/tmp/wayland-0 --frames=6 --token={token}')
        wait(r'WLTEST DONE run=' + re.escape(token) + r' frames=6', 'ordinary restarted client')
        return token

    compositor_pid = start_compositor()
    for mode in ('fifo', 'mailbox'):
        token = args.token + '-' + mode
        command(f'/bin/wltest --display=/tmp/wayland-0 --verify-session --mode={mode} --recreate-at=4 --token={token}')
        for frame in range(1, 7):
            marker = wait(r'WLTEST FRAME run=' + re.escape(token) + r' frame=' + str(frame) +
                          r' width=(\d+) height=(\d+) mode=' + mode,
                          f'{mode} frame {frame}')
            width, height = map(int, marker.groups())
            number = len(report['samples']) + 1
            shot = output / f'frame-{number}.ppm'
            capture_deadline = min(deadline, time.monotonic() + 15)
            mismatch = None
            while time.monotonic() < capture_deadline:
                info = capture_rfb(vnc_path, shot, min(5, capture_deadline - time.monotonic()))
                try:
                    check = oracle.verify(shot, frame, width, height)
                    mismatch = check
                    if check['passed']:
                        report['samples'].append({'mode': mode, 'frame': frame, 'width': width,
                                                  'height': height, 'oracle': check, 'capture': info,
                                                  'frame_sha256': common.digest(shot)})
                        (output / f'oracle-{number}.json').write_text(json.dumps(check, indent=2) + '\n')
                        save(output, report)
                        break
                except ValueError as error:
                    mismatch = str(error)
                time.sleep(0.05)
            else:
                raise TimeoutError(f'{mode} frame {frame} capture: {mismatch}')
            qmp.text('next\n')
        wait(r'WLTEST DONE run=' + re.escape(token) + r' frames=6', f'{mode} ordinary cleanup')
        report[mode + '_completed'] = True
        wait(r'WLTEST RECREATE run=' + re.escape(token) + r' before=4', f'{mode} oldSwapchain replacement')
    report['guest_completed'] = True
    # Correlate every observed compositor presentation with its imported K resource.
    text = '\n'.join(sorted(seen))
    imports = {(int(client), int(buffer), int(resource)) for client, buffer, resource in
               re.findall(r'ZWL IMPORT client=(\d+) buffer=(\d+) gpu_fd=\d+ resource=(\d+)', text)}
    presents = list(re.finditer(r'ZWL PRESENT client=(\d+) surface=\d+ buffer=(\d+) resource=(\d+) '
                               r'frame=(\d+) sequence=(\d+) width=(\d+) height=(\d+) flags=(\d+) refresh=(\d+)', text))
    report['shared_resources'] = [dict(zip(('client', 'buffer', 'resource', 'frame', 'sequence',
                                          'width', 'height', 'flags', 'refresh'), map(int, item.groups())))
                                  for item in presents]
    report['shared_gpu_path'] = (len(presents) == 12 and
        all(tuple(map(int, item.groups()[:3])) in imports and int(item.group(8)) & 2
            for item in presents))
    if not report['shared_gpu_path']:
        raise RuntimeError('compositor did not prove matching imported resources and BLOB presentation')

    if args.lifecycle:
        lifecycle = {'client_aborted': False, 'client_reopened': False,
                     'compositor_reopened': False, 'compositor_killed': False,
                     'surface_loss': False, 'console_restored': False}
        report['lifecycle'] = lifecycle
        token = args.token + '-abort'
        command(f'/bin/wltest --display=/tmp/wayland-0 --verify-session --token={token}')
        wait(r'WLTEST FRAME run=' + re.escape(token) + r' frame=1 ', 'held client frame before SIGINT')
        qmp.call('human-monitor-command', {'command-line': 'sendkey ctrl-c 1'})
        wait(r'WLTEST FRAME run=' + re.escape(token) + r' frame=1 [\s\S]*root@[^\r\n]*\$ ',
             'SIGINT returns the foreground terminal')
        if 'WLTEST DONE run=' + token in console():
            raise RuntimeError('interrupted client unexpectedly completed normally')
        lifecycle['client_aborted'] = True
        lifecycle['client_reopen_token'] = ordinary('-after-abort')
        lifecycle['client_reopened'] = True
        stop_compositor(compositor_pid)
        compositor_pid = start_compositor()
        lifecycle['compositor_reopened'] = True

        # SIGKILL exercises K fd teardown without compositor user-space cleanup.
        token = args.token + '-lost'
        expected_failure = token
        command(f'/bin/wltest --display=/tmp/wayland-0 --frames=600 --delay-ms=100 --token={token} &')
        wait(r'WLTEST FRAME run=' + re.escape(token) + r' frame=1 ', 'client before compositor SIGKILL')
        command(f'kill -9 {compositor_pid}')
        failure = wait(r'WLTEST FAILED run=' + re.escape(token) + r'[^\r\n]+',
                       'client observes compositor disconnection')
        details = re.search(r'api=(\S+) result=(-?\d+) cleanup=(-?\d+) errno=(\d+) frames=(\d+)',
                            failure.group(0))
        if details is None or int(details.group(3)) != 0 or int(details.group(5)) < 1:
            raise RuntimeError('disconnected client did not release its own GPU resources')
        native_loss = details.group(1) == 'wltest_window_dispatch' and int(details.group(2)) == 0
        vulkan_loss = details.group(1).startswith('vk') and int(details.group(2)) == -1000000000
        if not native_loss and not vulkan_loss:
            raise RuntimeError('client failure does not prove native disconnection or Vulkan surface loss')
        lifecycle['compositor_killed'] = True
        lifecycle['surface_loss'] = True
        lifecycle['surface_loss_marker'] = failure.group(0)
        killed = output / 'console-killed.ppm'
        capture_rfb(vnc_path, killed, 5)
        if oracle.read_ppm(killed)[:2] != (640, 480):
            raise RuntimeError('SIGKILL did not restore the native console scanout')
        lifecycle['killed_console_sha256'] = common.digest(killed)
        lifecycle['killed_console_restored'] = True
        # The disposable guest and this process identity prove ownership of this stale endpoint.
        command('/bin/rm /tmp/wayland-0')
        wait(r'/bin/rm /tmp/wayland-0[\s\S]*root@[^\r\n]*\$ ',
             'stale endpoint removal returns the shell prompt')
        compositor_pid = start_compositor()
        lifecycle['after_kill_token'] = ordinary('-after-kill')
        lifecycle['after_kill_reopened'] = True

    stop_compositor(compositor_pid)
    report['compositor_stopped'] = True
    if args.lifecycle:
        before, after = output / 'console-return.ppm', output / 'console-write.ppm'
        capture_rfb(vnc_path, before, 5)
        command('echo wayland-console-restoration')
        time.sleep(0.30)
        capture_rfb(vnc_path, after, 5)
        width, height, pixels = oracle.read_ppm(before)
        final_width, final_height, final_pixels = oracle.read_ppm(after)
        lifecycle['console_before_sha256'] = common.digest(before)
        lifecycle['console_after_sha256'] = common.digest(after)
        lifecycle['console_restored'] = ((width, height) == (640, 480) and
            (final_width, final_height) == (640, 480) and pixels != final_pixels)
        if not lifecycle['console_restored']:
            raise RuntimeError('console was not visibly restored and updated after compositor shutdown')
    report['status'] = 'pass'
    save(output, report)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--render-server', type=Path, required=True)
    parser.add_argument('--renderer-library-dir', type=Path)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--token', required=True)
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--lifecycle', action='store_true')
    parser.add_argument('--fault-test', choices=['recovery', 'producer-exit', 'producer-stop', 'submit-load', 'completion-delay', 'context-timeout', 'producer-exit-delayed', 'producer-exit-hang'])
    parser.add_argument('--console-address', type=lambda value: int(value, 0), required=True)
    parser.add_argument('--console-size', type=int, default=32768)
    parser.add_argument('--qemu', default='qemu-system-x86_64')
    parser.add_argument('--icd', default='/usr/share/vulkan/icd.d/intel_icd.json')
    parser.add_argument('--firmware', default='/usr/share/OVMF/OVMF_CODE_4M.fd')
    parser.add_argument('--variables', default='/usr/share/OVMF/OVMF_VARS_4M.fd')
    args = parser.parse_args()
    if not re.fullmatch(r'[a-z0-9-]{1,43}', args.token):
        parser.error('token must be a short lowercase identifier')
    if not 1 <= args.timeout <= 600 or not 1 <= args.console_size <= 1048576:
        parser.error('bounded timeout or console size is invalid')
    if not 0 <= args.console_address < 1024 ** 3 - args.console_size:
        parser.error('console capture lies outside guest memory')
    args.phase, args.frame, args.boot_only = 'wayland', 0, False
    if args.fault_test and args.lifecycle:
        parser.error('fault tests require their own fresh VM without the display lifecycle suite')
    return common.run(args, exercise=exercise_fault if args.fault_test else exercise, harness_path=__file__)


if __name__ == '__main__':
    sys.exit(main())
