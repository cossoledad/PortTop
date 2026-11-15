"""A real, isolated microservice tree used by integration tests."""
import json
import os
import signal
import socket
import time

sockets = []
ports = {}
for label, family, kind, address in [
    ("tcp", socket.AF_INET, socket.SOCK_STREAM, "127.0.0.1"),
    ("udp", socket.AF_INET, socket.SOCK_DGRAM, "127.0.0.1"),
    ("tcp6", socket.AF_INET6, socket.SOCK_STREAM, "::1"),
    ("udp6", socket.AF_INET6, socket.SOCK_DGRAM, "::1"),
]:
    try:
        s = socket.socket(family, kind)
        if family == socket.AF_INET6:
            s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
        s.bind((address, 0))
        if kind == socket.SOCK_STREAM:
            s.listen()
        ports[label] = s.getsockname()[1]
        sockets.append(s)
    except OSError:
        if family != socket.AF_INET6:
            raise
# One socket, two FDs, two processes. Every owner should appear exactly once.
duplicate = os.dup(sockets[0].fileno())
child = os.fork()
if child == 0:
    while True:
        time.sleep(1)
print(json.dumps({"pid": os.getpid(), "child": child, "ports": ports}), flush=True)
while True:
    time.sleep(1)
