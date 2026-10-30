"""Pydantic schemas for the support page CMS and public endpoint."""
from __future__ import annotations

from datetime import datetime
from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field, field_validator

from app.services.support_content import (
    SupportValidationError,
    normalize_external_url,
)

SUPPORT_SECTIONS: tuple[str, ...] = ("hero", "cta")
SUPPORT_BUTTON_STYLES: tuple[str, ...] = ("primary", "secondary", "link")


class SupportMethodPublic(BaseModel):
    """A support link as rendered on the public page."""

    model_config = ConfigDict(extra="forbid")

    key: str
    title: str = ""
    button_label: str = ""
    url: str
    description: str = ""
    open_in_new_tab: bool = True
    style: str = "primary"
    sections: list[str] = Field(default_factory=lambda: ["hero", "cta"])


class SupportMethodAdmin(SupportMethodPublic):
    id: int
    is_active: bool = True
    sort_order: int = 0
    created_at: datetime
    updated_at: datetime


class SupportMethodList(BaseModel):
    items: list[SupportMethodAdmin] = Field(default_factory=list)


class SupportMethodCreate(BaseModel):
    """Editable fields of a support method (``id`` is assigned by the server)."""

    model_config = ConfigDict(extra="forbid")

    key: str = Field(min_length=2, max_length=40, pattern=r"^[a-z0-9][a-z0-9_-]*$")
    title: str = Field(default="", max_length=120)
    button_label: str = Field(min_length=1, max_length=160)
    url: str = Field(max_length=2048)
    description: str = Field(default="", max_length=500)
    is_active: bool = True
    open_in_new_tab: bool = True
    sort_order: int = Field(default=0, ge=0, le=10_000)
    style: Literal["primary", "secondary", "link"] = "primary"
    sections: list[str] = Field(default_factory=lambda: ["hero", "cta"])

    @field_validator("url")
    @classmethod
    def _valid_url(cls, value: str) -> str:
        try:
            return normalize_external_url(value, "url", required=True)
        except SupportValidationError as exc:
            raise ValueError(str(exc)) from exc

    @field_validator("sections")
    @classmethod
    def _valid_sections(cls, value: list[str]) -> list[str]:
        cleaned: list[str] = []
        for item in value:
            key = str(item).strip().lower()
            if key not in SUPPORT_SECTIONS:
                raise ValueError(
                    "sections may only contain: " + ", ".join(SUPPORT_SECTIONS)
                )
            if key not in cleaned:
                cleaned.append(key)
        # An empty list is valid: the method is active but shown nowhere.
        return cleaned

    @field_validator("title", "button_label", "description")
    @classmethod
    def _no_control_characters(cls, value: str) -> str:
        if any(ord(char) < 32 or ord(char) == 127 for char in value):
            raise ValueError("must not contain control characters")
        return value.strip()

    @field_validator("button_label")
    @classmethod
    def _require_button_label(cls, value: str) -> str:
        if not value:
            raise ValueError("is required")
        return value


class SupportMethodUpdate(SupportMethodCreate):
    """Same contract as create; the key stays unique across methods."""


class SupportMethodOrder(BaseModel):
    model_config = ConfigDict(extra="forbid")

    ids: list[int] = Field(min_length=1, max_length=100)


class SupportContentUpdate(BaseModel):
    """Draft save with optimistic locking.

    ``revision`` is the revision the editor loaded; a mismatch means somebody
    else saved meanwhile and the request is rejected with ``409 Conflict``.
    """

    model_config = ConfigDict(extra="forbid")

    revision: int = Field(ge=0)
    content: dict[str, Any]
    comment: str = Field(default="", max_length=500)


class SupportPublishRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")

    comment: str = Field(default="", max_length=500)


class SupportCodeUpdate(BaseModel):
    """The code tab: sanitised HTML fragment and validated CSS."""

    model_config = ConfigDict(extra="forbid")

    revision: int = Field(ge=0)
    custom_html: str = Field(default="", max_length=65_536)
    custom_css: str = Field(default="", max_length=65_536)


class SupportAdminState(BaseModel):
    """Everything the editor needs in one response."""

    revision: int
    published_revision: int
    draft_updated_at: datetime
    draft_updated_by: str | None = None
    published_at: datetime | None = None
    published_by: str | None = None
    has_unpublished_changes: bool = False
    content: dict[str, Any]
    methods: list[SupportMethodAdmin] = Field(default_factory=list)


class SupportDraftResponse(BaseModel):
    revision: int
    content: dict[str, Any]
    sanitized: bool = False
    warnings: list[str] = Field(default_factory=list)
    draft_updated_at: datetime | None = None
    draft_updated_by: str | None = None


class SupportPublishResponse(BaseModel):
    revision: int
    published_revision: int
    published_at: datetime
    has_unpublished_changes: bool = False


class SupportVersionItem(BaseModel):
    id: int
    revision: int
    created_at: datetime
    created_by: str | None = None
    comment: str = ""


class SupportVersionList(BaseModel):
    items: list[SupportVersionItem] = Field(default_factory=list)
    total: int = 0


class SupportPublicResponse(BaseModel):
    """Published page payload, already resolved to one language."""

    lang: Literal["ru", "en"]
    revision: int
    updated_at: datetime | None = None
    order: list[str] = Field(default_factory=list)
    sections: dict[str, Any] = Field(default_factory=dict)
    methods: list[SupportMethodPublic] = Field(default_factory=list)
    code: dict[str, str] = Field(default_factory=dict)


class SupportEventRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")

    event: Literal["support_view", "support_click", "support_faq_open"]
    method_key: str = Field(default="", max_length=40)

    @field_validator("method_key")
    @classmethod
    def _safe_key(cls, value: str) -> str:
        key = value.strip().lower()
        if not key:
            return ""
        if not all(char.isalnum() or char in "-_" for char in key):
            raise ValueError("must contain only letters, digits, '-' and '_'")
        return key


class SupportEventStats(BaseModel):
    days: int
    views: int = 0
    clicks: int = 0
    faq_opens: int = 0
    by_method: list[dict[str, Any]] = Field(default_factory=list)
