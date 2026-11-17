#!/usr/bin/env python3
"""Smoke-test the actual distribution archive, using isolated paths with spaces."""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
BUILD.mkdir(exist_ok=True)
LOG = BUILD / "package-test.log"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


with LOG.open("w") as log, tempfile.TemporaryDirectory(prefix="package test ", dir=BUILD) as work:
    work = Path(work)
    output = work / "output with spaces"
    # Deliberately invalid inherited flags must not contaminate a distribution build.
    env = {**os.environ, "CFLAGS": "--invalid-inherited-flag", "LDFLAGS": "--invalid-inherited-flag"}
    result = subprocess.run([sys.executable, str(ROOT / "scripts/package.py"), "--output", str(output)],
                            cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=120)
    assert result.returncode == 0, f"package build failed; see {LOG}"
    archives = list(output.glob("*.tar.gz"))
    assert len(archives) == 1
    archive = archives[0]
    checksum = archive.with_name(archive.name + ".sha256").read_text().strip()
    assert checksum == f"{digest(archive)}  {archive.name}"
    top = archive.name.removesuffix(".tar.gz")
    destination = work / "extracted"
    destination.mkdir()
    with tarfile.open(archive, "r:gz") as tar:
        for member in tar.getmembers():
            path = PurePosixPath(member.name)
            assert not path.is_absolute() and ".." not in path.parts and path.parts[0] == top
            assert member.isdir() or member.isfile(), "archive must not contain links or special files"
            target = destination / member.name
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with tar.extractfile(member) as contents:
                    target.write_bytes(contents.read())
                target.chmod(member.mode)
    bundle = destination / top
    binary = bundle / "bin/porttop"
    info = json.loads((bundle / "BUILD-INFO.json").read_text())
    assert info["binary_sha256"] == digest(binary)
    assert (bundle / "LICENSE").read_bytes() == (ROOT / "LICENSE").read_bytes()
    assert (bundle / "README.md").is_file()
    assert subprocess.check_output([binary, "--version"], text=True, timeout=10).strip() == f"porttop {info['version']}"
    snapshot = json.loads(subprocess.check_output([binary, "--json"], text=True, timeout=10))
    assert snapshot["viewer_uid"] == os.geteuid()
    assert snapshot["scope"] == ("all" if os.geteuid() == 0 else "current-user")
    assert snapshot["system"]["cpu_count"] > 0
    assert snapshot["system"]["memory_total_bytes"] > 0
    deps = "\n".join(info["dependencies"])
    assert "libasan" not in deps and "libubsan" not in deps
    log.write(json.dumps(info, indent=2) + "\n")
print("PASS package: fresh build, checksum, safe archive, identity, extracted --version/JSON, no sanitizer runtime")
