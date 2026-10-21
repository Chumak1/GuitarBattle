import logging
from fastapi import FastAPI
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

@app.get("/")
def root():
    return {"service": settings.app_name, "version": "0.1.0"}
