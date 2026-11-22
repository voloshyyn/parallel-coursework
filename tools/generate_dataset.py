#!/usr/bin/env python3
"""Generate a reproducible synthetic text corpus for the search engine.

The repository must be usable from a clean clone without the private Gutenberg
mirror. This script is the fallback: it writes a deterministic, self-contained
corpus using only the Python standard library (no network, no pip packages).

Profiles
--------
``full``
    At least 10 000 non-empty documents and at least 2 GiB of actual UTF-8 text.
    It is the default for ``docker compose up`` and is meant for the volume and
    performance requirements of the course work.
``demo``
    A few dozen small documents for a near-instant demonstration. It is *not*
    a substitute for the volume requirements and the script says so.

The corpus is synthetic. It has varied topics, sentence lengths, word
frequencies and a list of known queries, but it is not natural language and
must not be presented as a real book corpus. The original Gutenberg splitter
(``tools/prepare_dataset.py``) remains available for a real corpus.

Output
------
``<output>/doc-000000.txt`` ... plus ``manifest.json``. The manifest records
the format version, seed, profile, document count, byte count, known queries
and a SHA-256 of the generated content. A repeated run validates the manifest
and file sizes and reuses the corpus without rewriting it. Writing happens in
a sibling staging directory that is renamed into place, so a reader never sees
a half-written corpus and existing user data is never silently deleted.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import random
import shutil
import sys
import tempfile
from pathlib import Path

FORMAT_VERSION = 1
GENERATOR_NAME = "tools/generate_dataset.py"

FULL_DOCUMENTS = 12_000
FULL_MIN_BYTES = 190 * 1024
FULL_MAX_BYTES = 210 * 1024
FULL_MIN_TOTAL_BYTES = 2 * 1024**3  # 2 GiB of actual text

DEMO_DOCUMENTS = 60
DEMO_MIN_BYTES = 4 * 1024
DEMO_MAX_BYTES = 8 * 1024

FULL_SEED = 20_240_517
DEMO_SEED = 7

PROFILES = {
    "full": {
        "documents": FULL_DOCUMENTS,
        "min_bytes": FULL_MIN_BYTES,
        "max_bytes": FULL_MAX_BYTES,
        "seed": FULL_SEED,
        "min_total_bytes": FULL_MIN_TOTAL_BYTES,
    },
    "demo": {
        "documents": DEMO_DOCUMENTS,
        "min_bytes": DEMO_MIN_BYTES,
        "max_bytes": DEMO_MAX_BYTES,
        "seed": DEMO_SEED,
        "min_total_bytes": 0,
    },
}

COMMON_WORDS = """
the of and to in a is that for it as was with be by on not he this are or
his from at which but have an they you were her all she there would their we
him been has when who will more no if out so said what up its about into than
them can only other new some could time these two may then do first any my such
and our their his her its this that these those a an the and or but because
while although however therefore perhaps often rarely always never sometimes
small large quiet loud bright dark ancient modern simple complex careful rapid
""".split()

TOPICS = {
    "ocean": """
    whale coral tide reef salt current wave plankton kelp harbor lagoon
    dolphin seagull anchor vessel shoreline estuary abyss mariner buoy
    """.split(),
    "space": """
    nebula quasar telescope orbit comet asteroid galaxy star planet rocket
    satellite eclipse gravity meteor cosmos lunar solar vacuum horizon
    """.split(),
    "history": """
    ancient rome empire archive chronicle century kingdom castle ruin
    parchment treaty legion citadel dynasty relic fortress medieval
    """.split(),
    "cooking": """
    sourdough bread oven flour yeast recipe kitchen simmer saffron basil
    harvest plate spoon broth pastry spice kettle feast
    """.split(),
    "programming": """
    algorithm graph compiler function variable thread mutex cache index
    buffer process kernel memory parser syntax module binary debug
    """.split(),
    "nature": """
    tundra lichen glacier fjord meadow mangrove swamp forest moss thunder
    storm monsoon breeze zephyr ripple canyon ridge valley
    """.split(),
    "music": """
    violin sonata harmonica waltz rhythm melody chord tempo orchestra
    trumpet piano drum chorus verse concert string
    """.split(),
    "art": """
    mosaic harbor origami canvas pigment sculpture portrait gallery velvet
    curtain lantern mural sketch marble granite quartz obsidian
    """.split(),
    "desert": """
    cactus desert sand dune oasis caravan camel mirage scorpion lizard
    sandstone canyon heat shade well palm
    """.split(),
    "science": """
    quantum physics crystal fossil layer experiment molecule atom energy
    particle theory measure laboratory sample reaction field
    """.split(),
    "garden": """
    sunflower seed soil petal blossom root stem hedge trellis rake spade
    compost beetle butterfly bee nectar orchard vine
    """.split(),
    "travel": """
    journey compass map trail passport luggage ferry bridge village market
    road valley summit beacon lighthouse coach station
    """.split(),
}

# A short list of real non-ASCII words keeps the UTF-8 path exercised without
# pretending the corpus is multilingual.
UNICODE_WORDS = ["naïve", "café", "résumé", "Море", "Дніпро", "ґанок"]

KNOWN_QUERIES = [
    "deep ocean whale",
    "coral reef",
    "ancient rome",
    "sourdough bread",
    "quantum physics",
    "tundra lichen",
    "violin sonata",
    "mosaic harbor",
    "glacier fjord",
    "kestrel meadow",
    "saffron harvest",
    "trebuchet castle",
    "telescope nebula",
    "mangrove swamp",
    "origami paper",
    "granite quarry",
    "monsoon rain",
    "waltz lantern",
    "cactus desert",
    "obsidian volcano",
    "quartz crystal",
    "sunflower field",
    "thunder storm",
    "velvet curtain",
    "zephyr breeze",
    "harmonica tune",
    "pixel shader",
    "algorithm graph",
    "sonnet verse",
    "fossil layer",
]

HEADER_TEMPLATES = [
    "Field notes on {topic}, volume {index}",
    "A short study of {topic}, part {index}",
    "Observations about {topic}, entry {index}",
    "Working notebook: {topic}, chapter {index}",
    "Collected remarks on {topic}, folio {index}",
    "Annotated record of {topic}, section {index}",
]


def resolve_settings(args: argparse.Namespace) -> dict:
    """Combine a named profile with optional explicit overrides."""
    profile_name = args.profile
    if profile_name not in PROFILES:
        raise SystemExit(
            f"unknown profile {profile_name!r}; choose one of "
            + ", ".join(sorted(PROFILES))
        )
    base = dict(PROFILES[profile_name])
    base["profile"] = profile_name

    overridden = (
        args.documents is not None
        or args.min_bytes is not None
        or args.max_bytes is not None
        or args.min_total_bytes is not None
    )
    if args.documents is not None:
        base["documents"] = args.documents
    if args.min_bytes is not None:
        base["min_bytes"] = args.min_bytes
    if args.max_bytes is not None:
        base["max_bytes"] = args.max_bytes
    if args.min_total_bytes is not None:
        base["min_total_bytes"] = args.min_total_bytes
    if args.seed is not None:
        base["seed"] = args.seed
    if overridden:
        # Explicit sizes are used by tests and experiments; they are not the
        # audited full profile.
        base["profile"] = "custom"

    if base["documents"] < 1:
        raise SystemExit("--documents must be >= 1")
    if base["min_bytes"] < 1 or base["max_bytes"] < base["min_bytes"]:
        raise SystemExit("--min-bytes/--max-bytes are invalid")
    return base


def make_document(rng: random.Random, index: int, min_bytes: int,
                  max_bytes: int) -> str:
    """Build one non-empty, varied UTF-8 document of approximately `target`."""
    topic = rng.choice(sorted(TOPICS))
    topic_words = TOPICS[topic]
    pool = COMMON_WORDS + topic_words
    query = KNOWN_QUERIES[index % len(KNOWN_QUERIES)]
    target = rng.randint(min_bytes, max_bytes)

    header = HEADER_TEMPLATES[index % len(HEADER_TEMPLATES)].format(
        topic=topic, index=index
    )
    pieces = [header + ".", f"This entry mentions {query} in context."]
    total = sum(len(piece.encode("utf-8")) + 1 for piece in pieces)

    while total < target:
        sentences = []
        for _ in range(rng.randint(5, 11)):
            count = rng.randint(9, 22)
            words = rng.choices(pool, k=count)
            # Keep the topic visible in every paragraph without making it the
            # only vocabulary.
            if rng.random() < 0.6:
                words[rng.randrange(count)] = rng.choice(topic_words)
            # Vary term frequencies: occasional repetition is useful for TF-IDF.
            if rng.random() < 0.06:
                words.append(words[rng.randrange(count)])
            if rng.random() < 0.03:
                words.insert(rng.randrange(len(words) + 1),
                             rng.choice(UNICODE_WORDS))
            sentences.append(" ".join(words).capitalize() + ".")
        paragraph = " ".join(sentences)
        pieces.append(paragraph)
        total += len(paragraph.encode("utf-8")) + 1

    return "\n".join(pieces) + "\n"


def manifest_matches(output: Path, expected: dict) -> tuple[bool, str]:
    """Validate a previous run. Cheap: counts and byte sizes, not content."""
    manifest_path = output / "manifest.json"
    if not manifest_path.is_file():
        return False, "manifest.json is missing"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        return False, f"manifest.json is unreadable: {error}"

    if not isinstance(manifest, dict):
        return False, "manifest must be an object"
    if manifest.get("generator") != GENERATOR_NAME:
        return False, "generator does not match"
    if manifest.get("format_version") != FORMAT_VERSION:
        return False, "format_version does not match"
    if manifest.get("profile") != expected["profile"]:
        return False, "profile does not match"
    if manifest.get("seed") != expected["seed"]:
        return False, "seed does not match"
    if manifest.get("documents") != expected["documents"]:
        return False, "document count does not match"
    if manifest.get("min_bytes_per_document") != expected["min_bytes"]:
        return False, "minimum document size does not match"
    if manifest.get("max_bytes_per_document") != expected["max_bytes"]:
        return False, "maximum document size does not match"

    files = sorted(output.glob("doc-*.txt"))
    if len(files) != expected["documents"]:
        return False, (
            f"found {len(files)} documents, expected {expected['documents']}"
        )
    expected_names = {f"doc-{i:06d}.txt" for i in range(expected["documents"])}
    if {p.name for p in output.iterdir()} != expected_names | {"manifest.json"}:
        return False, "unexpected or renamed files in generated corpus"
    if any(path.stat().st_size == 0 for path in files):
        return False, "an empty document was found"
    total = sum(path.stat().st_size for path in files)
    if total != manifest.get("bytes"):
        return False, "byte count does not match the manifest"
    if total < expected["min_total_bytes"]:
        return False, (
            f"corpus has {total} bytes, below the required "
            f"{expected['min_total_bytes']}"
        )
    return True, "ok"


def content_hash(directory: Path) -> str:
    digest = hashlib.sha256()
    for path in sorted(directory.glob("doc-*.txt")):
        digest.update(path.name.encode("utf-8"))
        digest.update(b"\0")
        with path.open("rb") as handle:
            for block in iter(lambda: handle.read(1024 * 1024), b""):
                digest.update(block)
        digest.update(b"\0")
    return digest.hexdigest()


def write_corpus(settings: dict, output: Path) -> dict:
    """Generate into a sibling staging directory, then rename into place."""
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{output.name}.tmp-",
                                    dir=output.parent))
    staging.chmod(0o755)  # A prepared corpus can be read by the app user too.

    rng = random.Random(settings["seed"])
    byte_count = 0
    try:
        for index in range(settings["documents"]):
            text = make_document(
                rng, index, settings["min_bytes"], settings["max_bytes"]
            )
            data = text.encode("utf-8")
            name = f"doc-{index:06d}.txt"
            temporary = staging / (name + ".tmp")
            with temporary.open("wb") as handle:
                handle.write(data)
            os.replace(temporary, staging / name)
            byte_count += len(data)

        if byte_count < settings["min_total_bytes"]:
            raise SystemExit(
                f"generated {byte_count} bytes, below the required "
                f"{settings['min_total_bytes']} for profile "
                f"{settings['profile']}"
            )

        manifest = {
            "format_version": FORMAT_VERSION,
            "generator": GENERATOR_NAME,
            "profile": settings["profile"],
            "seed": settings["seed"],
            "documents": settings["documents"],
            "bytes": byte_count,
            "min_bytes_per_document": settings["min_bytes"],
            "max_bytes_per_document": settings["max_bytes"],
            "known_queries": KNOWN_QUERIES,
            "content_sha256": content_hash(staging),
        }
        manifest_tmp = staging / "manifest.json.tmp"
        manifest_tmp.write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        os.replace(manifest_tmp, staging / "manifest.json")
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise

    # Never replace a non-empty directory, even with --force: a generated
    # corpus may contain files the user added. Select a new output directory
    # instead. In Docker the output is a child of the named volume, not its
    # mountpoint, so this rename stays on one filesystem.
    try:
        if output.exists():
            if not output.is_dir() or any(output.iterdir()):
                raise SystemExit(f"refusing to replace non-empty output: {output}")
            output.rmdir()
        os.replace(staging, output)
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    return manifest


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True,
                        help="directory that will hold doc-*.txt and manifest")
    parser.add_argument("--profile", default="full",
                        choices=sorted(PROFILES))
    parser.add_argument("--seed", type=int, default=None)
    parser.add_argument("--documents", type=int, default=None)
    parser.add_argument("--min-bytes", type=int, default=None)
    parser.add_argument("--max-bytes", type=int, default=None)
    parser.add_argument("--min-total-bytes", type=int, default=None)
    parser.add_argument("--verify-hash", action="store_true",
                        help="recompute the content hash when reusing a corpus")
    parser.add_argument("--plan", action="store_true",
                        help="print the resolved configuration and exit")
    parser.add_argument("--quiet", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    settings = resolve_settings(args)

    plan = {
        "format_version": FORMAT_VERSION,
        "profile": settings["profile"],
        "seed": settings["seed"],
        "documents": settings["documents"],
        "min_bytes_per_document": settings["min_bytes"],
        "max_bytes_per_document": settings["max_bytes"],
        "guaranteed_min_total_bytes":
            settings["documents"] * settings["min_bytes"],
        "required_total_bytes": settings["min_total_bytes"],
    }
    if args.plan:
        print(json.dumps(plan, indent=2, sort_keys=True))
        return 0

    output = Path(args.output)
    reusable, reason = manifest_matches(output, settings)
    if reusable:
        if args.verify_hash:
            manifest = json.loads(
                (output / "manifest.json").read_text(encoding="utf-8")
            )
            actual = content_hash(output)
            if actual != manifest.get("content_sha256"):
                print(
                    "error: content hash does not match the manifest",
                    file=sys.stderr,
                )
                return 2
        if not args.quiet:
            manifest = json.loads(
                (output / "manifest.json").read_text(encoding="utf-8")
            )
            print(
                "reusing dataset: profile={profile} documents={documents} "
                "bytes={bytes} seed={seed}".format(**manifest)
            )
            if manifest["profile"] == "demo":
                print(
                    "note: the demo profile is for a quick look and does "
                    "not meet the full volume requirement",
                    file=sys.stderr,
                )
        return 0
    if output.exists() and output.is_dir() and any(output.iterdir()):
        print(
            f"error: {output} exists but is not a matching generated "
            f"corpus ({reason}); choose a different output directory",
            file=sys.stderr,
        )
        return 2
    if output.exists() and not output.is_dir():
        print(f"error: {output} exists and is not a directory",
              file=sys.stderr)
        return 2

    manifest = write_corpus(settings, output)
    if not args.quiet:
        print(
            "generated dataset: profile={profile} documents={documents} "
            "bytes={bytes} seed={seed} sha256={content_sha256}".format(
                **manifest
            )
        )
        if manifest["profile"] == "demo":
            print(
                "note: the demo profile is for a quick look and does not meet "
                "the full volume requirement",
                file=sys.stderr,
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
