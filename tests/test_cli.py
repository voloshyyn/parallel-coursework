#!/usr/bin/env python3
"""Exercise the compiled CLI against a real local HTTP fixture."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import subprocess
import threading
import unittest

parser = argparse.ArgumentParser(add_help=False)
parser.add_argument("--binary", required=True)
args, remaining = parser.parse_known_args()
BINARY = str(Path(args.binary).resolve())


class CliTest(unittest.TestCase):
    def setUp(self):
        self.response = {"results": [{"doc_id": "a.txt", "score": 1.0,
                                      "snippet": "An ocean whale."}], "time_ms": 3}
        self.status = 200
        self.request = None
        case = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                case.request = (self.path, json.loads(self.rfile.read(int(self.headers["Content-Length"]))))
                self.send_response(case.status)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps(case.response).encode())

            def log_message(self, *_args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.start()
        self.base = f"http://127.0.0.1:{self.server.server_port}"

    def tearDown(self):
        self.server.shutdown()
        self.thread.join()
        self.server.server_close()

    def invoke(self, *arguments):
        return subprocess.run([BINARY, "--server", self.base, *arguments],
                              text=True, encoding="utf-8", capture_output=True, timeout=5)

    def test_human_output_and_request(self):
        result = self.invoke("ocean whale")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("a.txt", result.stdout)
        self.assertIn("An ocean whale.", result.stdout)
        self.assertIn("score", result.stdout)
        self.assertEqual(self.request, ("/api/search", {"query": "ocean whale", "mode": "keyword"}))

    def test_empty_json_is_success(self):
        self.response["results"] = []
        result = self.invoke("--json", "unknown")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout), self.response)

    def test_http_failure_is_not_empty_success(self):
        self.status, self.response = 409, {"message": "index is building"}
        result = self.invoke("whale")
        self.assertEqual(result.returncode, 1)
        self.assertIn("409", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_malformed_result_is_failure(self):
        self.response["results"] = [{"doc_id": "a.txt"}]
        self.assertEqual(self.invoke("whale").returncode, 1)

    def test_missing_query_is_usage_error(self):
        self.assertEqual(self.invoke().returncode, 2)

    def test_unreachable_server_is_failure(self):
        self.server.shutdown()
        self.server.server_close()
        result = self.invoke("whale")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Cannot reach", result.stderr)


if __name__ == "__main__":
    unittest.main(argv=[__file__, *remaining])
