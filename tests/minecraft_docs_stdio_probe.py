#!/usr/bin/env python3
"""MCP contract probe for minecraft_docs_search -> minecraft_docs_read."""
import argparse
import json
import queue
import subprocess
import threading
import time
from pathlib import Path


def send(proc, message):
    proc.stdin.write(json.dumps(message, ensure_ascii=False) + "\n")
    proc.stdin.flush()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True, help="mcdk-assistant executable with adjacent dicts/knowledge or cache")
    args = parser.parse_args()
    proc = subprocess.Popen([args.exe, "--stdio"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, encoding="utf-8", bufsize=1)
    lines = queue.Queue()
    threading.Thread(target=lambda: [lines.put(line) for line in proc.stdout], daemon=True).start()
    send(proc, {"jsonrpc":"2.0", "id":1, "method":"initialize", "params":{"protocolVersion":"2025-03-26", "capabilities":{}, "clientInfo":{"name":"docs-probe","version":"1"}}})
    deadline = time.time() + 60
    while time.time() < deadline:
        try: message = json.loads(lines.get(timeout=.5))
        except (queue.Empty, json.JSONDecodeError): continue
        if message.get("id") == 1: break
    else: raise RuntimeError("initialize timed out")
    send(proc, {"jsonrpc":"2.0", "method":"notifications/initialized", "params":{}})

    def call(req_id, method, params):
        send(proc, {"jsonrpc":"2.0", "id":req_id, "method":method, "params":params})
        while time.time() < deadline:
            try: message = json.loads(lines.get(timeout=.5))
            except (queue.Empty, json.JSONDecodeError): continue
            if message.get("id") == req_id: return message["result"]
        raise RuntimeError(f"{method} timed out")

    tools = call(2, "tools/list", {})["tools"]
    by_name = {tool["name"]: tool for tool in tools}
    for name in ("minecraft_docs_search", "minecraft_docs_read", "minecraft_docs_guide"):
        assert name in by_name and "outputSchema" in by_name[name], name
    result = call(3, "tools/call", {"name":"minecraft_docs_search", "arguments":{"query":"custom food", "corpus":"wiki", "limit":1, "max_chars":800}})
    hit = result["structuredContent"]["hits"][0]
    assert hit["ref"] in result["content"][0]["text"]
    read = call(4, "tools/call", {"name":"minecraft_docs_read", "arguments":{"ref":hit["ref"], "max_chars":800}})
    assert read["isError"] is False and read["structuredContent"]["ref"]
    proc.terminate()
    print("minecraft_docs MCP contract probe: PASS")


if __name__ == "__main__": main()
