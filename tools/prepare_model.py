#!/usr/bin/env python3
"""Idempotently pull and warm the configured Ollama model, with visible logs."""
import json
import os
import sys
import time
from urllib.request import Request, urlopen


def main() -> None:
    base = os.environ.get("OLLAMA_URL", "http://ollama:11434").rstrip("/")
    model = os.environ.get("OLLAMA_MODEL", "qwen2.5:1.5b")
    deadline = time.monotonic() + 180
    while True:
        try:
            with urlopen(base + "/api/tags", timeout=2) as response:
                models = json.load(response)["models"]
            break
        except (OSError, ValueError, KeyError):
            if time.monotonic() >= deadline:
                raise RuntimeError("Ollama did not become available in 180 seconds")
            time.sleep(1)
    available = any(entry.get("name") == model or entry.get("model") == model
                    for entry in models)
    if not available:
        request = Request(base + "/api/pull", data=json.dumps({"name": model}).encode(),
                          headers={"Content-Type": "application/json"})
        previous_status = None
        with urlopen(request, timeout=1800) as response:
            for line in response:
                event = json.loads(line)
                if event.get("error"):
                    raise RuntimeError(event["error"])
                if event.get("status") != previous_status:
                    previous_status = event.get("status")
                    print(f"model: {previous_status}", flush=True)
    request = Request(base + "/api/generate", data=json.dumps({
        "model": model, "prompt": "", "stream": False, "keep_alive": "24h",
    }).encode(), headers={"Content-Type": "application/json"})
    with urlopen(request, timeout=180) as response:
        result = json.load(response)
    if result.get("error") or result.get("done") is not True:
        raise RuntimeError(f"model warmup failed: {result}")
    print(f"model: {model} ready (cached for 24h)", flush=True)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"model: {error}", file=sys.stderr, flush=True)
        sys.exit(1)
