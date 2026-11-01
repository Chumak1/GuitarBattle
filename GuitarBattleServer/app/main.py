import logging
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

# The landing page is rendered by app.services.landing_page from the template at
# app/static/landing/index.html: / is Russian, /en is English, both server-side.

# The support page and the admin console load no third-party scripts (the
# landing pages keep their analytics and therefore no strict policy). The
# sandboxed preview iframes are same-origin ``srcdoc`` documents.
CONTENT_SECURITY_POLICY = (
    "default-src 'self'; "
    "img-src 'self' data:; "
    "style-src 'self' 'unsafe-inline'; "
    "script-src 'self'; "
    "frame-src 'self'; "
    "object-src 'none'; "
    "base-uri 'none'"
)


@app.middleware("http")
async def csp_headers(request: Request, call_next):
    """Send a strict CSP to the support page and the admin console."""
    response = await call_next(request)
    path = request.url.path
    if (
        path == "/support"
        or path == "/donate"
        or path == "/admin"
        or path.startswith("/admin/")
    ):
        response.headers["Content-Security-Policy"] = CONTENT_SECURITY_POLICY
        response.headers["X-Content-Type-Options"] = "nosniff"
    return response


@app.middleware("http")
async def html_no_cache(request: Request, call_next):
    """Always revalidate the landing page and the web apps."""
    response = await call_next(request)
    path = request.url.path
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
