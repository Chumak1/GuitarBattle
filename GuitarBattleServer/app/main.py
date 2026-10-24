import logging
from pathlib import Path

from fastapi import FastAPI, Request
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

from app.api.health import router as health_router
from app.api.v1.router import router as v1_router
from app.config import settings

logging.basicConfig(level=getattr(logging, settings.log_level.upper(), logging.INFO))
app = FastAPI(
    title=settings.app_name,
    version="0.1.0",
    description="GuitarBattle backend API",
    docs_url="/docs" if settings.app_env != "production" else None,
    redoc_url="/redoc" if settings.app_env != "production" else None,
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

auth_page = web_static_dir / "auth.html"
account_page = web_static_dir / "account.html"

landing_page = Path(__file__).resolve().parent / "static" / "landing" / "index.html"


@app.middleware("http")
async def html_no_cache(request: Request, call_next):
    """Always revalidate the landing page and the web apps."""
    response = await call_next(request)
    path = request.url.path
    if path == "/" or path.startswith(("/admin", "/app", "/login", "/register", "/account")):
        response.headers["Cache-Control"] = "no-cache, must-revalidate"
    return response


@app.get("/", include_in_schema=False)
def root() -> FileResponse:
    """Public landing page."""
    return FileResponse(landing_page, media_type="text/html")


@app.get("/login", include_in_schema=False)
def login_page() -> FileResponse:
    return FileResponse(auth_page, media_type="text/html")


@app.get("/register", include_in_schema=False)
def register_page() -> FileResponse:
    return FileResponse(auth_page, media_type="text/html")


@app.get("/account", include_in_schema=False)
def account_page_route() -> FileResponse:
    return FileResponse(account_page, media_type="text/html")
