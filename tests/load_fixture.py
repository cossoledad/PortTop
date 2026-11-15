"""Controlled memory growth and CPU work for the graph acceptance test."""
import json
import os
import socket
import sys
import time

listener = socket.socket()
listener.bind(("127.0.0.1", 0))
listener.listen()
print(json.dumps({"port": listener.getsockname()[1], "cores": os.cpu_count()}), flush=True)
sys.stdin.readline()
allocation = bytearray(48 * 1024 * 1024)
for i in range(0, len(allocation), 4096):
    allocation[i] = 1
print("loaded", flush=True)
while True:
    until = time.monotonic() + .075
    while time.monotonic() < until:
        pass
    time.sleep(.025)
