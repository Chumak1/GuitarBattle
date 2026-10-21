#include "GameSession.h"

#include <algorithm>

GameSession::GameSession()
{
    reset();
}

void GameSession::reset()
{
    state = State::SongSelect;
    stateBeforePause = State::Playing;

    gameplayStartSample.store(
        0,
        std::memory_order_relaxed);

    pausedSamples.store(
        0,
        std::memory_order_relaxed);

    pauseStartSample.store(
        0,
        std::memory_order_relaxed);

    score.store(
        0,
        std::memory_order_relaxed);

    targetHit.store(
        false,
        std::memory_order_relaxed);

    lastHitSample.store(
        0,
        std::memory_order_relaxed);

    resolvedTargetIndex.store(
        -1,
        std::memory_order_relaxed);

    targetNoteIndex.store(
        0,
        std::memory_order_relaxed);

    difficulty = Difficulty::Normal;

    speedPercent = 100.0;
    baseBpm = 120.0;

    bpm.store(
        120.0,
        std::memory_order_relaxed);

    midiNotes.clear();
    targetNote = "---";
}

void GameSession::resetGameplay()
{
    gameplayStartSample.store(
        0,
        std::memory_order_relaxed);

    pausedSamples.store(
        0,
        std::memory_order_relaxed);

    pauseStartSample.store(
        0,
        std::memory_order_relaxed);

    score.store(
        0,
        std::memory_order_relaxed);

    targetHit.store(
        false,
        std::memory_order_relaxed);

    lastHitSample.store(
        0,
        std::memory_order_relaxed);

    resolvedTargetIndex.store(
        -1,
        std::memory_order_relaxed);

    targetNoteIndex.store(
        0,
        std::memory_order_relaxed);

    targetNote = "---";
}

GameSession::State GameSession::getState() const
{
    return state;
}

void GameSession::setState(State newState)
{
    state = newState;
}

GameSession::State GameSession::getStateBeforePause() const
{
    return stateBeforePause;
}

void GameSession::setStateBeforePause(State newState)
{
    stateBeforePause = newState;
}

int GameSession::getScore() const
{
    return score.load(
        std::memory_order_relaxed);
}

void GameSession::resetScore()
{
    score.store(
        0,
        std::memory_order_relaxed);
}

int GameSession::addScore(int points)
{
    return score.fetch_add(
        points,
        std::memory_order_relaxed)
        + points;
}

int GameSession::getTargetNoteIndex() const
{
    return targetNoteIndex.load(
        std::memory_order_relaxed);
}

void GameSession::setTargetNoteIndex(int index)
{
    targetNoteIndex.store(
        index,
        std::memory_order_relaxed);
}

int GameSession::advanceTargetIndex()
{
    const int nextIndex = getTargetNoteIndex() + 1;

    targetNoteIndex.store(nextIndex, std::memory_order_relaxed);
    resolvedTargetIndex.store(-1, std::memory_order_relaxed);
    targetHit.store(false, std::memory_order_relaxed);

    return nextIndex;
}

bool GameSession::isTargetHit() const
{
    return targetHit.load(
        std::memory_order_relaxed);
}

void GameSession::setTargetHit(bool hit)
{
    targetHit.store(
        hit,
        std::memory_order_relaxed);
}

int GameSession::getResolvedTargetIndex() const
{
    return resolvedTargetIndex.load(
        std::memory_order_relaxed);
}

void GameSession::resetResolvedTarget()
{
    resolvedTargetIndex.store(
        -1,
        std::memory_order_relaxed);
}

bool GameSession::resolveTarget(int index)
{
    int expected = -1;

    return resolvedTargetIndex.compare_exchange_strong(
        expected,
        index,
        std::memory_order_acq_rel);
}

uint64_t GameSession::getGameplayStartSample() const
{
    return gameplayStartSample.load(
        std::memory_order_relaxed);
}

void GameSession::setGameplayStartSample(
    uint64_t sample)
{
    gameplayStartSample.store(
        sample,
        std::memory_order_relaxed);
}

uint64_t GameSession::getPausedSamples() const
{
    return pausedSamples.load(
        std::memory_order_relaxed);
}

void GameSession::setPausedSamples(
    uint64_t samples)
{
    pausedSamples.store(
        samples,
        std::memory_order_relaxed);
}

uint64_t GameSession::getPauseStartSample() const
{
    return pauseStartSample.load(
        std::memory_order_relaxed);
}

void GameSession::setPauseStartSample(
    uint64_t sample)
{
    pauseStartSample.store(
        sample,
        std::memory_order_relaxed);
}

uint64_t GameSession::getLastHitSample() const
{
    return lastHitSample.load(
        std::memory_order_relaxed);
}

void GameSession::setLastHitSample(
    uint64_t sample)
{
    lastHitSample.store(
        sample,
        std::memory_order_relaxed);
}

Difficulty GameSession::getDifficulty() const
{
    return difficulty;
}

void GameSession::setDifficulty(
    Difficulty newDifficulty)
{
    difficulty = newDifficulty;
}

double GameSession::getSpeedPercent() const
{
    return speedPercent;
}

void GameSession::setSpeedPercent(
    double percent)
{
    speedPercent = percent;
}

double GameSession::getBaseBpm() const
{
    return baseBpm;
}

void GameSession::setBaseBpm(
    double newBpm)
{
    baseBpm = newBpm;
}

double GameSession::getBpm() const
{
    return bpm.load(
        std::memory_order_relaxed);
}

void GameSession::setBpm(
    double newBpm)
{
    bpm.store(
        newBpm,
        std::memory_order_relaxed);
}

std::vector<TabNote>& GameSession::getMidiNotes()
{
    return midiNotes;
}

const std::vector<TabNote>&
GameSession::getMidiNotes() const
{
    return midiNotes;
}

void GameSession::setMidiNotes(
    const std::vector<TabNote>& notes)
{
    midiNotes = notes;
}

const juce::String& GameSession::getTargetNote() const
{
    return targetNote;
}

void GameSession::setTargetNote(
    const juce::String& note)
{
    targetNote = note;
}