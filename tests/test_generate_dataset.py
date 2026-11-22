"""Behavioural tests for tools/generate_dataset.py.

Run directly (``python tests/test_generate_dataset.py``) or through CTest.
Only the standard library is used, like the generator itself.
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
GENERATOR = REPO_ROOT / "tools" / "generate_dataset.py"
TWO_GIB = 2 * 1024**3


def run_generator(*arguments: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(GENERATOR), *arguments],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )


def tokens(text: str) -> set[str]:
    return set(re.findall(r"[a-z0-9]+", text.lower()))


class DatasetGeneratorTest(unittest.TestCase):
    def test_full_plan_meets_the_volume_requirement(self) -> None:
        result = run_generator("--output", "ignored", "--profile", "full",
                               "--plan")
        self.assertEqual(0, result.returncode, result.stderr)
        plan = json.loads(result.stdout)
        self.assertGreaterEqual(plan["documents"], 10_000)
        self.assertGreaterEqual(plan["guaranteed_min_total_bytes"], TWO_GIB)
        self.assertGreaterEqual(plan["required_total_bytes"], TWO_GIB)

    def test_same_seed_produces_identical_corpora(self) -> None:
        arguments = [
            "--profile", "demo",
            "--documents", "12",
            "--min-bytes", "600",
            "--max-bytes", "1200",
            "--seed", "424242",
        ]
        with tempfile.TemporaryDirectory() as first, \
                tempfile.TemporaryDirectory() as second, \
                tempfile.TemporaryDirectory() as other:
            one = run_generator(*arguments, "--output", first, "--quiet")
            two = run_generator(*arguments, "--output", second, "--quiet")
            self.assertEqual(0, one.returncode, one.stderr)
            self.assertEqual(0, two.returncode, two.stderr)

            manifest_one = json.loads(
                (Path(first) / "manifest.json").read_text(encoding="utf-8")
            )
            manifest_two = json.loads(
                (Path(second) / "manifest.json").read_text(encoding="utf-8")
            )
            self.assertEqual(manifest_one, manifest_two)
            self.assertEqual(manifest_one["seed"], 424242)
            self.assertEqual(manifest_one["documents"], 12)
            self.assertEqual(manifest_one["bytes"],
                             sum(path.stat().st_size
                                 for path in Path(first).glob("doc-*.txt")))

            different = run_generator(
                "--profile", "demo", "--documents", "12",
                "--min-bytes", "600", "--max-bytes", "1200",
                "--seed", "7", "--output", other, "--quiet",
            )
            self.assertEqual(0, different.returncode, different.stderr)
            other_manifest = json.loads(
                (Path(other) / "manifest.json").read_text(encoding="utf-8")
            )
            self.assertNotEqual(manifest_one["content_sha256"],
                                other_manifest["content_sha256"])

    def test_repeated_run_reuses_the_corpus_without_writing(self) -> None:
        arguments = [
            "--profile", "demo", "--documents", "10",
            "--min-bytes", "500", "--max-bytes", "900", "--seed", "99",
        ]
        with tempfile.TemporaryDirectory() as output:
            first = run_generator(*arguments, "--output", output)
            self.assertEqual(0, first.returncode, first.stderr)

            snapshot = {
                path.name: (path.stat().st_size, path.stat().st_mtime_ns)
                for path in Path(output).iterdir()
            }

            second = run_generator(*arguments, "--output", output)
            self.assertEqual(0, second.returncode, second.stderr)
            self.assertIn("reusing dataset", second.stdout)

            again = {
                path.name: (path.stat().st_size, path.stat().st_mtime_ns)
                for path in Path(output).iterdir()
            }
            self.assertEqual(snapshot, again)

    def test_known_queries_are_present(self) -> None:
        arguments = [
            "--profile", "demo", "--documents", "40",
            "--min-bytes", "512", "--max-bytes", "900", "--seed", "5",
        ]
        with tempfile.TemporaryDirectory() as output:
            result = run_generator(*arguments, "--output", output, "--quiet")
            self.assertEqual(0, result.returncode, result.stderr)
            manifest = json.loads(
                (Path(output) / "manifest.json").read_text(encoding="utf-8")
            )
            documents = [
                path.read_text(encoding="utf-8")
                for path in sorted(Path(output).glob("doc-*.txt"))
            ]
            self.assertGreaterEqual(len(documents), 30)

            for query in manifest["known_queries"]:
                wanted = tokens(query)
                self.assertTrue(
                    any(wanted <= tokens(document) for document in documents),
                    f"no generated document contains {query!r}",
                )

    def test_existing_user_data_is_not_deleted(self) -> None:
        with tempfile.TemporaryDirectory() as output:
            user_file = Path(output) / "my-notes.txt"
            user_file.write_text("keep me", encoding="utf-8")

            result = run_generator(
                "--profile", "demo", "--documents", "3",
                "--min-bytes", "200", "--max-bytes", "300",
                "--output", output,
            )

            self.assertEqual(2, result.returncode)
            self.assertTrue(user_file.is_file())
            self.assertEqual("keep me", user_file.read_text(encoding="utf-8"))

    def test_demo_profile_is_small_and_marked_as_a_demo(self) -> None:
        with tempfile.TemporaryDirectory() as output:
            result = run_generator("--profile", "demo",
                                   "--output", output)
            self.assertEqual(0, result.returncode, result.stderr)
            manifest = json.loads(
                (Path(output) / "manifest.json").read_text(encoding="utf-8")
            )
            self.assertEqual("demo", manifest["profile"])
            self.assertLess(manifest["bytes"], 2 * 1024 * 1024)
            self.assertIn("does not meet", result.stderr)

    def test_atomic_layout_has_no_temporary_files(self) -> None:
        with tempfile.TemporaryDirectory() as output:
            result = run_generator(
                "--profile", "demo", "--documents", "4",
                "--min-bytes", "256", "--max-bytes", "400",
                "--output", output, "--quiet",
            )
            self.assertEqual(0, result.returncode, result.stderr)
            entries = sorted(path.name for path in Path(output).iterdir())
            self.assertIn("manifest.json", entries)
            self.assertFalse(any(name.endswith(".tmp") for name in entries))
            self.assertFalse(
                any(name.startswith(".") for name in entries),
                entries,
            )


if __name__ == "__main__":
    unittest.main(verbosity=2)
