"""The fixed registry of UI sound events.

This is the single source of the event keys, their human labels and their
categories. The Alembic migration seeds exactly these rows, the admin panel and
the public manifest read them back from ``ui_sound_events``, and the JUCE client
has the same list compiled in (``GuitarBattle/Source/Audio/UiSounds.cpp``).

Do not add keys at runtime: an event exists in the product or it does not. The
migration is the only place the catalog is written, so the database and the
client can never drift silently.

Keeping this module dependency-free is deliberate: the migration imports it,
and importing ``app.config`` there would make an offline migration depend on the
environment of the machine that runs Alembic.
"""
from __future__ import annotations

from dataclasses import dataclass

UI_SOUND_CATEGORIES: tuple[str, ...] = ("navigation", "action", "system")


@dataclass(frozen=True, slots=True)
class UiSoundDefinition:
    """One fixed registry entry."""

    key: str
    name: str
    category: str


UI_SOUND_REGISTRY: tuple[UiSoundDefinition, ...] = (
    # Navigation: moving between screens and opening menus.
    UiSoundDefinition("nav.menu", "Menu item", "navigation"),
    UiSoundDefinition("nav.section", "Section opened", "navigation"),
    UiSoundDefinition("nav.back", "Back", "navigation"),
    UiSoundDefinition("tab.switch", "Tab switched", "navigation"),
    UiSoundDefinition("menu.open", "Dropdown opened", "navigation"),
    UiSoundDefinition("menu.close", "Dropdown closed", "navigation"),
    # Actions: buttons, cards, toggles and list selections.
    UiSoundDefinition("button", "Button", "action"),
    UiSoundDefinition("button.primary", "Primary action", "action"),
    UiSoundDefinition("card.select", "Card selected", "action"),
    UiSoundDefinition("toggle", "Toggle", "action"),
    UiSoundDefinition("checkbox", "Checkbox", "action"),
    UiSoundDefinition("character.select", "Character selected", "action"),
    UiSoundDefinition("song.select", "Song selected", "action"),
    # System: outcomes and rewards.
    UiSoundDefinition("save.success", "Settings saved", "system"),
    UiSoundDefinition("action.success", "Action succeeded", "system"),
    UiSoundDefinition("error", "Error", "system"),
    UiSoundDefinition("warning", "Warning", "system"),
    UiSoundDefinition("notification", "Notification", "system"),
    UiSoundDefinition("achievement", "Achievement unlocked", "system"),
    UiSoundDefinition("level.up", "Level up", "system"),
    UiSoundDefinition("character.unlock", "Character unlocked", "system"),
    UiSoundDefinition("tower.complete", "Tower completed", "system"),
    UiSoundDefinition("duel.start", "Duel started", "system"),
    UiSoundDefinition("duel.finish", "Duel finished", "system"),
)

UI_SOUND_KEYS: frozenset[str] = frozenset(item.key for item in UI_SOUND_REGISTRY)


def definition_for(key: str) -> UiSoundDefinition | None:
    """Return the registry entry for ``key`` (``None`` for an unknown key)."""
    for item in UI_SOUND_REGISTRY:
        if item.key == key:
            return item
    return None
