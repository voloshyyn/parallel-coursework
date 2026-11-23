#!/usr/bin/env python3
"""Serve HTTP immediately while preparing the corpus and index; supervise children."""
from __future__ import annotations

import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
from urllib.error import URLError
from urllib.request import Request, urlopen


def enabled(name: str, default: str = "true") -> bool:
    return os.environ.get(name, default).lower() in {"1", "true", "yes"}


def write_status(path: Path, state: str, error: str = "") -> None:
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps({"state": state, "error": error}), encoding="utf-8")
    temporary.replace(path)


def stop_child(child: subprocess.Popen | None) -> None:
    if child is not None and child.poll() is None:
        child.terminate()
        try:
            child.wait(timeout=10)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()


def main() -> int:
    root = Path(__file__).resolve().parent
    profile = os.environ.get("DATASET_PROFILE", "full")
    data_dir = os.environ.get("DATA_DIR", f"/data/generated/{profile}")
    status_path = Path(os.environ.get("STARTUP_STATUS_FILE", "/tmp/search-startup.json"))
    port = int(os.environ.get("PORT", "8080"))
    threads = int(os.environ.get("INDEX_THREADS", "4"))
    auto_index = enabled("AUTO_INDEX")
    server_env = dict(os.environ, DATA_DIR=data_dir, AUTO_INDEX="false",
                      STARTUP_STATUS_FILE=str(status_path))
    server = None
    generator = None

    def shutdown(signum, _frame):
        raise SystemExit(128 + signum)

    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)
    try:
        write_status(status_path, "preparing")
        server = subprocess.Popen([os.environ.get("SEARCH_SERVER", "/app/search_server")],
                                  env=server_env)
        try:
            if enabled("GENERATE_DATASET"):
                command = [sys.executable, str(root / "generate_dataset.py"),
                           "--output", data_dir, "--profile", profile]
                if os.environ.get("DATASET_SEED"):
                    command += ["--seed", os.environ["DATASET_SEED"]]
                if enabled("DATASET_VERIFY_HASH", "false"):
                    command.append("--verify-hash")
                generator = subprocess.Popen(command)
                while generator.poll() is None:
                    if server.poll() is not None:
                        raise RuntimeError(f"server exited with {server.returncode}")
                    time.sleep(0.1)
                if generator.returncode:
                    raise RuntimeError(f"dataset preparation failed (exit {generator.returncode}); see container logs")

            if auto_index:
                write_status(status_path, "indexing")
                base = f"http://127.0.0.1:{port}"
                for attempt in range(300):
                    if server.poll() is not None:
                        raise RuntimeError(f"server exited with {server.returncode}")
                    try:
                        with urlopen(base + "/api/health", timeout=1) as response:
                            if response.status == 200:
                                break
                    except (OSError, URLError):
                        time.sleep(0.1)
                else:
                    raise RuntimeError("HTTP server did not become available")
                request = Request(base + "/api/index",
                                  data=json.dumps({"threads": threads}).encode(),
                                  headers={"Content-Type": "application/json"})
                with urlopen(request, timeout=3600) as response:
                    result = json.load(response)
                if result.get("status") != "ok" or result.get("documents_indexed", 0) < 1:
                    raise RuntimeError(f"automatic indexing did not produce documents: {result}")
                print(f"startup: index ready: {result}", flush=True)
            write_status(status_path, "ready")
        except Exception as error:
            write_status(status_path, "error", str(error))
            print(f"startup: {error}", file=sys.stderr, flush=True)
            # Keep HTTP and its diagnostic page alive after preparation fails.
        return server.wait()
    finally:
        stop_child(generator)
        stop_child(server)


if __name__ == "__main__":
    sys.exit(main())
