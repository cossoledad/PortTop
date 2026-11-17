#!/usr/bin/env python3
"""Build a native Linux bundle from fresh objects, without changing the user install."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def run(args, *, env=None):
    return subprocess.run(args, cwd=ROOT, env=env, check=True, text=True,
                          stdout=subprocess.PIPE).stdout.strip()


def source_identity():
    if shutil.which("git"):
        try:
            if Path(run(["git", "rev-parse", "--show-toplevel"])) == ROOT:
                return {"commit": run(["git", "rev-parse", "HEAD"]),
                        "dirty": bool(run(["git", "status", "--porcelain"]))}
        except subprocess.CalledProcessError:
            pass
    return {"commit": None, "dirty": None}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/dist")
    args = parser.parse_args()
    if platform.system() != "Linux":
        parser.error("native packaging currently requires Linux")
    arch = {"AMD64": "x86_64", "arm64": "aarch64"}.get(platform.machine(), platform.machine())
    if arch not in ("x86_64", "aarch64"):
        parser.error(f"unsupported native architecture: {arch}")
    version = re.search(r'^#define PT_VERSION "([0-9A-Za-z][0-9A-Za-z.+-]*)"$',
                        (ROOT / "src/porttop.h").read_text(), re.MULTILINE)
    if not version:
        parser.error("missing or invalid PT_VERSION")
    version = version.group(1)
    compiler = os.environ.get("CC", "cc")
    for tool in ("make", "strip", "ldd"):
        if not shutil.which(tool):
            parser.error(f"required build tool is missing: {tool}")
    env = dict(os.environ)
    for key in ("MAKEFLAGS", "MFLAGS", "MAKEOVERRIDES", "CFLAGS", "CPPFLAGS", "LDFLAGS"):
        env.pop(key, None)
    env["LC_ALL"] = "C"
    build = ROOT / "build"
    build.mkdir(exist_ok=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    name = f"porttop-{version}-linux-{arch}"
    with tempfile.TemporaryDirectory(prefix="package-", dir=build) as work:
        work = Path(work)
        obj = work / "obj"
        subprocess.run(["make", f"BUILD_DIR={obj}", f"CC={compiler}",
                        "CFLAGS=-O2 -std=c11 -Wall -Wextra -Wpedantic -Wformat=2 -Wshadow -Wconversion",
                        "LDFLAGS=", "all"], cwd=ROOT, env=env, check=True)
        bundle = work / name
        (bundle / "bin").mkdir(parents=True)
        binary = bundle / "bin/porttop"
        shutil.copy2(obj / "porttop", binary)
        subprocess.run(["strip", str(binary)], check=True)
        actual = run([str(binary), "--version"])
        if actual != f"porttop {version}":
            raise RuntimeError(f"built version mismatch: {actual!r}")
        shutil.copyfile(ROOT / "LICENSE", bundle / "LICENSE")
        shutil.copyfile(ROOT / "docs/binary-install.md", bundle / "README.md")
        dependencies = re.sub(r"\s+\(0x[0-9a-fA-F]+\)", "", run(["ldd", str(binary)], env=env))
        info = {
            "version": version, "system": "Linux", "architecture": arch,
            "source": source_identity(),
            "distribution": platform.freedesktop_os_release().get("PRETTY_NAME", "Linux"),
            "libc": list(platform.libc_ver()),
            "compiler": run([*shlex.split(compiler), "--version"], env=env).splitlines()[0],
            "dependencies": dependencies.splitlines(),
            "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        }
        (bundle / "BUILD-INFO.json").write_text(json.dumps(info, indent=2) + "\n")
        # Normalize archive metadata; this is not a cross-toolchain reproducibility claim.
        archive = work / f"{name}.tar.gz"
        with archive.open("wb") as raw:
            with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
                with tarfile.open(fileobj=compressed, mode="w", format=tarfile.USTAR_FORMAT) as tar:
                    for path in [bundle, *sorted(bundle.rglob("*"))]:
                        entry = tar.gettarinfo(str(path), str(path.relative_to(work)))
                        entry.uid = entry.gid = entry.mtime = 0
                        entry.uname = entry.gname = ""
                        entry.mode = 0o755 if path.is_dir() or path == binary else 0o644
                        if path.is_file():
                            with path.open("rb") as contents:
                                tar.addfile(entry, contents)
                        else:
                            tar.addfile(entry)
        checksum = work / f"{name}.tar.gz.sha256"
        checksum.write_text(f"{hashlib.sha256(archive.read_bytes()).hexdigest()}  {archive.name}\n")
        os.replace(archive, output / archive.name)
        os.replace(checksum, output / checksum.name)
    print(output / archive.name)


if __name__ == "__main__":
    main()
