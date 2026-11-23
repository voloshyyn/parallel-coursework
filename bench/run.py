#!/usr/bin/env python3
"""Measure the real REST server; retain raw runs, failures and environment.

Use a separate server with QUERY_EXPANSION=false to isolate keyword throughput.
The normal application keeps expansion enabled. No performance values are faked.
"""
import argparse
import csv
from datetime import datetime, timezone
import json
from pathlib import Path
import platform
from queue import Empty, Queue
import statistics
import subprocess
import threading
import time
from urllib.request import Request, urlopen

QUERIES = ["coral reef", "quantum physics", "ancient rome", "deep ocean whale",
           "galaxy telescope", "forest river", "algorithm data"]


def request(base, path, body=None, timeout=600):
    req = Request(base + path, data=None if body is None else json.dumps(body).encode(),
                  headers={"Content-Type": "application/json"})
    with urlopen(req, timeout=timeout) as response:
        return json.load(response)


def load(base, clients, count):
    work = Queue()
    for i in range(count):
        work.put(QUERIES[i % len(QUERIES)])
    gate = threading.Barrier(clients + 1)
    lock = threading.Lock()
    latencies, errors = [], []

    def worker():
        gate.wait()
        while True:
            try:
                query = work.get_nowait()
            except Empty:
                return
            started = time.perf_counter()
            try:
                body = request(base, "/api/search", {"query": query, "mode": "keyword"}, 60)
                if not isinstance(body.get("results"), list):
                    raise ValueError("invalid response")
            except Exception as error:
                with lock:
                    errors.append(str(error))
            finally:
                with lock:
                    latencies.append((time.perf_counter() - started) * 1000)

    threads = [threading.Thread(target=worker) for _ in range(clients)]
    for thread in threads:
        thread.start()
    started = time.perf_counter()
    gate.wait()
    for thread in threads:
        thread.join()
    elapsed = time.perf_counter() - started
    ordered = sorted(latencies)
    return {"clients": clients, "requests": count, "errors": len(errors),
            "elapsed_s": elapsed, "throughput_rps": (count - len(errors)) / elapsed,
            "p50_ms": statistics.median(ordered),
            "p95_ms": ordered[min(len(ordered) - 1, int(len(ordered) * .95))]}, errors


def command(*args):
    return subprocess.check_output(args, text=True, encoding="utf-8").strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", default="http://localhost:18090")
    parser.add_argument("--container", default="coursework-bench")
    parser.add_argument("--output", type=Path, default=Path("bench/results"))
    parser.add_argument("--threads", default="1,2,4,8,12,16")
    parser.add_argument("--clients", default="1,2,4,8,16,32,64")
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--requests", type=int, default=256)
    parser.add_argument("--disk-info", type=Path,
                        help="optional JSON file with host disk model/type/capacity")
    args = parser.parse_args()
    if args.repetitions < 1 or args.requests < 1:
        parser.error("repetitions and requests must be positive")
    thread_counts = [int(value) for value in args.threads.split(",")]
    client_counts = [int(value) for value in args.clients.split(",")]
    if any(value < 1 or value > 64 for value in thread_counts + client_counts):
        parser.error("threads and clients must be in 1..64")
    args.output.mkdir(parents=True, exist_ok=True)
    base = args.server.rstrip("/")
    if not request(base, "/api/ready")["search_ready"]:
        raise RuntimeError("server is not ready")
    inspect = json.loads(command("docker", "inspect", args.container))[0]
    environment = dict(value.split("=", 1) for value in inspect["Config"]["Env"])
    if environment.get("QUERY_EXPANSION", "true").lower() != "false":
        raise RuntimeError("benchmark requires a separate QUERY_EXPANSION=false server")
    metadata = {
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "client_platform": platform.platform(), "python": platform.python_version(),
        "source_commit": command("git", "rev-parse", "HEAD"),
        "working_tree": command("git", "status", "--porcelain"),
        "docker_version": command("docker", "version", "--format", "{{.Server.Version}}"),
        "docker_cpus": int(command("docker", "info", "--format", "{{.NCPU}}")),
        "docker_memory_bytes": int(command("docker", "info", "--format", "{{.MemTotal}}")),
        "cpu_model": command("docker", "exec", args.container, "sh", "-c",
                             "sed -n 's/^model name[[:space:]]*: //p' /proc/cpuinfo | head -1"),
        "kernel": command("docker", "exec", args.container, "uname", "-a"),
        "container_image": inspect["Image"],
        "server_binary_sha256": command("docker", "exec", args.container,
                                         "sha256sum", "/app/search_server").split()[0],
        "disk": json.loads(args.disk_info.read_text(encoding="utf-8-sig")) if args.disk_info else None,
        "container_limits": {key: inspect["HostConfig"][key]
                             for key in ["Memory", "NanoCpus", "CpusetCpus"]},
        "query_expansion": False, "queries": QUERIES,
        "method": "One unrecorded warmup per point; repeated warm-cache runs; HTTP connection per request",
        "options": {key: str(value) for key, value in vars(args).items()},
    }
    data_dir = environment["DATA_DIR"]
    metadata["dataset"] = json.loads(command("docker", "exec", args.container,
                                              "cat", data_dir + "/manifest.json"))
    if metadata["dataset"]["documents"] < 10000 or metadata["dataset"]["bytes"] < 2 * 1024**3:
        raise RuntimeError("scalability measurements require the full corpus")
    (args.output / "environment.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    with (args.output / "e1.csv").open("w", newline="", encoding="utf-8") as file:
        writer = csv.DictWriter(file, fieldnames=["threads", "run", "documents", "time_ms", "wall_ms"])
        writer.writeheader()
        for threads in thread_counts:
            request(base, "/api/index", {"threads": threads})
            for run in range(1, args.repetitions + 1):
                started = time.perf_counter()
                result = request(base, "/api/index", {"threads": threads})
                row = {"threads": threads, "run": run, "documents": result["documents_indexed"],
                       "time_ms": result["time_ms"], "wall_ms": (time.perf_counter() - started) * 1000}
                if row["documents"] != metadata["dataset"]["documents"]:
                    raise RuntimeError("document count changed")
                writer.writerow(row)
                file.flush()
                print("index:", row, flush=True)
    with (args.output / "e2.csv").open("w", newline="", encoding="utf-8") as file:
        fields = ["clients", "run", "requests", "errors", "elapsed_s", "throughput_rps", "p50_ms", "p95_ms"]
        writer = csv.DictWriter(file, fieldnames=fields)
        writer.writeheader()
        for clients in client_counts:
            _, errors = load(base, clients, args.requests)
            if errors:
                raise RuntimeError(errors)
            for run in range(1, args.repetitions + 1):
                row, errors = load(base, clients, args.requests)
                row["run"] = run
                writer.writerow(row)
                file.flush()
                print("load:", row, flush=True)
                if errors:
                    raise RuntimeError(errors)
    cases = [(query, True) for query in QUERIES[:5]] + [("zzzzunknowntermxyz", False)]
    checks = []
    for query, expect_hits in cases:
        body = request(base, "/api/search", {"query": query, "mode": "keyword"})
        checks.append({"query": query, "expected": "nonempty" if expect_hits else "empty",
                       "actual_count": len(body["results"]), "results": body["results"],
                       "passed": bool(body["results"]) == expect_hits})
    (args.output / "search_cases.json").write_text(json.dumps(checks, indent=2), encoding="utf-8")
    if not all(case["passed"] for case in checks):
        raise RuntimeError("search cases failed")
    print("All measurements and search cases completed.", flush=True)


if __name__ == "__main__":
    main()
