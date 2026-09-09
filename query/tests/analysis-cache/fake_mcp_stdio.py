"""Test-only stdio peer; deliberately emits notifications before replies."""
import json
import sys
import time

for line in sys.stdin.buffer:
    request = json.loads(line)
    if "id" not in request:
        continue
    if request["method"] == "hang":
        time.sleep(2)
    if request["method"] == "oversize":
        print(json.dumps({"id": request["id"], "padding": "x"*8192}), flush=True)
        continue
    print(json.dumps({"jsonrpc": "2.0", "method": "notifications/progress", "params": {"progress": 1}}), flush=True)
    print(json.dumps({"jsonrpc": "2.0", "id": request["id"], "result": {"echo": request["params"]}}), flush=True)
