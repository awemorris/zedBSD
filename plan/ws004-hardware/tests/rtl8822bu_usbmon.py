"""Bounded target-only USB metadata; never retains frame or control payloads."""
import collections
import os
import select
import threading


class DescriptorObserver:
    def __init__(self, bus, address):
        self.bus = int(bus)
        self.address = int(address)
        self.fd = os.open(f'/sys/kernel/debug/usb/usbmon/{self.bus}u',
                          os.O_RDONLY | os.O_NONBLOCK)
        self.done = threading.Event()
        self.counts = collections.Counter()
        self.rx_samples = []
        self.tx_samples = []
        self.control_pending = {}
        self.control_longest = []
        self.control_max_us = 0
        self.error = None
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def accept(self, line):
        fields = line.split()
        if len(fields) < 6:
            return
        address = fields[3].split(':')
        if len(address) != 4 or address[0] not in ('Bi', 'Bo', 'Ci', 'Co'):
            return
        if int(address[1]) != self.bus or int(address[2]) != self.address:
            return
        direction, endpoint, event = address[0], int(address[3]), fields[2]
        if direction in ('Ci', 'Co'):
            if endpoint == 0:
                self.accept_control(fields, direction, event)
            return
        if (direction == 'Bi' and endpoint != 4) or (
                direction == 'Bo' and endpoint not in (5, 6, 8)):
            return
        if event not in ('S', 'C', 'E'):
            return
        status = int(fields[4])
        self.counts[f'{direction}-ep{endpoint}-{event}-status{status}'] += 1
        if len(fields) < 7 or fields[6] != '=' or (
                direction, event) not in (('Bi', 'C'), ('Bo', 'S')):
            return
        # usbmon words encode bytes, not native-endian machine integers.
        # Slice to the descriptor before interpreting or retaining anything.
        size = 24 if direction == 'Bi' else 32
        raw = bytes.fromhex(''.join(fields[7:])[:size * 2])
        if len(raw) < size:
            self.counts['short-descriptor-capture'] += 1
            return
        words = [int.from_bytes(raw[i:i + 4], 'little') for i in range(0, size, 4)]
        if direction == 'Bi':
            c2h = bool(words[2] & (1 << 28))
            sample = {'length': int(fields[5]), 'packet_length': words[0] & 0x3fff,
                      'c2h': c2h, 'crc_error': bool(words[0] & (1 << 14)),
                      'driver_info_length': ((words[0] >> 16) & 15) * 8,
                      'shift': (words[0] >> 24) & 3,
                      'phy_status': bool(words[0] & (1 << 26)),
                      'rate': words[3] & 0x7f}
            self.counts['rx-first-c2h' if c2h else 'rx-first-frame'] += 1
            kind_samples = sum(item['c2h'] == c2h for item in self.rx_samples)
            if sample not in self.rx_samples and kind_samples < 16:
                self.rx_samples.append(sample)
        else:
            queue = (words[1] >> 8) & 31
            # Firmware-download bodies use another format. Retain only the
            # two known management/H2C descriptor profiles, never their bodies.
            if not ((queue == 18 and ((words[0] >> 16) & 255) == 48) or
                    (queue == 19 and (words[0] & 0xffff) == 32)):
                return
            checksum = 0
            for offset in range(0, 32, 2):
                checksum ^= int.from_bytes(raw[offset:offset + 2], 'little')
            sample = {'length': int(fields[5]), 'endpoint': endpoint,
                      'packet_length': words[0] & 0xffff,
                      'offset': (words[0] >> 16) & 255, 'queue': queue,
                      'report_requested': bool(words[2] & (1 << 19)),
                      'rate': words[4] & 0x7f,
                      'sequence': (words[6] >> 2) & 63,
                      'checksum_valid': checksum == 0}
            if len(self.tx_samples) < 24:
                self.tx_samples.append(sample)

    def accept_control(self, fields, direction, event):
        # Retain request metadata and timing only; never decode control data.
        if event == 'S' and len(fields) >= 11 and fields[4] == 's':
            if int(fields[5], 16) not in (0x40, 0xc0) or int(fields[6], 16) != 5:
                return
            if int(fields[8], 16) != 0:
                return
            if len(self.control_pending) >= 64:
                self.counts['control-pending-limit'] += 1
                return
            self.control_pending[fields[0]] = (int(fields[1]), int(fields[7], 16),
                                                int(fields[9], 16), direction)
        elif event in ('C', 'E'):
            pending = self.control_pending.pop(fields[0], None)
            if pending is None:
                return
            elapsed = int(fields[1]) - pending[0]
            if elapsed < 0:
                self.counts['control-clock-wrap'] += 1
                return
            status = int(fields[4])
            self.counts[f'{pending[3]}-vendor-status{status}'] += 1
            self.control_max_us = max(self.control_max_us, elapsed)
            sample = {'register': pending[1], 'width': pending[2],
                      'direction': pending[3], 'elapsed_us': elapsed, 'status': status}
            self.control_longest.append(sample)
            self.control_longest.sort(key=lambda item: item['elapsed_us'], reverse=True)
            del self.control_longest[16:]

    def run(self):
        pending = b''
        try:
            while not self.done.is_set():
                if not select.select([self.fd], [], [], .1)[0]:
                    continue
                try:
                    block = os.read(self.fd, 65536)
                except BlockingIOError:
                    continue
                if not block:
                    break
                pending += block
                while b'\n' in pending:
                    line, pending = pending.split(b'\n', 1)
                    self.accept(line.decode('ascii', errors='replace'))
                if len(pending) > 4096:
                    raise ValueError('oversized usbmon record')
        except Exception as error:
            # Exception text could contain a malformed input record.
            self.error = type(error).__name__

    def stop(self):
        self.done.set()
        self.thread.join(timeout=2)
        os.close(self.fd)
        return {'counts': dict(self.counts), 'rx_first_descriptor_samples': self.rx_samples,
                'tx_descriptor_samples': self.tx_samples, 'error': self.error,
                'control_max_us': self.control_max_us,
                'longest_control_transfers': self.control_longest,
                'thread_stopped': not self.thread.is_alive()}
