#!/usr/bin/env python3
"""Make a self-contained qemu-system-aarch64 for an app bundle from unpacked Homebrew bottles.

  bundle_qemu.py --cellar DIR --out DIR
    DIR/<formula>/<version>/...  (fetch_bottles.py output)
    -> OUT/bin/qemu-system-aarch64, OUT/lib/*.dylib

Every Homebrew reference (@@HOMEBREW_PREFIX@@/opt/<f>/lib/x.dylib, @@HOMEBREW_CELLAR@@/<f>/<v>/lib/x.dylib,
@rpath/x.dylib, @loader_path/x.dylib) is resolved in the cellar, the library is copied once under its leaf name, and
the load commands are rewritten: the executable loads @executable_path/../lib/x.dylib, libraries load
@loader_path/x.dylib. System libraries (/usr/lib, /System) stay as they are. The files must be (re)signed
afterwards (rewriting invalidates signatures; Apple Silicon runs only signed code)."""
import argparse, glob, os, shutil, sys
from macholib.MachO import MachO

SYSTEM = ("/usr/lib/", "/System/")


def find_formula_lib(cellar, formula, leaf):
    hits = glob.glob(os.path.join(cellar, formula, "*", "lib", leaf))
    return hits[0] if hits else None


def resolve(cellar, ref, origin_dir):
    leaf = os.path.basename(ref)
    if ref.startswith("@@HOMEBREW_PREFIX@@/opt/"):
        return find_formula_lib(cellar, ref.split("/")[2], leaf)
    if ref.startswith("@@HOMEBREW_CELLAR@@/"):
        return find_formula_lib(cellar, ref.split("/")[1], leaf)
    if ref.startswith("@loader_path/"):
        p = os.path.normpath(os.path.join(origin_dir, ref[len("@loader_path/"):]))
        return p if os.path.exists(p) else None
    if ref.startswith("@rpath/") or ref.startswith("@@HOMEBREW_PREFIX@@/lib/"):
        hits = glob.glob(os.path.join(cellar, "*", "*", "lib", leaf))
        return hits[0] if hits else None
    return None


def deps(path):
    m = MachO(path)
    out = []
    for h in m.headers:
        for lc, cmd, data in h.commands:
            if lc.cmd in (0xC, 0x80000018, 0x8000001F, 0x80000023):  # LOAD, LOAD_WEAK, REEXPORT, LOAD_UPWARD
                out.append(data.rstrip(b"\0").decode())
    return out


def rewrite(path, mapping, new_id=None):
    m = MachO(path)

    def change(ref):
        return mapping.get(ref)

    m.rewriteLoadCommands(change)
    if new_id:
        for h in m.headers:
            h.rewriteInstallNameCommand(new_id.encode())
    with open(path, "rb+") as f:  # as macholib.MachOStandalone does
        f.seek(0)
        m.write(f)
        f.seek(0, 2)
        f.flush()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cellar", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--binary", default="qemu-system-aarch64")
    a = ap.parse_args()
    src_bin = glob.glob(os.path.join(a.cellar, "qemu", "*", "bin", a.binary))
    if not src_bin:
        sys.exit("qemu binary not found in the cellar")
    if os.path.exists(a.out):
        shutil.rmtree(a.out)
    os.makedirs(os.path.join(a.out, "bin"))
    os.makedirs(os.path.join(a.out, "lib"))
    exe = os.path.join(a.out, "bin", a.binary)
    shutil.copyfile(src_bin[0], exe)
    os.chmod(exe, 0o755)

    # breadth-first over (bundled file, its original location)
    queue = [(exe, os.path.dirname(os.path.realpath(src_bin[0])), True)]
    copied = {}
    while queue:
        path, origin_dir, is_exe = queue.pop(0)
        mapping = {}
        for ref in deps(path):
            if ref.startswith(SYSTEM):
                continue
            src = resolve(a.cellar, ref, origin_dir)
            if not src:
                sys.exit(f"{os.path.basename(path)}: cannot resolve {ref}")
            leaf = os.path.basename(ref)
            dst = os.path.join(a.out, "lib", leaf)
            if leaf not in copied:
                shutil.copyfile(os.path.realpath(src), dst)
                os.chmod(dst, 0o755)
                copied[leaf] = dst
                queue.append((dst, os.path.dirname(os.path.realpath(src)), False))
            mapping[ref] = ("@executable_path/../lib/" if is_exe else "@loader_path/") + leaf
        rewrite(path, mapping, None if is_exe else "@loader_path/" + os.path.basename(path))

    # nothing may still point at a Homebrew location
    for f in [exe] + list(copied.values()):
        for ref in deps(f):
            if "HOMEBREW" in ref or (ref.startswith("@rpath/")):
                sys.exit(f"{f}: unresolved reference left: {ref}")
    print(f"{a.binary}: {len(copied)} libraries")
    for leaf in sorted(copied):
        print("  " + leaf)


if __name__ == "__main__":
    main()
