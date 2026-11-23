#!/usr/bin/env python3
"""Verify first-run model preparation, cache reuse and failed downloads."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "prepare_model.py"


class ModelPreparationTest(unittest.TestCase):
    def run_preparation(self, cached, pull_error=False):
        calls = []

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                self.send_response(200)
                self.end_headers()
                models = [{"name": "test:1b"}] if cached else []
                self.wfile.write(json.dumps({"models": models}).encode())

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                calls.append((self.path, body))
                self.send_response(200)
                self.end_headers()
                if self.path == "/api/pull":
                    result = {"error": "download failed"} if pull_error else {"status": "success"}
                else:
                    result = {"done": True}
                self.wfile.write((json.dumps(result) + "\n").encode())

            def log_message(self, *_args):
                pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        listener = threading.Thread(target=server.serve_forever)
        listener.start()
        try:
            result = subprocess.run([sys.executable, str(SCRIPT)], capture_output=True,
                                    text=True, timeout=5, env=dict(os.environ,
                                    OLLAMA_URL=f"http://127.0.0.1:{server.server_port}",
                                    OLLAMA_MODEL="test:1b"))
        finally:
            server.shutdown()
            listener.join()
            server.server_close()
        return result, calls

    def test_first_run_pulls_then_warms(self):
        result, calls = self.run_preparation(False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([path for path, _ in calls], ["/api/pull", "/api/generate"])
        self.assertEqual(calls[1][1]["keep_alive"], "24h")

    def test_cached_model_only_warms(self):
        result, calls = self.run_preparation(True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([path for path, _ in calls], ["/api/generate"])

    def test_pull_failure_is_nonzero_and_does_not_warm(self):
        result, calls = self.run_preparation(False, True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("download failed", result.stderr)
        self.assertEqual([path for path, _ in calls], ["/api/pull"])


if __name__ == "__main__":
    unittest.main()
