"""Guitar Duels: matches, participants, events and ratings.

The tables are additive (migration ``0021_duels``). Existing user, song and
score tables are not touched.

A match is a small state machine:

``waiting`` -> ``playing`` -> ``finished``
                     \\-> ``cancelled`` (left or no opponent)

The server owns HP, damage, the winner, XP and the Elo change. Clients only
submit ``(note_index, quality)``; every number in this schema is derived from
those events by :mod:`app.services.duel_rules`.
"""
from __future__ import annotations

import uuid
from datetime import datetime

from sqlalchemy import (
    BigInteger,
    Boolean,
    CheckConstraint,
    DateTime,
    Float,
    ForeignKey,
    Index,
    Integer,
    String,
    UniqueConstraint,
    func,
)
from sqlalchemy.dialects.postgresql import JSONB, UUID
from sqlalchemy.orm import Mapped, mapped_column

from app.database import Base


class DuelMatch(Base):
    __tablename__ = "duel_matches"
    __table_args__ = (
        CheckConstraint("mode IN ('bot', 'online')", name="ck_duel_matches_mode"),
        CheckConstraint(
            "status IN ('waiting', 'playing', 'finished', 'cancelled')",
            name="ck_duel_matches_status",
        ),
        CheckConstraint("note_count >= 0", name="ck_duel_matches_note_count"),
        CheckConstraint(
            "next_note_index >= 0", name="ck_duel_matches_next_note_index"
        ),
        # Queue lookup: waiting online matches of one song, oldest first.
        Index(
            "ix_duel_matches_status_mode_created",
            "status",
            "mode",
            "created_at",
        ),
        # History and period leaderboards.
        Index("ix_duel_matches_finished_at", "finished_at"),
        Index("ix_duel_matches_song_id", "song_id"),
    )

    id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True), primary_key=True, default=uuid.uuid4
    )
    song_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        # A match is history: deleting a song must never silently delete it.
        # Songs are archived in this project, not hard-deleted.
        ForeignKey("songs.id", ondelete="RESTRICT"),
        nullable=False,
    )
    mode: Mapped[str] = mapped_column(String(16), nullable=False)
    #: ``easy``/``normal``/``hard`` for a bot match, ``NULL`` for online.
    bot_level: Mapped[str | None] = mapped_column(String(16), nullable=True)
    status: Mapped[str] = mapped_column(
        String(16), nullable=False, default="waiting", server_default="waiting"
    )
    #: Rules snapshot. The server rejects a match whose version is unknown.
    rules_version: Mapped[int] = mapped_column(
        Integer, nullable=False, default=2, server_default="2"
    )
    #: Deterministic bot seed (32-bit unsigned stored in a wider column).
    seed: Mapped[int] = mapped_column(BigInteger, nullable=False)
    #: Length of the frozen chart snapshot.
    note_count: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0, server_default="0"
    )
    #: Note start offsets in milliseconds, frozen when the match is created.
    #: ``NULL`` means the song has no analysed MIDI: the server then validates
    #: only the index range and a broad match window (legacy behaviour).
    note_times_ms: Mapped[list[int] | None] = mapped_column(JSONB, nullable=True)
    #: First chart note that has not been resolved yet. Resolution is strictly
    #: in chart order, independently of the order events arrive in.
    next_note_index: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0, server_default="0"
    )
    started_at: Mapped[datetime | None] = mapped_column(
        DateTime(timezone=True), nullable=True
    )
    finished_at: Mapped[datetime | None] = mapped_column(
        DateTime(timezone=True), nullable=True
    )
    winner_user_id: Mapped[uuid.UUID | None] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("users.id", ondelete="SET NULL"),
        nullable=True,
    )
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), nullable=False
    )
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        server_default=func.now(),
        onupdate=func.now(),
        nullable=False,
    )


class DuelParticipant(Base):
    __tablename__ = "duel_participants"
    __table_args__ = (
        UniqueConstraint("match_id", "side", name="uq_duel_participants_match_side"),
        CheckConstraint(
            "side IN ('player', 'opponent')", name="ck_duel_participants_side"
        ),
        CheckConstraint(
            "result IS NULL OR result IN ('win', 'loss', 'draw')",
            name="ck_duel_participants_result",
        ),
        CheckConstraint(
            "hp_final >= 0 AND hp_final <= 100", name="ck_duel_participants_hp"
        ),
        Index("ix_duel_participants_user_id", "user_id"),
    )

    id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True), primary_key=True, default=uuid.uuid4
    )
    match_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("duel_matches.id", ondelete="CASCADE"),
        nullable=False,
    )
    #: ``NULL`` for the bot side.
    user_id: Mapped[uuid.UUID | None] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("users.id", ondelete="SET NULL"),
        nullable=True,
    )
    side: Mapped[str] = mapped_column(String(8), nullable=False)
    is_bot: Mapped[bool] = mapped_column(
        Boolean, nullable=False, default=False, server_default="false"
    )
    #: Display identity of the bot side (``NULL`` for humans).
    bot_name: Mapped[str | None] = mapped_column(String(32), nullable=True)
    bot_avatar: Mapped[str | None] = mapped_column(String(32), nullable=True)

    #: Set when this side reported ready. The bot side is ready immediately.
    ready_at: Mapped[datetime | None] = mapped_column(
        DateTime(timezone=True), nullable=True
    )
    #: Set when this side asked to finish. Online matches finalise when both
    #: sides asked (or when a side leaves).
    finish_requested_at: Mapped[datetime | None] = mapped_column(
        DateTime(timezone=True), nullable=True
    )
    #: Set when this side left an online match; used for the technical loss.
    left_at: Mapped[datetime | None] = mapped_column(
        DateTime(timezone=True), nullable=True
    )
    joined_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), nullable=False
    )
    last_seen_at: Mapped[datetime | None] = mapped_column(
        DateTime(timezone=True), nullable=True
    )

    #: Current HP while the match is playing and the final HP afterwards.
    hp_final: Mapped[float] = mapped_column(
        Float, nullable=False, default=100.0, server_default="100"
    )
    perfect_count: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0, server_default="0"
    )
    great_count: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0, server_default="0"
    )
    good_count: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0, server_default="0"
    )
    miss_count: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0, server_default="0"
    )
    #: How many chart notes were resolved for this side (including auto-miss).
    notes_played: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0, server_default="0"
    )
    damage_dealt: Mapped[float] = mapped_column(
        Float, nullable=False, default=0.0, server_default="0"
    )
    damage_taken: Mapped[float] = mapped_column(
        Float, nullable=False, default=0.0, server_default="0"
    )

    xp_awarded: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0, server_default="0"
    )
    #: Elo snapshots. ``NULL`` for bots and for matches that never changed the
    #: rating. ``rating_before`` is also written at queue time (online) so the
    #: matchmaking window can be checked without recomputing the rating.
    rating_before: Mapped[int | None] = mapped_column(Integer, nullable=True)
    rating_after: Mapped[int | None] = mapped_column(Integer, nullable=True)
    result: Mapped[str | None] = mapped_column(String(8), nullable=True)


class DuelEvent(Base):
    """One client-reported chart note.

    ``UNIQUE(match_id, user_id, note_index)`` is the idempotency guard: retries
    and reconnects can resend a batch without applying a note twice.
    """

    __tablename__ = "duel_events"
    __table_args__ = (
        UniqueConstraint(
            "match_id",
            "user_id",
            "note_index",
            name="uq_duel_events_match_user_note",
        ),
        CheckConstraint("note_index >= 0", name="ck_duel_events_note_index"),
        CheckConstraint(
            "quality IN ('perfect', 'great', 'good', 'miss')",
            name="ck_duel_events_quality",
        ),
        Index("ix_duel_events_user_id", "user_id"),
    )

    id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True), primary_key=True, default=uuid.uuid4
    )
    match_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("duel_matches.id", ondelete="CASCADE"),
        nullable=False,
    )
    user_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("users.id", ondelete="CASCADE"),
        nullable=False,
    )
    note_index: Mapped[int] = mapped_column(Integer, nullable=False)
    quality: Mapped[str] = mapped_column(String(8), nullable=False)
    #: Client clock relative to the match start. Informational; the server
    #: validates against ``server_time_ms`` and never trusts this value alone.
    client_time_ms: Mapped[int | None] = mapped_column(BigInteger, nullable=True)
    #: Server clock relative to ``duel_matches.started_at`` at receipt time.
    server_time_ms: Mapped[int] = mapped_column(BigInteger, nullable=False)
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), nullable=False
    )
