#!/usr/bin/env python3
"""Fetch Alpine aarch64 packages (and their shared-library dependencies) and unpack them into a root directory.

No Alpine code is run: dependencies are resolved from APKINDEX here and each .apk (concatenated gzip tar streams)
is unpacked with Python's tarfile. Packages are checked against the APKINDEX SHA-1 (Q1 field).

usage: fetch_alpine.py --cache DIR --root DIR [--branch edge] [--skip PKG ...] PKG [PKG ...]
"""
import argparse, base64, gzip, hashlib, io, os, sys, tarfile, urllib.request

MIRROR = "https://dl-cdn.alpinelinux.org/alpine"


def load_index(cache, branch, repo):
    path = os.path.join(cache, f"APKINDEX-{branch}-{repo}.tar.gz")
    if not os.path.exists(path):
        urllib.request.urlretrieve(f"{MIRROR}/{branch}/{repo}/aarch64/APKINDEX.tar.gz", path)
    with tarfile.open(path) as t:
        text = t.extractfile("APKINDEX").read().decode()
    pkgs = {}
    for block in text.split("\n\n"):
        f = {}
        for line in block.splitlines():
            if len(line) > 2 and line[1] == ":":
                f[line[0]] = line[2:]
        if "P" in f:
            f["repo"] = repo
            pkgs[f["P"]] = f
    return pkgs


def resolve(pkgs, wanted, skip):
    provides = {}
    for name, f in pkgs.items():
        for p in f.get("p", "").split():
            provides.setdefault(p.split("=")[0], name)
        provides.setdefault(name, name)
    out, todo = [], list(wanted)
    while todo:
        n = todo.pop()
        if n.startswith("!"):
            continue
        base = n.split("=")[0].split(">")[0].split("<")[0]
        if base in skip:
            continue
        name = provides.get(base)
        if not name:
            if n.startswith("/"):
                continue  # file dependency (/bin/sh): busybox provides it
            sys.exit(f"unresolved dependency {n}")
        if name in out or name in skip:
            continue
        out.append(name)
        todo.extend(pkgs[name].get("D", "").split())
    return out


def fetch(cache, branch, f):
    fn = f"{f['P']}-{f['V']}.apk"
    path = os.path.join(cache, fn)
    if not os.path.exists(path):
        urllib.request.urlretrieve(f"{MIRROR}/{branch}/{f['repo']}/aarch64/{fn}", path)
    data = open(path, "rb").read()
    # Q1 is the SHA-1 of the control segment (second gzip stream); check it to catch corrupt downloads
    want = base64.b64decode(f["C"][2:]) if f.get("C", "").startswith("Q1") else None
    return path, data, want


def gzip_streams(data):
    """split concatenated gzip members (signature, control, data)"""
    out, pos = [], 0
    while pos < len(data):
        d = __import__("zlib").decompressobj(16 + 15)
        body = d.decompress(data[pos:])
        used = len(data) - pos - len(d.unused_data)
        out.append((data[pos:pos + used], body))
        pos += used
        if not d.unused_data:
            break
    return out


def unpack(data, want_sha1, root):
    streams = gzip_streams(data)
    if want_sha1 is not None:
        ctrl_raw = streams[1][0] if len(streams) >= 3 else streams[0][0]
        if hashlib.sha1(ctrl_raw).digest() != want_sha1:
            sys.exit("checksum mismatch")
    payload = streams[-1][1]
    with tarfile.open(fileobj=io.BytesIO(payload)) as t:
        for m in t.getmembers():
            if m.name.startswith(".") and "/" not in m.name:
                continue  # .PKGINFO, .SIGN.*, .pre-install
            if m.name.startswith("/") or ".." in m.name.split("/"):
                continue
            if m.isdev():
                continue
            if m.issym() and m.linkname.startswith("/"):
                continue  # absolute links (lib/modules/*/vmlinuz -> /boot/...): not needed, and unsafe to follow
            try:
                t.extract(m, root, filter="tar" if hasattr(tarfile, "data_filter") else None)
            except tarfile.FilterError as e:
                print(f"skipped {m.name}: {e}", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cache", required=True)
    ap.add_argument("--root", required=True)
    ap.add_argument("--branch", default="edge")
    ap.add_argument("--skip", nargs="*", default=[])
    ap.add_argument("--list", action="store_true", help="only print the resolved package list")
    ap.add_argument("pkgs", nargs="+")
    a = ap.parse_args()
    os.makedirs(a.cache, exist_ok=True)
    pkgs = {}
    for repo in ("main", "community"):
        for k, v in load_index(a.cache, a.branch, repo).items():
            pkgs.setdefault(k, v)
    names = resolve(pkgs, a.pkgs, set(a.skip))
    for n in names:
        print(f"{n}-{pkgs[n]['V']}")
    if a.list:
        return
    os.makedirs(a.root, exist_ok=True)
    for n in names:
        _, data, want = fetch(a.cache, a.branch, pkgs[n])
        unpack(data, want, a.root)


if __name__ == "__main__":
    main()
