#!/usr/bin/env python3
"""Download the DB-IP Lite country database used by ``GET /api/v1/geo``.

The free DB-IP database is published monthly:

    https://download.db-ip.com/free/dbip-country-lite-{YYYY-MM}.mmdb.gz

If the current month is not published yet (404), the previous month is used.
The archive is decompressed and written atomically, so the API never opens a
half-downloaded file. Only the destination path is printed; add ``--out`` to
write somewhere else (for example inside the ``geoip_data`` Docker volume).

Examples:
  python scripts/fetch_geoip.py
  python scripts/fetch_geoip.py --month 2026-09 --out /data/geoip/dbip.mmdb
"""
from __future__ import annotations

import argparse
import gzip
import os
import re
import sys
import tempfile
import urllib.error
import urllib.request
from datetime import date
from pathlib import Path

SERVER_ROOT = Path(__file__).resolve().parents[1]
if str(SERVER_ROOT) not in sys.path:
    sys.path.insert(0, str(SERVER_ROOT))

from app.config import Settings  # noqa: E402

BASE_URL = "https://download.db-ip.com/free/dbip-country-lite-{month}.mmdb.gz"
MONTH_RE = re.compile(r"^\d{4}-(0[1-9]|1[0-2])$")
TIMEOUT_SECONDS = 120


def _load_settings() -> Settings:
    # Database credentials are irrelevant here; the local .env may not define
    # them (the script is normally run on the host or inside the container).
    return Settings(
        _env_file=SERVER_ROOT / ".env",
        postgres_password="not-used-by-fetch-geoip",
        secret_key="not-used-by-fetch-geoip",
    )


def _previous_month(value: date) -> date:
    if value.month == 1:
        return date(value.year - 1, 12, 1)
    return date(value.year, value.month, 1)


def _month_key(value: date) -> str:
    return f"{value.year:04d}-{value.month:02d}"


def _month_date(value: str) -> date:
    if not MONTH_RE.match(value):
        raise argparse.ArgumentTypeError("month must look like YYYY-MM")
    year, month = value.split("-")
    return date(int(year), int(month), 1)


def _download(month: str) -> bytes:
    url = BASE_URL.format(month=month)
    request = urllib.request.Request(url, headers={"User-Agent": "GuitarBattle/1.0"})
    with urllib.request.urlopen(request, timeout=TIMEOUT_SECONDS) as response:
        return response.read()


def _write_atomic(destination: Path, payload: bytes) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    handle, temp_name = tempfile.mkstemp(
        prefix=f".{destination.name}.", suffix=".tmp", dir=destination.parent
    )
    try:
        with os.fdopen(handle, "wb") as temp_file:
            temp_file.write(payload)
            temp_file.flush()
            os.fsync(temp_file.fileno())
        os.replace(temp_name, destination)
    except BaseException:
        try:
            os.unlink(temp_name)
        except OSError:
            pass
        raise


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--out",
        type=Path,
        default=None,
        help="Destination .mmdb path (default: GEOIP_DB_PATH from settings)",
    )
    parser.add_argument(
        "--month",
        type=_month_date,
        default=None,
        help="Database month as YYYY-MM (default: current month)",
    )
    args = parser.parse_args(argv)

    destination = args.out or _load_settings().geoip_db_path
    requested = args.month or date.today().replace(day=1)

    candidates = [_month_key(requested), _month_key(_previous_month(requested))]
    compressed: bytes | None = None
    last_error: Exception | None = None
    for month in candidates:
        try:
            compressed = _download(month)
            break
        except urllib.error.HTTPError as exc:
            last_error = exc
            if exc.code != 404:
                break
        except urllib.error.URLError as exc:
            last_error = exc
            break

    if compressed is None:
        print(f"Could not download the GeoIP database: {last_error}", file=sys.stderr)
        return 1

    try:
        payload = gzip.decompress(compressed)
    except OSError as exc:
        print(f"Downloaded archive is not valid gzip: {exc}", file=sys.stderr)
        return 1

    if len(payload) < 16 * 1024 or b"\xab\xcd\xefMaxMind.com" not in payload[-512:]:
        # Every MMDB ends with the MaxMind metadata marker; anything else means
        # the server returned an error page instead of the database.
        print("Downloaded file is not a MaxMind database", file=sys.stderr)
        return 1

    _write_atomic(destination, payload)
    print(destination)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
