from typing import Literal
from urllib.parse import quote_plus
from pathlib import Path

from pydantic import field_validator
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
    song_files_dir: Path = Path("/data/song_files")
    max_midi_upload_bytes: int = 20 * 1024 * 1024
    max_audio_upload_bytes: int = 100 * 1024 * 1024

    # Transactional email. Empty values are treated as "not configured".
    smtp_host: str | None = None
    smtp_port: int = 587
    smtp_username: str | None = None
    smtp_password: str | None = None
    smtp_security: Literal["starttls", "ssl", "none"] = "starttls"
    smtp_from_email: str = "noreply@fretfight.online"
    smtp_from_name: str = "GuitarBattle"
    smtp_timeout_seconds: int = 10

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
