from fastapi import APIRouter
from app.api.v1.auth import router as auth_router
from app.api.v1.songs import router as songs_router
from app.api.v1.admin_songs import router as admin_songs_router
from app.api.v1.admin_gameplay import router as admin_gameplay_router
from app.api.v1.app_audio import admin_router as admin_app_audio_router, public_router as app_audio_router
from app.api.v1.games import router as games_router
from app.api.v1.client_assets import router as client_assets_router
from app.api.v1.site import router as site_router
from app.api.v1.admin_site import router as admin_site_router
from app.api.v1.support import router as support_router
from app.api.v1.admin_support import router as admin_support_router
from app.api.v1.activity import router as activity_router
from app.api.v1.admin_statistics import router as admin_statistics_router
from app.api.v1.admin_users import router as admin_users_router
from app.api.v1.admin_daily import router as admin_daily_router
from app.api.v1.admin_guitar_skills import router as admin_guitar_skills_router
from app.api.v1.admin_artwork import router as admin_artwork_router
from app.api.v1.profile_avatar import router as profile_avatar_router
from app.api.v1.progression import router as progression_router
from app.api.v1.duels import router as duels_router
from app.api.v1.duel_lobbies import router as duel_lobbies_router
from app.api.v1.voice import router as voice_router
from app.api.v1.feedback import router as feedback_router
from app.api.v1.characters import router as characters_router
from app.api.v1.campaigns import admin_router as admin_campaigns_router, router as campaigns_router
from app.api.v1.social import router as social_router
from app.api.v1.messages import router as messages_router
from app.api.v1.ui_sounds import (
    admin_router as admin_ui_sounds_router,
    profile_router as profile_ui_sounds_router,
    public_router as ui_sounds_router,
)

router = APIRouter()
router.include_router(auth_router)
router.include_router(songs_router)
router.include_router(characters_router)
router.include_router(campaigns_router)
router.include_router(admin_songs_router)
router.include_router(admin_campaigns_router)
router.include_router(admin_gameplay_router)
router.include_router(app_audio_router)
router.include_router(admin_app_audio_router)
router.include_router(games_router)
router.include_router(progression_router)
router.include_router(social_router)
router.include_router(messages_router)
router.include_router(ui_sounds_router)
router.include_router(profile_ui_sounds_router)
router.include_router(admin_ui_sounds_router)
# Literal /duels/lobbies routes must be matched before /duels/{match_id}.
router.include_router(duel_lobbies_router)
router.include_router(voice_router)
router.include_router(duels_router)
router.include_router(feedback_router)
router.include_router(client_assets_router)
router.include_router(site_router)
router.include_router(admin_site_router)
router.include_router(support_router)
router.include_router(admin_support_router)
router.include_router(activity_router)
router.include_router(admin_statistics_router)
router.include_router(admin_users_router)
router.include_router(admin_daily_router)
router.include_router(admin_guitar_skills_router)
router.include_router(admin_artwork_router)
router.include_router(profile_avatar_router)
