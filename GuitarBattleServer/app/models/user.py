import uuid
from datetime import datetime
from sqlalchemy import BigInteger, Boolean, DateTime, Integer, String, func
from sqlalchemy.dialects.postgresql import INET, UUID
from sqlalchemy.orm import Mapped, mapped_column
from sqlalchemy.types import TypeDecorator
from app.database import Base


class IPAddressString(TypeDecorator):
    """``INET`` column exposed to Python as a plain string.

    psycopg returns ``ipaddress.IPv4Address``/``IPv6Address`` objects for an
    ``INET`` column. The API, the admin schemas and the tests all speak
    strings, so the conversion lives here once instead of at every call site.
    """

    impl = INET
    cache_ok = True

    def process_bind_param(self, value, dialect):
        if value is None:
            return None
        cleaned = str(value).strip()
        return cleaned or None

    def process_result_value(self, value, dialect):
        if value is None:
            return None
        return str(value)


class User(Base):
    __tablename__ = "users"
    id: Mapped[uuid.UUID] = mapped_column(UUID(as_uuid=True), primary_key=True, default=uuid.uuid4)
    username: Mapped[str] = mapped_column(String(32), unique=True, index=True, nullable=False)
    email: Mapped[str] = mapped_column(String(320), unique=True, index=True, nullable=False)
    password_hash: Mapped[str] = mapped_column(String(255), nullable=False)
    display_name: Mapped[str] = mapped_column(String(64), nullable=False, default="")
    avatar_key: Mapped[str | None] = mapped_column(String(512), nullable=True)
    email_verified: Mapped[bool] = mapped_column(Boolean, nullable=False, default=False)
    is_admin: Mapped[bool] = mapped_column(Boolean, nullable=False, default=False)
    is_active: Mapped[bool] = mapped_column(Boolean, nullable=False, default=True)
    # Set once, at registration, when the user accepts the privacy policy.
    # Accounts created before the consent requirement keep NULL: the existing
    # data is never touched.
    privacy_consent_at: Mapped[datetime | None] = mapped_column(
        DateTime(timezone=True), nullable=True
    )
    # Country of registration, resolved in the background from the IP the
    # account was created with. ``registration_ip``/``last_login_ip`` hold the
    # public addresses only (a private or loopback address is never stored);
    # both stay NULL for accounts created before migration 0019.
    registration_ip: Mapped[str | None] = mapped_column(IPAddressString, nullable=True)
    last_login_ip: Mapped[str | None] = mapped_column(IPAddressString, nullable=True)
    # ISO 3166-1 alpha-2 (upper case) plus the provider's English name. NULL
    # means "not resolved yet": a failed lookup leaves ``country_checked_at``
    # NULL too, so the next sign-in tries again instead of caching a failure
    # for good.
    country_code: Mapped[str | None] = mapped_column(
        String(2), nullable=True, index=True
    )
    country_name: Mapped[str | None] = mapped_column(String(64), nullable=True)
    country_checked_at: Mapped[datetime | None] = mapped_column(
        DateTime(timezone=True), nullable=True
    )
    # Incremented when the password changes so older JWTs stop working.
    token_version: Mapped[int] = mapped_column(
        Integer, nullable=False, default=0, server_default="0"
    )
    # Progression counters. ``level`` is derived from ``xp_total`` but stored so
    # leaderboards can sort players without recomputing the curve.
    xp_total: Mapped[int] = mapped_column(
        BigInteger, nullable=False, default=0, server_default="0"
    )
    level: Mapped[int] = mapped_column(
        Integer, nullable=False, default=1, server_default="1"
    )
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), nullable=False)
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), onupdate=func.now(), nullable=False)
