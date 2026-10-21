#pragma once


#include <JuceHeader.h>
#include <atomic>
#include <vector>

#include "../Midi/MidiParser.h"
#include "../Gameplay/ScoringSystem.h"

class GameSession
{
public:
    enum class State
    {
        SongSelect,
        Tuner,
        Playing,
        Paused,
        Finished
    };

    GameSession();

    void reset();
    void resetGameplay();

    State getState() const;
    void setState(State newState);

    State getStateBeforePause() const;
    void setStateBeforePause(State newState);

    int getScore() const;
    void resetScore();
    int addScore(int points);

    int getTargetNoteIndex() const;
    void setTargetNoteIndex(int index);

    // Advances to the next target and clears per-target state.
    int advanceTargetIndex();

    bool isTargetHit() const;
    void setTargetHit(bool hit);

    int getResolvedTargetIndex() const;
    void resetResolvedTarget();
    bool resolveTarget(int index);

    uint64_t getGameplayStartSample() const;
    void setGameplayStartSample(uint64_t sample);

    uint64_t getPausedSamples() const;
    void setPausedSamples(uint64_t samples);

    uint64_t getPauseStartSample() const;
    void setPauseStartSample(uint64_t sample);

    uint64_t getLastHitSample() const;
    void setLastHitSample(uint64_t sample);

    Difficulty getDifficulty() const;
    void setDifficulty(Difficulty newDifficulty);

    double getSpeedPercent() const;
    void setSpeedPercent(double percent);

    double getBaseBpm() const;
    void setBaseBpm(double bpm);

    double getBpm() const;
    void setBpm(double newBpm);

    std::vector<TabNote>& getMidiNotes();
    const std::vector<TabNote>& getMidiNotes() const;

    void setMidiNotes(const std::vector<TabNote>& notes);

    const juce::String& getTargetNote() const;
    void setTargetNote(const juce::String& note);

private:
    State state = State::SongSelect;
    State stateBeforePause = State::Playing;

    std::atomic<uint64_t> gameplayStartSample{ 0 };
    std::atomic<uint64_t> pausedSamples{ 0 };
    std::atomic<uint64_t> pauseStartSample{ 0 };

    std::atomic<int> score{ 0 };
    std::atomic<bool> targetHit{ false };
    std::atomic<uint64_t> lastHitSample{ 0 };
    std::atomic<int> resolvedTargetIndex{ -1 };
    std::atomic<int> targetNoteIndex{ 0 };

    Difficulty difficulty = Difficulty::Normal;

    double speedPercent = 100.0;
    double baseBpm = 120.0;
    std::atomic<double> bpm{ 120.0 };

    std::vector<TabNote> midiNotes;
    juce::String targetNote{ "---" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GameSession)
};