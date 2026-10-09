#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib
"""P3 of WS126: Python's ssl and hashlib modules over OpenSSL.

Run on the guest as `python3 tls-loopback.py`.  It checks, in order:

1. hashlib reaches OpenSSL (_hashlib) and its digests are right;
2. the default CA bundle is the one security/ca-certificates installs, and a
   default context loads certificates from it;
3. a TLS server and client on the loopback, with a self-signed certificate
   made for the run by the openssl command, complete a handshake, verify the
   server by that certificate and exchange data both ways.

Each failure prints a `FAIL:` line; the last line is `tls-loopback: PASS` or
`tls-loopback: FAIL`.  The temporary files go to a directory of the run under
the system's temporary directory and are left there.
"""

import argparse
import hashlib
import os
import socket
import ssl
import subprocess
import sys
import tempfile
import threading

failures = []


def fail(message):
    """Records one failed check."""
    failures.append(message)
    print("FAIL: " + message, flush=True)


def check_hashlib():
    """Checks that hashlib's digests come from OpenSSL and are right."""
    try:
        import _hashlib
    except ImportError as error:
        fail("_hashlib does not import: %s" % error)
        return

    # The SHA-256 of "abc" from FIPS 180-2, through OpenSSL directly.
    expected = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    digest = _hashlib.openssl_sha256(b"abc").hexdigest()
    if digest != expected:
        fail("openssl_sha256(abc) is %s" % digest)

    # hashlib.new() reaches OpenSSL's algorithms beyond the built-in ones.
    if "sha512_256" not in hashlib.algorithms_available:
        fail("sha512_256 missing from hashlib.algorithms_available")

    # PBKDF2 is OpenSSL's in this build (RFC 6070, test 2).
    derived = hashlib.pbkdf2_hmac("sha1", b"password", b"salt", 2, 20).hex()
    if derived != "ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957":
        fail("pbkdf2_hmac(sha1) is %s" % derived)


def check_default_bundle(expected_cafile):
    """Checks the default CA bundle path and that a default context loads it."""
    paths = ssl.get_default_verify_paths()
    print("verify paths: %s" % (paths,), flush=True)
    if paths.openssl_cafile != expected_cafile:
        fail("OpenSSL's default CA file is %s, not %s" % (paths.openssl_cafile, expected_cafile))
        return
    if not os.path.isfile(expected_cafile):
        fail("%s is not there" % expected_cafile)
        return

    context = ssl.create_default_context()
    stats = context.cert_store_stats()
    print("default context: %s" % (stats,), flush=True)
    if stats.get("x509_ca", 0) == 0:
        fail("the default context loaded no CA certificate")


def make_certificate(directory):
    """Makes a self-signed certificate for localhost; returns (cert, key)."""
    certificate = os.path.join(directory, "server.pem")
    key = os.path.join(directory, "server.key")
    command = [
        "openssl", "req", "-x509", "-newkey", "ec",
        "-pkeyopt", "ec_paramgen_curve:prime256v1",
        "-nodes", "-days", "1", "-subj", "/CN=localhost",
        "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
        "-keyout", key, "-out", certificate,
    ]
    completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if completed.returncode != 0:
        fail("openssl req failed (%d): %s" % (completed.returncode, completed.stdout.decode(errors="replace")))
        return None
    return certificate, key


def serve_once(listener, context, outcome):
    """Accepts one TLS client, echoes what it sends with a prefix, and closes."""
    try:
        connection, _ = listener.accept()
        with context.wrap_socket(connection, server_side=True) as tls:
            request = tls.recv(1024)
            tls.sendall(b"echo:" + request)
            outcome["version"] = tls.version()
    except Exception as error:  # reported by the main thread
        outcome["error"] = repr(error)


def check_loopback(directory):
    """Checks a TLS handshake and an exchange over the loopback."""
    made = make_certificate(directory)
    if made is None:
        return
    certificate, key = made

    server_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    server_context.load_cert_chain(certificate, key)
    client_context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    client_context.load_verify_locations(certificate)

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(30)
    port = listener.getsockname()[1]

    outcome = {}
    server = threading.Thread(target=serve_once, args=(listener, server_context, outcome))
    server.start()
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=30) as raw:
            with client_context.wrap_socket(raw, server_hostname="localhost") as tls:
                tls.sendall(b"hello")
                reply = tls.recv(1024)
                print("client: %s %s" % (tls.version(), tls.cipher()[0]), flush=True)
        if reply != b"echo:hello":
            fail("the reply was %r" % reply)
    except Exception as error:
        fail("the client failed: %r" % error)
    server.join(30)
    listener.close()

    if "error" in outcome:
        fail("the server failed: %s" % outcome["error"])
    elif outcome.get("version") is None:
        fail("the server did not finish")
    else:
        print("server: %s" % outcome["version"], flush=True)


def main():
    """Runs the checks and prints the verdict."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--cafile", default="/etc/ssl/cert.pem",
                        help="the default CA bundle the build is expected to report")
    arguments = parser.parse_args()

    print("ssl: %s" % ssl.OPENSSL_VERSION, flush=True)
    check_hashlib()
    check_default_bundle(arguments.cafile)
    directory = tempfile.mkdtemp(prefix="tls-loopback.")
    check_loopback(directory)

    if failures:
        print("tls-loopback: FAIL", flush=True)
        return 1
    print("tls-loopback: PASS", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
