import logging
from datetime import datetime, timedelta, timezone
from uuid import UUID

import jwt
from fastapi import APIRouter, Body, Depends, HTTPException, Request, Response, status
from fastapi.responses import JSONResponse
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
    RefreshRequest,
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
from app.services.refresh_tokens import (
    find_refresh_session,
    is_expired,
    issue_refresh_session,
    refresh_token_expires_in,
    revoke_chain,
    revoke_session,
    revoke_user_sessions,
    rotating_reuse_is_race,
)

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/auth", tags=["authentication"])
bearer_scheme = HTTPBearer(auto_error=False)
password_hash = PasswordHash.recommended()
JWT_ALGORITHM = "HS256"

# The refresh cookie is only sent to the auth endpoints that need it, so it does
# not travel with every API call.
REFRESH_COOKIE_NAME = "ff_refresh"
REFRESH_COOKIE_PATH = "/api/v1/auth"


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


def _cookie_secure(request: Request) -> bool:
    """Whether the refresh cookie must carry the ``Secure`` flag.

    Behind Caddy the request arrives over plain HTTP with ``X-Forwarded-Proto``;
    honouring it keeps the flag correct in production and off on localhost.
    """
    mode = settings.refresh_cookie_secure
    if mode == "always":
        return True
    if mode == "never":
        return False
    forwarded = request.headers.get("x-forwarded-proto")
    if forwarded:
        return forwarded.split(",", 1)[0].strip().lower() == "https"
    return request.url.scheme == "https"


def _set_refresh_cookie(response: Response, request: Request, raw_token: str) -> None:
    response.set_cookie(
        key=REFRESH_COOKIE_NAME,
        value=raw_token,
        max_age=refresh_token_expires_in(),
        path=REFRESH_COOKIE_PATH,
        httponly=True,
        secure=_cookie_secure(request),
        samesite="lax",
    )


def _clear_refresh_cookie(response: Response, request: Request) -> None:
    response.delete_cookie(
        key=REFRESH_COOKIE_NAME,
        path=REFRESH_COOKIE_PATH,
        httponly=True,
        secure=_cookie_secure(request),
        samesite="lax",
    )


def _user_agent(request: Request) -> str | None:
    return request.headers.get("user-agent")


def _issue_tokens(
    db: Session,
    user: User,
    request: Request,
    response: Response,
) -> TokenResponse:
    """Create an access token plus a rotating refresh session for ``user``."""
    access_token, expires_in = _create_access_token(user)
    raw_refresh, _session = issue_refresh_session(
        db, user, user_agent=_user_agent(request)
    )
    db.commit()
    _set_refresh_cookie(response, request, raw_refresh)
    return TokenResponse(
        access_token=access_token,
        expires_in=expires_in,
        refresh_token=raw_refresh,
        refresh_expires_in=refresh_token_expires_in(),
    )


def _refresh_failure(request: Request) -> JSONResponse:
    """A generic 401 that also removes the unusable cookie.

    A raised ``HTTPException`` would build a fresh response and drop the
    ``Set-Cookie`` header, so the failure is returned as a plain JSON response.
    """
    response = JSONResponse(
        {"detail": "Invalid or expired refresh token"},
        status_code=status.HTTP_401_UNAUTHORIZED,
        headers={"WWW-Authenticate": "Bearer"},
    )
    _clear_refresh_cookie(response, request)
    return response


def _submitted_refresh_token(request: Request, payload: RefreshRequest | None) -> str:
    """Cookie first (web), JSON body as the fallback (desktop client)."""
    raw = request.cookies.get(REFRESH_COOKIE_NAME)
    if not raw and payload is not None and payload.refresh_token:
        raw = payload.refresh_token
    return (raw or "").strip()


def _user_from_access_token(raw_token: str, db: Session) -> User | None:
    """Resolve an access token to an active user, or ``None``.

    Shared by :func:`get_current_user` (which turns ``None`` into 401) and
    :func:`get_optional_current_user` (which turns it into an anonymous
    request), so both paths validate the token exactly the same way.
    """
    try:
        payload = jwt.decode(
            raw_token,
            settings.secret_key,
            algorithms=[JWT_ALGORITHM],
            options={"require": ["sub", "exp", "ver"]},
        )
        if payload.get("type") != "access":
            return None
        user_id = UUID(payload["sub"])
        token_version = int(payload["ver"])
    except (jwt.InvalidTokenError, ValueError, KeyError, TypeError):
        return None

    user = db.scalar(select(User).where(User.id == user_id, User.is_active.is_(True)))
    if user is None or user.token_version != token_version:
        return None
    return user


def get_current_user(
    credentials: HTTPAuthorizationCredentials | None = Depends(bearer_scheme),
    db: Session = Depends(get_db),
) -> User:
    user = (
        _user_from_access_token(credentials.credentials, db)
        if credentials is not None
        else None
    )
    if user is None:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Invalid or missing access token",
            headers={"WWW-Authenticate": "Bearer"},
        )
    return user


def get_optional_current_user(
    credentials: HTTPAuthorizationCredentials | None = Depends(bearer_scheme),
    db: Session = Depends(get_db),
) -> User | None:
    """The signed-in user, or ``None`` for an anonymous request.

    Public endpoints use it for *additive* admin behaviour (drafts in the
    catalogue, preview sessions). A missing, expired or foreign token is
    treated as anonymous instead of 401: an older client that sends no token
    keeps the documented public behaviour, and a broken token must never turn
    a public read into an error.
    """
    if credentials is None:
        return None
    return _user_from_access_token(credentials.credentials, db)


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
    # Consent is checked before the duplicate lookup so a request without
    # consent never reveals whether an account already exists.
    if not payload.privacy_consent:
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail="Privacy policy consent is required",
        )

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
        privacy_consent_at=datetime.now(timezone.utc),
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
def login(
    payload: LoginRequest,
    request: Request,
    response: Response,
    db: Session = Depends(get_db),
) -> TokenResponse:
    # Sign-in never asks for (and never stores) privacy consent: the policy is
    # accepted once, at registration. An unexpected ``privacy_consent`` field in
    # the body is ignored by the schema.
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

    return _issue_tokens(db, user, request, response)


@router.post("/refresh", response_model=TokenResponse)
def refresh(
    request: Request,
    response: Response,
    payload: RefreshRequest | None = Body(default=None),
    db: Session = Depends(get_db),
) -> TokenResponse | JSONResponse:
    """Rotate the refresh session and issue a fresh access token.

    The token comes from the ``ff_refresh`` cookie (web) or from
    ``{"refresh_token": "..."}`` (desktop client). The presented session is
    revoked and replaced; the new raw token is returned and set as a cookie.
    """
    raw_token = _submitted_refresh_token(request, payload)
    if not raw_token:
        return _refresh_failure(request)

    now = datetime.now(timezone.utc)
    session = find_refresh_session(db, raw_token, for_update=True)
    if session is None:
        return _refresh_failure(request)

    if session.revoked_at is not None:
        _handle_reused_token(db, session, now)
        return _refresh_failure(request)

    if is_expired(session, now):
        revoke_session(session, now=now)
        db.commit()
        return _refresh_failure(request)

    user = db.get(User, session.user_id)
    if user is None or not user.is_active:
        revoke_session(session, now=now)
        db.commit()
        return _refresh_failure(request)

    raw_refresh, new_session = issue_refresh_session(
        db, user, user_agent=_user_agent(request), now=now
    )
    revoke_session(session, now=now, replaced_by=new_session)
    db.commit()

    access_token, expires_in = _create_access_token(user)
    _set_refresh_cookie(response, request, raw_refresh)
    return TokenResponse(
        access_token=access_token,
        expires_in=expires_in,
        refresh_token=raw_refresh,
        refresh_expires_in=refresh_token_expires_in(),
    )


def _handle_reused_token(db: Session, session, now: datetime) -> None:
    """Reusing a rotated token means theft; a very recent reuse is just a race."""
    if rotating_reuse_is_race(session, now):
        # Two tabs refreshed at the same moment: the first one already rotated.
        # Answer 401 but keep the replacement session alive.
        return
    revoke_chain(db, session, now=now)
    db.commit()


@router.post("/logout", status_code=status.HTTP_204_NO_CONTENT)
def logout(
    request: Request,
    payload: RefreshRequest | None = Body(default=None),
    db: Session = Depends(get_db),
) -> Response:
    """Revoke the presented refresh session and clear the cookie.

    Idempotent: a missing, unknown or already revoked token still returns 204
    and clears the cookie. The access token is short-lived and is not tracked
    server-side, so the client simply drops it.
    """
    raw_token = _submitted_refresh_token(request, payload)
    if raw_token:
        session = find_refresh_session(db, raw_token)
        if session is not None and session.revoked_at is None:
            revoke_session(session)
            db.commit()

    result = Response(status_code=status.HTTP_204_NO_CONTENT)
    _clear_refresh_cookie(result, request)
    return result


@router.post("/verify-email", response_model=TokenResponse)
def verify_email(
    payload: VerifyEmailRequest,
    request: Request,
    response: Response,
    db: Session = Depends(get_db),
) -> TokenResponse:
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

    return _issue_tokens(db, user, request, response)


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
    payload: ResetPasswordRequest,
    request: Request,
    response: Response,
    db: Session = Depends(get_db),
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
    # Every device must sign in again with the new password.
    revoke_user_sessions(db, user.id)
    db.commit()
    db.refresh(user)

    return _issue_tokens(db, user, request, response)


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
    # Persistent sessions are invalidated together with the access tokens.
    revoke_user_sessions(db, current_user.id)
    db.commit()
    return Response(status_code=status.HTTP_204_NO_CONTENT)
