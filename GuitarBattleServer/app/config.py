from urllib.parse import quote_plus
from pathlib import Path

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
