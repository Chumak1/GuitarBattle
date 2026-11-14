"""Character ownership and server-side campaign ("Tower riff") progress.

Migration ``0029_character_unlocks`` adds both tables:

* ``user_characters`` — the player's collection. A character is owned when it is
  a starter (``ui_characters.is_starter``) or has a row here. Rewards granted by
  a tower store the granting campaign in ``source_campaign_id``; starters and the
  character an old profile already had selected keep it ``NULL``.
* ``user_campaign_progress`` — the highest cleared stage per tower and the
  completion timestamp. The client reports progress after a won stage, but the
  server clamps it to the published stage count and grants the reward itself, so
  a client can never unlock a character by asking for it.

Deletion rules: both tables cascade from ``users`` and ``campaigns``; deleting a
character removes its ownership rows (``ON DELETE CASCADE``), while deleting a
campaign only clears the provenance of rewards already granted
(``ON DELETE SET NULL``).
"""
import uuid
from datetime import datetime

from sqlalchemy import (
    CheckConstraint,
    DateTime,
    ForeignKey,
    Index,
    Integer,
    func,
)
from sqlalchemy.dialects.postgresql import UUID
from sqlalchemy.orm import Mapped, mapped_column

from app.database import Base


class UserCharacter(Base):
    """One owned character of one account."""

    __tablename__ = "user_characters"
    __table_args__ = (
        Index("ix_user_characters_character_id", "character_id"),
    )

    user_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("users.id", ondelete="CASCADE"),
        primary_key=True,
    )
    character_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("ui_characters.id", ondelete="CASCADE"),
        primary_key=True,
    )
    unlocked_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), nullable=False
    )
    #: Tower that rewarded this character; ``NULL`` for starters and for the
    #: character an existing profile had selected before migration 0029.
    source_campaign_id: Mapped[uuid.UUID | None] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("campaigns.id", ondelete="SET NULL"),
        nullable=True,
    )


class UserCampaignProgress(Base):
    """Highest cleared stage of one tower for one account."""

    __tablename__ = "user_campaign_progress"
    __table_args__ = (
        CheckConstraint(
            "best_stage >= 0",
            name="ck_user_campaign_progress_best_stage",
        ),
        Index("ix_user_campaign_progress_campaign_id", "campaign_id"),
    )

    user_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("users.id", ondelete="CASCADE"),
        primary_key=True,
    )
    campaign_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("campaigns.id", ondelete="CASCADE"),
        primary_key=True,
    )
    #: Cleared stages of the current run: a win advances it and a loss resets it
    #: to ``0``. A replay of a finished tower continues the raw count instead of
    #: resetting it, so ``best_stage % stage_count`` is the stage being played.
    best_stage: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0, server_default="0"
    )
    #: Number of **full** tower completions. Drives the Battle XP decay
    #: (``reward_config.tower_repeat_factor``) and survives re-logins.
    completion_count: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0, server_default="0"
    )
    #: Set once, when the final published stage is cleared. Never cleared again,
    #: so a reward cannot be lost by replaying or by a stale client report.
    completed_at: Mapped[datetime | None] = mapped_column(
        DateTime(timezone=True), nullable=True
    )
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        server_default=func.now(),
        onupdate=func.now(),
        nullable=False,
    )
