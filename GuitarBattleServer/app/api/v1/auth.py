import logging
from datetime import datetime, timedelta, timezone
from uuid import UUID

import jwt
from fastapi import APIRouter, Depends, HTTPException, Response, status
from fastapi.security import HTTPAuthorizationCredentials, HTTPBearer
from pwdlib import PasswordHash
from sqlalchemy import func, or_, select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session

from app.config import settings
from app.database import get_db
from app.models.user import User
from app.schemas.auth import (
    EmailOnlyRequest,
    LoginRequest,
    MessageResponse,
    PasswordChangeRequest,
    ProfileUpdateRequest,
    RegisterRequest,
    ResetPasswordRequest,
    TokenResponse,
    UserResponse,
    VerifyEmailRequest,
)
from app.services.auth_codes import (
    PURPOSE_EMAIL_VERIFY,
    PURPOSE_PASSWORD_RESET,
    issue_code,
    purge_codes,
    verify_code,
)
from app.services.email_service import (
    EmailDeliveryError,
    send_password_reset_email,
    send_verification_email,
)

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/auth", tags=["authentication"])
bearer_scheme = HTTPBearer(auto_error=False)
password_hash = PasswordHash.recommended()
JWT_ALGORITHM = "HS256"


def _create_access_token(user: User) -> tuple[str, int]:
    expires_in = settings.access_token_expire_minutes * 60
    now = datetime.now(timezone.utc)
    payload = {
        "sub": str(user.id),
        "ver": user.token_version,
        "iat": now,
        "exp": now + timedelta(seconds=expires_in),
        "type": "access",
    }
    token = jwt.encode(payload, settings.secret_key, algorithm=JWT_ALGORITHM)
    return token, expires_in


def get_current_user(
    credentials: HTTPAuthorizationCredentials | None = Depends(bearer_scheme),
    db: Session = Depends(get_db),
) -> User:
    unauthorized = HTTPException(
        status_code=status.HTTP_401_UNAUTHORIZED,
        detail="Invalid or missing access token",
        headers={"WWW-Authenticate": "Bearer"},
    )
    if credentials is None:
        raise unauthorized
    try:
        payload = jwt.decode(
            credentials.credentials,
            settings.secret_key,
            algorithms=[JWT_ALGORITHM],
            options={"require": ["sub", "exp", "ver"]},
        )
        if payload.get("type") != "access":
            raise unauthorized
        user_id = UUID(payload["sub"])
        token_version = int(payload["ver"])
    except (jwt.InvalidTokenError, ValueError, KeyError, TypeError):
        raise unauthorized

    user = db.scalar(select(User).where(User.id == user_id, User.is_active.is_(True)))
    if user is None or user.token_version != token_version:
        raise unauthorized
    return user


def _find_user_by_email(db: Session, email: str) -> User | None:
    return db.scalar(
        select(User).where(func.lower(User.email) == email.lower()).limit(1)
    )


def _send_verification_code(db: Session, user: User, *, force: bool) -> bool:
    """Return False only when the resend cooldown is active."""
    code = issue_code(db, user, PURPOSE_EMAIL_VERIFY, force=force)
    if code is None:
        return False
    send_verification_email(
        to=user.email,
        username=user.display_name or user.username,
        code=code,
    )
    return True


def _send_password_reset_code(db: Session, user: User) -> bool:
    """Return False only when the resend cooldown is active."""
    code = issue_code(db, user, PURPOSE_PASSWORD_RESET)
    if code is None:
        return False
    send_password_reset_email(
        to=user.email,
        username=user.display_name or user.username,
        code=code,
    )
    return True


@router.post("/register", response_model=UserResponse, status_code=status.HTTP_201_CREATED)
def register(payload: RegisterRequest, db: Session = Depends(get_db)) -> User:
    existing = db.scalar(
        select(User.id).where(
            or_(
                func.lower(User.username) == payload.username,
                func.lower(User.email) == str(payload.email).lower(),
            )
        ).limit(1)
    )
    if existing is not None:
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail="Username or email is already registered",
        )

    user = User(
        username=payload.username,
        email=str(payload.email).lower(),
        password_hash=password_hash.hash(payload.password),
        display_name=payload.display_name or payload.username,
        email_verified=False,
        is_admin=False,
        is_active=True,
        token_version=0,
    )
    db.add(user)
    try:
        db.flush()
    except IntegrityError:
        db.rollback()
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail="Username or email is already registered",
        )

    try:
        _send_verification_code(db, user, force=True)
    except EmailDeliveryError:
        db.rollback()
        logger.exception("Could not send verification email to %s", user.email)
        raise HTTPException(
            status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
            detail="Could not send the verification email. Try again later.",
        )

    db.commit()
    db.refresh(user)
    return user


@router.post("/login", response_model=TokenResponse)
def login(payload: LoginRequest, db: Session = Depends(get_db)) -> TokenResponse:
    identifier = payload.identifier
    user = db.scalar(
        select(User).where(
            or_(
                func.lower(User.email) == identifier,
                func.lower(User.username) == identifier,
            )
        ).limit(1)
    )
    # Use the same public error for unknown user and wrong password.
    if user is None or not password_hash.verify(payload.password, user.password_hash) or not user.is_active:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Incorrect email/username or password",
            headers={"WWW-Authenticate": "Bearer"},
        )

    if not user.email_verified:
        raise HTTPException(
            status_code=status.HTTP_403_FORBIDDEN,
            detail="Email address is not verified",
        )

    token, expires_in = _create_access_token(user)
    return TokenResponse(access_token=token, expires_in=expires_in)


@router.post("/verify-email", response_model=TokenResponse)
def verify_email(payload: VerifyEmailRequest, db: Session = Depends(get_db)) -> TokenResponse:
    user = _find_user_by_email(db, str(payload.email))
    if user is None or not user.is_active:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="Invalid or expired code",
        )

    if user.email_verified:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="Email address is already verified",
        )

    if not verify_code(db, user, PURPOSE_EMAIL_VERIFY, payload.code):
        # Persist the attempt counter even though the request fails.
        db.commit()
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="Invalid or expired code",
        )

    user.email_verified = True
    db.commit()
    db.refresh(user)

    token, expires_in = _create_access_token(user)
    return TokenResponse(access_token=token, expires_in=expires_in)


@router.post(
    "/resend-verification",
    response_model=MessageResponse,
    status_code=status.HTTP_202_ACCEPTED,
)
def resend_verification(
    payload: EmailOnlyRequest, db: Session = Depends(get_db)
) -> MessageResponse:
    generic = MessageResponse(
        message="If the account needs verification, a new code has been sent."
    )
    user = _find_user_by_email(db, str(payload.email))
    if user is None or not user.is_active or user.email_verified:
        return generic

    try:
        sent = _send_verification_code(db, user, force=False)
        if sent:
            db.commit()
        else:
            db.rollback()
    except EmailDeliveryError:
        db.rollback()
        logger.exception("Could not resend verification email to %s", user.email)

    return generic


@router.post(
    "/forgot-password",
    response_model=MessageResponse,
    status_code=status.HTTP_202_ACCEPTED,
)
def forgot_password(
    payload: EmailOnlyRequest, db: Session = Depends(get_db)
) -> MessageResponse:
    generic = MessageResponse(
        message="If the account exists, a password reset code has been sent."
    )
    user = _find_user_by_email(db, str(payload.email))
    if user is None or not user.is_active:
        return generic

    try:
        sent = _send_password_reset_code(db, user)
        if sent:
            db.commit()
        else:
            db.rollback()
    except EmailDeliveryError:
        db.rollback()
        logger.exception("Could not send password reset email to %s", user.email)

    return generic


@router.post("/reset-password", response_model=TokenResponse)
def reset_password(
    payload: ResetPasswordRequest, db: Session = Depends(get_db)
) -> TokenResponse:
    user = _find_user_by_email(db, str(payload.email))
    if user is None or not user.is_active:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="Invalid or expired code",
        )

    if not verify_code(db, user, PURPOSE_PASSWORD_RESET, payload.code):
        db.commit()
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="Invalid or expired code",
        )

    user.password_hash = password_hash.hash(payload.new_password)
    user.email_verified = True
    user.token_version += 1
    purge_codes(db, user.id, PURPOSE_EMAIL_VERIFY)
    db.commit()
    db.refresh(user)

    token, expires_in = _create_access_token(user)
    return TokenResponse(access_token=token, expires_in=expires_in)


@router.get("/me", response_model=UserResponse)
def me(current_user: User = Depends(get_current_user)) -> User:
    return current_user


@router.patch("/me", response_model=UserResponse)
def update_me(payload: ProfileUpdateRequest, current_user: User = Depends(get_current_user),
              db: Session = Depends(get_db)) -> User:
    if payload.username is not None and payload.username != current_user.username:
        taken = db.scalar(
            select(User.id)
            .where(func.lower(User.username) == payload.username, User.id != current_user.id)
            .limit(1)
        )
        if taken is not None:
            raise HTTPException(
                status_code=status.HTTP_409_CONFLICT,
                detail="Username is already taken",
            )
        current_user.username = payload.username

    if payload.display_name is not None:
        current_user.display_name = payload.display_name

    try:
        db.commit()
    except IntegrityError:
        db.rollback()
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail="Username is already taken",
        )
    db.refresh(current_user)
    return current_user


@router.post("/change-password", status_code=status.HTTP_204_NO_CONTENT)
def change_password(
    payload: PasswordChangeRequest,
    current_user: User = Depends(get_current_user),
    db: Session = Depends(get_db),
) -> Response:
    if not password_hash.verify(payload.current_password, current_user.password_hash):
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="Current password is incorrect",
        )
    current_user.password_hash = password_hash.hash(payload.new_password)
    current_user.token_version += 1
    # A code requested before the password change must not remain usable.
    purge_codes(db, current_user.id, PURPOSE_PASSWORD_RESET)
    db.commit()
    return Response(status_code=status.HTTP_204_NO_CONTENT)
