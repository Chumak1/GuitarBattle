from datetime import datetime

from sqlalchemy import BigInteger, DateTime, Integer, String, func
from sqlalchemy.orm import Mapped, mapped_column

from app.database import Base


class AppAudioAsset(Base):
    """A global audio asset of the application, addressed by a unique role.

    ``background_music`` is the only role today, but the row is keyed by role so
    another non-gameplay track (e.g. a menu loop) can be added without a schema
    change. The file lives in ``settings.app_audio_dir`` under a generated name;
    ``object_key`` is never exposed and is resolved against that directory.

    Replacing an asset updates the same row, so the public URL served by
    ``/api/v1/app/background-music`` never changes and cannot become a dangling
    reference: the URL resolves the current row on every request.
    """

    __tablename__ = "app_audio_assets"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    role: Mapped[str] = mapped_column(String(32), unique=True, index=True, nullable=False)
    object_key: Mapped[str] = mapped_column(String(1024), nullable=False)
    #: Original client file name, kept for the admin panel only.
    original_name: Mapped[str | None] = mapped_column(String(255), nullable=True)
    content_type: Mapped[str] = mapped_column(String(120), nullable=False)
    size_bytes: Mapped[int] = mapped_column(BigInteger, nullable=False)
    sha256: Mapped[str] = mapped_column(String(64), nullable=False)
    uploaded_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), nullable=False, server_default=func.now()
    )
