"""Launcher -> restarting supervisor -> service + idle worker; sibling service.
The test owns this whole session. No host service is touched.
"""
import json
import signal
import socket
import subprocess
import sys
import time

running = True

def stop(_sig, _frame):
    global running
    running = False

def spawn(mode, *args):
    return subprocess.Popen([sys.executable, __file__, mode, *map(str, args)], stdout=subprocess.PIPE, text=True)

def service(port=0):
    p = spawn("--worker", port)
    line = p.stdout.readline()
    if not line:
        raise RuntimeError("service did not start")
    return p, json.loads(line)

def cleanup(processes):
    for p in processes:
        if p.poll() is None:
            p.terminate()
    for p in processes:
        try:
            p.wait(timeout=3)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()

if len(sys.argv) > 1 and sys.argv[1] == "--worker":
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", int(sys.argv[2])))
    s.listen()
    print(json.dumps({"port": s.getsockname()[1]}), flush=True)
    while True:
        time.sleep(1)
elif len(sys.argv) > 1 and sys.argv[1] == "--idle":
    while True:
        time.sleep(1)
elif len(sys.argv) > 1 and sys.argv[1] == "--supervisor":
    signal.signal(signal.SIGTERM, stop)
    worker, info = service()
    idle = spawn("--idle")
    print(json.dumps({"port": info["port"], "worker": worker.pid, "idle": idle.pid}), flush=True)
    try:
        while running:
            if worker.poll() is not None:
                worker, _ = service(info["port"])
            time.sleep(.02)
    finally:
        cleanup([worker, idle])
else:
    signal.signal(signal.SIGTERM, stop)
    supervisor = spawn("--supervisor")
    info = json.loads(supervisor.stdout.readline())
    sibling, extra = service()
    info.update(root=__import__("os").getpid(), supervisor=supervisor.pid, sibling=sibling.pid, sibling_port=extra["port"])
    print(json.dumps(info), flush=True)
    try:
        while running:
            time.sleep(.02)
    finally:
        cleanup([supervisor, sibling])
