"""Add parsed MIDI analysis and note events."""
from alembic import op
import sqlalchemy as sa
from sqlalchemy.dialects import postgresql

revision = "0002_song_midi_analysis"
down_revision = "0001_initial_schema"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.create_table(
        "song_midi_analyses",
        sa.Column("id", postgresql.UUID(as_uuid=True), primary_key=True),
        sa.Column("song_file_id", postgresql.UUID(as_uuid=True),
                  sa.ForeignKey("song_files.id", ondelete="CASCADE"),
                  nullable=False, unique=True),
        sa.Column("midi_format", sa.Integer(), nullable=False),
        sa.Column("track_count", sa.Integer(), nullable=False),
        sa.Column("ticks_per_beat", sa.Integer(), nullable=False),
        sa.Column("bpm", sa.Float(), nullable=False),
        sa.Column("duration_seconds", sa.Float(), nullable=False),
        sa.Column("note_count", sa.Integer(), nullable=False),
        sa.Column("parsed_at", sa.DateTime(timezone=True), nullable=False, server_default=sa.func.now()),
    )
    op.create_table(
        "song_midi_notes",
        sa.Column("id", postgresql.UUID(as_uuid=True), primary_key=True),
        sa.Column("song_file_id", postgresql.UUID(as_uuid=True),
                  sa.ForeignKey("song_files.id", ondelete="CASCADE"), nullable=False),
        sa.Column("track_index", sa.Integer(), nullable=False),
        sa.Column("channel", sa.Integer(), nullable=False),
        sa.Column("midi_note", sa.Integer(), nullable=False),
        sa.Column("velocity", sa.Integer(), nullable=False),
        sa.Column("start_tick", sa.Integer(), nullable=False),
        sa.Column("end_tick", sa.Integer(), nullable=False),
        sa.Column("start_seconds", sa.Float(), nullable=False),
        sa.Column("duration_seconds", sa.Float(), nullable=False),
    )
    op.create_index("ix_song_midi_notes_song_file_id", "song_midi_notes", ["song_file_id"])


def downgrade() -> None:
    op.drop_index("ix_song_midi_notes_song_file_id", table_name="song_midi_notes")
    op.drop_table("song_midi_notes")
    op.drop_table("song_midi_analyses")
