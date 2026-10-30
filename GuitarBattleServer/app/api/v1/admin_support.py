"""Administrator endpoints for the support page CMS.

Draft saves never touch the public page; ``publish`` copies the draft into the
published snapshot and records a version. Every route requires an administrator
JWT, and every write is guarded by the document revision (``409 Conflict``).
"""
from __future__ import annotations

import logging
from datetime import datetime, timedelta, timezone
from typing import Any, Literal

from fastapi import APIRouter, Depends, HTTPException, Query, Response, status
from sqlalchemy import func, select
from sqlalchemy.orm import Session

from app.api.v1.admin_songs import require_admin
from app.database import get_db
from app.models.support_page import (
    SupportEventCounter,
    SupportMethod,
    SupportPage,
    SupportPageVersion,
)
from app.models.user import User
from app.schemas.support import (
    SupportAdminState,
    SupportCodeUpdate,
    SupportContentUpdate,
    SupportDraftResponse,
    SupportEventStats,
    SupportMethodAdmin,
    SupportMethodCreate,
    SupportMethodList,
    SupportMethodOrder,
    SupportMethodUpdate,
    SupportPublicResponse,
    SupportPublishRequest,
    SupportPublishResponse,
    SupportVersionItem,
    SupportVersionList,
)
from app.services.support_content import (
    SupportValidationError,
    build_public_payload,
    ensure_support_seed,
    normalize_content,
    normalize_custom_css,
    read_content,
    sanitize_html,
    serialize_method_payload,
)

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/admin/support", tags=["admin: support"])


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def _validation_error(exc: SupportValidationError) -> HTTPException:
    # ``str()`` is ``"field: message"``: names the field without leaking values.
    return HTTPException(
        status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail=str(exc)
    )


def _username(db: Session, user_id) -> str | None:
    if user_id is None:
        return None
    return db.scalar(select(User.username).where(User.id == user_id))


def _conflict(db: Session, row: SupportPage) -> HTTPException:
    editor = _username(db, row.draft_updated_by) or "another administrator"
    when = (
        row.draft_updated_at.strftime("%Y-%m-%d %H:%M UTC")
        if row.draft_updated_at
        else "an unknown time"
    )
    return HTTPException(
        status_code=status.HTTP_409_CONFLICT,
        detail=(
            f"Draft revision {row.revision} was saved by {editor} at {when}. "
            "Reload the draft before saving again."
        ),
    )


def _require_revision(row: SupportPage, expected: int, db: Session) -> None:
    if row.revision != expected:
        raise _conflict(db, row)


def _methods(db: Session) -> list[SupportMethod]:
    return list(
        db.scalars(
            select(SupportMethod).order_by(SupportMethod.sort_order, SupportMethod.id)
        ).all()
    )


def _method_admin(method: SupportMethod) -> SupportMethodAdmin:
    return SupportMethodAdmin.model_validate(serialize_method_payload(method))


def _admin_state(db: Session, row: SupportPage) -> SupportAdminState:
    return SupportAdminState(
        revision=row.revision,
        published_revision=row.published_revision,
        draft_updated_at=row.draft_updated_at,
        draft_updated_by=_username(db, row.draft_updated_by),
        published_at=row.published_at,
        published_by=_username(db, row.published_by),
        has_unpublished_changes=row.revision != row.published_revision,
        content=read_content(row.draft_content),
        methods=[_method_admin(method) for method in _methods(db)],
    )


def _draft_response(
    content: dict[str, Any],
    revision: int,
    *,
    sanitized: bool = False,
    warnings: list[str] | None = None,
    updated_at: datetime | None = None,
    updated_by: str | None = None,
) -> SupportDraftResponse:
    return SupportDraftResponse(
        revision=revision,
        content=content,
        sanitized=sanitized,
        warnings=warnings or [],
        draft_updated_at=updated_at,
        draft_updated_by=updated_by,
    )


def _code_was_sanitized(submitted: Any, stored: dict[str, str]) -> bool:
    raw = submitted if isinstance(submitted, dict) else {}
    raw_html = raw.get("custom_html")
    raw_css = raw.get("custom_css")
    if isinstance(raw_html, str) and raw_html.strip() != stored.get("custom_html", ""):
        return True
    if isinstance(raw_css, str) and raw_css.strip() != stored.get("custom_css", ""):
        return True
    return False


# ---------------------------------------------------------------------------
# Document
# ---------------------------------------------------------------------------
@router.get("", response_model=SupportAdminState, summary="Read the draft and its history state")
def get_admin_state(
    db: Session = Depends(get_db), _: User = Depends(require_admin)
) -> SupportAdminState:
    row = ensure_support_seed(db)
    return _admin_state(db, row)


@router.put("/draft", response_model=SupportDraftResponse, summary="Save the draft")
def save_draft(
    payload: SupportContentUpdate,
    db: Session = Depends(get_db),
    user: User = Depends(require_admin),
) -> SupportDraftResponse:
    row = ensure_support_seed(db)
    _require_revision(row, payload.revision, db)

    try:
        content = normalize_content(payload.content)
    except SupportValidationError as exc:
        raise _validation_error(exc) from exc

    warnings: list[str] = []
    sanitized = _code_was_sanitized(
        (payload.content or {}).get("code"), content["code"]
    )
    if sanitized:
        warnings.append(
            "HTML/CSS were cleaned: scripts, event handlers, unsafe URLs and "
            "external resources are not allowed."
        )

    row.draft_content = content
    row.revision = row.revision + 1
    row.draft_updated_at = datetime.now(timezone.utc)
    row.draft_updated_by = user.id
    db.commit()
    db.refresh(row)
    logger.info(
        "Support draft saved by admin %s at revision %s", user.id, row.revision
    )
    return _draft_response(
        content,
        row.revision,
        sanitized=sanitized,
        warnings=warnings,
        updated_at=row.draft_updated_at,
        updated_by=user.username,
    )


@router.put("/code", response_model=SupportDraftResponse, summary="Save the custom HTML/CSS tab")
def save_code(
    payload: SupportCodeUpdate,
    db: Session = Depends(get_db),
    user: User = Depends(require_admin),
) -> SupportDraftResponse:
    row = ensure_support_seed(db)
    _require_revision(row, payload.revision, db)

    content = read_content(row.draft_content)
    submitted = {
        "custom_html": payload.custom_html,
        "custom_css": payload.custom_css,
    }
    try:
        cleaned_css = normalize_custom_css(payload.custom_css)
    except SupportValidationError as exc:
        raise _validation_error(exc) from exc
    cleaned_html = sanitize_html(payload.custom_html)
    content["code"] = {"custom_css": cleaned_css, "custom_html": cleaned_html}

    warnings: list[str] = []
    if cleaned_html.strip() != payload.custom_html.strip():
        warnings.append(
            "HTML was cleaned: scripts, event handlers, unsafe URLs and "
            "non-whitelisted tags are removed."
        )
    if cleaned_css != payload.custom_css.strip():
        warnings.append("CSS was normalised (leading/trailing whitespace removed).")

    row.draft_content = content
    row.revision = row.revision + 1
    row.draft_updated_at = datetime.now(timezone.utc)
    row.draft_updated_by = user.id
    db.commit()
    db.refresh(row)
    logger.info("Support code saved by admin %s at revision %s", user.id, row.revision)
    return _draft_response(
        content,
        row.revision,
        sanitized=bool(warnings),
        warnings=warnings,
        updated_at=row.draft_updated_at,
        updated_by=user.username,
    )


@router.post("/publish", response_model=SupportPublishResponse, summary="Publish the draft")
def publish_draft(
    payload: SupportPublishRequest,
    db: Session = Depends(get_db),
    user: User = Depends(require_admin),
) -> SupportPublishResponse:
    row = ensure_support_seed(db)
    try:
        content = normalize_content(row.draft_content)
    except SupportValidationError as exc:
        raise _validation_error(exc) from exc

    now = datetime.now(timezone.utc)
    row.draft_content = content
    row.published_content = content
    row.published_revision = row.revision
    row.published_at = now
    row.published_by = user.id
    db.add(
        SupportPageVersion(
            content=content,
            revision=row.revision,
            created_by=user.id,
            comment=(payload.comment or "").strip(),
        )
    )
    db.commit()
    db.refresh(row)
    logger.info(
        "Support page published by admin %s at revision %s", user.id, row.revision
    )
    return SupportPublishResponse(
        revision=row.revision,
        published_revision=row.published_revision,
        published_at=now,
        has_unpublished_changes=False,
    )


@router.get(
    "/preview",
    response_model=SupportPublicResponse,
    summary="Render the current draft (administrator only)",
)
def preview_draft(
    lang: Literal["ru", "en"] = Query(default="ru"),
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> SupportPublicResponse:
    """Same projection as the public endpoint, but built from the draft."""
    row = ensure_support_seed(db)
    payload = build_public_payload(
        row.draft_content,
        _methods(db),
        lang,
        revision=row.revision,
        updated_at=row.draft_updated_at,
    )
    return SupportPublicResponse.model_validate(payload)


# ---------------------------------------------------------------------------
# Version history
# ---------------------------------------------------------------------------
@router.get("/versions", response_model=SupportVersionList, summary="List published versions")
def list_versions(
    limit: int = Query(default=50, ge=1, le=200),
    offset: int = Query(default=0, ge=0),
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> SupportVersionList:
    total = db.scalar(select(func.count(SupportPageVersion.id))) or 0
    rows = db.execute(
        select(SupportPageVersion, User.username)
        .outerjoin(User, User.id == SupportPageVersion.created_by)
        .order_by(SupportPageVersion.created_at.desc(), SupportPageVersion.id.desc())
        .offset(offset)
        .limit(limit)
    ).all()
    return SupportVersionList(
        items=[
            SupportVersionItem(
                id=version.id,
                revision=version.revision,
                created_at=version.created_at,
                created_by=username,
                comment=version.comment or "",
            )
            for version, username in rows
        ],
        total=total,
    )


@router.post(
    "/versions/{version_id}/restore",
    response_model=SupportDraftResponse,
    summary="Restore a version into the draft (does not publish)",
)
def restore_version(
    version_id: int,
    db: Session = Depends(get_db),
    user: User = Depends(require_admin),
) -> SupportDraftResponse:
    version = db.get(SupportPageVersion, version_id)
    if version is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND, detail="Version not found"
        )
    row = ensure_support_seed(db)
    try:
        content = normalize_content(version.content)
    except SupportValidationError:
        content = read_content(version.content)

    row.draft_content = content
    row.revision = row.revision + 1
    row.draft_updated_at = datetime.now(timezone.utc)
    row.draft_updated_by = user.id
    db.commit()
    db.refresh(row)
    logger.info(
        "Support page version %s restored into the draft by admin %s at revision %s",
        version.id,
        user.id,
        row.revision,
    )
    return _draft_response(
        content,
        row.revision,
        updated_at=row.draft_updated_at,
        updated_by=user.username,
    )


# ---------------------------------------------------------------------------
# Support methods
# ---------------------------------------------------------------------------
@router.get("/methods", response_model=SupportMethodList, summary="List support methods")
def list_methods(
    db: Session = Depends(get_db), _: User = Depends(require_admin)
) -> SupportMethodList:
    ensure_support_seed(db)
    return SupportMethodList(items=[_method_admin(item) for item in _methods(db)])


@router.post(
    "/methods",
    response_model=SupportMethodAdmin,
    status_code=status.HTTP_201_CREATED,
    summary="Create a support method",
)
def create_method(
    payload: SupportMethodCreate,
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> SupportMethodAdmin:
    ensure_support_seed(db)
    if db.scalar(select(SupportMethod.id).where(SupportMethod.key == payload.key)):
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail=f"A support method with key {payload.key!r} already exists",
        )
    method = SupportMethod(**payload.model_dump())
    if not method.sort_order:
        highest = db.scalar(select(func.max(SupportMethod.sort_order))) or 0
        method.sort_order = highest + 1
    db.add(method)
    try:
        db.commit()
    except Exception:
        db.rollback()
        raise
    db.refresh(method)
    return _method_admin(method)


@router.put(
    "/methods/order",
    response_model=SupportMethodList,
    summary="Reorder support methods",
)
def reorder_methods(
    payload: SupportMethodOrder,
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> SupportMethodList:
    ensure_support_seed(db)
    methods = _methods(db)
    by_id = {method.id: method for method in methods}
    if set(payload.ids) != set(by_id) or len(payload.ids) != len(by_id):
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail="ids must list every support method exactly once",
        )
    for index, method_id in enumerate(payload.ids):
        by_id[method_id].sort_order = index
    db.commit()
    return SupportMethodList(items=[_method_admin(item) for item in _methods(db)])


@router.put(
    "/methods/{method_id}",
    response_model=SupportMethodAdmin,
    summary="Update a support method",
)
def update_method(
    method_id: int,
    payload: SupportMethodUpdate,
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> SupportMethodAdmin:
    method = db.get(SupportMethod, method_id)
    if method is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND, detail="Support method not found"
        )
    duplicate = db.scalar(
        select(SupportMethod.id).where(
            SupportMethod.key == payload.key, SupportMethod.id != method_id
        )
    )
    if duplicate:
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail=f"A support method with key {payload.key!r} already exists",
        )
    for field, value in payload.model_dump().items():
        setattr(method, field, value)
    db.commit()
    db.refresh(method)
    return _method_admin(method)


@router.delete(
    "/methods/{method_id}",
    status_code=status.HTTP_204_NO_CONTENT,
    summary="Delete a support method",
)
def delete_method(
    method_id: int,
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> Response:
    method = db.get(SupportMethod, method_id)
    if method is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND, detail="Support method not found"
        )
    db.delete(method)
    db.commit()
    return Response(status_code=status.HTTP_204_NO_CONTENT)


# ---------------------------------------------------------------------------
# Analytics
# ---------------------------------------------------------------------------
@router.get(
    "/stats",
    response_model=SupportEventStats,
    summary="Aggregated support page events",
)
def support_stats(
    days: int = Query(default=30, ge=1, le=365),
    db: Session = Depends(get_db),
    _: User = Depends(require_admin),
) -> SupportEventStats:
    since = (datetime.now(timezone.utc) - timedelta(days=days)).date()
    rows = db.execute(
        select(
            SupportEventCounter.event,
            SupportEventCounter.method_key,
            func.sum(SupportEventCounter.count),
        )
        .where(SupportEventCounter.day >= since)
        .group_by(SupportEventCounter.event, SupportEventCounter.method_key)
    ).all()

    totals = {"support_view": 0, "support_click": 0, "support_faq_open": 0}
    per_method: dict[str, int] = {}
    for event, method_key, count in rows:
        value = int(count or 0)
        if event in totals:
            totals[event] += value
        if event == "support_click" and method_key:
            per_method[method_key] = per_method.get(method_key, 0) + value

    titles = {
        method.key: method.title or method.button_label or method.key
        for method in _methods(db)
    }
    return SupportEventStats(
        days=days,
        views=totals["support_view"],
        clicks=totals["support_click"],
        faq_opens=totals["support_faq_open"],
        by_method=[
            {"method_key": key, "title": titles.get(key, key), "clicks": clicks}
            for key, clicks in sorted(per_method.items(), key=lambda item: -item[1])
        ],
    )
