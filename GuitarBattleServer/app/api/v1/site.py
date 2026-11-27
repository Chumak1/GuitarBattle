"""Public landing page content, game build download and IP geo detection.

The landing page is a static file (``app/static/landing/index.html``) that
reads these endpoints at load time, so the CMS content and the current desktop
build can change without redeploying the page.
"""
from __future__ import annotations

from pathlib import Path
from typing import Any, Literal

from fastapi import APIRouter, Depends, HTTPException, Query, Request, status
from fastapi.responses import FileResponse
from pydantic import ValidationError
from sqlalchemy import select
from sqlalchemy.orm import Session

from app.config import settings
from app.database import get_db
from app.models.site_content import GameBuild, LandingSettings
from app.schemas.artwork import SiteConfigResponse
from app.schemas.site import (
    MAX_SCREENSHOTS,
    DownloadInfo,
    GameBuildItem,
    GeoResponse,
    HeroBackground,
    LandingDownloads,
    LandingResponse,
    LandingScreenshot,
    SiteContacts,
)
from app.services.client_ip import client_ip
from app.services.artwork import public_site_config
from app.services.contacts import contacts_update_from_content, public_contacts
from app.services.game_builds import (
    DEFAULT_BUILD_ARTIFACT,
    DEFAULT_GAME_BUILD_PLATFORM,
    GAME_BUILD_PLATFORMS,
    INSTALLER,
    MACOS,
    PORTABLE,
    WINDOWS,
)
from app.services.geo import country_for_ip, language_for_country

router = APIRouter(tags=["site"])

# Row id of the single landing settings document.
LANDING_SETTINGS_ID = 1
# Public URL prefix of hero uploads; the directory itself is mounted by main.py.
LANDING_MEDIA_URL_PREFIX = "/media/landing/"

#: ``?platform=`` query parameter shared by the public endpoints.
PlatformParam = Literal["windows", "macos"]
#: ``?artifact=`` query parameter of the admin build list.
ArtifactParam = Literal["installer", "portable"]


def download_url() -> str:
    """Canonical (Windows) download URL, kept for backwards compatibility."""
    return f"{settings.api_v1_prefix}/downloads/game"


def platform_download_url(platform: str) -> str:
    """Installer download URL of one platform.

    The Windows URL carries no query string: the desktop client builds it from
    ``/landing`` and older clients must keep working unchanged.
    """
    if platform in ("", WINDOWS):
        return download_url()
    return f"{download_url()}?platform={platform}"


def update_download_url(platform: str = DEFAULT_GAME_BUILD_PLATFORM) -> str:
    """Portable ZIP URL used by the installed client to update itself."""
    return f"{download_url()}/update?platform={platform}"


def get_landing_row(db: Session) -> LandingSettings | None:
    return db.get(LandingSettings, LANDING_SETTINGS_ID)


def landing_content(row: LandingSettings | None) -> dict[str, Any]:
    if row is None or not isinstance(row.content, dict):
        return {}
    return row.content


def blocks_from_content(content: dict[str, Any]) -> dict[str, Any]:
    blocks = content.get("blocks")
    return blocks if isinstance(blocks, dict) else {}


def hero_background_from_content(content: dict[str, Any]) -> HeroBackground:
    raw = content.get("hero_background")
    if not isinstance(raw, dict):
        return HeroBackground()
    try:
        return HeroBackground.model_validate(raw)
    except ValidationError:
        # A hand-edited or legacy value must not break the public page.
        return HeroBackground()


def screenshots_from_content(content: dict[str, Any]) -> list[LandingScreenshot]:
    """Tolerant reader of the landing gallery.

    Broken or hand-edited entries are skipped one by one and the list is cut at
    :data:`MAX_SCREENSHOTS`, so a corrupted CMS document can never break ``/``
    or the admin editor.
    """
    raw = content.get("screenshots")
    if not isinstance(raw, list):
        return []
    screenshots: list[LandingScreenshot] = []
    for item in raw:
        if len(screenshots) >= MAX_SCREENSHOTS:
            break
        if not isinstance(item, dict):
            continue
        try:
            screenshots.append(LandingScreenshot.model_validate(item))
        except ValidationError:
            continue
    return screenshots


def serialize_build(build: GameBuild) -> GameBuildItem:
    platform = build.platform or DEFAULT_GAME_BUILD_PLATFORM
    artifact = build.artifact or DEFAULT_BUILD_ARTIFACT
    # The URL must point at the endpoint that actually serves this artifact:
    # installers live on /downloads/game, portable ZIPs on /downloads/game/update.
    url = (
        update_download_url(platform)
        if artifact == PORTABLE
        else platform_download_url(platform)
    )
    return GameBuildItem(
        id=build.id,
        platform=platform,
        artifact=artifact,
        version=build.version or "",
        filename=build.filename,
        size_bytes=build.size_bytes,
        content_type=build.content_type,
        is_active=build.is_active,
        uploaded_at=build.uploaded_at,
        url=url,
        release_notes=build.release_notes or "",
    )


def build_file_path(build: GameBuild) -> Path | None:
    """Resolve a stored build inside ``settings.game_builds_dir``.

    ``stored_name`` is generated by the server, but the containment check keeps
    a corrupted row or symlinked directory from exposing other files.
    """
    storage_root = settings.game_builds_dir.resolve()
    target = (storage_root / build.stored_name).resolve()
    if not target.is_relative_to(storage_root) or not target.is_file():
        return None
    return target


def active_builds(
    db: Session, artifact: str = DEFAULT_BUILD_ARTIFACT
) -> dict[str, GameBuild | None]:
    """Newest published build of every platform for one artifact.

    ``artifact`` is part of the published slot: a portable ZIP and an installer
    of the same platform can be active at the same time.
    """
    result: dict[str, GameBuild | None] = {platform: None for platform in GAME_BUILD_PLATFORMS}
    rows = db.scalars(
        select(GameBuild)
        .where(GameBuild.is_active.is_(True), GameBuild.artifact == artifact)
        .order_by(GameBuild.uploaded_at.desc(), GameBuild.id.desc())
    ).all()
    for row in rows:
        platform = row.platform or DEFAULT_GAME_BUILD_PLATFORM
        if platform in result and result[platform] is None:
            result[platform] = row
    return result


def active_build(
    db: Session,
    platform: str = DEFAULT_GAME_BUILD_PLATFORM,
    artifact: str = DEFAULT_BUILD_ARTIFACT,
) -> GameBuild | None:
    return active_builds(db, artifact).get(platform)


def download_info(
    build: GameBuild | None,
    platform: str,
    artifact: str = DEFAULT_BUILD_ARTIFACT,
) -> DownloadInfo:
    """Public download descriptor for one platform and artifact.

    ``url`` is always set so the page can link to the endpoint even when no
    build is published; ``available`` says whether it will return a file.
    """
    url = (
        update_download_url(platform)
        if artifact == PORTABLE
        else platform_download_url(platform)
    )
    if build is None or build_file_path(build) is None:
        return DownloadInfo(url=url)
    return DownloadInfo(
        available=True,
        version=build.version or "",
        filename=build.filename,
        size_bytes=build.size_bytes,
        url=url,
        release_notes=build.release_notes or "",
    )


@router.get("/landing", response_model=LandingResponse, summary="Landing page content")
def get_landing(db: Session = Depends(get_db)) -> LandingResponse:
    content = landing_content(get_landing_row(db))
    installers = active_builds(db, INSTALLER)
    portables = active_builds(db, PORTABLE)

    # ``download`` stays the Windows installer: the desktop client reads it and
    # the URL must keep working unchanged. ``downloads`` is the additive,
    # per-platform map used by the macOS button on the page. ``updates`` is the
    # portable channel the installed client uses for automatic updates.
    downloads = LandingDownloads(
        windows=download_info(installers[WINDOWS], WINDOWS, INSTALLER),
        macos=download_info(installers[MACOS], MACOS, INSTALLER),
    )
    updates = LandingDownloads(
        windows=download_info(portables[WINDOWS], WINDOWS, PORTABLE),
        macos=download_info(portables[MACOS], MACOS, PORTABLE),
    )
    artwork = public_site_config(db)

    return LandingResponse(
        blocks=blocks_from_content(content),
        hero_background=hero_background_from_content(content),
        screenshots=screenshots_from_content(content),
        download=downloads.windows or DownloadInfo(url=download_url()),
        downloads=downloads,
        updates=updates,
        hero_battle_url=artwork.hero_battle_url,
        hero_duel_url=artwork.hero_duel_url,
        default_avatar_url=artwork.default_avatar_url,
    )


@router.get(
    "/site/config",
    response_model=SiteConfigResponse,
    summary="Public artwork URLs used by the desktop client",
)
def get_site_config(db: Session = Depends(get_db)) -> SiteConfigResponse:
    """Administrator artwork slots: two HOME hero backgrounds and the avatar.

    ``null`` means "not uploaded": the client keeps its procedural artwork and
    the initial-letter avatar. The fields are additive — a server without this
    endpoint leaves the client on its built-in fallbacks.
    """
    return public_site_config(db)


@router.get(
    "/site/contacts",
    response_model=SiteContacts,
    summary="Contacts block shown at the bottom of the public pages",
)
def get_contacts(db: Session = Depends(get_db)) -> SiteContacts:
    content = landing_content(get_landing_row(db))
    return public_contacts(contacts_update_from_content(content))


@router.get("/downloads/game", summary="Download the current desktop installer")
def download_game_build(
    platform: PlatformParam = Query(
        default=DEFAULT_GAME_BUILD_PLATFORM,
        description="windows (default) or macos",
    ),
    db: Session = Depends(get_db),
) -> FileResponse:
    # The public button installs the game, so it always serves the installer:
    # portable ZIPs are only reachable through /downloads/game/update.
    build = active_build(db, platform, INSTALLER)
    if build is None:
        # The Windows message is unchanged: older clients only ever ask for the
        # default platform.
        detail = "No game build is available"
        if platform != DEFAULT_GAME_BUILD_PLATFORM:
            detail = f"No game build is available for {platform}"
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=detail,
        )
    target = build_file_path(build)
    if target is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail="Stored game build is missing",
        )
    return FileResponse(
        path=target,
        media_type=build.content_type or "application/octet-stream",
        filename=build.filename or target.name,
    )


@router.get(
    "/downloads/game/update",
    summary="Download the portable ZIP used for automatic updates",
)
def download_game_update(
    platform: PlatformParam = Query(
        default=DEFAULT_GAME_BUILD_PLATFORM,
        description="windows (default) or macos",
    ),
    db: Session = Depends(get_db),
) -> FileResponse:
    build = active_build(db, platform, PORTABLE)
    if build is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"No portable game build is available for {platform}",
        )
    target = build_file_path(build)
    if target is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail="Stored game build is missing",
        )
    return FileResponse(
        path=target,
        media_type=build.content_type or "application/zip",
        filename=build.filename or target.name,
    )


@router.get("/geo", response_model=GeoResponse, summary="Country and language by IP")
def get_geo(request: Request) -> GeoResponse:
    country = country_for_ip(client_ip(request))
    return GeoResponse(country=country, language=language_for_country(country))
