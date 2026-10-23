#!/usr/bin/env python3
"""Build the public GuitarBattle client asset bundle and manifest.

The desktop client downloads metronome clicks and MIDI backing/guitar sample
banks from ``/api/v1/client-assets`` instead of shipping them in the installer.
Run this script on a machine with the client ``GuitarBattle/Sounds`` tree
checked out (Git LFS pulled), then upload ``manifest.json`` and ``bundle.zip``
to the directory configured by ``CLIENT_ASSETS_DIR`` on the server.

The zip is deterministic: entries are sorted by archive path and use a fixed
timestamp, so an unchanged source tree always yields the same archive SHA-256.
That hash is published as the manifest ``version``; the client only downloads
the bundle when it differs from its cached ``version.txt``.

The application never imports this script.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import posixpath
import sys
import tempfile
import zipfile
from pathlib import Path, PurePosixPath

SERVER_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SOUNDS_ROOT = SERVER_ROOT.parent / "GuitarBattle" / "Sounds"
DEFAULT_OUTPUT_DIR = SERVER_ROOT / "client_assets"

MANIFEST_SCHEMA = 1
ARCHIVE_URL = "/api/v1/client-assets/bundle.zip"
BUNDLE_FILENAME = "bundle.zip"
MANIFEST_FILENAME = "manifest.json"

# Fixed zip metadata keeps the archive byte-for-byte stable across runs.
ZIP_TIMESTAMP = (1980, 1, 1, 0, 0, 0)
ZIP_FILE_MODE = 0o644 << 16

# SFZ files that must keep the exact names the client expects.
REQUIRED_SFZ = {
    "bass": "bass/FingerBassYR 20190930.sfz",
    "piano": "piano/UprightPianoKW-small-20190703.sfz",
}

LFS_POINTER_PREFIX = b"version https://git-lfs.github.com/spec/v1"


class BuildError(RuntimeError):
    """Raised when the source tree cannot produce a usable bundle."""


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _iter_files(directory: Path) -> list[Path]:
    if not directory.is_dir():
        raise BuildError(f"Required directory is missing: {directory}")
    return sorted(
        (path for path in directory.rglob("*") if path.is_file()),
        key=lambda path: path.as_posix(),
    )


def _validate_archive_path(path: str) -> None:
    pure = PurePosixPath(path)
    if (
        not path
        or path.startswith("/")
        or "\\" in path
        or ":" in path
        or any(part in {"", ".", ".."} for part in pure.parts)
    ):
        raise BuildError(f"Unsafe path inside the bundle: {path!r}")


def _looks_like_lfs_pointer(path: Path) -> bool:
    if path.suffix.lower() not in {".wav", ".flac", ".mp3"}:
        return False
    with path.open("rb") as handle:
        return handle.read(len(LFS_POINTER_PREFIX)) == LFS_POINTER_PREFIX


def _add_bank(
    entries: list[tuple[Path, str]],
    source_root: Path,
    archive_prefix: str,
) -> None:
    if not source_root.exists():
        raise BuildError(f"Required sample bank is missing: {source_root}")
    if not source_root.is_dir():
        raise BuildError(f"Expected a directory: {source_root}")
    for source in _iter_files(source_root):
        relative = source.relative_to(source_root).as_posix()
        entries.append((source, f"{archive_prefix}/{relative}"))


def _collect_sources(sounds_root: Path) -> list[tuple[Path, str]]:
    """Return ``(source file, path inside the bundle)`` pairs, sorted."""
    if not sounds_root.is_dir():
        raise BuildError(f"Sounds root is missing: {sounds_root}")

    entries: list[tuple[Path, str]] = []

    for source_name in ("wooden-metronome-click.wav", "light-clicking-perc.wav"):
        source = sounds_root / source_name
        if not source.is_file():
            raise BuildError(f"Required metronome file is missing: {source}")
        entries.append((source, f"metronome/{source_name}"))

    _add_bank(entries, sounds_root / "GuitarSamples", "guitar")

    drums_root = sounds_root / "Drums"
    if not drums_root.is_dir():
        raise BuildError(f"Required directory is missing: {drums_root}")
    for source in sorted(drums_root.glob("*.wav"), key=lambda path: path.name):
        entries.append((source, f"drums/{source.name}"))

    _add_bank(
        entries,
        sounds_root / "FingerBassYR-SFZ+WAV-20190930" / "FingerBassYR SFZ+WAV-20190930",
        "bass",
    )
    _add_bank(
        entries,
        sounds_root / "UprightPianoKW-small-SFZ-20190703" / "UprightPianoKW-small-SFZ-20190703",
        "piano",
    )

    entries.sort(key=lambda item: item[1])
    _validate_entries(entries)
    return entries


def _validate_entries(entries: list[tuple[Path, str]]) -> None:
    archive_paths = [archive_path for _, archive_path in entries]
    duplicates = sorted({path for path in archive_paths if archive_paths.count(path) > 1})
    if duplicates:
        raise BuildError("Duplicate paths in the bundle: " + ", ".join(duplicates))
    for archive_path in archive_paths:
        _validate_archive_path(archive_path)

    for source, archive_path in entries:
        if _looks_like_lfs_pointer(source):
            raise BuildError(
                f"{source} is a Git LFS pointer, not the real sample. "
                "Run 'git lfs install' and 'git lfs pull' first."
            )

    available = set(archive_paths)
    for bank, expected in REQUIRED_SFZ.items():
        if expected not in available:
            raise BuildError(f"Expected {bank} SFZ at {expected!r}")

    for bank in ("guitar", "bass", "piano"):
        sfz_files = sorted(
            path
            for path in archive_paths
            if path.startswith(f"{bank}/") and path.lower().endswith(".sfz")
        )
        if not sfz_files:
            raise BuildError(f"No SFZ file found in {bank}/")
        for sfz_path in sfz_files:
            _validate_sfz_references(sfz_path, entries, available)


def _sfz_sample_references(sfz: Path) -> list[str]:
    references: list[str] = []
    text = sfz.read_text(encoding="utf-8", errors="replace")
    for raw_line in text.splitlines():
        line = raw_line.split("//", 1)[0].strip()
        if not line:
            continue
        key, separator, value = line.partition("=")
        if separator and key.strip().lower() == "sample":
            reference = value.strip()
            if reference:
                references.append(reference)
    return references


def _validate_sfz_references(
    sfz_archive_path: str,
    entries: list[tuple[Path, str]],
    available: set[str],
) -> None:
    """Ensure every sample referenced by an SFZ is present in the bundle."""
    sources = {archive_path: source for source, archive_path in entries}
    sfz_dir = posixpath.dirname(sfz_archive_path)
    for reference in _sfz_sample_references(sources[sfz_archive_path]):
        pure = PurePosixPath(reference)
        if pure.is_absolute() or ".." in pure.parts or "\\" in reference:
            raise BuildError(
                f"Unsafe sample path {reference!r} referenced by {sfz_archive_path}"
            )
        resolved = posixpath.normpath(posixpath.join(sfz_dir, reference))
        if resolved not in available:
            raise BuildError(
                f"{sfz_archive_path} references {resolved!r}, "
                "but that file is not in the bundle"
            )


def _write_archive(entries: list[tuple[Path, str]], target: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    handle, temp_name = tempfile.mkstemp(
        prefix=f".{target.name}.", suffix=".tmp", dir=target.parent
    )
    os.close(handle)
    temp_path = Path(temp_name)
    try:
        with zipfile.ZipFile(
            temp_path,
            mode="w",
            compression=zipfile.ZIP_DEFLATED,
            compresslevel=9,
            allowZip64=True,
        ) as archive:
            for source, archive_path in entries:
                info = zipfile.ZipInfo(filename=archive_path, date_time=ZIP_TIMESTAMP)
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = ZIP_FILE_MODE
                info.create_system = 3  # Unix, so the mode above is meaningful.
                archive.writestr(info, source.read_bytes())
        os.replace(temp_path, target)
    finally:
        temp_path.unlink(missing_ok=True)


def _write_manifest(manifest: dict[str, object], target: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    handle, temp_name = tempfile.mkstemp(
        prefix=f".{target.name}.", suffix=".tmp", dir=target.parent
    )
    temp_path = Path(temp_name)
    try:
        with os.fdopen(handle, "w", encoding="utf-8", newline="\n") as stream:
            json.dump(manifest, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
        os.replace(temp_path, target)
    finally:
        temp_path.unlink(missing_ok=True)


def build(sounds_root: Path, output_dir: Path) -> dict[str, object]:
    entries = _collect_sources(sounds_root)
    output_dir.mkdir(parents=True, exist_ok=True)

    bundle_path = output_dir / BUNDLE_FILENAME
    _write_archive(entries, bundle_path)

    files = [
        {
            "path": archive_path,
            "size_bytes": source.stat().st_size,
            "sha256": _sha256(source),
        }
        for source, archive_path in entries
    ]
    archive_size = bundle_path.stat().st_size
    archive_sha256 = _sha256(bundle_path)

    manifest: dict[str, object] = {
        "schema": MANIFEST_SCHEMA,
        "version": archive_sha256,
        "archive_url": ARCHIVE_URL,
        "archive_size_bytes": archive_size,
        "archive_sha256": archive_sha256,
        "files": files,
    }
    _write_manifest(manifest, output_dir / MANIFEST_FILENAME)
    return manifest


def _parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build the GuitarBattle client asset bundle and manifest."
    )
    parser.add_argument(
        "--sounds-root",
        type=Path,
        default=DEFAULT_SOUNDS_ROOT,
        help=f"Client Sounds directory (default: {DEFAULT_SOUNDS_ROOT})",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=DEFAULT_OUTPUT_DIR,
        help=f"Output directory for bundle.zip and manifest.json (default: {DEFAULT_OUTPUT_DIR})",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(argv)
    try:
        manifest = build(args.sounds_root.resolve(), args.output.resolve())
    except (BuildError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    output_dir = args.output.resolve()
    print(f"Client assets written to {output_dir}")
    print(f"  files:  {len(manifest['files'])}")
    print(f"  bundle: {manifest['archive_size_bytes']} bytes")
    print(f"  sha256: {manifest['archive_sha256']}")
    print(f"  version: {manifest['version']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
