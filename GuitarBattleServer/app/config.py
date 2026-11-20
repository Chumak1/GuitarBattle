from pathlib import Path
from typing import Literal
from urllib.parse import quote_plus

from pydantic import EmailStr, Field, field_validator
from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    app_name: str = "GuitarBattle API"
    app_env: str = "development"
    api_v1_prefix: str = "/api/v1"
    log_level: str = "INFO"
    postgres_db: str = "guitarbattle"
    postgres_user: str = "guitarbattle_app"
    postgres_password: str
    postgres_host: str = "db"
    postgres_port: int = 5432
    secret_key: str
    access_token_expire_minutes: int = 60 * 24
    # Persistent sessions. The web app keeps the raw refresh token in an
    # HttpOnly cookie; the desktop client receives it in the JSON response.
    # Only a sha256 hash of the token is stored in the database.
    refresh_token_expire_days: int = Field(default=30, ge=1, le=365)
    # A rotated token presented again within this window is treated as a race
    # between two tabs (401 without revoking the chain); later reuse is treated
    # as theft and revokes the whole rotation chain.
    refresh_reuse_grace_seconds: int = Field(default=15, ge=0, le=300)
    # "auto" sets Secure only for HTTPS requests (X-Forwarded-Proto is honoured
    # behind the Caddy reverse proxy). "always"/"never" pin the flag.
    refresh_cookie_secure: Literal["auto", "always", "never"] = "auto"
    song_files_dir: Path = Path("/data/song_files")
    # Generated manifest.json + bundle.zip served by /client-assets.
    client_assets_dir: Path = Path("/data/client_assets")
    max_midi_upload_bytes: int = 20 * 1024 * 1024
    max_audio_upload_bytes: int = 100 * 1024 * 1024
    # Landing page CMS: hero image/video uploads, served at /media/landing/<name>.
    landing_media_dir: Path = Path("/data/landing_media")
    # Battle tower covers uploaded in the admin panel, served at
    # /media/campaigns/<name>. The default lives inside the landing media root,
    # so the existing landing_media_data volume backs it without a new mount.
    campaign_media_dir: Path = Path("/data/landing_media/campaigns")
    max_campaign_cover_bytes: int = 2 * 1024 * 1024
    # Administrator artwork slots (HOME hero backgrounds and the default
    # avatar), served at /media/artwork/<name>. The default lives inside the
    # landing media root, so the existing volume backs it.
    artwork_media_dir: Path = Path("/data/landing_media/artwork")
    max_artwork_bytes: int = 8 * 1024 * 1024
    # Per-player avatars uploaded from the desktop client, served at
    # /media/avatars/<name>.
    avatar_media_dir: Path = Path("/data/landing_media/avatars")
    max_avatar_bytes: int = 2 * 1024 * 1024
    # Desktop game installers offered at /api/v1/downloads/game.
    game_builds_dir: Path = Path("/data/game_builds")
    # Global non-gameplay audio (background music), served at
    # /api/v1/app/background-music. Backed by the app_audio_data volume.
    app_audio_dir: Path = Path("/data/app_audio")
    # Gameplay visuals managed in the admin panel: UI-level character
    # spritesheets (``characters/``) and stage backgrounds (``stages/``).
    # Served at /media/characters/<file> and /media/stages/<file>.
    gameplay_media_dir: Path = Path("/data/gameplay_media")
    max_gameplay_media_bytes: int = 16 * 1024 * 1024
    # Admin-uploaded UI sounds (short MP3/WAV/OGG clips) stored under
    # ``gameplay_media_dir/ui_sounds`` and served by /api/v1/ui-sounds.
    max_ui_sound_bytes: int = 2 * 1024 * 1024
    # DB-IP Lite country database used for IP-based language detection.
    geoip_db_path: Path = Path("/data/geoip/dbip-country-lite.mmdb")
    # Country of registration/sign-in for the admin statistics. This is a
    # separate, external HTTPS service without an API key (ipwho.is); the
    # lookup runs in the background and never blocks a request. Disabling it
    # only skips the network call — the IP addresses are still stored.
    geoip_enabled: bool = True
    geoip_url: str = "https://ipwho.is"
    geoip_timeout_seconds: float = 2.0
    max_landing_media_bytes: int = 50 * 1024 * 1024
    max_game_build_bytes: int = 500 * 1024 * 1024

    # Transactional email. Empty values are treated as "not configured".
    # Yandex Cloud Postbox: SMTP_HOST=postbox.cloud.yandex.net, SMTP_PORT=587
    # with SMTP_SECURITY=starttls (or 465 with "ssl"). SMTP_USERNAME is the
    # API key ID and SMTP_PASSWORD is its secret part.
    smtp_host: str | None = None
    smtp_port: int = Field(default=587, ge=1, le=65535)
    smtp_username: str | None = None
    smtp_password: str | None = None
    smtp_security: Literal["starttls", "ssl", "none"] = "starttls"
    smtp_from_email: EmailStr = "noreply@fretfight.online"
    smtp_from_name: str = "FretFight"
    smtp_timeout_seconds: int = Field(default=10, ge=1)

    # Inbox for the public ``POST /feedback`` endpoint. Overridable so the
    # destination is not baked into the image.
    feedback_email_to: EmailStr = "chumakevgeny@gmail.com"

    # One-time codes for email verification and password recovery.
    email_code_expire_minutes: int = 15
    email_code_resend_seconds: int = 60
    email_code_max_attempts: int = 5

    @field_validator("smtp_host", "smtp_username", "smtp_password", mode="before")
    @classmethod
    def blank_smtp_values_are_none(cls, value: object) -> object:
        if isinstance(value, str):
            cleaned = value.strip()
            return cleaned or None
        return value

    model_config = SettingsConfigDict(
        env_file=".env",
        extra="ignore",
        case_sensitive=False,
    )

    @property
    def database_url(self) -> str:
        return (
            f"postgresql+psycopg://{quote_plus(self.postgres_user)}:"
            f"{quote_plus(self.postgres_password)}@{self.postgres_host}:"
            f"{self.postgres_port}/{quote_plus(self.postgres_db)}"
        )


settings = Settings()
