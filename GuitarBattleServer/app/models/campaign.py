"""Campaigns ("Tower riff"): a named, ordered set of songs with a bot level.

A campaign is authored by an administrator: a display name, a *default* bot
difficulty for newly added stages and an ordered list of songs. Each stage may
override that default with its own difficulty (``campaign_songs.bot_level``,
``NULL`` = use the campaign default). The number of *stages* is the number of
songs in ``campaign_songs``; the order is stored explicitly in ``position``
(0-based) so the same song can appear in many campaigns, but only once per
campaign.

Deletion rules (mirrored by migration ``0026`` and ``0029``):

* ``campaign_songs.campaign_id`` -> ``CASCADE``: deleting a campaign removes its
  ordered list.
* ``campaign_songs.song_id`` -> ``CASCADE``: permanently deleting a song (only
  possible while no player result references it) removes its campaign entries
  instead of leaving a broken stage. Archiving a song keeps the entry; the
  public endpoint filters unpublished songs out.
* ``user_campaign_progress.campaign_id`` -> ``CASCADE``: progress disappears with
  its tower.
* ``campaigns.reward_character_id`` -> ``SET NULL``: deleting a character only
  stops the tower from rewarding it; already granted collections keep the row.
"""
import uuid
from datetime import datetime

from sqlalchemy import (
    Boolean,
    CheckConstraint,
    DateTime,
    Float,
    ForeignKey,
    Index,
    Integer,
    SmallInteger,
    String,
    UniqueConstraint,
    func,
)
from sqlalchemy.dialects.postgresql import UUID
from sqlalchemy.orm import Mapped, mapped_column, relationship

from app.database import Base


class Campaign(Base):
    """A named run of songs played against a bot of a configurable difficulty."""

    __tablename__ = "campaigns"
    __table_args__ = (
        # 0 = easy, 1 = normal, 2 = hard. The client maps the number to its own
        # bot profile; the admin panel shows EASY/NORMAL/HARD labels.
        CheckConstraint(
            "bot_level >= 0 AND bot_level <= 2",
            name="ck_campaigns_bot_level",
        ),
    )

    id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True), primary_key=True, default=uuid.uuid4
    )
    #: Administrator-facing name, e.g. ``Tower riff``. The API additionally
    #: rejects a name that differs from an existing one only by case.
    name: Mapped[str] = mapped_column(
        String(120), unique=True, index=True, nullable=False
    )
    #: Default bot difficulty for stages added without an explicit level:
    #: ``0`` easy, ``1`` normal, ``2`` hard. A stage overrides it with
    #: :attr:`CampaignSong.bot_level`.
    bot_level: Mapped[int] = mapped_column(SmallInteger, nullable=False)
    #: Character granted once the player clears every published stage. ``NULL``
    #: means the tower completes without a reward. The client never sends a
    #: character id: the server grants it from this column.
    reward_character_id: Mapped[uuid.UUID | None] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("ui_characters.id", ondelete="SET NULL"),
        nullable=True,
        index=True,
    )
    #: Optional administrator text of the unlock condition. Empty means the
    #: public API derives ``"Complete <campaign name>"``.
    reward_condition: Mapped[str | None] = mapped_column(
        String(200), nullable=True
    )
    #: Origin-relative URL of the admin-uploaded cover, e.g.
    #: ``/media/campaigns/cover-<uuid>.png``. ``NULL`` means "no cover": the
    #: client shows its own placeholder. Every upload stores a new uuid name, so
    #: a replaced cover gets a new URL and clients that cache by URL refresh it.
    cover_image_url: Mapped[str | None] = mapped_column(
        String(500), nullable=True
    )
    #: Free-form administrator description shown on the campaign (tower) card.
    description: Mapped[str | None] = mapped_column(String(400), nullable=True)
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), nullable=False
    )
    #: Bumped on metadata edits and whenever the ordered song list is replaced.
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        server_default=func.now(),
        onupdate=func.now(),
        nullable=False,
    )

    songs: Mapped[list["CampaignSong"]] = relationship(
        "CampaignSong",
        back_populates="campaign",
        cascade="all, delete-orphan",
        order_by="CampaignSong.position",
        passive_deletes=True,
    )
    #: Reward character, always eager-loaded with the campaign so the public and
    #: admin payloads can resolve its name without an extra query per row.
    reward_character: Mapped["UiCharacter | None"] = relationship(
        "UiCharacter", lazy="joined"
    )


class CampaignSong(Base):
    """One ordered stage of a campaign."""

    __tablename__ = "campaign_songs"
    __table_args__ = (
        UniqueConstraint(
            "campaign_id", "song_id", name="uq_campaign_songs_campaign_song"
        ),
        UniqueConstraint(
            "campaign_id", "position", name="uq_campaign_songs_campaign_position"
        ),
        CheckConstraint("position >= 0", name="ck_campaign_songs_position"),
        # Per-stage override of ``campaigns.bot_level``; NULL keeps the
        # campaign default (rows created before migration 0028 are backfilled).
        CheckConstraint(
            "bot_level >= 0 AND bot_level <= 2",
            name="ck_campaign_songs_bot_level",
        ),
        Index("ix_campaign_songs_song_id", "song_id"),
    )

    id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True), primary_key=True, default=uuid.uuid4
    )
    campaign_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("campaigns.id", ondelete="CASCADE"),
        nullable=False,
        index=True,
    )
    song_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("songs.id", ondelete="CASCADE"),
        nullable=False,
    )
    #: 0-based stage index. ``unique(campaign_id, position)`` keeps the order
    #: unambiguous even if two administrators save the same campaign at once.
    position: Mapped[int] = mapped_column(Integer, nullable=False)
    #: Bot difficulty of this stage: ``0`` easy, ``1`` normal, ``2`` hard.
    #: ``NULL`` means "use the campaign default" (:attr:`Campaign.bot_level`).
    bot_level: Mapped[int | None] = mapped_column(SmallInteger, nullable=True)

    campaign: Mapped[Campaign] = relationship("Campaign", back_populates="songs")
    song: Mapped["Song"] = relationship("Song", lazy="selectin")


class CampaignStageRun(Base):
    """One reported Battle run of one tower stage.

    ``run_id`` is generated by the client per attempt and unique per player, so
    a retried report can never be rewarded twice: the second request returns the
    stored row with ``already_processed=true``. The actual score and the
    normalized one are stored separately; XP is always recomputed server-side
    from the payload (a client cannot send "xp").
    """

    __tablename__ = "campaign_stage_runs"
    __table_args__ = (
        UniqueConstraint("user_id", "run_id", name="uq_campaign_stage_runs_user_run"),
        Index(
            "ix_campaign_stage_runs_user_campaign_position",
            "user_id",
            "campaign_id",
            "position",
        ),
        CheckConstraint("position >= 0", name="ck_campaign_stage_runs_position"),
        CheckConstraint(
            "score_points >= 0", name="ck_campaign_stage_runs_score"
        ),
    )

    id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True), primary_key=True, default=uuid.uuid4
    )
    user_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("users.id", ondelete="CASCADE"),
        nullable=False,
    )
    campaign_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("campaigns.id", ondelete="CASCADE"),
        nullable=False,
    )
    #: 0-based stage index of the run (the stage being played).
    position: Mapped[int] = mapped_column(Integer, nullable=False)
    run_id: Mapped[uuid.UUID] = mapped_column(UUID(as_uuid=True), nullable=False)
    #: Actual score reported by the client (never overwritten).
    score_points: Mapped[int] = mapped_column(Integer, nullable=False, default=0)
    #: ``score_points / completion_ratio``; ``NULL`` when too early to tell.
    normalized_score: Mapped[int | None] = mapped_column(Integer, nullable=True)
    accuracy_percent: Mapped[float] = mapped_column(
        Float, nullable=False, default=0.0
    )
    perfect_count: Mapped[int] = mapped_column(Integer, nullable=False, default=0)
    great_count: Mapped[int] = mapped_column(Integer, nullable=False, default=0)
    good_count: Mapped[int] = mapped_column(Integer, nullable=False, default=0)
    miss_count: Mapped[int] = mapped_column(Integer, nullable=False, default=0)
    completion_percent: Mapped[float] = mapped_column(
        Float, nullable=False, default=0.0
    )
    notes_total: Mapped[int | None] = mapped_column(Integer, nullable=True)
    notes_played: Mapped[int | None] = mapped_column(Integer, nullable=True)
    won: Mapped[bool] = mapped_column(
        Boolean, nullable=False, default=False, server_default="false"
    )
    xp_awarded: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0, server_default="0"
    )
    repeat_factor: Mapped[float] = mapped_column(
        Float, nullable=False, default=1.0, server_default="1"
    )
    pb_bonus: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0, server_default="0"
    )
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), nullable=False
    )
