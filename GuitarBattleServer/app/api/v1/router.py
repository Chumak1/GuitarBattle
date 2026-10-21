from fastapi import APIRouter
from app.api.v1.auth import router as auth_router
from app.api.v1.songs import router as songs_router
from app.api.v1.admin_songs import router as admin_songs_router
from app.api.v1.games import router as games_router

router = APIRouter()
router.include_router(auth_router)
router.include_router(songs_router)
router.include_router(admin_songs_router)
router.include_router(games_router)
