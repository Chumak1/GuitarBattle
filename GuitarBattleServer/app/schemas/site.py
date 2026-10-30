from datetime import datetime
from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field


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


class LandingResponse(BaseModel):
    blocks: dict[str, Any] = Field(default_factory=dict)
    hero_background: HeroBackground = Field(default_factory=HeroBackground)
    download: DownloadInfo = Field(default_factory=DownloadInfo)


class GeoResponse(BaseModel):
    country: str | None = None
    language: str | None = None


class AdminLandingUpdate(BaseModel):
    blocks: dict[str, Any] = Field(default_factory=dict)
    hero_background: HeroBackground | None = None


class AdminLandingResponse(BaseModel):
    blocks: dict[str, Any] = Field(default_factory=dict)
    hero_background: HeroBackground = Field(default_factory=HeroBackground)


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
