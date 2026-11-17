"""A listener carrying an intentionally long argv for scrollable detail acceptance."""
import json
import socket
import time

listener = socket.socket()
listener.bind(("127.0.0.1", 0))
listener.listen()
print(json.dumps({"port": listener.getsockname()[1]}), flush=True)
while True:
    time.sleep(1)
