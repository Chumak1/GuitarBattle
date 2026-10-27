from datetime import datetime
from uuid import UUID

from pydantic import BaseModel, ConfigDict, EmailStr, Field, field_validator, model_validator


class RegisterRequest(BaseModel):
    username: str = Field(min_length=3, max_length=32, pattern=r"^[A-Za-z0-9_.-]+$")
    email: EmailStr
    password: str = Field(min_length=6, max_length=128)
    display_name: str | None = Field(default=None, max_length=64)
    # Mandatory consent to the privacy policy. The default keeps the field
    # additive in the schema; the endpoint rejects ``false`` with 422 so an
    # outdated client can never create an account without consent.
    privacy_consent: bool = Field(
        default=False,
        description="Must be true: the user accepts the privacy policy",
    )

    @field_validator("username")
    @classmethod
    def normalize_username(cls, value: str) -> str:
        return value.strip().lower()

    @field_validator("email")
    @classmethod
    def normalize_email(cls, value: str) -> str:
        return value.strip().lower()

    @field_validator("display_name")
    @classmethod
    def normalize_display_name(cls, value: str | None) -> str | None:
        if value is None:
            return None
        cleaned = value.strip()
        return cleaned or None


class LoginRequest(BaseModel):
    identifier: str = Field(min_length=3, max_length=320, description="Email or username")
    password: str = Field(min_length=1, max_length=128)

    @field_validator("identifier")
    @classmethod
    def normalize_identifier(cls, value: str) -> str:
        return value.strip().lower()


class EmailOnlyRequest(BaseModel):
    email: EmailStr

    @field_validator("email")
    @classmethod
    def normalize_email(cls, value: str) -> str:
        return value.strip().lower()


class VerifyEmailRequest(EmailOnlyRequest):
    code: str = Field(min_length=6, max_length=6, pattern=r"^\d{6}$")

    @field_validator("code", mode="before")
    @classmethod
    def normalize_code(cls, value: object) -> object:
        if isinstance(value, str):
            return value.strip()
        return value


class ResetPasswordRequest(VerifyEmailRequest):
    new_password: str = Field(min_length=6, max_length=128)


class MessageResponse(BaseModel):
    message: str


class TokenResponse(BaseModel):
    access_token: str
    token_type: str = "bearer"
    expires_in: int
    # Additive: newer clients keep a long-lived session with the rotating
    # refresh token. Older clients ignore both fields and keep using
    # ``access_token`` exactly as before.
    refresh_token: str | None = None
    refresh_expires_in: int | None = None


class RefreshRequest(BaseModel):
    """Optional body for /refresh and /logout.

    The web app relies on the HttpOnly ``ff_refresh`` cookie; the desktop client
    has no cookie jar and sends the token in the body instead.
    """

    refresh_token: str | None = Field(default=None, max_length=512)


class UserResponse(BaseModel):
    model_config = ConfigDict(from_attributes=True)

    id: UUID
    username: str
    email: EmailStr
    display_name: str
    avatar_key: str | None
    email_verified: bool
    created_at: datetime
    # Additive: NULL for accounts created before the consent requirement.
    privacy_consent_at: datetime | None = None


class ProfileUpdateRequest(BaseModel):
    """Partial profile update: send only the fields that change.

    The desktop client still sends just ``display_name``; ``username`` was added
    for the web account page and is normalised the same way as at registration.
    """

    display_name: str | None = Field(default=None, max_length=64)
    username: str | None = Field(
        default=None,
        min_length=3,
        max_length=32,
        pattern=r"^[A-Za-z0-9_.-]+$",
    )

    @field_validator("display_name")
    @classmethod
    def normalize_profile_name(cls, value: str | None) -> str | None:
        if value is None:
            return None
        cleaned = value.strip()
        if not cleaned:
            raise ValueError("Display name cannot be blank")
        return cleaned

    @field_validator("username")
    @classmethod
    def normalize_profile_username(cls, value: str | None) -> str | None:
        if value is None:
            return None
        return value.strip().lower()

    @model_validator(mode="after")
    def require_at_least_one_field(self) -> "ProfileUpdateRequest":
        if self.username is None and self.display_name is None:
            raise ValueError("Nothing to update")
        return self


class PasswordChangeRequest(BaseModel):
    current_password: str = Field(min_length=1, max_length=128)
    new_password: str = Field(min_length=6, max_length=128)
