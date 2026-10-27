"""Administrator-only account deletion.

Deleting an account is a hard delete. Every foreign key that points at
``users.id`` is ``ON DELETE CASCADE`` (game sessions and their scores, app
sessions, game launches, email codes, competitions), so one statement removes
the player and all of their data. The audit trail records ids only: no login,
email or other personal data is written to the log.
"""
import logging
from uuid import UUID

from fastapi import APIRouter, Depends, HTTPException, Response, status
from sqlalchemy import select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session

from app.api.v1.admin_songs import require_admin
from app.database import get_db
from app.models.user import User

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/admin/users", tags=["admin: users"])


@router.delete(
    "/{user_id}",
    status_code=status.HTTP_204_NO_CONTENT,
    summary="Delete a user account and all of its data",
)
def delete_user(
    user_id: UUID,
    db: Session = Depends(get_db),
    current_user: User = Depends(require_admin),
) -> Response:
    """Permanently delete one account.

    An administrator cannot delete their own account: that would lock the panel
    out mid-session and is almost always a misclick. Data that a future schema
    change protects with ``RESTRICT`` is reported as 409 instead of a 500.
    """
    if user_id == current_user.id:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="You cannot delete your own account",
        )

    user = db.scalar(select(User).where(User.id == user_id))
    if user is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="User not found")

    db.delete(user)
    try:
        db.commit()
    except IntegrityError:
        db.rollback()
        logger.warning(
            "admin %s could not delete user %s: related data blocks the delete",
            current_user.id,
            user_id,
        )
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail="User is referenced by data that cannot be deleted",
        )

    logger.info("admin %s deleted user %s", current_user.id, user_id)
    return Response(status_code=status.HTTP_204_NO_CONTENT)
