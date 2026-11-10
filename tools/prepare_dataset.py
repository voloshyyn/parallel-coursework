#!/usr/bin/env python3
"""Split Project Gutenberg books into similarly sized documents.

The script reads every ``*.txt`` book in the raw directory, drops the
Project Gutenberg boilerplate (everything outside the START/END markers),
and cuts the remaining text into chunks of approximately ``--target-bytes``
bytes. Chunks are cut on paragraph boundaries; a paragraph that does not fit
into the target size is split on sentence boundaries, and a sentence that is
still too long is split on word boundaries. No word is ever cut in half.

Output files are named ``<book_id>_<n>.txt`` and are written as UTF-8.
Only aggregate statistics are printed; book contents never leave the process.
"""

from __future__ import annotations

import argparse
import re
import statistics
import sys
import time
from pathlib import Path

EXPECTED_BOOKS = 5_322
EXPECTED_BYTES = 1_999_124_802
DEFAULT_TARGET_BYTES = 200 * 1024

START_MARKER = re.compile(r"^\*\*\*\s*START OF.*$", re.IGNORECASE | re.MULTILINE)
END_MARKER = re.compile(r"^\*\*\*\s*END OF.*$", re.IGNORECASE | re.MULTILINE)
PARAGRAPH_SEPARATOR = re.compile(r"\n\s*\n")
SENTENCE_SEPARATOR = re.compile(r"(?<=[.!?])\s+")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--raw-dir",
        type=Path,
        default=Path("data/raw/train"),
        help="directory with Project Gutenberg .txt books",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=Path("data/docs"),
        help="directory for the prepared documents",
    )
    parser.add_argument(
        "--target-bytes",
        type=int,
        default=DEFAULT_TARGET_BYTES,
        help="approximate size of one document in bytes (default: %(default)s)",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="compute statistics without writing any file",
    )
    parser.add_argument(
        "--no-clean",
        action="store_true",
        help="do not remove existing .txt files from the output directory",
    )
    return parser.parse_args()


def verify_raw_dataset(raw_dir: Path) -> list[Path]:
    """Check the raw corpus against the expected file count and total size."""
    if not raw_dir.is_dir():
        sys.exit(f"error: raw directory not found: {raw_dir}")

    books = sorted(raw_dir.glob("*.txt"))
    total_bytes = sum(book.stat().st_size for book in books)
    if len(books) != EXPECTED_BOOKS or total_bytes != EXPECTED_BYTES:
        sys.exit(
            "error: unexpected dataset: "
            f"{len(books)} files, {total_bytes} bytes; "
            f"expected {EXPECTED_BOOKS} files, {EXPECTED_BYTES} bytes"
        )
    return books


def read_book(path: Path) -> str:
    """Read a book, tolerating the legacy encodings found in old archives."""
    raw = path.read_bytes()
    for encoding in ("utf-8-sig", "cp1252", "latin-1"):
        try:
            return raw.decode(encoding)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", errors="replace")


def strip_boilerplate(text: str) -> tuple[str, bool, bool]:
    """Return the book body and flags telling whether each marker was found.

    Everything before the START marker and everything after the END marker is
    Project Gutenberg boilerplate (header, license), so it is dropped.
    """
    text = text.replace("\r\n", "\n").replace("\r", "\n")

    start_match = START_MARKER.search(text)
    start = start_match.end() if start_match else 0

    end = len(text)
    end_matches = list(END_MARKER.finditer(text))
    if end_matches:
        end = end_matches[-1].start()

    if end <= start:
        return "", start_match is not None, bool(end_matches)
    return text[start:end].lstrip("\ufeff"), start_match is not None, bool(end_matches)


def split_paragraphs(text: str) -> list[str]:
    paragraphs = []
    for block in PARAGRAPH_SEPARATOR.split(text):
        block = block.strip().lstrip("\ufeff").strip()
        if block:
            paragraphs.append(block)
    return paragraphs


def split_by_words(piece: str, limit: int) -> list[str]:
    """Split an overlong sentence on word boundaries."""
    chunks: list[str] = []
    current: list[str] = []
    current_len = 0
    for word in piece.split():
        extra = len(word) + (1 if current else 0)
        if current and current_len + extra > limit:
            chunks.append(" ".join(current))
            current = [word]
            current_len = len(word)
        else:
            current.append(word)
            current_len += extra
    if current:
        chunks.append(" ".join(current))
    return chunks


def split_long_paragraph(paragraph: str, limit: int) -> list[str]:
    """Split a paragraph that exceeds the target into sentence-sized pieces."""
    pieces: list[str] = []
    buffer = ""
    for sentence in SENTENCE_SEPARATOR.split(paragraph):
        if len(sentence) > limit:
            if buffer:
                pieces.append(buffer)
                buffer = ""
            pieces.extend(split_by_words(sentence, limit))
        elif buffer and len(buffer) + 1 + len(sentence) > limit:
            pieces.append(buffer)
            buffer = sentence
        else:
            buffer = f"{buffer} {sentence}" if buffer else sentence
    if buffer:
        pieces.append(buffer)
    return pieces


def merge_small_tail(chunks: list[str], target_bytes: int) -> list[str]:
    """Append a tiny final chunk to the previous one to avoid stub documents."""
    min_tail = max(1024, target_bytes // 10)
    if len(chunks) > 1 and len(chunks[-1].encode("utf-8")) < min_tail:
        tail = chunks.pop().rstrip("\n")
        chunks[-1] = chunks[-1].rstrip("\n") + "\n\n" + tail + "\n"
    return chunks


def chunk_book(text: str, target_bytes: int) -> list[str]:
    """Cut a book into chunks of approximately ``target_bytes`` bytes."""
    chunks: list[str] = []
    current: list[str] = []
    current_len = 0

    def flush() -> None:
        nonlocal current, current_len
        if current:
            chunks.append("\n\n".join(current) + "\n")
            current = []
            current_len = 0

    for paragraph in split_paragraphs(text):
        pieces = (
            [paragraph]
            if len(paragraph) <= target_bytes
            else split_long_paragraph(paragraph, target_bytes)
        )
        for piece in pieces:
            separator = 2 if current else 0
            if current and current_len + separator + len(piece) > target_bytes:
                flush()
                separator = 0
            current.append(piece)
            current_len += separator + len(piece)
    flush()
    return merge_small_tail(chunks, target_bytes)


def clean_output_dir(out_dir: Path) -> int:
    removed = 0
    for stale in out_dir.glob("*.txt"):
        stale.unlink()
        removed += 1
    return removed


def main() -> None:
    args = parse_args()
    if args.target_bytes < 1_000:
        sys.exit("error: --target-bytes must be at least 1000")

    started = time.perf_counter()
    books = verify_raw_dataset(args.raw_dir)
    print(f"raw books: {len(books)}")
    print(f"raw bytes: {sum(book.stat().st_size for book in books)}")

    out_dir: Path = args.out_dir
    if not args.dry_run:
        out_dir.mkdir(parents=True, exist_ok=True)
        if not args.no_clean:
            print(f"removed stale documents: {clean_output_dir(out_dir)}")

    sizes: list[int] = []
    books_with_body = 0
    books_skipped = 0
    start_markers = 0
    end_markers = 0

    for index, book in enumerate(books, start=1):
        body, has_start, has_end = strip_boilerplate(read_book(book))
        start_markers += int(has_start)
        end_markers += int(has_end)

        chunks = chunk_book(body, args.target_bytes)
        if not chunks:
            books_skipped += 1
            continue
        books_with_body += 1

        if not args.dry_run:
            for number, chunk in enumerate(chunks, start=1):
                output = out_dir / f"{book.stem}_{number}.txt"
                output.write_text(chunk, encoding="utf-8", newline="\n")

        sizes.extend(len(chunk.encode("utf-8")) for chunk in chunks)
        if index % 500 == 0:
            print(f"processed books: {index}/{len(books)}")

    elapsed = time.perf_counter() - started
    print(f"books with body: {books_with_body}")
    print(f"books skipped (empty body): {books_skipped}")
    print(f"start markers found: {start_markers}")
    print(f"end markers found: {end_markers}")
    print(f"documents: {len(sizes)}")
    print(f"document bytes: {sum(sizes)}")
    if sizes:
        print(f"document size min/median/max: {min(sizes)}/"
              f"{int(statistics.median(sizes))}/{max(sizes)}")
        for limit in (10_240, 51_200, 102_400, 204_800):
            below = sum(1 for size in sizes if size < limit)
            print(f"documents below {limit} bytes: {below}")
    print(f"target bytes: {args.target_bytes}")
    print(f"dry run: {args.dry_run}")
    print(f"elapsed: {elapsed:.1f}s")


if __name__ == "__main__":
    main()
