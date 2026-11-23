#!/usr/bin/env python3
"""Download Homebrew arm64 bottles for a formula and its runtime dependencies (recursive), checksum-verified.
usage: fetch_bottles.py --cache DIR --out DIR [--tag arm64_sequoia] FORMULA
Unpacks each bottle into OUT/<formula>/ (Homebrew's <name>/<version>/ layout inside)."""
import argparse, hashlib, json, os, tarfile, urllib.request

def formula(name, cache):
    p = os.path.join(cache, f"{name}.json")
    if not os.path.exists(p):
        urllib.request.urlretrieve(f"https://formulae.brew.sh/api/formula/{name}.json", p)
    return json.load(open(p))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cache", required=True); ap.add_argument("--out", required=True)
    ap.add_argument("--tag", default="arm64_sequoia"); ap.add_argument("name")
    a = ap.parse_args()
    os.makedirs(a.cache, exist_ok=True); os.makedirs(a.out, exist_ok=True)
    seen, todo, order = set(), [a.name], []
    while todo:
        n = todo.pop()
        if n in seen: continue
        seen.add(n)
        f = formula(n, a.cache)
        order.append(n)
        todo.extend(f.get("dependencies", []))
    for n in order:
        f = formula(n, a.cache)
        files = f["bottle"]["stable"]["files"]
        b = files.get(a.tag) or files.get("all")
        if not b: raise SystemExit(f"{n}: no {a.tag} bottle (have {list(files)})")
        path = os.path.join(a.cache, f"{n}-{f['versions']['stable']}.{a.tag}.tar.gz")
        if not os.path.exists(path):
            req = urllib.request.Request(b["url"], headers={"Authorization": "Bearer QQ=="})
            with urllib.request.urlopen(req) as r, open(path + ".part", "wb") as o:
                while chunk := r.read(1 << 20): o.write(chunk)
            os.rename(path + ".part", path)
        if hashlib.sha256(open(path, "rb").read()).hexdigest() != b["sha256"]:
            raise SystemExit(f"{n}: checksum mismatch")
        dst = os.path.join(a.out, n)
        if not os.path.exists(dst):
            with tarfile.open(path) as t:
                t.extractall(a.out, filter="tar")
        print(f"{n} {f['versions']['stable']}")

if __name__ == "__main__":
    main()
