from datetime import datetime
from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field, field_validator

#: Hard cap of the landing screenshot gallery. The whole CMS document travels
#: to the browser on every visit, so the gallery cannot grow without a bound.
MAX_SCREENSHOTS = 12
#: Uploads of the gallery live next to the hero background and are served by the
#: same ``/media/landing`` mount.
SCREENSHOT_URL_PREFIX = "/media/landing/"
MAX_SCREENSHOT_CAPTION = 200


class HeroBackground(BaseModel):
    """The landing hero media. ``type=none`` disables the background layer."""

    model_config = ConfigDict(extra="forbid")

    type: Literal["none", "image", "video"] = "none"
    url: str | None = Field(default=None, max_length=2048)
    poster_url: str | None = Field(default=None, max_length=2048)


class DownloadInfo(BaseModel):
    """Public information about the currently active desktop build."""

    available: bool = False
    version: str = ""
    filename: str = ""
    size_bytes: int = 0
    url: str = ""
    release_notes: str = ""


class LandingDownloads(BaseModel):
    """Per-platform download links of the landing page.

    The keys are the ``?platform=`` values; a platform without a published
    build is ``null``. ``LandingResponse.download`` stays the Windows installer
    for backwards compatibility with the desktop client. The same shape is used
    for ``updates`` (the portable ZIPs of the automatic updater), so a client
    can parse both maps with one code path.
    """

    windows: DownloadInfo | None = None
    macos: DownloadInfo | None = None


class LandingScreenshot(BaseModel):
    """One administrator-uploaded screenshot of the game.

    ``url`` is always a file inside the landing media directory: the editor
    never types a URL by hand, it only reorders and captions uploaded files.
    The validators keep a hand-edited CMS document from turning the field into
    an arbitrary link or a path escape.
    """

    model_config = ConfigDict(extra="forbid")

    url: str = Field(..., max_length=512)
    caption_ru: str = ""
    caption_en: str = ""

    @field_validator("url")
    @classmethod
    def _check_url(cls, value: str) -> str:
        value = value.strip()
        if not value.startswith(SCREENSHOT_URL_PREFIX):
            raise ValueError(
                f"screenshot url must start with {SCREENSHOT_URL_PREFIX}"
            )
        name = value[len(SCREENSHOT_URL_PREFIX):]
        if (
            not name
            or name in {".", ".."}
            or "/" in name
            or "\\" in name
            or ".." in name
            or "?" in name
            or "#" in name
        ):
            raise ValueError(
                "screenshot url must name a file inside /media/landing/"
            )
        return value

    @field_validator("caption_ru", "caption_en")
    @classmethod
    def _clean_caption(cls, value: str) -> str:
        value = value.strip()
        if len(value) > MAX_SCREENSHOT_CAPTION:
            raise ValueError(
                f"screenshot caption is longer than {MAX_SCREENSHOT_CAPTION} characters"
            )
        return value


def _check_screenshot_list(
    value: list[LandingScreenshot] | None,
) -> list[LandingScreenshot] | None:
    """Shared limit/duplicate checks of the editable gallery."""
    if value is None:
        return None
    if len(value) > MAX_SCREENSHOTS:
        raise ValueError(f"at most {MAX_SCREENSHOTS} screenshots are allowed")
    urls = [item.url for item in value]
    if len(set(urls)) != len(urls):
        raise ValueError("screenshot urls must be unique")
    return value


class LandingResponse(BaseModel):
    blocks: dict[str, Any] = Field(default_factory=dict)
    hero_background: HeroBackground = Field(default_factory=HeroBackground)
    screenshots: list[LandingScreenshot] = Field(default_factory=list)
    download: DownloadInfo = Field(default_factory=DownloadInfo)
    downloads: LandingDownloads = Field(default_factory=LandingDownloads)
    #: Portable ZIPs used by the installed client to update itself. Additive:
    #: ``download``/``downloads`` keep pointing at the installers.
    updates: LandingDownloads = Field(default_factory=LandingDownloads)
    #: Additive: administrator artwork slots also exposed by ``/site/config``
    #: (``/media/artwork/<name>`` or ``null``). The desktop client reads the
    #: dedicated endpoint; the landing page ignores these fields.
    hero_battle_url: str | None = None
    hero_duel_url: str | None = None
    default_avatar_url: str | None = None


class GeoResponse(BaseModel):
    country: str | None = None
    language: str | None = None


class AdminLandingUpdate(BaseModel):
    blocks: dict[str, Any] = Field(default_factory=dict)
    hero_background: HeroBackground | None = None
    #: ``None`` keeps the gallery that is already stored; a list replaces it.
    screenshots: list[LandingScreenshot] | None = None

    @field_validator("screenshots")
    @classmethod
    def _check_screenshots(
        cls, value: list[LandingScreenshot] | None
    ) -> list[LandingScreenshot] | None:
        return _check_screenshot_list(value)


class AdminLandingResponse(BaseModel):
    blocks: dict[str, Any] = Field(default_factory=dict)
    hero_background: HeroBackground = Field(default_factory=HeroBackground)
    screenshots: list[LandingScreenshot] = Field(default_factory=list)


class ScreenshotsUpdate(BaseModel):
    """Replace the gallery: order and captions of the uploaded screenshots."""

    model_config = ConfigDict(extra="forbid")

    screenshots: list[LandingScreenshot] = Field(default_factory=list)

    @field_validator("screenshots")
    @classmethod
    def _check_screenshots(
        cls, value: list[LandingScreenshot]
    ) -> list[LandingScreenshot]:
        return _check_screenshot_list(value) or []


class ScreenshotsResponse(BaseModel):
    screenshots: list[LandingScreenshot] = Field(default_factory=list)


class HeroBackgroundResponse(BaseModel):
    hero_background: HeroBackground = Field(default_factory=HeroBackground)


class SiteSocials(BaseModel):
    """Links to the author's social profiles.

    ``None`` means "no profile yet": the public page renders a disabled
    placeholder icon instead of a link.
    """

    model_config = ConfigDict(extra="forbid")

    youtube: str | None = Field(default=None, max_length=2048)
    instagram: str | None = Field(default=None, max_length=2048)
    twitter: str | None = Field(default=None, max_length=2048)
    vk: str | None = Field(default=None, max_length=2048)


class SiteContactsUpdate(BaseModel):
    """What the administrator edits; also the admin ``GET`` response."""

    model_config = ConfigDict(extra="forbid")

    name_ru: str = Field(default="", max_length=120)
    name_en: str = Field(default="", max_length=120)
    email: str = Field(default="", max_length=254)
    telegram: str = Field(default="", max_length=128)
    socials: SiteSocials = Field(default_factory=SiteSocials)


class SiteContactsName(BaseModel):
    model_config = ConfigDict(extra="forbid")

    ru: str = ""
    en: str = ""


class SiteContacts(BaseModel):
    """Public contacts block served to the static pages."""

    model_config = ConfigDict(extra="forbid")

    name: SiteContactsName
    email: str = ""
    telegram: str = ""  # "@handle"
    telegram_url: str = ""  # "https://t.me/handle"
    socials: dict[str, str | None]


class GameBuildItem(BaseModel):
    id: int
    #: ``windows`` | ``macos``.
    platform: str
    #: ``installer`` (site download) | ``portable`` (auto-update ZIP).
    artifact: str = "installer"
    version: str = ""
    filename: str
    size_bytes: int
    content_type: str
    is_active: bool
    uploaded_at: datetime
    url: str
    release_notes: str = ""


class GameBuildUpdate(BaseModel):
    """Editable fields of an uploaded build; the installer itself is immutable."""

    model_config = ConfigDict(extra="forbid")

    release_notes: str = Field(default="", max_length=8000)


class GameBuildListResponse(BaseModel):
    items: list[GameBuildItem] = Field(default_factory=list)
