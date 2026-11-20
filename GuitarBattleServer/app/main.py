import logging
import mimetypes
import re
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI, Request, status
from fastapi.responses import (
    FileResponse,
    HTMLResponse,
    PlainTextResponse,
    RedirectResponse,
    Response,
)
from fastapi.staticfiles import StaticFiles

from app.api.health import router as health_router
from app.api.v1.router import router as v1_router
from app.config import settings
from app.database import SessionLocal
from app.services import landing_page
from app.services.email_service import log_email_configuration
from app.services.geo import country_for_ip, language_for_country

logging.basicConfig(level=getattr(logging, settings.log_level.upper(), logging.INFO))
log_email_configuration()
logger = logging.getLogger(__name__)


class _RedactAccessTokens(logging.Filter):
    """Keep JWTs out of uvicorn access logs.

    The WebSocket handshake carries the access token in ``?token=`` because a
    browser cannot set an ``Authorization`` header on a WebSocket. Uvicorn
    logs the full path with the query string, so the access logger gets a
    filter that masks the value. The token is never written by the application
    itself either.
    """

    _TOKEN_RE = re.compile(
        r"([?&](?:token|access_token)=)[^&\s\"']+", re.IGNORECASE
    )

    @classmethod
    def redact(cls, value: str) -> str:
        return cls._TOKEN_RE.sub(r"\1***", value)

    def filter(self, record: logging.LogRecord) -> bool:
        if isinstance(record.args, tuple):
            record.args = tuple(
                self.redact(arg) if isinstance(arg, str) else arg
                for arg in record.args
            )
        elif isinstance(record.args, dict):
            record.args = {
                key: self.redact(value) if isinstance(value, str) else value
                for key, value in record.args.items()
            }
        if isinstance(record.msg, str):
            record.msg = self.redact(record.msg)
        return True


for _logger_name in ("uvicorn.access", "uvicorn.error"):
    logging.getLogger(_logger_name).addFilter(_RedactAccessTokens())


def _sync_reference_data() -> None:
    """Keep the achievement catalog in sync with the code on every start.

    The catalog is reference data, so this is an idempotent upsert, never a
    destructive step. A missing database must not stop the API from booting;
    ``/health/ready`` reports that instead.
    """
    from app.services.achievement_catalog import sync_achievement_catalog

    db = SessionLocal()
    try:
        count = sync_achievement_catalog(db)
        logger.info("Achievement catalog synced: %s rows", count)
    except Exception:  # pragma: no cover - startup must survive a cold database
        db.rollback()
        logger.exception("Could not sync the achievement catalog")
    finally:
        db.close()


def _seed_support_page() -> None:
    """Create the default support page once; existing content is never touched."""
    from app.services.support_content import ensure_support_seed

    db = SessionLocal()
    try:
        ensure_support_seed(db)
        logger.info("Support page seed checked")
    except Exception:  # pragma: no cover - startup must survive a cold database
        db.rollback()
        logger.exception("Could not seed the support page")
    finally:
        # ``ensure_support_seed`` ends with a refresh, which opens a
        # transaction. Returning the connection to the pool explicitly keeps a
        # startup read from pinning a pooled connection until garbage collection
        # (that blocks later ``TRUNCATE`` statements, e.g. in the test suite).
        db.close()


@asynccontextmanager
async def lifespan(app: FastAPI):
    _sync_reference_data()
    _seed_support_page()
    yield


app = FastAPI(
    title=settings.app_name,
    version="0.1.0",
    description="GuitarBattle backend API",
    docs_url="/docs" if settings.app_env != "production" else None,
    redoc_url="/redoc" if settings.app_env != "production" else None,
    lifespan=lifespan,
)
app.include_router(health_router)
app.include_router(v1_router, prefix=settings.api_v1_prefix)

# The admin panel is a static single-page app served from the same origin as
# the API. Same-origin requests keep the JWT out of CORS and avoid a second
# deployment target for the small internal tool.
admin_static_dir = Path(__file__).resolve().parent / "static" / "admin"
app.mount("/admin", StaticFiles(directory=admin_static_dir, html=True), name="admin")

# Player-facing web pages: sign in, registration with email confirmation and the
# personal account page. They are plain static files that talk to the public
# API from the same origin, so no CORS or second deployment target is needed.
web_static_dir = Path(__file__).resolve().parent / "static" / "app"

# ``.webmanifest`` is not in the ``mimetypes`` table of every Python build;
# without the explicit type ``StaticFiles`` would send it as ``text/plain`` and
# browsers would ignore the manifest. The registration is idempotent.
mimetypes.add_type("application/manifest+json", ".webmanifest")

app.mount("/app", StaticFiles(directory=web_static_dir, html=True), name="web")

# Uploaded hero images/videos are served from the same origin. The directory is
# backed by a Docker volume; create it eagerly so StaticFiles can start.
media_static_dir = settings.landing_media_dir
try:
    media_static_dir.mkdir(parents=True, exist_ok=True)
except OSError:
    # A read-only or missing mount must not prevent the API from starting.
    pass
app.mount(
    "/media/landing",
    StaticFiles(directory=media_static_dir, check_dir=False),
    name="landing-media",
)

# Battle tower covers uploaded in the admin panel. The default directory is a
# subdirectory of the landing media root, so the same Docker volume backs it;
# CAMPAIGN_MEDIA_DIR can point elsewhere without touching the code. The cover
# URL returned by the campaign API is ``/media/campaigns/<name>``.
campaign_media_static_dir = settings.campaign_media_dir
try:
    campaign_media_static_dir.mkdir(parents=True, exist_ok=True)
except OSError:
    # A read-only or missing mount must not prevent the API from starting.
    pass
app.mount(
    "/media/campaigns",
    StaticFiles(directory=campaign_media_static_dir, check_dir=False),
    name="campaign-media",
)

# Administrator artwork slots (HOME hero backgrounds, default avatar) and
# player-uploaded avatars. Both directories live inside the landing media root
# by default, so the same Docker volume backs them.
for _media_name, _media_dir in (
    ("artwork", settings.artwork_media_dir),
    ("avatars", settings.avatar_media_dir),
):
    try:
        _media_dir.mkdir(parents=True, exist_ok=True)
    except OSError:
        # A read-only or missing mount must not prevent the API from starting.
        pass
    app.mount(
        f"/media/{_media_name}",
        StaticFiles(directory=_media_dir, check_dir=False),
        name=f"{_media_name}-media",
    )

# Gameplay visuals uploaded in the admin panel (UI-level character sheets and
# stage backgrounds). They live on the gameplay_media_data volume and are
# served from the same origin as the API; the catalogue returns these
# origin-relative URLs. The directories are created eagerly so StaticFiles can
# start even before the first upload.
for _media_name, _media_subdir in (
    ("characters", "characters"),
    ("stages", "stages"),
):
    _gameplay_dir = settings.gameplay_media_dir / _media_subdir
    try:
        _gameplay_dir.mkdir(parents=True, exist_ok=True)
    except OSError:
        # A read-only or missing mount must not prevent the API from starting.
        pass
    app.mount(
        f"/media/{_media_name}",
        StaticFiles(directory=_gameplay_dir, check_dir=False),
        name=f"{_media_name}-media",
    )

# Achievement icons are bundled in the image, not uploaded at runtime: they are
# part of the code catalog, so no volume is needed and every replica serves the
# same files. The API exposes them as ``/media/achievements/<icon>``.
achievement_icons_dir = (
    Path(__file__).resolve().parent / "static" / "media" / "achievements"
)
app.mount(
    "/media/achievements",
    StaticFiles(directory=achievement_icons_dir, check_dir=False),
    name="achievement-icons",
)

auth_page = web_static_dir / "auth.html"
account_page = web_static_dir / "account.html"
policy_page = web_static_dir / "policy.html"
support_page = web_static_dir / "support.html"

# Brand icons live in the ``/app`` static tree (generated by
# ``scripts/build_brand_icons.py``), so ``StaticFiles`` already serves them as
# ``/app/<name>``. Browsers and crawlers still ask for the conventional root
# URLs; the aliases below serve the same bytes with a pinned media type and
# cache policy instead of relying on ``mimetypes`` guesses.
favicon_file = web_static_dir / "favicon.ico"
apple_touch_icon_file = web_static_dir / "apple-touch-icon.png"

# Icons change with a release, not with a visitor: a week of caching is safe
# and stops tabs and crawlers from re-requesting them on every navigation.
BRAND_ICON_CACHE_CONTROL = "public, max-age=604800"
BRAND_ICON_PATHS = frozenset(
    {
        "/favicon.ico",
        "/apple-touch-icon.png",
        "/apple-touch-icon-precomposed.png",
        "/app/favicon.ico",
        "/app/apple-touch-icon.png",
        "/app/icon-48.png",
        "/app/icon-96.png",
        "/app/icon-192.png",
        "/app/icon-512.png",
    }
)

# The landing page is rendered by app.services.landing_page from the template at
# app/static/landing/index.html: / is Russian, /en is English, both server-side.

# The support page and the admin console load no third-party scripts (the
# landing pages keep their analytics and therefore no strict policy). The
# sandboxed preview iframes are same-origin ``srcdoc`` documents.
CONTENT_SECURITY_POLICY = (
    "default-src 'self'; "
    "img-src 'self' data:; "
    # The admin songs page plays an authorized preview from a blob: URL; the
    # fetch itself is same-origin, but <audio> needs the scheme explicitly.
    "media-src 'self' blob:; "
    "style-src 'self' 'unsafe-inline'; "
    "script-src 'self'; "
    "frame-src 'self'; "
    "object-src 'none'; "
    "base-uri 'none'"
)


# Baseline headers for every response. HSTS is deliberately short (one day) and
# carries neither includeSubDomains nor preload: the certificates cover only
# ``fretfight.online`` and ``www.fretfight.online``, and preload is irreversible.
# The application is the single source for these headers in production too: Caddy
# only adds them on the ``www`` redirect host, because setting them at the edge as
# well duplicated ``X-Content-Type-Options`` on proxied responses
# (``deploy/Caddyfile``, ``docs/HTTPS_TLS.md``).
SECURITY_HEADERS = {
    "X-Content-Type-Options": "nosniff",
    "Referrer-Policy": "strict-origin-when-cross-origin",
}
HSTS_HEADER_VALUE = "max-age=86400"


def _request_is_https(request: Request) -> bool:
    """True when the browser reached us over TLS.

    Behind Caddy/uvicorn ``--proxy-headers`` the scheme already comes from
    ``X-Forwarded-Proto``; the header is checked as well so a proxy that does not
    rewrite the scope cannot silently drop HSTS.
    """
    forwarded = request.headers.get("x-forwarded-proto", "")
    if forwarded:
        return forwarded.split(",", 1)[0].strip().lower() == "https"
    return request.url.scheme == "https"


@app.middleware("http")
async def security_headers(request: Request, call_next):
    """Baseline security headers, plus a strict CSP on the internal pages."""
    response = await call_next(request)
    for name, value in SECURITY_HEADERS.items():
        response.headers.setdefault(name, value)
    # Browsers must ignore HSTS delivered over plain HTTP, so it is only sent on
    # HTTPS responses; local development keeps working without TLS.
    if _request_is_https(request):
        response.headers.setdefault("Strict-Transport-Security", HSTS_HEADER_VALUE)
    path = request.url.path
    if (
        path == "/support"
        or path == "/donate"
        or path == "/admin"
        or path.startswith("/admin/")
    ):
        response.headers["Content-Security-Policy"] = CONTENT_SECURITY_POLICY
    return response


@app.middleware("http")
async def html_no_cache(request: Request, call_next):
    """Always revalidate the landing page and the web apps."""
    response = await call_next(request)
    path = request.url.path
    # Icons are the one exception under ``/app``: they are immutable per release
    # and are allowed to sit in the browser cache.
    if path in BRAND_ICON_PATHS:
        response.headers["Cache-Control"] = BRAND_ICON_CACHE_CONTROL
        return response
    if path in ("/", "/en", "/robots.txt", "/sitemap.xml") or path.startswith(
        ("/admin", "/app", "/login", "/register", "/account", "/policy", "/support")
    ):
        response.headers["Cache-Control"] = "no-cache, must-revalidate"
    return response


PUBLIC_HTML_PATHS = {"/", "/login", "/register", "/account", "/policy", "/support"}


@app.middleware("http")
async def language_cookie(request: Request, call_next):
    """Give first-time visitors a country-based language cookie.

    The pages also ask /api/v1/geo from JavaScript; this cookie makes the choice
    stick for later requests (and for clients without JavaScript). An explicit
    choice made through the language switcher is never overwritten.
    """
    response = await call_next(request)
    if request.url.path in PUBLIC_HTML_PATHS and "ff_lang" not in request.cookies:
        forwarded = request.headers.get("x-forwarded-for")
        if forwarded:
            ip = forwarded.split(",", 1)[0].strip()
        else:
            ip = request.client.host if request.client is not None else None
        language = language_for_country(country_for_ip(ip))
        if language:
            response.set_cookie(
                "ff_lang",
                language,
                max_age=365 * 24 * 60 * 60,
                path="/",
                samesite="lax",
            )
    return response


@app.get("/", include_in_schema=False)
def root() -> HTMLResponse:
    """Public landing page, Russian variant (canonical for x-default)."""
    return HTMLResponse(landing_page.render_landing("ru"))


@app.get("/en", include_in_schema=False)
def root_en() -> HTMLResponse:
    """Public landing page, English variant.

    A separate URL (not a client-side toggle) so crawlers get the English title,
    description and body copy without running JavaScript.
    """
    return HTMLResponse(landing_page.render_landing("en"))


@app.get("/robots.txt", include_in_schema=False)
def robots_txt() -> PlainTextResponse:
    return PlainTextResponse(landing_page.robots_txt())


@app.get("/sitemap.xml", include_in_schema=False)
def sitemap_xml() -> Response:
    return Response(content=landing_page.sitemap_xml(), media_type="application/xml")


# Root icon aliases. Browsers request ``/favicon.ico`` by convention even when
# the page links another icon, and Safari requests the two
# ``apple-touch-icon[-precomposed].png`` names when the page is added to the
# home screen. Serving them here (instead of only under ``/app``) keeps those
# probes from returning 404, which crawlers and tab bars treat as "no icon".
# ``HEAD`` is accepted explicitly: FastAPI's ``@app.get`` does not add it, and
# link checkers / crawlers often probe icons with HEAD.
@app.api_route("/favicon.ico", methods=["GET", "HEAD"], include_in_schema=False)
def favicon_ico() -> FileResponse:
    return FileResponse(
        favicon_file,
        media_type="image/x-icon",
        headers={"Cache-Control": BRAND_ICON_CACHE_CONTROL},
    )


@app.api_route(
    "/apple-touch-icon.png", methods=["GET", "HEAD"], include_in_schema=False
)
@app.api_route(
    "/apple-touch-icon-precomposed.png", methods=["GET", "HEAD"], include_in_schema=False
)
def apple_touch_icon() -> FileResponse:
    return FileResponse(
        apple_touch_icon_file,
        media_type="image/png",
        headers={"Cache-Control": BRAND_ICON_CACHE_CONTROL},
    )


# Yandex.Webmaster verifies the site by fetching this exact file from the root.
# It is served by a route (not a static mount) so the body stays byte-exact and
# cannot be affected by the static file handler. The same code is also present
# as a <meta name="yandex-verification"> tag in the landing template.
@app.get(landing_page.YANDEX_VERIFICATION_PATH, include_in_schema=False)
def yandex_verification_file() -> HTMLResponse:
    return HTMLResponse(
        landing_page.YANDEX_VERIFICATION_HTML,
        media_type="text/html; charset=UTF-8",
    )


# Google Search Console checks the same way: a meta tag in the landing template
# plus this root file, whose body is ``google-site-verification: <filename>``.
@app.get(landing_page.GOOGLE_VERIFICATION_PATH, include_in_schema=False)
def google_verification_file() -> HTMLResponse:
    return HTMLResponse(
        landing_page.GOOGLE_VERIFICATION_HTML,
        media_type="text/html; charset=UTF-8",
    )


@app.get("/login", include_in_schema=False)
def login_page() -> FileResponse:
    return FileResponse(auth_page, media_type="text/html")


@app.get("/register", include_in_schema=False)
def register_page() -> FileResponse:
    return FileResponse(auth_page, media_type="text/html")


@app.get("/policy", include_in_schema=False)
def policy_route() -> FileResponse:
    """Public privacy policy (ru/en are both in the page)."""
    return FileResponse(policy_page, media_type="text/html")


@app.get("/account", include_in_schema=False)
def account_page_route() -> FileResponse:
    return FileResponse(account_page, media_type="text/html")


@app.get("/support", include_in_schema=False)
def support_page_route() -> FileResponse:
    """Public support page. All of its content comes from the API."""
    return FileResponse(support_page, media_type="text/html")


@app.get("/donate", include_in_schema=False)
def donate_redirect() -> RedirectResponse:
    """Keep the old short link working without a second copy of the page."""
    return RedirectResponse(
        url="/support", status_code=status.HTTP_308_PERMANENT_REDIRECT
    )
