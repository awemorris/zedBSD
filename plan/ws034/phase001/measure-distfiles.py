#!/usr/bin/env python3
"""ws034-p001: measure and check every archive listed in distfiles.tsv.

For each archive in build/distfiles this records the byte size, the SHA-256,
the top-level directory of the tar members, and the result of running the
WS032 checker (userland/packages/tools/archive.sh verify) with the measured
values, so the values can be copied into a package Makefile as they are.
It then compares against what upstream publishes (signature, digest file,
signed list, Debian .dsc, GitHub asset digest).  Signatures are checked in a
throw-away GNUPGHOME given by the caller; keys missing there are looked up on
a keyserver by the issuer fingerprint, and that is recorded, because such a
key proves only that the file was signed by the holder of that fingerprint.

usage: measure-distfiles.py <repo-root> <gnupghome> <out.json>
"""
import hashlib
import json
import os
import re
import subprocess
import sys

root, gnupghome, out = sys.argv[1:4]
dist = os.path.join(root, "build/distfiles")
manifest = os.path.join(root, "plan/ws034/phase001/distfiles.tsv")
archive_sh = os.path.join(root, "userland/packages/tools/archive.sh")
env = dict(os.environ, GNUPGHOME=gnupghome, LC_ALL="C")


def digest(path, algo):
    h = hashlib.new(algo)
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def run(args, timeout=600):
    p = subprocess.run(args, env=env, capture_output=True, text=True, errors="replace",
                       timeout=timeout)
    return p.returncode, p.stdout, p.stderr


def tar_roots(path):
    rc, so, se = run(["tar", "-tf", path])
    if rc != 0:
        return None, 0, se.strip()[:200]
    names = [n for n in so.splitlines() if n]
    roots = sorted({n.split("/", 1)[0] for n in names})
    return roots, len(names), ""


def gpg_verify(sig, data):
    """Return (status, fingerprint, key-source)."""
    args = ["gpg", "--batch", "--status-fd", "1", "--verify", sig]
    if data:
        args.append(data)
    rc, so, se = run(args, timeout=120)
    m = re.search(r"VALIDSIG ([0-9A-F]{40})", so)
    if m:
        return "good", m.group(1), "local-keyring"
    m = re.search(r"NO_PUBKEY ([0-9A-F]+)", so)
    issuer = re.search(r"ISSUER_FPR ([0-9A-F]{40})", so)
    want = issuer.group(1) if issuer else (m.group(1) if m else None)
    if not want:
        return "bad:" + so.strip().replace("\n", " ")[:200], None, None
    for ks in ("hkps://keys.openpgp.org", "hkps://keyserver.ubuntu.com"):
        run(["gpg", "--batch", "--keyserver", ks, "--recv-keys", want],
            timeout=120)
        rc, so, se = run(args, timeout=120)
        m = re.search(r"VALIDSIG ([0-9A-F]{40})", so)
        if m:
            return "good", m.group(1), "keyserver " + ks
        if "BADSIG" in so:
            return "BADSIG", want, ks
    return "no-key", want, None


results = []
for line in open(manifest):
    if line.startswith("#") or not line.strip():
        continue
    f = line.rstrip("\n").split("\t")
    key, version, archive, url, kind = f[:5]
    vurls = f[5:]
    path = os.path.join(dist, archive)
    r = {"key": key, "version": version, "archive": archive, "url": url,
         "verify_kind": kind}
    if not os.path.isfile(path):
        r["error"] = "not fetched"
        results.append(r)
        continue
    r["size"] = os.path.getsize(path)
    r["sha256"] = digest(path, "sha256")
    if archive.endswith(".pem"):
        r["roots"], r["members"] = None, 0
    else:
        roots, members, err = tar_roots(path)
        r["roots"], r["members"] = roots, members
        if err:
            r["tar_error"] = err
        if roots and len(roots) == 1:
            rc, so, se = run(["sh", archive_sh, "verify", path, str(r["size"]),
                              r["sha256"], roots[0]])
            r["archive_sh_verify"] = "ok" if rc == 0 else se.strip()[:300]
        else:
            r["archive_sh_verify"] = "not single root"

    checks = []
    if kind == "sig":
        for v in vurls:
            vp = os.path.join(dist, archive + ".verify." + v.rsplit("/", 1)[1])
            if v.endswith((".sig", ".asc")):
                st, fp, src = gpg_verify(vp, path)
                checks.append({"method": "openpgp-signature", "file": v,
                               "status": st, "fingerprint": fp,
                               "key_source": src})
            elif v.endswith(".sha256sum") or v.endswith(".sha256"):
                txt = open(vp).read()
                checks.append({"method": "published-sha256", "file": v,
                               "status": "match" if r["sha256"] in txt else "MISMATCH"})
    elif kind in ("sha256-file", "sha512-file"):
        v = vurls[0]
        vp = os.path.join(dist, archive + ".verify." + v.rsplit("/", 1)[1])
        txt = open(vp).read().lower()
        want = r["sha256"] if kind == "sha256-file" else digest(path, "sha512")
        checks.append({"method": "published-" + kind[:-5], "file": v,
                       "status": "match" if want in txt else "MISMATCH"})
        asc = vp + ".asc"
        if os.path.isfile(asc):
            st, fp, src = gpg_verify(asc, vp)
            checks.append({"method": "openpgp-signature-of-digest-file",
                           "status": st, "fingerprint": fp, "key_source": src})
    elif kind == "sha256sums-signed":
        v = vurls[0]
        vp = os.path.join(dist, archive + ".verify." + v.rsplit("/", 1)[1])
        txt = open(vp).read()
        ok = re.search(re.escape(r["sha256"]) + r"\s+" + re.escape(archive), txt)
        checks.append({"method": "published-sha256-list", "file": v,
                       "status": "match" if ok else "MISMATCH"})
        st, fp, src = gpg_verify(vp, None)
        checks.append({"method": "openpgp-clearsigned-list", "status": st,
                       "fingerprint": fp, "key_source": src})
    elif kind == "dsc":
        v = vurls[0]
        vp = os.path.join(dist, archive + ".verify." + v.rsplit("/", 1)[1])
        txt = open(vp).read()
        ok = re.search(re.escape(r["sha256"]) + r"\s+" + str(r["size"]) + r"\s+"
                       + re.escape(archive), txt)
        checks.append({"method": "debian-dsc-sha256", "file": v,
                       "status": "match" if ok else "MISMATCH"})
        st, fp, src = gpg_verify(vp, None)
        checks.append({"method": "openpgp-clearsigned-dsc", "status": st,
                       "fingerprint": fp, "key_source": src})
    elif kind == "gh-digest":
        vp = path + ".gh-digest"
        txt = open(vp).read().strip() if os.path.isfile(vp) else ""
        checks.append({"method": "github-release-asset-digest",
                       "published": txt,
                       "status": "match" if txt == "sha256:" + r["sha256"]
                       else ("absent" if not txt else "MISMATCH")})
    r["checks"] = checks
    results.append(r)

json.dump(results, open(out, "w"), indent=1)
for r in results:
    print(r["key"], r.get("size"), r.get("sha256"), r.get("roots"),
          r.get("archive_sh_verify"),
          [(c["method"], c["status"]) for c in r.get("checks", [])],
          r.get("error", ""))
