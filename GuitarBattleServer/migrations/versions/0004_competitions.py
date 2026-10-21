"""Add competition rooms and participant membership."""
from alembic import op
import sqlalchemy as sa
from sqlalchemy.dialects import postgresql

revision = "0004_competitions"
down_revision = "0003_song_midi_tempo_changes"
branch_labels = None
depends_on = None

def upgrade() -> None:
    op.create_table(
        "competitions",
        sa.Column("id", postgresql.UUID(as_uuid=True), primary_key=True),
        sa.Column("name", sa.String(120), nullable=False),
        sa.Column("description", sa.String(1000), nullable=True),
        sa.Column("song_id", postgresql.UUID(as_uuid=True),
                  sa.ForeignKey("songs.id", ondelete="RESTRICT"), nullable=False),
        sa.Column("created_by", postgresql.UUID(as_uuid=True),
                  sa.ForeignKey("users.id", ondelete="CASCADE"), nullable=False),
        sa.Column("status", sa.String(20), nullable=False, server_default="open"),
        sa.Column("max_players", sa.Integer(), nullable=False, server_default="8"),
        sa.Column("created_at", sa.DateTime(timezone=True), nullable=False, server_default=sa.func.now()),
    )
    op.create_index("ix_competitions_song_id", "competitions", ["song_id"])
    op.create_index("ix_competitions_created_by", "competitions", ["created_by"])
    op.create_table(
        "competition_participants",
        sa.Column("id", postgresql.UUID(as_uuid=True), primary_key=True),
        sa.Column("competition_id", postgresql.UUID(as_uuid=True),
                  sa.ForeignKey("competitions.id", ondelete="CASCADE"), nullable=False),
        sa.Column("user_id", postgresql.UUID(as_uuid=True),
                  sa.ForeignKey("users.id", ondelete="CASCADE"), nullable=False),
        sa.Column("joined_at", sa.DateTime(timezone=True), nullable=False, server_default=sa.func.now()),
        sa.UniqueConstraint("competition_id", "user_id", name="uq_competition_participant"),
    )
    op.create_index("ix_competition_participants_competition_id", "competition_participants", ["competition_id"])
    op.create_index("ix_competition_participants_user_id", "competition_participants", ["user_id"])

def downgrade() -> None:
    op.drop_index("ix_competition_participants_user_id", table_name="competition_participants")
    op.drop_index("ix_competition_participants_competition_id", table_name="competition_participants")
    op.drop_table("competition_participants")
    op.drop_index("ix_competitions_created_by", table_name="competitions")
    op.drop_index("ix_competitions_song_id", table_name="competitions")
    op.drop_table("competitions")
