#!/usr/bin/env python3
"""Local-only install/upgrade acceptance, using isolated HOME, checkout and remote."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
stage = Path(tempfile.mkdtemp(prefix="install-test-", dir=ROOT / "build"))
log = ROOT / "build/install-test.log"
env = {**os.environ, "HOME": str(stage / "test home"), "CC": "cc", "CFLAGS": "-O2",
       "LDFLAGS": "", "MAKEFLAGS": "", "MAKEOVERRIDES": "", "GIT_CONFIG_NOSYSTEM": "1"}
Path(env["HOME"]).mkdir()
prefix = Path(env["HOME"]) / ".local"
root_args = ["--system", "--prefix", str(prefix)] if os.geteuid() == 0 else []

def run(args, cwd=None, ok=True, extra_env=None):
    process = subprocess.run(list(map(str, args)), cwd=cwd, env={**env, **(extra_env or {})},
                             capture_output=True, text=True, timeout=60)
    with log.open("a") as f:
        f.write(f"$ {args!r}\n{process.stdout}{process.stderr}\nexit: {process.returncode}\n")
    assert (process.returncode == 0) == ok, (args, process.returncode, process.stdout[-3000:], process.stderr[-3000:])
    return process

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

try:
    log.write_text("")
    seed = stage / "seed"
    shutil.copytree(ROOT, seed, ignore=shutil.ignore_patterns(".git", "build", "__pycache__"))
    remote = stage / "remote.git"
    run(["git", "init", "--bare", remote])
    run(["git", "init", "-b", "main"], seed)
    run(["git", "config", "user.name", "PortTop tests"], seed)
    run(["git", "config", "user.email", "tests@example.invalid"], seed)
    run(["git", "add", "."], seed)
    run(["git", "commit", "-m", "installer test base"], seed)
    run(["git", "remote", "add", "origin", remote], seed)
    run(["git", "push", "-u", "origin", "main"], seed)
    checkout = stage / "checkout with spaces"
    run(["git", "clone", "-b", "main", remote, checkout])
    script = checkout / "install.sh"
    base = ["bash", script, *root_args]
    binary = prefix / "bin/porttop"
    helper = prefix / "bin/porttop-update"
    run([*base, "--bogus"], ok=False)
    run(["bash", script, "--prefix", "", *(["--system"] if root_args else [])], ok=False)
    run(["bash", script, "--prefix", "//", *(["--system"] if root_args else [])], ok=False)
    run(["bash", script, "--prefix", "relative", *(["--system"] if root_args else [])], ok=False)
    run(base, ok=False, extra_env={"CC": "porttop-no-such-compiler"})
    assert not binary.exists()
    if root_args: run(["bash", script], ok=False)
    run(base)
    original_version = run([binary, "--version"]).stdout
    assert original_version.startswith("porttop ") and helper.exists()
    sentinel = prefix / "bin/keep-me"
    sentinel.write_text("unrelated user program\n")
    # A real fast-forward from a local remote with a newer source version.
    header = seed / "src/porttop.h"
    base_header = header.read_text()
    old_version = original_version.strip().split()[1]
    header.write_text(base_header.replace(f'"{old_version}"', '"99.1.0"'))
    run(["git", "add", "src/porttop.h"], seed)
    run(["git", "commit", "-m", "new version"], seed)
    run(["git", "push"], seed)
    run([helper])
    assert run([binary, "--version"]).stdout.strip() == "porttop 99.1.0"
    before_failure = digest(binary)
    header.write_text(header.read_text() + "\nTHIS_BUILD_MUST_FAIL\n")
    run(["git", "add", "src/porttop.h"], seed)
    run(["git", "commit", "-m", "deliberately broken build"], seed)
    run(["git", "push"], seed)
    run([helper], ok=False)
    assert digest(binary) == before_failure, "failed build replaced working binary"
    # Dirty checkout must be rejected before pulling or rebuilding.
    readme = checkout / "README.md"
    readme.write_text(readme.read_text() + "\nlocal change\n")
    failed = run([helper], ok=False)
    assert "local changes" in failed.stderr
    assert digest(binary) == before_failure
    run(["git", "restore", "README.md"], checkout)
    header.write_text(base_header.replace(f'"{old_version}"', '"99.2.0"'))
    run(["git", "add", "src/porttop.h"], seed)
    run(["git", "commit", "-m", "fixed build"], seed)
    run(["git", "push"], seed)
    run([helper])
    assert run([binary, "--version"]).stdout.strip() == "porttop 99.2.0"
    # Modified installed files are not blindly removed.
    content = binary.read_bytes()
    binary.write_bytes(content + b"changed")
    run([*base, "--uninstall"], ok=False)
    assert binary.exists() and helper.exists()
    binary.write_bytes(content)
    run([*base, "--uninstall"])
    assert not binary.exists() and not helper.exists() and sentinel.exists()
    print("PASS install: default user prefix, paths with spaces, local-remote fast-forward upgrade, dirty/build failure preserves binary, guarded uninstall")
finally:
    shutil.rmtree(stage)
