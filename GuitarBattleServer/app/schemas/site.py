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


class GameBuildItem(BaseModel):
    id: int
    version: str = ""
    filename: str
    size_bytes: int
    content_type: str
    is_active: bool
    uploaded_at: datetime
    url: str


class GameBuildListResponse(BaseModel):
    items: list[GameBuildItem] = Field(default_factory=list)
