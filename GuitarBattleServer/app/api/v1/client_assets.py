"""Public client asset bundle and manifest.

The desktop client downloads its metronome clicks and MIDI sample banks from
these endpoints instead of reading the local ``Sounds`` tree. The files are
generated offline by ``scripts/build_client_assets.py`` and mounted into the
API container, so this module only serves them: no database access and no
user-controlled path segments.

Both endpoints are public. The manifest ``version`` is the SHA-256 of the
archive, which lets the client skip the download when its cached copy matches.
"""

from pathlib import Path

from fastapi import APIRouter, HTTPException, status
from fastapi.responses import FileResponse

from app.config import settings

router = APIRouter(prefix="/client-assets", tags=["client-assets"])

MANIFEST_FILENAME = "manifest.json"
BUNDLE_FILENAME = "bundle.zip"
BUNDLE_DOWNLOAD_NAME = "guitarbattle-sounds.zip"

# The client compares the manifest version with a cached value on every launch,
# so intermediaries must not serve a stale manifest after a deployment.
NO_CACHE_HEADERS = {"Cache-Control": "no-cache"}

_UNAVAILABLE = status.HTTP_503_SERVICE_UNAVAILABLE


def _asset_path(filename: str) -> Path:
    """Resolve a generated asset inside ``settings.client_assets_dir``.

    Filenames are constants defined in this module, but the containment check
    keeps a misconfigured or symlinked directory from exposing other files.
    """
    root = settings.client_assets_dir.resolve()
    path = (root / filename).resolve()
    if not path.is_relative_to(root):
        raise HTTPException(status_code=_UNAVAILABLE, detail="Client assets are not available")
    return path


def _require_asset(filename: str, detail: str) -> Path:
    path = _asset_path(filename)
    if not path.is_file():
        raise HTTPException(status_code=_UNAVAILABLE, detail=detail)
    return path


@router.get("/manifest", summary="Get the client asset manifest")
def get_client_assets_manifest() -> FileResponse:
    """Return the generated manifest.

    A manifest without its archive is unusable, so report 503 while either file
    is missing (for example during a partial deployment) instead of letting the
    client fail halfway through the download.
    """
    manifest = _require_asset(MANIFEST_FILENAME, "Client asset manifest is not available")
    _require_asset(BUNDLE_FILENAME, "Client asset bundle is not available")
    return FileResponse(
        path=manifest,
        media_type="application/json",
        headers=NO_CACHE_HEADERS,
    )


@router.get("/bundle.zip", summary="Download the client asset bundle")
def download_client_assets_bundle() -> FileResponse:
    """Return the generated zip archive as a download."""
    bundle = _require_asset(BUNDLE_FILENAME, "Client asset bundle is not available")
    return FileResponse(
        path=bundle,
        media_type="application/zip",
        filename=BUNDLE_DOWNLOAD_NAME,
        headers=NO_CACHE_HEADERS,
    )


# FastAPI does not add HEAD automatically for GET routes. Clients (and deploy
# checks) use HEAD to read the archive size before downloading.
@router.head("/manifest", include_in_schema=False)
def head_client_assets_manifest() -> FileResponse:
    return get_client_assets_manifest()


@router.head("/bundle.zip", include_in_schema=False)
def head_client_assets_bundle() -> FileResponse:
    return download_client_assets_bundle()
