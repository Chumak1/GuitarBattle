"""Public support page content and privacy-free page telemetry."""
from __future__ import annotations

from datetime import datetime, timezone
from typing import Literal

from fastapi import APIRouter, Depends, Query, Response, status
from sqlalchemy import select
from sqlalchemy.dialects.postgresql import insert as pg_insert
from sqlalchemy.orm import Session

from app.database import get_db
from app.models.support_page import SupportEventCounter, SupportMethod
from app.schemas.support import SupportEventRequest, SupportPublicResponse
from app.services.support_content import build_public_payload, ensure_support_seed

router = APIRouter(tags=["support"])


@router.get(
    "/support",
    response_model=SupportPublicResponse,
    summary="Published support page content",
)
def get_support(
    lang: Literal["ru", "en"] = Query(default="ru"),
    db: Session = Depends(get_db),
) -> SupportPublicResponse:
    """Serve only the published snapshot, resolved to one language.

    Hidden sections are removed here rather than in the browser, so the page
    never receives content the administrator switched off.
    """
    row = ensure_support_seed(db)
    methods = db.scalars(
        select(SupportMethod).order_by(SupportMethod.sort_order, SupportMethod.id)
    ).all()
    payload = build_public_payload(
        row.published_content,
        methods,
        lang,
        revision=row.published_revision,
        updated_at=row.published_at,
    )
    return SupportPublicResponse.model_validate(payload)


@router.post(
    "/support/events",
    status_code=status.HTTP_204_NO_CONTENT,
    summary="Record a support page event (aggregated, no personal data)",
)
def record_support_event(
    payload: SupportEventRequest,
    db: Session = Depends(get_db),
) -> Response:
    """Increment a per-day counter.

    Only the event name, the method key and the UTC day are stored; the caller's
    address, account, cookies and user agent are never read. The endpoint is
    deliberately tolerant: analytics must never break the page.
    """
    today = datetime.now(timezone.utc).date()
    method_key = payload.method_key if payload.event == "support_click" else ""
    statement = (
        pg_insert(SupportEventCounter)
        .values(
            event=payload.event,
            method_key=method_key,
            day=today,
            count=1,
        )
        .on_conflict_do_update(
            index_elements=[
                SupportEventCounter.event,
                SupportEventCounter.method_key,
                SupportEventCounter.day,
            ],
            set_={"count": SupportEventCounter.count + 1},
        )
    )
    db.execute(statement)
    db.commit()
    return Response(status_code=status.HTTP_204_NO_CONTENT)
