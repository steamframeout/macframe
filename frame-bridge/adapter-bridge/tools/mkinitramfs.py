#!/usr/bin/env python3
"""Assemble the adapter-bridge initramfs (cpio newc, gzip) from an unpacked Alpine aarch64 root.

  --root DIR        Alpine packages unpacked (fetch_alpine.py)
  --kver VER        kernel release of the modules (7.3.5-0-lts)
  --extra-modules   directory of our own .ko files that replace the stock ones of the same name (rtw89 stack)
  --modules NAMES   modules to load (dependencies from modules.dep are added, loaded first)
  --bins PATHS      executables to copy from the root (their shared libraries are resolved with readelf)
  --firmware GLOBS  paths under lib/firmware to copy
  --overlay DIR     our files (init, scripts, templates), copied last
  --out FILE        initramfs.cpio.gz
Writes /etc/bridge/modules.load: one module path per line, in load order.
"""
import argparse, glob, gzip, io, os, re, shutil, stat, subprocess, sys

READELF = os.environ.get("READELF", "aarch64-linux-gnu-readelf")


def needed(path):
    out = subprocess.run([READELF, "-d", path], capture_output=True, text=True).stdout
    libs = []
    for line in out.splitlines():
        if "(NEEDED)" in line:
            libs.append(line.split("[")[1].split("]")[0])
    interp = subprocess.run([READELF, "-l", path], capture_output=True, text=True).stdout
    for line in interp.splitlines():
        if "Requesting program interpreter" in line:
            libs.append(os.path.basename(line.split(":")[1].strip().rstrip("]")))
    return libs


def find_lib(root, name):
    for d in ("lib", "usr/lib"):
        p = os.path.join(root, d, name)
        if os.path.lexists(p):
            return os.path.join(d, name)
    sys.exit(f"library {name} not found in {root}")


def copy_rel(root, rel, stage):
    """copy root/rel to stage/rel, following symlinks within root (keeping the link too)"""
    src = os.path.join(root, rel)
    dst = os.path.join(stage, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.islink(src):
        target = os.readlink(src)
        if not os.path.lexists(dst):
            os.symlink(target, dst)
        tgt_rel = os.path.normpath(os.path.join(os.path.dirname(rel), target)) if not target.startswith("/") else target.lstrip("/")
        copy_rel(root, tgt_rel, stage)
        return
    if not os.path.exists(dst):
        shutil.copy2(src, dst)


def modinfo_depends(ko):
    out = subprocess.run([READELF, "-p", ".modinfo", ko], capture_output=True, text=True).stdout
    for line in out.splitlines():
        if "depends=" in line:
            v = line.split("depends=", 1)[1].strip()
            return [d for d in v.split(",") if d]
    return []


def module_closure(root, kver, names, ours):
    """load order (dependencies first) of `names`; our modules replace stock ones and are added to the graph"""
    moddir = os.path.join(root, "lib/modules", kver)
    deps, by_name = {}, {}
    for line in open(os.path.join(moddir, "modules.dep")):
        mod, _, rest = line.partition(":")
        deps[mod] = rest.split()
        by_name[os.path.basename(mod).split(".ko")[0].replace("-", "_")] = mod
    builtin = set()
    bpath = os.path.join(moddir, "modules.builtin")
    if os.path.exists(bpath):
        builtin = {os.path.basename(l.strip()).split(".ko")[0].replace("-", "_") for l in open(bpath)}
    for name, ko in ours.items():
        path = "extra/" + name + ".ko"
        deps[path] = modinfo_depends(ko)  # names, resolved in visit()
        by_name[name] = path
    order, seen = [], set()

    def visit(n):
        if n in deps:  # a path from modules.dep (a stock module's dependency)
            name = os.path.basename(n).split(".ko")[0].replace("-", "_")
            path = by_name.get(name, n)  # ours replace stock modules of the same name
        else:  # a module name (command line, or our modules' modinfo depends)
            name = n.replace("-", "_")
            if name in builtin and name not in by_name:
                return
            path = by_name.get(name)
            if path is None:
                sys.exit(f"module {n} not in modules.dep or modules.builtin")
        if path in seen:
            return
        seen.add(path)
        for d in deps[path]:
            visit(d)
        order.append(path)

    for n in names:
        visit(n)
    return order


def busybox_applets(path):
    """applet names from busybox's sorted name table (starts at "[", "[["), so only applets that exist are linked"""
    b = open(path, "rb").read()
    i = b.find(b"[\0[[\0")
    if i < 0:
        sys.exit("busybox applet table not found")
    names, prev = [], ""
    for tok in b[i:i + 65536].split(b"\0"):
        t = tok.decode("ascii", "replace")
        if not re.fullmatch(r"[\[a-z0-9_.\-]+", t) or t < prev:
            break
        names.append(t)
        prev = t
    return names


def write_cpio(stage, out):
    buf = io.BytesIO()
    ino = 1

    def entry(name, mode, data=b"", nlink=1, mtime=0, rdev=(0, 0)):
        nonlocal ino
        nb = name.encode() + b"\0"
        hdr = "070701" + "".join(f"{v:08X}" for v in (ino, mode, 0, 0, nlink, mtime, len(data), 0, 0, rdev[0], rdev[1],
                                                        len(nb), 0))
        buf.write(hdr.encode() + nb)
        buf.write(b"\0" * ((4 - (110 + len(nb)) % 4) % 4))
        buf.write(data)
        buf.write(b"\0" * ((4 - len(data) % 4) % 4))
        ino += 1

    for d in ("dev", "proc", "sys", "run", "tmp", "mnt"):
        os.makedirs(os.path.join(stage, d), exist_ok=True)
    paths = []
    for dirpath, dirnames, filenames in os.walk(stage):
        dirnames.sort()
        for n in sorted(dirnames + filenames):
            paths.append(os.path.join(dirpath, n))
    for p in sorted(paths):
        rel = os.path.relpath(p, stage)
        st = os.lstat(p)
        if stat.S_ISLNK(st.st_mode):
            entry(rel, stat.S_IFLNK | 0o777, os.readlink(p).encode())
        elif stat.S_ISDIR(st.st_mode):
            entry(rel, stat.S_IFDIR | 0o755, nlink=2)
        else:
            mode = 0o755 if st.st_mode & 0o111 else 0o644
            entry(rel, stat.S_IFREG | mode, open(p, "rb").read())
    # the kernel opens /dev/console for init before anything can mount devtmpfs
    entry("dev/console", stat.S_IFCHR | 0o600, rdev=(5, 1))
    entry("TRAILER!!!", 0)
    with gzip.GzipFile(out, "wb", compresslevel=9, mtime=0) as f:
        f.write(buf.getvalue())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--kver", required=True)
    ap.add_argument("--extra-modules", required=True)
    ap.add_argument("--modules", nargs="+", required=True)
    ap.add_argument("--bins", nargs="+", required=True)
    ap.add_argument("--firmware", nargs="*", default=[])
    ap.add_argument("--files", nargs="*", default=[], help="other paths under root to copy as they are")
    ap.add_argument("--overlay", required=True)
    ap.add_argument("--stage", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    root, stage = a.root, a.stage
    if os.path.exists(stage):
        shutil.rmtree(stage)
    os.makedirs(stage)

    # programs and their libraries
    todo, done = [b.lstrip("/") for b in a.bins], set()
    while todo:
        rel = todo.pop()
        if rel in done:
            continue
        done.add(rel)
        copy_rel(root, rel, stage)
        real = os.path.realpath(os.path.join(root, rel))
        with open(real, "rb") as f:
            if f.read(4) != b"\x7fELF":
                continue
        for lib in needed(real):
            todo.append(find_lib(root, lib))

    # kernel modules: ours replace the stock files of the same name
    ours = {os.path.basename(p).split(".ko")[0]: p for p in glob.glob(os.path.join(a.extra_modules, "*.ko"))}
    order = module_closure(root, a.kver, a.modules, ours)
    load = []
    kdir = os.path.join("lib/modules", a.kver)
    for mod in order:
        base = os.path.basename(mod).split(".ko")[0].replace("-", "_")
        dst_rel = os.path.join(kdir, base + ".ko")
        dst = os.path.join(stage, dst_rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if mod.startswith("extra/"):
            shutil.copy2(ours[base], dst)
        else:
            src = os.path.join(root, kdir, mod)
            data = open(src, "rb").read()
            if mod.endswith(".gz"):
                data = gzip.decompress(data)
            elif mod.endswith(".zst"):
                sys.exit(f"{mod}: zstd modules not handled")
            open(dst, "wb").write(data)
        load.append("/" + dst_rel)
    # every module of ours must be in the load order (the rtw89 stack is listed explicitly)
    missing = [b for b in ours if "/" + os.path.join(kdir, b + ".ko") not in load]
    if missing:
        sys.exit(f"our modules not reached from --modules: {missing}")

    for g in a.firmware:
        hits = glob.glob(os.path.join(root, "lib/firmware", g))
        if not hits:
            sys.exit(f"no firmware matches {g}")
        for h in hits:
            copy_rel(root, os.path.relpath(h, root), stage)
    for f in a.files:
        copy_rel(root, f.lstrip("/"), stage)

    # busybox applets (busybox-static installs bin/busybox.static)
    bb = os.path.join(stage, "bin/busybox")
    if not os.path.exists(bb) and os.path.exists(bb + ".static"):
        os.rename(bb + ".static", bb)
    for d in ("bin", "sbin", "usr/bin", "usr/sbin"):
        os.makedirs(os.path.join(stage, d), exist_ok=True)
    applets = busybox_applets(bb)
    for ap_ in applets:
        if ap_ in ("busybox",):
            continue
        link = os.path.join(stage, "bin", ap_)
        if not os.path.lexists(link):
            os.symlink("busybox", link)

    # overlay (init, scripts) and the module load order
    for dirpath, _, filenames in os.walk(a.overlay):
        for n in filenames:
            src = os.path.join(dirpath, n)
            rel = os.path.relpath(src, a.overlay)
            dst = os.path.join(stage, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
    os.makedirs(os.path.join(stage, "etc/bridge"), exist_ok=True)
    open(os.path.join(stage, "etc/bridge/modules.load"), "w").write("\n".join(load) + "\n")
    # modules.dep / modules.alias for the included set, so the kernel's request_module() (crypto algorithms, ...)
    # finds them through /sbin/modprobe (busybox)
    moddir = os.path.join(root, "lib/modules", a.kver)
    included = {os.path.basename(p).split(".ko")[0] for p in load}
    deps_out = []
    for p_ in load:
        name = os.path.basename(p_).split(".ko")[0]
        ko = os.path.join(stage, p_.lstrip("/"))
        d = [x.replace("-", "_") for x in modinfo_depends(ko)]
        deps_out.append(f"{name}.ko: " + " ".join(f"{x}.ko" for x in d if x in included))
    open(os.path.join(stage, kdir, "modules.dep"), "w").write("\n".join(deps_out) + "\n")
    aliases = []
    for line in open(os.path.join(moddir, "modules.alias")):
        parts = line.split()
        if len(parts) == 3 and parts[0] == "alias" and parts[2].replace("-", "_") in included:
            aliases.append(line.strip())
    for p_ in load:  # ours are not in the stock modules.alias
        ko = os.path.join(stage, p_.lstrip("/"))
        name = os.path.basename(p_).split(".ko")[0]
        out = subprocess.run([READELF, "-p", ".modinfo", ko], capture_output=True, text=True).stdout
        for l in out.splitlines():
            if "alias=" in l:
                al = f"alias {l.split('alias=', 1)[1].strip()} {name}"
                if al not in aliases:
                    aliases.append(al)
    open(os.path.join(stage, kdir, "modules.alias"), "w").write("\n".join(aliases) + "\n")
    os.makedirs(os.path.join(stage, "sbin"), exist_ok=True)
    if not os.path.lexists(os.path.join(stage, "sbin/modprobe")):
        os.symlink("../bin/busybox", os.path.join(stage, "sbin/modprobe"))
    write_cpio(stage, a.out)
    print(f"{a.out}: {os.path.getsize(a.out) // 1024} KiB, {len(load)} modules")


if __name__ == "__main__":
    main()
