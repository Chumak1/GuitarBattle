from fastapi import APIRouter
from app.api.v1.auth import router as auth_router
from app.api.v1.songs import router as songs_router
from app.api.v1.admin_songs import router as admin_songs_router
from app.api.v1.games import router as games_router
from app.api.v1.client_assets import router as client_assets_router
from app.api.v1.site import router as site_router
from app.api.v1.admin_site import router as admin_site_router

router = APIRouter()
router.include_router(auth_router)
router.include_router(songs_router)
router.include_router(admin_songs_router)
router.include_router(games_router)
router.include_router(client_assets_router)
router.include_router(site_router)
router.include_router(admin_site_router)
