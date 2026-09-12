#!/usr/bin/env python3
"""Exercise the bounded capture parser against a finite Unix RFB test peer.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

No real QEMU, remote connection or GPU is required.  Each case owns a temporary
Unix socket, and every peer and capture has a finite timeout.
"""
import importlib.util
import pathlib
import socket
import struct
import tempfile
import threading
import time

path = pathlib.Path(__file__).with_name('venus_rfb.py')
spec = importlib.util.spec_from_file_location('rfb', path)
rfb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rfb)
fmt = struct.pack('!BBBBHHHBBB3x', 32, 24, 0, 1, 255, 255, 255, 16, 8, 0)
raw = bytes([0, 0, 255, 0, 0, 255, 0, 0]) * 2
expected = b'P6\n2 2\n255\n' + bytes([255, 0, 0, 0, 255, 0]) * 2


def read(sock, size):
    result = b''
    while len(result) < size:
        chunk = sock.recv(size - len(result))
        if not chunk:
            raise EOFError('test client closed')
        result += chunk
    return result


def send(sock, data, partial):
    if not partial:
        sock.sendall(data)
        return
    for offset in range(0, len(data), 2):
        sock.sendall(data[offset:offset + 2])
        time.sleep(0.001)


def update(rectangles, count=None):
    return struct.pack('!BBH', 0, 0, len(rectangles) if count is None else count) + b''.join(rectangles)


def rectangle(x, y, width, height, encoding, data=b''):
    return struct.pack('!HHHHi', x, y, width, height, encoding) + data


def case(name, mode, failure=None, partial=False, timeout=2):
    errors = []
    with tempfile.TemporaryDirectory(prefix='q306-rfb-') as temporary:
        directory = pathlib.Path(temporary)
        endpoint = directory / 'vnc.sock'
        output = directory / 'frame.ppm'
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
            server.bind(str(endpoint))
            server.listen(1)
            server.settimeout(3)

            def peer():
                try:
                    with server.accept()[0] as client:
                        client.settimeout(3)
                        if mode == 'timeout':
                            time.sleep(0.15)
                            return
                        send(client, b'RFB 003.008\n', partial)
                        assert read(client, 12) == b'RFB 003.008\n'
                        send(client, b'\x01\x01', partial)
                        assert read(client, 1) == b'\x01'
                        send(client, bytes(4), partial)
                        assert read(client, 1) == b'\x01'
                        width = 5000 if mode == 'geometry' else 2
                        height = 2
                        if mode == 'resize':
                            width = height = 1
                        name_bytes = b'fake QEMU'
                        name_size = 65537 if mode == 'name' else len(name_bytes)
                        send(client, struct.pack('!HH', width, height) + fmt + struct.pack('!I', name_size) + name_bytes, partial)
                        if mode in ('geometry', 'name'):
                            return
                        assert read(client, 20) == bytes(4) + fmt
                        assert read(client, 16) == struct.pack('!BBHiii', 2, 0, 3, 0, -223, -224)
                        request = read(client, 10)
                        assert request == struct.pack('!BBHHHH', 3, 0, 0, 0, width, height)
                        if mode == 'resize':
                            send(client, update([rectangle(0, 0, 2, 2, -223), rectangle(0, 0, 0, 0, -224)], 65535), partial)
                            assert read(client, 10) == struct.pack('!BBHHHH', 3, 0, 0, 0, 2, 2)
                            send(client, update([rectangle(0, 0, 2, 1, 0, raw[:8])]), partial)
                            assert read(client, 10) == struct.pack('!BBHHHH', 3, 0, 0, 0, 2, 2)
                            send(client, update([rectangle(0, 1, 2, 1, 0, raw[8:])]), partial)
                        elif mode == 'bounds':
                            send(client, update([rectangle(1, 0, 2, 2, 0)]), partial)
                        elif mode == 'truncated':
                            send(client, update([rectangle(0, 0, 2, 2, 0, raw[:5])]), partial)
                        elif mode == 'encoding':
                            send(client, update([rectangle(0, 0, 2, 2, 5)]), partial)
                        else:
                            send(client, b'\x02' + b'\x03\x00\x00\x00' + struct.pack('!I', 0), partial)
                            send(client, update([rectangle(0, 0, 2, 2, 0, raw)]), partial)
                except Exception as error:
                    errors.append(error)

            worker = threading.Thread(target=peer)
            worker.start()
            try:
                result = rfb.capture(endpoint, output, timeout)
            except Exception as error:
                if failure is None or not isinstance(error, failure):
                    raise
                assert not output.exists(), 'failed capture published an image'
            else:
                assert failure is None, 'malformed server was accepted'
                assert output.read_bytes() == expected
                assert result['pixels'] == 4
                if mode == 'resize':
                    assert result['desktop_resizes'] == 1 and result['last_rectangles'] == 1
            finally:
                worker.join(4)
                assert not worker.is_alive(), 'fake peer failed to terminate'
            assert not errors, errors
    print(name, 'PASS')


def main():
    case('RFB partial handshake/RAW reads + exact RGB + notifications', 'normal', partial=True)
    case('RFB DesktopSize/LastRect + split spatial coverage', 'resize', partial=True)
    case('RFB rejects initial geometry limit', 'geometry', ValueError)
    case('RFB rejects server name length limit', 'name', ValueError)
    case('RFB rejects out-of-bounds rectangle', 'bounds', ValueError)
    case('RFB rejects unsupported encoding', 'encoding', ValueError)
    case('RFB rejects truncated RAW data without publishing output', 'truncated', EOFError)
    case('RFB total deadline expires and closes peer', 'timeout', TimeoutError, timeout=0.05)
    print('All finite fake RFB peer checks PASS; no QEMU or remote VM was executed.')


if __name__ == '__main__':
    main()
