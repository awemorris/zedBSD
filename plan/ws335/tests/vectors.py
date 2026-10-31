#!/usr/bin/env python3
# Converts the fetched test vectors into record files the C unit tests read with vecfile.h:
#   <cache>/rec/<name>.vec, records of "key = value" lines separated by blank lines.
# Sources: NIST CAVP .rsp files (inside the zips), Wycheproof JSON (inside the tarball), x509-limbo JSON, and the
# labelled hex values of RFC 8448's traces.  Values are copied, not computed: the expected values stay those of
# the source.  Run by fetch.sh; usage: python3 -I vectors.py <cache dir> <rfc8448.txt>
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import base64
import calendar
import fnmatch
import io
import json
import os
import re
import sys
import tarfile
import time
import zipfile

WYCHEPROOF = [
    'aes_gcm_test.json', 'chacha20_poly1305_test.json', 'x25519_test.json', 'ed25519_test.json',
    'ecdsa_secp256r1_sha256_test.json', 'ecdsa_secp256r1_sha512_test.json', 'ecdsa_secp384r1_sha384_test.json',
    'ecdsa_secp384r1_sha512_test.json', 'ecdh_secp256r1_ecpoint_test.json', 'rsa_signature_2048_sha256_test.json',
    'rsa_signature_2048_sha384_test.json', 'rsa_signature_2048_sha512_test.json', 'rsa_signature_3072_sha256_test.json',
    'rsa_signature_4096_sha512_test.json', 'rsa_pss_2048_sha256_mgf1_32_test.json', 'rsa_pss_3072_sha256_mgf1_32_test.json',
    'rsa_pss_4096_sha512_mgf1_64_test.json', 'hkdf_sha256_test.json', 'hkdf_sha384_test.json', 'hkdf_sha512_test.json',
    'hmac_sha256_test.json', 'hmac_sha384_test.json', 'hmac_sha512_test.json', 'mlkem_768_*test.json',
]


def write_records(path, records, header):
    with open(path, 'w') as out:
        out.write('# %s\n' % header)
        for record in records:
            out.write('\n')
            for key, value in record:
                out.write('%s = %s\n' % (key, value))


def flatten(prefix, value, into):
    if isinstance(value, dict):
        for key in value:
            flatten(prefix + '.' + key if prefix else key, value[key], into)
    elif isinstance(value, list):
        into.append((prefix, ','.join(str(item) for item in value)))
    else:
        into.append((prefix, str(value)))


def convert_cavp(cache, rec):
    for name in sorted(os.listdir(cache)):
        if not (name.startswith('cavp-') and name.endswith('.zip')):
            continue
        with zipfile.ZipFile(os.path.join(cache, name)) as archive:
            for member in archive.namelist():
                if not member.endswith('.rsp'):
                    continue
                text = archive.read(member).decode('latin-1')
                records, section, current = [], [], []
                for line in text.splitlines():
                    line = line.strip()
                    if line.startswith('#'):
                        continue
                    if line.startswith('[') and line.endswith(']'):
                        if current:
                            records.append(section + current)
                            current = []
                        body = line[1:-1]
                        key, _, value = body.partition('=')
                        section = [entry for entry in section if entry[0] != 'section.' + key.strip()]
                        section.append(('section.' + key.strip(), value.strip()))
                        continue
                    if not line:
                        if current:
                            records.append(section + current)
                            current = []
                        continue
                    key, sep, value = line.partition('=')
                    if sep:
                        current.append((key.strip(), value.strip()))
                    else:
                        current.append((line, ''))
                if current:
                    records.append(section + current)
                stem = os.path.splitext(os.path.basename(member))[0]
                out = 'cavp-%s-%s.vec' % (name[5:-4], stem)
                write_records(os.path.join(rec, out), records, 'NIST CAVP %s %s' % (name, member))


def convert_wycheproof(cache, rec):
    tarballs = [name for name in os.listdir(cache) if name.startswith('wycheproof-') and name.endswith('.tar.gz')]
    for tarball in tarballs:
        with tarfile.open(os.path.join(cache, tarball)) as archive:
            for member in archive.getmembers():
                base = os.path.basename(member.name)
                if '/testvectors_v1/' not in member.name:
                    continue
                if not any(fnmatch.fnmatch(base, pattern) for pattern in WYCHEPROOF):
                    continue
                data = json.load(archive.extractfile(member))
                records = []
                for group in data['testGroups']:
                    fields = []
                    for key in group:
                        if key != 'tests':
                            flatten('group.' + key, group[key], fields)
                    for test in group['tests']:
                        record = list(fields)
                        flatten('', test, record)
                        records.append(record)
                write_records(os.path.join(rec, 'wycheproof-' + base[:-5] + '.vec'), records,
                              'Wycheproof %s %s' % (tarball, member.name))


def pem_to_hex(pem):
    body = ''.join(line for line in pem.splitlines() if line and not line.startswith('-----'))
    return base64.b64decode(body).hex()


def convert_limbo(cache, rec):
    for name in os.listdir(cache):
        if not (name.startswith('x509-limbo-') and name.endswith('.json')):
            continue
        data = json.load(open(os.path.join(cache, name)))
        records = []
        for case in data['testcases']:
            if case['validation_kind'] != 'SERVER':
                continue
            record = [('id', case['id']), ('expected', case['expected_result'])]
            record.append(('features', ','.join(case.get('features') or [])))
            moment = case.get('validation_time')
            if moment:
                parsed = time.strptime(moment[:19], '%Y-%m-%dT%H:%M:%S')
                record.append(('time', str(calendar.timegm(parsed))))
            peer = case.get('expected_peer_name')
            if peer:
                record.append(('peer_kind', peer['kind']))
                record.append(('peer_value', peer['value']))
            record.append(('max_depth', str(case.get('max_chain_depth') or 0)))
            record.append(('extended_key_usage', ','.join(case.get('extended_key_usage') or [])))
            record.append(('peer', pem_to_hex(case['peer_certificate'])))
            for index, pem in enumerate(case['trusted_certs']):
                record.append(('trusted.%d' % index, pem_to_hex(pem)))
            for index, pem in enumerate(case['untrusted_intermediates']):
                record.append(('untrusted.%d' % index, pem_to_hex(pem)))
            records.append(record)
        write_records(os.path.join(rec, 'limbo.vec'), records, 'x509-limbo %s (SERVER cases)' % name)


def convert_rfc8448(path, rec):
    lines = []
    for line in open(path, encoding='latin-1'):
        line = line.rstrip('\n').replace('\f', '')
        if re.match(r'^(Thomson|RFC 8448) ', line):
            continue
        lines.append(line)
    records, step, section = [], '', ''
    index = 0
    while index < len(lines):
        line = lines[index]
        heading = re.match(r'^(\d+)\.\s+(.*)$', line)
        if heading:
            section = heading.group(1) + ' ' + heading.group(2).strip()
        stepline = re.match(r'^\s+\{(client|server)\}\s+(.*)$', line)
        if stepline:
            step = '{%s} %s' % (stepline.group(1), stepline.group(2).strip())
            while not step.endswith(':') and index + 1 < len(lines) and lines[index + 1].strip():
                index += 1
                step += ' ' + lines[index].strip()
        value = re.match(r'^\s+([A-Za-z][A-Za-z0-9 ]*) \((\d+) octets?\):\s*(.*)$', line)
        if value:
            hexes = [] if value.group(3).strip() == '(empty)' else value.group(3).split()
            wanted = int(value.group(2))
            look = index
            while len(hexes) < wanted and look + 1 < len(lines):
                following = lines[look + 1]
                if not following.strip():
                    look += 1
                    continue
                if not re.match(r'^\s+([0-9a-f]{2}\s*)+$', following):
                    break
                look += 1
                index = look
                hexes += following.split()
            records.append([('section', section), ('step', step), ('label', value.group(1).strip()),
                            ('octets', value.group(2)), ('hex', ''.join(hexes))])
        index += 1
    write_records(os.path.join(rec, 'rfc8448.vec'), records, 'RFC 8448 labelled values')


def main():
    cache, rfc8448 = sys.argv[1], sys.argv[2]
    rec = os.path.join(cache, 'rec')
    os.makedirs(rec, exist_ok=True)
    convert_cavp(cache, rec)
    convert_wycheproof(cache, rec)
    convert_limbo(cache, rec)
    if os.path.exists(rfc8448):
        convert_rfc8448(rfc8448, rec)
    else:
        print('vectors.py: no %s (rfc8448.vec not made)' % rfc8448)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
