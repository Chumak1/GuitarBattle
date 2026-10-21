"""Persist MIDI tempo changes."""
from alembic import op
import sqlalchemy as sa
from sqlalchemy.dialects import postgresql

revision = "0003_song_midi_tempo_changes"
down_revision = "0002_song_midi_analysis"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.create_table(
        "song_midi_tempo_changes",
        sa.Column(
            "song_file_id",
            postgresql.UUID(as_uuid=True),
            sa.ForeignKey("song_files.id", ondelete="CASCADE"),
            primary_key=True,
        ),
        sa.Column("tick", sa.Integer(), primary_key=True),
        sa.Column("bpm", sa.Float(), nullable=False),
    )

def downgrade() -> None:
    op.drop_table("song_midi_tempo_changes")
