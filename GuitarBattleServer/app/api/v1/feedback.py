"""Public feedback endpoint used by the desktop client's "Send feedback" dialog.

The endpoint is anonymous-friendly: a signed-in player's token is optional and
only used to fill in the username and the ``Reply-To`` address. Delivery goes
through the same SMTP configuration as verification emails; the message body
and attachment are never logged.

Contract (shared with the JUCE client):

* ``POST /api/v1/feedback``
* request ``{"message": "...", "username": str|null, "app_version": str|null,
  "attachment_name": str|null, "attachment_data": base64 str|null}``
* ``200 {"ok": true}`` on success
* ``503 {"ok": false, "detail": "mail is not configured"}`` when SMTP is not
  configured, and ``503 {"ok": false, "detail": "mail delivery failed"}`` when
  the SMTP server rejects the message
* ``422`` for an empty or over-long message, invalid base64, or an attachment
  that decodes to more than 5 MiB
"""
from __future__ import annotations

import logging

from fastapi import APIRouter, Depends
from fastapi.responses import JSONResponse

from app.api.v1.auth import get_optional_current_user
from app.config import settings
from app.models.user import User
from app.schemas.feedback import (
    DEFAULT_ATTACHMENT_NAME,
    FeedbackRequest,
    FeedbackResponse,
    decode_attachment_data,
)
from app.services.email_service import EmailDeliveryError, send_feedback_email

logger = logging.getLogger(__name__)

router = APIRouter(tags=["feedback"])

MAIL_NOT_CONFIGURED = "mail is not configured"
MAIL_DELIVERY_FAILED = "mail delivery failed"


@router.post(
    "/feedback",
    response_model=FeedbackResponse,
    response_model_exclude_none=True,
    summary="Send player feedback to the project inbox",
)
def submit_feedback(
    payload: FeedbackRequest,
    current_user: User | None = Depends(get_optional_current_user),
) -> FeedbackResponse | JSONResponse:
    """Email one feedback message to ``FEEDBACK_EMAIL_TO``.

    A broken or missing token is treated as an anonymous request. The account
    email is used only as ``Reply-To``; the request body cannot override it.
    """
    if not settings.smtp_host:
        # Do not call the service: in development it logs the whole body, and
        # feedback must never end up in the application log.
        logger.warning("Feedback received but SMTP is not configured")
        return JSONResponse(
            status_code=503,
            content={"ok": False, "detail": MAIL_NOT_CONFIGURED},
        )

    username = payload.username
    if not username and current_user is not None:
        username = current_user.username

    # Reply address comes from the verified bearer token only. Anonymous
    # requests keep no reply address at all.
    reply_to = current_user.email if current_user is not None else None

    try:
        attachment_bytes = decode_attachment_data(payload.attachment_data)
    except ValueError as exc:
        # Small, payload-free error body: the default Pydantic 422 handler would
        # echo the whole base64 string (up to several megabytes) back.
        return JSONResponse(
            status_code=422,
            content={"detail": str(exc)},
        )

    try:
        send_feedback_email(
            message=payload.message,
            username=username,
            email=reply_to,
            app_version=payload.app_version,
            attachment_name=(
                payload.attachment_name
                or (DEFAULT_ATTACHMENT_NAME if attachment_bytes else None)
            ),
            attachment_data=attachment_bytes,
        )
    except EmailDeliveryError:
        # No message body, username, address or attachment name in the log.
        logger.warning("Feedback email could not be delivered")
        return JSONResponse(
            status_code=503,
            content={"ok": False, "detail": MAIL_DELIVERY_FAILED},
        )

    logger.info("Feedback email accepted for delivery")
    return FeedbackResponse(ok=True)
