"""UI sounds: the fixed event registry, uploaded files and user preferences.

Public routes (no authentication) — used by the JUCE client:

* ``GET /ui-sounds/manifest`` — every active event that has a file, with the
  gain, version and an absolute download URL. The response is revalidated on
  every request (``no-cache`` + ETag), while a file URL carrying ``?v=<n>`` is
  immutable, so a replacement is picked up on the next manifest fetch.
* ``GET /ui-sounds/{key}/file`` — the current file of a key. ``?v=<n>`` pins
  the version the client saw; a stale value is still served but without the
  immutable cache header.

Authenticated routes:

* ``GET /ui-sounds/events`` — the full registry (names and categories) plus the
  caller's preferences, for the settings screen.
* ``GET/PATCH /profile/ui-sounds`` — the caller's preferences only. There is no
  user id in the path or body: settings always belong to the token's account.

Admin routes (``require_admin``):

* ``GET    /admin/ui-sounds``                 — every registry row with file info
* ``POST   /admin/ui-sounds/{key}/file``      — upload or replace
* ``DELETE /admin/ui-sounds/{key}/file``      — remove the file
* ``PATCH  /admin/ui-sounds/{key}``           — active/gain
* ``GET    /admin/ui-sounds/{key}/file``      — authorized preview

Unknown keys always answer 404. Files live under ``gameplay_media_dir`` on the
gameplay media volume; ``stored_name`` is generated server-side and resolved
against that directory, so a client cannot influence the path.
"""
from __future__ import annotations

import hashlib
from datetime import datetime, timezone

from fastapi import (
    APIRouter,
    Depends,
    File,
    HTTPException,
    Query,
    Request,
    Response,
    UploadFile,
    status,
)
from fastapi.responses import FileResponse
from sqlalchemy.orm import Session

from app.api.v1.admin_songs import require_admin
from app.api.v1.auth import get_current_user
from app.config import settings
from app.database import get_db
from app.models.ui_sound import UiSoundEvent
from app.models.user import User
from app.schemas.ui_sounds import (
    UiSoundAdminItem,
    UiSoundAdminList,
    UiSoundAdminUpdate,
    UiSoundEventInfo,
    UiSoundEventsResponse,
    UiSoundManifestItem,
    UiSoundManifestResponse,
    UiSoundUserSettings,
    UiSoundUserSettingsUpdate,
)
from app.services import ui_sounds as service

public_router = APIRouter(prefix="/ui-sounds", tags=["ui sounds"])
profile_router = APIRouter(prefix="/profile", tags=["profile: ui sounds"])
admin_router = APIRouter(prefix="/admin/ui-sounds", tags=["admin: ui sounds"])

MANIFEST_CACHE_CONTROL = "no-cache, must-revalidate"
IMMUTABLE_CACHE_CONTROL = "public, max-age=31536000, immutable"


def _http(exc: service.UiSoundError) -> HTTPException:
    return HTTPException(status_code=exc.status_code, detail=exc.message)


def _event(db: Session, key: str) -> UiSoundEvent:
    event = service.get_event(db, key)
    if event is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"Unknown UI sound event '{key}'",
        )
    return event


def _file_url(request: Request, event: UiSoundEvent, *, route: str) -> str | None:
    if not event.stored_name:
        return None
    url = request.url_for(route, key=event.key)
    return str(url.include_query_params(v=event.version))


def _admin_item(request: Request, event: UiSoundEvent) -> UiSoundAdminItem:
    return UiSoundAdminItem(
        key=event.key,
        name=event.name,
        category=event.category,
        active=bool(event.active),
        gain=float(event.gain),
        version=event.version,
        has_file=bool(event.stored_name),
        preview_url=_file_url(request, event, route="admin_ui_sound_file"),
        original_name=event.original_name,
        content_type=event.content_type,
        size_bytes=event.size_bytes,
        updated_at=event.updated_at,
    )


def _manifest_version(events: list[UiSoundEvent]) -> int:
    """A stable number that changes when any event or setting changes.

    The manifest contains the labels, categories, gains, active flags and the
    per-file versions, so all of them take part. ``sha256`` keeps the value
    deterministic across replicas and restarts without a separate counter.
    """
    if not events:
        return 0
    digest = hashlib.sha256()
    for event in sorted(events, key=lambda item: item.key):
        digest.update(
            (
                f"{event.key}\x1f{event.name}\x1f{event.category}\x1f"
                f"{int(bool(event.active))}\x1f{float(event.gain):.6f}\x1f"
                f"{event.version}\x1f{int(bool(event.stored_name))}\n"
            ).encode("utf-8")
        )
    return int.from_bytes(digest.digest()[:8], "big")


def _serve_file(
    request: Request,
    event: UiSoundEvent,
    *,
    version: int | None,
    immutable_when_current: bool,
) -> Response:
    path = service.stored_file(event.stored_name)
    if path is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"No file is stored for '{event.key}'",
        )

    etag = f'"ui-sound-{event.key}-{event.version}"'
    if immutable_when_current and version == event.version:
        cache_control = IMMUTABLE_CACHE_CONTROL
    else:
        # A missing or stale ``v`` must not pin an old file in the browser.
        cache_control = MANIFEST_CACHE_CONTROL
    headers = {"Cache-Control": cache_control, "ETag": etag}
    if version is not None and version == event.version:
        if request.headers.get("if-none-match") == etag:
            return Response(status_code=status.HTTP_304_NOT_MODIFIED, headers=headers)

    return FileResponse(
        path=path,
        media_type=event.content_type or "application/octet-stream",
        headers=headers,
    )


# ---------------------------------------------------------------------------
# Public
# ---------------------------------------------------------------------------
@public_router.get(
    "/manifest",
    response_model=UiSoundManifestResponse,
    summary="Manifest of active UI sound events that have a file",
)
def get_manifest(
    request: Request,
    response: Response,
    db: Session = Depends(get_db),
) -> UiSoundManifestResponse | Response:
    events = service.all_events(db)
    version = _manifest_version(events)
    etag = f'"ui-sounds-{version}"'
    headers = {"Cache-Control": MANIFEST_CACHE_CONTROL, "ETag": etag}
    if request.headers.get("if-none-match") == etag:
        return Response(status_code=status.HTTP_304_NOT_MODIFIED, headers=headers)

    items = [
        UiSoundManifestItem(
            key=event.key,
            name=event.name,
            category=event.category,
            gain=float(event.gain),
            url=_file_url(request, event, route="download_ui_sound") or "",
            version=event.version,
        )
        for event in events
        if event.active and event.stored_name
    ]
    response.headers.update(headers)
    return UiSoundManifestResponse(version=version, items=items)


@public_router.get(
    "/events",
    response_model=UiSoundEventsResponse,
    summary="Full UI sound registry with the caller's preferences",
)
def list_events(
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> UiSoundEventsResponse:
    events = service.all_events(db)
    service.ensure_settings_row(db, user.id)
    settings_payload = service.settings_payload(db, user.id)
    return UiSoundEventsResponse(
        items=[
            UiSoundEventInfo(
                key=event.key,
                name=event.name,
                category=event.category,
                active=bool(event.active),
                gain=float(event.gain),
                version=event.version,
                has_file=bool(event.stored_name),
            )
            for event in events
        ],
        settings=UiSoundUserSettings.model_validate(settings_payload),
    )


@public_router.get(
    "/{key}/file",
    name="download_ui_sound",
    summary="Download the current file of a UI sound event",
)
def download_ui_sound(
    key: str,
    request: Request,
    v: int | None = Query(default=None, description="Version the client cached"),
    db: Session = Depends(get_db),
) -> Response:
    event = _event(db, key)
    if not event.active:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"UI sound event '{key}' is inactive",
        )
    return _serve_file(
        request, event, version=v, immutable_when_current=True
    )


# ---------------------------------------------------------------------------
# Caller preferences
# ---------------------------------------------------------------------------
@profile_router.get(
    "/ui-sounds",
    response_model=UiSoundUserSettings,
    summary="The caller's UI sound preferences",
)
def get_profile_ui_sounds(
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> UiSoundUserSettings:
    # The defaults are materialized lazily on first access; a read never
    # requires a row and the transaction is not committed here.
    service.ensure_settings_row(db, user.id)
    return UiSoundUserSettings.model_validate(service.settings_payload(db, user.id))


@profile_router.patch(
    "/ui-sounds",
    response_model=UiSoundUserSettings,
    summary="Update the caller's UI sound preferences",
)
def update_profile_ui_sounds(
    payload: UiSoundUserSettingsUpdate,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
) -> UiSoundUserSettings:
    try:
        settings_payload = service.update_settings(
            db, user.id, payload.model_dump(exclude_unset=True)
        )
    except service.UiSoundError as exc:
        raise _http(exc) from exc
    return UiSoundUserSettings.model_validate(settings_payload)


# ---------------------------------------------------------------------------
# Admin
# ---------------------------------------------------------------------------
@admin_router.get(
    "",
    response_model=UiSoundAdminList,
    summary="List every UI sound event",
)
def list_ui_sounds(
    request: Request,
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> UiSoundAdminList:
    return UiSoundAdminList(
        items=[_admin_item(request, event) for event in service.all_events(db)]
    )


@admin_router.get(
    "/{key}/file",
    name="admin_ui_sound_file",
    summary="Preview/download a UI sound file (admin only)",
)
def admin_ui_sound_file(
    key: str,
    request: Request,
    v: int | None = Query(default=None, description="Version the panel cached"),
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> Response:
    return _serve_file(
        request, _event(db, key), version=v, immutable_when_current=False
    )


async def _read_upload(file: UploadFile) -> bytes:
    limit = settings.max_ui_sound_bytes
    try:
        data = await file.read(limit + 1)
    finally:
        await file.close()
    if len(data) > limit:
        raise HTTPException(
            status_code=status.HTTP_413_REQUEST_ENTITY_TOO_LARGE,
            detail=(
                "UI sound exceeds the configured upload limit "
                f"({limit // 1024} KiB)"
            ),
        )
    return data


@admin_router.post(
    "/{key}/file",
    response_model=UiSoundAdminItem,
    summary="Upload or replace the file of a UI sound event",
)
async def upload_ui_sound(
    key: str,
    request: Request,
    file: UploadFile = File(..., description="MP3, WAV or OGG, up to 2 MiB"),
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> UiSoundAdminItem:
    event = _event(db, key)
    data = await _read_upload(file)
    try:
        suffix, content_type = service.validate_upload(file.filename, data)
    except service.UiSoundError as exc:
        raise _http(exc) from exc

    stored_name = service.store_file(event.key, data, suffix)
    previous_name = event.stored_name
    event.stored_name = stored_name
    event.original_name = (file.filename or "").rsplit("/", 1)[-1].rsplit("\\", 1)[-1] or None
    event.content_type = content_type
    event.size_bytes = len(data)
    event.version = (event.version or 0) + 1
    event.updated_at = datetime.now(timezone.utc)

    try:
        db.commit()
        db.refresh(event)
    except Exception:
        db.rollback()
        service.delete_file(stored_name)
        raise

    # The row now points at the new file, so removing the previous one cannot
    # create a dangling reference. A failed commit above keeps the old file.
    if previous_name and previous_name != stored_name:
        service.delete_file(previous_name)
    return _admin_item(request, event)


@admin_router.delete(
    "/{key}/file",
    status_code=status.HTTP_204_NO_CONTENT,
    summary="Remove the file of a UI sound event",
)
def delete_ui_sound_file(
    key: str,
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> Response:
    event = _event(db, key)
    if not event.stored_name and event.size_bytes is None:
        # Idempotent: no file means nothing to bump.
        return Response(status_code=status.HTTP_204_NO_CONTENT)

    stored_name = event.stored_name
    event.stored_name = None
    event.original_name = None
    event.content_type = None
    event.size_bytes = None
    event.version = (event.version or 0) + 1
    event.updated_at = datetime.now(timezone.utc)
    db.commit()
    service.delete_file(stored_name)
    return Response(status_code=status.HTTP_204_NO_CONTENT)


@admin_router.patch(
    "/{key}",
    response_model=UiSoundAdminItem,
    summary="Enable/disable an event or change its gain",
)
def update_ui_sound(
    key: str,
    payload: UiSoundAdminUpdate,
    request: Request,
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> UiSoundAdminItem:
    event = _event(db, key)
    if payload.gain is not None:
        try:
            event.gain = service.clamp_unit(payload.gain, field="gain")
        except service.UiSoundError as exc:
            raise _http(exc) from exc
    if payload.active is not None:
        event.active = payload.active
    if payload.gain is None and payload.active is None:
        return _admin_item(request, event)

    event.updated_at = datetime.now(timezone.utc)
    db.commit()
    db.refresh(event)
    return _admin_item(request, event)
