#!/usr/bin/env python3
"""Real /proc and PTY acceptance. Only signals processes created by this test."""
import errno
import fcntl
import json
import os
from pathlib import Path
import pty
import re
import select
import signal
import socket
import struct
import subprocess
import sys
import termios
import time
from vt import Screen

BIN = str(Path(sys.argv[1]).resolve())
ROOT = Path(__file__).resolve().parent.parent

def snapshot(*args):
    p = subprocess.run([BIN, "--json", *map(str, args)], capture_output=True, text=True, timeout=10)
    assert p.returncode == 0, p.stderr
    return json.loads(p.stdout)

class Terminal:
    def __init__(self, *args, locale="C.UTF-8", color=False):
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 36, 130, 0, 0))
        self.original = termios.tcgetattr(slave)
        self.slave = slave
        terminal_env = {**os.environ, "TERM": "xterm-256color", "LC_ALL": locale}
        if color: terminal_env.pop("NO_COLOR", None)
        self.process = subprocess.Popen([BIN, *([] if color else ["--no-color"]), *map(str, args)], stdin=slave, stdout=slave, stderr=slave, env=terminal_env)
        self.data = b""
        self.screen = Screen()
    def send(self, data):
        os.write(self.master, data)
    def until(self, pattern, seconds=5):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if pattern.decode() in self.screen.text:
                return
            ready, _, _ = select.select([self.master], [], [], .1)
            if ready:
                try:
                    block = os.read(self.master, 65536)
                    self.data += block
                    self.screen.feed(block)
                except OSError as e:
                    if e.errno != errno.EIO:
                        raise
            if self.process.poll() is not None:
                break
        if pattern.decode() not in self.screen.text:
            (ROOT / "build/pty-failure.log").write_bytes(self.data)
            plain = re.sub(rb"\x1b\[[0-9;?]*[A-Za-z]", b" ", self.data[-6000:])
            raise AssertionError((pattern, self.screen.text, plain))
    def drain(self, seconds=.2):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            ready, _, _ = select.select([self.master], [], [], min(.02, max(0, deadline-time.monotonic())))
            if ready:
                block = os.read(self.master, 65536)
                self.data += block
                self.screen.feed(block)
    def wheel(self, down=True, x=25, y=10):
        self.send(f"\x1b[<{65 if down else 64};{x+1};{y+1}M".encode())
    def info_progress(self):
        line = next(line for line in self.screen.text.splitlines() if "PROCESS INFORMATION" in line)
        return tuple(map(int, re.search(r"(\d+)-(\d+) / (\d+)", line).groups()))
    def info_bounds(self):
        lines = self.screen.text.splitlines()
        top = next(i for i, line in enumerate(lines) if "PROCESS INFORMATION" in line)
        title = lines[top].index("PROCESS INFORMATION")
        left, right = lines[top].rfind("╭", 0, title), lines[top].find("╮", title)
        bottom = next(i for i in range(top+1, len(lines)) if lines[i][left]=="╰" and lines[i][right]=="╯")
        return left, top, right-left+1, bottom-top+1
    def clear(self):
        self.data = b""
    def resize(self, height, width):
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", height, width, 0, 0))
        self.screen.resize(height, width)
    def close(self, external=False):
        if self.process.poll() is None:
            if external:
                self.process.send_signal(signal.SIGTERM)
            else:
                self.send(b"q")
        try:
            assert self.process.wait(timeout=5) == 0
            assert termios.tcgetattr(self.slave) == self.original, "terminal was not restored"
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait()
            os.close(self.master)
            os.close(self.slave)

fixture = subprocess.Popen(
    ["bash", "-c", '"$1" "$2" "porttop-fixture-ESC-\033[31m" & wait', "fixture-wrapper", sys.executable, str(ROOT / "tests/fixture.py")],
    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
term = None
try:
    assert select.select([fixture.stdout], [], [], 5)[0], "fixture startup timed out"
    info = json.loads(fixture.stdout.readline())
    pid, child, ports = info["pid"], info["child"], info["ports"]
    data = snapshot()
    assert data["unavailable_tables"] == 0
    assert data["scope"] == ("all" if os.geteuid() == 0 else "current-user")
    assert data["viewer_uid"] == os.geteuid() and data["system"]["cpu_count"] > 0
    rows = data["rows"]
    if os.geteuid() != 0:
        assert all(r["process"] and r["process"]["uid"] == os.geteuid() for r in rows)

    assert [r["port"] for r in rows] == sorted(r["port"] for r in rows)
    for proto, port in ports.items():
        matches = [r for r in rows if r["port"] == port and r["protocol"] == proto]
        assert len(matches) == 2, matches
        assert {r["process"]["pid"] for r in matches} == {pid, child}
        for r in matches:
            assert r["address"] == ("::1" if "6" in proto else "127.0.0.1")
            assert "fixture.py" in r["process"]["command"]
            assert "\x1b" not in r["process"]["command"]
            assert r["process"]["cwd"] == str(ROOT)
            assert r["process"]["start_ticks"] > 0
    assert next(r["process"]["ppid"] for r in rows if r["process"] and r["process"]["pid"] == pid) == fixture.pid
    filtered = snapshot("--port", ports["tcp"], "--filter", "fixture.py")["rows"]
    assert len(filtered) == 2
    assert not snapshot("--filter", "porttop-no-such-process-token")["rows"]
    rev = snapshot("--reverse")["rows"]
    assert [r["port"] for r in rev] == sorted((r["port"] for r in rev), reverse=True)
    by_pid = snapshot("--sort", "pid")["rows"]
    ids = [r["process"]["pid"] if r["process"] else 2147483647 for r in by_pid]
    assert ids == sorted(ids)
    client = socket.create_connection(("127.0.0.1", ports["tcp"]))
    try:
        assert not any(r["state"] == "ESTAB" for r in snapshot("-p", client.getsockname()[1])["rows"])
        assert any(r["state"] == "ESTAB" for r in snapshot("--all", "-p", client.getsockname()[1])["rows"])
    finally:
        client.close()
    plain = subprocess.run([BIN, "--once", "-p", str(ports["tcp"])], capture_output=True, text=True, check=True)
    assert "fixture.py" in plain.stdout and "\x1b" not in plain.stdout
    for args in [["--port", "0"], ["--port", "65536"], ["--interval", "99"], ["--sort", "bogus"], ["extra"]]:
        assert subprocess.run([BIN, *args], capture_output=True).returncode == 2
    print("PASS live sockets: IPv4/IPv6, TCP/UDP, shared owners, duplicate FDs, wrapper PPID, command, filter, sort, all mode")

    # Select one owner by PID, cancel TERM first, then confirm. Other owner survives.
    term = Terminal("-p", ports["tcp"], "-f", pid, "-i", 100)
    term.until(b"PROCESS DETAILS")
    term.until(b"CPU HISTORY")
    term.until(b"MEM HISTORY")
    term.until(f"* PID {pid}".encode())
    term.until(b"PARENTS")
    assert f"{fixture.pid} (bash)" in term.screen.text
    term.send(b" ")
    term.until(b"PAUSED")
    term.drain()
    term.clear()
    term.drain(.4)
    assert not term.data, "paused unchanged screen emitted output"
    term.send(b" ")
    term.until(b"LIVE")
    term.drain()
    term.clear()
    term.drain(.4)
    assert b"\x1b[2J" not in term.data and b"\x1b[H\x1b[J" not in term.data, "live refresh cleared the screen"
    term.clear()
    term.send(b"?")
    term.until(b"KEYBOARD HELP")
    term.send(b"?")
    term.clear()
    term.send(b"/no-such-porttop-service")
    term.until(b"No matching ports")
    term.until(f"FILTER > {pid}no-such-porttop-service".encode())
    term.send(b"\x1b")
    term.clear()
    term.until(f"Filter: {pid}".encode())
    term.resize(16, 60)
    term.until(b"terminal too small")
    term.resize(36, 130)
    term.clear()
    term.until(b"PROCESS DETAILS")
    term.clear()
    term.send(b"k")
    term.until(b"TERMINATE PROCESS")
    term.send(b"i")
    term.until(b"PROCESS INFORMATION")
    term.until(b"fixture.py")
    term.send(b"\x1bOF")
    term.until(b"END OF DETAILS")
    term.send(b"\x1b")
    term.drain(.1)
    term.until(b"TERMINATE PROCESS")
    term.resize(16, 60)
    term.until(b"terminal too small")
    term.send(b"y") # confirmation must be disabled when popup controls are hidden
    term.drain(.1)
    assert any(r["process"] and r["process"]["pid"] == pid for r in snapshot("-p", ports["tcp"])["rows"])
    term.resize(36, 130)
    term.until(b"TERMINATE PROCESS")
    term.send(b"\r") # Cancel is the initial focused button
    term.until(b"Signal cancelled")
    assert any(r["process"] and r["process"]["pid"] == pid for r in snapshot("-p", ports["tcp"])["rows"])
    term.clear()
    term.send(b"k")
    term.until(b"TERMINATE PROCESS")
    term.send(b"y")
    term.until(f"SIGTERM sent to PID {pid}".encode())
    term.close()
    term = None
    remaining = snapshot("-p", ports["tcp"])["rows"]
    assert {r["process"]["pid"] for r in remaining if r["process"]} == {child}
    term = Terminal("-p", ports["tcp"], "-f", child, "-i", 100)
    term.until(b"PROCESS DETAILS")
    term.send(b"K")
    term.until(b"SIGKILL")
    term.send(b"y")
    term.until(f"SIGKILL sent to PID {child}".encode())
    term.close(external=True)
    term = None
    assert not snapshot("-p", ports["tcp"])["rows"]
    term = Terminal("--port", ports["tcp"])
    term.until(b"PROCESS DETAILS")
    term.drain(.1)
    term.process.send_signal(signal.SIGTSTP)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        stopped, status = os.waitpid(term.process.pid, os.WUNTRACED | os.WNOHANG)
        if stopped:
            assert os.WIFSTOPPED(status)
            break
        term.drain(.02)
    else:
        raise AssertionError("suspend timed out")
    assert termios.tcgetattr(term.slave) == term.original
    term.clear()
    term.process.send_signal(signal.SIGCONT)
    term.until(b"PROCESS DETAILS")
    term.send(b"\x03")
    term.close()
    term = None
    print("PASS PTY: layout, parent chain, pause, help, filter cancel, resize, TERM cancel/confirm, KILL, suspend/resume, Ctrl-C, terminal restoration")
finally:
    try:
        if term:
            term.close(external=True)
    finally:
        try:
            os.killpg(fixture.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        fixture.wait(timeout=5)

# Reproduce a self-restarting service beneath a launcher with multiple services.
launcher = subprocess.Popen([sys.executable, str(ROOT / "tests/supervisor_fixture.py")],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
term = None
try:
    assert select.select([launcher.stdout], [], [], 5)[0], "supervisor fixture startup timed out"
    info = json.loads(launcher.stdout.readline())
    port = info["port"]
    term = Terminal("-p", port, "-i", 100)
    term.until(f"PID {info['worker']} ".encode())
    term.send(b"k")
    term.until(b"TERMINATE PROCESS")
    term.until(b"[ Process only ]")
    term.until(b"[ Terminate ]")
    term.drain(.1)
    before = term.screen.text
    term.drain(.3)
    assert term.screen.text == before, "popup moved while awaiting confirmation"
    term.send(b"y")
    term.until(f"SIGTERM sent to PID {info['worker']}".encode())
    deadline = time.monotonic() + 5
    replacement = None
    while time.monotonic() < deadline:
        owners = [r["process"] for r in snapshot("-p", port)["rows"] if r["process"]]
        replacement = next((p for p in owners if p["pid"] != info["worker"]), None)
        if replacement: break
        time.sleep(.02)
    assert replacement and replacement["ppid"] == info["supervisor"], "fixture must actually restart the killed worker"
    term.until(f"PID {replacement['pid']} ".encode())
    term.send(b"k")
    term.until(b"TERMINATE PROCESS")
    term.send(b"\x1bOA") # xterm application-mode Up: choose immediate supervisor
    term.until(f"target PID {info['supervisor']}".encode())
    term.until(b"[ Subtree ]")
    term.until(b"Affected: 3 processes")
    term.until(str(info["idle"]).encode()) # no-port descendant is also reviewed
    assert str(info["sibling"]) not in term.screen.text.split("Affected:", 1)[1]
    term.send(b"\t\r") # focus Terminate, then Enter
    term.until(b"SIGTERM subtree:")
    deadline = time.monotonic() + 5
    while snapshot("-p", port)["rows"] and time.monotonic() < deadline:
        time.sleep(.03)
    assert not snapshot("-p", port)["rows"], "supervisor branch must release port"
    time.sleep(.15)
    assert not snapshot("-p", port)["rows"], "port must stay closed after supervisor termination"
    assert {r["process"]["pid"] for r in snapshot("-p", info["sibling_port"])["rows"] if r["process"]} == {info["sibling"]}
    assert launcher.poll() is None, "other services in launching script must survive branch termination"
    term.close()
    term = Terminal("-p", info["sibling_port"], "-i", 100)
    term.until(b"PROCESS DETAILS")
    term.send(b"k")
    term.until(b"TERMINATE PROCESS")
    term.send(b"\x1bOA")
    term.until(f"target PID {info['root']}".encode())
    term.until(b"[ Subtree ]")
    term.until(b"Affected: 2 processes")
    term.send(b"y")
    term.until(b"SIGTERM subtree:")
    term.close()
    term = None
    assert launcher.wait(timeout=5) == 0
    assert not snapshot("-p", info["sibling_port"])["rows"]
    print("PASS supervisor: confirmed worker restart, parent subtree stops restart and idle worker, sibling isolated, explicit launcher subtree stops all")
finally:
    try:
        if term: term.close(external=True)
    finally:
        try: os.killpg(launcher.pid, signal.SIGKILL)
        except ProcessLookupError: pass
        launcher.wait(timeout=5)

# Keep selected identity at the same screen row when earlier ports disappear.
owned_sockets = []
term = None
try:
    for _ in range(25):
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        sock.listen()
        owned_sockets.append(sock)
    owned_sockets.sort(key=lambda sock: sock.getsockname()[1])
    selected_port = owned_sockets[20].getsockname()[1]
    term = Terminal("--filter", os.getpid(), "--interval", 100)
    term.until(b"25 rows")
    term.send(b"\x1bOB" * 20)
    marker = f">{selected_port:5d}"
    term.until(marker.encode())
    term.drain(.1)
    original_row = next(i for i, line in enumerate(term.screen.text.splitlines()) if marker in line)
    for sock in owned_sockets[:5]: sock.close()
    term.until(b"20 rows")
    term.drain(.1)
    current_row = next(i for i, line in enumerate(term.screen.text.splitlines()) if marker in line)
    assert original_row == current_row, (original_row, current_row)
    term.close()
    term = None
    print("PASS stability: no paused output, no live full-screen clears, frozen popup, selection screen row survives port churn")
finally:
    if term: term.close(external=True)
    for sock in owned_sockets: sock.close()

# Dynamic CPU and resident memory must actually change in the selected series.
load = subprocess.Popen([sys.executable, str(ROOT / "tests/load_fixture.py")],
                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, start_new_session=True)
term = None
try:
    assert select.select([load.stdout], [], [], 5)[0]
    info = json.loads(load.stdout.readline())
    term = Terminal("--port", info["port"], "--interval", 100)
    term.until(f"* PID {load.pid}".encode())
    term.until(b"MEM HISTORY")
    term.drain(.3)
    before = term.screen.text
    rss_before = next(r["process"]["rss_bytes"] for r in snapshot("-p", info["port"])["rows"] if r["process"] and r["process"]["pid"] == load.pid)
    load.stdin.write("load\n"); load.stdin.flush()
    assert select.select([load.stdout], [], [], 5)[0]
    assert load.stdout.readline().strip() == "loaded"
    term.drain(.6)
    after = term.screen.text
    rss_after = next(r["process"]["rss_bytes"] for r in snapshot("-p", info["port"])["rows"] if r["process"] and r["process"]["pid"] == load.pid)
    assert rss_after-rss_before >= 40*1024*1024
    cpu = re.search(r"TOTAL[^|]+\| PID ([0-9.]+)%", after)
    assert cpu and float(cpu.group(1))>0, after
    assert before.splitlines()[2] != after.splitlines()[2], (before, after)
    core_title = next(line for line in after.splitlines() if "CORES" in line)
    plot_right = core_title.index("CORES")-2
    legend_row = next(i for i, line in enumerate(after.splitlines()) if "* PID" in line)
    assert any(0x2800 <= ord(ch) <= 0x28ff and (ord(ch)-0x2800)&0xb8
               for line in after.splitlines()[3:legend_row-1] for ch in line[3:plot_right]), "process CPU curve must rise above baseline"
    term.send(b"g")
    term.until(b"CPU scaled to")
    term.send(b"g")
    term.until(b"axes: CPU")
    labels_before = re.search(r"CORES (\d+)-(\d+)/(\d+)", term.screen.text).groups()
    term.send(b"c")
    term.drain(.15)
    if int(labels_before[1]) < info["cores"]:
        assert labels_before != re.search(r"CORES (\d+)-(\d+)/(\d+)", term.screen.text).groups()
    term.resize(24, 80)
    term.drain(.3)
    term.until(b"CPU HISTORY")
    term.until(b"MEM HISTORY")
    term.until(b"PROCESS DETAILS")
    term.close()
    term = None
    print("PASS charts: UID default, system/selected history, actual CPU/RSS change, core paging, compact layout")
finally:
    if term: term.close(external=True)
    if load.poll() is None: load.terminate()
    load.wait(timeout=5)

# Panels, adaptive columns, the 256-color theme and the C-locale fallback.
for locale, color in [("C.UTF-8", True), ("C", False)]:
    term = Terminal(locale=locale, color=color)
    try:
        term.until(b"Filter:")
        term.until(b"CPU HISTORY")
        term.until(b"MEM HISTORY")
        term.until(b"PROCESS DETAILS")
        if color: assert b"\x1b[38;5;" in term.data
        for rows, cols in [(31, 107), (24, 80), (36, 130)]:
            term.resize(rows, cols)
            term.drain(.3)
            text = term.screen.text
            for title in ["CPU HISTORY", "MEM HISTORY", "PORTS", "PROCESS DETAILS"]:
                assert title in text, (rows, cols, text)
            if locale != "C": assert text.count("╭") == 4 and text.count("╰") == 4
            else: assert "╭" not in text and "+" in text
            assert "arrows" in text and "terminate" in text
        term.send(b"?")
        term.until(b"KEYBOARD HELP")
        term.send(b"?")
    finally:
        term.close()
print("PASS presentation: framed panels, 80/107/130 columns, UTF-8 and ASCII, color and monochrome")

# A long real argv must be wrapped, fully reachable and safe in a fixed modal.
long_command = " ".join(f"DETAIL_ARG_{i:04d}" for i in range(320)) + " DETAIL_CMD_TAIL"
for locale in ["C.UTF-8", "C"]:
    service = subprocess.Popen([sys.executable, str(ROOT / "tests/detail_fixture.py"), long_command],
                               stdout=subprocess.PIPE, text=True, start_new_session=True)
    term = None
    try:
        assert select.select([service.stdout], [], [], 5)[0]
        detail_port = json.loads(service.stdout.readline())["port"]
        term = Terminal("-p", detail_port, "-i", 100, locale=locale)
        term.until(b"PROCESS DETAILS")
        term.send(b"\r")
        term.until(b"PROCESS INFORMATION")
        term.until(b"read-only snapshot")
        term.until(b"DETAIL_ARG_0000")
        term.drain(.1)
        before = term.info_progress()
        if locale != "C": bounds = term.info_bounds()
        assert b"1006" in term.data and b"1000h" in term.data, "mouse reporting was not enabled"
        frozen = term.screen.text
        term.drain(.3)
        assert term.screen.text == frozen, "detail snapshot must not move while reading"
        term.wheel(x=0,y=0) # outside the popup must not scroll the background or details
        term.drain(.1)
        assert term.info_progress() == before
        term.wheel()
        term.drain(.1)
        assert term.info_progress()[0] == before[0]+3
        term.wheel(down=False)
        term.drain(.1)
        assert term.info_progress()[0] == before[0]
        # Actual wheel events traverse the command until its final argv bytes appear.
        for _ in range(60):
            if "DETAIL_CMD_TAIL" in term.screen.text: break
            term.wheel(); term.drain(.02)
        term.until(b"DETAIL_CMD_TAIL")
        if locale != "C": assert term.info_bounds() == bounds
        term.send(b"\x1bOF")
        term.until(b"END OF DETAILS")
        end = term.info_progress()
        for _ in range(3): term.wheel()
        term.drain(.1)
        assert term.info_progress() == end, "wheel must stop at document end"
        term.send(b"\x1bOH")
        term.until(b"DETAIL_ARG_0000")
        assert term.info_progress()[0] == 1
        term.send(b"\x1b[6~")
        term.drain(.1)
        assert term.info_progress()[0] > 1
        if locale != "C":
            term.resize(44, 150); term.drain(.3)
            assert term.info_bounds()[2:] == bounds[2:], "larger terminals must not enlarge the modal"
            term.resize(24, 80); term.drain(.3)
            assert term.info_bounds()[2:] == (76,22)
            term.send(b"\x1bOF"); term.until(b"END OF DETAILS")
            term.resize(16,60); term.until(b"terminal too small")
            term.resize(36,130); term.drain(.3); term.until(b"PROCESS INFORMATION")
        if locale == "C.UTF-8":
            term.drain(.1)
            term.process.send_signal(signal.SIGTSTP)
            deadline = time.monotonic()+5
            while time.monotonic()<deadline:
                stopped,status = os.waitpid(term.process.pid, os.WUNTRACED | os.WNOHANG)
                if stopped:
                    assert os.WIFSTOPPED(status)
                    break
                term.drain(.02)
            else: raise AssertionError("details suspend timed out")
            assert termios.tcgetattr(term.slave) == term.original
            term.clear()
            term.process.send_signal(signal.SIGCONT)
            term.drain(.3)
            assert b"1000h" in term.data, "mouse mode must resume with the viewer"
            term.send(b"\x1bOH"); term.until(b"DETAIL_ARG_0000")
            term.wheel(); term.drain(.1)
            assert term.info_progress()[0] == 4
        term.send(b"ky") # read-only: these cannot open or confirm a kill
        term.drain(.1)
        assert service.poll() is None and "PROCESS INFORMATION" in term.screen.text
        service.terminate(); service.wait(timeout=5)
        term.drain(.15)
        assert "PROCESS INFORMATION" in term.screen.text, "exited PID must not replace the captured detail identity"
        term.send(b"\x1b"); term.drain(.3)
        term.until(b"No matching ports")
        assert "PROCESS INFORMATION" not in term.screen.text
        assert b"1000l" in term.data, "mouse capture must be released on close"
        term.close(); term = None
    finally:
        if term: term.close(external=True)
        if service.poll() is None: service.terminate()
        service.wait(timeout=5)
print("PASS info: Enter/i, full long argv, real mouse wheel, page/home/end, fixed geometry, resize, snapshot identity, read-only safety, ASCII fallback")

service = subprocess.Popen([sys.executable, str(ROOT / "tests/detail_fixture.py")], stdout=subprocess.PIPE, text=True)
term = None
try:
    assert select.select([service.stdout], [], [], 5)[0]
    port = json.loads(service.stdout.readline())["port"]
    term = Terminal("-p", port)
    term.until(b"PROCESS DETAILS"); term.send(b"i"); term.until(b"PROCESS INFORMATION")
    term.process.terminate(); assert term.process.wait(timeout=5) == 0
    term.drain(.1)
    assert b"1000l" in term.data, "external termination must release mouse mode"
    term.close(); term = None
finally:
    if term: term.close(external=True)
    service.terminate(); service.wait(timeout=5)
